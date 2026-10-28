/**
 * Tests for MetricsEndpoint over real TCP sockets on 127.0.0.1.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "metrics_endpoint.hpp"
#include "metrics_render.hpp"
#include "test_harness.hpp"

using namespace codafb;
using namespace std::chrono_literals;

namespace {

int connectTo(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/** Send a raw request and read until the server closes. */
std::string request(uint16_t port, const std::string& raw) {
    int fd = connectTo(port);
    if (fd < 0) return "";
    (void)!write(fd, raw.data(), raw.size());
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    close(fd);
    return out;
}

std::string get(uint16_t port, const std::string& path) {
    return request(port, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
}

int status(const std::string& response) {
    if (response.rfind("HTTP/1.1 ", 0) != 0) return -1;
    return std::stoi(response.substr(9, 3));
}

std::string body(const std::string& response) {
    auto p = response.find("\r\n\r\n");
    return p == std::string::npos ? "" : response.substr(p + 4);
}

}  // namespace

TEST(serves_metrics_with_prometheus_content_type) {
    std::atomic<int> renders{0};
    MetricsEndpoint ep([&] {
        renders++;
        return std::string("coda_fb_test_total 42\n");
    });
    CHECK_EQ(ep.start("127.0.0.1", 0), std::string());
    CHECK(ep.port() != 0);

    auto r = get(ep.port(), "/metrics");
    CHECK_EQ(status(r), 200);
    CHECK(r.find("Content-Type: text/plain; version=0.0.4; charset=utf-8") != std::string::npos);
    CHECK(r.find("Connection: close") != std::string::npos);
    CHECK_EQ(body(r), std::string("coda_fb_test_total 42\n"));
    CHECK_EQ(status(get(ep.port(), "/metrics?debug=1")), 200);  // query string ignored
    CHECK_EQ(renders.load(), 2);
    CHECK_EQ(ep.scrapes(), uint64_t{2});
}

TEST(other_paths_and_methods) {
    MetricsEndpoint ep([] { return std::string("x 1\n"); });
    CHECK_EQ(ep.start("127.0.0.1", 0), std::string());
    CHECK_EQ(status(get(ep.port(), "/")), 200);
    CHECK_EQ(status(get(ep.port(), "/metricsx")), 404);
    CHECK_EQ(status(get(ep.port(), "/favicon.ico")), 404);
    auto post = request(ep.port(), "POST /metrics HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    CHECK_EQ(status(post), 405);
    CHECK(post.find("Allow: GET") != std::string::npos);
    CHECK_EQ(ep.scrapes(), uint64_t{0});
    CHECK_EQ(request(ep.port(), "garbage\r\n\r\n").find("HTTP/1.1 200"), std::string::npos);
    CHECK_EQ(status(get(ep.port(), "/metrics")), 200);  // still serving after a bad request
}

TEST(render_failure_is_500_not_a_crash) {
    MetricsEndpoint ep([]() -> std::string { throw std::runtime_error("boom"); });
    CHECK_EQ(ep.start("127.0.0.1", 0), std::string());
    auto r = get(ep.port(), "/metrics");
    CHECK_EQ(status(r), 500);
    CHECK(body(r).find("boom") != std::string::npos);
}

TEST(stalled_client_does_not_block_scrapes) {
    MetricsEndpoint ep([] { return std::string("x 1\n"); }, 300ms);
    CHECK_EQ(ep.start("127.0.0.1", 0), std::string());

    int stalled = connectTo(ep.port());  // connects, never sends a request
    CHECK(stalled >= 0);
    auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(status(get(ep.port(), "/metrics")), 200);
    CHECK(std::chrono::steady_clock::now() - t0 < 250ms);  // answered while the other is pending

    // the stalled connection is closed by the server after the read timeout
    char c;
    t0 = std::chrono::steady_clock::now();
    ssize_t n = read(stalled, &c, 1);
    CHECK_EQ(n, ssize_t{0});
    CHECK(std::chrono::steady_clock::now() - t0 < 3s);
    close(stalled);
}

TEST(concurrent_scrapes) {
    MetricsEndpoint ep([] { return std::string(100'000, 'a') + "\n"; });
    CHECK_EQ(ep.start("127.0.0.1", 0), std::string());
    std::atomic<int> ok{0};
    std::vector<std::thread> clients;
    for (int t = 0; t < 8; t++) {
        clients.emplace_back([&] {
            for (int i = 0; i < 25; i++) {
                auto r = get(ep.port(), "/metrics");
                if (status(r) == 200 && body(r).size() == 100'001) ok++;
            }
        });
    }
    for (auto& c : clients) c.join();
    CHECK_EQ(ok.load(), 200);
    CHECK_EQ(ep.scrapes(), uint64_t{200});
}

TEST(bind_errors_are_reported) {
    MetricsEndpoint first([] { return std::string(); });
    CHECK_EQ(first.start("127.0.0.1", 0), std::string());

    MetricsEndpoint second([] { return std::string(); });
    std::string err = second.start("127.0.0.1", first.port());
    CHECK(err.find("cannot listen for metrics on 127.0.0.1:" + std::to_string(first.port())) != std::string::npos);
    CHECK_EQ(second.port(), uint16_t{0});

    MetricsEndpoint bad([] { return std::string(); });
    CHECK(bad.start("not-an-address", 9465).find("invalid --metrics-bind") != std::string::npos);
    CHECK(first.start("127.0.0.1", 0).find("already started") != std::string::npos);
}

TEST(stop_closes_the_port_and_is_idempotent) {
    auto* ep = new MetricsEndpoint([] { return std::string("x 1\n"); });
    CHECK_EQ(ep->start("127.0.0.1", 0), std::string());
    uint16_t port = ep->port();
    CHECK_EQ(status(get(port, "/metrics")), 200);
    ep->stop();
    ep->stop();
    CHECK_EQ(ep->port(), uint16_t{0});
    CHECK(connectTo(port) < 0);
    delete ep;  // destructor after stop

    MetricsEndpoint neverStarted([] { return std::string(); });
    neverStarted.stop();  // harmless
}

TEST(serves_rendered_pipeline_stats) {
    PipelineStats st;
    st.recv.eventsReceived.add(123);
    MetricsEndpoint ep([&] {
        MetricsSources src;
        src.stats = &st;
        stampNow(src);
        return renderPrometheus(src);
    });
    CHECK_EQ(ep.start("127.0.0.1", 0), std::string());
    auto b = body(get(ep.port(), "/metrics"));
    CHECK(b.find("\ncoda_fb_recv_events_total 123\n") != std::string::npos);
    st.recv.eventsReceived.add(1);
    CHECK(body(get(ep.port(), "/metrics")).find("\ncoda_fb_recv_events_total 124\n") != std::string::npos);
}

int main() { return testing::runAllTests(); }
