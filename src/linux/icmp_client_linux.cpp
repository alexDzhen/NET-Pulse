// icmp_client_linux.cpp
// The Linux implementation of IcmpClient. Only compiled on Linux.
//
// Unlike Windows, here the program builds the ICMP packet itself (icmp.cpp),
// sends it through a socket and reads the raw bytes that come back. Two
// kinds of socket are supported:
//
//   raw socket (SOCK_RAW)      Needs root or CAP_NET_RAW. The kernel hands us
//                              every ICMP packet that arrives, IPv4 header
//                              included, and we filter out the ones that are
//                              ours by identifier and sequence number.
//
//   datagram socket (SOCK_DGRAM, "ping socket")
//                              Works without privileges when the sysctl
//                              net.ipv4.ping_group_range allows it. The kernel
//                              strips the IP header, assigns the identifier
//                              itself and delivers only our own replies. ICMP
//                              errors from routers (TTL exceeded, unreachable)
//                              do not arrive as normal data; they are queued
//                              on the socket's error queue and read with
//                              MSG_ERRQUEUE after enabling IP_RECVERR.
//
// create() tries raw first and falls back to datagram, and the `raw_` flag
// tells the rest of the class which of the two it got.

#include "icmp_client.hpp"

#include <linux/errqueue.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

#include "../../include/icmp.hpp"
#include "platform/socket_address.hpp"

namespace netpulse {

namespace {

// steady_clock never jumps (unlike the wall clock), so it is the right one
// for measuring round-trip time.
using Clock = std::chrono::steady_clock;

// Largest possible IP packet, so no reply is ever truncated.
constexpr std::size_t kReceiveBufferSize = 65536;
// Room for the "control messages" the kernel attaches to a received packet
// (the TTL, and the error details on the error queue).
constexpr std::size_t kControlBufferSize = 512;

// Text for an errno value, e.g. ENETUNREACH -> "Network is unreachable".
std::string error_text(int error) {
    return std::generic_category().message(error);
}

// RAII wrapper around a socket file descriptor: whoever owns the object owns
// the descriptor, and close() runs automatically when the object dies, even
// if an exception is thrown. Copying is forbidden (two owners would close it
// twice); moving transfers ownership and leaves the source with -1.
class FileDescriptor {
public:
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    ~FileDescriptor() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    FileDescriptor& operator=(FileDescriptor&&) = delete;

    int get() const noexcept { return fd_; }

private:
    int fd_;
};

// Everything recvmsg() needs and fills in for one received packet:
// the sender's address, the data buffer description, and the control
// (ancillary) data. `alignas(cmsghdr)` because the CMSG_* macros walk the
// control buffer as an array of cmsghdr structs and need it aligned.
struct ReceivedDatagram {
    ssize_t size = -1;
    sockaddr_storage sender{};
    msghdr header{};
    iovec buffer{};
    alignas(cmsghdr) std::array<char, kControlBufferSize> control{};
};

class LinuxIcmpClient final : public IcmpClient {
public:
    // Takes ownership of the already-opened socket and configures it.
    // The identifier is the process id, the traditional choice: it makes
    // replies to this process distinguishable from replies to another ping
    // running at the same time (on a raw socket, which sees all of them).
    LinuxIcmpClient(FileDescriptor socket, bool raw, const IpAddress& target)
        : socket_(std::move(socket)),
          raw_(raw),
          family_(target.family()),
          target_(platform::to_socket_address(target)),
          identifier_(static_cast<std::uint16_t>(::getpid() & 0xFFFF)),
          buffer_(kReceiveBufferSize) {
        // Ask the kernel to attach the TTL / hop limit of each received packet
        // as a control message, so it can be shown in the output.
        // On a datagram socket also enable IP_RECVERR, which routes ICMP
        // errors from routers to the error queue instead of silently
        // dropping them.
        const int on = 1;
        if (family_ == Family::IPv4) {
            ::setsockopt(socket_.get(), IPPROTO_IP, IP_RECVTTL, &on, sizeof(on));
            if (!raw_) {
                ::setsockopt(socket_.get(), IPPROTO_IP, IP_RECVERR, &on, sizeof(on));
            }
        } else {
            ::setsockopt(socket_.get(), IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &on, sizeof(on));
            if (!raw_) {
                ::setsockopt(socket_.get(), IPPROTO_IPV6, IPV6_RECVERR, &on, sizeof(on));
            }
        }
    }

    // Sends one echo request and waits for its answer, the timeout, or an
    // interrupt (Ctrl+C). This is the heart of the file.
    EchoResult echo(const EchoRequest& request) override {
        EchoResult failure;
        failure.status = EchoStatus::Error;

        if (!apply_ttl(request.ttl)) {
            failure.error = "cannot set the time to live: " + error_text(errno);
            return failure;
        }
        // Errors left over from a previous request must not be mistaken
        // for an answer to this one.
        if (!raw_) {
            discard_stale_errors();
        }

        const std::vector<std::uint8_t> packet =
            icmp::build_echo_request(family_, identifier_, request.sequence, request.payload_size);

        // Timing starts just before the send; the deadline is when we give up.
        const Clock::time_point start = Clock::now();
        const Clock::time_point deadline = start + request.timeout;

        if (::sendto(socket_.get(), packet.data(), packet.size(), 0, target_.get(),
                     target_.length) < 0) {
            failure.error = "cannot send: " + error_text(errno);
            return failure;
        }

        // Wait loop. Each pass sleeps in poll() until the socket has
        // something to read or the remaining time runs out. The loop repeats
        // because what arrives may be a packet that is not ours (another
        // program's ping, a late reply to an earlier sequence number), in
        // which case we keep waiting with whatever time is left.
        while (true) {
            if (interrupted()) {
                EchoResult result;
                result.status = EchoStatus::Interrupted;
                return result;
            }

            const Clock::time_point now = Clock::now();
            if (now >= deadline) {
                // A default EchoResult has status Timeout.
                return EchoResult{};
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

            // poll() blocks until the socket is readable (POLLIN), has a
            // queued error (POLLERR, reported even though we did not ask for
            // it), or the timeout in milliseconds passes. The +1 rounds up so
            // we never wake a fraction early and spin.
            pollfd descriptor{socket_.get(), POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()) + 1);
            const Clock::time_point received_at = Clock::now();

            if (ready < 0) {
                // EINTR: a signal (e.g. Ctrl+C) interrupted the wait. Loop
                // around; the interrupted() check at the top will notice.
                if (errno == EINTR) {
                    continue;
                }
                failure.error = "cannot wait for a reply: " + error_text(errno);
                return failure;
            }
            if (ready == 0) {
                continue;
            }

            // Something is there. Check the error queue first (router
            // errors on a datagram socket), then normal data.
            std::optional<EchoResult> result;
            if ((descriptor.revents & POLLERR) != 0) {
                result = read_error(request);
            }
            if (!result && (descriptor.revents & POLLIN) != 0) {
                result = read_reply(request);
            }
            if (result) {
                result->round_trip = received_at - start;
                return *result;
            }
        }
    }

private:
    // Sets the TTL (IPv4) / hop limit (IPv6) of outgoing packets. 0 means
    // "leave the system default". The value is cached so the system call is
    // only made when it changes (traceroute changes it every hop, ping never).
    bool apply_ttl(int ttl) {
        if (ttl <= 0 || ttl == current_ttl_) {
            return true;
        }
        const int result =
            family_ == Family::IPv4
                ? ::setsockopt(socket_.get(), IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl))
                : ::setsockopt(socket_.get(), IPPROTO_IPV6, IPV6_UNICAST_HOPS, &ttl, sizeof(ttl));
        if (result != 0) {
            return false;
        }
        current_ttl_ = ttl;
        return true;
    }

    // Reads one packet (or one error-queue entry when flags contains
    // MSG_ERRQUEUE) into buffer_, with the sender address and control data
    // going into `datagram`. recvmsg is used instead of the simpler recvfrom
    // because only recvmsg returns control messages. MSG_DONTWAIT makes it
    // return immediately if nothing is there (size becomes -1) rather than
    // blocking; poll() has already told us whether to expect data.
    void receive(ReceivedDatagram& datagram, int flags) {
        datagram.buffer.iov_base = buffer_.data();
        datagram.buffer.iov_len = buffer_.size();
        datagram.header.msg_name = &datagram.sender;
        datagram.header.msg_namelen = sizeof(datagram.sender);
        datagram.header.msg_iov = &datagram.buffer;
        datagram.header.msg_iovlen = 1;
        datagram.header.msg_control = datagram.control.data();
        datagram.header.msg_controllen = datagram.control.size();
        datagram.size = ::recvmsg(socket_.get(), &datagram.header, flags | MSG_DONTWAIT);
    }

    // Reading SO_ERROR clears the socket's pending error flag. Without this
    // a later sendto() could fail with the old error.
    void clear_socket_error() {
        int pending = 0;
        socklen_t length = sizeof(pending);
        ::getsockopt(socket_.get(), SOL_SOCKET, SO_ERROR, &pending, &length);
    }

    // Empties the error queue, dropping errors that belong to earlier
    // requests (e.g. a late "TTL exceeded" after we already timed out).
    void discard_stale_errors() {
        ReceivedDatagram datagram;
        do {
            receive(datagram, MSG_ERRQUEUE);
        } while (datagram.size >= 0);
        clear_socket_error();
    }

    // Is this message an answer to the request we are waiting for?
    // The sequence number must match. On a raw socket the identifier must
    // match too, since we see every process's ICMP traffic. On a datagram
    // socket the kernel chose the identifier (we do not know it) and already
    // filters by it, so only the sequence is checked.
    bool is_ours(const icmp::Message& message, const EchoRequest& request) const noexcept {
        if (message.sequence != request.sequence) {
            return false;
        }
        return !raw_ || message.identifier == identifier_;
    }

    // Maps the parsed message kind onto the public result status.
    static EchoStatus status_of(icmp::MessageKind kind) noexcept {
        switch (kind) {
            case icmp::MessageKind::EchoReply:
                return EchoStatus::Reply;
            case icmp::MessageKind::TimeExceeded:
                return EchoStatus::TimeExceeded;
            case icmp::MessageKind::DestinationUnreachable:
                return EchoStatus::DestinationUnreachable;
        }
        return EchoStatus::Error;
    }

    // Walks the control messages attached to a received packet looking for
    // the TTL / hop limit that IP_RECVTTL / IPV6_RECVHOPLIMIT asked for.
    // CMSG_FIRSTHDR / CMSG_NXTHDR / CMSG_DATA are the standard macros for
    // iterating that buffer. Returns -1 when the kernel did not report it.
    int reply_ttl(ReceivedDatagram& datagram) const {
        for (cmsghdr* control = CMSG_FIRSTHDR(&datagram.header); control != nullptr;
             control = CMSG_NXTHDR(&datagram.header, control)) {
            const bool is_ttl =
                (control->cmsg_level == IPPROTO_IP && control->cmsg_type == IP_TTL) ||
                (control->cmsg_level == IPPROTO_IPV6 && control->cmsg_type == IPV6_HOPLIMIT);
            if (is_ttl && control->cmsg_len >= CMSG_LEN(sizeof(int))) {
                int ttl = 0;
                std::memcpy(&ttl, CMSG_DATA(control), sizeof(ttl));
                return ttl;
            }
        }
        return -1;
    }

    // Reads one normal packet and decides whether it answers `request`.
    // Returns nothing for packets that are not ours, so echo() keeps waiting.
    std::optional<EchoResult> read_reply(const EchoRequest& request) {
        ReceivedDatagram datagram;
        receive(datagram, 0);
        if (datagram.size < 0) {
            return std::nullopt;
        }

        const std::uint8_t* data = buffer_.data();
        std::size_t size = static_cast<std::size_t>(datagram.size);

        // A raw IPv4 socket delivers the whole IP packet, so skip the IP
        // header (variable length) to get to the ICMP part, and verify the
        // ICMP checksum: a valid message sums to zero. (Raw IPv6 sockets and
        // datagram sockets deliver just the ICMP part, and the kernel has
        // already verified the checksum.)
        if (raw_ && family_ == Family::IPv4) {
            const auto header_size = icmp::ipv4_header_size(data, size);
            if (!header_size) {
                return std::nullopt;
            }
            data += *header_size;
            size -= *header_size;
            if (icmp::checksum(data, size) != 0) {
                return std::nullopt;
            }
        }

        const auto message = icmp::parse_message(family_, data, size);
        if (!message || !is_ours(*message, request)) {
            return std::nullopt;
        }

        // The sender is the target for an echo reply, or the router that
        // produced the error message.
        EchoResult result;
        result.status = status_of(message->kind);
        result.responder =
            platform::from_socket_address(reinterpret_cast<const sockaddr*>(&datagram.sender));
        result.ttl = reply_ttl(datagram);
        result.bytes = size;
        result.code = message->code;
        return result;
    }

    // Datagram sockets only: reads one entry from the error queue. The
    // kernel stores there a copy of the packet we sent (so its sequence
    // number can be checked) plus a sock_extended_err control message that
    // says what went wrong and, for ICMP errors, which router reported it.
    std::optional<EchoResult> read_error(const EchoRequest& request) {
        ReceivedDatagram datagram;
        receive(datagram, MSG_ERRQUEUE);
        if (datagram.size < 0) {
            clear_socket_error();
            return std::nullopt;
        }

        // The data part is our original echo request; bytes 6-7 hold the
        // sequence number we put there.
        const std::size_t size = static_cast<std::size_t>(datagram.size);
        if (size < icmp::kHeaderSize) {
            return std::nullopt;
        }
        const std::uint16_t sequence = static_cast<std::uint16_t>((buffer_[6] << 8) | buffer_[7]);
        if (sequence != request.sequence) {
            return std::nullopt;
        }

        // Find the IP_RECVERR / IPV6_RECVERR control message.
        for (cmsghdr* control = CMSG_FIRSTHDR(&datagram.header); control != nullptr;
             control = CMSG_NXTHDR(&datagram.header, control)) {
            const bool is_error =
                (control->cmsg_level == IPPROTO_IP && control->cmsg_type == IP_RECVERR) ||
                (control->cmsg_level == IPPROTO_IPV6 && control->cmsg_type == IPV6_RECVERR);
            if (!is_error || control->cmsg_len < CMSG_LEN(sizeof(sock_extended_err))) {
                continue;
            }

            sock_extended_err error{};
            std::memcpy(&error, CMSG_DATA(control), sizeof(error));

            EchoResult result;
            if (error.ee_origin == SO_EE_ORIGIN_ICMP || error.ee_origin == SO_EE_ORIGIN_ICMP6) {
                // The error came from an ICMP message sent by a router.
                // ee_type / ee_code are the ICMP type and code fields.
                const bool v4 = error.ee_origin == SO_EE_ORIGIN_ICMP;
                if (error.ee_type == (v4 ? icmp::kV4TimeExceeded : icmp::kV6TimeExceeded)) {
                    result.status = EchoStatus::TimeExceeded;
                } else if (error.ee_type == (v4 ? icmp::kV4DestinationUnreachable
                                                : icmp::kV6DestinationUnreachable)) {
                    result.status = EchoStatus::DestinationUnreachable;
                } else {
                    return std::nullopt;
                }
                result.code = error.ee_code;

                // The router's address follows the sock_extended_err struct
                // in the same control message (what SO_EE_OFFENDER returns).
                const std::size_t offender_size = v4 ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
                if (control->cmsg_len >= CMSG_LEN(sizeof(error) + offender_size)) {
                    sockaddr_storage offender{};
                    std::memcpy(&offender, CMSG_DATA(control) + sizeof(error), offender_size);
                    result.responder =
                        platform::from_socket_address(reinterpret_cast<const sockaddr*>(&offender));
                }
            } else {
                // A local error (e.g. the network went away), reported as errno.
                result.status = EchoStatus::Error;
                result.error = error_text(static_cast<int>(error.ee_errno));
            }
            return result;
        }
        return std::nullopt;
    }

    FileDescriptor socket_;
    bool raw_;
    Family family_;
    platform::SocketAddress target_;
    std::uint16_t identifier_;
    std::vector<std::uint8_t> buffer_;
    int current_ttl_ = 0;
};

}  // namespace

// Factory declared in icmp_client.hpp; the Windows file defines its own
// version. Opens the socket, preferring raw and falling back to a datagram
// ("ping") socket when raw is not permitted. SOCK_CLOEXEC stops the
// descriptor from leaking into any child process.
std::unique_ptr<IcmpClient> IcmpClient::create(const IpAddress& target) {
    const bool v4 = target.family() == Family::IPv4;
    const int domain = v4 ? AF_INET : AF_INET6;
    const int protocol = v4 ? static_cast<int>(IPPROTO_ICMP) : static_cast<int>(IPPROTO_ICMPV6);

    bool raw = true;
    int fd = ::socket(domain, SOCK_RAW | SOCK_CLOEXEC, protocol);
    if (fd < 0 && (errno == EPERM || errno == EACCES)) {
        raw = false;
        fd = ::socket(domain, SOCK_DGRAM | SOCK_CLOEXEC, protocol);
    }

    // Neither worked: explain what the user can do about it.
    if (fd < 0) {
        const int error = errno;
        if (error == EPERM || error == EACCES) {
            throw NetworkError(
                "permission denied while opening an ICMP socket.\n"
                "Use one of the following:\n"
                "  sudo netpulse ...                                        run as root\n"
                "  sudo setcap cap_net_raw+ep <path to netpulse>            grant the capability "
                "once\n"
                "  sudo sysctl -w net.ipv4.ping_group_range=\"0 2147483647\"  allow unprivileged "
                "ICMP");
        }
        if (error == EAFNOSUPPORT || error == EPROTONOSUPPORT) {
            throw NetworkError(std::string{v4 ? "IPv4" : "IPv6"} +
                               " ICMP sockets are not available on this system");
        }
        throw NetworkError("cannot open an ICMP socket: " + error_text(error));
    }

    return std::make_unique<LinuxIcmpClient>(FileDescriptor{fd}, raw, target);
}

}  // namespace netpulse
