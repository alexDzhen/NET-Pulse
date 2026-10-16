// socket_address.cpp
// Conversions between IpAddress and the operating system's sockaddr
// structures. memcpy is used everywhere instead of pointer casts: the
// sockaddr family of structs has strict alignment and aliasing rules, and
// copying bytes is the one way that is guaranteed to be correct on every
// compiler and platform.

#include "platform/socket_address.hpp"

#include <cstring>

namespace netpulse::platform {

// IpAddress -> sockaddr, for passing a destination to sendto() and friends.
// Fills the family-specific struct (sockaddr_in or sockaddr_in6), then
// copies it into the generic sockaddr_storage and records its real length.
// The port is left at 0; ICMP has no ports.
SocketAddress to_socket_address(const IpAddress& address) noexcept {
    SocketAddress result;
    if (address.family() == Family::IPv4) {
        sockaddr_in ipv4{};
        ipv4.sin_family = AF_INET;
        // sin_addr is 4 bytes in network order, same as our storage.
        std::memcpy(&ipv4.sin_addr, address.data(), 4);
        std::memcpy(&result.storage, &ipv4, sizeof(ipv4));
        result.length = static_cast<SocketLength>(sizeof(ipv4));
    } else {
        sockaddr_in6 ipv6{};
        ipv6.sin6_family = AF_INET6;
        std::memcpy(&ipv6.sin6_addr, address.data(), 16);
        // The scope id selects the network interface for link-local
        // addresses (fe80::...), which are otherwise ambiguous.
        ipv6.sin6_scope_id = address.scope_id();
        std::memcpy(&result.storage, &ipv6, sizeof(ipv6));
        result.length = static_cast<SocketLength>(sizeof(ipv6));
    }
    return result;
}

// sockaddr -> IpAddress, for addresses the system gives us: resolver
// results from getaddrinfo and the sender of a received packet.
// The first field of every sockaddr variant is the family, which tells us
// which concrete struct to copy into. Anything that is not IPv4 or IPv6
// (e.g. a Unix socket address) yields nothing.
std::optional<IpAddress> from_socket_address(const sockaddr* address) noexcept {
    if (address == nullptr) {
        return std::nullopt;
    }
    if (address->sa_family == AF_INET) {
        sockaddr_in ipv4{};
        std::memcpy(&ipv4, address, sizeof(ipv4));
        std::uint8_t bytes[4];
        std::memcpy(bytes, &ipv4.sin_addr, sizeof(bytes));
        return IpAddress::from_bytes(Family::IPv4, bytes);
    }
    if (address->sa_family == AF_INET6) {
        sockaddr_in6 ipv6{};
        std::memcpy(&ipv6, address, sizeof(ipv6));
        std::uint8_t bytes[16];
        std::memcpy(bytes, &ipv6.sin6_addr, sizeof(bytes));
        return IpAddress::from_bytes(Family::IPv6, bytes,
                                     static_cast<std::uint32_t>(ipv6.sin6_scope_id));
    }
    return std::nullopt;
}

}  // namespace netpulse::platform
