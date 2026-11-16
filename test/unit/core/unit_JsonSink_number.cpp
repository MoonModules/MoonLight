/// @module JsonSink

/// Pins that every value writeJsonNumber emits is parseable JSON.
///
/// JSON has no NaN and no infinity, and the whole-value test casts to long long, which is undefined behavior for those and for any magnitude past what a long long holds. A document carrying `nan` or `inf` is rejected wholesale by a browser's parser, so one bad reading would lose the entire state frame rather than the one field that produced it.

#include "doctest.h"
#include "unit/json_number.h"

#include <cmath>
#include <limits>
#include <string>

static std::string written(double v) {
    mm::JsonSink sink;
    mm::writeJsonNumber(sink, v);
    return std::string(sink.data(), sink.size());
}

TEST_CASE("a non-finite reading writes null rather than unparseable text") {
    CHECK(written(std::numeric_limits<double>::quiet_NaN()) == "null");
    CHECK(written(std::numeric_limits<double>::infinity()) == "null");
    CHECK(written(-std::numeric_limits<double>::infinity()) == "null");
}

TEST_CASE("a finite value past the integer range still writes a number") {
    // Too large for the long long cast, so it takes the fractional path rather than UB.
    const std::string s = written(1e300);
    CHECK(s != "null");
    CHECK(s.find("nan") == std::string::npos);
    CHECK(s.find("inf") == std::string::npos);
}

TEST_CASE("a whole value writes as an integer and a fraction compactly") {
    CHECK(written(42.0) == "42");
    CHECK(written(-7.0) == "-7");
    CHECK(written(0.5) == "0.5");
}
