// cli.hpp
// Declares the command-line interface: the struct that holds every option
// the user can set, the parser that fills it, and the --help text.
// Implemented in cli.cpp.

#pragma once

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "resolver.hpp"

namespace netpulse {

/// Thrown when the command line cannot be understood.
// main() catches it, prints the message plus a hint to try --help, and
// exits with code 2.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// All options in one place, with their default values. parse_cli() only
// changes the fields the user actually mentioned.
struct CliOptions {
    // The one positional argument.
    std::string host;
    /// Number of echo requests; 0 means "until interrupted".
    std::size_t count = 4;
    // -i: pause between requests. -W: how long to wait for each reply.
    std::chrono::milliseconds interval{1000};
    std::chrono::milliseconds timeout{2000};
    // -s: bytes of data after the ICMP header (56 + 8 = the classic 64).
    std::size_t payload_size = 56;
    /// 0 keeps the system default.
    int ttl = 0;
    // -4 / -6
    FamilyPreference family = FamilyPreference::Any;
    // -q: print only the summary.
    bool quiet = false;
    // -h / -V: print and exit without pinging anything.
    bool show_help = false;
    bool show_version = false;
};

// Upper limit for -s. An IP packet can be at most 65535 bytes, and the IP
// and ICMP headers need some of that.
inline constexpr std::size_t kMaxPayloadSize = 65500;

/// Parses the arguments that follow the program name.
// Takes a vector of strings rather than argc/argv so the tests can call it
// with literal lists like {"-c", "3", "host"}.
CliOptions parse_cli(const std::vector<std::string>& args);

/// The text printed for --help.
std::string usage_text();

}  // namespace netpulse