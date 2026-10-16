// icmp_client_windows.cpp
// The Windows implementation of IcmpClient. Only compiled on Windows.
//
// Windows does not let a normal program use raw ICMP sockets (they need
// administrator rights, and the firewall drops the ICMP error replies that
// ping and traceroute depend on). Instead, Windows provides the IP Helper
// ICMP API (iphlpapi.dll): you hand it the destination, the payload and the
// TTL, it builds and sends the packet, waits for the answer and fills a
// reply buffer with the result. This is the same API the built-in ping.exe
// and tracert.exe use. So on Windows the packet building in icmp.cpp is
// not used; the system does that part for us.

#include "../include/icmp_client.hpp"

// Order matters: winsock2.h must come before any other Windows header, and
// iphlpapi.h must come before icmpapi.h (Microsoft's documented requirement).
#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>

#include <icmpapi.h>

#include <cstring>
#include <vector>

#include "platform/socket_address.hpp"

namespace netpulse {

namespace {

// steady_clock never jumps (unlike the wall clock), so it is the right one
// for measuring round-trip time.
using Clock = std::chrono::steady_clock;

constexpr std::size_t kIcmpHeaderSize = 8;
// Extra room in the reply buffer beyond what the documentation requires
// (an ICMP error message plus an IO_STATUS_BLOCK), to be safe.
constexpr std::size_t kReplySlack = 64;

// Layout of the ICMPV6_ECHO_REPLY that Windows writes into the reply buffer:
// port (2 bytes), flow info (4), address (16), scope id (4), padding (2), status (4),
// round-trip time (4). The address part is packed to one byte in the Windows SDK
// but not in every compiler's headers, so the fields are read by offset.
constexpr std::size_t kV6ReplyAddressOffset = 6;
constexpr std::size_t kV6ReplyScopeOffset = 22;
constexpr std::size_t kV6ReplyStatusOffset = 28;
constexpr std::size_t kV6ReplySize = 36;

// When the send function returns 0 replies, GetLastError() holds the reason.
// A reason of 0 would be misread as "success" by classify(), so it is
// turned into a generic failure instead.
ULONG failure_status(DWORD last_error) {
    return last_error == IP_SUCCESS ? static_cast<ULONG>(IP_GENERAL_FAILURE) : last_error;
}

// Readable text for the Windows status codes that we treat as errors.
// Fixed English strings are used instead of GetIpErrorString, which returns
// localized text that comes out garbled in a non-English console.
std::string status_text(ULONG status) {
    switch (status) {
        case IP_BUF_TOO_SMALL:
            return "reply buffer too small";
        case IP_NO_RESOURCES:
            return "insufficient system resources";
        case IP_HW_ERROR:
            return "hardware error";
        case IP_PACKET_TOO_BIG:
            return "packet too big";
        case IP_BAD_REQ:
            return "bad request";
        case IP_BAD_ROUTE:
            return "bad route";
        case IP_PARAM_PROBLEM:
            return "parameter problem";
        case IP_BAD_DESTINATION:
            return "bad destination address";
        case IP_GENERAL_FAILURE:
            return "general failure";
        default:
            return "ICMP status " + std::to_string(status);
    }
}

// Maps a Windows IP_* status code onto our platform-independent EchoStatus.
// Windows reports "destination unreachable" as separate status codes rather
// than as an ICMP code number, so for those the matching ICMP code is filled
// in by hand (the numbers differ between IPv4 and IPv6; see
// describe_unreachable in icmp.cpp), which lets the shared report code print
// the same text on both operating systems.
void classify(Family family, ULONG status, EchoResult& result) {
    const bool v4 = family == Family::IPv4;
    switch (status) {
        case IP_SUCCESS:
            result.status = EchoStatus::Reply;
            return;
        case IP_REQ_TIMED_OUT:
            result.status = EchoStatus::Timeout;
            return;
        case IP_TTL_EXPIRED_TRANSIT:
        case IP_TTL_EXPIRED_REASSEM:
        case IP_TIME_EXCEEDED:
            result.status = EchoStatus::TimeExceeded;
            return;
        case IP_DEST_NET_UNREACHABLE:
            result.status = EchoStatus::DestinationUnreachable;
            result.code = 0;
            return;
        case IP_DEST_HOST_UNREACHABLE:
            result.status = EchoStatus::DestinationUnreachable;
            result.code = v4 ? 1 : 3;
            return;
        case IP_DEST_PROT_UNREACHABLE:
            result.status = EchoStatus::DestinationUnreachable;
            result.code = v4 ? 2 : 1;
            return;
        case IP_DEST_PORT_UNREACHABLE:
            result.status = EchoStatus::DestinationUnreachable;
            result.code = v4 ? 3 : 4;
            return;
        case IP_DEST_SCOPE_MISMATCH:
            result.status = EchoStatus::DestinationUnreachable;
            result.code = 2;
            return;
        default:
            result.status = EchoStatus::Error;
            result.error = status_text(status);
            return;
    }
}

// Owns one ICMP handle from IcmpCreateFile / Icmp6CreateFile and sends echo
// requests through it. `final` because nothing derives from it.
class WindowsIcmpClient final : public IcmpClient {
public:
    WindowsIcmpClient(HANDLE handle, const IpAddress& target) : handle_(handle), target_(target) {}

    // Releasing the handle in the destructor means it cannot leak.
    ~WindowsIcmpClient() override { IcmpCloseHandle(handle_); }

    // Sends one echo request and blocks until its answer or the timeout.
    // The API call itself cannot be interrupted, so Ctrl+C is only checked
    // before sending; main() checks again after each call returns.
    EchoResult echo(const EchoRequest& request) override {
        if (interrupted()) {
            EchoResult result;
            result.status = EchoStatus::Interrupted;
            return result;
        }

        // Payload filled with 0,1,2,...; the vector is never empty so that
        // the API never receives a null data pointer, even with --size 0.
        const std::size_t size = request.payload_size;
        std::vector<std::uint8_t> payload(size > 0 ? size : 1);
        for (std::size_t i = 0; i < size; ++i) {
            payload[i] = static_cast<std::uint8_t>(i & 0xFFu);
        }

        // The TTL is passed through IP_OPTION_INFORMATION. A null pointer
        // means "use the system default", which is what ttl == 0 asks for.
        IP_OPTION_INFORMATION options{};
        options.Ttl = static_cast<UCHAR>(request.ttl);
        IP_OPTION_INFORMATION* options_ptr = request.ttl > 0 ? &options : nullptr;

        const DWORD timeout = static_cast<DWORD>(request.timeout.count());
        return target_.family() == Family::IPv4 ? echo_v4(payload, size, options_ptr, timeout)
                                                : echo_v6(payload, size, options_ptr, timeout);
    }

private:
    EchoResult echo_v4(std::vector<std::uint8_t>& payload, std::size_t size,
                       IP_OPTION_INFORMATION* options, DWORD timeout) {
        // The reply buffer must hold an ICMP_ECHO_REPLY structure, the echoed
        // payload and, per the documentation, 8 more bytes for an ICMP error.
        std::vector<std::uint8_t> reply_buffer(sizeof(ICMP_ECHO_REPLY) + size + kIcmpHeaderSize +
                                               kReplySlack);
        // IcmpSendEcho2 takes the IPv4 address as a 32-bit value in network
        // byte order, which is exactly the 4 bytes we already hold.
        IPAddr destination = 0;
        std::memcpy(&destination, target_.data(), sizeof(destination));

        // Synchronous call (no event, no callback): it returns when a reply
        // arrives or the timeout expires. The time is measured around it.
        const Clock::time_point start = Clock::now();
        const DWORD replies =
            IcmpSendEcho2(handle_, nullptr, nullptr, nullptr, destination, payload.data(),
                          static_cast<WORD>(size), options, reply_buffer.data(),
                          static_cast<DWORD>(reply_buffer.size()), timeout);
        // GetLastError must be read right away, before any other call can
        // overwrite it.
        const DWORD last_error = GetLastError();
        const Clock::time_point end = Clock::now();

        // Zero replies: timeout or failure, the reason is in last_error.
        EchoResult result;
        if (replies == 0) {
            classify(Family::IPv4, failure_status(last_error), result);
            return result;
        }

        // At least one reply: the first ICMP_ECHO_REPLY sits at the start of
        // the buffer. memcpy instead of a cast keeps alignment rules happy.
        ICMP_ECHO_REPLY reply{};
        std::memcpy(&reply, reply_buffer.data(), sizeof(reply));

        // Status says whether it is a real echo reply or a router's error
        // message; Address is whoever sent it (target or router).
        classify(Family::IPv4, reply.Status, result);
        std::uint8_t responder[4];
        std::memcpy(responder, &reply.Address, sizeof(responder));
        result.responder = IpAddress::from_bytes(Family::IPv4, responder);
        result.round_trip = end - start;
        if (result.status == EchoStatus::Reply) {
            result.ttl = reply.Options.Ttl;
            result.bytes = reply.DataSize + kIcmpHeaderSize;
        }
        return result;
    }

    EchoResult echo_v6(std::vector<std::uint8_t>& payload, std::size_t size,
                       IP_OPTION_INFORMATION* options, DWORD timeout) {
        std::vector<std::uint8_t> reply_buffer(kV6ReplySize + size + kIcmpHeaderSize + kReplySlack);
        // Icmp6SendEcho2 wants sockaddr_in6 structures for both ends. An
        // all-zero source address ("::") lets Windows pick the right
        // outgoing address itself.
        sockaddr_in6 source{};
        source.sin6_family = AF_INET6;
        sockaddr_in6 destination{};
        const platform::SocketAddress target = platform::to_socket_address(target_);
        std::memcpy(&destination, &target.storage, sizeof(destination));

        const Clock::time_point start = Clock::now();
        const DWORD replies =
            Icmp6SendEcho2(handle_, nullptr, nullptr, nullptr, &source, &destination,
                           payload.data(), static_cast<WORD>(size), options, reply_buffer.data(),
                           static_cast<DWORD>(reply_buffer.size()), timeout);
        const DWORD last_error = GetLastError();
        const Clock::time_point end = Clock::now();

        EchoResult result;
        if (replies == 0) {
            classify(Family::IPv6, failure_status(last_error), result);
            return result;
        }

        // Read the reply fields by byte offset (see the layout note above)
        // rather than through the ICMPV6_ECHO_REPLY struct, so the result is
        // the same whether the headers pack the struct or not.
        ULONG scope_id = 0;
        std::memcpy(&scope_id, reply_buffer.data() + kV6ReplyScopeOffset, sizeof(scope_id));
        ULONG status = 0;
        std::memcpy(&status, reply_buffer.data() + kV6ReplyStatusOffset, sizeof(status));

        classify(Family::IPv6, status, result);
        result.responder = IpAddress::from_bytes(
            Family::IPv6, reply_buffer.data() + kV6ReplyAddressOffset, scope_id);
        result.round_trip = end - start;
        // The IPv6 reply structure has no TTL or data size field, so the TTL
        // stays at -1 (not shown) and the size is taken from what we sent.
        if (result.status == EchoStatus::Reply) {
            result.bytes = size + kIcmpHeaderSize;
        }
        return result;
    }

    HANDLE handle_;
    IpAddress target_;
};

}  // namespace

// Factory declared in icmp_client.hpp. Each platform file defines its own
// version, so the rest of the program never knows which one it is using.
// Opens an IPv4 or IPv6 ICMP handle depending on the target's family.
std::unique_ptr<IcmpClient> IcmpClient::create(const IpAddress& target) {
    const HANDLE handle = target.family() == Family::IPv4 ? IcmpCreateFile() : Icmp6CreateFile();
    if (handle == INVALID_HANDLE_VALUE) {
        throw NetworkError("cannot open the Windows ICMP service (error " +
                           std::to_string(GetLastError()) + ")");
    }
    return std::make_unique<WindowsIcmpClient>(handle, target);
}

}  // namespace netpulse