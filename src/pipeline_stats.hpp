/**
 * Pipeline statistics for coda-fb
 *
 * Lock-free counters, gauges, event timestamps and fixed-bucket histograms,
 * plus the statistics coda-fb itself measures at each pipeline stage:
 *
 *   EJFAT LB #1 -> E2SAR Reassembler -> reception loop -> FrameBuilder
 *               -> FrameSender -> E2SAR Segmenter -> EJFAT LB #2
 *
 * Statistics that E2SAR measures internally (UDP packets/bytes received,
 * reassembly losses, packets sent, socket errors) are NOT duplicated here.
 * They are polled from Reassembler::getStats() and Segmenter::getSendStats()
 * at report time, see coda-fb.cpp.
 *
 * CONCURRENCY:
 * - Every primitive is a std::atomic updated with memory_order_relaxed.
 *   Updates never block and cost one atomic RMW each.
 * - Each value is individually consistent. A report reads values one at a
 *   time, so two related values (e.g. submitted vs. released) may be up to
 *   one in-flight update apart in a single snapshot.
 * - All values are cumulative since process start. coda-fb has no run
 *   transition inside a process: a new run is a new process, and every
 *   counter restarts at zero.
 *
 * UNITS: bytes are bytes, durations are nanoseconds internally and are
 * reported in seconds. Nothing here is an estimate unless its name says so.
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#ifndef CODA_FB_PIPELINE_STATS_HPP
#define CODA_FB_PIPELINE_STATS_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace codafb {

/** Monotonic clock in nanoseconds; never 0 on a running system. */
inline int64_t monotonicNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** Monotonically increasing count. */
class Counter {
public:
    void add(uint64_t n = 1) noexcept { v_.fetch_add(n, std::memory_order_relaxed); }
    uint64_t value() const noexcept { return v_.load(std::memory_order_relaxed); }
private:
    std::atomic<uint64_t> v_{0};
};

/** Instantaneous value that can go up and down. */
class Gauge {
public:
    void set(int64_t v) noexcept { v_.store(v, std::memory_order_relaxed); }
    void add(int64_t n) noexcept { v_.fetch_add(n, std::memory_order_relaxed); }
    int64_t value() const noexcept { return v_.load(std::memory_order_relaxed); }
private:
    std::atomic<int64_t> v_{0};
};

/** Time of the most recent occurrence of something, on the monotonic clock. */
class EventTime {
public:
    void mark() noexcept { markAt(monotonicNowNs()); }
    void markAt(int64_t ns) noexcept { ns_.store(ns, std::memory_order_relaxed); }
    bool seen() const noexcept { return ns_.load(std::memory_order_relaxed) != 0; }
    /** Seconds since the last mark, or -1 if it never happened. */
    double ageSeconds(int64_t nowNs = monotonicNowNs()) const noexcept {
        int64_t t = ns_.load(std::memory_order_relaxed);
        return t == 0 ? -1.0 : static_cast<double>(nowNs - t) / 1e9;
    }
private:
    std::atomic<int64_t> ns_{0};
};

/**
 * Fixed-bucket histogram with Prometheus "le" semantics: bucket i counts
 * observations <= bounds[i]; one extra overflow bucket counts the rest.
 * Bounds are integers in the base unit (bytes or nanoseconds) and must be
 * strictly increasing. observe() is a binary search plus two relaxed RMWs.
 */
class Histogram {
public:
    explicit Histogram(std::vector<uint64_t> upperBounds)
        : bounds_(std::move(upperBounds))
        , buckets_(new std::atomic<uint64_t>[bounds_.size() + 1]) {
        for (size_t i = 0; i <= bounds_.size(); i++) buckets_[i].store(0, std::memory_order_relaxed);
    }

    void observe(uint64_t v) noexcept {
        size_t idx = std::lower_bound(bounds_.begin(), bounds_.end(), v) - bounds_.begin();
        buckets_[idx].fetch_add(1, std::memory_order_relaxed);
        sum_.fetch_add(v, std::memory_order_relaxed);
    }

    struct Snapshot {
        std::vector<uint64_t> bounds;      // upper bounds, base units
        std::vector<uint64_t> cumulative;  // size bounds+1; last entry (+Inf) == count
        uint64_t count{0};
        uint64_t sum{0};

        /**
         * ESTIMATED quantile by linear interpolation inside the bucket that
         * contains it (the same method as PromQL histogram_quantile). Values
         * in the overflow bucket are reported as the highest finite bound.
         * Returns -1 when there are no observations.
         */
        double quantile(double q) const {
            if (count == 0) return -1.0;
            double rank = q * static_cast<double>(count);
            uint64_t prevCum = 0;
            for (size_t i = 0; i < cumulative.size(); i++) {
                if (static_cast<double>(cumulative[i]) >= rank) {
                    if (i == bounds.size()) return static_cast<double>(bounds.empty() ? 0 : bounds.back());
                    double lo = (i == 0) ? 0.0 : static_cast<double>(bounds[i - 1]);
                    double hi = static_cast<double>(bounds[i]);
                    uint64_t inBucket = cumulative[i] - prevCum;
                    if (inBucket == 0) return hi;
                    return lo + (hi - lo) * (rank - static_cast<double>(prevCum)) / static_cast<double>(inBucket);
                }
                prevCum = cumulative[i];
            }
            return static_cast<double>(bounds.empty() ? 0 : bounds.back());
        }
    };

    /** count is derived from the buckets so that +Inf == count always holds. */
    Snapshot snapshot() const {
        Snapshot s;
        s.bounds = bounds_;
        s.cumulative.resize(bounds_.size() + 1);
        uint64_t running = 0;
        for (size_t i = 0; i <= bounds_.size(); i++) {
            running += buckets_[i].load(std::memory_order_relaxed);
            s.cumulative[i] = running;
        }
        s.count = running;
        s.sum = sum_.load(std::memory_order_relaxed);
        return s;
    }

    Histogram(const Histogram&) = delete;
    Histogram& operator=(const Histogram&) = delete;

private:
    const std::vector<uint64_t> bounds_;
    std::unique_ptr<std::atomic<uint64_t>[]> buckets_;
    std::atomic<uint64_t> sum_{0};
};

namespace buckets {
/** 1 KiB .. 64 MiB in powers of 4 (ET events default to 2 MiB). */
inline std::vector<uint64_t> frameBytes() {
    return {1ULL << 10, 4ULL << 10, 16ULL << 10, 64ULL << 10, 256ULL << 10,
            1ULL << 20, 2ULL << 20, 4ULL << 20, 8ULL << 20, 16ULL << 20, 64ULL << 20};
}
/** 10 us .. 10 s; covers serialization (us) up to the frame timeout (s). */
inline std::vector<uint64_t> durationNs() {
    return {10'000ULL, 50'000ULL, 100'000ULL, 500'000ULL,
            1'000'000ULL, 5'000'000ULL, 10'000'000ULL, 50'000'000ULL,
            100'000'000ULL, 500'000'000ULL, 1'000'000'000ULL,
            2'500'000'000ULL, 5'000'000'000ULL, 10'000'000'000ULL};
}
}  // namespace buckets

/**
 * Reception loop: events coda-fb took off the E2SAR reassembler queue.
 * Boundary: Reassembler::recvEvent() returning, before payload validation.
 */
struct ReceiveLoopStats {
    Counter eventsReceived;           // events returned by recvEvent()
    Counter bytesReceived;            // payload bytes of those events
    Counter recvErrors;               // recvEvent() returned an error
    Counter payloadValidationErrors;  // events dropped: EVIO payload failed validation
    Counter wrongEndianness;          // events whose CODA magic word was byte-swapped
    EventTime lastEvent;              // last successful recvEvent()
};

/**
 * Frame building. Boundaries:
 * - slicesIn:  FrameBuilder::addTimeSlice() accepted a validated event.
 * - frames*:   a builder thread finalized the set of slices for one corrected
 *              event number. Exactly one of framesComplete, framesPartial,
 *              framesBuildFailed or framesDiscardedShutdown is incremented per
 *              finalized frame.
 * - Complete = slices from all --expected-streams. Partial = fewer (timeout
 *              or stream misalignment); these are still output, as before.
 */
struct FrameBuildStats {
    Counter slicesIn;
    Counter framesComplete;           // serialized, all expected streams present
    Counter framesPartial;            // serialized, some streams missing
    Counter framesBuildFailed;        // not serialized: no valid slice or frame-number inconsistency
    Counter framesDiscardedShutdown;  // slices aggregated or pending at shutdown, never serialized
    Counter slicesDiscardedShutdown;  // slices in the above
    Counter bytesSerialized;          // EVIO-6 bytes of serialized frames
    Counter etOutputErrors;           // ET event get/put failures for serialized frames
    Counter fileOutputErrors;         // file open/write failures for serialized frames
    Gauge slicesPending;              // slices queued in builder FIFOs right now
    Gauge framesInProgress;           // corrected event numbers with slices waiting right now
    Histogram frameBytes{buckets::frameBytes()};   // serialized frame size
    Histogram serializeNs{buckets::durationNs()};  // time inside buildEVIO6Frame()
    Histogram assemblyNs{buckets::durationNs()};   // first slice arrival -> frame finalized
    EventTime lastFrame;              // last serialized frame
};

/**
 * Forwarding to the second load balancer. Boundaries:
 * - offered:   a builder handed a serialized frame to FrameSender::submit().
 * - submitted: Segmenter::addToSendQueue() accepted it (LOCAL submission).
 * - released:  E2SAR's send callback fired: E2SAR finished its sendmsg()
 *              attempts for every packet of the frame and no longer
 *              references the buffer. This is NOT downstream delivery: UDP
 *              has no acknowledgement and E2SAR 0.4.0rc1 does not report
 *              per-frame send status, so packet-level failures appear only in
 *              E2SAR's send errCnt.
 * Every offered frame ends as exactly one of submitted, droppedOverflow,
 * droppedShutdown or rejected.
 */
struct FrameSendStats {
    Counter framesOffered;
    Counter framesSubmitted;
    Counter framesReleased;
    Counter framesDroppedOverflow;    // --send-overflow=drop and no space within --send-block-timeout
    Counter framesDroppedShutdown;    // no space before the shutdown drain deadline
    Counter framesRejected;           // permanent error: E2SAR rejected it, invalid size or event number
    Counter framesNotForwardedPartial;  // partial frames skipped by --send-partial-frames=0
    Counter submitRetries;            // E2SAR queue momentarily full; nothing was enqueued
    Counter bytesSubmitted;
    Counter bytesReleased;
    Counter packetsEstimated;         // ESTIMATE: sum of ceil(frame bytes / max payload per packet)
    Counter blockedNs;                // total time submitters waited for in-flight space
    Counter blockedSubmits;           // submits that had to wait at all
    Gauge inFlightFrames;             // submitted or waiting for E2SAR, not yet released
    Gauge inFlightBytes;
    Gauge capacityFrames;             // --send-queue-frames
    Gauge capacityBytes;              // --send-queue-bytes
    Gauge framesAbandonedShutdown;    // still in flight when the drain deadline expired
    Histogram queueWaitNs{buckets::durationNs()};       // offered -> submitted
    Histogram releaseLatencyNs{buckets::durationNs()};  // submitted -> released
    EventTime lastSubmitted;
    EventTime lastReleased;
};

/** Readiness flags flip to true once the stage is fully initialized and back to false on shutdown. */
struct HealthStats {
    const int64_t startNs{monotonicNowNs()};
    // Wall clock at start, captured once so that exporting it never jitters.
    const double startUnixSeconds{std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count()};
    std::atomic<bool> receiverReady{false};  // worker registered on LB #1 and receive threads running
    std::atomic<bool> builderReady{false};   // builder threads running
    std::atomic<bool> senderEnabled{false};  // --send-ejfat-uri was given
    std::atomic<bool> senderReady{false};    // sender registered on LB #2 and segmenter running

    double uptimeSeconds(int64_t nowNs = monotonicNowNs()) const noexcept {
        return static_cast<double>(nowNs - startNs) / 1e9;
    }
};

struct PipelineStats {
    ReceiveLoopStats recv;
    FrameBuildStats build;
    FrameSendStats send;
    HealthStats health;
};

}  // namespace codafb

#endif  // CODA_FB_PIPELINE_STATS_HPP
