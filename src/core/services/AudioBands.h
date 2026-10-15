#pragma once

#include "core/util/AudioFrame.h"
#include "core/services/AudioLevel.h"   // magToByte: the shared log/dB mapping
#include "core/util/math16.h"       // ballistic: the per-band meter shape

#include <cmath>     // band maths is inherently float, as is the transform seam it feeds
#include <cstddef>
#include <cstdint>

namespace mm {

/// The FFT post-processing: magnitude bins in, sixteen band bytes and a dominant peak out.
///
/// @technical
///
/// This is pure domain maths with no platform header, the FFT kernel itself being the one seam.
/// It is host-tested by feeding a synthesised sine through the window, the desktop's own transform and the band grouping.
/// The test then asserts the dominant band and peak frequency, all without a device.
///
/// ## Why every band must own a bin
///
/// A pure geometric split gives each band an equal frequency ratio, which is the textbook mapping of linear bins onto pitch and is scale-free.
/// The transform is not: below twice the bin width there are no distinct bins to hand out, so the low edges collide.
///
/// Measured at the shipped shape, two bands owned no bins at all and two owned one, while the top band owned seventy-five.
/// A quarter of the display could not respond to anything, and it was the quarter where music has its energy.
///
/// So the geometric curve is kept and then made monotonic by construction: every band gets at least one bin, taken from the wide top bands that have them to spare.
/// The result stays log-shaped where the transform can afford it and degrades to one bin a band where it cannot.
/// That is the honest answer at the bottom of the range.
/// No split can separate two nearby low tones when they share a bin.
///
/// The edges are computed once per rate change and never per frame, and the summing loop costs the same additions wherever they sit.
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
/// A band reports the peak magnitude of its bins while the level reports the block's RMS, and a peak sits far above an RMS for the same sound.
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
///
/// ## Textbook spectrum analysis, nothing exotic
///
/// The window is a Hann, the standard general-purpose choice.
/// Tapering the block's edges to zero stops spectral leakage, where a tone smears across many bins because the block is not a whole number of cycles.
/// It also strips the DC offset while converting the samples to floats.
///
/// The band grouping folds the transform's magnitude bins into sixteen log-spaced bands, because pitch is logarithmic and bass needs few bins where treble needs many.
/// The split is a plain geometric one of equal ratio, the result normalizes onto the byte range, and the single loudest bin becomes the dominant peak.

/// The Hann window's coefficient at sample `i` of `n`.
inline float hannWindow(size_t i, size_t n) {
    if (n <= 1) return 1.0f;
    const float x = 6.28318530718f * static_cast<float>(i) / static_cast<float>(n - 1);
    return 0.5f - 0.5f * std::cos(x);
}

/// Window `n` samples into `out`, stripping the DC offset that would otherwise swamp the peak.
inline void applyWindow(const int32_t* samples, size_t n, float* out) {
    if (!samples || !out || n == 0) return;
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (samples[i] >> 8);
    const float mean = static_cast<float>(sum) / static_cast<float>(n);
    for (size_t i = 0; i < n; i++) {
        const float s = static_cast<float>(samples[i] >> 8) - mean;
        out[i] = s * hannWindow(i, n);
    }
}

/// The 17 bin-index edges of the 16 bands, `edge[b]..edge[b+1]` per band.

/// The lowest frequency the spectrum shows, below which is infrasound rather than music.
inline constexpr float kLowestAudibleHz = 40.0f;

inline void audioBandEdges(size_t nMag, uint32_t sampleRate, size_t edge[17]) {
    if (nMag < 17) {                        // pathologically small: one bin each, as far as it goes
        for (uint8_t e = 0; e <= 16; e++) edge[e] = e < nMag ? e : nMag;
        return;
    }
    // The geometric ideal, in floating point so the collisions are visible before rounding.
    for (uint8_t e = 0; e <= 16; e++) {
        const float frac = static_cast<float>(e) / 16.0f;
        float ix = std::pow(static_cast<float>(nMag), frac);
        edge[e] = static_cast<size_t>(ix);
    }
    // The lowest audible bin, not bin 1: bin 0 is DC and those above it are infrasound.
    size_t firstBin = 1;
    if (sampleRate > 0) {
        const float binHz = static_cast<float>(sampleRate) / (2.0f * static_cast<float>(nMag));
        if (binHz > 0.0f) {
            firstBin = static_cast<size_t>(kLowestAudibleHz / binHz);
            if (firstBin < 1) firstBin = 1;
            if (firstBin > nMag / 2) firstBin = nMag / 2;   // never eat half the spectrum
        }
    }
    edge[0] = firstBin;
    edge[16] = nMag;
    // Push each edge up so every band owns a bin, which the geometric split alone could not do.
    for (uint8_t e = 1; e <= 16; e++)
        if (edge[e] <= edge[e - 1]) edge[e] = edge[e - 1] + 1;
    // Pull them back when the forward pass ran past the end, so the edges strictly increase.
    edge[16] = nMag;
    for (uint8_t e = 16; e >= 1; e--)
        if (edge[e] <= edge[e - 1]) edge[e - 1] = edge[e] - 1;
}

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

    float floorDb[16];
    float peakDb[16];
    bool  primed = false;   ///< whether a first block has seeded the two tables

    /// Starts with both tables zeroed, the first block seeding them.
    BandConditioner() { for (uint8_t b = 0; b < 16; b++) { floorDb[b] = 0.0f; peakDb[b] = 0.0f; } }

    /// Condition one block of decibel values into the display window.
    void process(const float db[16], float out[16], uint32_t dtMs, float windowFloor,
                 float windowSpan, uint8_t ratioN, float maxGainDb, bool learning, float gateDb,
                 float floorRiseDbPerS = 1.0f, float peakReleaseDbPerS = 3.0f) {
        if (!primed) {
            for (uint8_t b = 0; b < 16; b++) {
                floorDb[b] = db[b];
                peakDb[b] = db[b] + kMinRangeDb;
            }
            primed = true;
        }
        const float dt = static_cast<float>(dtMs) / 1000.0f;
        const float amount = ratioN <= 1 ? 0.0f : 1.0f - 1.0f / static_cast<float>(ratioN);
        for (uint8_t b = 0; b < 16; b++) {
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

inline void magnitudesToBands(const float* mag, size_t nMag, uint32_t sampleRate,
                              uint16_t noiseFloor, uint16_t gain,
                              uint8_t bands[16], uint16_t& peakHz, uint16_t& peakMag,
                              BandConditioner* cond = nullptr, uint32_t dtMs = 23,
                              uint8_t ratioN = 1, float maxGainDb = 24.0f, bool learning = true) {
    for (uint8_t b = 0; b < 16; b++) bands[b] = 0;
    peakHz = 0;
    peakMag = 0;
    if (!mag || nMag == 0 || sampleRate == 0) return;

    // Hz per bin = sampleRate / (2 * nMag).
    const float binHz = static_cast<float>(sampleRate) / (2.0f * static_cast<float>(nMag));

    size_t edge[17];
    audioBandEdges(nMag, sampleRate, edge);
    float bandDb[16];

    // The same logarithmic mapping the level path uses, so the two knobs mean one thing across both.
    auto toByte = [noiseFloor, gain](float m) -> uint8_t {
        return magToByte(m, noiseFloor, gain);
    };

    float peakVal = 0.0f;
    size_t peakBin = 0;
    for (size_t i = 1; i < nMag; i++)              // single peak scan (skip DC)
        if (mag[i] > peakVal) { peakVal = mag[i]; peakBin = i; }

    for (uint8_t b = 0; b < 16; b++) {
        size_t lo = edge[b], hi = edge[b + 1];
        if (hi <= lo) hi = lo + 1;
        if (hi > nMag) hi = nMag;
        // The peak rather than the average, so a narrow tone lights one band instead of smearing.
        float best = 0.0f;
        for (size_t i = lo; i < hi; i++) if (mag[i] > best) best = mag[i];
        bandDb[b] = best <= 1.0f ? 0.0f : 20.0f * std::log10(best);
    }
    // Conditioned in decibels, then through the shared window, so the two paths cannot disagree.
    if (cond) {
        float out[16];
        // The gate sits at the window floor, with no margin: see the appendix on why.
        cond->process(bandDb, out, dtMs, windowFloorDb(noiseFloor), windowSpanDb(gain), ratioN,
                      maxGainDb, learning, windowFloorDb(noiseFloor));
        for (uint8_t b = 0; b < 16; b++) bands[b] = bandDb[b] <= 0.0f ? 0 : dbToByte(out[b], noiseFloor, gain);
    } else {
        for (uint8_t b = 0; b < 16; b++) bands[b] = bandDb[b] <= 0.0f ? 0 : dbToByte(bandDb[b], noiseFloor, gain);
    }

    if (peakVal > 0.0f) {
        peakHz = static_cast<uint16_t>(static_cast<float>(peakBin) * binHz);
        peakMag = toByte(peakVal);
    }
}

} // namespace mm
