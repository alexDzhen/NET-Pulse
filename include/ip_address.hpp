// ip_address.hpp
// Declares IpAddress, the one type the whole program uses for an IP address.
// It is a small value type (cheap to copy, no heap memory) that can hold
// either family, so functions like resolve() and the socket code do not
// need separate IPv4 and IPv6 versions. Implemented in ip_address.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace netpulse {

// Which kind of address an IpAddress holds.
enum class Family {
    IPv4,
    IPv6,
};

/// An IPv4 or IPv6 address in network byte order.
// "Network byte order" means the bytes are stored exactly as they appear in
// a packet, most significant first: 192.168.1.1 is {192, 168, 1, 1}.
class IpAddress {
public:
    /// 0.0.0.0
    IpAddress() noexcept = default;

    // Convenience constructor: IpAddress::ipv4(192, 168, 1, 1).
    static IpAddress ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) noexcept;

    /// Copies 4 bytes (IPv4) or 16 bytes (IPv6) from `data`.
    // The main way addresses are built, from bytes handed over by the
    // operating system (resolver results, packet senders).
    static IpAddress from_bytes(Family family, const std::uint8_t* data,
                                std::uint32_t scope_id = 0) noexcept;

    Family family() const noexcept { return family_; }

    /// The raw address bytes; size() of them are meaningful.
    // Used when the address has to go into a sockaddr or a packet.
    const std::uint8_t* data() const noexcept { return bytes_.data(); }
    std::size_t size() const noexcept { return family_ == Family::IPv4 ? 4 : 16; }

    /// Interface index for link-local IPv6 addresses, otherwise 0.
    // A link-local address (fe80::...) is only unique per network interface,
    // so "fe80::1%eth0" needs the interface to be usable at all.
    std::uint32_t scope_id() const noexcept { return scope_id_; }

    /// Dotted-decimal for IPv4, RFC 5952 text for IPv6.
    std::string to_string() const;

private:
    Family family_ = Family::IPv4;
    // Always 16 bytes of storage; an IPv4 address uses only the first 4.
    std::array<std::uint8_t, 16> bytes_{};
    std::uint32_t scope_id_ = 0;
};

// Equal when family, scope id and the meaningful bytes all match.
bool operator==(const IpAddress& lhs, const IpAddress& rhs) noexcept;
bool operator!=(const IpAddress& lhs, const IpAddress& rhs) noexcept;

}  // namespace netpulse