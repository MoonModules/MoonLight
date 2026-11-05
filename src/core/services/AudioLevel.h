#pragma once

#include <cmath>     // log10, the level in decibels
#include <cstddef>
#include <cstdint>

#include "core/util/math16.h"   // isqrt64 — the shared integer root

namespace mm {

/// The loudness path: a block of microphone samples in, its level in dBFS out, and the high-pass that clears its offset first.
///
/// @technical
///
/// This is pure domain maths with no platform header, so it is host-tested without a device. The platform owns only the read that produces the samples; what the level means on the display is AudioDynamics'.
///
/// ## Why the saturation bound is not the type's maximum
///
/// Casting a float outside the integer range is undefined, and a settling transient can briefly push the filter's output past the bounds, so it saturates first.
///
/// The positive bound is the largest float strictly below two to the thirty-first: the type's maximum has no exact float and rounds up out of range. The negative bound is exact.

/// The standard one-pole high-pass that removes the DC offset before analysis.
struct DcBlocker {
    float xPrev = 0.0f;   ///< the previous input sample
    float yPrev = 0.0f;   ///< the previous output sample

    /// Forget the filter's state, so the next block starts clean.
    void reset() { xPrev = 0.0f; yPrev = 0.0f; }

    /// Filter `n` samples in place, `r` being the pole: higher means a lower cutoff.
    void process(int32_t* samples, size_t n, float r = 0.99f) {
        if (!samples) return;
        for (size_t i = 0; i < n; i++) {
            const float x = static_cast<float>(samples[i]);
            const float y = x - xPrev + r * yPrev;
            xPrev = x;
            yPrev = y;
            // Saturate before narrowing: @xref{why-the-saturation-bound-is-not-the-types-maximum}.
            const float clamped = y < -2147483648.0f ? -2147483648.0f
                                : y >  2147483520.0f ?  2147483520.0f : y;
            samples[i] = static_cast<int32_t>(clamped);
        }
    }
};

/// A block's RMS in dBFS, its mean removed: a full-scale sine reads 0, silence -120.
inline float blockLevelDbFs(const int32_t* samples, size_t n) {
    if (!samples || n == 0) return -120.0f;
    // The sample is 24-bit data left-justified in a 32-bit slot, so the magnitude lives in the top bits; a 64-bit sum cannot overflow.
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (samples[i] >> 8);
    const int64_t mean = sum / static_cast<int64_t>(n);
    uint64_t sqSum = 0;
    for (size_t i = 0; i < n; i++) {
        const int64_t v = (samples[i] >> 8) - mean;
        sqSum += static_cast<uint64_t>(v * v);
    }
    const uint64_t rms = isqrt64(sqSum / static_cast<uint64_t>(n));
    if (rms == 0) return -120.0f;
    // 2^23 is 24-bit full scale, and a sine's RMS sits 3 dB under its peak.
    return 20.0f * std::log10(static_cast<float>(rms) / 8388608.0f) + 3.0103f;
}

} // namespace mm
