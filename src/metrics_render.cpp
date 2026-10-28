/**
 * Prometheus text exposition - see metrics_render.hpp and doc/monitoring.md
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#include "metrics_render.hpp"

#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <utility>

namespace codafb {

namespace {

std::string fmtDouble(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    return buf;
}

std::string escapeLabel(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

class Writer {
public:
    void family(const char* name, const char* type, const char* help) {
        out_ += "# HELP ";
        out_ += name;
        out_ += ' ';
        out_ += help;  // help texts are constants without '\' or newlines
        out_ += "\n# TYPE ";
        out_ += name;
        out_ += ' ';
        out_ += type;
        out_ += '\n';
    }

    void sample(const char* name, const std::string& labels, const std::string& value) {
        out_ += name;
        if (!labels.empty()) {
            out_ += '{';
            out_ += labels;
            out_ += '}';
        }
        out_ += ' ';
        out_ += value;
        out_ += '\n';
    }
    void sample(const char* name, const std::string& labels, uint64_t v) { sample(name, labels, std::to_string(v)); }
    void sample(const char* name, const std::string& labels, int64_t v) { sample(name, labels, std::to_string(v)); }
    void sampleD(const char* name, const std::string& labels, double v) { sample(name, labels, fmtDouble(v)); }

    /** Single unlabeled counter or gauge. */
    void scalar(const char* name, const char* type, const char* help, uint64_t v) {
        family(name, type, help);
        sample(name, "", v);
    }
    void scalar(const char* name, const char* type, const char* help, int64_t v) {
        family(name, type, help);
        sample(name, "", v);
    }

    /** One family, one sample per value of 'label'. */
    void labeled(const char* name, const char* type, const char* help, const char* label,
                 std::initializer_list<std::pair<const char*, uint64_t>> values) {
        family(name, type, help);
        for (const auto& [lv, v] : values)
            sample(name, std::string(label) + "=\"" + lv + "\"", v);
    }

    /** Histogram; 'scale' converts the internal unit to the exported one (1e-9: ns -> s). */
    void histogram(const char* name, const char* help, const Histogram& h, double scale) {
        auto s = h.snapshot();
        family(name, "histogram", help);
        const std::string bucket = std::string(name) + "_bucket";
        for (size_t i = 0; i < s.bounds.size(); i++)
            sample(bucket.c_str(), "le=\"" + fmtDouble(static_cast<double>(s.bounds[i]) * scale) + "\"",
                   s.cumulative[i]);
        sample(bucket.c_str(), "le=\"+Inf\"", s.count);
        sampleD((std::string(name) + "_sum").c_str(), "", static_cast<double>(s.sum) * scale);
        sample((std::string(name) + "_count").c_str(), "", s.count);
    }

    std::string take() { return std::move(out_); }

private:
    std::string out_;
};

constexpr double kNsToSeconds = 1e-9;

}  // namespace

void stampNow(MetricsSources& src) {
    src.nowMonotonicNs = monotonicNowNs();
    src.nowUnixSeconds = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string renderPrometheus(const MetricsSources& src) {
    Writer w;
    const PipelineStats& st = *src.stats;

    // ---- health -------------------------------------------------------------------------
    w.family("coda_fb_build_info", "gauge", "Build information; the value is always 1.");
    w.sample("coda_fb_build_info",
             "version=\"" + escapeLabel(src.version) + "\",e2sar_version=\"" + escapeLabel(src.e2sarVersion) + "\"",
             uint64_t{1});

    w.family("coda_fb_start_time_seconds", "gauge", "Unix time at which the process started.");
    w.sampleD("coda_fb_start_time_seconds", "", st.health.startUnixSeconds);

    w.family("coda_fb_ready", "gauge",
             "1 once the stage is initialized and running, 0 before that and during shutdown.");
    w.sample("coda_fb_ready", "stage=\"receiver\"", uint64_t{st.health.receiverReady.load() ? 1u : 0u});
    if (src.building)
        w.sample("coda_fb_ready", "stage=\"builder\"", uint64_t{st.health.builderReady.load() ? 1u : 0u});
    if (src.forwarding)
        w.sample("coda_fb_ready", "stage=\"sender\"", uint64_t{st.health.senderReady.load() ? 1u : 0u});

    // Monotonic marks converted to wall clock; a stage that never happened is left out.
    w.family("coda_fb_last_activity_timestamp_seconds", "gauge",
             "Unix time of the last event received, frame built, frame submitted and frame released by E2SAR.");
    auto activity = [&](const char* stage, const EventTime& t) {
        if (!t.seen()) return;
        w.sampleD("coda_fb_last_activity_timestamp_seconds", std::string("stage=\"") + stage + "\"",
                  src.nowUnixSeconds - t.ageSeconds(src.nowMonotonicNs));
    };
    activity("received", st.recv.lastEvent);
    if (src.building) activity("built", st.build.lastFrame);
    if (src.forwarding) {
        activity("submitted", st.send.lastSubmitted);
        activity("released", st.send.lastReleased);
    }

    // ---- receive: E2SAR reassembler (LB #1) ---------------------------------------------
    if (src.recv) {
        const auto& r = *src.recv;
        w.scalar("coda_fb_recv_udp_packets_total", "counter",
                 "UDP packets received by the E2SAR reassembler.", r.packets);
        w.scalar("coda_fb_recv_udp_bytes_total", "counter",
                 "UDP payload bytes received by the E2SAR reassembler, including E2SAR headers.", r.bytes);
        w.scalar("coda_fb_recv_events_reassembled_total", "counter",
                 "Events fully reassembled by E2SAR, including events then lost on enqueue.", r.reassembled);
        w.labeled("coda_fb_recv_events_lost_total", "counter",
                  "Events lost by E2SAR: segments missing at the reassembly timeout, or its queue full.", "reason",
                  {{"reassembly_timeout", r.lostReassembly}, {"enqueue_full", r.lostEnqueue}});
        w.labeled("coda_fb_recv_errors_total", "counter",
                  "E2SAR receive errors: invalid headers, data socket errors, control plane gRPC errors.", "reason",
                  {{"bad_header", r.badHeaders}, {"socket", r.socketErrors}, {"grpc", r.grpcErrors}});
    }

    // ---- receive: coda-fb reception loop ------------------------------------------------
    w.scalar("coda_fb_recv_events_total", "counter",
             "Reassembled events taken off the E2SAR queue by coda-fb.", st.recv.eventsReceived.value());
    w.scalar("coda_fb_recv_event_bytes_total", "counter",
             "Payload bytes of the events taken off the E2SAR queue.", st.recv.bytesReceived.value());
    w.labeled("coda_fb_recv_events_rejected_total", "counter",
              "Events not used: recvEvent() errors, or EVIO payloads that failed validation.", "reason",
              {{"recv_error", st.recv.recvErrors.value()},
               {"payload_invalid", st.recv.payloadValidationErrors.value()}});
    w.scalar("coda_fb_recv_events_byteswapped_total", "counter",
             "Events with a byte-swapped CODA magic word (kept, swapped).", st.recv.wrongEndianness.value());

    // ---- frame building -----------------------------------------------------------------
    if (src.building) {
        const auto& b = st.build;
        w.scalar("coda_fb_build_slices_total", "counter", "Slices accepted by the frame builder.", b.slicesIn.value());
        w.labeled("coda_fb_build_frames_total", "counter",
                  "Finalized frames by outcome; exactly one outcome per frame.", "outcome",
                  {{"complete", b.framesComplete.value()},
                   {"partial", b.framesPartial.value()},
                   {"build_failed", b.framesBuildFailed.value()},
                   {"discarded_shutdown", b.framesDiscardedShutdown.value()}});
        w.scalar("coda_fb_build_discarded_slices_total", "counter",
                 "Slices of frames discarded at shutdown.", b.slicesDiscardedShutdown.value());
        w.scalar("coda_fb_build_serialized_bytes_total", "counter",
                 "EVIO-6 bytes of serialized frames.", b.bytesSerialized.value());
        w.labeled("coda_fb_build_output_errors_total", "counter",
                  "Serialized frames that failed to be written to ET or to file.", "output",
                  {{"et", b.etOutputErrors.value()}, {"file", b.fileOutputErrors.value()}});
        w.scalar("coda_fb_build_pending_slices", "gauge", "Slices waiting in builder FIFOs.", b.slicesPending.value());
        w.scalar("coda_fb_build_frames_in_progress", "gauge",
                 "Corrected event numbers with slices waiting in builder FIFOs.", b.framesInProgress.value());
        w.histogram("coda_fb_build_frame_size_bytes", "Serialized EVIO-6 frame size.", b.frameBytes, 1.0);
        w.histogram("coda_fb_build_serialize_duration_seconds", "Time spent serializing one frame.",
                    b.serializeNs, kNsToSeconds);
        w.histogram("coda_fb_build_assembly_duration_seconds",
                    "Time from the first slice of a frame arriving to the frame being serialized.",
                    b.assemblyNs, kNsToSeconds);
    }

    // ---- forwarding (LB #2) -------------------------------------------------------------
    if (src.forwarding) {
        const auto& f = st.send;
        w.scalar("coda_fb_send_frames_offered_total", "counter",
                 "Built frames handed to the frame sender.", f.framesOffered.value());
        w.labeled("coda_fb_send_frames_total", "counter",
                  "Offered frames by outcome. submitted means accepted by the local E2SAR queue, not delivered.",
                  "outcome",
                  {{"submitted", f.framesSubmitted.value()},
                   {"dropped_overflow", f.framesDroppedOverflow.value()},
                   {"dropped_shutdown", f.framesDroppedShutdown.value()},
                   {"rejected", f.framesRejected.value()}});
        w.scalar("coda_fb_send_frames_released_total", "counter",
                 "Frames E2SAR finished sending (send callback). Not confirmation of delivery.",
                 f.framesReleased.value());
        w.scalar("coda_fb_send_partial_frames_skipped_total", "counter",
                 "Partial frames not forwarded because of --send-partial-frames=0.",
                 f.framesNotForwardedPartial.value());
        w.labeled("coda_fb_send_bytes_total", "counter",
                  "Frame payload bytes submitted to and released by E2SAR, excluding LB/RE/UDP/IP headers.", "stage",
                  {{"submitted", f.bytesSubmitted.value()}, {"released", f.bytesReleased.value()}});
        w.scalar("coda_fb_send_submit_retries_total", "counter",
                 "Submissions retried because the E2SAR queue was momentarily full (nothing was enqueued).",
                 f.submitRetries.value());
        w.family("coda_fb_send_blocked_seconds_total", "counter",
                 "Time builder threads spent waiting for in-flight space, summed over threads.");
        w.sampleD("coda_fb_send_blocked_seconds_total", "", static_cast<double>(f.blockedNs.value()) * kNsToSeconds);
        w.scalar("coda_fb_send_blocked_submits_total", "counter",
                 "Submissions that had to wait for in-flight space.", f.blockedSubmits.value());
        w.scalar("coda_fb_send_inflight_frames", "gauge",
                 "Frames handed to E2SAR and not yet released.", f.inFlightFrames.value());
        w.scalar("coda_fb_send_inflight_bytes", "gauge",
                 "Bytes of the frames in flight.", f.inFlightBytes.value());
        w.scalar("coda_fb_send_capacity_frames", "gauge", "In-flight frame limit (--send-queue-frames).",
                 f.capacityFrames.value());
        w.scalar("coda_fb_send_capacity_bytes", "gauge", "In-flight byte limit (--send-queue-bytes).",
                 f.capacityBytes.value());
        w.scalar("coda_fb_send_abandoned_frames", "gauge",
                 "Frames still in flight when the shutdown drain deadline expired (set at shutdown).",
                 f.framesAbandonedShutdown.value());
        w.histogram("coda_fb_send_queue_wait_seconds", "Time from a frame being offered to being submitted.",
                    f.queueWaitNs, kNsToSeconds);
        w.histogram("coda_fb_send_release_latency_seconds",
                    "Time from a frame being submitted to E2SAR releasing it.", f.releaseLatencyNs, kNsToSeconds);
        w.scalar("coda_fb_send_packets_estimated_total", "counter",
                 "ESTIMATE of packets sent: sum of ceil(frame bytes / payload bytes per packet).",
                 f.packetsEstimated.value());
        if (src.send) {
            const auto& s = *src.send;
            w.scalar("coda_fb_send_packets_total", "counter",
                     "Packets counted by the E2SAR segmenter (sendmsg attempts by default; successes with sendmmsg).",
                     s.packets);
            w.scalar("coda_fb_send_packet_errors_total", "counter",
                     "E2SAR packet send errors; the only signal of send failure in E2SAR 0.4.0rc1.", s.packetErrors);
            w.scalar("coda_fb_send_sync_messages_total", "counter", "Sync packets sent to the send LB.", s.syncMessages);
            w.scalar("coda_fb_send_sync_errors_total", "counter", "Sync packet send errors.", s.syncErrors);
        }
    }

    return w.take();
}

}  // namespace codafb
