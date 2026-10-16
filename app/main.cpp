// main.cpp
// Entry point of netpulse. Ties the other modules together:
//   parse the command line (cli)  ->  resolve the host (resolver)
//   -> open the ICMP transport (icmp_client)  ->  loop: send, wait, print
//   (report) and count (statistics)  ->  print the summary and exit.
// It also handles Ctrl+C so the summary is printed even when the user
// stops an endless run.

#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../include/cli.hpp"
#include "../include/icmp_client.hpp"
#include "report.hpp"
#include "resolver.hpp"
#include "statistics.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// Set to 1 by the signal handler when Ctrl+C (SIGINT) or a kill (SIGTERM)
// arrives. sig_atomic_t + volatile is the one combination the C++ standard
// guarantees is safe to write from a signal handler and read elsewhere.
volatile std::sig_atomic_t g_stop_requested = 0;

// A signal handler may do almost nothing safely (no printing, no memory
// allocation), so it only raises the flag; the main loop checks the flag
// and does the actual stopping.
void handle_stop_signal(int) {
    g_stop_requested = 1;
}

// Waits until `wake_time` (the moment the next request is due), but in
// small 50 ms slices so that a Ctrl+C during the pause is noticed quickly
// instead of after a full interval.
void sleep_until(Clock::time_point wake_time) {
    constexpr std::chrono::milliseconds kSlice{50};
    while (g_stop_requested == 0) {
        const Clock::time_point now = Clock::now();
        if (now >= wake_time) {
            return;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(wake_time - now);
        std::this_thread::sleep_for(remaining < kSlice ? remaining + std::chrono::milliseconds{1}
                                                       : kSlice);
    }
}

// The ping session itself. Returns the process exit code.
int ping(const netpulse::CliOptions& options) {
    // Socket library setup (Windows), name resolution, and the OS-specific
    // transport. Any of these may throw; main() reports the error.
    const netpulse::NetworkSession session;
    const netpulse::IpAddress target = netpulse::resolve(options.host, options.family);
    const std::unique_ptr<netpulse::IcmpClient> client = netpulse::IcmpClient::create(target);

    // Handlers are installed only now, after setup, so a Ctrl+C during a
    // slow DNS lookup still kills the program the normal way.
    // The client is given the same flag so it can abandon a wait early.
    std::signal(SIGINT, handle_stop_signal);
    std::signal(SIGTERM, handle_stop_signal);
    client->set_interrupt_flag(&g_stop_requested);

    std::cout << netpulse::format_header(options.host, target, options.payload_size) << std::endl;

    netpulse::Statistics statistics;
    Clock::time_point next_send = Clock::now();

    // Main loop: one echo request per iteration. count == 0 means forever
    // (until Ctrl+C).
    for (std::size_t sent = 0; options.count == 0 || sent < options.count; ++sent) {
        // Keep the requests evenly spaced: the next send time is fixed
        // before this request goes out, so a slow reply does not push the
        // following requests later.
        sleep_until(next_send);
        if (g_stop_requested != 0) {
            break;
        }
        next_send = Clock::now() + options.interval;

        // Sequence numbers start at 1 (as in the classic ping) and wrap at
        // 16 bits, the size of the field in the ICMP header.
        netpulse::EchoRequest request;
        request.sequence = static_cast<std::uint16_t>((sent + 1) & 0xFFFFu);
        request.payload_size = options.payload_size;
        request.ttl = options.ttl;
        request.timeout = options.timeout;

        // Blocks until a reply, an error, the timeout or an interrupt.
        const netpulse::EchoResult result = client->echo(request);
        if (result.status == netpulse::EchoStatus::Reply) {
            statistics.record_reply(result.round_trip.count());
        } else {
            statistics.record_loss();
        }
        // An interrupted request is counted as sent (above) but not printed.
        if (result.status == netpulse::EchoStatus::Interrupted) {
            break;
        }
        if (!options.quiet) {
            std::cout << netpulse::format_result(result, request.sequence, target.family())
                      << std::endl;
        }
    }

    // Printed on every exit path from the loop, including Ctrl+C.
    // Exit code 0 only if at least one reply arrived, so scripts can test
    // "is the host up?" with the exit status.
    std::cout << "\n" << netpulse::format_summary(options.host, statistics) << std::endl;
    return statistics.received() > 0 ? 0 : 1;
}

// Parses the arguments and dispatches: help, version, or a ping session.
// A bad command line is reported with the conventional exit code 2.
int run(const std::vector<std::string>& args) {
    netpulse::CliOptions options;
    try {
        options = netpulse::parse_cli(args);
    } catch (const netpulse::UsageError& error) {
        std::cerr << "netpulse: " << error.what()
                  << "\nTry 'netpulse --help' for more information.\n";
        return 2;
    }

    if (options.show_help) {
        std::cout << netpulse::usage_text();
        return 0;
    }
    if (options.show_version) {
        // NETPULSE_VERSION is defined by CMake from the project version.
        std::cout << "netpulse " << NETPULSE_VERSION << "\n";
        return 0;
    }
    return ping(options);
}

}  // namespace

// argv[0] is the program name, so the arguments proper are argv[1..argc).
// Every remaining exception (ResolveError, NetworkError, ...) ends up here
// and becomes a one-line message and exit code 2.
int main(int argc, char* argv[]) {
    try {
        return run(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::exception& error) {
        std::cerr << "netpulse: " << error.what() << "\n";
        return 2;
    }
}