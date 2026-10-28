/**
 * End-to-end forwarding test of the real coda-fb binary, on loopback, without
 * any EJFAT control plane:
 *
 *   upstream E2SAR Segmenter --UDP--> coda-fb (--recv-ejfat-uri, --withcp=0)
 *        2 ROC streams                  builds EVIO-6 frames, writes them to a
 *                                       file AND forwards them (--send-ejfat-uri,
 *                                       --send-nocp)
 *   downstream E2SAR Reassembler <--UDP--'
 *
 * Every frame reassembled downstream is compared byte-for-byte with the
 * record coda-fb wrote to its .evio file for the same frame number. The test
 * also checks the event number / data id mapping, that each frame arrives
 * exactly once, that frames that never completed are not sent, the final
 * statistics, the shutdown exit status, and that no token reaches the log.
 *
 * Usage: e2e_forwarding_driver <coda-fb> <work dir> <scenario>
 *   scenario "incomplete":   frame K+1 lacks a stream and never times out
 *                            before shutdown -> discarded, not sent
 *   scenario "partial-skip": frame K+1 lacks a stream, times out as a partial
 *                            frame -> written to file, not forwarded
 *                            (--send-partial-frames=0)
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <e2sar.hpp>

using namespace e2sar;
using namespace std::chrono_literals;

namespace {

constexpr uint16_t kCodaFbPort = 21000;      // coda-fb receive port
constexpr uint16_t kDownstreamPort = 22000;  // "second LB" data address
constexpr int kFrames = 40;                  // complete frames sent
constexpr uint64_t kEventOffset = 1000;      // --send-event-offset
constexpr uint16_t kSendDataId = 77;         // --send-data-id
constexpr int kSocketBuf = 200000;           // below the default net.core.[rw]mem_max
constexpr uint16_t kMetricsPort = 23000;     // --metrics-port

int failures = 0;
#define EXPECT(cond, what)                                                         \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::cerr << "FAIL: " << what << "  [" #cond "]" << std::endl;        \
            failures++;                                                            \
        } else {                                                                   \
            std::cout << "ok:   " << what << std::endl;                            \
        }                                                                          \
    } while (0)

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool waitForLog(const std::string& log, const std::string& needle, std::chrono::seconds timeout) {
    auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (readFile(log).find(needle) != std::string::npos) return true;
        std::this_thread::sleep_for(100ms);
    }
    return false;
}

/** A ROC time slice payload that passes coda-fb's EVIO validation. */
std::vector<uint8_t> makeSlice(uint32_t frame, uint16_t roc, std::mt19937& rng) {
    size_t words = 16 + (frame * 13 + roc * 7) % 700;  // 64 B .. ~2.8 KB per slice
    std::vector<uint32_t> w(words);
    for (auto& x : w) x = rng();
    w[7] = 0xc0da0100;                               // word 8: magic, host order
    w[9] = (uint32_t(roc) << 16) | (0x10 << 8) | 1;  // word 10: ROC id | 0x10 | status
    w[13] = frame;                                   // word 14: frame number
    w[14] = 0x1000 + frame;                          // word 15/16: timestamp
    w[15] = 0;
    std::vector<uint8_t> b(words * 4);
    std::memcpy(b.data(), w.data(), b.size());
    return b;
}

/** GET /metrics from coda-fb; returns the whole HTTP response. */
std::string scrape() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kMetricsPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string out;
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        const std::string req = "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n";
        (void)!write(fd, req.data(), req.size());
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return out;
}

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

/** frame number -> complete EVIO-6 record bytes, from coda-fb's output file. */
std::map<uint32_t, std::vector<uint8_t>> readRecords(const std::string& path) {
    std::map<uint32_t, std::vector<uint8_t>> out;
    std::string f = readFile(path);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(f.data());
    size_t off = 56;  // EVIO-6 file header
    while (off + 80 <= f.size()) {
        size_t bytes = size_t(be32(p + off)) * 4;
        if (bytes == 0 || off + bytes > f.size()) break;
        uint32_t frameNumber = be32(p + off + 19 * 4);  // first word of the time slice segment
        out[frameNumber] = std::vector<uint8_t>(p + off, p + off + bytes);
        off += bytes;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: " << argv[0] << " <coda-fb> <work dir> incomplete|partial-skip" << std::endl;
        return 2;
    }
    const std::string codaFb = argv[1];
    const std::string work = argv[2];
    const std::string scenario = argv[3];
    const bool partialSkip = (scenario == "partial-skip");
    if (!partialSkip && scenario != "incomplete") {
        std::cerr << "unknown scenario " << scenario << std::endl;
        return 2;
    }
    const std::string frameDir = work + "/frames";
    const std::string log = work + "/coda-fb.log";
    if (system(("rm -rf '" + work + "' && mkdir -p '" + frameDir + "'").c_str()) != 0) return 2;

    // ---- downstream: stands in for the workers behind the second LB ----------------
    EjfatURI downUri("ejfat://unused@127.0.0.1:9876/lb/99?data=127.0.0.1", EjfatURI::TokenType::instance);
    Reassembler::ReassemblerFlags rflags;
    rflags.useCP = false;
    rflags.rcvSocketBufSize = kSocketBuf;
    Reassembler downstream(downUri, ip::make_address("127.0.0.1"), kDownstreamPort, 1, rflags);
    if (downstream.openAndStart().has_error()) {
        std::cerr << "cannot start downstream reassembler" << std::endl;
        return 2;
    }

    // ---- coda-fb under test, with two distinct URIs ---------------------------------
    std::vector<std::string> args = {
        codaFb,
        "--recv-ejfat-uri", "ejfat://RECVTOKENxyz@127.0.0.1:18347/lb/1?data=127.0.0.1",
        "--withcp=0", "--ip", "127.0.0.1", "--port", std::to_string(kCodaFbPort), "--threads", "1",
        "--bufsize", std::to_string(kSocketBuf),
        "--enable-framebuild=1", "--expected-streams", "2", "--fb-threads", "1",
        "--frame-timeout", partialSkip ? "300" : "60000",
        "--fb-output-dir", frameDir,
        "--send-ejfat-uri", "ejfat://SENDTOKENxyz@127.0.0.2:18347/lb/2?data=127.0.0.1:" +
                            std::to_string(kDownstreamPort) + "&sync=127.0.0.1:19999",
        "--send-nocp", "--send-warmup", "0", "--send-bufsize", std::to_string(kSocketBuf),
        "--send-data-id", std::to_string(kSendDataId),
        "--send-event-offset", std::to_string(kEventOffset),
        "--send-queue-frames", "4",
        "--send-partial-frames", partialSkip ? "0" : "1",
        "--report-interval", "1000",
        "--metrics-port", std::to_string(kMetricsPort),
    };
    pid_t child = fork();
    if (child == 0) {
        int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        std::vector<char*> cargs;
        for (auto& a : args) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);
        execv(cargs[0], cargs.data());
        _exit(127);
    }
    if (!waitForLog(log, "Receiver started successfully", 60s)) {
        std::cerr << readFile(log) << "\ncoda-fb did not start" << std::endl;
        kill(child, SIGKILL);
        return 1;
    }

    // ---- upstream: two ROC streams into coda-fb -------------------------------------
    EjfatURI upUri("ejfat://unused@127.0.0.1:9876/lb/5?data=127.0.0.1:" + std::to_string(kCodaFbPort) +
                   "&sync=127.0.0.1:19998", EjfatURI::TokenType::instance);
    Segmenter::SegmenterFlags sflags;
    sflags.useCP = false;
    sflags.warmUpMs = 0;
    sflags.sndSocketBufSize = kSocketBuf;
    sflags.numSendSockets = 1;
    Segmenter upstream(upUri, 1, 0x1234, sflags);
    if (upstream.openAndStart().has_error()) {
        std::cerr << "cannot start upstream segmenter" << std::endl;
        kill(child, SIGKILL);
        return 2;
    }
    std::mt19937 rng(12345);
    std::vector<std::vector<uint8_t>> keepAlive;  // segmenter reads these asynchronously
    auto sendSlice = [&](uint32_t frame, uint16_t roc) {
        keepAlive.push_back(makeSlice(frame, roc, rng));
        auto& b = keepAlive.back();
        while (upstream.addToSendQueue(b.data(), b.size(), frame, roc).has_error())
            std::this_thread::sleep_for(1ms);
    };
    keepAlive.reserve(2 * kFrames + 2);
    for (uint32_t frame = 1; frame <= kFrames; frame++) {
        sendSlice(frame, 1);
        sendSlice(frame, 2);
        std::this_thread::sleep_for(5ms);  // stay well inside the small socket buffers
    }
    sendSlice(kFrames + 1, 1);  // stream 2 never sends this frame

    // ---- collect what reaches the "second LB" ---------------------------------------
    struct Got { uint64_t eventNum; uint16_t dataId; std::vector<uint8_t> bytes; };
    std::vector<Got> got;
    auto deadline = std::chrono::steady_clock::now() + 30s;
    while ((int)got.size() < kFrames && std::chrono::steady_clock::now() < deadline) {
        uint8_t* buf{nullptr};
        size_t len{0};
        EventNum_t evn{0};
        uint16_t did{0};
        auto r = downstream.recvEvent(&buf, &len, &evn, &did, 500);
        if (!r.has_error() && r.value() == 0) {
            got.push_back({evn, did, std::vector<uint8_t>(buf, buf + len)});
            delete[] buf;
        }
    }
    // anything extra (a duplicate, or the incomplete/partial frame) would arrive by now
    auto extraUntil = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < extraUntil) {
        uint8_t* buf{nullptr};
        size_t len{0};
        EventNum_t evn{0};
        uint16_t did{0};
        auto r = downstream.recvEvent(&buf, &len, &evn, &did, 200);
        if (!r.has_error() && r.value() == 0) {
            got.push_back({evn, did, std::vector<uint8_t>(buf, buf + len)});
            delete[] buf;
        }
    }

    // ---- scrape the live process; must agree with what the downstream received -----------
    const std::string metrics = scrape();
    { std::ofstream(work + "/metrics.txt") << metrics.substr(std::min(metrics.size(), metrics.find("\r\n\r\n") + 4)); }

    // ---- shut coda-fb down the way docker stop / Ctrl+C does --------------------------
    kill(child, SIGINT);
    int status = 0;
    bool exited = false;
    for (int i = 0; i < 300 && !exited; i++) {
        exited = waitpid(child, &status, WNOHANG) == child;
        if (!exited) std::this_thread::sleep_for(100ms);
    }
    if (!exited) kill(child, SIGKILL);
    upstream.stopThreads();
    downstream.stopThreads();
    const std::string out = readFile(log);

    // ---- checks -------------------------------------------------------------------------
    EXPECT(exited && WIFEXITED(status) && WEXITSTATUS(status) == 0, "coda-fb exits 0 within 30 s of SIGINT");
    EXPECT((int)got.size() == kFrames, "exactly " << kFrames << " frames reach the second LB (got " << got.size() << ")");

    std::set<uint64_t> seen;
    bool idsOk = true, mappingOk = true;
    for (auto& g : got) {
        seen.insert(g.eventNum);
        idsOk = idsOk && g.dataId == kSendDataId;
        mappingOk = mappingOk && g.eventNum >= kEventOffset + 1 && g.eventNum <= kEventOffset + kFrames;
    }
    EXPECT(seen.size() == got.size(), "no frame arrives twice");
    EXPECT(idsOk, "every frame carries --send-data-id " << kSendDataId);
    EXPECT(mappingOk, "event number = frame number + --send-event-offset for frames 1.." << kFrames);

    auto records = readRecords(frameDir + "/frames_thread0_file0000.evio");
    EXPECT((int)records.size() == kFrames + (partialSkip ? 1 : 0),
           "file output has " << kFrames + (partialSkip ? 1 : 0) << " records (got " << records.size() << ")");
    int identical = 0;
    for (auto& g : got) {
        auto it = records.find(static_cast<uint32_t>(g.eventNum - kEventOffset));
        if (it != records.end() && it->second == g.bytes) identical++;
    }
    EXPECT(identical == kFrames, "every forwarded frame is byte-for-byte identical to the built frame ("
                                 << identical << "/" << kFrames << ")");
    size_t largest = 0;
    for (auto& r : records) largest = std::max(largest, r.second.size());
    EXPECT(largest > 2 * 1500, "frames span at least 3 packets (largest " << largest << " B at MTU 1500)");

    auto has = [&](const std::string& s) { return out.find(s) != std::string::npos; };
    EXPECT(has("=== Final Statistics ==="), "final statistics printed");
    EXPECT(has("Submitted: " + std::to_string(kFrames) + " "), "final stats: Submitted " << kFrames);
    EXPECT(has("Released by E2SAR: " + std::to_string(kFrames)), "final stats: Released by E2SAR " << kFrames);
    EXPECT(has("Abandoned at shutdown: 0"), "final stats: nothing abandoned");
    EXPECT(has("Dropped: overflow 0, shutdown 0"), "final stats: nothing dropped");
    EXPECT(has("All forwarded frames were released by E2SAR"), "shutdown drained the sender");
    if (partialSkip) {
        EXPECT(has("Partial not forwarded: 1"), "partial frame counted as not forwarded");
        EXPECT(has("partial 1,"), "partial frame built for file output");
    } else {
        EXPECT(has("discarded at shutdown 1 (1 slices)"), "incomplete frame counted as discarded at shutdown");
    }
    EXPECT(!has("RECVTOKENxyz") && !has("SENDTOKENxyz"), "no token in coda-fb output");
    EXPECT(has("downstream delivery is NOT measured"), "report distinguishes submission from delivery");

    auto metric = [&](const std::string& line) { return metrics.find("\n" + line + "\n") != std::string::npos; };
    const std::string n = std::to_string(kFrames);
    EXPECT(metrics.rfind("HTTP/1.1 200", 0) == 0, "/metrics answers 200 on the live process");
    EXPECT(metrics.find("Content-Type: text/plain; version=0.0.4") != std::string::npos, "/metrics content type");
    EXPECT(metric("coda_fb_send_frames_total{outcome=\"submitted\"} " + n), "metrics: submitted " << n);
    EXPECT(metric("coda_fb_send_frames_released_total " + n), "metrics: released " << n);
    EXPECT(metric("coda_fb_send_frames_offered_total " + n), "metrics: offered " << n);
    EXPECT(metric("coda_fb_build_frames_total{outcome=\"complete\"} " + n), "metrics: complete frames " << n);
    EXPECT(metric("coda_fb_recv_events_total " + std::to_string(2 * kFrames + 1)), "metrics: events received");
    EXPECT(metric("coda_fb_ready{stage=\"receiver\"} 1") && metric("coda_fb_ready{stage=\"builder\"} 1") &&
           metric("coda_fb_ready{stage=\"sender\"} 1"), "metrics: all stages ready while running");
    EXPECT(metrics.find("\ncoda_fb_recv_udp_packets_total ") != std::string::npos &&
           metrics.find("\ncoda_fb_send_packets_total ") != std::string::npos, "metrics: E2SAR families present");
    EXPECT(metrics.find("RECVTOKENxyz") == std::string::npos && metrics.find("SENDTOKENxyz") == std::string::npos &&
           metrics.find("ejfat") == std::string::npos, "metrics: no token or URI");
    if (partialSkip)
        EXPECT(metric("coda_fb_send_partial_frames_skipped_total 1"), "metrics: partial frame skipped");
    else
        EXPECT(metric("coda_fb_build_frames_in_progress 1"), "metrics: incomplete frame in progress");
    EXPECT(has("Metrics endpoint stopped after "), "metrics endpoint stopped at shutdown");

    if (failures) std::cerr << "\n--- coda-fb log ---\n" << out << std::endl;
    std::cout << (failures ? "FAILED" : "PASSED") << " (" << scenario << ")" << std::endl;
    return failures ? 1 : 0;
}
