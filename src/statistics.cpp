// statistics.cpp
// Keeps the running totals for one ping session and computes the figures
// shown in the final summary: packets sent and received, packet loss, and
// the minimum / average / maximum / standard deviation of the round-trip time.
// Nothing is stored per packet; only a few running sums are kept, so memory
// use stays constant no matter how long the session runs.

#include "../include/statistics.hpp"

#include <algorithm>
#include <cmath>

namespace netpulse {

// Called once for every request that got an echo reply.
// Counts it as transmitted and received, and folds its round-trip time
// into the running min, max, sum and sum of squares.
void Statistics::record_reply(double round_trip_ms) noexcept {
    ++transmitted_;
    if (received_ == 0) {
        // First reply: it is both the minimum and the maximum so far.
        min_ = round_trip_ms;
        max_ = round_trip_ms;
    } else {
        min_ = std::min(min_, round_trip_ms);
        max_ = std::max(max_, round_trip_ms);
    }
    ++received_;
    sum_ += round_trip_ms;
    // Sum of squares is needed later for the standard deviation.
    sum_of_squares_ += round_trip_ms * round_trip_ms;
}

// Called for every request that got no reply (timeout, TTL exceeded,
// destination unreachable). It counts as sent but not received.
void Statistics::record_loss() noexcept {
    ++transmitted_;
}

// Percentage of requests that got no reply, e.g. 4 sent and 3 received -> 25.0.
// Returns 0 when nothing was sent so there is no division by zero.
double Statistics::loss_percent() const noexcept {
    if (transmitted_ == 0) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(transmitted_ - received_) /
           static_cast<double>(transmitted_);
}

// Mean round-trip time of the received replies, 0 when there were none.
double Statistics::average_ms() const noexcept {
    return received_ == 0 ? 0.0 : sum_ / static_cast<double>(received_);
}

// Population standard deviation of the round-trip times, computed from the
// running sums with the formula  variance = mean(x^2) - mean(x)^2.
// This avoids storing every sample. Floating-point rounding can make the
// variance slightly negative when all samples are equal, so anything
// non-positive is reported as 0.
double Statistics::stddev_ms() const noexcept {
    if (received_ == 0) {
        return 0.0;
    }
    const double mean = average_ms();
    const double variance = sum_of_squares_ / static_cast<double>(received_) - mean * mean;
    return variance > 0.0 ? std::sqrt(variance) : 0.0;
}

}  // namespace netpulse