/**
 * EJFAT URI handling for the receive and send sides of coda-fb
 *
 * coda-fb talks to two independent EJFAT load balancer instances:
 *   --recv-ejfat-uri  LB #1: coda-fb registers as a WORKER and reassembles
 *                     events it receives (E2SAR Reassembler).
 *   --send-ejfat-uri  LB #2: coda-fb registers as a SENDER and forwards built
 *                     frames (E2SAR Segmenter).
 *
 * URIs carry an instance token. Nothing in here ever returns or prints a
 * token: error messages are scrubbed with redactCredentials(), and URIs are
 * displayed with describeEjfatUri().
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#ifndef CODA_FB_EJFAT_URI_CONFIG_HPP
#define CODA_FB_EJFAT_URI_CONFIG_HPP

#include <string>

#include <e2sar.hpp>

namespace codafb {

enum class UriRole { Receive, Send };

/**
 * Replace the credential (userinfo) part of every URI in 'text' with
 * "<redacted>". E2SAR embeds the raw URI, token included, in its parse
 * error messages, so every such message goes through this before printing.
 */
std::string redactCredentials(const std::string& text);

/** Human-readable summary without credentials, e.g. "ejfats://<token>@cp:18347/lb/7 data=10.0.0.1:19522". */
std::string describeEjfatUri(const e2sar::EjfatURI& uri);

/**
 * Parse an EJFAT URI given on option 'optionName' and check that it has
 * what 'role' needs:
 *   Receive: a data address; with the control plane, also an instance
 *            token and an LB id (needed by registerWorker()).
 *   Send:    a data address of the requested family (dataV6); with the
 *            control plane, also an IPv4 sync address (E2SAR's default sync
 *            family), an instance token and an LB id (needed by addSenders()).
 * Errors name the option and say what to fix; they never contain the token.
 */
e2sar::result<e2sar::EjfatURI> parseEjfatUri(const std::string& optionName,
                                              const std::string& raw,
                                              UriRole role,
                                              bool useCP,
                                              bool preferV6,
                                              bool dataV6 = false);

/**
 * Refuse a send URI that names the same LB instance as the receive URI:
 * frames forwarded to LB #1 would be delivered back to its workers.
 */
e2sar::result<int> checkDistinctLoadBalancers(const e2sar::EjfatURI& recvUri,
                                              const e2sar::EjfatURI& sendUri);

}  // namespace codafb

#endif  // CODA_FB_EJFAT_URI_CONFIG_HPP
