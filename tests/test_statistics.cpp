// test_statistics.cpp
// Unit tests for the Statistics class (statistics.cpp).
// Floating-point results are compared with doctest::Approx, which allows a
// tiny rounding difference instead of demanding bit-for-bit equality.

#include <doctest.h>

#include <initializer_list>

#include "../include/statistics.hpp"

using netpulse::Statistics;

// Nothing recorded: every figure is zero and nothing divides by zero.
TEST_CASE("empty statistics report zeros") {
    const Statistics statistics;
    CHECK(statistics.transmitted() == 0);
    CHECK(statistics.received() == 0);
    CHECK(statistics.loss_percent() == doctest::Approx(0.0));
    CHECK(statistics.min_ms() == doctest::Approx(0.0));
    CHECK(statistics.average_ms() == doctest::Approx(0.0));
    CHECK(statistics.max_ms() == doctest::Approx(0.0));
    CHECK(statistics.stddev_ms() == doctest::Approx(0.0));
}

// The classic textbook data set {2,4,4,4,5,5,7,9}: mean 5, population
// standard deviation exactly 2. Also min 2, max 9, no loss.
TEST_CASE("replies update minimum, average, maximum and deviation") {
    Statistics statistics;
    for (const double rtt : {2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0}) {
        statistics.record_reply(rtt);
    }

    CHECK(statistics.transmitted() == 8);
    CHECK(statistics.received() == 8);
    CHECK(statistics.loss_percent() == doctest::Approx(0.0));
    CHECK(statistics.min_ms() == doctest::Approx(2.0));
    CHECK(statistics.max_ms() == doctest::Approx(9.0));
    CHECK(statistics.average_ms() == doctest::Approx(5.0));
    CHECK(statistics.stddev_ms() == doctest::Approx(2.0));
}

// Mixed replies and losses: losses raise the transmitted count and the
// loss percentage but must not affect the timing average.
TEST_CASE("losses count as transmitted but not received") {
    Statistics statistics;
    statistics.record_reply(10.0);
    statistics.record_loss();
    statistics.record_loss();
    statistics.record_reply(20.0);

    CHECK(statistics.transmitted() == 4);
    CHECK(statistics.received() == 2);
    CHECK(statistics.loss_percent() == doctest::Approx(50.0));
    CHECK(statistics.average_ms() == doctest::Approx(15.0));
}

// Only losses: 100% loss, and the timing getters return 0 rather than
// garbage from uninitialised min/max.
TEST_CASE("total loss leaves the timing figures at zero") {
    Statistics statistics;
    statistics.record_loss();
    statistics.record_loss();
    statistics.record_loss();

    CHECK(statistics.loss_percent() == doctest::Approx(100.0));
    CHECK(statistics.min_ms() == doctest::Approx(0.0));
    CHECK(statistics.max_ms() == doctest::Approx(0.0));
    CHECK(statistics.stddev_ms() == doctest::Approx(0.0));
}

// One sample: min == max == that sample, and the deviation is 0. The
// variance formula mean(x^2) - mean(x)^2 can produce a tiny negative or
// positive rounding residue here, which is why stddev_ms clamps it and why
// the comparison uses an explicit epsilon.
TEST_CASE("a single reply has zero deviation") {
    Statistics statistics;
    statistics.record_reply(0.123);
    CHECK(statistics.min_ms() == doctest::Approx(0.123));
    CHECK(statistics.max_ms() == doctest::Approx(0.123));
    CHECK(statistics.stddev_ms() == doctest::Approx(0.0).epsilon(1e-6));
}