// test_icmp.cpp
// Unit tests for the ICMP wire-format code (icmp.cpp): the checksum, echo
// request building, parsing of replies and error messages, IPv4 header
// length detection and the unreachable-code descriptions.
// Because icmp.cpp works on plain byte arrays, the tests can build fake
// packets by hand (helpers below) and never need a network.
// REQUIRE stops the test case on failure (used where continuing would read
// out of bounds); CHECK records the failure and continues.

#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../include/icmp.hpp"

using netpulse::Family;
namespace icmp = netpulse::icmp;

namespace {

// Builds a minimal IPv4 header of 20 + options_size bytes. Byte 0 holds the
// version (4) in the high nibble and the header length in 32-bit words in
// the low nibble; byte 8 is the TTL and byte 9 the protocol (1 = ICMP).
std::vector<std::uint8_t> ipv4_header(std::uint8_t protocol, std::size_t options_size = 0) {
    std::vector<std::uint8_t> header(20 + options_size, 0);
    header[0] = static_cast<std::uint8_t>(0x40u | ((20 + options_size) / 4));
    header[8] = 64;
    header[9] = protocol;
    return header;
}

// Builds a minimal 40-byte IPv6 header: version 6 in the high nibble of
// byte 0, "next header" (58 = ICMPv6) in byte 6, hop limit in byte 7.
std::vector<std::uint8_t> ipv6_header(std::uint8_t next_header) {
    std::vector<std::uint8_t> header(40, 0);
    header[0] = 0x60;
    header[6] = next_header;
    header[7] = 64;
    return header;
}

// Assembles an ICMP error message the way a router would: an 8-byte ICMP
// header (type, code, zero checksum, unused) followed by a quoted copy of
// the IP header and payload of the packet that caused the error.
std::vector<std::uint8_t> error_message(std::uint8_t type, std::uint8_t code,
                                        const std::vector<std::uint8_t>& quoted_header,
                                        const std::vector<std::uint8_t>& quoted_payload) {
    std::vector<std::uint8_t> message = {type, code, 0, 0, 0, 0, 0, 0};
    message.insert(message.end(), quoted_header.begin(), quoted_header.end());
    message.insert(message.end(), quoted_payload.begin(), quoted_payload.end());
    return message;
}

}  // namespace

// Known answers: the worked example from RFC 1071 itself, a hand-computed
// echo header, the empty input (sum 0, complemented -> 0xffff) and an input
// whose one's-complement sum is all ones (complemented -> 0).
TEST_CASE("the Internet checksum matches known values") {
    const std::uint8_t rfc1071_example[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK(icmp::checksum(rfc1071_example, sizeof(rfc1071_example)) == 0x220d);

    const std::uint8_t echo_header[] = {0x08, 0x00, 0x00, 0x00, 0x12, 0x34, 0x00, 0x01};
    CHECK(icmp::checksum(echo_header, sizeof(echo_header)) == 0xe5ca);

    CHECK(icmp::checksum(nullptr, 0) == 0xffff);

    const std::uint8_t all_ones[] = {0xff, 0xff, 0xff, 0xff};
    CHECK(icmp::checksum(all_ones, sizeof(all_ones)) == 0x0000);
}

// An odd number of bytes: the last byte must count as the high half of a
// final 16-bit word, exactly as if a zero byte followed it.
TEST_CASE("an odd trailing byte is padded with zero on the right") {
    const std::uint8_t odd[] = {0x01, 0x02, 0x03};
    const std::uint8_t padded[] = {0x01, 0x02, 0x03, 0x00};
    CHECK(icmp::checksum(odd, sizeof(odd)) == icmp::checksum(padded, sizeof(padded)));
    CHECK(icmp::checksum(odd, sizeof(odd)) == 0xfbfd);
}

// Field by field: type 8, code 0, identifier and sequence big-endian at
// bytes 4-7, payload 0,1,2,... (so byte 63 = payload index 55), a non-zero
// checksum, and the whole packet verifying to zero.
TEST_CASE("an IPv4 echo request has the expected layout and a valid checksum") {
    const auto packet = icmp::build_echo_request(Family::IPv4, 0x1234, 0x0007, 56);

    REQUIRE(packet.size() == 64);
    CHECK(packet[0] == 8);
    CHECK(packet[1] == 0);
    CHECK(packet[4] == 0x12);
    CHECK(packet[5] == 0x34);
    CHECK(packet[6] == 0x00);
    CHECK(packet[7] == 0x07);
    CHECK(packet[8] == 0);
    CHECK(packet[9] == 1);
    CHECK(packet[63] == 55);
    CHECK((packet[2] != 0 || packet[3] != 0));
    CHECK(icmp::checksum(packet.data(), packet.size()) == 0);
}

// Exercises the odd-length path of the checksum through every size from
// 0 to 300 bytes of payload.
TEST_CASE("the checksum stays valid for every payload size, odd or even") {
    for (std::size_t payload = 0; payload <= 300; ++payload) {
        CAPTURE(payload);
        const auto packet = icmp::build_echo_request(Family::IPv4, 0xBEEF, 42, payload);
        REQUIRE(packet.size() == icmp::kHeaderSize + payload);
        CHECK(icmp::checksum(packet.data(), packet.size()) == 0);
    }
}

// Flipping a single bit must make the verification fail; this is what
// protects against damaged packets.
TEST_CASE("corrupting any byte of a packet breaks the checksum") {
    auto packet = icmp::build_echo_request(Family::IPv4, 1, 1, 16);
    packet[12] ^= 0x01;
    CHECK(icmp::checksum(packet.data(), packet.size()) != 0);
}

// IPv6: type 128 and the checksum bytes (2-3) left at zero for the kernel.
TEST_CASE("an IPv6 echo request leaves the checksum to the kernel") {
    const auto packet = icmp::build_echo_request(Family::IPv6, 0xABCD, 0x0102, 8);

    REQUIRE(packet.size() == 16);
    CHECK(packet[0] == 128);
    CHECK(packet[1] == 0);
    CHECK(packet[2] == 0);
    CHECK(packet[3] == 0);
    CHECK(packet[4] == 0xAB);
    CHECK(packet[5] == 0xCD);
    CHECK(packet[6] == 0x01);
    CHECK(packet[7] == 0x02);
}

// A reply is simulated by taking a request and changing only the type
// byte, which is what the target does. Identifier and sequence must come
// back unchanged.
TEST_CASE("echo replies are recognised for both families") {
    auto v4 = icmp::build_echo_request(Family::IPv4, 0x1111, 0x2222, 4);
    v4[0] = icmp::kV4EchoReply;
    const auto parsed_v4 = icmp::parse_message(Family::IPv4, v4.data(), v4.size());
    REQUIRE(parsed_v4.has_value());
    CHECK(parsed_v4->kind == icmp::MessageKind::EchoReply);
    CHECK(parsed_v4->identifier == 0x1111);
    CHECK(parsed_v4->sequence == 0x2222);

    auto v6 = icmp::build_echo_request(Family::IPv6, 0x3333, 0x4444, 4);
    v6[0] = icmp::kV6EchoReply;
    const auto parsed_v6 = icmp::parse_message(Family::IPv6, v6.data(), v6.size());
    REQUIRE(parsed_v6.has_value());
    CHECK(parsed_v6->kind == icmp::MessageKind::EchoReply);
    CHECK(parsed_v6->identifier == 0x3333);
    CHECK(parsed_v6->sequence == 0x4444);
}

// On a raw socket the kernel can hand us our own outgoing request (e.g. when
// pinging localhost). Its type is "request", not "reply", so it must be
// ignored rather than counted as an answer.
TEST_CASE("our own echo request is not mistaken for a reply") {
    const auto v4 = icmp::build_echo_request(Family::IPv4, 1, 1, 4);
    CHECK_FALSE(icmp::parse_message(Family::IPv4, v4.data(), v4.size()).has_value());

    const auto v6 = icmp::build_echo_request(Family::IPv6, 1, 1, 4);
    CHECK_FALSE(icmp::parse_message(Family::IPv6, v6.data(), v6.size()).has_value());
}

// For error messages the identifier and sequence are not in the error's own
// header; they must be dug out of the quoted copy of our request after the
// quoted IPv4 header. Also checks that the unreachable code is passed on.
TEST_CASE("IPv4 errors expose the identifier and sequence of the quoted request") {
    const auto request = icmp::build_echo_request(Family::IPv4, 0xCAFE, 0x0009, 0);

    const auto exceeded = error_message(icmp::kV4TimeExceeded, 0, ipv4_header(1), request);
    const auto parsed = icmp::parse_message(Family::IPv4, exceeded.data(), exceeded.size());
    REQUIRE(parsed.has_value());
    CHECK(parsed->kind == icmp::MessageKind::TimeExceeded);
    CHECK(parsed->identifier == 0xCAFE);
    CHECK(parsed->sequence == 0x0009);

    const auto unreachable =
        error_message(icmp::kV4DestinationUnreachable, 1, ipv4_header(1), request);
    const auto parsed_unreachable =
        icmp::parse_message(Family::IPv4, unreachable.data(), unreachable.size());
    REQUIRE(parsed_unreachable.has_value());
    CHECK(parsed_unreachable->kind == icmp::MessageKind::DestinationUnreachable);
    CHECK(parsed_unreachable->code == 1);
    CHECK(parsed_unreachable->sequence == 0x0009);
}

// A quoted IPv4 header with 8 bytes of options is 28 bytes long; the parser
// must use the IHL field rather than assume 20.
TEST_CASE("IPv4 options in the quoted header are skipped") {
    const auto request = icmp::build_echo_request(Family::IPv4, 0x0102, 0x0304, 0);
    const auto message = error_message(icmp::kV4TimeExceeded, 0, ipv4_header(1, 8), request);

    const auto parsed = icmp::parse_message(Family::IPv4, message.data(), message.size());
    REQUIRE(parsed.has_value());
    CHECK(parsed->identifier == 0x0102);
    CHECK(parsed->sequence == 0x0304);
}

// Same as above for IPv6, where the quoted header is a fixed 40 bytes.
TEST_CASE("IPv6 errors expose the identifier and sequence of the quoted request") {
    const auto request = icmp::build_echo_request(Family::IPv6, 0xF00D, 0x0003, 0);

    const auto exceeded = error_message(icmp::kV6TimeExceeded, 0, ipv6_header(58), request);
    const auto parsed = icmp::parse_message(Family::IPv6, exceeded.data(), exceeded.size());
    REQUIRE(parsed.has_value());
    CHECK(parsed->kind == icmp::MessageKind::TimeExceeded);
    CHECK(parsed->identifier == 0xF00D);
    CHECK(parsed->sequence == 0x0003);

    const auto unreachable =
        error_message(icmp::kV6DestinationUnreachable, 3, ipv6_header(58), request);
    const auto parsed_unreachable =
        icmp::parse_message(Family::IPv6, unreachable.data(), unreachable.size());
    REQUIRE(parsed_unreachable.has_value());
    CHECK(parsed_unreachable->kind == icmp::MessageKind::DestinationUnreachable);
    CHECK(parsed_unreachable->code == 3);
}

// A raw socket also receives errors caused by other programs' traffic.
// If the quoted packet is UDP (protocol 17), TCP (next header 6) or an echo
// *reply* rather than a request, it is not ours and must be ignored.
TEST_CASE("errors about other traffic are ignored") {
    const auto request = icmp::build_echo_request(Family::IPv4, 1, 1, 0);

    const auto about_udp = error_message(icmp::kV4TimeExceeded, 0, ipv4_header(17), request);
    CHECK_FALSE(icmp::parse_message(Family::IPv4, about_udp.data(), about_udp.size()).has_value());

    auto reply_quoted = request;
    reply_quoted[0] = icmp::kV4EchoReply;
    const auto about_reply = error_message(icmp::kV4TimeExceeded, 0, ipv4_header(1), reply_quoted);
    CHECK_FALSE(
        icmp::parse_message(Family::IPv4, about_reply.data(), about_reply.size()).has_value());

    const auto request_v6 = icmp::build_echo_request(Family::IPv6, 1, 1, 0);
    const auto about_tcp = error_message(icmp::kV6TimeExceeded, 0, ipv6_header(6), request_v6);
    CHECK_FALSE(icmp::parse_message(Family::IPv6, about_tcp.data(), about_tcp.size()).has_value());
}

// Robustness against bad or unexpected input: a packet shorter than a
// header, message types we do not handle (redirect, neighbour advertisement),
// an error message cut off before the quoted ICMP header, and an error with
// no quoted packet at all. None may crash or be accepted.
TEST_CASE("truncated and unrelated messages are ignored") {
    const std::uint8_t too_short[] = {0, 0, 0, 0};
    CHECK_FALSE(icmp::parse_message(Family::IPv4, too_short, sizeof(too_short)).has_value());

    const std::uint8_t redirect[] = {5, 0, 0, 0, 0, 0, 0, 0};
    CHECK_FALSE(icmp::parse_message(Family::IPv4, redirect, sizeof(redirect)).has_value());

    const std::uint8_t neighbour_advert[] = {136, 0, 0, 0, 0, 0, 0, 0};
    CHECK_FALSE(
        icmp::parse_message(Family::IPv6, neighbour_advert, sizeof(neighbour_advert)).has_value());

    const auto request = icmp::build_echo_request(Family::IPv4, 1, 1, 0);
    auto truncated = error_message(icmp::kV4TimeExceeded, 0, ipv4_header(1), request);
    truncated.resize(truncated.size() - 4);
    CHECK_FALSE(icmp::parse_message(Family::IPv4, truncated.data(), truncated.size()).has_value());

    const std::uint8_t header_only[] = {11, 0, 0, 0, 0, 0, 0, 0};
    CHECK_FALSE(icmp::parse_message(Family::IPv4, header_only, sizeof(header_only)).has_value());
}

// 20 bytes without options, 32 with 12 bytes of options; rejected when the
// buffer is shorter than the header claims, when the version nibble is not
// 4 (0x65 = version 6), and when the IHL says less than 20 bytes (0x44 = 16).
TEST_CASE("the IPv4 header length is read from the IHL field") {
    const auto plain = ipv4_header(1);
    CHECK(icmp::ipv4_header_size(plain.data(), plain.size()) == 20);

    const auto with_options = ipv4_header(1, 12);
    CHECK(icmp::ipv4_header_size(with_options.data(), with_options.size()) == 32);

    CHECK_FALSE(icmp::ipv4_header_size(plain.data(), 19).has_value());
    CHECK_FALSE(icmp::ipv4_header_size(with_options.data(), 24).has_value());

    auto wrong_version = plain;
    wrong_version[0] = 0x65;
    CHECK_FALSE(icmp::ipv4_header_size(wrong_version.data(), wrong_version.size()).has_value());

    auto short_ihl = plain;
    short_ihl[0] = 0x44;
    CHECK_FALSE(icmp::ipv4_header_size(short_ihl.data(), short_ihl.size()).has_value());
}

// A few known codes per family, plus the generic text for an unknown code.
TEST_CASE("unreachable codes have readable descriptions") {
    CHECK(icmp::describe_unreachable(Family::IPv4, 0) == "Destination net unreachable");
    CHECK(icmp::describe_unreachable(Family::IPv4, 1) == "Destination host unreachable");
    CHECK(icmp::describe_unreachable(Family::IPv4, 3) == "Destination port unreachable");
    CHECK(icmp::describe_unreachable(Family::IPv4, 13) ==
          "Communication administratively prohibited");
    CHECK(icmp::describe_unreachable(Family::IPv4, 99) == "Destination unreachable (code 99)");
    CHECK(icmp::describe_unreachable(Family::IPv6, 0) == "No route to destination");
    CHECK(icmp::describe_unreachable(Family::IPv6, 3) == "Address unreachable");
    CHECK(icmp::describe_unreachable(Family::IPv6, 99) == "Destination unreachable (code 99)");
}