// socket_address.hpp
// Bridge between our own IpAddress type and the operating system's sockaddr
// structures, which every socket function (sendto, getaddrinfo, recvmsg...)
// expects. Keeping the conversion in one place means the rest of the code
// never touches sockaddr_in / sockaddr_in6 directly.

#pragma once

// The socket headers live in different places on Windows and on POSIX
// systems (Linux), so pick the right set at compile time.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <optional>

#include "../../include/ip_address.hpp"

namespace netpulse::platform {

// The "length of a sockaddr" parameter is an int on Windows and a socklen_t
// on POSIX. This alias hides the difference.
#ifdef _WIN32
using SocketLength = int;
#else
using SocketLength = socklen_t;
#endif

// A sockaddr together with its length, the pair every socket call wants.
// sockaddr_storage is a structure guaranteed to be large enough (and aligned
// correctly) to hold either a sockaddr_in (IPv4) or a sockaddr_in6 (IPv6),
// so one type covers both families.
struct SocketAddress {
    sockaddr_storage storage{};
    SocketLength length = 0;

    // The socket API takes a generic `const sockaddr*`, so this gives the
    // storage that view.
    const sockaddr* get() const noexcept { return reinterpret_cast<const sockaddr*>(&storage); }
};

/// Converts an address to the sockaddr form the socket API expects.
SocketAddress to_socket_address(const IpAddress& address) noexcept;

/// Converts a sockaddr_in or sockaddr_in6 back. Other families give nothing.
std::optional<IpAddress> from_socket_address(const sockaddr* address) noexcept;

}  // namespace netpulse::platform