// ip_address.cpp
// Implements the IpAddress class: a small value type that holds either an
// IPv4 address (4 bytes) or an IPv6 address (16 bytes) and can print itself
// as text. Everything here is plain byte manipulation; no network calls.

#include "../include/ip_address.hpp"

#include <algorithm>
#include <cstring>

namespace netpulse {

namespace {

// An IPv6 address is written as 8 groups of 16 bits (e.g. 2001:db8:0:0:0:0:0:1).
constexpr int kGroupCount = 8;

// Appends one 16-bit IPv6 group to `out` as lowercase hex without leading
// zeros, e.g. 0x0db8 -> "db8", 0x0000 -> "0".
// Walks the four hex digits from most to least significant (shift 12, 8, 4, 0)
// and starts printing at the first non-zero digit. The last digit (shift == 0)
// is always printed so that a zero group becomes "0" and not "".
void append_hex(std::string& out, unsigned group) {
    static const char kDigits[] = "0123456789abcdef";
    bool started = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        const unsigned digit = (group >> shift) & 0xFu;
        if (digit != 0 || started || shift == 0) {
            out += kDigits[digit];
            started = true;
        }
    }
}

// Formats 4 bytes as dotted decimal, e.g. {192,168,1,1} -> "192.168.1.1".
std::string ipv4_text(const std::uint8_t* bytes) {
    std::string text;
    for (int i = 0; i < 4; ++i) {
        if (i != 0) {
            text += '.';
        }
        text += std::to_string(bytes[i]);
    }
    return text;
}

// Formats 16 bytes as IPv6 text in the canonical form from RFC 5952:
// lowercase hex, no leading zeros in a group, and the longest run of two or
// more zero groups replaced by "::" (only the first such run if there is a tie).
// Example: 2001:0db8:0000:0000:0000:0000:0000:0001 -> "2001:db8::1".
std::string ipv6_text(const std::uint8_t* bytes) {
    // Step 1: combine the 16 bytes into 8 groups of 16 bits (network byte
    // order, so the first byte of each pair is the high byte).
    unsigned groups[kGroupCount];
    for (int i = 0; i < kGroupCount; ++i) {
        groups[i] = (static_cast<unsigned>(bytes[2 * i]) << 8) | bytes[2 * i + 1];
    }

    // Step 2: find the longest run of consecutive zero groups.
    // best_start / best_length describe that run; -1 means "none".
    int best_start = -1;
    int best_length = 0;
    for (int i = 0; i < kGroupCount;) {
        if (groups[i] != 0) {
            ++i;
            continue;
        }
        // groups[i] is zero: measure how far the run of zeros extends.
        int end = i;
        while (end < kGroupCount && groups[end] == 0) {
            ++end;
        }
        // Keep it only if it is strictly longer than the best so far, which
        // makes the leftmost run win when two runs have the same length.
        if (end - i > best_length) {
            best_start = i;
            best_length = end - i;
        }
        i = end;
    }
    // RFC 5952 forbids compressing a single zero group, so "::" needs at
    // least two zero groups in a row.
    if (best_length < 2) {
        best_start = -1;
    }

    // Step 3: print the groups, replacing the chosen zero run with "::".
    std::string text;
    for (int i = 0; i < kGroupCount;) {
        if (i == best_start) {
            text += "::";
            i += best_length;
            continue;
        }
        // Separate groups with ':' unless we just emitted "::", which already
        // ends with a colon.
        if (!text.empty() && text.back() != ':') {
            text += ':';
        }
        append_hex(text, groups[i]);
        ++i;
    }
    return text;
}

}  // namespace

// Convenience constructor for IPv4: IpAddress::ipv4(192, 168, 1, 1).
IpAddress IpAddress::ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) noexcept {
    const std::uint8_t bytes[4] = {a, b, c, d};
    return from_bytes(Family::IPv4, bytes);
}

// Builds an address from raw bytes in network order: 4 bytes for IPv4,
// 16 bytes for IPv6. The scope id (interface index used for link-local IPv6
// addresses such as fe80::1%eth0) is only meaningful for IPv6, so it is
// forced to 0 for IPv4. size() returns 4 or 16 depending on the family, so
// only that many bytes are copied.
IpAddress IpAddress::from_bytes(Family family, const std::uint8_t* data,
                                std::uint32_t scope_id) noexcept {
    IpAddress address;
    address.family_ = family;
    address.scope_id_ = family == Family::IPv6 ? scope_id : 0;
    std::memcpy(address.bytes_.data(), data, address.size());
    return address;
}

// Text form of the address, picking the formatter for the family.
std::string IpAddress::to_string() const {
    return family_ == Family::IPv4 ? ipv4_text(bytes_.data()) : ipv6_text(bytes_.data());
}

// Two addresses are equal when they have the same family, the same scope id
// and the same address bytes. Only size() bytes are compared, so leftover
// bytes in the 16-byte storage of an IPv4 address do not matter.
bool operator==(const IpAddress& lhs, const IpAddress& rhs) noexcept {
    return lhs.family() == rhs.family() && lhs.scope_id() == rhs.scope_id() &&
           std::equal(lhs.data(), lhs.data() + lhs.size(), rhs.data());
}

bool operator!=(const IpAddress& lhs, const IpAddress& rhs) noexcept {
    return !(lhs == rhs);
}

}  // namespace netpulse