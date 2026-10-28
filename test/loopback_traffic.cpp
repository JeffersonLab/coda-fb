/**
 * Loopback traffic for demonstrating coda-fb monitoring without EJFAT.
 *
 *   this tool (upstream)  --UDP-->  coda-fb  --UDP-->  this tool (downstream)
 *   E2SAR Segmenter, N ROC          --withcp=0         E2SAR Reassembler that
 *   streams, varying rate           --send-nocp        drains what coda-fb forwards
 *
 * The frame rate follows a slow sine between --min-hz and --max-hz, slice
 * sizes vary, and every --partial-every'th frame one stream is left out, so
 * the dashboard shows changing throughput and some partial frames.
 *
 * Usage:
 *   loopback_traffic [--coda-fb-port 21000] [--downstream-port 22000]
 *                    [--streams 3] [--seconds 600] [--min-hz 50] [--max-hz 250]
 *                    [--partial-every 200] [--period 120]
 * Start coda-fb with the matching options, see doc/monitoring.md section 6.
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <thread>

#include <e2sar.hpp>

using namespace e2sar;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> running{true};

void onSignal(int) { running = false; }

void freeBuffer(boost::any a) { free(boost::any_cast<uint8_t*>(a)); }

/** A ROC time slice that passes coda-fb's EVIO payload validation. */
uint8_t* makeSlice(uint32_t frame, uint16_t roc, size_t words, std::mt19937& rng) {
    auto* w = static_cast<uint32_t*>(malloc(words * 4));
    for (size_t i = 0; i < words; i++) w[i] = rng();
    w[7] = 0xc0da0100;                               // word 8: magic
    w[9] = (uint32_t(roc) << 16) | (0x10 << 8) | 1;  // word 10: ROC id | 0x10 | status
    w[13] = frame;                                   // word 14: frame number
    w[14] = frame * 1000;                            // words 15/16: timestamp
    w[15] = 0;
    return reinterpret_cast<uint8_t*>(w);
}

long argOr(int argc, char** argv, const char* name, long dflt) {
    for (int i = 1; i + 1 < argc; i++)
        if (std::strcmp(argv[i], name) == 0) return std::atol(argv[i + 1]);
    return dflt;
}

}  // namespace

int main(int argc, char** argv) {
    const uint16_t inPort = static_cast<uint16_t>(argOr(argc, argv, "--coda-fb-port", 21000));
    const uint16_t outPort = static_cast<uint16_t>(argOr(argc, argv, "--downstream-port", 22000));
    const int streams = static_cast<int>(argOr(argc, argv, "--streams", 3));
    const long seconds = argOr(argc, argv, "--seconds", 600);
    const double minHz = static_cast<double>(argOr(argc, argv, "--min-hz", 50));
    const double maxHz = static_cast<double>(argOr(argc, argv, "--max-hz", 250));
    const long partialEvery = argOr(argc, argv, "--partial-every", 200);
    const double period = static_cast<double>(argOr(argc, argv, "--period", 120));
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // downstream: stands in for the workers behind the second load balancer
    EjfatURI downUri("ejfat://unused@127.0.0.1:9876/lb/99?data=127.0.0.1", EjfatURI::TokenType::instance);
    Reassembler::ReassemblerFlags rflags;
    rflags.useCP = false;
    rflags.rcvSocketBufSize = 200000;
    Reassembler downstream(downUri, ip::make_address("127.0.0.1"), outPort, 1, rflags);
    if (downstream.openAndStart().has_error()) {
        std::cerr << "cannot listen on 127.0.0.1:" << outPort << std::endl;
        return 1;
    }
    std::atomic<uint64_t> drained{0};
    std::thread drain([&] {
        while (running) {
            uint8_t* buf{nullptr};
            size_t len{0};
            EventNum_t evn{0};
            uint16_t did{0};
            auto r = downstream.recvEvent(&buf, &len, &evn, &did, 100);
            if (!r.has_error() && r.value() == 0) {
                delete[] buf;
                drained++;
            }
        }
    });

    // upstream: ROC streams into coda-fb's receive port
    EjfatURI upUri("ejfat://unused@127.0.0.1:9876/lb/5?data=127.0.0.1:" + std::to_string(inPort) +
                   "&sync=127.0.0.1:19998", EjfatURI::TokenType::instance);
    Segmenter::SegmenterFlags sflags;
    sflags.useCP = false;
    sflags.warmUpMs = 0;
    sflags.sndSocketBufSize = 200000;
    sflags.numSendSockets = 1;
    Segmenter upstream(upUri, 1, 0x1234, sflags);
    if (upstream.openAndStart().has_error()) {
        std::cerr << "cannot start the upstream segmenter" << std::endl;
        running = false;
        drain.join();
        return 1;
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> words(256, 2048);  // 1-8 KiB per slice
    const auto t0 = std::chrono::steady_clock::now();
    auto next = t0;
    uint32_t frame = 0;
    uint64_t sent = 0;
    auto lastReport = t0;
    std::cout << "sending " << streams << " streams to 127.0.0.1:" << inPort << ", draining 127.0.0.1:" << outPort
              << " for " << seconds << " s" << std::endl;

    while (running && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double hz = minHz + (maxHz - minHz) * 0.5 * (1.0 + std::sin(2 * M_PI * t / period));
        frame++;
        const bool partial = partialEvery > 0 && frame % partialEvery == 0;
        for (int s = 1; s <= streams; s++) {
            if (partial && s == streams) continue;  // this frame will time out as partial
            size_t n = words(rng);
            uint8_t* buf = makeSlice(frame, static_cast<uint16_t>(s), n, rng);
            while (running && upstream.addToSendQueue(buf, n * 4, frame, static_cast<uint16_t>(s), 0,
                                                      &freeBuffer, buf).has_error())
                std::this_thread::sleep_for(1ms);
            sent++;
        }
        next += std::chrono::nanoseconds(static_cast<int64_t>(1e9 / hz));
        std::this_thread::sleep_until(next);

        if (std::chrono::steady_clock::now() - lastReport > 10s) {
            lastReport = std::chrono::steady_clock::now();
            std::cout << "t=" << static_cast<int>(t) << "s rate=" << static_cast<int>(hz) << " Hz frames=" << frame
                      << " slices=" << sent << " drained=" << drained.load() << std::endl;
        }
    }

    running = false;
    upstream.stopThreads();
    drain.join();
    downstream.stopThreads();
    std::cout << "done: frames=" << frame << " slices=" << sent << " frames drained downstream=" << drained.load()
              << std::endl;
    return 0;
}
