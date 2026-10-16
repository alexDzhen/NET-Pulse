# NET-Pulse

An implementation of `ping` in C++17 for Linux and Windows. It sends ICMP echo
requests to a host, waits for the replies and reports round-trip times and packet loss.

*Български: [README.bg.md](README.bg.md)*

```console
$ netpulse -c 4 10.0.3.2
PING 10.0.3.2 (10.0.3.2): 56 data bytes
64 bytes from 10.0.3.2: icmp_seq=1 ttl=62 time=0.058 ms
64 bytes from 10.0.3.2: icmp_seq=2 ttl=62 time=0.087 ms
64 bytes from 10.0.3.2: icmp_seq=3 ttl=62 time=0.070 ms
64 bytes from 10.0.3.2: icmp_seq=4 ttl=62 time=0.068 ms

--- 10.0.3.2 ping statistics ---
4 packets transmitted, 4 received, 0% packet loss
round-trip min/avg/max/stddev = 0.058/0.070/0.087/0.010 ms
```

## Features

- IPv4 and IPv6, host names or numeric addresses.
- On Linux the ICMP packets are built and parsed by hand, including the checksum.
- Runs without root on Linux when the system allows unprivileged ICMP sockets,
  and without administrator rights on Windows.
- Reports what went wrong and who said so: timeouts, "time to live exceeded" and
  "destination unreachable" answers from routers.
- Configurable count, interval, timeout, payload size and TTL.
- Summary with packet loss and minimum, average, maximum and standard deviation.
- Ctrl+C stops the run and still prints the summary. On Windows this happens once
  the request in flight has been answered or has timed out.
- No third-party dependencies.

## Quick start

You need CMake 3.20 or newer and a C++17 compiler.

**Linux**

```sh
git clone https://github.com/alexDzhen/NET-Pulse.git
cd NET-Pulse
./scripts/build.sh
sudo ./build/netpulse 127.0.0.1
```

To install it and run it without `sudo` from then on:

```sh
./scripts/install.sh
netpulse example.com
```

**Windows**

Install Visual Studio 2022 with the "Desktop development with C++" workload and run
the commands in "Developer PowerShell for VS 2022", where `cmake` is already available.

```powershell
git clone https://github.com/alexDzhen/NET-Pulse.git
cd NET-Pulse
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
.\build\Release\netpulse.exe 127.0.0.1
```

The scripts run these three commands, which you can also type yourself:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Ready-made binaries for both systems are attached to every
[release](https://github.com/alexDzhen/netpulse/releases). The Linux binary is built
on GitHub's current Ubuntu image and needs a distribution at least that recent. On
older systems, build from source.

## Usage

```text
netpulse [options] <host>

  -c, --count <n>        Number of requests to send (default 4, 0 = until Ctrl+C)
  -i, --interval <sec>   Seconds between requests (default 1)
  -W, --timeout <sec>    Seconds to wait for each reply (default 2)
  -s, --size <bytes>     Payload size in bytes (default 56)
  -t, --ttl <n>          Time to live of outgoing packets, 1-255
  -4                     Use IPv4 only
  -6                     Use IPv6 only
  -q, --quiet            Print only the summary
  -h, --help             Show help and exit
  -V, --version          Show the version and exit
```

A host that does not answer:

```console
$ netpulse -c 2 -W 1 10.8.8.8
PING 10.8.8.8 (10.8.8.8): 56 data bytes
Request timed out: icmp_seq=1
Request timed out: icmp_seq=2

--- 10.8.8.8 ping statistics ---
2 packets transmitted, 0 received, 100% packet loss
```

A TTL too small to reach the target. The router that dropped the packet says so:

```console
$ netpulse -c 2 -t 1 10.0.3.2
PING 10.0.3.2 (10.0.3.2): 56 data bytes
From 10.0.1.1: icmp_seq=1 Time to live exceeded
From 10.0.1.1: icmp_seq=2 Time to live exceeded

--- 10.0.3.2 ping statistics ---
2 packets transmitted, 0 received, 100% packet loss
```

Only the summary, ten requests, five per second:

```console
$ netpulse -q -c 10 -i 0.2 10.0.3.2
PING 10.0.3.2 (10.0.3.2): 56 data bytes

--- 10.0.3.2 ping statistics ---
10 packets transmitted, 10 received, 0% packet loss
round-trip min/avg/max/stddev = 0.056/0.081/0.122/0.018 ms
```

The examples were recorded on a small test network with two routers between the
source and `10.0.3.2`.

| Exit code | Meaning                                         |
|-----------|-------------------------------------------------|
| 0         | At least one reply arrived                      |
| 1         | No reply arrived                                |
| 2         | Usage error, unknown host or no permission      |

## Permissions on Linux

Sending ICMP needs a special socket. `netpulse` tries two kinds, in this order:

1. A **raw socket**. It needs root or the `CAP_NET_RAW` capability.
2. An **ICMP datagram socket**. It needs no privileges, but the system must allow
   it through the `net.ipv4.ping_group_range` setting. Most current
   distributions do.

So one of these is enough:

```sh
sudo netpulse example.com                                   # run as root
sudo setcap cap_net_raw+ep /usr/local/bin/netpulse          # grant the capability once
sudo sysctl -w net.ipv4.ping_group_range="0 2147483647"     # allow unprivileged ICMP
```

`scripts/install.sh` does the second one for you. On Windows nothing is needed.

## How it works

### The echo request

`ping` uses ICMP, the control protocol that travels inside IP packets. An echo
request is an 8-byte header followed by a payload:

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+---------------+---------------+-------------------------------+
|     Type      |     Code      |           Checksum            |
+---------------+---------------+-------------------------------+
|          Identifier           |        Sequence Number        |
+---------------+---------------+-------------------------------+
|                          Payload ...                          |
+---------------------------------------------------------------+
```

| Field      | IPv4 request | IPv4 reply | IPv6 request | IPv6 reply |
|------------|--------------|------------|--------------|------------|
| Type       | 8            | 0          | 128          | 129        |
| Code       | 0            | 0          | 0            | 0          |

The target copies the identifier, sequence number and payload into its reply. The
identifier tells our replies apart from those of other programs, and the sequence
number tells which request a reply belongs to.

### The checksum

The checksum is the Internet checksum from RFC 1071: add the message up as
16-bit big-endian words, fold the carries back into the low 16 bits, and take
the one's complement. A receiver that runs the same sum over the message,
checksum included, gets zero if nothing was damaged. `netpulse` verifies this for
every IPv4 packet it reads from a raw socket.

For IPv6 the checksum also covers a pseudo-header with the source and destination
addresses, and the kernel always fills it in.

### One request, step by step

1. Build the echo request ([`icmp.cpp`](src/icmp.cpp)).
2. Note the time on a monotonic clock and send the packet.
3. Wait with `poll` until something arrives or the timeout expires.
4. Read the message and decide whether it is ours:
   - an **echo reply** with our identifier and sequence number, or
   - an **error** ("time exceeded", "destination unreachable"). Errors quote the
     start of the packet that caused them, so the identifier and sequence number
     are read from that quoted copy.
5. Anything else (other programs' pings, late replies) is ignored and the wait
   continues with the time that is left.
6. The round-trip time is the difference between the two clock readings.

### Linux and Windows backends

The public interface is [`IcmpClient`](include/icmp_client.hpp): send one
echo request, get one result. There are two implementations.

| | Linux | Windows |
|---|---|---|
| File | [`src/linux/icmp_client_linux.cpp`](src/linux/icmp_client_linux.cpp) | [`src/windows/icmp_client_windows.cpp`](src/windows/icmp_client_windows.cpp) |
| Mechanism | Raw socket, or ICMP datagram socket | IP Helper API (`IcmpSendEcho2`, `Icmp6SendEcho2`) |
| Who builds the packet | `netpulse` | Windows |
| Privileges | Root, `CAP_NET_RAW` or `ping_group_range` | None |

Windows restricts raw sockets to administrators, and its firewall drops incoming
ICMP error messages unless a rule allows them. The IP Helper API has neither
problem, which is why the system `ping.exe` and `tracert.exe` use it as well.

On a Linux datagram socket the kernel chooses the identifier and delivers router
errors through the socket's error queue instead of the normal receive path. The
backend handles both socket types.

Everything above the backend (option parsing, statistics, output) is shared.

## Install and package

```sh
./scripts/install.sh                       # Linux: /usr/local/bin + CAP_NET_RAW
cmake --install build --config Release --prefix C:\Tools\netpulse   # Windows
```

To produce a distributable archive (`.tar.gz` on Linux, `.zip` on Windows):

```sh
cpack --config build/CPackConfig.cmake -C Release -B dist
```

Pushing a tag such as `v1.0.0` runs the release workflow, which builds, tests and
attaches both archives to a GitHub release.

## Tests

The test suite uses [doctest](https://github.com/doctest/doctest), which is included
in `third_party/`, so nothing is downloaded during the build.

- Unit tests for the checksum, packet building and parsing, address formatting,
  statistics, output formatting and the command-line parser.
- Command-line tests that run the real binary.
- A loopback test that pings `127.0.0.1`. It is skipped when the system does not
  permit ICMP sockets for the current user.

```sh
ctest --test-dir build -C Release --output-on-failure
```

## Project layout

```text
netpulse/
├── app/main.cpp              Program entry point and the ping loop
├── include/                  Public headers
├── src/                      Implementation
│   ├── linux/                  Raw and datagram socket backend
│   ├── windows/                IP Helper backend
│   ├── platform/               sockaddr conversions
│   └── *.cpp                   Portable implementation
├── tests/                    Unit and command-line tests
├── third_party/doctest/      Test framework (single header)
├── scripts/                  Build and install scripts
└── .github/workflows/        CI and release pipelines
```

## License

[MIT](LICENSE)
