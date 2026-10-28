/**
 * CODA Frame Builder - receives UDP packets from an EJFAT load balancer,
 * reassembles them into frames and either persists the raw frames to a file
 * (reassembly-only mode) or aggregates them into EVIO-6 time frames.
 *
 * Pipeline with frame building and forwarding enabled:
 *
 *   EJFAT LB #1 --UDP--> E2SAR Reassembler --> reception loop --> FrameBuilder
 *     (--recv-ejfat-uri,   (worker on LB #1)                       |  ET / files
 *      worker)                                                     v
 *                        EJFAT LB #2 <--UDP-- E2SAR Segmenter <-- FrameSender
 *                        (--send-ejfat-uri,     (sender on LB #2)
 *                         sender)
 *
 * The program registers with the EJFAT control plane(s), accepts segmented
 * UDP packets, reassembles them, builds frames and forwards each built frame
 * as one E2SAR event. Ctrl+C (SIGINT) shuts the pipeline down in data-flow
 * order and drains forwarded frames, see shutdownPipeline().
 */

#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <cstring>
#include <mutex>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <locale>

#include <algorithm>
#include <condition_variable>
#include <optional>
#include <thread>

#include <e2sar.hpp>
#include "ejfat_uri_config.hpp"
#include "metrics_endpoint.hpp"
#include "metrics_render.hpp"
#include "pipeline_stats.hpp"
#ifdef ENABLE_FRAME_BUILDER
#include "e2sar_reassembler_framebuilder.hpp"
#include "e2sar_frame_transport.hpp"
#include "frame_sender.hpp"
#include <et.h>
#else
// Forward declaration when frame builder is not available
namespace e2sar { class FrameBuilder; }
#endif

#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace po = boost::program_options;
namespace bfs = boost::filesystem;
using namespace e2sar;

// Version information
#ifndef CODA_FB_VERSION
#define CODA_FB_VERSION "1.0.0"
#endif

// Global state for signal handling
std::atomic<bool> threadsRunning{true};  // cleared by the SIGINT handler (lock-free, so signal-safe)
Reassembler *reasPtr{nullptr};
bool workerRegistered{false};            // registerWorker() succeeded on LB #1
e2sar::FrameBuilder *frameBuilderPtr{nullptr};  // Global frame builder pointer (nullptr if not built with ET)
#ifdef ENABLE_FRAME_BUILDER
codafb::E2SARFrameTransport *sendTransportPtr{nullptr};  // forwarding to LB #2 (nullptr if disabled)
codafb::FrameSender *frameSenderPtr{nullptr};
bool senderSafeToDelete{true};           // false if frames or builder threads outlived shutdown
#endif
std::atomic<bool> handlerTriggered{false};
int globalOutputFd{-1};  // Global file descriptor for single output file
std::mutex fileMutex;    // Mutex to protect file writes
bool verboseFrameInfo = false;  // Verbose frame logging: print all frames and builder messages
bool verboseReassemble = false;  // Print event numbers for all streams (reassembly-only mode)

// Note: Frame builder is always used when ENABLE_FRAME_BUILDER is defined

// Statistics: definitions and measurement boundaries are in pipeline_stats.hpp.
// E2SAR's own receive/send statistics are polled at report time.
codafb::PipelineStats pipelineStats;

// Prometheus endpoint (--metrics-port). It reads the E2SAR objects only once
// startup has published them (e2sarStatsReady), and statsSourceMutex keeps a
// scrape from reading the segmenter while shutdown tears it down.
codafb::MetricsEndpoint *metricsEndpointPtr{nullptr};
std::mutex statsSourceMutex;
std::atomic<bool> e2sarStatsReady{false};

// Reassembly-only mode output (raw frames written to one file)
std::atomic<u_int64_t> buildEventsWritten{0};      // Frames written to the output file
std::atomic<u_int64_t> buildEventsBytesTotal{0};   // Bytes of those frames
std::atomic<u_int64_t> writeErrors{0};

u_int16_t reportThreadSleepMs{1000};

// Forward declarations (none currently needed)

/**
 * Get the local host IP address (first non-loopback IPv4 address)
 */
std::string getLocalHostIP(bool preferV6 = false) {
    struct ifaddrs *ifaddr, *ifa;
    char host[NI_MAXHOST];

    if (getifaddrs(&ifaddr) == -1) {
        std::cerr << "Failed to get network interfaces" << std::endl;
        return "";
    }

    std::string result;

    // Iterate through all network interfaces
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr)
            continue;

        int family = ifa->ifa_addr->sa_family;

        // Check for IPv4 or IPv6
        if ((family == AF_INET && !preferV6) || (family == AF_INET6 && preferV6)) {
            int s = getnameinfo(ifa->ifa_addr,
                              (family == AF_INET) ? sizeof(struct sockaddr_in) :
                                                   sizeof(struct sockaddr_in6),
                              host, NI_MAXHOST,
                              nullptr, 0, NI_NUMERICHOST);
            if (s != 0) {
                continue;
            }

            // Skip loopback addresses
            std::string addr(host);
            if (addr != "127.0.0.1" && addr != "::1" && addr.find("127.") != 0) {
                result = addr;
                break;  // Use first non-loopback address found
            }
        }
    }

    freeifaddrs(ifaddr);
    return result;
}

void ctrlCHandler(int sig)
{
    if (handlerTriggered.exchange(true))
        return;

    // write(2) rather than iostreams: only async-signal-safe calls in a handler
    static const char msg[] = "\nCTRL-C received, initiating shutdown...\n";
    ssize_t ignored = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    (void)ignored;
    threadsRunning = false;

    // NOTE: We do NOT call exit() or delete objects here!
    // The signal handler just sets the flag to stop threads.
    // The main thread will detect this and perform proper cleanup.
}

// ============================================================================
// Statistics reporting
// ============================================================================

namespace {

std::string fmtBytes(double b)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(2);
    if (b >= 1024.0 * 1024 * 1024) os << b / (1024.0 * 1024 * 1024) << " GiB";
    else if (b >= 1024.0 * 1024)   os << b / (1024.0 * 1024) << " MiB";
    else if (b >= 1024.0)          os << b / 1024.0 << " KiB";
    else                           os << std::setprecision(0) << b << " B";
    return os.str();
}

std::string fmtNs(double ns)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(1);
    if (ns < 0)             os << "n/a";
    else if (ns < 1e3)      os << ns << " ns";
    else if (ns < 1e6)      os << ns / 1e3 << " us";
    else if (ns < 1e9)      os << ns / 1e6 << " ms";
    else                    os << ns / 1e9 << " s";
    return os.str();
}

std::string fmtAge(const codafb::EventTime &t, int64_t nowNs)
{
    double age = t.ageSeconds(nowNs);
    if (age < 0) return "never";
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << age << " s ago";
    return os.str();
}

std::string fmtRate(uint64_t delta, double sec, const char *unit)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << (sec > 0 ? delta / sec : 0.0) << " " << unit << "/s";
    return os.str();
}

std::string fmtByteRate(uint64_t delta, double sec)
{
    return fmtBytes(sec > 0 ? delta / sec : 0.0) + "/s";
}

// "~" marks values estimated from histogram buckets
std::string fmtQuantiles(const codafb::Histogram &h, bool bytes)
{
    auto snap = h.snapshot();
    if (snap.count == 0) return "no data";
    auto f = [bytes](double v) { return bytes ? fmtBytes(v) : fmtNs(v); };
    return "~p50 " + f(snap.quantile(0.50)) + ", ~p99 " + f(snap.quantile(0.99));
}

const char *readiness(bool enabled, const std::atomic<bool> &ready)
{
    if (!enabled) return "disabled";
    return ready.load() ? "READY" : "NOT READY";
}

} // namespace

/**
 * Values the report shows as rates. Rates are deltas between consecutive
 * reports divided by the measured time between them (not since start).
 */
struct ReportSample {
    int64_t ns{0};
    uint64_t udpPackets{0}, udpBytes{0};
    uint64_t events{0}, eventBytes{0};
    uint64_t frames{0}, frameBytes{0};
    uint64_t submitted{0}, submittedBytes{0};
    uint64_t sentPackets{0};
    uint64_t written{0}, writtenBytes{0};
};

/**
 * Print one statistics report and advance 'prev'. With final=true the
 * interval is the whole run, so rates are run averages.
 *
 * Only the reporting thread calls this while the pipeline runs; the final
 * report is printed by the main thread after that thread was joined.
 */
void printStatisticsReport(ReportSample &prev, bool final)
{
    const int64_t now = codafb::monotonicNowNs();
    const auto &st = pipelineStats;
    const double sec = static_cast<double>(now - prev.ns) / 1e9;
    ReportSample cur;
    cur.ns = now;

    bool building = false;
    bool forwarding = st.health.senderEnabled.load();
#ifdef ENABLE_FRAME_BUILDER
    building = (frameBuilderPtr != nullptr);
#endif

    std::cout << "\n=== " << (final ? "Final Statistics" : "Statistics Report") << " ===  uptime "
              << std::fixed << std::setprecision(1) << st.health.uptimeSeconds(now) << " s, "
              << (final ? "rates averaged over the run" : "rates over the last ")
              << std::setprecision(1);
    if (!final) std::cout << sec << " s";
    std::cout << std::endl;

    std::cout << "Health: receiver " << readiness(true, st.health.receiverReady)
              << " | builder " << readiness(building, st.health.builderReady)
              << " | sender " << readiness(forwarding, st.health.senderReady) << std::endl;
    std::cout << "  last event " << fmtAge(st.recv.lastEvent, now);
    if (building) std::cout << " | last frame " << fmtAge(st.build.lastFrame, now);
    if (forwarding) std::cout << " | last submit " << fmtAge(st.send.lastSubmitted, now)
                              << " | last release " << fmtAge(st.send.lastReleased, now);
    std::cout << std::endl;

    if (reasPtr != nullptr) {
        auto rs = reasPtr->getStats();
        cur.udpPackets = rs.totalPackets;
        cur.udpBytes = rs.totalBytes;
        std::cout << "--- Receive: E2SAR reassembler (LB #1, measured by E2SAR) ---" << std::endl;
        std::cout << "  UDP packets: " << rs.totalPackets << " (" << fmtRate(cur.udpPackets - prev.udpPackets, sec, "pkt")
                  << ")   UDP payload: " << fmtBytes(rs.totalBytes) << " ("
                  << fmtByteRate(cur.udpBytes - prev.udpBytes, sec) << ")" << std::endl;
        std::cout << "  Events reassembled: " << rs.eventSuccess
                  << "   Lost in reassembly (segments missing at timeout): " << rs.reassemblyLoss
                  << "   Lost on enqueue (queue full): " << rs.enqueueLoss << std::endl;
        std::cout << "  Bad headers: " << rs.badHeaderDiscards << "   Data socket errors: " << rs.dataErrCnt
                  << "   gRPC errors: " << rs.grpcErrCnt << std::endl;
    }

    cur.events = st.recv.eventsReceived.value();
    cur.eventBytes = st.recv.bytesReceived.value();
    std::cout << "--- Receive: coda-fb reception loop ---" << std::endl;
    std::cout << "  Events: " << cur.events << " (" << fmtRate(cur.events - prev.events, sec, "evt")
              << ")   Bytes: " << fmtBytes(cur.eventBytes) << " ("
              << fmtByteRate(cur.eventBytes - prev.eventBytes, sec) << ")" << std::endl;
    std::cout << "  recvEvent errors: " << st.recv.recvErrors.value()
              << "   Payload validation errors: " << st.recv.payloadValidationErrors.value()
              << "   Wrong endianness: " << st.recv.wrongEndianness.value() << std::endl;

    if (building) {
        const auto &b = st.build;
        cur.frames = b.framesComplete.value() + b.framesPartial.value();
        cur.frameBytes = b.bytesSerialized.value();
        std::cout << "--- Frame building ---" << std::endl;
        std::cout << "  Slices in: " << b.slicesIn.value()
                  << "   Frames: complete " << b.framesComplete.value()
                  << ", partial " << b.framesPartial.value()
                  << ", build failed " << b.framesBuildFailed.value()
                  << ", discarded at shutdown " << b.framesDiscardedShutdown.value()
                  << " (" << b.slicesDiscardedShutdown.value() << " slices)" << std::endl;
        std::cout << "  Built: " << fmtRate(cur.frames - prev.frames, sec, "frames") << ", "
                  << fmtByteRate(cur.frameBytes - prev.frameBytes, sec)
                  << "   Pending now: " << b.slicesPending.value() << " slices, "
                  << b.framesInProgress.value() << " frames in progress" << std::endl;
        std::cout << "  Frame size " << fmtQuantiles(b.frameBytes, true)
                  << "   Serialize " << fmtQuantiles(b.serializeNs, false)
                  << "   Assembly " << fmtQuantiles(b.assemblyNs, false) << std::endl;
#ifdef ENABLE_FRAME_BUILDER
        uint64_t delivered, slices, errors, bytes;
        frameBuilderPtr->getStatistics(delivered, slices, errors, bytes);
        std::cout << "  Delivered to every enabled output: " << delivered << " frames, " << fmtBytes(bytes)
                  << "   Output errors: ET " << b.etOutputErrors.value()
                  << ", file " << b.fileOutputErrors.value() << std::endl;
#endif
    } else {
        cur.written = buildEventsWritten.load();
        cur.writtenBytes = buildEventsBytesTotal.load();
        std::cout << "--- Output frames (written to file) ---" << std::endl;
        std::cout << "  Frames written: " << cur.written << " (" << fmtRate(cur.written - prev.written, sec, "frames")
                  << ")   Bytes: " << fmtBytes(cur.writtenBytes) << " ("
                  << fmtByteRate(cur.writtenBytes - prev.writtenBytes, sec) << ")   Write errors: "
                  << writeErrors.load() << std::endl;
    }

#ifdef ENABLE_FRAME_BUILDER
    if (forwarding) {
        const auto &f = st.send;
        cur.submitted = f.framesSubmitted.value();
        cur.submittedBytes = f.bytesSubmitted.value();
        std::cout << "--- Forwarding to LB #2 (local submission only; downstream delivery is NOT measured) ---" << std::endl;
        std::cout << "  Offered: " << f.framesOffered.value()
                  << "   Submitted: " << cur.submitted << " (" << fmtRate(cur.submitted - prev.submitted, sec, "frames")
                  << ", " << fmtByteRate(cur.submittedBytes - prev.submittedBytes, sec) << ")"
                  << "   Released by E2SAR: " << f.framesReleased.value() << std::endl;
        std::cout << "  Dropped: overflow " << f.framesDroppedOverflow.value()
                  << ", shutdown " << f.framesDroppedShutdown.value()
                  << "   Rejected: " << f.framesRejected.value()
                  << "   Partial not forwarded: " << f.framesNotForwardedPartial.value()
                  << "   Queue-full retries: " << f.submitRetries.value();
        if (final) std::cout << "   Abandoned at shutdown: " << f.framesAbandonedShutdown.value();
        std::cout << std::endl;
        std::cout << "  In flight: " << f.inFlightFrames.value() << "/" << f.capacityFrames.value() << " frames, "
                  << fmtBytes(f.inFlightBytes.value()) << "/" << fmtBytes(f.capacityBytes.value())
                  << "   Blocked: " << f.blockedSubmits.value() << " submits, "
                  << fmtNs(static_cast<double>(f.blockedNs.value())) << " total" << std::endl;
        std::cout << "  Queue wait " << fmtQuantiles(f.queueWaitNs, false)
                  << "   E2SAR release latency " << fmtQuantiles(f.releaseLatencyNs, false) << std::endl;
        if (sendTransportPtr != nullptr) {
            auto ss = sendTransportPtr->sendStats();
            auto sy = sendTransportPtr->syncStats();
            cur.sentPackets = ss.msgCnt;
            std::cout << "--- Forwarding: E2SAR segmenter (measured by E2SAR) ---" << std::endl;
            std::cout << "  Packets: " << ss.msgCnt << " (" << fmtRate(cur.sentPackets - prev.sentPackets, sec, "pkt")
                      << "; coda-fb estimate " << f.packetsEstimated.value() << ")"
                      << "   Packet send errors: " << ss.errCnt;
            if (ss.errCnt > 0) std::cout << " (last: " << strerror(ss.lastErrno) << ")";
            std::cout << "   Sync messages: " << sy.msgCnt << ", sync errors: " << sy.errCnt << std::endl;
        }
    }
#endif
    std::cout << "=========================" << std::endl;
    prev = cur;
}

std::mutex statsMutex;
std::condition_variable statsCV;
bool statsStop{false};

/** Prints a report every --report-interval ms until stopStatsReporting(). */
void statsReportingThread()
{
    ReportSample prev;
    prev.ns = pipelineStats.health.startNs;
    std::unique_lock<std::mutex> lock(statsMutex);
    while (!statsCV.wait_for(lock, std::chrono::milliseconds(reportThreadSleepMs), [] { return statsStop; }))
    {
        lock.unlock();
        printStatisticsReport(prev, false);
        lock.lock();
    }
}

void stopStatsReporting(std::thread &t)
{
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        statsStop = true;
    }
    statsCV.notify_all();
    if (t.joinable())
        t.join();
}

/**
 * Render /metrics. Runs on the endpoint thread. Before startup completes only
 * coda-fb's own (atomic) statistics are rendered; E2SAR families appear once
 * the reassembler and segmenter exist.
 */
std::string renderMetrics(bool building, bool forwarding)
{
    codafb::MetricsSources src;
    src.stats = &pipelineStats;
    src.building = building;
    src.forwarding = forwarding;
    src.version = CODA_FB_VERSION;
    src.e2sarVersion = get_Version();
    if (e2sarStatsReady.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(statsSourceMutex);
        if (reasPtr != nullptr) {
            auto r = reasPtr->getStats();
            codafb::E2SARReceiveSnapshot rs;
            rs.packets = r.totalPackets;
            rs.bytes = r.totalBytes;
            rs.reassembled = r.eventSuccess;
            rs.lostReassembly = r.reassemblyLoss;
            rs.lostEnqueue = r.enqueueLoss;
            rs.badHeaders = r.badHeaderDiscards;
            rs.socketErrors = static_cast<uint64_t>(r.dataErrCnt);
            rs.grpcErrors = static_cast<uint64_t>(r.grpcErrCnt);
            src.recv = rs;
        }
#ifdef ENABLE_FRAME_BUILDER
        if (sendTransportPtr != nullptr) {
            auto ss = sendTransportPtr->sendStats();
            auto sy = sendTransportPtr->syncStats();
            codafb::E2SARSendSnapshot snap;
            snap.packets = ss.msgCnt;
            snap.packetErrors = ss.errCnt;
            snap.syncMessages = sy.msgCnt;
            snap.syncErrors = sy.errCnt;
            src.send = snap;
        }
#endif
    }
    codafb::stampNow(src);
    return codafb::renderPrometheus(src);
}

/** Stop serving /metrics; must happen before the E2SAR objects are deleted. */
void stopMetricsEndpoint()
{
    e2sarStatsReady = false;
    if (metricsEndpointPtr != nullptr) {
        metricsEndpointPtr->stop();
        std::cout << "Metrics endpoint stopped after " << metricsEndpointPtr->scrapes() << " scrapes" << std::endl;
        delete metricsEndpointPtr;
        metricsEndpointPtr = nullptr;
    }
}

// ============================================================================
// Startup and shutdown
// ============================================================================

/**
 * Release everything that startup created, in reverse order, after a startup
 * failure or an exception. Safe to call with any subset initialized.
 */
void cleanupAfterFailure()
{
    stopMetricsEndpoint();

    if (reasPtr != nullptr)
    {
        if (workerRegistered)
        {
            auto deregres = reasPtr->deregisterWorker();
            if (deregres.has_error())
                std::cerr << "Unable to deregister worker: "
                          << codafb::redactCredentials(deregres.error().message()) << std::endl;
            workerRegistered = false;
        }
        reasPtr->stopThreads();
        delete reasPtr;
        reasPtr = nullptr;
    }
    pipelineStats.health.receiverReady = false;

#ifdef ENABLE_FRAME_BUILDER
    bool buildersJoined = true;
    if (frameBuilderPtr != nullptr)
    {
        if (pipelineStats.health.builderReady.exchange(false))
        {
            frameBuilderPtr->stop(3000);
            buildersJoined = frameBuilderPtr->allThreadsJoined();
        }
        // leaked if a builder thread is still running, as on normal exit
        if (buildersJoined)
            delete frameBuilderPtr;
        frameBuilderPtr = nullptr;
    }
    if (frameSenderPtr != nullptr)
    {
        frameSenderPtr->beginShutdown(std::chrono::steady_clock::now());
        size_t abandoned = frameSenderPtr->finishShutdown();
        senderSafeToDelete = (abandoned == 0) && buildersJoined;
    }
    if (sendTransportPtr != nullptr)
    {
        if (senderSafeToDelete)
        {
            sendTransportPtr->shutdown();
            delete frameSenderPtr;
            delete sendTransportPtr;
        }
        else
        {
            sendTransportPtr->abandon();
        }
        frameSenderPtr = nullptr;
        sendTransportPtr = nullptr;
    }
    pipelineStats.health.senderReady = false;
#endif

    if (globalOutputFd >= 0)
    {
        close(globalOutputFd);
        globalOutputFd = -1;
    }
}

/**
 * Orderly shutdown in data-flow order, after the reception loop has exited:
 * 1. Deregister from LB #1 first, before any call that can block, so the LB
 *    stops sending even if a later step hangs; then stop the receive threads.
 * 2. Bound every wait in the frame sender by the drain deadline.
 * 3. Stop the builder threads; each finishes and forwards its current frame.
 *    Slices still waiting in builder FIFOs are counted as discarded.
 * 4. Wait, up to the deadline, until E2SAR has released every forwarded frame.
 * 5. Stop the E2SAR segmenter and remove the sender registration from LB #2.
 *    If frames are still in flight, the segmenter is left running instead
 *    (it still uses the buffers) and only the registration is removed.
 */
void shutdownPipeline(Reassembler *r, e2sar::FrameBuilder *frameBuilder, int drainTimeoutMs)
{
    if (r != nullptr) {
        std::cout << "Deregistering worker from control plane..." << std::endl;
        auto deregres = r->deregisterWorker();
        if (deregres.has_error())
            std::cerr << "Unable to deregister worker: "
                      << codafb::redactCredentials(deregres.error().message()) << std::endl;
        else
            std::cout << "Worker deregistered successfully" << std::endl;
        workerRegistered = false;

        std::cout << "Stopping reassembler threads..." << std::endl;
        r->stopThreads();
        std::cout << "Reassembler threads stopped" << std::endl;
    }
    pipelineStats.health.receiverReady = false;

#ifdef ENABLE_FRAME_BUILDER
    if (frameSenderPtr != nullptr) {
        std::cout << "Draining forwarded frames (deadline " << drainTimeoutMs << " ms)..." << std::endl;
        frameSenderPtr->beginShutdown(std::chrono::steady_clock::now() + std::chrono::milliseconds(drainTimeoutMs));
    }

    // Builders may wait for the sender up to the drain deadline, and ET calls
    // time out after 2 s, so allow for both before detaching a thread.
    if (frameBuilder != nullptr) {
        frameBuilder->stop(drainTimeoutMs + 3000);
        pipelineStats.health.builderReady = false;
    }

    if (frameSenderPtr != nullptr) {
        size_t abandoned = frameSenderPtr->finishShutdown();
        pipelineStats.health.senderReady = false;
        bool buildersJoined = (frameBuilder == nullptr) || frameBuilder->allThreadsJoined();
        senderSafeToDelete = (abandoned == 0) && buildersJoined;
        if (abandoned == 0) {
            std::cout << "All forwarded frames were released by E2SAR" << std::endl;
        } else {
            std::cerr << "WARNING: " << abandoned << " forwarded frame(s) were still in flight at the drain "
                      << "deadline; they are counted as abandoned and may or may not have been sent" << std::endl;
        }
        // a concurrent scrape may be reading the segmenter's statistics
        std::lock_guard<std::mutex> lock(statsSourceMutex);
        if (senderSafeToDelete) {
            sendTransportPtr->shutdown();
        } else {
            sendTransportPtr->abandon();
        }
    }
#else
    (void)drainTimeoutMs;
#endif
}

/**
 * Print final statistics and release what shutdownPipeline() left behind.
 * Called from the main thread after the statistics thread was joined.
 */
void performFinalCleanup()
{
    std::cout << "\nPerforming final cleanup..." << std::endl;

    // Prometheus keeps the last scraped values; stop before deleting E2SAR objects.
    stopMetricsEndpoint();

    // Close output file if open
    if (globalOutputFd >= 0)
    {
        std::cout << "Closing output file..." << std::endl;
        fsync(globalOutputFd);  // Ensure all data is written to disk
        close(globalOutputFd);
        globalOutputFd = -1;
    }

    ReportSample whole;
    whole.ns = pipelineStats.health.startNs;
    printStatisticsReport(whole, true);

    // NOTE: We intentionally leak the frameBuilderPtr to avoid hanging
    // in the destructor if a builder thread was detached. The OS
    // will clean up all resources when the process exits.
    frameBuilderPtr = nullptr;

#ifdef ENABLE_FRAME_BUILDER
    // Leaked for the same reason when frames or threads outlived shutdown.
    if (senderSafeToDelete) {
        delete frameSenderPtr;
        delete sendTransportPtr;
    }
    frameSenderPtr = nullptr;
    sendTransportPtr = nullptr;
#endif

    if (reasPtr != nullptr)
    {
        // deregisterWorker() and stopThreads() were already called in shutdownPipeline()
        delete reasPtr;
        reasPtr = nullptr;
    }
}

/**
 * ============================================================================
 * EVIO Payload Metadata Structure
 * ============================================================================
 *
 * This structure holds metadata extracted from a reassembled EVIO frame's
 * payload. The metadata is parsed from specific words in the EVIO structure
 * to ensure data integrity and provide accurate timing information.
 */
struct EVIOMetadata {
    uint64_t timestamp;      // 64-bit timestamp extracted from payload words 15-16
                              // Used for synchronizing frames across multiple streams

    uint32_t frameNumber;    // Frame/event sequence number from payload word 14
                              // Identifies this specific frame in the data stream

    uint16_t dataId;         // ROC (Readout Controller) or Stream ID from payload word 10
                              // Identifies which data source produced this frame

    bool valid;              // Flag indicating if payload passed all validation checks
                              // false = frame should be skipped due to corruption/errors

    bool wrongEndian;        // Flag indicating if data had incorrect byte ordering
                              // true = data was byte-swapped but successfully corrected
};

#ifdef ENABLE_FRAME_BUILDER
// Direct ET functions removed - frame builder is always used
#if 0  // Keeping for reference only
bool initializeDirectET_REMOVED(const std::string& etFile, const std::string& etHost,
                        int etPort, int etEventSize) {
    std::cout << "Initializing direct ET connection..." << std::endl;

    // Create ET open configuration
    et_openconfig openConfig;
    et_open_config_init(&openConfig);

    // Set connection mode based on host parameter
    if (etHost.empty()) {
        et_open_config_setcast(openConfig, ET_BROADCAST);
        std::cout << "  Mode: broadcast (local network)" << std::endl;
    } else {
        et_open_config_setcast(openConfig, ET_DIRECT);
        et_open_config_sethost(openConfig, etHost.c_str());
        std::cout << "  Mode: direct connection to " << etHost;
        if (etPort > 0) std::cout << ":" << etPort;
        std::cout << std::endl;
    }

    // Configure port if specified (use setserverport for ET server's TCP port)
    if (etPort > 0) {
        et_open_config_setserverport(openConfig, etPort);
    }

    // Set timeout for connection attempts
    struct timespec timeout;
    timeout.tv_sec = 10;
    timeout.tv_nsec = 0;
    et_open_config_settimeout(openConfig, timeout);

    // Open ET system
    int status = et_open(&directEtSystem, etFile.c_str(), openConfig);
    et_open_config_destroy(openConfig);

    if (status != ET_OK) {
        std::cerr << "Failed to open ET system: " << etFile << " (status=" << status << ")" << std::endl;
        return false;
    }

    std::cout << "  ET system opened: " << etFile << std::endl;

    // Attach to Grand Central station (ID 0) - the primary entry point of ET system
    std::cout << "  Attaching to GRAND_CENTRAL station (ID 0)" << std::endl;
    status = et_station_attach(directEtSystem, 0, &directEtAttachment);
    if (status != ET_OK) {
        std::cerr << "Failed to attach to GRAND_CENTRAL station (status=" << status << ")" << std::endl;
        et_close(directEtSystem);
        directEtSystem = nullptr;
        return false;
    }

    std::cout << "  Attached to GRAND_CENTRAL successfully" << std::endl;
    std::cout << "  Event size: " << (etEventSize / (1024*1024)) << " MB" << std::endl;
    std::cout << "Direct ET connection initialized successfully" << std::endl;

    return true;
}

/**
 * Cleanup direct ET connection
 */
void cleanupDirectET_REMOVED() {
    // Removed - frame builder is always used
}
#endif  // 0 - reference code
#endif  // ENABLE_FRAME_BUILDER

/**
 * ============================================================================
 * EVIO-6 CODA Tags and Data Types
 * ============================================================================
 */
namespace CODATag {
    const uint16_t STREAMING_PHYS = 0xFFD0;      // Top-level streaming physics event
    const uint16_t STREAMING_SIB_BUILT = 0xFFD1; // Stream Info Bank (built/aggregated)
    const uint8_t  STREAMING_TSS_BUILT = 0x01;   // Time Slice Segment
    const uint8_t  STREAMING_AIS_BUILT = 0x02;   // Aggregation Info Segment
}

namespace DataType {
    const uint8_t BANK = 0x10;    // EVIO bank type
    const uint8_t SEGMENT = 0x20; // EVIO segment type
}

/**
 * ============================================================================
 * Byte Swap Utility for 32-bit Words
 * ============================================================================
 *
 * Converts a 32-bit word from one endianness to another by reversing byte order.
 * Used when EVIO payload is detected to have wrong endianness.
 *
 * Example: 0x12345678 -> 0x78563412
 *
 * @param val  The 32-bit value to byte-swap
 * @return     The byte-swapped 32-bit value
 */
inline uint32_t swap32(uint32_t val) {
    return ((val & 0x000000FF) << 24) |  // Move byte 0 to byte 3
           ((val & 0x0000FF00) << 8)  |  // Move byte 1 to byte 2
           ((val & 0x00FF0000) >> 8)  |  // Move byte 2 to byte 1
           ((val & 0xFF000000) >> 24);   // Move byte 3 to byte 0
}

/**
 * ============================================================================
 * Parse EVIO Payload and Extract Metadata
 * ============================================================================
 *
 * Validates a reassembled EVIO frame and extracts timing/identification metadata
 * from its payload structure. This ensures data integrity and provides accurate
 * information for frame aggregation.
 *
 * EVIO Payload Structure (32-bit words, 1-indexed as per specification):
 * -------------------------------------------------------------------------
 * Word 1-7:   [Header data - not parsed here]
 * Word 8:     0xc0da0100    - Magic number (correctness verification)
 * Word 9:     ROC bank length
 * Word 10:    0x0010_10_ss  - ROC_ID (ss = stream/ROC identifier)
 * Word 11:    Stream info bank length
 * Word 12:    0xFF30_20_ss  - Stream info header
 * Word 13:    0x31_01_LLLL  - Time slice segment header (LLLL = length)
 * Word 14:    Frame number  - Sequence number for this frame
 * Word 15:    Timestamp[31:0]  - Lower 32 bits of 64-bit timestamp
 * Word 16:    Timestamp[63:32] - Upper 32 bits of 64-bit timestamp
 *
 * @param payload      Pointer to the reassembled frame payload data
 * @param payloadSize  Size of the payload in bytes
 * @return             EVIOMetadata structure with extracted data and validation flags
 */
EVIOMetadata parseEVIOPayload(const u_int8_t* payload, size_t payloadSize) {
    // Initialize metadata structure with all zeros and invalid flags
    EVIOMetadata meta = {0, 0, 0, false, false};

    // ========================================================================
    // STEP 1: Validate Minimum Payload Size
    // ========================================================================
    // We need at least 16 32-bit words (64 bytes) to access all required fields
    if (payloadSize < 64) {
        std::cerr << "ERROR: Payload too small for EVIO format: " << payloadSize
                  << " bytes (minimum: 64 bytes)" << std::endl;
        return meta;  // Return invalid metadata
    }

    // ========================================================================
    // STEP 2: Interpret Payload as Array of 32-bit Words
    // ========================================================================
    // Cast byte array to 32-bit word array for easier access
    // NOTE: Assumes payload is properly aligned (should be from network stack)
    const uint32_t* words = reinterpret_cast<const uint32_t*>(payload);

    // ========================================================================
    // STEP 3: Check Magic Number at Word 8 (Index 7 in 0-Based Array)
    // ========================================================================
    // The magic number serves two purposes:
    // 1. Verifies frame was correctly reassembled (no missing/corrupt packets)
    // 2. Indicates correct byte ordering (endianness)

    uint32_t magic = words[7];  // Word 8 in 1-based indexing
    bool needsSwap = false;

    if (magic == 0xc0da0100) {
        // SUCCESS: Correct magic number and correct endianness
        needsSwap = false;

    } else if (magic == 0x0001dac0) {
        // WARNING: Magic number is byte-swapped (wrong endianness detected)
        // We can still use the data, but need to swap all words
        needsSwap = true;
        meta.wrongEndian = true;

    } else {
        // FAILURE: Invalid magic number - frame is corrupted or incorrectly assembled
        std::cerr << "ERROR: Invalid EVIO magic number at word 8: 0x" << std::hex << magic
                  << std::dec << " (expected 0xc0da0100 or 0x0001dac0)" << std::endl;
        return meta;  // Return invalid metadata
    }

    // ========================================================================
    // STEP 4: Define Helper Function for Reading Words with Byte Swapping
    // ========================================================================
    // This lambda automatically swaps bytes if wrong endianness was detected
    auto readWord = [&](size_t index) -> uint32_t {
        return needsSwap ? swap32(words[index]) : words[index];
    };

    // ========================================================================
    // STEP 5: Extract and Validate ROC_ID from Word 10 (Index 9)
    // ========================================================================
    // Word 10 format: ROC_ID (16 bits) + 0x10 + StreamStatus (8 bits)
    //   - Upper 16 bits (bits 31-16): ROC_ID (readout controller identifier)
    //   - Next 8 bits (bits 15-8) must be 0x10 (fixed identifier)
    //   - Lowest 8 bits (bits 7-0) = StreamStatus flags

    uint32_t word10 = readWord(9);  // Word 10 in 1-based indexing

    // Extract the three components of word 10
    uint16_t upper16 = (word10 >> 16) & 0xFFFF;  // Bits 31-16 (ROC_ID)
    uint8_t  next8   = (word10 >> 8)  & 0xFF;    // Bits 15-8 (must be 0x10)
    uint8_t  ss      = word10 & 0xFF;             // Bits 7-0 (StreamStatus)

    // Validate only the middle byte (0x10) - upper 16 bits may vary by format version
    if (next8 != 0x10) {
        std::cerr << "ERROR: Invalid ROC_ID format at word 10: 0x" << std::hex << word10
                  << std::dec << " (expected middle byte 0x10)" << std::endl;
        return meta;  // Return invalid metadata
    }

    // Extract the ROC/Stream ID from the upper 16 bits
    meta.dataId = upper16;

    // ========================================================================
    // STEP 6: Extract Frame Number from Word 14 (Index 13)
    // ========================================================================
    // This is a simple 32-bit sequence number identifying this frame
    meta.frameNumber = readWord(13);  // Word 14 in 1-based indexing

    // ========================================================================
    // STEP 7: Extract 64-bit Timestamp from Words 15-16 (Indices 14-15)
    // ========================================================================
    // The timestamp is split across two consecutive 32-bit words:
    //   - Word 15: Lower 32 bits [31:0]
    //   - Word 16: Upper 32 bits [63:32]

    uint32_t ts_low  = readWord(14);  // Word 15: timestamp bits [31:0]
    uint32_t ts_high = readWord(15);  // Word 16: timestamp bits [63:32]

    // Combine the two 32-bit halves into a single 64-bit timestamp
    // Example: ts_high=0x12345678, ts_low=0x9ABCDEF0 => 0x123456789ABCDEF0
    meta.timestamp = (static_cast<uint64_t>(ts_high) << 32) | ts_low;

    // ========================================================================
    // STEP 8: Mark Metadata as Valid
    // ========================================================================
    // All validation checks passed - metadata is ready to use
    meta.valid = true;

    return meta;
}

result<int> prepareToReceive(Reassembler &r)
{
    // Get hostname and register with control plane
    std::cout << "Getting hostname... " << std::flush;
    auto hostname_res = NetUtil::getHostName();
    if (hostname_res.has_error())
    {
        return E2SARErrorInfo{hostname_res.error().code(), hostname_res.error().message()};
    }
    std::cout << "done" << std::endl;

    std::cout << "Registering worker '" << hostname_res.value() << "' with control plane... " << std::flush;
    auto regres = r.registerWorker(hostname_res.value());
    if (regres.has_error())
    {
        return E2SARErrorInfo{E2SARErrorc::RPCError,
            "Unable to register worker node: " + codafb::redactCredentials(regres.error().message())};
    }
    // From here on a failure must deregister, see cleanupAfterFailure()
    workerRegistered = true;
    std::cout << "done" << std::endl;

    // Open sockets and start receiver threads
    auto openRes = r.openAndStart();
    if (openRes.has_error())
        return openRes;

    return 0;
}

/**
 * ============================================================================
 * Main Frame Reception and Processing Loop
 * ============================================================================
 *
 * Receives reassembled frames from the E2SAR reassembler and either:
 * - Sends them to the frame builder for aggregation (if enabled), which also
 *   forwards built frames to the second load balancer (if enabled)
 * - Writes them directly to an output file (original mode)
 *
 * Returns when Ctrl+C is pressed; the caller then runs shutdownPipeline().
 *
 * Each frame is validated by parsing its EVIO payload structure to ensure
 * data integrity and extract accurate metadata.
 *
 * @param r              Pointer to the E2SAR Reassembler
 * @param outputFd       File descriptor for output file (used in file-only mode)
 * @param frameBuilder   Pointer to frame builder (nullptr if not using frame builder)
 * @return               Result code (0 on success, error otherwise)
 */
result<int> receiveAndWriteFrames(Reassembler *r, int outputFd, e2sar::FrameBuilder* frameBuilder)
{
    // Variables to hold received frame data from reassembler
    u_int8_t *eventBuf{nullptr};    // Pointer to reassembled payload data
    size_t eventSize;                // Size of reassembled payload in bytes
    EventNum_t eventNum;             // Event number from reassembler
    u_int16_t dataId;                // Data ID from reassembler

    // Print startup message based on mode
    if (frameBuilder != nullptr) {
        std::cout << "Starting frame reception and frame building loop..." << std::endl;
    } else {
        std::cout << "Starting frame reception and file writing loop..." << std::endl;
    }

    // ========================================================================
    // MAIN RECEPTION LOOP
    // ========================================================================
    // Continue processing until Ctrl+C is pressed (threadsRunning = false)
    while(threadsRunning)
    {
        // ====================================================================
        // STEP 1: Receive Next Reassembled Frame
        // ====================================================================
        // Attempt to receive a frame with 1 second timeout
        // Returns -1 on timeout (not an error), error code if real error
        auto getEvtRes = r->recvEvent(&eventBuf, &eventSize, &eventNum, &dataId, 1000);

        if (getEvtRes.has_error())
        {
            // Error occurred during reception - log and continue
            pipelineStats.recv.recvErrors.add();
            continue;
        }

        // Timeout occurred (no frame received within 1 second) - continue waiting
        if (getEvtRes.value() == -1)
            continue;

        // Successfully received a frame
        pipelineStats.recv.eventsReceived.add();
        pipelineStats.recv.bytesReceived.add(eventSize);  // Track total data volume
        pipelineStats.recv.lastEvent.mark();

        // ====================================================================
        // STEP 2: Parse and Validate EVIO Payload
        // ====================================================================
        // Extract metadata from payload and verify data integrity
        // This checks the magic number, endianness, and extracts timestamp/IDs
        EVIOMetadata meta = parseEVIOPayload(eventBuf, eventSize);

        if (!meta.valid) {
            // VALIDATION FAILED: Payload is corrupt or incorrectly assembled
            // This frame cannot be used - skip it and continue to next frame
            pipelineStats.recv.payloadValidationErrors.add();
            std::cerr << "Skipping frame " << eventNum << " due to invalid payload" << std::endl;

            // Clean up the unusable frame buffer
            delete[] eventBuf;
            eventBuf = nullptr;
            continue;  // Skip to next frame
        }

        // ====================================================================
        // STEP 3: Check for Endianness Issues
        // ====================================================================
        if (meta.wrongEndian) {
            // WARNING: Frame had wrong byte ordering but was corrected
            // This indicates potential issue with data source but data is usable
            pipelineStats.recv.wrongEndianness.add();
            // Note: The parseEVIOPayload function has already byte-swapped
            // the data, so we can proceed normally
        }

        // ====================================================================
        // STEP 4: Use Extracted Metadata from Payload
        // ====================================================================
        // Extract timestamp and ROC ID from payload for data quality.
        // Use reassembler's eventNum for frame building (stream-independent numbering).

        uint64_t timestamp   = meta.timestamp;     // 64-bit timestamp from payload words 15-16
        uint32_t frameNumber = eventNum;           // Use reassembler's event number for alignment
        uint16_t rocId       = meta.dataId;        // ROC ID from payload word 10
        uint32_t payloadFrameNum = meta.frameNumber;  // Frame number from payload (for reference)

        // ====================================================================
        // VERBOSE LOGGING: Print frame information if requested
        // ====================================================================
        if (verboseFrameInfo) {
            std::cout << "[FRAME] EventNum=" << std::setw(8) << eventNum
                      << " | Timestamp=" << std::setw(16) << timestamp
                      << " | ROC_ID=" << std::setw(4) << rocId
                      << " | PayloadFrameNum=" << std::setw(8) << payloadFrameNum
                      << " | Size=" << std::setw(8) << eventSize << " bytes"
                      << std::endl;
        }

        // ====================================================================
        // STEP 5: Route Frame Based on Mode
        // ====================================================================
        // Two modes:
        // 1. Frame Building Mode (when frameBuilder != nullptr):
        //    - Send to frame builder for aggregation
        //    - Frame builder validates, groups by timestamp, builds EVIO-6 format
        //    - Outputs to ET system and/or files with 2GB rollover
        // 2. Reassembly-Only Mode (when frameBuilder == nullptr):
        //    - Write raw reassembled frames directly to file
        //    - No aggregation, no EVIO-6 formatting

        if (frameBuilder != nullptr) {
            // Frame building mode: send to aggregator
            // IMPORTANT: Ownership of eventBuf is transferred to frame builder!
            frameBuilder->addTimeSlice(
                timestamp,       // 64-bit timestamp for synchronization
                frameNumber,     // Frame sequence number
                rocId,           // ROC/Stream identifier
                eventBuf,        // Pointer to payload data (ownership transferred!)
                eventSize        // Size of payload
            );
            // Note: eventBuf is NOT deleted here - frame builder now owns it
            eventBuf = nullptr;  // Clear pointer to prevent accidental double-delete
        } else {
            // Reassembly-only mode: write raw frame directly to file

            // ====================================================================
            // VERBOSE REASSEMBLE: Print event numbers for all streams
            // ====================================================================
            if (verboseReassemble) {
                std::cout << "[REASSEMBLE] EventNum=" << std::setw(8) << eventNum
                          << " | DataID=" << std::setw(4) << dataId
                          << " | ROC_ID=" << std::setw(4) << rocId
                          << " | PayloadFrameNum=" << std::setw(8) << payloadFrameNum
                          << " | Size=" << std::setw(8) << eventSize << " bytes"
                          << std::endl;
            }

            if (outputFd >= 0) {
                // Acquire mutex to ensure thread-safe file writing
                std::lock_guard<std::mutex> lock(fileMutex);

                // Write raw frame to file
                ssize_t bytesWritten = write(outputFd, eventBuf, eventSize);

                if (bytesWritten < 0) {
                    std::cerr << "Error writing event " << eventNum << " to file: "
                              << strerror(errno) << std::endl;
                    writeErrors++;
                }
                else if (static_cast<size_t>(bytesWritten) != eventSize) {
                    std::cerr << "Incomplete write for event " << eventNum
                              << ": wrote " << bytesWritten << " of " << eventSize
                              << " bytes" << std::endl;
                    writeErrors++;
                } else {
                    buildEventsWritten++;
                    buildEventsBytesTotal += eventSize;
                }
            }

            // ====================================================================
            // STEP 6: Clean Up Frame Buffer
            // ====================================================================
            // The event buffer was allocated by the reassembler's recvEvent()
            // We must delete it here to avoid memory leaks
            delete[] eventBuf;
            eventBuf = nullptr;
        }

    }  // End of main reception loop

    // Shutdown (deregistration, draining) happens in shutdownPipeline()
    std::cout << "\nFrame reception loop completed" << std::endl;
    return 0;
}

/** 32-bit FNV-1a, used to derive a default E2SAR source id from the host name. */
uint32_t fnv1a32(const std::string &s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

int main(int argc, char **argv)
{
    po::options_description od("CODA Frame Builder Options");

    // Command line variables
    std::string recvUriStr;
    std::string legacyUriStr;
    std::string recvIP;
    std::string outputDir;
    std::string filePrefix;
    std::string fileExtension;
    u_int16_t recvStartPort;
    size_t numThreads;
    int sockBufSize;
    int eventTimeoutMS;
    std::vector<int> coreList;
    int numaNode;
    bool autoIP, withCP, preferV6, validate;

    auto opts = od.add_options()("help,h", "show this help message");

    // EJFAT load balancer to receive from
    opts("recv-ejfat-uri", po::value<std::string>(&recvUriStr),
         "EJFAT instance URI of the load balancer to RECEIVE from (LB #1); coda-fb registers "
         "there as a worker (required)");
    opts("uri,u", po::value<std::string>(&legacyUriStr),
         "DEPRECATED alias for --recv-ejfat-uri, kept for existing scripts; cannot be combined with it");
    opts("output-dir,o", po::value<std::string>(&outputDir)->default_value(""),
         "directory to save received frames (required unless using frame builder)");

    // Network parameters
    opts("ip", po::value<std::string>(&recvIP)->default_value(""), 
         "IP address for receiving UDP packets (conflicts with --autoip)");
    opts("port,p", po::value<u_int16_t>(&recvStartPort)->default_value(10000), 
         "starting UDP port number (default: 10000)");
    opts("autoip", po::bool_switch(&autoIP)->default_value(false),
         "auto-detect local host IP address for receiving UDP packets (conflicts with --ip)");

    // File output parameters
    opts("prefix", po::value<std::string>(&filePrefix)->default_value("events"),
         "filename for output file (default: 'events')");
    opts("extension,e", po::value<std::string>(&fileExtension)->default_value(".bin"),
         "file extension for output file (default: '.bin')");

    // Frame builder parameters
    bool enableFramebuild;
    std::string etFile;
    std::string etHost;
    int etPort;
    std::string fbOutputDir;
    std::string fbOutputPrefix;
    int fbThreads;
    int etEventSize;
    int timestampSlop;
    int frameTimeout;
    int expectedStreams;

    // Framebuilding control option
    opts("enable-framebuild", po::value<bool>(&enableFramebuild)->default_value(false),
         "enable frame building/aggregation (default: false, reassembly-only mode)");

    // ET output options
    opts("et-file", po::value<std::string>(&etFile)->default_value(""),
         "ET system file name (empty to disable ET output)");
    opts("et-host", po::value<std::string>(&etHost)->default_value(""),
         "ET system host (empty for local/broadcast, hostname, or IP)");
    opts("et-port", po::value<int>(&etPort)->default_value(0),
         "ET system port (0 for default)");
    opts("et-event-size", po::value<int>(&etEventSize)->default_value(2*1024*1024),
         "ET event size in bytes (default: 2MB)");

    // File output options for frame builder
    opts("fb-output-dir", po::value<std::string>(&fbOutputDir)->default_value(""),
         "frame builder file output directory (empty to disable file output)");
    opts("fb-output-prefix", po::value<std::string>(&fbOutputPrefix)->default_value("frames"),
         "frame builder file output prefix (default: frames)");
    opts("fb-threads", po::value<int>(&fbThreads)->default_value(1),
         "number of parallel frame builder threads (default: 1)");

    // Frame building options
    opts("framenumber-slop", po::value<int>(&timestampSlop)->default_value(0),
         "maximum frame number difference allowed between corrected event numbers within the same frame - "
         "used for data quality validation only. After startup correction factors are applied, "
         "this checks if corrected event numbers are within slop range (default: 0)");
    opts("frame-timeout", po::value<int>(&frameTimeout)->default_value(1000),
         "frame building timeout in milliseconds - how long to wait for all expected streams "
         "before building a partial frame (default: 1000)");
    opts("expected-streams", po::value<int>(&expectedStreams)->default_value(1),
         "number of expected data streams per frame number - frame builds immediately when all "
         "streams arrive, or after timeout if incomplete (default: 1)");
    opts("verbose-frames", po::bool_switch(&verboseFrameInfo)->default_value(false),
         "enable verbose frame logging: print all frames and frame builder alignment messages "
         "(default: false)");
    opts("verbose-reassemble", po::bool_switch(&verboseReassemble)->default_value(false),
         "print reassembler event numbers for all streams (works in reassembly-only mode, "
         "i.e., --enable-framebuild=0) (default: false)");

    // Performance parameters
    opts("threads,t", po::value<size_t>(&numThreads)->default_value(1), 
         "number of receiver threads (default: 1)");
    opts("bufsize,b", po::value<int>(&sockBufSize)->default_value(3*1024*1024), 
         "socket buffer size in bytes (default: 3MB)");
    opts("timeout", po::value<int>(&eventTimeoutMS)->default_value(500), 
         "event reassembly timeout in milliseconds (default: 500)");

    // Control plane parameters
    // implicit_value keeps a bare --withcp/-c working; it used to be a
    // bool_switch defaulting to true, which made it impossible to turn off.
    opts("withcp,c", po::value<bool>(&withCP)->default_value(true)->implicit_value(true),
         "control plane interactions on the receive LB; --withcp=0 is for debugging only: no "
         "worker registration, and packets must arrive with LB headers (default: true)");
    opts("ipv6,6", po::bool_switch(&preferV6)->default_value(false), 
         "prefer IPv6 for control plane connections");
    opts("novalidate,v", po::bool_switch()->default_value(false), 
         "don't validate TLS certificates");

    // Advanced parameters
    opts("cores", po::value<std::vector<int>>(&coreList)->multitoken(), 
         "list of CPU cores to bind receiver threads to");
    opts("numa", po::value<int>(&numaNode)->default_value(-1), 
         "bind memory allocation to specific NUMA node (if >= 0)");
    opts("report-interval", po::value<u_int16_t>(&reportThreadSleepMs)->default_value(5000), 
         "statistics reporting interval in milliseconds (default: 5000)");

    // Prometheus metrics endpoint
    u_int16_t metricsPort;
    std::string metricsBind;
    opts("metrics-port", po::value<u_int16_t>(&metricsPort)->default_value(0),
         "serve Prometheus metrics at http://<metrics-bind>:<port>/metrics; 0 disables (default: 0)");
    opts("metrics-bind", po::value<std::string>(&metricsBind)->default_value("127.0.0.1"),
         "address the metrics endpoint listens on; 0.0.0.0 for all interfaces (default: 127.0.0.1, "
         "this host only)");

    // Forwarding of built frames to a second EJFAT load balancer
    std::string sendUriStr;
    u_int16_t sendDataId;
    u_int32_t sendSrcId;
    std::vector<std::string> sendSenderIPs;
    u_int64_t sendEventOffset;
    bool sendPartial;
    u_int16_t sendMtu;
    size_t sendSockets;
    int sendBufSize;
    float sendRateGbps;
    bool sendIPv6;
    int sendLbHdrVersion;
    u_int16_t sendWarmUpMs;
    bool sendNoCP;
    size_t sendQueueFrames;
    size_t sendQueueBytes;
    std::string sendOverflowStr;
    int sendBlockTimeoutMs;
    int sendDrainTimeoutMs;

    po::options_description sendOd("Forwarding built frames to a second EJFAT load balancer "
                                   "(requires --enable-framebuild=1)");
    auto sopts = sendOd.add_options();
    sopts("send-ejfat-uri", po::value<std::string>(&sendUriStr)->default_value(""),
          "EJFAT instance URI of the load balancer to SEND built frames to (LB #2); coda-fb "
          "registers there as a sender. Empty disables forwarding");
    sopts("send-data-id", po::value<u_int16_t>(&sendDataId)->default_value(1),
          "E2SAR data id carried by every forwarded frame; must differ between senders whose "
          "frames meet in one downstream reassembler (default: 1)");
    sopts("send-src-id", po::value<u_int32_t>(&sendSrcId)->default_value(0),
          "E2SAR event source id in sync packets, unique per sender on LB #2; 0 derives it "
          "from the host name (default: 0)");
    sopts("send-sender-ip", po::value<std::vector<std::string>>(&sendSenderIPs)->multitoken(),
          "address(es) to register as sender with LB #2 (default: the local address that routes "
          "to its data address)");
    sopts("send-event-offset", po::value<u_int64_t>(&sendEventOffset)->default_value(1),
          "outbound E2SAR event number = offset + built frame number; must be >= 1 because "
          "E2SAR reserves event number 0 (default: 1)");
    sopts("send-partial-frames", po::value<bool>(&sendPartial)->default_value(true),
          "also forward partial frames (fewer streams than --expected-streams); ET and file "
          "output always receive them (default: 1)");
    sopts("send-mtu", po::value<u_int16_t>(&sendMtu)->default_value(1500),
          "MTU for forwarded packets including IP/UDP/LB/RE headers; 0 uses the outgoing "
          "interface MTU (default: 1500)");
    sopts("send-sockets", po::value<size_t>(&sendSockets)->default_value(4),
          "number of send sockets/threads, spreads packets over LB ports (default: 4)");
    sopts("send-bufsize", po::value<int>(&sendBufSize)->default_value(3*1024*1024),
          "send socket buffer size in bytes, limited by net.core.wmem_max (default: 3MB)");
    sopts("send-rate", po::value<float>(&sendRateGbps)->default_value(-1.0f),
          "send rate limit in Gbps; negative means unlimited (default: -1)");
    sopts("send-ipv6", po::bool_switch(&sendIPv6)->default_value(false),
          "send to the IPv6 data address of LB #2");
    sopts("send-lbhdr-version", po::value<int>(&sendLbHdrVersion)->default_value(2),
          "LB header version, 2 or 3 (default: 2)");
    sopts("send-warmup", po::value<u_int16_t>(&sendWarmUpMs)->default_value(1000),
          "milliseconds of sync packets before the first frame is sent (default: 1000)");
    sopts("send-nocp", po::bool_switch(&sendNoCP)->default_value(false),
          "debugging only: no sender registration and no sync packets on LB #2; packets go "
          "straight to the URI data address");
    sopts("send-queue-frames", po::value<size_t>(&sendQueueFrames)->default_value(256),
          "maximum frames in flight (handed to E2SAR, not yet sent), 1-65534 (default: 256)");
    sopts("send-queue-bytes", po::value<size_t>(&sendQueueBytes)->default_value(512ULL*1024*1024),
          "maximum bytes in flight; a single larger frame is sent on its own (default: 512MiB)");
    sopts("send-overflow", po::value<std::string>(&sendOverflowStr)->default_value("block"),
          "when the in-flight limit is reached: 'block' the builder threads, or 'drop' the frame "
          "after --send-block-timeout. Drops are counted and logged (default: block)");
    sopts("send-block-timeout", po::value<int>(&sendBlockTimeoutMs)->default_value(1000),
          "milliseconds to wait for space before dropping, with --send-overflow=drop (default: 1000)");
    sopts("send-drain-timeout", po::value<int>(&sendDrainTimeoutMs)->default_value(10000),
          "milliseconds allowed at shutdown for completed frames to be sent before they are "
          "counted as abandoned (default: 10000)");
    od.add(sendOd);

    po::variables_map vm;

    try {
        po::store(po::parse_command_line(argc, argv, od), vm);
        
        if (vm.count("help")) {
            std::cout << "CODA Frame Builder v" << CODA_FB_VERSION << std::endl;
            std::cout << "============================================" << std::endl;
            std::cout << od << std::endl;
            std::cout << "\n=== Example Usage ===" << std::endl;
            std::cout << "\nDEFAULT MODE: Reassembly-only (no frame building)" << std::endl;
            std::cout << "Framebuilding is OPTIONAL and must be explicitly enabled with --enable-framebuild=1" << std::endl;
            std::cout << "--uri/-u still works as a deprecated alias for --recv-ejfat-uri." << std::endl;

            std::cout << "\n1. Default: Reassembly-only mode (raw frames to file):" << std::endl;
            std::cout << "coda-fb --recv-ejfat-uri 'ejfats://token@cp1:18347/lb/1?data=10.0.0.1&sync=10.0.0.1:19010' \\" << std::endl;
            std::cout << "        --ip 192.168.1.100 --port 10000 \\" << std::endl;
            std::cout << "        --output-dir /data/raw \\" << std::endl;
            std::cout << "        --prefix events --threads 4" << std::endl;

            std::cout << "\n2. Enable framebuilding with file output (EVIO-6 aggregation, 2GB auto-rollover):" << std::endl;
            std::cout << "coda-fb --recv-ejfat-uri 'ejfats://token@cp1:18347/lb/1?data=10.0.0.1&sync=10.0.0.1:19010' \\" << std::endl;
            std::cout << "        --ip 192.168.1.100 --port 10000 \\" << std::endl;
            std::cout << "        --enable-framebuild=1 \\" << std::endl;
            std::cout << "        --fb-output-dir /data/frames \\" << std::endl;
            std::cout << "        --fb-output-prefix aggregated \\" << std::endl;
            std::cout << "        --fb-threads 4" << std::endl;

            std::cout << "\n3. Enable framebuilding with ET output:" << std::endl;
            std::cout << "coda-fb --recv-ejfat-uri 'ejfats://token@cp1:18347/lb/1?data=10.0.0.1&sync=10.0.0.1:19010' \\" << std::endl;
            std::cout << "        --ip 192.168.1.100 --port 10000 \\" << std::endl;
            std::cout << "        --enable-framebuild=1 \\" << std::endl;
            std::cout << "        --et-file /tmp/et_sys_pagg \\" << std::endl;
            std::cout << "        --fb-threads 4" << std::endl;

            std::cout << "\n4. Enable framebuilding with dual output (ET + file backup):" << std::endl;
            std::cout << "coda-fb --recv-ejfat-uri 'ejfats://token@cp1:18347/lb/1?data=10.0.0.1&sync=10.0.0.1:19010' \\" << std::endl;
            std::cout << "        --ip 192.168.1.100 --port 10000 \\" << std::endl;
            std::cout << "        --enable-framebuild=1 \\" << std::endl;
            std::cout << "        --et-file /tmp/et_sys_pagg \\" << std::endl;
            std::cout << "        --fb-output-dir /data/backup \\" << std::endl;
            std::cout << "        --fb-output-prefix backup" << std::endl;

            std::cout << "\n5. Build frames and forward each one to a second load balancer:" << std::endl;
            std::cout << "coda-fb --recv-ejfat-uri 'ejfats://token1@cp1:18347/lb/1?data=10.0.0.1&sync=10.0.0.1:19010' \\" << std::endl;
            std::cout << "        --send-ejfat-uri 'ejfats://token2@cp2:18347/lb/7?data=10.0.1.1&sync=10.0.1.1:19010' \\" << std::endl;
            std::cout << "        --ip 192.168.1.100 --port 10000 \\" << std::endl;
            std::cout << "        --enable-framebuild=1 --expected-streams 3 \\" << std::endl;
            std::cout << "        --send-data-id 1 --send-queue-frames 256 --send-overflow block" << std::endl;

            return 0;
        }
        
        po::notify(vm);
    } catch (const boost::program_options::error &e) {
        std::cerr << "Command line error: " << e.what() << std::endl;
        return -1;
    }

    // Receive URI: --recv-ejfat-uri, or the deprecated --uri/-u with the same meaning
    std::string recvUriOption{"--recv-ejfat-uri"};
    if (vm.count("recv-ejfat-uri") && vm.count("uri")) {
        std::cerr << "Use either --recv-ejfat-uri or the deprecated --uri/-u, not both" << std::endl;
        return -1;
    }
    if (!vm.count("recv-ejfat-uri")) {
        if (!vm.count("uri")) {
            std::cerr << "--recv-ejfat-uri is required: the instance URI of the load balancer to receive from" << std::endl;
            return -1;
        }
        recvUriStr = legacyUriStr;
        recvUriOption = "--uri";
        std::cerr << "WARNING: --uri/-u is deprecated, use --recv-ejfat-uri (same meaning)" << std::endl;
    }

    // Validate parameters
    if (!autoIP && recvIP.empty()) {
        std::cerr << "Either --ip or --autoip must be specified" << std::endl;
        return -1;
    }

    if (autoIP && !recvIP.empty()) {
        std::cerr << "Cannot specify both --ip and --autoip" << std::endl;
        return -1;
    }

    validate = !vm["novalidate"].as<bool>();

    const bool forwarding = !sendUriStr.empty();

    // Validate based on framebuilding mode
#ifdef ENABLE_FRAME_BUILDER
    if (forwarding && !enableFramebuild) {
        std::cerr << "ERROR: --send-ejfat-uri forwards built frames and requires --enable-framebuild=1" << std::endl;
        return -1;
    }
    if (enableFramebuild) {
        // Frame building mode - check that at least one output is enabled
        bool hasETOutput = !etFile.empty();
        bool hasFileOutput = !fbOutputDir.empty();

        if (!hasETOutput && !hasFileOutput && !forwarding) {
            std::cerr << "ERROR: Frame builder mode requires at least one output:" << std::endl;
            std::cerr << "  ET output: specify --et-file" << std::endl;
            std::cerr << "  File output: specify --fb-output-dir" << std::endl;
            std::cerr << "  Forwarding to a second load balancer: specify --send-ejfat-uri" << std::endl;
            return -1;
        }
    } else {
        // Reassembly-only mode - require direct file output
        if (outputDir.empty()) {
            std::cerr << "ERROR: Reassembly-only mode requires --output-dir" << std::endl;
            std::cerr << "  (Or enable frame building with --enable-framebuild=1)" << std::endl;
            return -1;
        }
    }
#else
    if (forwarding) {
        std::cerr << "ERROR: --send-ejfat-uri requires the frame builder, which this build does not "
                  << "include (the ET library was not found at build time)" << std::endl;
        return -1;
    }
    // Without frame builder compiled in, always require direct file output
    if (outputDir.empty()) {
        std::cerr << "Output directory (--output-dir) is required (frame builder not available)" << std::endl;
        return -1;
    }
#endif

    // Validate forwarding parameters
#ifdef ENABLE_FRAME_BUILDER
    codafb::OverflowPolicy sendOverflow{codafb::OverflowPolicy::Block};
#endif
    if (forwarding) {
        std::string bad;
        if (sendQueueFrames < 1 || sendQueueFrames > 65534)
            bad = "--send-queue-frames must be between 1 and 65534 (E2SAR's queue limit)";
        else if (sendQueueBytes < 1)
            bad = "--send-queue-bytes must be at least 1";
        else if (sendEventOffset < 1)
            bad = "--send-event-offset must be >= 1: E2SAR treats event number 0 as 'next sequential number'";
        else if (sendLbHdrVersion != 2 && sendLbHdrVersion != 3)
            bad = "--send-lbhdr-version must be 2 or 3";
        else if (sendSockets < 1 || sendSockets > 128)
            bad = "--send-sockets must be between 1 and 128";
        else if (sendBlockTimeoutMs < 0 || sendDrainTimeoutMs < 0)
            bad = "--send-block-timeout and --send-drain-timeout must not be negative";
#ifdef ENABLE_FRAME_BUILDER
        else if (!codafb::parseOverflowPolicy(sendOverflowStr, sendOverflow))
            bad = "--send-overflow must be 'block' or 'drop'";
#endif
        if (!bad.empty()) {
            std::cerr << "ERROR: " << bad << std::endl;
            return -1;
        }
    }

    // Parse both URIs before touching the network, so that a typo in either
    // one fails before anything is registered.
    auto recvUriRes = codafb::parseEjfatUri(recvUriOption, recvUriStr, codafb::UriRole::Receive, withCP, preferV6);
    if (recvUriRes.has_error()) {
        std::cerr << "ERROR: " << recvUriRes.error().message() << std::endl;
        return -1;
    }
    const EjfatURI recvUri = recvUriRes.value();

    std::optional<EjfatURI> sendUri;
    if (forwarding) {
        auto sendUriRes = codafb::parseEjfatUri("--send-ejfat-uri", sendUriStr, codafb::UriRole::Send,
                                                !sendNoCP, preferV6, sendIPv6);
        if (sendUriRes.has_error()) {
            std::cerr << "ERROR: " << sendUriRes.error().message() << std::endl;
            return -1;
        }
        sendUri = sendUriRes.value();
        auto distinct = codafb::checkDistinctLoadBalancers(recvUri, *sendUri);
        if (distinct.has_error()) {
            std::cerr << "ERROR: " << distinct.error().message() << std::endl;
            return -1;
        }
    }

    // Validate output directory if needed (reassembly-only mode)
#ifdef ENABLE_FRAME_BUILDER
    if (!enableFramebuild && !outputDir.empty())
#else
    if (!outputDir.empty())
#endif
    {
        // Validate output directory
        if (!bfs::exists(outputDir) || !bfs::is_directory(outputDir)) {
            std::cerr << "Output directory '" << outputDir << "' does not exist or is not a directory" << std::endl;
            return -1;
        }

        // Test write permissions
        struct stat dirStat;
        if (stat(outputDir.c_str(), &dirStat) != 0) {
            std::cerr << "Cannot access output directory: " << strerror(errno) << std::endl;
            return -1;
        }

        if (!(dirStat.st_mode & S_IWUSR)) {
            std::cerr << "No write permission for output directory" << std::endl;
            return -1;
        }

        // Ensure extension starts with '.' if not empty
        if (!fileExtension.empty() && fileExtension[0] != '.') {
            fileExtension.insert(fileExtension.begin(), '.');
        }
    }

#ifdef ENABLE_FRAME_BUILDER
    if (enableFramebuild) {
        // Validate thread count
        if (fbThreads < 1 || fbThreads > 32) {
            std::cerr << "Frame builder threads must be between 1 and 32" << std::endl;
            return -1;
        }

        bool hasETOutput = !etFile.empty();
        bool hasFileOutput = !fbOutputDir.empty();

        std::cout << "Frame builder: ENABLED" << std::endl;
        if (hasETOutput) {
            std::cout << "  ET output: " << etFile;
            if (!etHost.empty()) std::cout << " @ " << etHost;
            if (etPort > 0) std::cout << ":" << etPort;
            std::cout << " (GRAND_CENTRAL station)" << std::endl;
        }
        if (hasFileOutput) {
            std::cout << "  File output: " << fbOutputDir << "/" << fbOutputPrefix << "_*.evio" << std::endl;
        }
        if (forwarding) {
            std::cout << "  Forwarding: " << codafb::describeEjfatUri(*sendUri) << std::endl;
        }
        std::cout << "  Builder Threads: " << fbThreads << std::endl;
        std::cout << "  Expected Streams: " << expectedStreams << std::endl;
    } else {
        std::cout << "Frame builder: DISABLED (reassembly-only mode)" << std::endl;
    }
#else
    std::cout << "Frame builder: NOT COMPILED (reassembly-only mode)" << std::endl;
#endif

    // Set up signal handler
    signal(SIGINT, ctrlCHandler);

    std::cout << "CODA Frame Builder v" << CODA_FB_VERSION << std::endl;
    std::cout << "Using E2SAR library v" << get_Version() << std::endl;
    std::cout << "=========================================" << std::endl;

    // Handle NUMA binding if requested
#ifdef NUMA_AVAILABLE
    if (numaNode >= 0) {
        auto numaRes = Affinity::setNUMABind(numaNode);
        if (numaRes.has_error()) {
            std::cerr << "Unable to bind to NUMA node " << numaNode << ": " << numaRes.error().message() << std::endl;
            return -1;
        }
        std::cout << "NUMA binding: node " << numaNode << std::endl;
    }
#else
    if (numaNode >= 0) {
        std::cerr << "NUMA support not available in this build" << std::endl;
        return -1;
    }
#endif

    pipelineStats.health.senderEnabled = forwarding;

    // Metrics first: a port conflict fails before anything is registered, and
    // readiness is visible (as 0) while startup is still in progress.
    if (metricsPort != 0) {
#ifdef ENABLE_FRAME_BUILDER
        const bool building = enableFramebuild;
#else
        const bool building = false;
#endif
        metricsEndpointPtr = new codafb::MetricsEndpoint([building, forwarding] {
            return renderMetrics(building, forwarding);
        });
        std::string err = metricsEndpointPtr->start(metricsBind, metricsPort);
        if (!err.empty()) {
            std::cerr << "ERROR: " << err << " (--metrics-port/--metrics-bind)" << std::endl;
            delete metricsEndpointPtr;
            metricsEndpointPtr = nullptr;
            return -1;
        }
        std::cout << "Prometheus metrics: http://" << metricsBind << ":" << metricsEndpointPtr->port()
                  << "/metrics" << std::endl;
    }

    std::thread statsThread;

    // Startup order is the reverse of the data flow: the sender to LB #2 and
    // the frame builder are ready before this worker registers on LB #1, so no
    // data arrives before it can be handled, and a failure on the send side
    // leaves nothing registered anywhere. Every failure path undoes what was
    // already set up (cleanupAfterFailure()).
    try {
#ifdef ENABLE_FRAME_BUILDER
        // ---- 1. Sender to LB #2 -------------------------------------------------------
        if (forwarding) {
            std::cout << "\nInitializing frame forwarding to the send LB: "
                      << codafb::describeEjfatUri(*sendUri) << std::endl;

            u_int32_t srcId = sendSrcId;
            if (srcId == 0) {
                auto hostname = NetUtil::getHostName();
                srcId = fnv1a32(hostname.has_error() ? std::string("coda-fb") : hostname.value());
                if (srcId == 0) srcId = 1;
            }

            codafb::E2SARFrameTransport::Config tcfg;
            tcfg.dataId = sendDataId;
            tcfg.eventSrcId = srcId;
            tcfg.sflags.useCP = !sendNoCP;
            tcfg.sflags.mtu = sendMtu;
            tcfg.sflags.numSendSockets = sendSockets;
            tcfg.sflags.sndSocketBufSize = sendBufSize;
            tcfg.sflags.rateGbps = sendRateGbps;
            tcfg.sflags.dpV6 = sendIPv6;
            tcfg.sflags.lbHdrVersion = static_cast<u_int8_t>(sendLbHdrVersion);
            tcfg.sflags.warmUpMs = sendWarmUpMs;
            // FrameSender bounds what is in flight; E2SAR's own queue only has to hold that much.
            tcfg.sflags.eventQueueSize = std::max<size_t>(tcfg.sflags.eventQueueSize, sendQueueFrames + 1);
            tcfg.senderAddresses = sendSenderIPs;
            tcfg.validateCert = validate;
            tcfg.useHostAddress = preferV6;

            auto transport = codafb::E2SARFrameTransport::create(*sendUri, tcfg);
            if (transport.has_error()) {
                std::cerr << "\nERROR: unable to initialize forwarding to the send LB: "
                          << transport.error().message() << std::endl;
                cleanupAfterFailure();
                return -1;
            }
            sendTransportPtr = transport.value().release();

            codafb::FrameSenderConfig scfg;
            scfg.maxInFlightFrames = sendQueueFrames;
            scfg.maxInFlightBytes = sendQueueBytes;
            scfg.overflow = sendOverflow;
            scfg.dropAfter = std::chrono::milliseconds(sendBlockTimeoutMs);
            scfg.eventNumOffset = sendEventOffset;
            frameSenderPtr = new codafb::FrameSender(*sendTransportPtr, scfg, pipelineStats.send);
            pipelineStats.health.senderReady = true;

            std::cout << "Forwarding ready: data id " << sendDataId
                      << ", source id 0x" << std::hex << srcId << std::dec
                      << ", outbound event number = frame number + " << sendEventOffset << std::endl;
            std::cout << "  MTU " << sendTransportPtr->mtu() << " (" << sendTransportPtr->maxPayloadPerPacket()
                      << " payload bytes/packet)";
            if (!sendTransportPtr->interface().empty())
                std::cout << " via " << sendTransportPtr->interface();
            std::cout << ", " << sendSockets << " send sockets"
                      << ", rate " << (sendRateGbps > 0 ? std::to_string(sendRateGbps) + " Gbps" : std::string("unlimited"))
                      << std::endl;
            std::cout << "  In flight at most " << sendQueueFrames << " frames / "
                      << fmtBytes(static_cast<double>(sendQueueBytes)) << ", overflow policy '"
                      << codafb::toString(sendOverflow) << "'"
                      << (sendOverflow == codafb::OverflowPolicy::Drop
                          ? " after " + std::to_string(sendBlockTimeoutMs) + " ms" : std::string())
                      << ", " << (sendPartial ? "complete and partial" : "complete") << " frames forwarded"
                      << std::endl;
            if (sendNoCP)
                std::cout << "  WARNING: --send-nocp: no sender registration and no sync packets" << std::endl;
        }

        // ---- 2. Frame builder (or the reassembly-only output file) --------------------
        if (enableFramebuild) {
            std::cout << "\nInitializing frame builder..." << std::endl;

            frameBuilderPtr = new e2sar::FrameBuilder(
                etFile,
                etHost,
                etPort,
                fbOutputDir,
                fbOutputPrefix,
                fbThreads,
                etEventSize,
                timestampSlop,
                frameTimeout,
                expectedStreams,  // Number of expected data streams for aggregation
                verboseFrameInfo  // Enable verbose logging
            );
            frameBuilderPtr->setStatistics(&pipelineStats.build);
            frameBuilderPtr->setFrameSender(frameSenderPtr, sendPartial);

            if (!frameBuilderPtr->start()) {
                std::cerr << "Failed to start frame builder" << std::endl;
                cleanupAfterFailure();
                return -1;
            }
            pipelineStats.health.builderReady = true;

            std::cout << "Frame builder started successfully\n" << std::endl;
        } else
#endif
        {
            // Reassembly-only mode - create direct output file
            std::ostringstream outputFileName;
            outputFileName << filePrefix << fileExtension;
            bfs::path outputFilePath{outputDir};
            outputFilePath /= outputFileName.str();

            mode_t fileMode{S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH};
            globalOutputFd = open(outputFilePath.c_str(), O_WRONLY|O_CREAT|O_TRUNC, fileMode);
            if (globalOutputFd < 0)
            {
                std::cerr << "Unable to create output file " << outputFilePath << ": " << strerror(errno) << std::endl;
                cleanupAfterFailure();
                return -1;
            }

            std::cout << "Reassembly-only mode: Writing all frames to: " << outputFilePath << "\n" << std::endl;
        }

        // ---- 3. Receiver on LB #1 ---------------------------------------------------------
        std::cout << "Receive LB: " << codafb::describeEjfatUri(recvUri) << std::endl;

        // Configure reassembler
        Reassembler::ReassemblerFlags rflags;
        rflags.useCP = withCP;
        // E2SAR >=0.4.0 removed ReassemblerFlags::withLBHeader and derives the
        // equivalent internally as (not rflags.useCP), which is exactly what we
        // used to set here, so the behaviour is unchanged.
        rflags.rcvSocketBufSize = sockBufSize;
        rflags.useHostAddress = preferV6;
        // Only set validateCert when actually using control plane (SSL/TLS context)
        if (withCP) {
            rflags.validateCert = validate;
        }
        rflags.eventTimeout_ms = eventTimeoutMS;

        std::cout << "Control plane: " << (rflags.useCP ? "enabled" : "disabled") << std::endl;
        if (withCP) {
            std::cout << "SSL certificate validation: " << (validate ? "enabled" : "disabled") << std::endl;
        }
        std::cout << "Event timeout: " << rflags.eventTimeout_ms << " ms" << std::endl;
        std::cout << "Socket buffer size: " << sockBufSize << " bytes" << std::endl;
        std::cout << "Output directory: " << outputDir << std::endl;

        // Get IP address - either from command line or auto-detect local host IP
        boost::asio::ip::address data_ip;
        if (autoIP) {
            // Auto-detect local host IP address
            std::string localIP = getLocalHostIP(preferV6);
            if (localIP.empty()) {
                std::cerr << "Failed to auto-detect local host IP address" << std::endl;
                cleanupAfterFailure();
                return -1;
            }
            std::cout << "Auto-detected local host IP: " << localIP << std::endl;
            data_ip = boost::asio::ip::make_address(localIP);
        } else {
            data_ip = boost::asio::ip::make_address(recvIP);
        }

        // Create reassembler with multithreaded UDP reception
        if (coreList.size() > 0) {
            reasPtr = new Reassembler(recvUri, data_ip, recvStartPort, coreList, rflags);
            std::cout << "Reassembly threads: " << coreList.size() << " (CPU-pinned)" << std::endl;
            std::cout << "  CPU cores: ";
            for (size_t i = 0; i < coreList.size(); ++i) {
                std::cout << coreList[i];
                if (i < coreList.size() - 1) std::cout << ", ";
            }
            std::cout << std::endl;
        } else {
            reasPtr = new Reassembler(recvUri, data_ip, recvStartPort, numThreads, rflags);
            std::cout << "Reassembly threads: " << numThreads << " (parallel UDP receivers)" << std::endl;
        }

        std::cout << "Listening on: " << data_ip << ":"
                  << recvStartPort << "-" << (recvStartPort + (coreList.size() > 0 ? coreList.size() : numThreads) - 1) << std::endl;

        // Register and start receiving
        auto prepareResult = prepareToReceive(*reasPtr);
        if (prepareResult.has_error()) {
            std::cerr << "Failed to prepare receiver: "
                      << codafb::redactCredentials(prepareResult.error().message()) << std::endl;
            cleanupAfterFailure();
            return -1;
        }
        pipelineStats.health.receiverReady = true;
        // all E2SAR objects exist now; publish them to the metrics renderer
        e2sarStatsReady.store(true, std::memory_order_release);

        std::cout << "Receiver started successfully. Press Ctrl+C to stop." << std::endl;

        // Start statistics reporting thread
        statsThread = std::thread(statsReportingThread);

        // Start frame reception and writing
        auto result = receiveAndWriteFrames(reasPtr, globalOutputFd, frameBuilderPtr);

        if (result.has_error()) {
            std::cerr << "Error in frame reception: " << result.error().message() << std::endl;
        }

        // The reporting thread reads E2SAR objects that shutdown tears down
        stopStatsReporting(statsThread);
        shutdownPipeline(reasPtr, frameBuilderPtr, sendDrainTimeoutMs);

        // Normal cleanup
        performFinalCleanup();

    } catch (const E2SARException &e) {
        std::cerr << "E2SAR error: " << codafb::redactCredentials(static_cast<std::string>(e)) << std::endl;
        stopStatsReporting(statsThread);
        cleanupAfterFailure();
        return -1;
    } catch (const std::exception &e) {
        std::cerr << "Standard exception: " << codafb::redactCredentials(e.what()) << std::endl;
        stopStatsReporting(statsThread);
        cleanupAfterFailure();
        return -1;
    }

    return 0;
}
