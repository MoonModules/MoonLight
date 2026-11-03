#pragma once

#include "core/util/AudioFrame.h"
#include "core/services/AudioLevel.h"   // magToByte: the shared log/dB mapping
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
/// The sixteen display bands are third-octave bands picked as WLED-MM's default GEQ pattern picks them, 40 Hz to 10 kHz, so a MoonLight board and a WLED-MM board draw the same bars and a WLED receiver reads the same sixteen from either.
/// A band's power is put on the decibel scale the display window and the conditioner are tuned on: a tone of amplitude A reads 20 log10(128 A), what a Hann-windowed 512-point transform's peak bin read for it.
///
/// ## The dominant frequency
///
/// The loudest band, its position refined by a parabola through its decibels and its two neighbors': the textbook peak interpolation, here on bands rather than bins.
/// A tone between two band centers reads close to where it sits, and its readers map it to a hue or a place on a log scale, which a third of an octave's interpolation serves.
///
/// ## What a band conditioner learns
///
/// Every band carries two learned numbers.
/// Its floor is the level it reads with no program material: the microphone's own noise, mains hum and the room.
/// That is followed as a running minimum which drifts up slowly, so a room growing noisier is re-learned.
/// Its peak is followed with an instant attack and a release of seconds, so it settles to the band's typical loudest level rather than tracking every beat.
///
/// Between the two is the band's own dynamic range, and the correction maps that onto the display window.
/// Without it the treble sits at a fraction of the bass under spectrally balanced material.
/// A peak-per-band reading of pink noise falls as the inverse root of frequency across the sixteen.
///
/// ## How much of that correction is applied
///
/// The correction takes a compressor's ratio, removing that share of a band's deviation from the window.
/// One to one is off and the music's balance is untouched, two to one halves the rig's coloration, and a high ratio flattens it.
///
/// It is slow on purpose: a fast per-band gain is what causes cross-spectral pumping, and every source on multiband dynamics says not to.
/// A maximum gain caps the lift, so a silent band is never amplified into its own noise.
/// Turning learning off freezes both tables, which is the deterministic mode a show wants.
///
/// The state is thirty-two floats, and the work is sixteen logarithms and a handful of multiplies per block, on the audio path and never per light.
///
/// ## How an onset is decided
///
/// The decision is the textbook one: flux against its own recent mean rather than an absolute threshold.
/// A loud room and a quiet one therefore fire on the same kind of event.
/// A refractory window then makes one hit one onset, however many blocks it spans.
///
/// The mean is an exponential average in fixed point, advanced after the decision so a hit does not raise the bar it is being judged against.
/// A hit is flux above a configurable fraction of that mean plus a margin.
///
/// ## Why the gate must also stop the learning
///
/// A band below the silence threshold carries no program material, so it reads zero and is not learned from, and both halves of that matter.
///
/// Without the gate the lift is dominated by the distance from the window floor.
/// That relocates a silent band upward as eagerly as a quiet instrument, so an empty room is displayed at full scale.
/// Measured on the bench, the raw path read a flux of nought to three while the learner made thirty-three to sixty-eight of it.
///
/// Learning from silence also drags the floor table down to the noise, so the next wobble reads as music.
///
/// ## Why a band's silence gate carries no margin
///
/// The gate sits at the window floor, with no margin, unlike the level path's.
///
/// ## Why the margin would have to be tuned
/// The two measure different quantities.
/// A band reports its filter's power on the scale a transform's peak bin read, while the level reports the block's RMS, and that scale sits far above an RMS for the same sound.
/// The same margin in decibels is therefore a much larger concession on a band.
///
/// Measured on the bench in a quiet room, the spectrum reads a flux of one or two at the window floor.
/// A twenty-decibel margin takes it to fifty or more, with onsets firing.
/// That is the room's own noise displayed as music.
/// A margin large enough to matter would have to be tuned per part, which is exactly what the learner exists to avoid.
///
/// The cost is real and accepted.
/// A band below the window is primed once and returns early, so its learned floor does not track a room that grows quieter still.
/// That band is dark either way, and re-learning starts the moment anything audible arrives.

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

/// Turns a flux stream into onsets: one per hit, and none for a swell.
struct OnsetDetector {
    uint16_t mean_ = 0;            ///< EMA of the flux, 8.8 fixed point
    uint32_t lastMs_ = 0;          ///< when the last onset fired
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

/// Per-band conditioning in the decibel domain: the tier that learns the rig rather than the music.
struct BandConditioner {
    /// The narrowest dynamic range a band is credited with, shared with the level path.
    static constexpr float kMinRangeDb = kConditionerMinRangeDb;

    /// The most bands it conditions: the 31 third octaves.
    static constexpr uint8_t kMaxBands = 31;

    float floorDb[kMaxBands];
    float peakDb[kMaxBands];
    bool  primed = false;   ///< whether a first block has seeded the two tables

    /// Starts with both tables zeroed, the first block seeding them.
    BandConditioner() { for (uint8_t b = 0; b < kMaxBands; b++) { floorDb[b] = 0.0f; peakDb[b] = 0.0f; } }

    /// Condition one block of `n` decibel values into the display window.
    void process(const float* db, float* out, uint32_t dtMs, float windowFloor,
                 float windowSpan, uint8_t ratioN, float maxGainDb, bool learning, float gateDb,
                 float floorRiseDbPerS = 1.0f, float peakReleaseDbPerS = 3.0f, uint8_t n = 16) {
        if (n > kMaxBands) n = kMaxBands;
        if (!primed) {
            for (uint8_t b = 0; b < n; b++) {
                floorDb[b] = db[b];
                peakDb[b] = db[b] + kMinRangeDb;
            }
            primed = true;
        }
        const float dt = static_cast<float>(dtMs) / 1000.0f;
        const float amount = ratioN <= 1 ? 0.0f : 1.0f - 1.0f / static_cast<float>(ratioN);
        for (uint8_t b = 0; b < n; b++) {
            // Held before the followers, so the tables describe the music and not the room.
            if (db[b] < gateDb) { out[b] = 0.0f; continue; }
            if (learning) {
                // A minimum follower: it takes a new low at once and forgets one slowly.
                floorDb[b] = db[b] < floorDb[b] ? db[b] : floorDb[b] + floorRiseDbPerS * dt;
                // Peak: instant attack, slow release, so it settles on the band's typical top.
                peakDb[b] = db[b] > peakDb[b] ? db[b] : peakDb[b] - peakReleaseDbPerS * dt;
                // Bounded so a collapsed follower cannot divide by nearly zero.
                if (peakDb[b] < floorDb[b] + kMinRangeDb) peakDb[b] = floorDb[b] + kMinRangeDb;
            }
            // Where the band's own range would place the value, and how much of that move is taken.
            const float range = peakDb[b] - floorDb[b];
            const float normalized = windowFloor + (db[b] - floorDb[b]) * (windowSpan / range);
            float shift = (normalized - db[b]) * amount;
            if (shift > maxGainDb) shift = maxGainDb;          // never lift a band into its noise
            out[b] = db[b] + shift;
        }
    }
};

/// The third-octave bank the bands are measured with.
using ThirdOctaveBank = FractionalOctaveBank<3>;

/// The sixteen display bands as third-octave bands, WLED-MM's default GEQ pattern: @xref{sixteen-of-the-thirty-one}.
inline constexpr uint8_t kSixteenOfThirds[16] = {3, 5, 6, 8, 9, 11, 13, 14, 16, 17, 19, 20, 22, 24, 25, 27};

/// A band's power on the display window's decibel scale: a tone of amplitude A reads 20 log10(128 A); nothing audible reads 0.
inline float bandPowerDb(float power) {
    const float scaled = power * 32768.0f;   // 2 x (512 / 4)^2: a Hann-windowed 512-point peak bin's square, over a tone's power
    return scaled <= 1.0f ? 0.0f : 10.0f * std::log10(scaled);
}

/// The 31 band bytes and the sixteen picked from them, and the dominant peak, from the bank's powers, conditioned per band when a conditioner is given; answers how many of the 31 the bank measures, the ones above reading 0.
inline uint8_t bankToBands(const ThirdOctaveBank& bank, uint16_t noiseFloor, uint16_t gain,
                           uint8_t thirds[31], uint8_t bands[16], uint16_t& peakHz, uint16_t& peakMag,
                           BandConditioner* cond = nullptr, uint32_t dtMs = 10,
                           uint8_t ratioN = 1, float maxGainDb = 24.0f, bool learning = true) {
    for (uint8_t b = 0; b < 31; b++) thirds[b] = 0;
    for (uint8_t b = 0; b < 16; b++) bands[b] = 0;
    peakHz = 0;
    peakMag = 0;
    const uint8_t count = bank.bandCount();
    if (count == 0) return 0;

    float db[31];
    for (uint8_t b = 0; b < count; b++) db[b] = bandPowerDb(bank.power(b));
    // Conditioned in decibels, then through the shared window, so the two paths cannot disagree.
    float out[31];
    if (cond) {
        // The gate sits at the window floor, with no margin: see the appendix on why.
        cond->process(db, out, dtMs, windowFloorDb(noiseFloor), windowSpanDb(gain), ratioN,
                      maxGainDb, learning, windowFloorDb(noiseFloor), 1.0f, 3.0f, count);
    } else {
        for (uint8_t b = 0; b < count; b++) out[b] = db[b];
    }
    for (uint8_t b = 0; b < count; b++) thirds[b] = db[b] <= 0.0f ? 0 : dbToByte(out[b], noiseFloor, gain);
    for (uint8_t b = 0; b < 16; b++) bands[b] = thirds[kSixteenOfThirds[b]];

    uint8_t loudest = 0;
    for (uint8_t b = 1; b < count; b++) if (db[b] > db[loudest]) loudest = b;
    if (db[loudest] <= 0.0f) return count;
    float offset = 0.0f;   // in bands, from the loudest band's center: @xref{the-dominant-frequency}
    if (loudest > 0 && loudest + 1 < count) {
        const float curve = db[loudest - 1] - 2.0f * db[loudest] + db[loudest + 1];
        if (curve < 0.0f) offset = 0.5f * (db[loudest - 1] - db[loudest + 1]) / curve;
        if (offset > 0.5f) offset = 0.5f;
        if (offset < -0.5f) offset = -0.5f;
    }
    peakHz = static_cast<uint16_t>(bank.centerHz(loudest) * std::exp2(offset / 3.0f));
    peakMag = dbToByte(db[loudest], noiseFloor, gain);
    return count;
}

} // namespace mm
