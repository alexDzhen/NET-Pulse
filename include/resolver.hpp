// resolver.hpp
// Declares the pieces needed to turn the host given on the command line
// into an IpAddress: the error type, the -4/-6 preference, the socket
// library setup object and the resolve() function. Implemented in
// resolver.cpp.

#pragma once

#include <stdexcept>
#include <string>

#include "ip_address.hpp"

namespace netpulse {

/// Thrown when a host name cannot be turned into an address.
// Inherits from runtime_error so the message is available through what().
// `using std::runtime_error::runtime_error;` reuses the base constructors,
// so ResolveError("text") works without writing a constructor by hand.
class ResolveError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Which address family the user wants, set by the -4 / -6 options.
enum class FamilyPreference {
    /// IPv4 when the host has an IPv4 address, otherwise IPv6.
    Any,
    IPv4,
    IPv6,
};

/// Initialises the operating system's socket library for the lifetime of the
/// object. Required on Windows (WSAStartup), a no-op elsewhere.
// RAII: construct one at the start of main(); its destructor does the
// cleanup. Copying is deleted because two copies would clean up twice.
class NetworkSession {
public:
    NetworkSession();
    ~NetworkSession();
    NetworkSession(const NetworkSession&) = delete;
    NetworkSession& operator=(const NetworkSession&) = delete;
};

/// Resolves a host name or a numeric address. Throws ResolveError on failure.
// Accepts anything the system resolver does: "example.com", "192.168.1.1",
// "::1". With FamilyPreference::Any, IPv4 is preferred when both exist.
IpAddress resolve(const std::string& host, FamilyPreference preference = FamilyPreference::Any);

}  // namespace netpulse