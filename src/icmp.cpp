// icmp.cpp
// The ICMP wire format: how an echo request is laid out in bytes, how its
// checksum is computed, and how a received ICMP message is taken apart.
// This file does no I/O at all; it only reads and writes byte arrays, which
// is what makes it easy to unit-test. The socket code lives in the
// platform-specific icmp_client files.
//
// ICMP header (8 bytes, RFC 792 for IPv4, RFC 4443 for IPv6):
//   byte 0      type        (8 = echo request, 0 = echo reply for IPv4;
//                            128 / 129 for IPv6)
//   byte 1      code        (0 for echo; a reason code for error messages)
//   bytes 2-3   checksum
//   bytes 4-5   identifier  (tells our packets apart from other programs')
//   bytes 6-7   sequence    (tells which request a reply belongs to)
//   bytes 8...  payload

#include "../include/icmp.hpp"

namespace netpulse::icmp {

namespace {

// An IPv4 header is at least 20 bytes (longer if it carries options);
// an IPv6 header is always 40 bytes.
constexpr std::size_t kIpv4MinHeaderSize = 20;
constexpr std::size_t kIpv6HeaderSize = 40;
// Protocol numbers that mark "the payload of this IP packet is ICMP".
constexpr std::uint8_t kProtocolIcmp = 1;
constexpr std::uint8_t kNextHeaderIcmpV6 = 58;

// Network protocols store multi-byte numbers big-endian (most significant
// byte first), regardless of the CPU. These two helpers read and write a
// 16-bit value in that order so the code works on any machine.
std::uint16_t read_be16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
}

void write_be16(std::uint8_t* data, std::uint16_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value & 0xFFu);
}

// An ICMP error message ("time exceeded", "destination unreachable") carries
// a copy of the packet that caused it: the original IP header followed by at
// least the first 8 bytes of its payload. `quoted` points at that copy.
// This function checks that the copy really is one of our echo requests and
// returns a pointer to the quoted ICMP header, or nullptr if it is not.
const std::uint8_t* quoted_echo_request(Family family, const std::uint8_t* quoted,
                                        std::size_t size) noexcept {
    if (family == Family::IPv4) {
        // Skip the quoted IPv4 header (variable length), make sure byte 9
        // (the protocol field) says ICMP and that a whole ICMP header follows.
        const auto header_size = ipv4_header_size(quoted, size);
        if (!header_size || quoted[9] != kProtocolIcmp || size < *header_size + kHeaderSize) {
            return nullptr;
        }
        const std::uint8_t* inner = quoted + *header_size;
        return inner[0] == kV4EchoRequest ? inner : nullptr;
    }

    // IPv6: fixed 40-byte header. The high nibble of byte 0 is the version
    // and byte 6 is the "next header" field, which must say ICMPv6.
    if (size < kIpv6HeaderSize + kHeaderSize || (quoted[0] >> 4) != 6 ||
        quoted[6] != kNextHeaderIcmpV6) {
        return nullptr;
    }
    const std::uint8_t* inner = quoted + kIpv6HeaderSize;
    return inner[0] == kV6EchoRequest ? inner : nullptr;
}

}  // namespace

// The Internet checksum from RFC 1071, used by IP, ICMP, TCP and UDP.
// Algorithm:
//   1. Add up the data as 16-bit big-endian words into a 32-bit sum.
//   2. If the length is odd, the last byte is treated as the high byte of a
//      final word padded with zero.
//   3. Fold the carries: add the upper 16 bits back into the lower 16 bits
//      until nothing is left above bit 15 (one's-complement addition).
//   4. Return the bitwise NOT of the result.
// A receiver that runs the same sum over the whole message, checksum field
// included, gets 0 if the message is intact.
std::uint16_t checksum(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t sum = 0;
    std::size_t i = 0;
    for (; i + 1 < size; i += 2) {
        sum += (static_cast<std::uint32_t>(data[i]) << 8) | data[i + 1];
    }
    if (i < size) {
        sum += static_cast<std::uint32_t>(data[i]) << 8;
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return static_cast<std::uint16_t>(~sum & 0xFFFFu);
}

// Builds a complete echo request ready to be sent: the 8-byte header plus
// `payload_size` bytes of data. The payload is filled with 0,1,2,...,255,0,1...
// so a corrupted reply would be noticeable.
// For IPv4 the checksum is computed here, over the whole packet, with the
// checksum field itself set to zero first (which it is, since the vector
// starts zero-filled). For IPv6 the checksum also covers a pseudo-header with
// the source and destination addresses, which only the kernel knows, so the
// field is left at zero and the kernel fills it in.
std::vector<std::uint8_t> build_echo_request(Family family, std::uint16_t identifier,
                                             std::uint16_t sequence, std::size_t payload_size) {
    std::vector<std::uint8_t> packet(kHeaderSize + payload_size, 0);
    packet[0] = family == Family::IPv4 ? kV4EchoRequest : kV6EchoRequest;
    packet[1] = 0;
    write_be16(&packet[4], identifier);
    write_be16(&packet[6], sequence);
    for (std::size_t i = 0; i < payload_size; ++i) {
        packet[kHeaderSize + i] = static_cast<std::uint8_t>(i & 0xFFu);
    }
    if (family == Family::IPv4) {
        write_be16(&packet[2], checksum(packet.data(), packet.size()));
    }
    return packet;
}

// Takes apart a received ICMP message (`data` points at the ICMP header, the
// IP header has already been stripped). Returns the parts ping cares about,
// or nothing if the message is not an echo reply or an error about one of
// our echo requests (routers also send redirects, neighbour discovery, etc.).
std::optional<Message> parse_message(Family family, const std::uint8_t* data, std::size_t size) {
    // Never read past the end of the buffer: an ICMP header is 8 bytes.
    if (size < kHeaderSize) {
        return std::nullopt;
    }

    const bool v4 = family == Family::IPv4;
    Message message;
    message.type = data[0];
    message.code = data[1];

    // Echo reply: identifier and sequence are right there in the header,
    // copied back by the target from our request.
    if (message.type == (v4 ? kV4EchoReply : kV6EchoReply)) {
        message.kind = MessageKind::EchoReply;
        message.identifier = read_be16(data + 4);
        message.sequence = read_be16(data + 6);
        return message;
    }

    // Error messages we understand. Anything else is ignored.
    if (message.type == (v4 ? kV4TimeExceeded : kV6TimeExceeded)) {
        message.kind = MessageKind::TimeExceeded;
    } else if (message.type == (v4 ? kV4DestinationUnreachable : kV6DestinationUnreachable)) {
        message.kind = MessageKind::DestinationUnreachable;
    } else {
        return std::nullopt;
    }

    // For an error, the identifier and sequence come from the quoted copy of
    // our original request, which starts right after the error's own header.
    const std::uint8_t* inner = quoted_echo_request(family, data + kHeaderSize, size - kHeaderSize);
    if (inner == nullptr) {
        return std::nullopt;
    }
    message.identifier = read_be16(inner + 4);
    message.sequence = read_be16(inner + 6);
    return message;
}

// Returns the length of the IPv4 header at the start of `packet`, if there is
// a valid one. Byte 0 holds the version in its high nibble (must be 4) and
// the header length in its low nibble, counted in 32-bit words, so the size
// in bytes is that value times 4 (20 bytes without options, up to 60 with).
// Needed because a raw IPv4 socket delivers packets with the IP header still
// attached, and because error messages quote the original IP header.
std::optional<std::size_t> ipv4_header_size(const std::uint8_t* packet, std::size_t size) noexcept {
    if (size < kIpv4MinHeaderSize || (packet[0] >> 4) != 4) {
        return std::nullopt;
    }
    const std::size_t header_size = static_cast<std::size_t>(packet[0] & 0x0Fu) * 4;
    if (header_size < kIpv4MinHeaderSize || header_size > size) {
        return std::nullopt;
    }
    return header_size;
}

// Human-readable text for the code field of a "destination unreachable"
// message. The codes mean different things in IPv4 (RFC 792) and IPv6
// (RFC 4443), so there is one table per family. Codes 9, 10 and 13 in IPv4
// are all variants of "a firewall or filter blocked this".
std::string describe_unreachable(Family family, int code) {
    if (family == Family::IPv4) {
        switch (code) {
            case 0:
                return "Destination net unreachable";
            case 1:
                return "Destination host unreachable";
            case 2:
                return "Destination protocol unreachable";
            case 3:
                return "Destination port unreachable";
            case 4:
                return "Fragmentation needed";
            case 9:
            case 10:
            case 13:
                return "Communication administratively prohibited";
            default:
                break;
        }
    } else {
        switch (code) {
            case 0:
                return "No route to destination";
            case 1:
                return "Communication administratively prohibited";
            case 2:
                return "Beyond scope of source address";
            case 3:
                return "Address unreachable";
            case 4:
                return "Port unreachable";
            default:
                break;
        }
    }
    return "Destination unreachable (code " + std::to_string(code) + ")";
}

}  // namespace netpulse::icmp