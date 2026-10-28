/**
 * E2SARFrameTransport implementation - see e2sar_frame_transport.hpp
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#include "e2sar_frame_transport.hpp"
#include "ejfat_uri_config.hpp"

#include <iostream>

using namespace e2sar;

namespace codafb {

result<std::unique_ptr<E2SARFrameTransport>>
E2SARFrameTransport::create(const EjfatURI& uri, const Config& cfg) noexcept {
    std::unique_ptr<E2SARFrameTransport> t(new E2SARFrameTransport());

    try {
        // ---- 1. sender registration on the second LB --------------------------------
        if (cfg.sflags.useCP) {
            t->lbm_ = std::make_unique<LBManager>(uri, cfg.validateCert, cfg.useHostAddress);

            std::vector<std::string> senders = cfg.senderAddresses;
            if (senders.empty()) {
                // Same detection LBManager::addSenderSelf() does; done here so that
                // exactly the registered addresses are removed again at shutdown.
                EjfatURI routeUri{uri};  // getDataplaneLocalAddresses() is not const in E2SAR 0.4.0rc1
                auto local = routeUri.getDataplaneLocalAddresses(cfg.sflags.dpV6);
                if (local.has_error())
                    return E2SARErrorInfo{local.error().code(),
                        "unable to detect the local address used to reach the send LB data plane (" +
                        redactCredentials(local.error().message()) + "); set --send-sender-ip"};
                for (const auto& a : local.value()) senders.push_back(a.to_string());
                if (senders.empty())
                    return E2SARErrorInfo{E2SARErrorc::SystemError,
                        "no local address found towards the send LB data plane; set --send-sender-ip"};
            }
            for (const auto& s : senders)
                if (NetUtil::isNonRoutable(s))
                    std::cerr << "WARNING: sender address " << s
                              << " is non-routable (private/loopback/link-local); the LB may not see it" << std::endl;

            std::cout << "Registering sender address(es) with the send LB control plane:";
            for (const auto& s : senders) std::cout << " " << s;
            std::cout << " ... " << std::flush;
            auto reg = t->lbm_->addSenders(senders);
            if (reg.has_error())
                return E2SARErrorInfo{E2SARErrorc::RPCError,
                    "unable to register as a sender with the send LB: " + redactCredentials(reg.error().message())};
            t->senders_ = senders;
            std::cout << "done" << std::endl;
        }

        // ---- 2. segmenter ---------------------------------------------------------------
        t->seg_ = std::make_unique<Segmenter>(uri, cfg.dataId, cfg.eventSrcId, cfg.sflags);

        // ---- 3. sockets and threads (includes the sync warm-up period) -------------------
        if (cfg.sflags.useCP)
            std::cout << "Starting segmenter (sync warm-up " << cfg.sflags.warmUpMs << " ms) ... " << std::flush;
        auto open = t->seg_->openAndStart();
        if (open.has_error()) {
            // the destructor stops whatever threads did start, then removes the senders
            return E2SARErrorInfo{open.error().code(),
                "unable to start the E2SAR segmenter: " + redactCredentials(open.error().message())};
        }
        if (cfg.sflags.useCP) std::cout << "done" << std::endl;

        t->maxPldLen_ = t->seg_->getMaxPldLen();
        t->mtu_ = t->seg_->getMTU();
        t->iface_ = t->seg_->getIntf();
        return std::move(t);
    } catch (const E2SARException& e) {
        return E2SARErrorInfo{E2SARErrorc::CaughtException,
            "unable to initialize the E2SAR sender: " + redactCredentials(static_cast<std::string>(e))};
    } catch (const std::exception& e) {
        return E2SARErrorInfo{E2SARErrorc::CaughtException,
            "unable to initialize the E2SAR sender: " + redactCredentials(e.what())};
    }
    // On every early return 't' is destroyed, which undoes the steps already taken.
}

E2SARFrameTransport::~E2SARFrameTransport() {
    if (!torn_) shutdown();
}

void E2SARFrameTransport::onSent(boost::any arg) {
    // Runs on an E2SAR send pool thread after the last sendmsg() attempt for the event.
    try {
        auto c = boost::any_cast<Completion>(arg);
        c.done(c.ctx);
    } catch (const boost::bad_any_cast&) {
        std::cerr << "[E2SARFrameTransport] BUG: unexpected callback argument" << std::endl;
    }
}

FrameTransport::SubmitStatus E2SARFrameTransport::submit(const uint8_t* data, size_t bytes, uint64_t eventNum,
                                                         CompletionFn done, void* ctx, std::string& error) noexcept {
    if (!seg_ || torn_) {
        error = "sender is shut down";
        return SubmitStatus::Failed;
    }
    // E2SAR only reads the buffer; the const_cast is for its non-const signature.
    auto res = seg_->addToSendQueue(const_cast<uint8_t*>(data), bytes, eventNum, 0, 0,
                                    &E2SARFrameTransport::onSent, boost::any(Completion{done, ctx}));
    if (!res.has_error())
        return SubmitStatus::Accepted;
    // addToSendQueue() reports a full queue, and only that, as MemoryError; the item was not enqueued.
    if (res.error().code() == E2SARErrorc::MemoryError)
        return SubmitStatus::QueueFull;
    error = res.error().message();
    return SubmitStatus::Failed;
}

size_t E2SARFrameTransport::maxPayloadPerPacket() const noexcept {
    return maxPldLen_;
}

void E2SARFrameTransport::freezeStats() noexcept {
    if (seg_ && !finalSend_) {
        finalSend_.emplace(seg_->getSendStats());
        finalSync_.emplace(seg_->getSyncStats());
    }
}

void E2SARFrameTransport::removeSenders() noexcept {
    if (!lbm_ || senders_.empty()) return;
    std::cout << "Removing sender registration from the send LB ... " << std::flush;
    auto res = lbm_->removeSenders(senders_);
    if (res.has_error())
        std::cerr << "failed: " << redactCredentials(res.error().message()) << std::endl;
    else
        std::cout << "done" << std::endl;
    senders_.clear();
}

void E2SARFrameTransport::shutdown() noexcept {
    if (torn_) return;
    torn_ = true;
    if (seg_) {
        // Waits for E2SAR's queue to empty and its send pool to finish every
        // posted event; all frame callbacks have run when this returns.
        seg_->stopThreads();
        freezeStats();
        seg_.reset();
    }
    // after the segmenter, so data already queued is not dropped by the LB
    removeSenders();
}

void E2SARFrameTransport::abandon() noexcept {
    if (torn_) return;
    torn_ = true;
    freezeStats();
    // Deliberately leaked: its threads still hold frame buffers and will call
    // back into the FrameSender, which the owner leaks for the same reason.
    (void)seg_.release();
    removeSenders();
}

Segmenter::ReportedStats E2SARFrameTransport::sendStats() const noexcept {
    if (finalSend_) return *finalSend_;
    return seg_->getSendStats();
}

Segmenter::ReportedStats E2SARFrameTransport::syncStats() const noexcept {
    if (finalSync_) return *finalSync_;
    return seg_->getSyncStats();
}

}  // namespace codafb
