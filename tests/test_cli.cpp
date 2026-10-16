// test_cli.cpp
// Unit tests for the command-line parser (cli.cpp), written with doctest.
// Each TEST_CASE is an independent test; CHECK records a failure and keeps
// going, CHECK_THROWS_AS passes only if the expression throws the given
// exception type. The tests call parse_cli() directly with lists of
// arguments, so no process is started and no network is touched.

#include <doctest.h>

#include <chrono>
#include <initializer_list>
#include <string>

#include "../include/cli.hpp"

using netpulse::FamilyPreference;
using netpulse::UsageError;
using std::chrono::milliseconds;

// With only a host given, every option must have its documented default.
TEST_CASE("a bare host uses the defaults") {
    const auto options = netpulse::parse_cli({"example.com"});
    CHECK(options.host == "example.com");
    CHECK(options.count == 4);
    CHECK(options.interval == milliseconds{1000});
    CHECK(options.timeout == milliseconds{2000});
    CHECK(options.payload_size == 56);
    CHECK(options.ttl == 0);
    CHECK(options.family == FamilyPreference::Any);
    CHECK_FALSE(options.quiet);
    CHECK_FALSE(options.show_help);
    CHECK_FALSE(options.show_version);
}

// The "-c 10" form: every short option sets its field, fractional seconds
// become milliseconds, and flags without a value (-4, -q) work too.
TEST_CASE("short options take their value from the next argument") {
    const auto options = netpulse::parse_cli(
        {"-c", "10", "-i", "0.5", "-W", "3", "-s", "100", "-t", "5", "-4", "-q", "10.0.0.1"});
    CHECK(options.host == "10.0.0.1");
    CHECK(options.count == 10);
    CHECK(options.interval == milliseconds{500});
    CHECK(options.timeout == milliseconds{3000});
    CHECK(options.payload_size == 100);
    CHECK(options.ttl == 5);
    CHECK(options.family == FamilyPreference::IPv4);
    CHECK(options.quiet);
}

// Long options work both as "--count 2" and as "--count=2". The values
// chosen here also sit on the edges of the allowed ranges (size 0, ttl 255,
// ttl 1, count 0).
TEST_CASE("long options accept both spellings") {
    const auto spaced = netpulse::parse_cli({"--count", "2", "--interval", "1.5", "--timeout",
                                             "0.25", "--size", "0", "--ttl", "255", "host"});
    CHECK(spaced.count == 2);
    CHECK(spaced.interval == milliseconds{1500});
    CHECK(spaced.timeout == milliseconds{250});
    CHECK(spaced.payload_size == 0);
    CHECK(spaced.ttl == 255);

    const auto joined = netpulse::parse_cli(
        {"--count=0", "--interval=2", "--timeout=10", "--size=1472", "--ttl=1", "--quiet", "host"});
    CHECK(joined.count == 0);
    CHECK(joined.interval == milliseconds{2000});
    CHECK(joined.timeout == milliseconds{10000});
    CHECK(joined.payload_size == 1472);
    CHECK(joined.ttl == 1);
    CHECK(joined.quiet);
}

// The host does not have to come last; "::1" must not be mistaken for an
// option even though it is not a plain word.
TEST_CASE("options may follow the host") {
    const auto options = netpulse::parse_cli({"::1", "-6", "-c", "1"});
    CHECK(options.host == "::1");
    CHECK(options.family == FamilyPreference::IPv6);
    CHECK(options.count == 1);
}

// "netpulse --help" alone must not fail with "missing host".
TEST_CASE("help and version do not need a host") {
    CHECK(netpulse::parse_cli({"-h"}).show_help);
    CHECK(netpulse::parse_cli({"--help"}).show_help);
    CHECK(netpulse::parse_cli({"-V"}).show_version);
    CHECK(netpulse::parse_cli({"--version"}).show_version);
}

// After "--" everything is positional, so a host starting with '-' is
// accepted instead of being reported as an unknown option.
TEST_CASE("a double dash ends option parsing") {
    const auto options = netpulse::parse_cli({"--", "-weird-host"});
    CHECK(options.host == "-weird-host");
}

// No host, options but no host, and two hosts are all rejected.
TEST_CASE("a missing or extra host is a usage error") {
    CHECK_THROWS_AS(netpulse::parse_cli({}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-c", "3"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"a", "b"}), UsageError);
}

// Unknown names, an option at the very end with nothing after it, and a
// value attached to a flag that takes none.
TEST_CASE("unknown options and missing values are usage errors") {
    CHECK_THROWS_AS(netpulse::parse_cli({"--bogus", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-x", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"host", "-c"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"host", "--ttl"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"host", "--quiet=yes"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"host", "--help=1"}), UsageError);
}

// The strict number parsing: negative, non-numeric, trailing garbage,
// empty, fractional where a whole number is needed, and values just
// outside each option's range (ttl 0 and 256, size 65501, interval 0,
// timeout above 3600 s, "nan").
TEST_CASE("out-of-range and malformed numbers are usage errors") {
    CHECK_THROWS_AS(netpulse::parse_cli({"-c", "-1", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-c", "abc", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-c", "3x", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-c", "", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-c", "1.5", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-t", "0", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-t", "256", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-s", "65501", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-s", "-1", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-i", "0", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-i", "fast", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-W", "-2", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-W", "nan", "host"}), UsageError);
    CHECK_THROWS_AS(netpulse::parse_cli({"-W", "99999", "host"}), UsageError);
}

// Guards against adding an option and forgetting to document it.
// CAPTURE makes doctest print which option was being checked if it fails.
TEST_CASE("the usage text mentions every option") {
    const std::string usage = netpulse::usage_text();
    for (const char* option : {"--count", "--interval", "--timeout", "--size", "--ttl", "-4", "-6",
                               "--quiet", "--help", "--version"}) {
        CAPTURE(option);
        CHECK(usage.find(option) != std::string::npos);
    }
}