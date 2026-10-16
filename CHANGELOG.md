# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-10-13

### Added

- ICMP echo ("ping") over IPv4 and IPv6.
- Linux backend with hand-built ICMP packets on a raw socket, and an automatic
  fallback to unprivileged ICMP datagram sockets.
- Windows backend on the IP Helper ICMP API, which needs no administrator rights.
- Reporting of "time to live exceeded" and "destination unreachable" answers.
- Options for count, interval, timeout, payload size, TTL and address family.
- Round-trip statistics: minimum, average, maximum, standard deviation, packet loss.
- Unit tests, command-line tests and a loopback test.
- CMake build for Linux and Windows, CI and release workflows.
