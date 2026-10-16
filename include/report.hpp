// report.hpp
// Declares the functions that turn results into the text ping prints.
// They only build strings (no printing, no trailing newline), which keeps
// the output format in one place and lets the tests check it exactly
// without touching the network. Implemented in report.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "icmp_client.hpp"
#include "ip_address.hpp"
#include "statistics.hpp"

namespace netpulse {

/// Milliseconds with a precision that depends on the magnitude,
/// e.g. "0.045", "1.23", "11.4", "250".
std::string format_milliseconds(double milliseconds);

/// The banner printed before the first request.
// e.g. "PING example.com (93.184.216.34): 56 data bytes"
std::string format_header(const std::string& host, const IpAddress& target,
                          std::size_t payload_size);

/// One line describing the outcome of a single echo request.
// Covers every EchoStatus: a reply with its size, ttl and time; an error
// reported by a router ("From 10.0.1.1: ... Time to live exceeded"); a
// timeout; an interrupt; or a send failure. `family` picks IPv4 or IPv6
// wording for the error messages.
std::string format_result(const EchoResult& result, std::uint16_t sequence, Family family);

/// The closing statistics block.
// Two or three lines: the "--- host ping statistics ---" title, the packet
// counts with loss percentage, and (only if something was received) the
// min/avg/max/stddev line.
std::string format_summary(const std::string& host, const Statistics& statistics);

}  // namespace netpulse