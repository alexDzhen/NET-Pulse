// report.cpp
// Builds every line of text that ping prints: the opening banner, one line
// per echo request, and the closing statistics block. All functions return
// strings without a trailing newline; main() decides where they go.
// Keeping the formatting here (and not in main) means it can be unit-tested
// without sending any packets.

#include "../include/report.hpp"

#include <cmath>
#include <cstdio>

#include "../include/icmp.hpp"

namespace netpulse {

namespace {

// Prints a double with exactly `decimals` digits after the point,
// e.g. format_fixed(0.0451, 3) -> "0.045".
std::string format_fixed(double value, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

// Formats packet loss for the summary.
// Whole numbers are printed without decimals ("25%"), others with one
// decimal ("33.3%"). Very small or very large losses are shown as "<0.1%"
// or ">99.9%" so that one lost packet in ten thousand does not round to
// "0%" and look like a perfect run.
std::string format_percent(double percent) {
    if (percent > 0.0 && percent < 0.1) {
        return "<0.1%";
    }
    if (percent < 100.0 && percent > 99.9) {
        return ">99.9%";
    }
    const double rounded = std::round(percent * 10.0) / 10.0;
    const bool whole = std::fabs(rounded - std::round(rounded)) < 1e-9;
    return format_fixed(rounded, whole ? 0 : 1) + "%";
}

}  // namespace

// Round-trip time with a precision that depends on its size, so that the
// value always shows about three significant digits:
//   0.045 ms, 1.23 ms, 11.4 ms, 250 ms.
std::string format_milliseconds(double milliseconds) {
    if (milliseconds < 1.0) {
        return format_fixed(milliseconds, 3);
    }
    if (milliseconds < 10.0) {
        return format_fixed(milliseconds, 2);
    }
    if (milliseconds < 100.0) {
        return format_fixed(milliseconds, 1);
    }
    return format_fixed(milliseconds, 0);
}

// Banner printed before the first request, e.g.
//   PING example.com (93.184.216.34): 56 data bytes
std::string format_header(const std::string& host, const IpAddress& target,
                          std::size_t payload_size) {
    return "PING " + host + " (" + target.to_string() + "): " + std::to_string(payload_size) +
           " data bytes";
}

// One line describing what happened to a single echo request.
// `family` is needed because the wording of errors differs between
// IPv4 and IPv6 (e.g. "time to live" versus "hop limit").
std::string format_result(const EchoResult& result, std::uint16_t sequence, Family family) {
    // Pieces shared by several branches below.
    const std::string seq = "icmp_seq=" + std::to_string(sequence);
    // "From <router>: " is used for errors, which are reported by whichever
    // router dropped the packet, not by the target itself.
    const std::string from =
        result.responder ? "From " + result.responder->to_string() + ": " : std::string{};

    switch (result.status) {
        case EchoStatus::Reply: {
            // Normal case, in the classic ping layout:
            //   64 bytes from 10.0.3.2: icmp_seq=1 ttl=62 time=0.058 ms
            // The ttl is omitted when the backend could not report it (-1).
            std::string line = std::to_string(result.bytes) + " bytes from " +
                               (result.responder ? result.responder->to_string() : "?") + ": " +
                               seq;
            if (result.ttl >= 0) {
                line += " ttl=" + std::to_string(result.ttl);
            }
            line += " time=" + format_milliseconds(result.round_trip.count()) + " ms";
            return line;
        }
        case EchoStatus::TimeExceeded:
            // A router dropped the packet because its TTL reached zero.
            return from + seq +
                   (family == Family::IPv4 ? " Time to live exceeded" : " Hop limit exceeded");
        case EchoStatus::DestinationUnreachable:
            // The ICMP code says why (host, network, port, prohibited...).
            return from + seq + " " + icmp::describe_unreachable(family, result.code);
        case EchoStatus::Timeout:
            // Nothing came back within the timeout.
            return "Request timed out: " + seq;
        case EchoStatus::Interrupted:
            // Ctrl+C was pressed while waiting for this reply.
            return "Interrupted: " + seq;
        case EchoStatus::Error:
            // The request could not even be sent (e.g. "Network is unreachable").
            return "Error: " + seq + " " + result.error;
    }
    return {};
}

// Closing block printed at the end, e.g.
//   --- example.com ping statistics ---
//   4 packets transmitted, 4 received, 0% packet loss
//   round-trip min/avg/max/stddev = 0.058/0.070/0.087/0.010 ms
// The timing line is left out when no reply arrived, since there is nothing
// to measure.
std::string format_summary(const std::string& host, const Statistics& statistics) {
    std::string text = "--- " + host + " ping statistics ---\n";
    text += std::to_string(statistics.transmitted()) + " packets transmitted, " +
            std::to_string(statistics.received()) + " received, " +
            format_percent(statistics.loss_percent()) + " packet loss";
    if (statistics.received() > 0) {
        text += "\nround-trip min/avg/max/stddev = " + format_fixed(statistics.min_ms(), 3) + "/" +
                format_fixed(statistics.average_ms(), 3) + "/" +
                format_fixed(statistics.max_ms(), 3) + "/" +
                format_fixed(statistics.stddev_ms(), 3) + " ms";
    }
    return text;
}

}  // namespace netpulse