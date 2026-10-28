/**
 * FrameSender - bounded, ownership-safe forwarding of built frames
 *
 * Builder threads call submit() with each serialized EVIO-6 frame as soon as
 * it is built. FrameSender hands the frame to a FrameTransport (in production
 * the E2SAR Segmenter's asynchronous send queue) as ONE logical event, and
 * the transport fragments it into packets.
 *
 * OWNERSHIP: the frame is a shared_ptr to an immutable buffer. FrameSender
 * holds one reference from submit() until the transport's completion
 * callback fires, so the bytes stay valid for as long as the transport reads
 * them, while the builder can still use its own reference for ET/file output.
 * No copy of the frame is made.
 *
 * BACKPRESSURE: frames are "in flight" from the moment they are admitted
 * until the transport releases them. Both the in-flight frame count and the
 * in-flight byte total are bounded. A single frame larger than the byte
 * bound is admitted only when nothing else is in flight, so it cannot wait
 * forever. When there is no space:
 *   - OverflowPolicy::Block: the builder thread waits (default). Pressure
 *     propagates upstream to the builder FIFOs and eventually to E2SAR's
 *     reassembly queue, where E2SAR counts enqueue losses.
 *   - OverflowPolicy::Drop: wait up to dropAfter, then drop the frame and
 *     count it in framesDroppedOverflow.
 * Frames are never dropped silently: each one is counted and logged
 * (rate-limited).
 *
 * RETRIES: the only retry is when the transport reports QueueFull, which
 * means the frame was NOT enqueued, so a retry cannot create a duplicate.
 * Retries are bounded by the same deadline as the wait above.
 *
 * EVENT NUMBERS: outbound E2SAR event number = eventNumOffset + frameNumber,
 * where frameNumber is the builder's corrected event number. E2SAR's
 * addToSendQueue() treats event number 0 as "use the next sequential
 * number", so 0 can never be sent explicitly and eventNumOffset must be >= 1.
 * Submissions are serialized because addToSendQueue() sets and reads its
 * event number counter in two separate steps.
 *
 * SHUTDOWN: beginShutdown(deadline) bounds every wait by the deadline, then
 * finishShutdown() refuses new frames and waits for in-flight frames to be
 * released. Frames still in flight at the deadline are reported as
 * abandoned; the FrameSender must then NOT be destroyed, because the
 * transport will still call back into it.
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#ifndef CODA_FB_FRAME_SENDER_HPP
#define CODA_FB_FRAME_SENDER_HPP

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pipeline_stats.hpp"

namespace codafb {

using FrameBuffer = std::shared_ptr<const std::vector<uint8_t>>;

/**
 * Something that can send one frame asynchronously.
 */
class FrameTransport {
public:
    enum class SubmitStatus {
        Accepted,   // transport owns a reference; done(ctx) will be called exactly once
        QueueFull,  // transient; nothing was enqueued, done(ctx) will NOT be called
        Failed      // permanent; nothing was enqueued, done(ctx) will NOT be called
    };
    using CompletionFn = void (*)(void* ctx);

    virtual ~FrameTransport() = default;

    /** Non-blocking. On Failed, 'error' describes why. */
    virtual SubmitStatus submit(const uint8_t* data, size_t bytes, uint64_t eventNum,
                                CompletionFn done, void* ctx, std::string& error) noexcept = 0;

    /** Payload bytes per packet, used only for the packet estimate. */
    virtual size_t maxPayloadPerPacket() const noexcept = 0;
};

enum class OverflowPolicy { Block, Drop };

const char* toString(OverflowPolicy p) noexcept;
bool parseOverflowPolicy(const std::string& s, OverflowPolicy& out) noexcept;

struct FrameSenderConfig {
    size_t maxInFlightFrames{256};
    size_t maxInFlightBytes{512ULL * 1024 * 1024};
    OverflowPolicy overflow{OverflowPolicy::Block};
    std::chrono::milliseconds dropAfter{1000};       // Drop policy only
    uint64_t eventNumOffset{1};
    std::chrono::microseconds retryBackoff{100};     // between QueueFull retries
};

class FrameSender {
public:
    enum class Outcome { Submitted, DroppedOverflow, DroppedShutdown, Rejected };

    FrameSender(FrameTransport& transport, const FrameSenderConfig& cfg, FrameSendStats& stats);
    ~FrameSender();

    FrameSender(const FrameSender&) = delete;
    FrameSender& operator=(const FrameSender&) = delete;

    /** The outbound E2SAR event number for a builder frame number. */
    static uint64_t eventNumberFor(uint32_t frameNumber, uint64_t offset) noexcept {
        return offset + frameNumber;
    }

    /**
     * Forward one built frame. Thread-safe; may block according to the
     * overflow policy. Called exactly once per built frame.
     */
    Outcome submit(FrameBuffer frame, uint32_t frameNumber);

    /** Record a partial frame that the builder did not forward (--send-partial-frames=0). */
    void countSkippedPartial() noexcept { stats_.framesNotForwardedPartial.add(); }

    /** Bound all further waiting by 'deadline' and wake blocked submitters. */
    void beginShutdown(std::chrono::steady_clock::time_point deadline);

    /**
     * Refuse new frames and wait until all in-flight frames are released or
     * the deadline set by beginShutdown() passes (immediately if it was never
     * called). Returns the number of frames still in flight (abandoned).
     */
    size_t finishShutdown();

    size_t inFlightFrames() const;
    size_t inFlightBytes() const;
    const FrameSenderConfig& config() const noexcept { return cfg_; }

private:
    struct InFlight {
        FrameSender* owner;
        FrameBuffer frame;
        int64_t submitNs;
    };

    static void onTransportDone(void* ctx);
    void release(InFlight* f);
    bool hasSpaceLocked(size_t bytes) const noexcept;
    void unreserve(size_t bytes);
    Outcome refuse(Outcome why, uint32_t frameNumber, const std::string& detail);

    FrameTransport& transport_;
    const FrameSenderConfig cfg_;
    FrameSendStats& stats_;

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    size_t inFlightFrames_{0};
    size_t inFlightBytes_{0};
    bool shuttingDown_{false};
    bool closed_{false};
    std::chrono::steady_clock::time_point shutdownDeadline_{};

    // serializes transport_.submit(), see EVENT NUMBERS above
    std::mutex submitMtx_;

    // rate limiting for drop/reject log messages
    std::mutex logMtx_;
    int64_t lastLogNs_{0};
    uint64_t suppressedLogs_{0};
};

}  // namespace codafb

#endif  // CODA_FB_FRAME_SENDER_HPP
