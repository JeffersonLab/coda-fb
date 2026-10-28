/**
 * EJFAT URI handling - see ejfat_uri_config.hpp
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#include "ejfat_uri_config.hpp"

#include <regex>
#include <sstream>

using namespace e2sar;

namespace codafb {

std::string redactCredentials(const std::string& text) {
    // userinfo is everything between "://" and the first '@' before any '/'.
    // Any scheme, not just ejfat[s]: E2SAR echoes a mistyped one (e.g. http://)
    // back in its "invalid scheme" message, token included.
    static const std::regex credentials(R"(([A-Za-z][A-Za-z0-9+.\-]*://)[^@/\s]*@)");
    return std::regex_replace(text, credentials, "$1<redacted>@");
}

namespace {

std::string cpEndpoint(const EjfatURI& uri) {
    auto host = uri.get_cpHost();
    if (!host.has_error())
        return host.value().first + ":" + std::to_string(host.value().second);
    auto addr = uri.get_cpAddr();
    if (addr.has_error())
        return "<none>";
    const auto& ip = addr.value().first;
    return (ip.is_v6() ? "[" + ip.to_string() + "]" : ip.to_string()) + ":" + std::to_string(addr.value().second);
}

}  // namespace

std::string describeEjfatUri(const EjfatURI& uri) {
    std::ostringstream os;
    os << (uri.get_useTls() ? "ejfats://" : "ejfat://");
    if (!uri.get_InstanceToken().has_error() || !uri.get_AdminToken().has_error() ||
        !uri.get_SessionToken().has_error())
        os << "<token>@";
    os << cpEndpoint(uri);
    if (!uri.get_lbId().empty())
        os << "/lb/" << uri.get_lbId();
    if (uri.has_dataAddrv4()) {
        auto d = uri.get_dataAddrv4().value();
        os << " data=" << d.first.to_string() << ":" << d.second.first;
    }
    if (uri.has_dataAddrv6()) {
        auto d = uri.get_dataAddrv6().value();
        os << " data=[" << d.first.to_string() << "]:" << d.second.first;
    }
    if (uri.has_syncAddrv4()) {
        auto s = uri.get_syncAddrv4().value();
        os << " sync=" << s.first.to_string() << ":" << s.second;
    }
    if (uri.has_syncAddrv6()) {
        auto s = uri.get_syncAddrv6().value();
        os << " sync=[" << s.first.to_string() << "]:" << s.second;
    }
    return os.str();
}

result<EjfatURI> parseEjfatUri(const std::string& optionName,
                               const std::string& raw,
                               UriRole role,
                               bool useCP,
                               bool preferV6,
                               bool dataV6) {
    const std::string side = (role == UriRole::Receive) ? "receive" : "send";

    if (raw.empty())
        return E2SARErrorInfo{E2SARErrorc::ParameterError, optionName + " is empty"};

    // Instance token: registerWorker(), addSenders() and removeSenders() all use it.
    auto parsed = EjfatURI::getFromString(raw, EjfatURI::TokenType::instance, preferV6);
    if (parsed.has_error())
        return E2SARErrorInfo{E2SARErrorc::ParseError,
            optionName + ": not a valid EJFAT URI (" + redactCredentials(parsed.error().message()) +
            "). Expected ejfat[s]://<instance-token>@<cp-host>:<cp-port>/lb/<lb-id>?data=<ip>[:<port>]&sync=<ip>:<port>"};
    EjfatURI uri = parsed.value();

    if (role == UriRole::Send) {
        if (dataV6 ? !uri.has_dataAddrv6() : !uri.has_dataAddrv4())
            return E2SARErrorInfo{E2SARErrorc::ParameterNotAvailable,
                optionName + ": no " + (dataV6 ? "IPv6" : "IPv4") +
                " data= address, which is where the frames are sent. Use the instance URI "
                "returned by the reservation of the second load balancer" +
                (dataV6 ? "" : ", or pass --send-ipv6 if it only has an IPv6 data address")};
    } else if (!uri.has_dataAddr()) {
        return E2SARErrorInfo{E2SARErrorc::ParameterNotAvailable,
            optionName + ": no data= address. Use the instance URI returned by the reservation of the first load balancer"};
    }

    if (useCP) {
        if (uri.get_InstanceToken().has_error())
            return E2SARErrorInfo{E2SARErrorc::ParameterNotAvailable,
                optionName + ": no instance token; the " + side + " side needs one to " +
                (role == UriRole::Receive ? "register as a worker" : "register as a sender")};
        if (uri.get_lbId().empty())
            return E2SARErrorInfo{E2SARErrorc::ParameterNotAvailable,
                optionName + ": no LB id (/lb/<id>); the " + side + " side needs the reserved instance URI, not the base URI"};
        if (role == UriRole::Send && !uri.has_syncAddrv4())
            return E2SARErrorInfo{E2SARErrorc::ParameterNotAvailable,
                optionName + ": no IPv4 sync= address; the sender reports its event rate there"};
    }

    return uri;
}

result<int> checkDistinctLoadBalancers(const EjfatURI& recvUri, const EjfatURI& sendUri) {
    if (recvUri.get_lbId().empty() || sendUri.get_lbId().empty())
        return 0;
    if (recvUri.get_lbId() == sendUri.get_lbId() && cpEndpoint(recvUri) == cpEndpoint(sendUri))
        return E2SARErrorInfo{E2SARErrorc::ParameterError,
            "--send-ejfat-uri and --recv-ejfat-uri name the same load balancer instance (lb " +
            recvUri.get_lbId() + " at " + cpEndpoint(recvUri) +
            "); forwarded frames would loop back to this worker. Reserve a second LB instance for sending"};
    return 0;
}

}  // namespace codafb
