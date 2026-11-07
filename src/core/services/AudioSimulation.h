#pragma once

#include <cstdint>

#include "core/util/AudioFrame.h"
#include "core/util/math8.h"              // sin8 / triwave8, the oscillators
#include "core/services/AudioBands.h"     // kSixteenOfThirds

namespace mm {

/// The simulated song's beat period, in milliseconds.
constexpr uint32_t kSimBeatMs = 600;
/// The simulated frame's period: a block at the default 24 kHz, as an input makes them, so the smoothing runs at its own rate whatever the render's.
constexpr uint32_t kSimFrameMs = kAudioBlock * 1000u / 24000u;
/// Whether a simulated frame is due at `now`, the last made at `prevMs`, 0 before the first.
constexpr bool simFrameDue(uint32_t now, uint32_t prevMs) { return prevMs == 0 || now - prevMs >= kSimFrameMs; }

/// AudioService's `simulate` signal at time `t`, so a device shows sound before anything is wired: the bands, thirds, level and peak, and the kick as an onset when a beat starts after `prevT`.
inline void simulateAudio(AudioFrame& f, uint32_t t, bool sweep, uint32_t prevT) {
    f.onset = f.onsetLow = 0;
    if (sweep) {
        // One band lit at a time, stepping bass to treble every 250 ms and wrapping.
        const uint8_t pos = static_cast<uint8_t>((t / 250u) % 16u);
        const uint8_t env = triwave8(static_cast<uint8_t>((t % 250u) * 255u / 250u));   // 0..255 within a step
        for (uint8_t b = 0; b < 16; b++) f.bands[b] = (b == pos) ? env : 0;
        f.thirdsCount = 0;   // a sweep of the sixteen, so a 31-band display falls back to them
        f.level = env;
        f.peakHz = static_cast<uint16_t>(80 + pos * 700);   // bass to about 10.6 kHz across the 16 steps
        f.peakMag = env;
        return;
    }
    // A plausible song: a beat every kSimBeatMs lifting the bass, each band its own oscillator; the song knows its kick, so it states it.
    if (prevT != 0 && t / kSimBeatMs != prevT / kSimBeatMs) f.onset = f.onsetLow = 200;   // a first frame, prevT 0, starts the count
    const uint8_t beat = (t % kSimBeatMs < 90u) ? static_cast<uint8_t>(triwave8(static_cast<uint8_t>((t % kSimBeatMs) * 255u / 90u))) : 0;
    uint16_t sum = 0;
    for (uint8_t b = 0; b < 16; b++) {
        const uint8_t osc = sin8(static_cast<uint8_t>(t * (1u + b) / 8u + b * 24u));   // the treble flickers faster
        uint16_t v = static_cast<uint16_t>((osc * 3u) / 4u);
        if (b < 4) v = static_cast<uint16_t>(v + beat / 2u);
        f.bands[b] = static_cast<uint8_t>(v > 255 ? 255 : v);
        sum = static_cast<uint16_t>(sum + f.bands[b]);
    }
    const uint8_t swell = sin8(static_cast<uint8_t>(t / 24u));   // a slow breath in the volume
    const uint16_t lvl = static_cast<uint16_t>(swell / 2u + sum / 32u + beat / 2u);
    f.level = lvl > 255 ? 255 : lvl;
    // The 31 hold the sixteen where they sit, and each third between two of them reads their mean.
    bool picked[31] = {};
    for (uint8_t b = 0; b < 16; b++) { f.thirds[kSixteenOfThirds[b]] = f.bands[b]; picked[kSixteenOfThirds[b]] = true; }
    for (uint8_t c = 0; c < 31; c++)
        if (!picked[c]) f.thirds[c] = static_cast<uint8_t>(((c > 0 ? f.thirds[c - 1] : 0) + (c + 1 < 31 ? f.thirds[c + 1] : 0)) / 2);
    f.thirdsCount = 31;
    f.peakHz = static_cast<uint16_t>(80 + sin8(static_cast<uint8_t>(t / 40u)) * 40u);   // the peak drifts, so frequency-mapped effects move
    f.peakMag = static_cast<uint16_t>(f.level);
}

}  // namespace mm
