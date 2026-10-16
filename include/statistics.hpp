// statistics.hpp
// Declares the Statistics class: the running totals for one ping session
// and the figures derived from them for the final summary. The class keeps
// a handful of running sums rather than every sample, so it uses constant
// memory however long the session runs. The implementation is in
// statistics.cpp.

#pragma once

#include <cstddef>

namespace netpulse {

/// Running totals for one ping session.
class Statistics {
public:
    // --- Feeding in results, one call per echo request ---

    /// Counts a request that was answered after `round_trip_ms` milliseconds.
    void record_reply(double round_trip_ms) noexcept;

    /// Counts a request that was not answered by the target.
    /// (Timeouts, "TTL exceeded" and "unreachable" all count as a loss.)
    void record_loss() noexcept;

    // --- Reading the figures out ---

    // Total requests sent, and how many of them got an echo reply.
    std::size_t transmitted() const noexcept { return transmitted_; }
    std::size_t received() const noexcept { return received_; }

    /// Percentage of requests without a reply, 0 when nothing was sent.
    double loss_percent() const noexcept;

    /// Round-trip times in milliseconds. All are 0 when nothing was received.
    // min and max are simple enough to be defined right here in the header;
    // average and standard deviation need a little arithmetic and live in
    // the .cpp file.
    double min_ms() const noexcept { return received_ == 0 ? 0.0 : min_; }
    double max_ms() const noexcept { return received_ == 0 ? 0.0 : max_; }
    double average_ms() const noexcept;
    double stddev_ms() const noexcept;

private:
    // Counters.
    std::size_t transmitted_ = 0;
    std::size_t received_ = 0;
    // Smallest and largest round-trip time seen so far.
    double min_ = 0.0;
    double max_ = 0.0;
    // Sum of all round-trip times (for the average) and sum of their
    // squares (for the standard deviation).
    double sum_ = 0.0;
    double sum_of_squares_ = 0.0;
};

}  // namespace netpulse