/// @module Color

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "core/util/color.h"
#include "platform/platform.h"   // setTestNowMs, setTestClockStep: the shared test clock

// Every test case, and every rerun for its next subcase, starts on the real clock, so a test that froze or stepped it leaves nothing to the next.
struct RealClockPerTest : doctest::IReporter {
    explicit RealClockPerTest(const doctest::ContextOptions&) {}
    static void reset() { mm::platform::setTestNowMs(0); mm::platform::setTestClockStep(0); }
    void test_case_start(const doctest::TestCaseData&) override { reset(); }
    void test_case_reenter(const doctest::TestCaseData&) override { reset(); }
    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};
REGISTER_LISTENER("real clock per test", 1, RealClockPerTest);

// Hue 0 is pure red.
TEST_CASE("hsvToRgb red at h=0") {
    auto c = mm::hsvToRgb(0, 255, 255);
    CHECK(c.r == 255);
    CHECK(c.g == 0);
    CHECK(c.b == 0);
}

// Hue 85 (one third round the wheel) is pure green; a sliver of red is tolerated since 85 is approximate, not exact.
TEST_CASE("hsvToRgb near-green at h=85") {
    auto c = mm::hsvToRgb(85, 255, 255);
    CHECK(c.r < 5);
    CHECK(c.g == 255);
    CHECK(c.b == 0);
}

// Hue 170 (two thirds round) is pure blue.
TEST_CASE("hsvToRgb near-blue at h=170") {
    auto c = mm::hsvToRgb(170, 255, 255);
    CHECK(c.r == 0);
    CHECK(c.g < 10);
    CHECK(c.b == 255);
}

// Zero saturation produces a grey of the given value, regardless of hue.
TEST_CASE("hsvToRgb white when saturation is zero") {
    auto white = mm::hsvToRgb(0, 0, 255);
    CHECK(white.r == 255);
    CHECK(white.g == 255);
    CHECK(white.b == 255);

    auto gray = mm::hsvToRgb(128, 0, 128);
    CHECK(gray.r == 128);
    CHECK(gray.g == 128);
    CHECK(gray.b == 128);
}

// Zero value is black, regardless of hue or saturation.
TEST_CASE("hsvToRgb black when value is zero") {
    auto black = mm::hsvToRgb(0, 255, 0);
    CHECK(black.r == 0);
    CHECK(black.g == 0);
    CHECK(black.b == 0);
}

// A hue between the cardinal points blends two channels (here: orange = red + green).
TEST_CASE("hsvToRgb produces color at intermediate hue") {
    auto c = mm::hsvToRgb(42, 255, 255);
    CHECK(c.r > 0);
    CHECK(c.g > 0);
    CHECK(c.b == 0);
}

// `hsvToRgb` is `constexpr` — evaluable at compile time.
TEST_CASE("hsvToRgb is constexpr") {
    constexpr auto c = mm::hsvToRgb(0, 255, 255);
    static_assert(c.r == 255);
    static_assert(c.g == 0);
    static_assert(c.b == 0);
}

// `scale8(v, f)` multiplies two 8-bit values and returns 8 bits. Factor 255 is identity, factor 0 zeroes, factor 128 halves (within integer rounding).
TEST_CASE("scale8") {
    CHECK(mm::scale8(255, 255) == 255);
    CHECK(mm::scale8(255, 0) == 0);
    CHECK(mm::scale8(0, 255) == 0);
    CHECK(mm::scale8(255, 128) == 128);
    CHECK(mm::scale8(128, 128) == 64);
}

// `scale8` is also `constexpr`.
TEST_CASE("scale8 is constexpr") {
    constexpr auto v = mm::scale8(255, 128);
    static_assert(v == 128);
}
