/**
 * Tests for FrameSender against a mock transport: exactly-once submission,
 * buffer lifetime, backpressure (frames and bytes), overflow policies,
 * bounded retries without duplicates, event number mapping and shutdown.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "frame_sender.hpp"
#include "test_harness.hpp"

using namespace codafb;
using namespace std::chrono_literals;

namespace {

/** Frame whose bytes are derived from its outbound event number, so corruption is detectable. */
FrameBuffer makeFrame(uint64_t eventNum, size_t bytes) {
    auto v = std::make_shared<std::vector<uint8_t>>(bytes);
    for (size_t i = 0; i < bytes; i++) (*v)[i] = static_cast<uint8_t>((eventNum * 31 + i) & 0xFF);
    return v;
}

bool contentMatches(const uint8_t* data, size_t bytes, uint64_t eventNum) {
    for (size_t i = 0; i < bytes; i++)
        if (data[i] != static_cast<uint8_t>((eventNum * 31 + i) & 0xFF)) return false;
    return true;
}

/**
 * Transport that queues accepted frames. Frames are completed manually by
 * the test, or by a background thread in async mode. Every completion first
 * verifies the frame bytes, which would fail if the buffer had been freed.
 */
class MockTransport : public FrameTransport {
public:
    struct Pending {
        const uint8_t* data;
        size_t bytes;
        uint64_t eventNum;
        CompletionFn done;
        void* ctx;
    };

    std::atomic<int> queueFullResponses{0};  // answer QueueFull this many times
    std::atomic<bool> failAll{false};
    std::atomic<int> submitCalls{0};
    std::atomic<int> badContent{0};

    ~MockTransport() override { stopAsync(); }

    SubmitStatus submit(const uint8_t* data, size_t bytes, uint64_t eventNum,
                        CompletionFn done, void* ctx, std::string& error) noexcept override {
        submitCalls++;
        if (failAll) {
            error = "mock permanent failure";
            return SubmitStatus::Failed;
        }
        if (queueFullResponses > 0) {
            queueFullResponses--;
            return SubmitStatus::QueueFull;
        }
        std::lock_guard<std::mutex> lock(m_);
        pending_.push_back({data, bytes, eventNum, done, ctx});
        accepted_.push_back(eventNum);
        cv_.notify_all();
        return SubmitStatus::Accepted;
    }

    size_t maxPayloadPerPacket() const noexcept override { return 1000; }

    size_t pendingCount() {
        std::lock_guard<std::mutex> lock(m_);
        return pending_.size();
    }

    std::vector<uint64_t> accepted() {
        std::lock_guard<std::mutex> lock(m_);
        return accepted_;
    }

    /** Complete the oldest pending frame; returns false if none. */
    bool completeOne() {
        Pending p;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (pending_.empty()) return false;
            p = pending_.front();
            pending_.pop_front();
        }
        if (!contentMatches(p.data, p.bytes, p.eventNum)) badContent++;
        p.done(p.ctx);
        return true;
    }

    void completeAll() {
        while (completeOne()) {}
    }

    /** Complete frames on a background thread, optionally after a delay each. */
    void startAsync(std::chrono::microseconds delay = 0us) {
        stop_ = false;
        worker_ = std::thread([this, delay] {
            std::unique_lock<std::mutex> lock(m_);
            while (!stop_ || !pending_.empty()) {
                if (pending_.empty()) {
                    cv_.wait_for(lock, 5ms);
                    continue;
                }
                lock.unlock();
                if (delay.count() > 0) std::this_thread::sleep_for(delay);
                completeOne();
                lock.lock();
            }
        });
    }

    void stopAsync() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
            cv_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Pending> pending_;
    std::vector<uint64_t> accepted_;
    std::thread worker_;
    bool stop_{false};
};

FrameSenderConfig smallConfig() {
    FrameSenderConfig c;
    c.maxInFlightFrames = 4;
    c.maxInFlightBytes = 1 << 20;
    c.eventNumOffset = 1;
    return c;
}

template <typename T>
bool finishesWithin(std::future<T>& f, std::chrono::milliseconds d) {
    return f.wait_for(d) == std::future_status::ready;
}

}  // namespace

TEST(each_frame_submitted_exactly_once_concurrently) {
    MockTransport t;
    FrameSendStats st;
    FrameSenderConfig cfg;
    cfg.maxInFlightFrames = 16;  // small, so submitters block regularly
    FrameSender s(t, cfg, st);
    t.startAsync();

    constexpr int kThreads = 4, kPerThread = 2000;
    std::vector<std::thread> builders;
    std::atomic<int> notSubmitted{0};
    for (int b = 0; b < kThreads; b++) {
        builders.emplace_back([&, b] {
            for (int i = 0; i < kPerThread; i++) {
                uint32_t frameNumber = static_cast<uint32_t>(b * kPerThread + i);
                auto frame = makeFrame(FrameSender::eventNumberFor(frameNumber, cfg.eventNumOffset), 100 + (i % 3000));
                if (s.submit(std::move(frame), frameNumber) != FrameSender::Outcome::Submitted) notSubmitted++;
            }
        });
    }
    for (auto& th : builders) th.join();

    s.beginShutdown(std::chrono::steady_clock::now() + 5s);
    CHECK_EQ(s.finishShutdown(), size_t{0});
    t.stopAsync();

    constexpr uint64_t total = kThreads * kPerThread;
    CHECK_EQ(notSubmitted.load(), 0);
    auto acc = t.accepted();
    CHECK_EQ(acc.size(), size_t{total});
    std::set<uint64_t> unique(acc.begin(), acc.end());
    CHECK_EQ(unique.size(), size_t{total});           // no duplicates
    CHECK_EQ(*unique.begin(), uint64_t{1});           // frame 0 -> event 1
    CHECK_EQ(*unique.rbegin(), uint64_t{total});      // frame total-1 -> event total
    CHECK_EQ(t.badContent.load(), 0);                 // every buffer still intact when sent
    CHECK_EQ(st.framesOffered.value(), total);
    CHECK_EQ(st.framesSubmitted.value(), total);
    CHECK_EQ(st.framesReleased.value(), total);
    CHECK_EQ(st.bytesSubmitted.value(), st.bytesReleased.value());
    CHECK_EQ(st.inFlightFrames.value(), int64_t{0});
    CHECK_EQ(st.inFlightBytes.value(), int64_t{0});
    CHECK_EQ(st.queueWaitNs.snapshot().count, total);
    CHECK_EQ(st.releaseLatencyNs.snapshot().count, total);
}

TEST(buffer_kept_alive_until_transport_releases_it) {
    MockTransport t;
    FrameSendStats st;
    FrameSender s(t, smallConfig(), st);

    auto frame = makeFrame(8, 5000);
    std::weak_ptr<const std::vector<uint8_t>> watch = frame;
    CHECK(s.submit(std::move(frame), 7) == FrameSender::Outcome::Submitted);
    // The builder no longer holds a reference; the sender must.
    CHECK(!watch.expired());
    CHECK_EQ(st.inFlightFrames.value(), int64_t{1});
    CHECK_EQ(st.packetsEstimated.value(), uint64_t{5});  // ceil(5000 / 1000)

    t.completeAll();
    CHECK_EQ(t.badContent.load(), 0);
    CHECK(watch.expired());
    CHECK_EQ(st.framesReleased.value(), uint64_t{1});
    CHECK_EQ(s.inFlightFrames(), size_t{0});
}

TEST(block_policy_waits_for_frame_space) {
    MockTransport t;
    FrameSendStats st;
    auto cfg = smallConfig();
    cfg.maxInFlightFrames = 2;
    FrameSender s(t, cfg, st);

    CHECK(s.submit(makeFrame(1, 10), 0) == FrameSender::Outcome::Submitted);
    CHECK(s.submit(makeFrame(2, 10), 1) == FrameSender::Outcome::Submitted);
    auto third = std::async(std::launch::async, [&] { return s.submit(makeFrame(3, 10), 2); });
    CHECK(!finishesWithin(third, 200ms));    // blocked: no space
    CHECK_EQ(t.accepted().size(), size_t{2});

    t.completeOne();
    CHECK(finishesWithin(third, 2000ms));
    CHECK(third.get() == FrameSender::Outcome::Submitted);
    CHECK_EQ(st.blockedSubmits.value(), uint64_t{1});
    CHECK(st.blockedNs.value() >= 150'000'000ULL);
    t.completeAll();
}

TEST(byte_limit_and_oversized_frame) {
    MockTransport t;
    FrameSendStats st;
    auto cfg = smallConfig();
    cfg.maxInFlightFrames = 100;
    cfg.maxInFlightBytes = 1000;
    FrameSender s(t, cfg, st);

    CHECK(s.submit(makeFrame(1, 600), 0) == FrameSender::Outcome::Submitted);
    auto second = std::async(std::launch::async, [&] { return s.submit(makeFrame(2, 600), 1); });
    CHECK(!finishesWithin(second, 200ms));   // 1200 > 1000 bytes
    t.completeOne();
    CHECK(finishesWithin(second, 2000ms));
    CHECK(second.get() == FrameSender::Outcome::Submitted);
    t.completeAll();

    // larger than the whole byte budget: admitted on its own instead of waiting forever
    auto big = std::async(std::launch::async, [&] { return s.submit(makeFrame(3, 5000), 2); });
    CHECK(finishesWithin(big, 2000ms));
    CHECK(big.get() == FrameSender::Outcome::Submitted);
    CHECK_EQ(st.capacityBytes.value(), int64_t{1000});
    t.completeAll();
}

TEST(drop_policy_drops_visibly_after_timeout) {
    MockTransport t;
    FrameSendStats st;
    auto cfg = smallConfig();
    cfg.maxInFlightFrames = 1;
    cfg.overflow = OverflowPolicy::Drop;
    cfg.dropAfter = 100ms;
    FrameSender s(t, cfg, st);

    CHECK(s.submit(makeFrame(1, 10), 0) == FrameSender::Outcome::Submitted);
    auto t0 = std::chrono::steady_clock::now();
    CHECK(s.submit(makeFrame(2, 10), 1) == FrameSender::Outcome::DroppedOverflow);
    auto waited = std::chrono::steady_clock::now() - t0;
    CHECK(waited >= 90ms && waited < 2s);
    CHECK_EQ(st.framesDroppedOverflow.value(), uint64_t{1});
    CHECK_EQ(st.framesOffered.value(), uint64_t{2});
    CHECK_EQ(st.framesSubmitted.value(), uint64_t{1});
    CHECK_EQ(t.accepted().size(), size_t{1});
    t.completeAll();
}

TEST(queue_full_is_retried_without_duplicates) {
    MockTransport t;
    FrameSendStats st;
    FrameSender s(t, smallConfig(), st);

    t.queueFullResponses = 3;
    CHECK(s.submit(makeFrame(6, 10), 5) == FrameSender::Outcome::Submitted);
    CHECK_EQ(t.submitCalls.load(), 4);
    CHECK_EQ(st.submitRetries.value(), uint64_t{3});
    CHECK_EQ(t.accepted().size(), size_t{1});
    CHECK_EQ(st.framesSubmitted.value(), uint64_t{1});
    t.completeAll();
    CHECK_EQ(st.framesReleased.value(), uint64_t{1});
}

TEST(queue_full_retries_are_bounded_by_drop_timeout) {
    MockTransport t;
    FrameSendStats st;
    auto cfg = smallConfig();
    cfg.overflow = OverflowPolicy::Drop;
    cfg.dropAfter = 50ms;
    FrameSender s(t, cfg, st);

    t.queueFullResponses = 1'000'000;
    CHECK(s.submit(makeFrame(1, 10), 0) == FrameSender::Outcome::DroppedOverflow);
    CHECK_EQ(t.accepted().size(), size_t{0});
    CHECK_EQ(s.inFlightFrames(), size_t{0});  // reservation was returned
    CHECK_EQ(st.framesDroppedOverflow.value(), uint64_t{1});
}

TEST(permanent_failure_is_rejected_and_counted) {
    MockTransport t;
    FrameSendStats st;
    FrameSender s(t, smallConfig(), st);

    t.failAll = true;
    CHECK(s.submit(makeFrame(1, 10), 0) == FrameSender::Outcome::Rejected);
    CHECK_EQ(t.submitCalls.load(), 1);        // no retry of a permanent failure
    CHECK_EQ(st.framesRejected.value(), uint64_t{1});
    CHECK_EQ(st.framesReleased.value(), uint64_t{0});
    CHECK_EQ(s.inFlightFrames(), size_t{0});

    t.failAll = false;
    CHECK(s.submit(FrameBuffer{}, 1) == FrameSender::Outcome::Rejected);  // no frame at all
    CHECK_EQ(st.framesRejected.value(), uint64_t{2});
}

TEST(event_number_mapping) {
    CHECK_EQ(FrameSender::eventNumberFor(0, 1), uint64_t{1});
    CHECK_EQ(FrameSender::eventNumberFor(41, 1), uint64_t{42});
    CHECK_EQ(FrameSender::eventNumberFor(0xFFFFFFFFu, 1), uint64_t{0x100000000ULL});  // no 32-bit wrap
    CHECK_EQ(FrameSender::eventNumberFor(7, 1000), uint64_t{1007});

    MockTransport t;
    FrameSendStats st;
    auto cfg = smallConfig();
    cfg.eventNumOffset = 0;
    FrameSender s(t, cfg, st);
    // event number 0 means "auto-increment" to E2SAR and must never be sent
    CHECK(s.submit(makeFrame(0, 10), 0) == FrameSender::Outcome::Rejected);
    CHECK(s.submit(makeFrame(5, 10), 5) == FrameSender::Outcome::Submitted);
    CHECK_EQ(t.accepted().at(0), uint64_t{5});
    t.completeAll();
}

TEST(shutdown_drains_in_flight_frames) {
    MockTransport t;
    FrameSendStats st;
    FrameSender s(t, smallConfig(), st);
    t.startAsync(20ms);

    for (uint32_t i = 0; i < 4; i++) CHECK(s.submit(makeFrame(i + 1, 100), i) == FrameSender::Outcome::Submitted);
    s.beginShutdown(std::chrono::steady_clock::now() + 5s);
    CHECK_EQ(s.finishShutdown(), size_t{0});
    CHECK_EQ(st.framesReleased.value(), uint64_t{4});
    CHECK_EQ(st.framesAbandonedShutdown.value(), int64_t{0});

    // closed: further frames are refused, counted, and never reach the transport
    CHECK(s.submit(makeFrame(9, 10), 8) == FrameSender::Outcome::DroppedShutdown);
    CHECK_EQ(st.framesDroppedShutdown.value(), uint64_t{1});
    CHECK_EQ(t.accepted().size(), size_t{4});
    t.stopAsync();
}

TEST(shutdown_deadline_abandons_and_drops_blocked) {
    MockTransport t;
    FrameSendStats st;
    auto cfg = smallConfig();
    cfg.maxInFlightFrames = 2;
    FrameSender s(t, cfg, st);

    CHECK(s.submit(makeFrame(1, 10), 0) == FrameSender::Outcome::Submitted);
    CHECK(s.submit(makeFrame(2, 10), 1) == FrameSender::Outcome::Submitted);
    // a builder blocked under the block policy...
    auto blocked = std::async(std::launch::async, [&] { return s.submit(makeFrame(3, 10), 2); });
    CHECK(!finishesWithin(blocked, 100ms));

    // ...is released at the drain deadline instead of hanging shutdown
    auto t0 = std::chrono::steady_clock::now();
    s.beginShutdown(t0 + 150ms);
    CHECK(finishesWithin(blocked, 2000ms));
    CHECK(blocked.get() == FrameSender::Outcome::DroppedShutdown);

    // nothing completes, so both in-flight frames are abandoned at the deadline
    CHECK_EQ(s.finishShutdown(), size_t{2});
    CHECK(std::chrono::steady_clock::now() - t0 >= 140ms);
    CHECK_EQ(st.framesAbandonedShutdown.value(), int64_t{2});
    CHECK_EQ(st.framesDroppedShutdown.value(), uint64_t{1});

    // late completions after abandonment are still safe
    t.completeAll();
    CHECK_EQ(st.framesReleased.value(), uint64_t{2});
    CHECK_EQ(s.inFlightFrames(), size_t{0});
}

TEST(outcomes_partition_offered_frames) {
    MockTransport t;
    FrameSendStats st;
    auto cfg = smallConfig();
    cfg.maxInFlightFrames = 3;
    cfg.overflow = OverflowPolicy::Drop;
    cfg.dropAfter = 1ms;
    FrameSender s(t, cfg, st);
    t.startAsync(200us);

    std::vector<std::thread> builders;
    for (int b = 0; b < 4; b++) {
        builders.emplace_back([&, b] {
            for (uint32_t i = 0; i < 500; i++) s.submit(makeFrame(b * 500 + i + 1, 64), b * 500 + i);
        });
    }
    for (auto& th : builders) th.join();
    s.beginShutdown(std::chrono::steady_clock::now() + 5s);
    CHECK_EQ(s.finishShutdown(), size_t{0});
    t.stopAsync();

    uint64_t outcomes = st.framesSubmitted.value() + st.framesDroppedOverflow.value() +
                        st.framesDroppedShutdown.value() + st.framesRejected.value();
    CHECK_EQ(st.framesOffered.value(), uint64_t{2000});
    CHECK_EQ(outcomes, st.framesOffered.value());
    CHECK_EQ(st.framesReleased.value(), st.framesSubmitted.value());
    CHECK_EQ(t.accepted().size(), size_t{st.framesSubmitted.value()});
    CHECK(st.framesDroppedOverflow.value() > 0);  // the configuration is meant to overflow
}

TEST(overflow_policy_parsing) {
    OverflowPolicy p{OverflowPolicy::Drop};
    CHECK(parseOverflowPolicy("block", p) && p == OverflowPolicy::Block);
    CHECK(parseOverflowPolicy("drop", p) && p == OverflowPolicy::Drop);
    CHECK(!parseOverflowPolicy("Block", p));
    CHECK(!parseOverflowPolicy("", p));
    CHECK(std::string(toString(OverflowPolicy::Block)) == "block");
}

int main() { return testing::runAllTests(); }
