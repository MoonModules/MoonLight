/// @module WaveEffect

/// Pins WaveEffect's pure waveform map (phase → y) for each of the six shapes, the behavior that defines the effect. The animation/trail/color need a Layer + buffer (covered by the scenario run); here we drive waveYForTest directly, no grid.

#include "doctest.h"
#include "light/effects/WaveEffect.h"

using mm::lengthType;

TEST_CASE("WaveEffect: sawtooth ramps 0→top across the phase") {
    const lengthType h = 16;
    CHECK(mm::WaveEffect::waveYForTest(0, 0, h) == 0);            // phase 0 → bottom
    CHECK(mm::WaveEffect::waveYForTest(0, 255, h) == h - 1);      // phase max → top
    // Monotonic non-decreasing across the ramp.
    lengthType prev = 0;
    for (int p = 0; p <= 255; p += 17) {
        lengthType y = mm::WaveEffect::waveYForTest(0, static_cast<uint8_t>(p), h);
        CHECK(y >= prev);
        prev = y;
    }
}

TEST_CASE("WaveEffect: triangle peaks in the middle and returns") {
    const lengthType h = 16;
    CHECK(mm::WaveEffect::waveYForTest(1, 0, h) == 0);
    CHECK(mm::WaveEffect::waveYForTest(1, 128, h) >= h - 1);      // mid-phase → top
    CHECK(mm::WaveEffect::waveYForTest(1, 255, h) <= 1);          // back near the bottom
}

TEST_CASE("WaveEffect: sine sits mid at the zero crossings") {
    const lengthType h = 16;
    // sin8(0) = 128 → middle of the grid.
    CHECK(mm::WaveEffect::waveYForTest(2, 0, h) == (128 * h) / 256);
}

TEST_CASE("WaveEffect: square is low then high") {
    const lengthType h = 16;
    CHECK(mm::WaveEffect::waveYForTest(3, 0, h) == 0);            // low half → bottom
    CHECK(mm::WaveEffect::waveYForTest(3, 200, h) == h - 1);     // high half → top
}

TEST_CASE("WaveEffect: every type stays within the grid bounds") {
    for (uint8_t type = 0; type < mm::WaveEffect::kWaveformCount; type++) {
        for (int p = 0; p <= 255; p++) {
            const lengthType h = 8;
            lengthType y = mm::WaveEffect::waveYForTest(type, static_cast<uint8_t>(p), h);
            CHECK(y >= 0);
            CHECK(y < h);
        }
    }
}

TEST_CASE("WaveEffect: a zero-height grid never reads out of bounds") {
    CHECK(mm::WaveEffect::waveYForTest(2, 100, 0) == 0);
}
