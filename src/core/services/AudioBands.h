#pragma once

#include "core/util/AudioFrame.h"
#include "core/services/AudioDynamics.h"   // the window every band maps through
#include "core/util/math16.h"       // ballistic: the per-band meter shape
#include "core/services/FractionalOctaveBank.h"   // the bands' measurement

#include <cmath>     // band maths is inherently float
#include <cstddef>
#include <cstdint>

namespace mm {

/// The bands' post-processing: the third-octave bank's powers in, sixteen band bytes and a dominant peak out.
///
/// @technical
///
/// This is pure domain maths with no platform header, host-tested by feeding a synthesized sine through the bank and asserting the band it lights and the peak frequency it reports, all without a device.
///
/// ## Sixteen of the thirty-one
///
/// The sixteen are the third octaves WLED-MM's default GEQ picks, 40 Hz to 10 kHz, so both boards draw the same bars. A band's power is in dBFS, a full-scale sine at its center reading 0, the scale the level and the dynamics share.
///
/// ## The dominant frequency
///
/// The loudest band, refined by a parabola through its decibels and its two neighbors': the textbook peak interpolation, on bands rather than bins. Its readers map it to a hue or a place on a log scale, which this serves.
///
/// ## How an onset is decided
///
/// The textbook decision: flux against its own recent mean, not an absolute threshold, so a loud room and a quiet one fire on the same event. A refractory window makes one hit one onset.
///
/// The mean is an exponential average in fixed point, advanced after the decision so a hit does not raise the bar it is judged against. A hit is flux over a configurable fraction of that mean plus a margin.


/// A peak meter's ballistic over all sixteen bands: fast on a rise, slow on a fall.
constexpr uint8_t kBandRise = 200;
constexpr uint8_t kBandFall = 24;
inline void smoothBands(const uint8_t raw[16], uint8_t smoothed[16],
                        uint8_t rise = kBandRise, uint8_t fall = kBandFall) {
    for (uint8_t b = 0; b < 16; b++) smoothed[b] = ballistic(smoothed[b], raw[b], rise, fall);
}

/// How much the spectrum rose since the last block: the standard onset detection function.
inline uint8_t spectralFlux(const uint8_t prev[16], const uint8_t cur[16]) {
    uint32_t sum = 0;
    for (uint8_t b = 0; b < 16; b++) if (cur[b] > prev[b]) sum += static_cast<uint32_t>(cur[b] - prev[b]);
    sum /= 16;                                    // sixteen bands of 255 fold back onto 0..255
    return static_cast<uint8_t>(sum > 255 ? 255 : sum);
}

/// Turns a flux stream into onsets: one per hit, and none for a swell: @xref{how-an-onset-is-decided}.
struct OnsetDetector {
    uint32_t lastMs_ = 0;          ///< when the last onset fired, first so the struct packs to 8 bytes
    uint16_t mean_ = 0;            ///< EMA of the flux, 8.8 fixed point
    bool     fired_ = false;       ///< whether one has fired yet (lastMs_ of 0 is a valid time)

    /// Feed one block's flux, true on the block an onset is detected.
    bool feed(uint8_t flux, uint32_t nowMs, uint8_t num = 3, uint8_t den = 2,
              uint8_t margin = 20, uint16_t refractoryMs = 100) {
        const uint32_t meanNow = mean_ >> 8;
        const bool above = flux > (meanNow * num) / den + margin;
        mean_ = static_cast<uint16_t>(mean_ - (mean_ >> 4) + (static_cast<uint16_t>(flux) << 4));
        if (above && (!fired_ || nowMs - lastMs_ >= refractoryMs)) {
            lastMs_ = nowMs;
            fired_ = true;
            return true;
        }
        return false;
    }
};

/// The third-octave bank the bands are measured with.
using ThirdOctaveBank = FractionalOctaveBank<3>;

/// The semitone bank the harmony is measured with, counted from A440.
using SemitoneBank = FractionalOctaveBank<12>;
/// The notes' top: harmony lives under it.
inline constexpr float kSemitoneTopHz = 2500.0f;

/// The sixteen display bands as third-octave bands, WLED-MM's default GEQ pattern: @xref{sixteen-of-the-thirty-one}.
inline constexpr uint8_t kSixteenOfThirds[16] = {3, 5, 6, 8, 9, 11, 13, 14, 16, 17, 19, 20, 22, 24, 25, 27};

/// A power in dB, held at -120 so silence reads a number: one floor for every detector on the bands.
inline float powerDb(float power) { return 10.0f * std::log10(power > 1e-12f ? power : 1e-12f); }

/// A band's power in dBFS, a full-scale sine at its center reading 0; nothing audible reads -120.
inline float bandPowerDbFs(float power) {
    return powerDb(power * (2.0f / 70368744177664.0f));   // 2 / 2^46: a 24-bit full-scale sine's power, A * A / 2 with A = 2^23
}

/// The 31 band bytes and the sixteen picked from them, and the dominant peak, from the bank's powers through the dynamics, which this block's level and loudest band update first; answers how many of the 31 the bank measures, the ones above reading 0.
inline uint8_t bankToBands(const ThirdOctaveBank& bank, float levelDb, float dtMs, AudioDynamics& dyn,
                           uint8_t thirds[31], uint8_t bands[16], uint16_t& peakHz, uint16_t& peakMag) {
    for (uint8_t b = 0; b < 31; b++) thirds[b] = 0;
    for (uint8_t b = 0; b < 16; b++) bands[b] = 0;
    peakHz = 0;
    peakMag = 0;
    const uint8_t count = bank.bandCount();
    if (count == 0) return 0;

    float db[31];
    uint8_t loudest = 0;
    for (uint8_t b = 0; b < count; b++) {
        db[b] = bandPowerDbFs(bank.power(b));
        if (db[b] > db[loudest]) loudest = b;
    }
    dyn.update(levelDb, db[loudest], dtMs);
    for (uint8_t b = 0; b < count; b++) thirds[b] = dyn.toByte(db[b]);
    for (uint8_t b = 0; b < 16; b++) bands[b] = thirds[kSixteenOfThirds[b]];

    if (!dyn.gateOpen) return count;
    float offset = 0.0f;   // in bands, from the loudest band's center: @xref{the-dominant-frequency}
    if (loudest > 0 && loudest + 1 < count) {
        const float curve = db[loudest - 1] - 2.0f * db[loudest] + db[loudest + 1];
        if (curve < 0.0f) offset = 0.5f * (db[loudest - 1] - db[loudest + 1]) / curve;
        if (offset > 0.5f) offset = 0.5f;
        if (offset < -0.5f) offset = -0.5f;
    }
    peakHz = static_cast<uint16_t>(bank.centerHz(loudest) * std::exp2(offset / 3.0f));
    peakMag = dyn.toByte(db[loudest]);
    return count;
}

/// The last third-octave band of the bass and of the mids: 125 Hz and 2 kHz, the treble from 2.5 kHz up.
inline constexpr uint8_t kBassTopThird = 8, kMidTopThird = 20;

/// The range of a third-octave band: 0 the bass, 1 the mids, 2 the treble.
constexpr uint8_t rangeOfThird(uint8_t third) { return third <= kBassTopThird ? 0 : third <= kMidTopThird ? 1 : 2; }

/// The bass, mid and treble of a frame, each its loudest band on its bars' own scale, from the thirds or else the sixteen, so a frame from audio sync carries them too.
inline void bandsToRanges(AudioFrame& f) {
    uint8_t* out[3] = {&f.bass, &f.mid, &f.treble};
    for (uint8_t* o : out) *o = 0;
    if (f.thirdsCount > 0)
        for (uint8_t b = 0; b < f.thirdsCount && b < 31; b++) { uint8_t& o = *out[rangeOfThird(b)]; o = f.thirds[b] > o ? f.thirds[b] : o; }
    else
        for (uint8_t b = 0; b < 16; b++) { uint8_t& o = *out[rangeOfThird(kSixteenOfThirds[b])]; o = f.bands[b] > o ? f.bands[b] : o; }
}

} // namespace mm
