// icmp.hpp
// Declares the ICMP wire-format helpers: the protocol constants, the
// checksum, building an echo request and parsing what comes back.
// Nothing here touches a socket, which is what makes this part fully
// unit-testable. Used by the Linux backend (the Windows API builds packets
// itself). Implemented in icmp.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ip_address.hpp"

/// The ICMP wire format (RFC 792 for IPv4, RFC 4443 for IPv6).
/// Everything here is plain byte manipulation with no system calls.
namespace netpulse::icmp {

// Every ICMP message starts with an 8-byte header:
// type (1), code (1), checksum (2), identifier (2), sequence (2).
inline constexpr std::size_t kHeaderSize = 8;

// ICMP message types we send or recognise. IPv4 and IPv6 use different
// numbers for the same things, hence two sets of constants.
// `inline constexpr` (C++17) lets a constant be defined in a header and
// included from several .cpp files without "multiple definition" errors.
inline constexpr std::uint8_t kV4EchoReply = 0;
inline constexpr std::uint8_t kV4DestinationUnreachable = 3;
inline constexpr std::uint8_t kV4EchoRequest = 8;
inline constexpr std::uint8_t kV4TimeExceeded = 11;

inline constexpr std::uint8_t kV6DestinationUnreachable = 1;
inline constexpr std::uint8_t kV6TimeExceeded = 3;
inline constexpr std::uint8_t kV6EchoRequest = 128;
inline constexpr std::uint8_t kV6EchoReply = 129;

/// The Internet checksum (RFC 1071): the one's complement of the one's
/// complement sum of the data taken as big-endian 16-bit words.
/// Running it over a message that already carries a correct checksum gives 0.
std::uint16_t checksum(const std::uint8_t* data, std::size_t size) noexcept;

/// Builds an echo request: 8-byte header followed by `payload_size` bytes.
/// For IPv4 the checksum is filled in. For IPv6 it is left at zero because it
/// covers a pseudo-header and the kernel always computes it.
// `identifier` tells our packets apart from other programs'; `sequence`
// tells which request a reply belongs to. The target copies both back.
std::vector<std::uint8_t> build_echo_request(Family family, std::uint16_t identifier,
                                             std::uint16_t sequence, std::size_t payload_size);

// The three kinds of message that matter to ping. Everything else on the
// wire (redirects, router advertisements, ...) is ignored by parse_message.
enum class MessageKind {
    EchoReply,
    TimeExceeded,
    DestinationUnreachable,
};

/// The parts of a received ICMP message that matter to ping.
struct Message {
    MessageKind kind = MessageKind::EchoReply;
    // The raw type and code bytes from the header. `code` says why a
    // "destination unreachable" message was sent (see describe_unreachable).
    std::uint8_t type = 0;
    std::uint8_t code = 0;
    /// Identifier and sequence of the echo request this message answers.
    /// For error messages they come from the quoted original packet.
    std::uint16_t identifier = 0;
    std::uint16_t sequence = 0;
};

/// Parses an ICMP message that starts at the ICMP header (no IP header).
/// Returns nothing for messages that are not an echo reply or an error
/// about one of our echo requests.
// Returning std::optional instead of throwing: an unrecognised packet is a
// normal event on a raw socket, not an error.
std::optional<Message> parse_message(Family family, const std::uint8_t* data, std::size_t size);

/// Length of the IPv4 header at the start of `packet`, if it is a valid one.
// 20 bytes normally, up to 60 with options. Needed because a raw IPv4
// socket delivers the IP header together with the ICMP message.
std::optional<std::size_t> ipv4_header_size(const std::uint8_t* packet, std::size_t size) noexcept;

/// Text for the code of a "destination unreachable" message.
// e.g. IPv4 code 1 -> "Destination host unreachable".
std::string describe_unreachable(Family family, int code);

}  // namespace netpulse::icmp