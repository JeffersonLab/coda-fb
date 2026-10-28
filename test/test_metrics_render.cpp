/**
 * Tests for metrics_render: Prometheus exposition format rules, naming
 * conventions, values and unit conversion, histogram invariants, omitted
 * families, escaping, and rendering while statistics are being updated.
 */

#include <atomic>
#include <cmath>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "metrics_render.hpp"
#include "test_harness.hpp"

using namespace codafb;

namespace {

struct Exposition {
    std::map<std::string, std::string> type;  // family -> TYPE
    std::set<std::string> help;               // families with HELP
    std::map<std::string, double> samples;    // "name{labels}" -> value
    std::vector<std::string> errors;
};

/** Strip _bucket/_sum/_count of a histogram sample back to its family name. */
std::string familyOf(const std::string& name, const std::map<std::string, std::string>& types) {
    for (const char* suffix : {"_bucket", "_sum", "_count"}) {
        std::string s(suffix);
        if (name.size() > s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0) {
            std::string base = name.substr(0, name.size() - s.size());
            auto it = types.find(base);
            if (it != types.end() && it->second == "histogram") return base;
        }
    }
    return name;
}

/** A strict-enough parser for the subset of the text format we emit. */
Exposition parse(const std::string& text) {
    static const std::regex sampleRe(R"(^([a-zA-Z_:][a-zA-Z0-9_:]*)(\{[^}]*\})? (\S+)$)");
    static const std::regex labelRe(R"(([a-zA-Z_][a-zA-Z0-9_]*)="((?:[^"\\]|\\.)*)\"(,|$))");
    Exposition e;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("# HELP ", 0) == 0) {
            std::string name = line.substr(7, line.find(' ', 7) - 7);
            if (!e.help.insert(name).second) e.errors.push_back("duplicate HELP " + name);
            continue;
        }
        if (line.rfind("# TYPE ", 0) == 0) {
            std::istringstream ls(line.substr(7));
            std::string name, type;
            ls >> name >> type;
            if (e.type.count(name)) e.errors.push_back("duplicate TYPE " + name);
            if (!e.help.count(name)) e.errors.push_back("TYPE before HELP " + name);
            e.type[name] = type;
            continue;
        }
        std::smatch m;
        if (!std::regex_match(line, m, sampleRe)) {
            e.errors.push_back("unparsable line: " + line);
            continue;
        }
        std::string name = m[1], labels = m[2];
        if (!labels.empty()) {  // every label must be name="value"
            std::string inner = labels.substr(1, labels.size() - 2);
            std::string rebuilt;
            for (auto it = std::sregex_iterator(inner.begin(), inner.end(), labelRe); it != std::sregex_iterator(); ++it)
                rebuilt += (*it)[0];
            if (rebuilt != inner) e.errors.push_back("bad labels: " + line);
        }
        if (!e.type.count(familyOf(name, e.type))) e.errors.push_back("sample without TYPE: " + name);
        std::string key = name + labels;
        if (e.samples.count(key)) e.errors.push_back("duplicate series " + key);
        e.samples[key] = (m[3] == "+Inf") ? INFINITY : std::stod(m[3]);
    }
    return e;
}

MetricsSources sources(const PipelineStats& st, bool building, bool forwarding) {
    MetricsSources src;
    src.stats = &st;
    src.building = building;
    src.forwarding = forwarding;
    src.version = "1.0.0";
    src.e2sarVersion = "0.4.0rc1";
    stampNow(src);
    return src;
}

bool has(const Exposition& e, const std::string& key) { return e.samples.count(key) != 0; }

}  // namespace

TEST(format_and_naming_rules_hold_for_full_output) {
    PipelineStats st;
    st.recv.lastEvent.mark();
    st.build.lastFrame.mark();
    st.send.lastSubmitted.mark();
    st.send.lastReleased.mark();
    st.build.frameBytes.observe(5000);
    st.send.queueWaitNs.observe(1234);
    auto src = sources(st, true, true);
    src.recv = E2SARReceiveSnapshot{};
    src.send = E2SARSendSnapshot{};
    std::string text = renderPrometheus(src);
    auto e = parse(text);

    for (auto& err : e.errors) std::cerr << "  " << err << std::endl;
    CHECK(e.errors.empty());
    CHECK(!text.empty() && text.back() == '\n');
    for (auto& [name, type] : e.type) {
        CHECK(name.rfind("coda_fb_", 0) == 0);
        bool total = name.size() > 6 && name.compare(name.size() - 6, 6, "_total") == 0;
        CHECK(type == "counter" ? total : !total);  // _total iff counter
        CHECK(type == "counter" || type == "gauge" || type == "histogram");
    }
    // nothing per-frame, per-event or per-URI leaks into labels
    CHECK(text.find("ejfat") == std::string::npos);
    CHECK_EQ(e.type.size(), size_t{43});  // every family of the schema in doc/monitoring.md
}

TEST(values_and_units) {
    PipelineStats st;
    st.recv.eventsReceived.add(7);
    st.recv.payloadValidationErrors.add(2);
    st.build.framesComplete.add(5);
    st.build.framesPartial.add(1);
    st.build.etOutputErrors.add(3);
    st.build.slicesPending.add(4);
    st.send.framesOffered.add(6);
    st.send.framesSubmitted.add(4);
    st.send.framesDroppedOverflow.add(2);
    st.send.bytesReleased.add(1000);
    st.send.blockedNs.add(1'500'000'000ULL);  // 1.5 s
    st.send.capacityFrames.set(256);
    st.health.receiverReady = true;
    st.health.senderReady = true;
    auto src = sources(st, true, true);
    src.recv = E2SARReceiveSnapshot{100, 20000, 9, 1, 2, 3, 4, 5};
    src.send = E2SARSendSnapshot{50, 1, 7, 0};
    auto e = parse(renderPrometheus(src));

    CHECK_EQ(e.samples["coda_fb_recv_events_total"], 7.0);
    CHECK_EQ(e.samples["coda_fb_recv_events_rejected_total{reason=\"payload_invalid\"}"], 2.0);
    CHECK_EQ(e.samples["coda_fb_recv_udp_packets_total"], 100.0);
    CHECK_EQ(e.samples["coda_fb_recv_events_lost_total{reason=\"reassembly_timeout\"}"], 1.0);
    CHECK_EQ(e.samples["coda_fb_recv_events_lost_total{reason=\"enqueue_full\"}"], 2.0);
    CHECK_EQ(e.samples["coda_fb_recv_errors_total{reason=\"grpc\"}"], 5.0);
    CHECK_EQ(e.samples["coda_fb_build_frames_total{outcome=\"complete\"}"], 5.0);
    CHECK_EQ(e.samples["coda_fb_build_frames_total{outcome=\"partial\"}"], 1.0);
    CHECK_EQ(e.samples["coda_fb_build_output_errors_total{output=\"et\"}"], 3.0);
    CHECK_EQ(e.samples["coda_fb_build_output_errors_total{output=\"file\"}"], 0.0);
    CHECK_EQ(e.samples["coda_fb_build_pending_slices"], 4.0);
    CHECK_EQ(e.samples["coda_fb_send_frames_offered_total"], 6.0);
    CHECK_EQ(e.samples["coda_fb_send_frames_total{outcome=\"submitted\"}"], 4.0);
    CHECK_EQ(e.samples["coda_fb_send_frames_total{outcome=\"dropped_overflow\"}"], 2.0);
    CHECK_EQ(e.samples["coda_fb_send_bytes_total{stage=\"released\"}"], 1000.0);
    CHECK(std::fabs(e.samples["coda_fb_send_blocked_seconds_total"] - 1.5) < 1e-12);
    CHECK_EQ(e.samples["coda_fb_send_capacity_frames"], 256.0);
    CHECK_EQ(e.samples["coda_fb_send_packets_total"], 50.0);
    CHECK_EQ(e.samples["coda_fb_send_packet_errors_total"], 1.0);
    CHECK_EQ(e.samples["coda_fb_ready{stage=\"receiver\"}"], 1.0);
    CHECK_EQ(e.samples["coda_fb_ready{stage=\"builder\"}"], 0.0);
    CHECK_EQ(e.samples["coda_fb_ready{stage=\"sender\"}"], 1.0);
    CHECK_EQ(e.samples["coda_fb_build_info{version=\"1.0.0\",e2sar_version=\"0.4.0rc1\"}"], 1.0);

    // outcomes partition offered
    double outcomes = 0;
    for (const char* o : {"submitted", "dropped_overflow", "dropped_shutdown", "rejected"})
        outcomes += e.samples[std::string("coda_fb_send_frames_total{outcome=\"") + o + "\"}"];
    CHECK_EQ(outcomes, e.samples["coda_fb_send_frames_offered_total"]);
}

TEST(histograms_convert_units_and_are_cumulative) {
    PipelineStats st;
    st.build.assemblyNs.observe(5'000);          // 5 us   -> le 1e-05
    st.build.assemblyNs.observe(2'000'000'000);  // 2 s    -> le 2.5
    st.build.assemblyNs.observe(20'000'000'000); // 20 s   -> +Inf only
    st.build.frameBytes.observe(1024);
    auto e = parse(renderPrometheus(sources(st, true, false)));

    const std::string a = "coda_fb_build_assembly_duration_seconds";
    CHECK_EQ(e.type[a], std::string("histogram"));
    CHECK_EQ(e.samples[a + "_bucket{le=\"1e-05\"}"], 1.0);
    CHECK_EQ(e.samples[a + "_bucket{le=\"2.5\"}"], 2.0);
    CHECK_EQ(e.samples[a + "_bucket{le=\"10\"}"], 2.0);
    CHECK_EQ(e.samples[a + "_bucket{le=\"+Inf\"}"], 3.0);
    CHECK_EQ(e.samples[a + "_count"], 3.0);
    CHECK(std::fabs(e.samples[a + "_sum"] - 22.000005) < 1e-9);

    // buckets never decrease, in the order they are listed
    double prev = 0;
    for (double le : {1e-05, 5e-05, 0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 2.5, 5.0, 10.0}) {
        std::ostringstream k;
        k << a << "_bucket{le=\"" << le << "\"}";
        CHECK(has(e, k.str()));
        CHECK(e.samples[k.str()] >= prev);
        prev = e.samples[k.str()];
    }

    // byte histogram keeps integer bounds
    CHECK_EQ(e.samples["coda_fb_build_frame_size_bytes_bucket{le=\"1024\"}"], 1.0);
    CHECK_EQ(e.samples["coda_fb_build_frame_size_bytes_sum"], 1024.0);
}

TEST(disabled_stages_and_missing_sources_are_omitted) {
    PipelineStats st;
    auto e = parse(renderPrometheus(sources(st, false, false)));
    for (auto& [name, type] : e.type) {
        CHECK(name.rfind("coda_fb_build_", 0) != 0 || name == "coda_fb_build_info");
        CHECK(name.rfind("coda_fb_send_", 0) != 0);
    }
    CHECK(!e.type.count("coda_fb_recv_udp_packets_total"));  // no E2SAR snapshot given
    CHECK(has(e, "coda_fb_recv_events_total"));              // coda-fb's own: always
    CHECK(has(e, "coda_fb_ready{stage=\"receiver\"}"));
    CHECK(!has(e, "coda_fb_ready{stage=\"builder\"}"));
    CHECK(!has(e, "coda_fb_ready{stage=\"sender\"}"));

    // forwarding without a segmenter snapshot: coda-fb send families yes, E2SAR ones no
    auto f = parse(renderPrometheus(sources(st, true, true)));
    CHECK(f.type.count("coda_fb_send_frames_offered_total"));
    CHECK(!f.type.count("coda_fb_send_packets_total"));
}

TEST(last_activity_only_after_it_happened) {
    PipelineStats st;
    auto e = parse(renderPrometheus(sources(st, true, true)));
    CHECK(e.type.count("coda_fb_last_activity_timestamp_seconds"));
    for (const char* stage : {"received", "built", "submitted", "released"})
        CHECK(!has(e, std::string("coda_fb_last_activity_timestamp_seconds{stage=\"") + stage + "\"}"));

    st.build.lastFrame.markAt(monotonicNowNs() - 3'000'000'000LL);  // 3 s ago
    auto src = sources(st, true, true);
    auto e2 = parse(renderPrometheus(src));
    double t = e2.samples["coda_fb_last_activity_timestamp_seconds{stage=\"built\"}"];
    CHECK(std::fabs((src.nowUnixSeconds - t) - 3.0) < 0.05);
}

TEST(start_time_is_stable_across_scrapes) {
    PipelineStats st;
    auto a = parse(renderPrometheus(sources(st, false, false)));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto b = parse(renderPrometheus(sources(st, false, false)));
    CHECK_EQ(a.samples["coda_fb_start_time_seconds"], b.samples["coda_fb_start_time_seconds"]);
    CHECK(a.samples["coda_fb_start_time_seconds"] > 1.6e9);
}

TEST(label_values_are_escaped) {
    PipelineStats st;
    auto src = sources(st, false, false);
    src.version = "1.0 \"rc\"\\x\nnext";
    std::string text = renderPrometheus(src);
    CHECK(text.find("version=\"1.0 \\\"rc\\\"\\\\x\\nnext\"") != std::string::npos);
    auto e = parse(text);
    CHECK(e.errors.empty());
}

TEST(render_while_counters_change) {
    PipelineStats st;
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; t++) {
        writers.emplace_back([&] {
            while (!stop) {
                st.send.framesOffered.add();
                st.send.framesSubmitted.add();
                st.send.queueWaitNs.observe(1000);
                st.build.slicesPending.add(1);
                st.build.slicesPending.add(-1);
            }
        });
    }
    double last = -1;
    bool monotonic = true, clean = true;
    for (int i = 0; i < 300; i++) {
        auto e = parse(renderPrometheus(sources(st, true, true)));
        clean = clean && e.errors.empty();
        double v = e.samples["coda_fb_send_frames_offered_total"];
        monotonic = monotonic && v >= last;
        last = v;
        const std::string q = "coda_fb_send_queue_wait_seconds";
        clean = clean && e.samples[q + "_bucket{le=\"+Inf\"}"] == e.samples[q + "_count"];
    }
    stop = true;
    for (auto& w : writers) w.join();
    CHECK(clean);
    CHECK(monotonic);
    CHECK(last > 0);
}

int main() { return testing::runAllTests(); }
