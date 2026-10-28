/**
 * Prometheus text exposition (format 0.0.4) of coda-fb's statistics.
 *
 * Pure function of a snapshot: no network, no E2SAR objects. E2SAR's own
 * statistics are passed in as plain values, copied by the caller from
 * Reassembler::getStats() / Segmenter::getSendStats() / getSyncStats().
 *
 * Schema: doc/monitoring.md. Conventions:
 * - every name starts with coda_fb_; counters end in _total
 * - base units: bytes and seconds (internal nanoseconds are converted)
 * - labels only from fixed sets (stage, outcome, reason, output); never
 *   frame or event numbers, data ids, URIs, tokens or LB ids
 * - families for a disabled stage (no frame building, no forwarding) and
 *   E2SAR families whose source is not available are left out entirely
 *   rather than exported as zeros
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#ifndef CODA_FB_METRICS_RENDER_HPP
#define CODA_FB_METRICS_RENDER_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "pipeline_stats.hpp"

namespace codafb {

/** Copy of E2SAR Reassembler::ReportedStats (receive LB). */
struct E2SARReceiveSnapshot {
    uint64_t packets{0};
    uint64_t bytes{0};
    uint64_t reassembled{0};
    uint64_t lostReassembly{0};
    uint64_t lostEnqueue{0};
    uint64_t badHeaders{0};
    uint64_t socketErrors{0};
    uint64_t grpcErrors{0};
};

/** Copy of E2SAR Segmenter send and sync ReportedStats (send LB). */
struct E2SARSendSnapshot {
    uint64_t packets{0};
    uint64_t packetErrors{0};
    uint64_t syncMessages{0};
    uint64_t syncErrors{0};
};

struct MetricsSources {
    const PipelineStats* stats{nullptr};
    bool building{false};    // frame builder enabled
    bool forwarding{false};  // --send-ejfat-uri given
    std::optional<E2SARReceiveSnapshot> recv;
    std::optional<E2SARSendSnapshot> send;
    std::string version;
    std::string e2sarVersion;
    int64_t nowMonotonicNs{0};   // monotonicNowNs() at the time of the snapshot
    double nowUnixSeconds{0};    // wall clock at the same moment
};

/** Content-Type of renderPrometheus() output. */
inline constexpr const char* kPrometheusContentType = "text/plain; version=0.0.4; charset=utf-8";

std::string renderPrometheus(const MetricsSources& src);

/** Fill the two clock fields of 'src' with the current time. */
void stampNow(MetricsSources& src);

}  // namespace codafb

#endif  // CODA_FB_METRICS_RENDER_HPP
