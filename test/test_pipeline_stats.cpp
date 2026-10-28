/**
 * Tests for pipeline_stats.hpp: counter accuracy, concurrent updates,
 * histogram bucketing and quantile estimation, and update overhead.
 */

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "pipeline_stats.hpp"
#include "test_harness.hpp"

using namespace codafb;

namespace {
constexpr int kThreads = 8;
constexpr uint64_t kPerThread = 500'000;

template <typename F>
void runConcurrently(F body) {
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) threads.emplace_back(body, t);
    for (auto& th : threads) th.join();
}
}  // namespace

TEST(counter_exact_under_contention) {
    Counter c;
    runConcurrently([&](int) {
        for (uint64_t i = 0; i < kPerThread; i++) c.add();
    });
    CHECK_EQ(c.value(), kThreads * kPerThread);

    Counter bytes;
    runConcurrently([&](int t) {
        for (uint64_t i = 0; i < kPerThread; i++) bytes.add(static_cast<uint64_t>(t + 1));
    });
    // sum over t of (t+1) * kPerThread
    CHECK_EQ(bytes.value(), kPerThread * (kThreads * (kThreads + 1) / 2));
}

TEST(gauge_balanced_updates_return_to_zero) {
    Gauge g;
    runConcurrently([&](int) {
        for (uint64_t i = 0; i < kPerThread; i++) {
            g.add(3);
            g.add(-3);
        }
    });
    CHECK_EQ(g.value(), 0);
    g.set(-42);
    CHECK_EQ(g.value(), -42);
}

TEST(event_time_never_then_recent) {
    EventTime t;
    CHECK(!t.seen());
    CHECK(t.ageSeconds() < 0);
    t.mark();
    CHECK(t.seen());
    double age = t.ageSeconds();
    CHECK(age >= 0 && age < 1.0);
    t.markAt(monotonicNowNs() - 2'000'000'000LL);
    CHECK(t.ageSeconds() >= 2.0);
}

TEST(histogram_le_bucket_boundaries) {
    Histogram h({10, 100, 1000});
    h.observe(0);      // <= 10
    h.observe(10);     // <= 10 (le is inclusive)
    h.observe(11);     // <= 100
    h.observe(100);    // <= 100
    h.observe(1000);   // <= 1000
    h.observe(1001);   // +Inf
    auto s = h.snapshot();
    CHECK_EQ(s.cumulative.size(), size_t{4});
    CHECK_EQ(s.cumulative[0], uint64_t{2});
    CHECK_EQ(s.cumulative[1], uint64_t{4});
    CHECK_EQ(s.cumulative[2], uint64_t{5});
    CHECK_EQ(s.cumulative[3], uint64_t{6});
    CHECK_EQ(s.count, uint64_t{6});
    CHECK_EQ(s.sum, uint64_t{0 + 10 + 11 + 100 + 1000 + 1001});
}

TEST(histogram_concurrent_observations) {
    Histogram h(buckets::durationNs());
    runConcurrently([&](int t) {
        for (uint64_t i = 0; i < kPerThread; i++) h.observe(static_cast<uint64_t>(t) * 1'000'000ULL);
    });
    auto s = h.snapshot();
    CHECK_EQ(s.count, kThreads * kPerThread);
    CHECK_EQ(s.cumulative.back(), s.count);
    uint64_t expectedSum = 0;
    for (int t = 0; t < kThreads; t++) expectedSum += kPerThread * static_cast<uint64_t>(t) * 1'000'000ULL;
    CHECK_EQ(s.sum, expectedSum);
    for (size_t i = 1; i < s.cumulative.size(); i++) CHECK(s.cumulative[i] >= s.cumulative[i - 1]);
}

TEST(histogram_quantile_interpolation) {
    Histogram h({100, 200, 400});
    CHECK(h.snapshot().quantile(0.5) < 0);  // no data
    for (int i = 0; i < 50; i++) h.observe(150);  // bucket (100, 200]
    for (int i = 0; i < 50; i++) h.observe(300);  // bucket (200, 400]
    auto s = h.snapshot();
    // rank 50 is the top of the (100,200] bucket
    CHECK(std::fabs(s.quantile(0.5) - 200.0) < 1e-9);
    // rank 75 is halfway through (200,400]
    CHECK(std::fabs(s.quantile(0.75) - 300.0) < 1e-9);
    h.observe(10'000);  // overflow bucket: reported as the highest finite bound
    CHECK(std::fabs(h.snapshot().quantile(1.0) - 400.0) < 1e-9);
}

TEST(preset_buckets_are_strictly_increasing) {
    for (const auto& b : {buckets::frameBytes(), buckets::durationNs()}) {
        for (size_t i = 1; i < b.size(); i++) CHECK(b[i] > b[i - 1]);
    }
}

TEST(update_overhead_is_low) {
    // Not a benchmark; guards against an accidental lock or allocation on the hot path.
    Counter c;
    Histogram h(buckets::frameBytes());
    constexpr int n = 2'000'000;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) {
        c.add();
        h.observe(static_cast<uint64_t>(i) & 0xFFFFF);
    }
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    double perUpdate = static_cast<double>(ns) / n;
    std::cout << "  counter add + histogram observe: " << perUpdate << " ns per pair (uncontended)" << std::endl;
    CHECK(perUpdate < 1000.0);
    CHECK_EQ(c.value(), static_cast<uint64_t>(n));
}

TEST(pipeline_stats_start_zeroed) {
    PipelineStats st;
    CHECK_EQ(st.recv.eventsReceived.value(), uint64_t{0});
    CHECK_EQ(st.build.framesComplete.value(), uint64_t{0});
    CHECK_EQ(st.send.framesSubmitted.value(), uint64_t{0});
    CHECK_EQ(st.send.inFlightFrames.value(), int64_t{0});
    CHECK(!st.health.receiverReady.load());
    CHECK(st.health.uptimeSeconds() >= 0.0);
}

int main() { return testing::runAllTests(); }
