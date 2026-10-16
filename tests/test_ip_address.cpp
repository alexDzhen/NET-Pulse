// test_ip_address.cpp
// Unit tests for the IpAddress class (ip_address.cpp): construction,
// text output for both families, and equality.

#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "../include/ip_address.hpp"

using netpulse::Family;
using netpulse::IpAddress;

namespace {

// Test helper: builds an IPv6 address from its eight 16-bit groups, written
// the way they appear in text, e.g. ipv6({0x2001, 0xdb8, 0, 0, 0, 0, 0, 1}).
// Each group is split into its high and low byte (network order).
IpAddress ipv6(std::initializer_list<std::uint16_t> groups, std::uint32_t scope_id = 0) {
    std::uint8_t bytes[16] = {};
    std::size_t i = 0;
    for (const std::uint16_t group : groups) {
        bytes[i++] = static_cast<std::uint8_t>(group >> 8);
        bytes[i++] = static_cast<std::uint8_t>(group & 0xFF);
    }
    return IpAddress::from_bytes(Family::IPv6, bytes, scope_id);
}

}  // namespace

// A default-constructed address is the IPv4 "any" address.
TEST_CASE("the default address is 0.0.0.0") {
    const IpAddress address;
    CHECK(address.family() == Family::IPv4);
    CHECK(address.size() == 4);
    CHECK(address.to_string() == "0.0.0.0");
}

// Dotted-decimal output, and the two ways of building an IPv4 address
// (ipv4() and from_bytes()) giving the same result.
TEST_CASE("IPv4 addresses print in dotted-decimal") {
    CHECK(IpAddress::ipv4(192, 168, 1, 1).to_string() == "192.168.1.1");
    CHECK(IpAddress::ipv4(255, 255, 255, 255).to_string() == "255.255.255.255");
    CHECK(IpAddress::ipv4(8, 8, 8, 8).size() == 4);

    const std::uint8_t bytes[4] = {10, 0, 0, 1};
    CHECK(IpAddress::from_bytes(Family::IPv4, bytes) == IpAddress::ipv4(10, 0, 0, 1));
}

// The RFC 5952 rules, one case each:
//   all zeros -> "::"; zeros at the end -> "fe80::";
//   a single zero group is NOT compressed ("2001:db8:0:1:...");
//   with two runs of equal length the first one is compressed ("2001:0:0:1::1");
//   with runs of different length the longest wins ("2001:db8::1:0:0:1");
//   no zeros at all -> no "::".
TEST_CASE("IPv6 addresses print in RFC 5952 form") {
    CHECK(ipv6({0, 0, 0, 0, 0, 0, 0, 0}).to_string() == "::");
    CHECK(ipv6({0, 0, 0, 0, 0, 0, 0, 1}).to_string() == "::1");
    CHECK(ipv6({0x2001, 0xdb8, 0, 0, 0, 0, 0, 1}).to_string() == "2001:db8::1");
    CHECK(ipv6({0xfe80, 0, 0, 0, 0, 0, 0, 0}).to_string() == "fe80::");
    CHECK(ipv6({0x2001, 0xdb8, 0, 1, 1, 1, 1, 1}).to_string() == "2001:db8:0:1:1:1:1:1");
    CHECK(ipv6({0x2001, 0, 0, 1, 0, 0, 0, 1}).to_string() == "2001:0:0:1::1");
    CHECK(ipv6({1, 2, 3, 4, 5, 6, 7, 8}).to_string() == "1:2:3:4:5:6:7:8");
    CHECK(ipv6({0x2001, 0xdb8, 0, 0, 1, 0, 0, 1}).to_string() == "2001:db8::1:0:0:1");
    CHECK(ipv6({0, 0, 0, 0, 0, 0, 0, 1}).size() == 16);
}

// Equality must take all three parts into account: the bytes, the scope id
// (same link-local address on interface 2 vs 3 is different), and the
// family (16 zero bytes as IPv6 "::" is not the same as IPv4 0.0.0.0, even
// though the stored bytes are identical).
TEST_CASE("addresses compare by family, bytes and scope") {
    CHECK(IpAddress::ipv4(1, 2, 3, 4) == IpAddress::ipv4(1, 2, 3, 4));
    CHECK(IpAddress::ipv4(1, 2, 3, 4) != IpAddress::ipv4(1, 2, 3, 5));
    CHECK(ipv6({0, 0, 0, 0, 0, 0, 0, 1}) == ipv6({0, 0, 0, 0, 0, 0, 0, 1}));
    CHECK(ipv6({0xfe80, 0, 0, 0, 0, 0, 0, 1}, 2) != ipv6({0xfe80, 0, 0, 0, 0, 0, 0, 1}, 3));
    CHECK(ipv6({0xfe80, 0, 0, 0, 0, 0, 0, 1}, 2).scope_id() == 2);

    const std::uint8_t zeros[16] = {};
    CHECK(IpAddress::from_bytes(Family::IPv6, zeros) != IpAddress{});
}