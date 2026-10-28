/**
 * Tests for ejfat_uri_config: independent parsing of the receive and send
 * URIs, per-role validation, the same-LB guard, and that no token ever
 * appears in error messages or descriptions.
 */

#include <string>

#include "ejfat_uri_config.hpp"
#include "test_harness.hpp"

using namespace codafb;

namespace {
const std::string kRecvToken = "RECVsecretTOKEN111";
const std::string kSendToken = "SENDsecretTOKEN222";

const std::string kRecvUri =
    "ejfats://" + kRecvToken + "@192.0.2.10:18347/lb/11?data=192.0.2.20&sync=192.0.2.20:19010";
const std::string kSendUri =
    "ejfats://" + kSendToken + "@198.51.100.10:18347/lb/22?data=198.51.100.20:19522&sync=198.51.100.20:19010";

bool hasToken(const std::string& s) {
    return s.find(kRecvToken) != std::string::npos || s.find(kSendToken) != std::string::npos;
}
}  // namespace

TEST(receive_and_send_uris_parse_independently) {
    auto r = parseEjfatUri("--recv-ejfat-uri", kRecvUri, UriRole::Receive, true, false);
    auto s = parseEjfatUri("--send-ejfat-uri", kSendUri, UriRole::Send, true, false);
    CHECK(!r.has_error());
    CHECK(!s.has_error());
    if (r.has_error() || s.has_error()) return;

    CHECK_EQ(r.value().get_lbId(), std::string("11"));
    CHECK_EQ(s.value().get_lbId(), std::string("22"));
    CHECK_EQ(r.value().get_InstanceToken().value(), kRecvToken);
    CHECK_EQ(s.value().get_InstanceToken().value(), kSendToken);
    CHECK_EQ(r.value().get_dataAddrv4().value().first.to_string(), std::string("192.0.2.20"));
    CHECK_EQ(s.value().get_dataAddrv4().value().first.to_string(), std::string("198.51.100.20"));
    CHECK_EQ(s.value().get_dataAddrv4().value().second.first, uint16_t{19522});
    CHECK(!checkDistinctLoadBalancers(r.value(), s.value()).has_error());
}

TEST(description_never_contains_the_token) {
    auto s = parseEjfatUri("--send-ejfat-uri", kSendUri, UriRole::Send, true, false);
    CHECK(!s.has_error());
    if (s.has_error()) return;
    std::string d = describeEjfatUri(s.value());
    CHECK(!hasToken(d));
    CHECK(d.find("<token>@198.51.100.10:18347/lb/22") != std::string::npos);
    CHECK(d.find("data=198.51.100.20:19522") != std::string::npos);
    CHECK(d.find("sync=198.51.100.20:19010") != std::string::npos);
}

TEST(send_requires_ipv4_sync_with_control_plane) {
    std::string noSync = "ejfats://" + kSendToken + "@198.51.100.10:18347/lb/22?data=198.51.100.20";
    auto s = parseEjfatUri("--send-ejfat-uri", noSync, UriRole::Send, true, false);
    CHECK(s.has_error());
    if (s.has_error()) {
        CHECK(s.error().message().find("--send-ejfat-uri") != std::string::npos);
        CHECK(s.error().message().find("sync") != std::string::npos);
        CHECK(!hasToken(s.error().message()));
    }
    // without the control plane only the data address is used
    CHECK(!parseEjfatUri("--send-ejfat-uri", noSync, UriRole::Send, false, false).has_error());
    // the receive side never needs sync
    CHECK(!parseEjfatUri("--recv-ejfat-uri", noSync, UriRole::Receive, true, false).has_error());
}

TEST(data_address_required_for_both_roles) {
    std::string noData = "ejfats://" + kSendToken + "@198.51.100.10:18347/lb/22?sync=198.51.100.20:19010";
    auto s = parseEjfatUri("--send-ejfat-uri", noData, UriRole::Send, true, false);
    auto r = parseEjfatUri("--recv-ejfat-uri", noData, UriRole::Receive, true, false);
    CHECK(s.has_error());
    CHECK(r.has_error());
    if (s.has_error()) CHECK(s.error().message().find("data=") != std::string::npos);
    if (r.has_error()) CHECK(r.error().message().find("--recv-ejfat-uri") != std::string::npos);
}

TEST(send_ipv6_requires_ipv6_data_address) {
    auto s = parseEjfatUri("--send-ejfat-uri", kSendUri, UriRole::Send, true, false, /*dataV6=*/true);
    CHECK(s.has_error());
    if (s.has_error()) CHECK(s.error().message().find("IPv6") != std::string::npos);
}

TEST(control_plane_needs_token_and_lb_id) {
    std::string noToken = "ejfats://198.51.100.10:18347/lb/22?data=198.51.100.20&sync=198.51.100.20:19010";
    auto a = parseEjfatUri("--send-ejfat-uri", noToken, UriRole::Send, true, false);
    CHECK(a.has_error());
    if (a.has_error()) CHECK(a.error().message().find("instance token") != std::string::npos);

    std::string noLb = "ejfats://" + kRecvToken + "@192.0.2.10:18347/?data=192.0.2.20";
    auto b = parseEjfatUri("--recv-ejfat-uri", noLb, UriRole::Receive, true, false);
    CHECK(b.has_error());
    if (b.has_error()) {
        CHECK(b.error().message().find("LB id") != std::string::npos);
        CHECK(!hasToken(b.error().message()));
    }
    // without the control plane neither is needed
    CHECK(!parseEjfatUri("--recv-ejfat-uri", noLb, UriRole::Receive, false, false).has_error());
}

TEST(parse_errors_are_redacted) {
    // E2SAR's own message for these embeds the raw URI, token included
    std::string badScheme = "http://" + kSendToken + "@198.51.100.10:18347/lb/22?data=198.51.100.20";
    std::string badData = "ejfats://" + kSendToken + "@198.51.100.10:18347/lb/22?data=not-an-ip";
    for (const auto& bad : {badScheme, badData}) {
        auto s = parseEjfatUri("--send-ejfat-uri", bad, UriRole::Send, true, false);
        CHECK(s.has_error());
        if (s.has_error()) {
            CHECK(s.error().message().find("--send-ejfat-uri") != std::string::npos);
            CHECK(s.error().message().find(kSendToken) == std::string::npos);
        }
    }
    CHECK(parseEjfatUri("--send-ejfat-uri", "", UriRole::Send, true, false).has_error());
}

TEST(redact_credentials_text) {
    CHECK_EQ(redactCredentials("x ejfats://abc@h:1/lb/2 y"), std::string("x ejfats://<redacted>@h:1/lb/2 y"));
    CHECK_EQ(redactCredentials("EJFAT://abc@h"), std::string("EJFAT://<redacted>@h"));
    CHECK_EQ(redactCredentials("a ejfat://t1@h b ejfats://t2@g"),
             std::string("a ejfat://<redacted>@h b ejfats://<redacted>@g"));
    CHECK_EQ(redactCredentials("ejfat://h:1/lb/2?data=1.2.3.4"), std::string("ejfat://h:1/lb/2?data=1.2.3.4"));
    CHECK_EQ(redactCredentials("no uri here"), std::string("no uri here"));
    CHECK_EQ(redactCredentials("scheme http in URI http://tok@h:1/lb/2"),
             std::string("scheme http in URI http://<redacted>@h:1/lb/2"));
    CHECK_EQ(redactCredentials("user@example.org"), std::string("user@example.org"));
}

TEST(same_load_balancer_is_refused) {
    std::string sameLbOtherToken =
        "ejfats://" + kSendToken + "@192.0.2.10:18347/lb/11?data=192.0.2.20&sync=192.0.2.20:19010";
    auto r = parseEjfatUri("--recv-ejfat-uri", kRecvUri, UriRole::Receive, true, false);
    auto s = parseEjfatUri("--send-ejfat-uri", sameLbOtherToken, UriRole::Send, true, false);
    CHECK(!r.has_error() && !s.has_error());
    if (r.has_error() || s.has_error()) return;
    auto d = checkDistinctLoadBalancers(r.value(), s.value());
    CHECK(d.has_error());
    if (d.has_error()) {
        CHECK(d.error().message().find("same load balancer") != std::string::npos);
        CHECK(!hasToken(d.error().message()));
    }

    // same LB id on a different control plane is a different instance
    std::string otherCp =
        "ejfats://" + kSendToken + "@198.51.100.10:18347/lb/11?data=198.51.100.20&sync=198.51.100.20:19010";
    auto s2 = parseEjfatUri("--send-ejfat-uri", otherCp, UriRole::Send, true, false);
    CHECK(!s2.has_error());
    if (!s2.has_error()) CHECK(!checkDistinctLoadBalancers(r.value(), s2.value()).has_error());
}

int main() { return testing::runAllTests(); }
