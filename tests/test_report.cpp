// test_report.cpp
// Unit tests for the output formatting (report.cpp). Each test builds an
// EchoResult or Statistics object by hand and compares the produced text
// character for character, so any accidental change to the output format
// is caught.

#include <doctest.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "../include/report.hpp"

using netpulse::EchoResult;
using netpulse::EchoStatus;
using netpulse::Family;
using netpulse::IpAddress;
using netpulse::Statistics;

// One case per precision band: 3 decimals below 1 ms, 2 below 10, 1 below
// 100, none above. Note 0.9994 stays "0.999" (truncated by rounding, not
// pushed into the next band).
TEST_CASE("milliseconds are printed with magnitude-dependent precision") {
    CHECK(netpulse::format_milliseconds(0.0451) == "0.045");
    CHECK(netpulse::format_milliseconds(0.9994) == "0.999");
    CHECK(netpulse::format_milliseconds(1.234) == "1.23");
    CHECK(netpulse::format_milliseconds(11.44) == "11.4");
    CHECK(netpulse::format_milliseconds(250.4) == "250");
    CHECK(netpulse::format_milliseconds(0.0) == "0.000");
}

TEST_CASE("the header names the host, its address and the payload size") {
    CHECK(netpulse::format_header("example.com", IpAddress::ipv4(93, 184, 216, 34), 56) ==
          "PING example.com (93.184.216.34): 56 data bytes");
}

// The normal reply line, and the same line when the backend could not
// report a TTL (-1): the "ttl=" part must simply disappear.
TEST_CASE("a reply line shows size, source, sequence, ttl and time") {
    EchoResult result;
    result.status = EchoStatus::Reply;
    result.responder = IpAddress::ipv4(10, 0, 3, 2);
    result.bytes = 64;
    result.ttl = 62;
    result.round_trip = std::chrono::duration<double, std::milli>{11.44};

    CHECK(netpulse::format_result(result, 3, Family::IPv4) ==
          "64 bytes from 10.0.3.2: icmp_seq=3 ttl=62 time=11.4 ms");

    result.ttl = -1;
    CHECK(netpulse::format_result(result, 3, Family::IPv4) ==
          "64 bytes from 10.0.3.2: icmp_seq=3 time=11.4 ms");
}

// Router errors: "From <router>:" prefix, IPv4 vs IPv6 wording for an
// expired TTL, the unreachable code turned into text, and the prefix
// dropped when the responder is unknown.
TEST_CASE("error lines name the router that reported the problem") {
    EchoResult exceeded;
    exceeded.status = EchoStatus::TimeExceeded;
    exceeded.responder = IpAddress::ipv4(10, 0, 1, 1);
    CHECK(netpulse::format_result(exceeded, 1, Family::IPv4) ==
          "From 10.0.1.1: icmp_seq=1 Time to live exceeded");

    // fd00:1::1 written out as bytes: fd 00 00 01 00 ... 00 01.
    std::uint8_t router_v6[16] = {0xfd, 0x00, 0x00, 0x01};
    router_v6[15] = 1;
    exceeded.responder = IpAddress::from_bytes(Family::IPv6, router_v6);
    CHECK(netpulse::format_result(exceeded, 1, Family::IPv6) ==
          "From fd00:1::1: icmp_seq=1 Hop limit exceeded");

    EchoResult unreachable;
    unreachable.status = EchoStatus::DestinationUnreachable;
    unreachable.responder = IpAddress::ipv4(10, 0, 2, 2);
    unreachable.code = 1;
    CHECK(netpulse::format_result(unreachable, 2, Family::IPv4) ==
          "From 10.0.2.2: icmp_seq=2 Destination host unreachable");

    unreachable.responder.reset();
    CHECK(netpulse::format_result(unreachable, 2, Family::IPv4) ==
          "icmp_seq=2 Destination host unreachable");
}

// A default-constructed EchoResult is a timeout; an Error carries its own
// message text.
TEST_CASE("timeouts and send errors have their own lines") {
    EchoResult timeout;
    CHECK(netpulse::format_result(timeout, 7, Family::IPv4) == "Request timed out: icmp_seq=7");

    EchoResult error;
    error.status = EchoStatus::Error;
    error.error = "cannot send: Network is unreachable";
    CHECK(netpulse::format_result(error, 8, Family::IPv4) ==
          "Error: icmp_seq=8 cannot send: Network is unreachable");
}

// Full three-line summary. With samples 1, 2, 3 the mean is 2 and the
// population standard deviation is sqrt(2/3) = 0.816.
TEST_CASE("the summary reports loss and timing") {
    Statistics statistics;
    statistics.record_reply(1.0);
    statistics.record_reply(2.0);
    statistics.record_reply(3.0);
    statistics.record_loss();

    CHECK(netpulse::format_summary("example.com", statistics) ==
          "--- example.com ping statistics ---\n"
          "4 packets transmitted, 3 received, 25% packet loss\n"
          "round-trip min/avg/max/stddev = 1.000/2.000/3.000/0.816 ms");
}

// With no replies there are no timings, so the third line must be absent.
TEST_CASE("the summary omits timing when nothing was received") {
    Statistics statistics;
    statistics.record_loss();
    statistics.record_loss();
    statistics.record_loss();

    CHECK(netpulse::format_summary("10.0.0.9", statistics) ==
          "--- 10.0.0.9 ping statistics ---\n"
          "3 packets transmitted, 0 received, 100% packet loss");
}

// 1 lost out of 3 is 33.333...%, shown with one decimal.
TEST_CASE("fractional loss keeps one decimal") {
    Statistics statistics;
    statistics.record_reply(1.0);
    statistics.record_reply(1.0);
    statistics.record_loss();

    const std::string summary = netpulse::format_summary("h", statistics);
    CHECK(summary.find("33.3% packet loss") != std::string::npos);
}

// 1 lost in 10000 is 0.01%, which plain rounding would print as "0%" and
// hide the loss; the formatter must say "<0.1%" instead (and ">99.9%" for
// the mirror case).
TEST_CASE("tiny and near-total loss are not rounded to 0% or 100%") {
    Statistics mostly_fine;
    mostly_fine.record_loss();
    for (int i = 0; i < 9999; ++i) {
        mostly_fine.record_reply(1.0);
    }
    CHECK(netpulse::format_summary("h", mostly_fine).find(", <0.1% packet loss") !=
          std::string::npos);

    Statistics mostly_lost;
    mostly_lost.record_reply(1.0);
    for (int i = 0; i < 9999; ++i) {
        mostly_lost.record_loss();
    }
    CHECK(netpulse::format_summary("h", mostly_lost).find(", >99.9% packet loss") !=
          std::string::npos);
}