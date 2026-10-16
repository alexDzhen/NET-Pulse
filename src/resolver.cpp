// resolver.cpp
// Turns a host name (or a numeric address) typed by the user into an
// IpAddress, using the operating system's resolver (getaddrinfo), and takes
// care of the one-time socket library setup that Windows requires.

#include "../include/resolver.hpp"

#include <memory>

#include "platform/socket_address.hpp"

namespace netpulse {

namespace {

// Converts a getaddrinfo error code into readable text.
// On Windows gai_strerror returns text in the system language and code page,
// which comes out garbled in the console, so the common codes are mapped to
// fixed English messages instead. On Linux gai_strerror is fine as it is.
std::string resolve_error_text(int error) {
#ifdef _WIN32
    switch (error) {
        case WSAHOST_NOT_FOUND:
            return "host not found";
        case WSATRY_AGAIN:
            return "temporary failure in name resolution";
        case WSANO_RECOVERY:
            return "name server failure";
        case WSANO_DATA:
            return "the name has no address";
        case WSAEAFNOSUPPORT:
            return "address family not supported";
        default:
            return "error " + std::to_string(error);
    }
#else
    return gai_strerror(error);
#endif
}

}  // namespace

// Windows needs WSAStartup before any socket function can be used, and
// WSACleanup when done. Wrapping both in an object (RAII) ties the socket
// library's lifetime to the object's: create a NetworkSession in main(), and
// cleanup happens automatically when it goes out of scope, even on exceptions.
// On Linux there is nothing to do, so both bodies are empty.
NetworkSession::NetworkSession() {
#ifdef _WIN32
    WSADATA data{};
    const int error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error != 0) {
        throw ResolveError("cannot initialise Winsock (error " + std::to_string(error) + ")");
    }
#endif
}

NetworkSession::~NetworkSession() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// Resolves `host` to one address.
// - A numeric address ("8.8.8.8", "::1") is accepted as is.
// - A name may have several addresses; with FamilyPreference::Any the first
//   IPv4 one is preferred and IPv6 is used only when there is no IPv4.
// - With -4 / -6 only that family is requested from the resolver.
IpAddress resolve(const std::string& host, FamilyPreference preference) {
    // `hints` tells getaddrinfo what kind of results we want. The socket type
    // does not matter for us, but setting one stops the resolver from
    // returning the same address three times (once per socket type).
    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    switch (preference) {
        case FamilyPreference::Any:
            hints.ai_family = AF_UNSPEC;
            break;
        case FamilyPreference::IPv4:
            hints.ai_family = AF_INET;
            break;
        case FamilyPreference::IPv6:
            hints.ai_family = AF_INET6;
            break;
    }

    // Ask the system resolver. It returns a linked list of addrinfo entries
    // that must later be released with freeaddrinfo.
    addrinfo* raw_list = nullptr;
    const int error = getaddrinfo(host.c_str(), nullptr, &hints, &raw_list);
    if (error != 0) {
        throw ResolveError("cannot resolve '" + host + "': " + resolve_error_text(error));
    }
    // Hand the list to a unique_ptr with freeaddrinfo as its deleter, so it is
    // released on every exit path from this function, including exceptions.
    const std::unique_ptr<addrinfo, void (*)(addrinfo*)> list{
        raw_list, [](addrinfo* info) { freeaddrinfo(info); }};

    // Walk the list. Return the first IPv4 address immediately; remember the
    // first IPv6 address as a fallback in case no IPv4 address shows up.
    std::optional<IpAddress> fallback;
    for (const addrinfo* entry = list.get(); entry != nullptr; entry = entry->ai_next) {
        const auto address = platform::from_socket_address(entry->ai_addr);
        if (!address) {
            continue;
        }
        if (address->family() == Family::IPv4) {
            return *address;
        }
        if (!fallback) {
            fallback = address;
        }
    }

    // The resolver answered, but with nothing we can use (should be rare).
    if (!fallback) {
        throw ResolveError("cannot resolve '" + host + "': no usable address");
    }
    return *fallback;
}

}  // namespace netpulse