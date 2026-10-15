#pragma once

#include "core/util/AudioFrame.h"

#include <cmath>     // log10 for the shared dB mapping
#include <cstddef>
#include <cstdint>

#include "core/util/math16.h"   // isqrt64 — the shared integer root

namespace mm {

/// The loudness path: a block of microphone samples in, one level byte out.
///
/// @technical
///
/// This is pure domain maths with no platform header, so it is host-tested without a device.
/// The platform owns only the read that produces the samples, the same host-testable shape the RMT and slot encoders take.
///
/// ## Why the scale is logarithmic
///
/// The magnitude-to-byte mapping is shared with the spectrum path, so a VU meter and a spectrum bar scale alike.
/// A magnitude is taken to decibels and mapped through a window onto the byte range.
///
/// The noise floor sets the window's bottom, and the gain is sensitivity in the intuitive direction: a higher gain narrows the window, so a sound fills more of the range.
///
/// Human hearing is logarithmic and these magnitudes span a huge range, so a linear map would crush the quiet or saturate the loud.
/// This is the standard fix.
///
/// ## Why the saturation bound is not the type's maximum
///
/// Casting a float outside the integer range is undefined, and a settling transient can briefly push the filter's output past the bounds, so it saturates first.
/// The positive bound is the largest float strictly below two to the thirty-first, not the type's maximum.
/// That maximum has no exact float and rounds up to a value itself out of range.
/// Clamping to it would still cast out of range.
/// The negative bound is exactly representable, so it needs no such care.
///
/// ## The three range constants
///
/// A learned follower credits its input with some minimum dynamic range, which exists only to bound the window it divides by; a collapsed follower would otherwise drive that toward infinity.
/// It is deliberately small, because the silence gate is what keeps a quiet room quiet and this is not a second mechanism for the same job.
/// A large value flattens real music instead: at twelve decibels a band swinging six filled only half the display, which reads as vivid bands with no dynamic range.
/// The gate can tell silence from a quiet passage, which a range clamp fundamentally cannot, so the gate does that work and this stays out of the way.
///
/// The level's own minimum is larger, for the same reason its gate sits lower: it follows a whole block's RMS, which swings far less than any single band's peak.
/// At the band's value the meter stretched that small natural variation to full scale and sat pinned at the top.
///
/// The level is scaled by gain rather than sized from it.
/// Gain sizes the band window directly, but a block RMS covers far more decibels than a single bin's peak.
/// Feeding one raw number to both left the meter in its bottom third at the settings that made the spectrum look right.
/// Scaling keeps the knob meaning what it means in both paths, and twenty decibels is the room's measured speech-to-quiet range on the bench parts.
///
/// ## Why the level's silence gate differs from a band's
///
/// A band gate reads a single bin's peak magnitude, while the level reads the whole block's RMS, which for real music sits well below the strongest bin.
/// Gating both at the band threshold left the spectrum lively with the meter pinned at zero.
///
/// The window floor is the level a manual setup displays, so it is audible by definition, and silence sits a fixed margin below it.
///
/// ## What an I2S microphone forces
///
/// Two facts about a MEMS part drive the maths, both straight from how it behaves rather than from any tuning recipe.
///
/// It carries a DC bias: the sample stream sits on a large constant offset.
/// A plain RMS would be dominated by the bias and a silent room would read as loud.
/// The block mean is subtracted first.
///
/// Its quietest output is hiss rather than zero, so a noise-floor threshold treats anything below it as silence and idle hiss never twitches the LEDs.
/// The gain then scales what is left.
///
/// The sample format is 24-bit signed data left-justified in a 32-bit slot, so the magnitude lives in the top bits.
/// An arithmetic shift lands it in an integer, and the accumulation is 64-bit so a full block cannot overflow.

/// The display window in dB: where it starts and how wide it is. One home for the two knobs.
inline float windowFloorDb(uint16_t noiseFloor) { return 60.0f + static_cast<float>(noiseFloor) * 0.5f; }
inline float windowSpanDb(uint16_t gain)        { return static_cast<float>(255 - gain) * 0.25f + 4.0f; }

/// Map a value already in decibels onto the byte range through the window.
inline uint8_t dbToByte(float db, uint16_t noiseFloor, uint16_t gain) {
    const float t = (db - windowFloorDb(noiseFloor)) / windowSpanDb(gain);
    if (t <= 0.0f) return 0;
    if (t >= 1.0f) return 255;
    return static_cast<uint8_t>(t * 255.0f);
}

inline uint8_t magToByte(float m, uint16_t noiseFloor, uint16_t gain) {
    if (m <= 1.0f) return 0;
    return dbToByte(20.0f * std::log10(m), noiseFloor, gain);
}

/// The standard one-pole high-pass that removes the DC offset and sub-bass rumble before analysis.
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
            // Saturate before narrowing: see the appendix on why this bound, not the type's max.
            const float clamped = y < -2147483648.0f ? -2147483648.0f
                                : y >  2147483520.0f ?  2147483520.0f : y;
            samples[i] = static_cast<int32_t>(clamped);
        }
    }
};

/// The narrowest dynamic range a learned follower credits its input with.
inline constexpr float kConditionerMinRangeDb = 3.0f;

/// The level's own minimum, larger than a band's because it follows a whole block's RMS.
inline constexpr float kLevelMinRangeDb = 20.0f;

/// The manual level window's width at full gain.
inline constexpr float kLevelWindowSpanDb = 20.0f;

/// The manual level window's span for a given gain, widening as the gain falls.
inline float levelWindowSpanDb(uint16_t gain) {
    return kLevelWindowSpanDb * (2.0f - static_cast<float>(gain) / 255.0f);
}

/// How far below the display window a level falls before it counts as silence.
inline constexpr float kMuteMarginDb = 20.0f;

/// The level's own floor and peak, learned the way a band's are.
struct LevelConditioner {
    static constexpr float kMinRangeDb = kLevelMinRangeDb;   ///< the narrowest window it will learn

    float floorDb = 0.0f;   ///< the learned bottom of the window
    float peakDb = 0.0f;    ///< the learned top of it
    bool  primed = false;   ///< whether a first block has seeded the two

    /// Learn from this block's RMS and return the dB window [floor, floor+span] to display it in.
    void observe(float db, uint32_t dtMs, float& windowFloor, float& windowSpan,
                 float floorRiseDbPerS = 1.0f, float peakReleaseDbPerS = 3.0f) {
        if (!primed) { floorDb = db; peakDb = db + kMinRangeDb; primed = true; }
        const float dt = static_cast<float>(dtMs) / 1000.0f;
        floorDb = db < floorDb ? db : floorDb + floorRiseDbPerS * dt;
        peakDb  = db > peakDb  ? db : peakDb - peakReleaseDbPerS * dt;
        if (peakDb < floorDb + kMinRangeDb) peakDb = floorDb + kMinRangeDb;
        windowFloor = floorDb;
        windowSpan  = peakDb - floorDb;
    }
};

inline void computeLevel(const int32_t* samples, size_t n,
                         uint16_t noiseFloor, uint16_t gain, AudioFrame& frame,
                         LevelConditioner* cond = nullptr, uint32_t dtMs = 23) {
    if (!samples || n == 0) {
        frame.level = 0;
        return;
    }

    // DC mean of the block. 64-bit sum: n * 2^23 fits easily.
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (samples[i] >> 8);
    const int64_t mean = sum / static_cast<int64_t>(n);

    // RMS of the DC-removed signal.
    uint64_t sqSum = 0;
    for (size_t i = 0; i < n; i++) {
        const int64_t v = (samples[i] >> 8) - mean;
        sqSum += static_cast<uint64_t>(v * v);
    }
    const uint64_t meanSq = sqSum / static_cast<uint64_t>(n);
    const uint64_t rms = isqrt64(meanSq);

    // Automatic learns the window from the level itself; manual takes the two sliders.
    if (cond) {
        const float db = rms <= 1 ? 0.0f : 20.0f * std::log10(static_cast<float>(rms));
        // The level's silence is not a band's: see the appendix on why the two gates differ.
        const float gateDb = windowFloorDb(noiseFloor) - kMuteMarginDb;
        if (db < gateDb) { frame.level = 0; return; }
        float wFloor = 0.0f, wSpan = 1.0f;
        cond->observe(db, dtMs, wFloor, wSpan);
        const float t = (db - wFloor) / wSpan;
        const float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        frame.level = static_cast<uint8_t>(clamped * 255.0f + 0.5f);
        return;
    }
    // Manual: the floor positions the window and the gain scales it, from the level's own base span.
    const float db = rms <= 1 ? 0.0f : 20.0f * std::log10(static_cast<float>(rms));
    const float wFloor = windowFloorDb(noiseFloor);
    const float t = (db - wFloor) / levelWindowSpanDb(gain);
    const float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    frame.level = rms <= 1 ? 0 : static_cast<uint8_t>(clamped * 255.0f + 0.5f);
}

} // namespace mm
