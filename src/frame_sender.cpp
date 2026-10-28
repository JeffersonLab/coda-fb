/**
 * FrameSender implementation - see frame_sender.hpp for the contract.
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#include "frame_sender.hpp"

#include <iostream>
#include <limits>
#include <thread>

namespace codafb {

const char* toString(OverflowPolicy p) noexcept {
    return p == OverflowPolicy::Block ? "block" : "drop";
}

bool parseOverflowPolicy(const std::string& s, OverflowPolicy& out) noexcept {
    if (s == "block") { out = OverflowPolicy::Block; return true; }
    if (s == "drop")  { out = OverflowPolicy::Drop;  return true; }
    return false;
}

FrameSender::FrameSender(FrameTransport& transport, const FrameSenderConfig& cfg, FrameSendStats& stats)
    : transport_(transport), cfg_(cfg), stats_(stats) {
    stats_.capacityFrames.set(static_cast<int64_t>(cfg_.maxInFlightFrames));
    stats_.capacityBytes.set(static_cast<int64_t>(cfg_.maxInFlightBytes));
}

FrameSender::~FrameSender() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (inFlightFrames_ != 0) {
        // The owner must leak an un-drained sender instead of destroying it,
        // since the transport still holds pointers into it.
        std::cerr << "[FrameSender] BUG: destroyed with " << inFlightFrames_
                  << " frames still in flight" << std::endl;
    }
}

bool FrameSender::hasSpaceLocked(size_t bytes) const noexcept {
    if (inFlightFrames_ >= cfg_.maxInFlightFrames) return false;
    // an oversized frame may go when nothing else is in flight
    return inFlightFrames_ == 0 || inFlightBytes_ + bytes <= cfg_.maxInFlightBytes;
}

void FrameSender::unreserve(size_t bytes) {
    std::lock_guard<std::mutex> lock(mtx_);
    inFlightFrames_--;
    inFlightBytes_ -= bytes;
    stats_.inFlightFrames.set(static_cast<int64_t>(inFlightFrames_));
    stats_.inFlightBytes.set(static_cast<int64_t>(inFlightBytes_));
    cv_.notify_all();
}

FrameSender::Outcome FrameSender::refuse(Outcome why, uint32_t frameNumber, const std::string& detail) {
    const char* what = "rejected";
    switch (why) {
        case Outcome::DroppedOverflow: stats_.framesDroppedOverflow.add(); what = "DROPPED (send queue full)"; break;
        case Outcome::DroppedShutdown: stats_.framesDroppedShutdown.add(); what = "DROPPED (shutdown drain deadline)"; break;
        case Outcome::Rejected:        stats_.framesRejected.add(); what = "REJECTED"; break;
        case Outcome::Submitted:       break;
    }

    // At most one message per second, with a count of what was suppressed.
    const int64_t now = monotonicNowNs();
    std::lock_guard<std::mutex> lock(logMtx_);
    if (now - lastLogNs_ >= 1'000'000'000LL) {
        std::cerr << "[FrameSender] frame " << frameNumber << " " << what;
        if (!detail.empty()) std::cerr << ": " << detail;
        if (suppressedLogs_ > 0) std::cerr << " (" << suppressedLogs_ << " similar messages suppressed)";
        std::cerr << std::endl;
        lastLogNs_ = now;
        suppressedLogs_ = 0;
    } else {
        suppressedLogs_++;
    }
    return why;
}

FrameSender::Outcome FrameSender::submit(FrameBuffer frame, uint32_t frameNumber) {
    using clock = std::chrono::steady_clock;
    const auto offered = clock::now();
    const int64_t offeredNs = monotonicNowNs();
    stats_.framesOffered.add();

    if (!frame || frame->empty())
        return refuse(Outcome::Rejected, frameNumber, "empty frame");
    const size_t bytes = frame->size();
    if (bytes > std::numeric_limits<uint32_t>::max())
        return refuse(Outcome::Rejected, frameNumber, "frame exceeds the 4 GiB E2SAR event limit");
    const uint64_t eventNum = eventNumberFor(frameNumber, cfg_.eventNumOffset);
    if (eventNum == 0)
        return refuse(Outcome::Rejected, frameNumber, "event number 0 is reserved by E2SAR");

    // Absolute time after which this frame gives up waiting, given the policy and any shutdown.
    auto waitLimit = [&]() -> clock::time_point {
        clock::time_point limit = clock::time_point::max();
        if (cfg_.overflow == OverflowPolicy::Drop) limit = offered + cfg_.dropAfter;
        if (shuttingDown_ && shutdownDeadline_ < limit) limit = shutdownDeadline_;
        return limit;
    };

    // ---- Phase 1: reserve in-flight capacity ------------------------------------------
    {
        std::unique_lock<std::mutex> lock(mtx_);
        if (closed_)
            return refuse(Outcome::DroppedShutdown, frameNumber, "sender is closed");

        if (!hasSpaceLocked(bytes)) {
            stats_.blockedSubmits.add();
            while (!hasSpaceLocked(bytes)) {
                if (closed_) {
                    stats_.blockedNs.add(static_cast<uint64_t>(monotonicNowNs() - offeredNs));
                    return refuse(Outcome::DroppedShutdown, frameNumber, "sender closed while waiting");
                }
                auto limit = waitLimit();
                if (clock::now() >= limit) {
                    stats_.blockedNs.add(static_cast<uint64_t>(monotonicNowNs() - offeredNs));
                    return shuttingDown_
                        ? refuse(Outcome::DroppedShutdown, frameNumber, "no space before drain deadline")
                        : refuse(Outcome::DroppedOverflow, frameNumber,
                                 "no space within " + std::to_string(cfg_.dropAfter.count()) + " ms");
                }
                if (limit == clock::time_point::max())
                    cv_.wait(lock);
                else
                    cv_.wait_until(lock, limit);
            }
            stats_.blockedNs.add(static_cast<uint64_t>(monotonicNowNs() - offeredNs));
        }

        inFlightFrames_++;
        inFlightBytes_ += bytes;
        stats_.inFlightFrames.set(static_cast<int64_t>(inFlightFrames_));
        stats_.inFlightBytes.set(static_cast<int64_t>(inFlightBytes_));
    }

    // ---- Phase 2: hand the frame to the transport --------------------------------------
    // The InFlight record carries our buffer reference until the transport calls back.
    auto* f = new InFlight{this, std::move(frame), 0};
    const uint8_t* data = f->frame->data();
    std::string error;
    {
        std::lock_guard<std::mutex> submitLock(submitMtx_);
        while (true) {
            // set before handing over: the callback may run before submit() returns
            f->submitNs = monotonicNowNs();
            auto st = transport_.submit(data, bytes, eventNum, &FrameSender::onTransportDone, f, error);
            if (st == FrameTransport::SubmitStatus::Accepted)
                break;

            if (st == FrameTransport::SubmitStatus::Failed) {
                delete f;
                unreserve(bytes);
                return refuse(Outcome::Rejected, frameNumber, error);
            }

            // QueueFull: nothing was enqueued, so retrying cannot duplicate the frame.
            stats_.submitRetries.add();
            clock::time_point limit;
            bool stopping;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                limit = waitLimit();
                stopping = shuttingDown_;
            }
            if (clock::now() >= limit) {
                delete f;
                unreserve(bytes);
                return stopping
                    ? refuse(Outcome::DroppedShutdown, frameNumber, "transport queue full at drain deadline")
                    : refuse(Outcome::DroppedOverflow, frameNumber, "transport queue full");
            }
            std::this_thread::sleep_for(cfg_.retryBackoff);
        }
    }

    // Counted after the hand-over, so in a snapshot 'released' may briefly lead 'submitted'.
    const int64_t submittedNs = monotonicNowNs();
    stats_.framesSubmitted.add();
    stats_.bytesSubmitted.add(bytes);
    const size_t perPacket = transport_.maxPayloadPerPacket();
    if (perPacket > 0) stats_.packetsEstimated.add((bytes + perPacket - 1) / perPacket);
    stats_.queueWaitNs.observe(static_cast<uint64_t>(submittedNs - offeredNs));
    stats_.lastSubmitted.markAt(submittedNs);
    return Outcome::Submitted;
}

void FrameSender::onTransportDone(void* ctx) {
    auto* f = static_cast<InFlight*>(ctx);
    f->owner->release(f);
}

void FrameSender::release(InFlight* f) {
    const size_t bytes = f->frame->size();
    const int64_t now = monotonicNowNs();
    stats_.framesReleased.add();
    stats_.bytesReleased.add(bytes);
    stats_.releaseLatencyNs.observe(static_cast<uint64_t>(now - f->submitNs));
    stats_.lastReleased.markAt(now);
    delete f;  // drops this sender's reference to the frame buffer

    // Notify while holding the lock: once it is released, finishShutdown() may
    // return and the owner may tear this object down.
    std::lock_guard<std::mutex> lock(mtx_);
    inFlightFrames_--;
    inFlightBytes_ -= bytes;
    stats_.inFlightFrames.set(static_cast<int64_t>(inFlightFrames_));
    stats_.inFlightBytes.set(static_cast<int64_t>(inFlightBytes_));
    cv_.notify_all();
}

void FrameSender::beginShutdown(std::chrono::steady_clock::time_point deadline) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!shuttingDown_ || deadline < shutdownDeadline_) shutdownDeadline_ = deadline;
    shuttingDown_ = true;
    cv_.notify_all();
}

size_t FrameSender::finishShutdown() {
    std::unique_lock<std::mutex> lock(mtx_);
    if (!shuttingDown_) {
        shuttingDown_ = true;
        shutdownDeadline_ = std::chrono::steady_clock::now();
    }
    closed_ = true;
    cv_.notify_all();
    cv_.wait_until(lock, shutdownDeadline_, [this] { return inFlightFrames_ == 0; });
    stats_.framesAbandonedShutdown.set(static_cast<int64_t>(inFlightFrames_));
    return inFlightFrames_;
}

size_t FrameSender::inFlightFrames() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return inFlightFrames_;
}

size_t FrameSender::inFlightBytes() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return inFlightBytes_;
}

}  // namespace codafb
