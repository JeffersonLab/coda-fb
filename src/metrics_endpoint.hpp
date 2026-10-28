/**
 * MetricsEndpoint - embedded HTTP server for Prometheus scraping
 *
 * One background thread runs a Boost.Asio io_context with a Boost.Beast
 * HTTP/1.1 listener. It answers:
 *   GET  /metrics  -> 200, body from the render callback, Prometheus text format
 *   GET  /         -> 200, a one-line pointer to /metrics
 *   other paths    -> 404;  other methods -> 405
 * Every response closes the connection. A client that does not send a
 * complete request within the read timeout is disconnected, so a stuck
 * client cannot hold the listener.
 *
 * The render callback runs on the endpoint thread, once per scrape. It must
 * be thread-safe with respect to the data path; coda-fb's renders only read
 * relaxed atomics plus E2SAR's getStats() under a mutex shared with teardown.
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#ifndef CODA_FB_METRICS_ENDPOINT_HPP
#define CODA_FB_METRICS_ENDPOINT_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace codafb {

class MetricsEndpoint {
public:
    using RenderFn = std::function<std::string()>;

    MetricsEndpoint(RenderFn render, std::chrono::milliseconds readTimeout = std::chrono::seconds(5));
    ~MetricsEndpoint();  // calls stop()

    MetricsEndpoint(const MetricsEndpoint&) = delete;
    MetricsEndpoint& operator=(const MetricsEndpoint&) = delete;

    /**
     * Bind to bindAddress:port (port 0 = any free port) and start serving.
     * Returns an empty string on success, otherwise an error message.
     */
    std::string start(const std::string& bindAddress, uint16_t port);

    /**
     * Close the listening socket and join the thread. Idempotent. A connection
     * still open at that moment is dropped when the endpoint is destroyed.
     */
    void stop();

    /** The bound port (useful with port 0); 0 if not running. */
    uint16_t port() const noexcept;

    /** Scrapes served with 200 on /metrics. */
    uint64_t scrapes() const noexcept;

    struct Impl;  // implementation detail (Boost.Beast), defined in metrics_endpoint.cpp

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace codafb

#endif  // CODA_FB_METRICS_ENDPOINT_HPP
