/// @module AudioService

#include "doctest.h"
#include "core/services/AudioLevel.h"
#include "core/services/AudioService.h"
#include "core/util/ModuleFactory.h"
#include "light/effects/AudioSpectrumEffect.h"

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

// The success spec for the level path on synthesized blocks: DC offset is removed, the level reads in dBFS, and empty or degenerate input is crash-safe.

namespace {

// A pure sine of `cycles` periods across `n` samples at 24-bit-ish amplitude, left-justified into the int32 slot (<<8) the INMP441 produces, plus an optional DC bias to prove the bias is stripped.
std::vector<int32_t> sine(size_t n, double cycles, double amp24, double dc24 = 0.0) {
    constexpr double kPi = std::numbers::pi_v<double>;
    std::vector<int32_t> v(n);
    for (size_t i = 0; i < n; i++) {
        const double s = amp24 * std::sin(2.0 * kPi * cycles * static_cast<double>(i) / n) + dc24;
        // <<8 on a wider signed type, left-shifting a negative int32 is UB.
        const int64_t sample = static_cast<int64_t>(s) << 8;   // 24-bit into the high bits
        v[i] = static_cast<int32_t>(sample);
    }
    return v;
}

} // namespace

TEST_CASE("DcBlocker: a constant DC offset is filtered out") {
    mm::DcBlocker dc;
    std::vector<int32_t> s(1024, 100000);   // a big constant offset, no AC
    dc.process(s.data(), s.size());
    // After the filter settles, the output rides near zero (the DC is gone).
    int32_t lo = s[1023], hi = s[1023];
    for (size_t i = 900; i < s.size(); i++) {   // look past the transient
        if (s[i] < lo) lo = s[i];
        if (s[i] > hi) hi = s[i];
    }
    CHECK(std::abs(lo) < 2000);
    CHECK(std::abs(hi) < 2000);
}

TEST_CASE("DcBlocker: an audio tone passes through (DC removed, AC kept)") {
    mm::DcBlocker dc;
    // A mid-frequency sine on a big DC pedestal, the DC must go, the swing stay.
    const int32_t amp = 1 << 18;
    auto biased = sine(1024, 40, amp, 1 << 21);   // amp on a much larger DC pedestal
    dc.process(biased.data(), biased.size());
    int32_t lo = biased[512], hi = biased[512];
    for (size_t i = 512; i < biased.size(); i++) {     // past the transient
        if (biased[i] < lo) lo = biased[i];
        if (biased[i] > hi) hi = biased[i];
    }
    const int32_t swing = hi - lo;
    // Centerd near zero (DC removed): the midpoint is tiny vs the swing.
    CHECK(std::abs(lo + hi) < swing / 4);
    // The tone survived: the swing is on the order of the AC amplitude (<<8 slot).
    CHECK(swing > amp);
}

TEST_CASE("DcBlocker: reset clears state, null-safe") {
    mm::DcBlocker dc;
    std::vector<int32_t> s(64, 50000);
    dc.process(s.data(), s.size());
    dc.reset();
    CHECK(dc.xPrev == 0.0f);
    CHECK(dc.yPrev == 0.0f);
    dc.process(nullptr, 64);   // no crash
    CHECK(true);
}

TEST_CASE("AudioLevel: a full-scale sine reads 0 dBFS and each halving 6 dB less") {
    const auto full = sine(4096, 37.0, 8388607.0);
    const auto half = sine(4096, 37.0, 4194303.0);
    CHECK(mm::blockLevelDbFs(full.data(), full.size()) == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(mm::blockLevelDbFs(half.data(), half.size()) == doctest::Approx(-6.02f).epsilon(0.01));
}

TEST_CASE("AudioLevel: silence and pure DC read -120 dBFS, and a bias does not move a sine's level") {
    const auto silent = sine(512, 0.0, 0.0);
    const auto dc = sine(512, 0.0, 0.0, 300000.0);
    CHECK(mm::blockLevelDbFs(silent.data(), silent.size()) == -120.0f);
    CHECK(mm::blockLevelDbFs(dc.data(), dc.size()) == -120.0f);
    const auto clean = sine(4096, 37.0, 100000.0), biased = sine(4096, 37.0, 100000.0, 500000.0);
    CHECK(mm::blockLevelDbFs(biased.data(), biased.size()) == doctest::Approx(mm::blockLevelDbFs(clean.data(), clean.size())).epsilon(0.001));
}

TEST_CASE("AudioLevel: empty or null input reads silence, never a crash") {
    int32_t dummy = 0;
    CHECK(mm::blockLevelDbFs(nullptr, 512) == -120.0f);
    CHECK(mm::blockLevelDbFs(&dummy, 0) == -120.0f);
}

TEST_CASE("AudioLevel: isqrt64 matches floor(sqrt) on a spread of values") {
    const uint64_t xs[] = {0, 1, 2, 3, 4, 99, 100, 12345, 1ull << 40, (1ull << 46) + 7};
    for (uint64_t x : xs) {
        const uint64_t r = mm::isqrt64(x);
        CHECK(r * r <= x);
        CHECK((r + 1) * (r + 1) > x);
    }
}

// Regression: create() returns nullptr for an UNREGISTERED type, which boot-looped the S3 via main.cpp's markWiredByCode(). These pin that AudioService and the audio effects are createable and latestFrame() is never null.
TEST_CASE("AudioService + audio effects are registered and createable (boot-loop guard)") {
    mm::ModuleFactory::registerType<mm::AudioService>("AudioService");
    mm::ModuleFactory::registerType<mm::AudioSpectrumEffect>("AudioSpectrumEffect");

    auto* mic = mm::ModuleFactory::create("AudioService");
    REQUIRE(mic != nullptr);
    CHECK(mic->role() == mm::ModuleRole::Service);

    auto* spec = mm::ModuleFactory::create("AudioSpectrumEffect");
    REQUIRE(spec != nullptr);

    delete mic;
    delete spec;
}

TEST_CASE("AudioService::latestFrame is never null (silent frame with no active mic)") {
    const mm::AudioFrame* f = mm::AudioService::latestFrame();
    REQUIRE(f != nullptr);
    // With no mic having run setup(), it's the static silent frame.
    CHECK(f->level == 0);
    CHECK(f->peakHz == 0);
}
