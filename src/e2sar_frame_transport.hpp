/**
 * E2SARFrameTransport - FrameTransport backed by an E2SAR Segmenter
 *
 * Sends each frame as one E2SAR event to the second EJFAT load balancer,
 * using the Segmenter's asynchronous queue (addToSendQueue()); E2SAR
 * fragments it into MTU-sized packets with LB+RE headers.
 *
 * LIFECYCLE (verified against E2SAR v0.4.0rc1, and the order its own
 * e2sar_perf tool uses):
 *   create():   1. register this host as a sender with the LB control plane
 *                  (LBManager::addSenders(), instance token). The LB drops
 *                  data from unregistered senders.
 *               2. construct the Segmenter (checks MTU, data/sync addresses)
 *               3. Segmenter::openAndStart(): starts the sync thread, waits
 *                  SegmenterFlags::warmUpMs, opens the data sockets, starts
 *                  the send thread.
 *               Any failure undoes the steps already done, in reverse.
 *   shutdown(): Segmenter::stopThreads() (waits for E2SAR's own queue and
 *               send thread pool to finish), destroy it, then remove the
 *               sender registration.
 *   abandon():  for when frames are still in flight past the drain deadline:
 *               leave the Segmenter running (it still references frame
 *               buffers and will call back when done) and only remove the
 *               sender registration.
 *
 * E2SAR 0.4.0rc1 LIMITATIONS this class has to live with:
 * - The send callback fires whether or not sendmsg() succeeded, and the
 *   per-event result is discarded, so there is no per-frame send status.
 *   Packet failures are visible only as Segmenter send stats errCnt.
 * - The send thread moves events from the bounded queue into an unbounded
 *   thread pool queue, so E2SAR's eventQueueSize does not bound memory.
 *   FrameSender's in-flight limit does.
 * - The send thread polls its queue without sleeping; expect one core busy.
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#ifndef CODA_FB_E2SAR_FRAME_TRANSPORT_HPP
#define CODA_FB_E2SAR_FRAME_TRANSPORT_HPP

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <e2sar.hpp>

#include "frame_sender.hpp"

namespace codafb {

class E2SARFrameTransport final : public FrameTransport {
public:
    struct Config {
        uint16_t dataId{1};       // RE header data id of every forwarded frame
        uint32_t eventSrcId{0};   // sync header source id of this sender
        e2sar::Segmenter::SegmenterFlags sflags;  // sflags.useCP: sync packets + sender registration
        std::vector<std::string> senderAddresses; // empty: auto-detect the route towards the LB data address
        bool validateCert{true};
        bool useHostAddress{false};
    };

    static e2sar::result<std::unique_ptr<E2SARFrameTransport>>
    create(const e2sar::EjfatURI& uri, const Config& cfg) noexcept;

    ~E2SARFrameTransport() override;

    SubmitStatus submit(const uint8_t* data, size_t bytes, uint64_t eventNum,
                        CompletionFn done, void* ctx, std::string& error) noexcept override;
    size_t maxPayloadPerPacket() const noexcept override;

    void shutdown() noexcept;
    void abandon() noexcept;

    /** Live while running, frozen at the last value after shutdown()/abandon(). */
    e2sar::Segmenter::ReportedStats sendStats() const noexcept;
    e2sar::Segmenter::ReportedStats syncStats() const noexcept;

    const std::vector<std::string>& registeredSenders() const noexcept { return senders_; }
    uint16_t mtu() const noexcept { return mtu_; }
    const std::string& interface() const noexcept { return iface_; }

    E2SARFrameTransport(const E2SARFrameTransport&) = delete;
    E2SARFrameTransport& operator=(const E2SARFrameTransport&) = delete;

private:
    E2SARFrameTransport() = default;
    void removeSenders() noexcept;
    void freezeStats() noexcept;

    struct Completion {
        CompletionFn done;
        void* ctx;
    };
    static void onSent(boost::any arg);

    std::unique_ptr<e2sar::LBManager> lbm_;
    std::unique_ptr<e2sar::Segmenter> seg_;
    std::vector<std::string> senders_;
    size_t maxPldLen_{0};
    uint16_t mtu_{0};
    std::string iface_;
    bool torn_{false};
    std::optional<e2sar::Segmenter::ReportedStats> finalSend_;
    std::optional<e2sar::Segmenter::ReportedStats> finalSync_;
};

}  // namespace codafb

#endif  // CODA_FB_E2SAR_FRAME_TRANSPORT_HPP
