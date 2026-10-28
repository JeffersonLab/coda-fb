/**
 * MetricsEndpoint implementation - see metrics_endpoint.hpp
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#include "metrics_endpoint.hpp"
#include "metrics_render.hpp"

#include <atomic>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;

namespace codafb {

struct MetricsEndpoint::Impl {
    RenderFn render;
    std::chrono::milliseconds readTimeout;
    net::io_context ioc{1};
    std::unique_ptr<tcp::acceptor> acceptor;
    std::thread thread;
    std::atomic<uint16_t> boundPort{0};
    std::atomic<uint64_t> scrapes{0};
    bool started{false};
    bool stopped{false};

    void accept();
};

namespace {

/** One request, one response, then close. */
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, MetricsEndpoint::Impl& impl) : stream_(std::move(socket)), impl_(impl) {}

    void start() {
        stream_.expires_after(impl_.readTimeout);
        http::async_read(stream_, buffer_, req_,
                         [self = shared_from_this()](beast::error_code ec, size_t) { self->onRead(ec); });
    }

private:
    void onRead(beast::error_code ec) {
        if (ec) {  // timeout, malformed request or client gone
            close();
            return;
        }
        res_.version(req_.version());
        res_.keep_alive(false);
        res_.set(http::field::server, "coda-fb");

        std::string target(req_.target());
        std::string path = target.substr(0, target.find('?'));

        if (req_.method() != http::verb::get) {
            res_.result(http::status::method_not_allowed);
            res_.set(http::field::allow, "GET");
            res_.set(http::field::content_type, "text/plain");
            res_.body() = "only GET is supported\n";
        } else if (path == "/metrics") {
            try {
                res_.body() = impl_.render();
                res_.result(http::status::ok);
                res_.set(http::field::content_type, kPrometheusContentType);
                impl_.scrapes.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception& e) {
                res_.result(http::status::internal_server_error);
                res_.set(http::field::content_type, "text/plain");
                res_.body() = std::string("rendering failed: ") + e.what() + "\n";
            }
        } else if (path == "/") {
            res_.result(http::status::ok);
            res_.set(http::field::content_type, "text/plain");
            res_.body() = "coda-fb: Prometheus metrics are at /metrics\n";
        } else {
            res_.result(http::status::not_found);
            res_.set(http::field::content_type, "text/plain");
            res_.body() = "not found; metrics are at /metrics\n";
        }
        res_.prepare_payload();

        stream_.expires_after(impl_.readTimeout);
        http::async_write(stream_, res_,
                          [self = shared_from_this()](beast::error_code, size_t) { self->close(); });
    }

    void close() {
        beast::error_code ignored;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
        stream_.socket().close(ignored);
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    http::response<http::string_body> res_;
    MetricsEndpoint::Impl& impl_;
};

}  // namespace

void MetricsEndpoint::Impl::accept() {
    acceptor->async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (ec == net::error::operation_aborted || !acceptor->is_open())
            return;  // stop() closed the acceptor
        if (!ec)
            std::make_shared<Session>(std::move(socket), *this)->start();
        accept();
    });
}

MetricsEndpoint::MetricsEndpoint(RenderFn render, std::chrono::milliseconds readTimeout)
    : impl_(std::make_unique<Impl>()) {
    impl_->render = std::move(render);
    impl_->readTimeout = readTimeout;
}

MetricsEndpoint::~MetricsEndpoint() { stop(); }

std::string MetricsEndpoint::start(const std::string& bindAddress, uint16_t port) {
    if (impl_->started) return "metrics endpoint already started";

    beast::error_code ec;
    auto address = net::ip::make_address(bindAddress, ec);
    if (ec) return "invalid --metrics-bind address '" + bindAddress + "'";
    tcp::endpoint where{address, port};
    const std::string at = (address.is_v6() ? "[" + bindAddress + "]" : bindAddress) + ":" + std::to_string(port);

    impl_->acceptor = std::make_unique<tcp::acceptor>(impl_->ioc);
    impl_->acceptor->open(where.protocol(), ec);
    if (!ec) impl_->acceptor->set_option(net::socket_base::reuse_address(true), ec);
    if (!ec) impl_->acceptor->bind(where, ec);
    if (!ec) impl_->acceptor->listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        impl_->acceptor.reset();
        return "cannot listen for metrics on " + at + ": " + ec.message();
    }
    impl_->boundPort = impl_->acceptor->local_endpoint().port();

    impl_->accept();
    impl_->started = true;
    impl_->thread = std::thread([impl = impl_.get()] { impl->ioc.run(); });
    return "";
}

void MetricsEndpoint::stop() {
    if (!impl_->started || impl_->stopped) return;
    impl_->stopped = true;
    // Close and stop from the endpoint thread itself: calling ioc.stop() from
    // here right after post() could stop the loop before the close ran.
    net::post(impl_->ioc, [impl = impl_.get()] {
        beast::error_code ignored;
        impl->acceptor->close(ignored);
        impl->ioc.stop();
    });
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->boundPort = 0;
}

uint16_t MetricsEndpoint::port() const noexcept { return impl_->boundPort.load(); }

uint64_t MetricsEndpoint::scrapes() const noexcept { return impl_->scrapes.load(); }

}  // namespace codafb
