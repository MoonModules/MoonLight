#pragma once

#include "core/util/math16.h"            // map32: the shared, fencepost-safe range map
#include "light/effects/EffectBase.h"
#include "light/powerfunctions/audioReadout.h"   // barsOf: the bands, the thirds or the keys

namespace mm {

/// Audio-reactive sawtooth waves: each column maps to a frequency band whose loudness drives a per-band oscillator speed.
/// @card FreqSawsEffect.gif
///
/// Louder bands sweep their sawtooth up the column faster. A band's speed rises instantly to a loud hit and decays slowly once the sound stops. A struck band keeps sawing for a while, and a quiet one winds down.
///
/// @technical
///
/// The band loop caches each band's position and the column loop only reads that cache, so a band spanning many columns integrates once a frame.
class FreqSawsEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, audio-reactive.
    const char* tags() const override { return "💫🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// Background decay per frame, which is the motion trail.
    uint8_t fade      = 4;
    /// How fast a band's speed ramps up with its magnitude.
    uint8_t increaser = 211;
    /// How fast a silent band's speed decays.
    uint8_t decreaser = 18;
    /// Ceiling on a band's oscillation speed.
    uint8_t bpmMax    = 198;
    /// Flip alternate columns vertically, for a woven look.
    bool    invert    = false;
    /// Keep oscillating even when a band is silent.
    bool    keepOn    = false;
    /// The position methods, in option order.
    enum Method : uint8_t { kMethodChaos, kMethodChaosFix, kMethodBandPhases, kMethodCount };
    /// The phase model that places the saw.
    /// @guide `Chaos` reads the position off the beat, so the saw teleports as the rate changes. `Chaos fix` carries a per-band offset, so a rate change continues smoothly. `BandPhases` integrates an accumulator, so the saw never jumps.
    uint8_t method    = kMethodBandPhases;
    /// The bars: the sixteen, the third octaves or the piano's keys.
    uint8_t bands     = kBandsThirds;

    /// Publish the trail, the band physics, the position method and the bands.
    void defineControls() override {
        controls_.addControl("fade", fade, 0, 255);
        controls_.addControl("increaser", increaser, 0, 255);
        controls_.addControl("decreaser", decreaser, 0, 255);
        controls_.addControl("bpmMax", bpmMax, 0, 255);
        controls_.addControl("invert", invert);
        controls_.addControl("keepOn", keepOn);
        static constexpr const char* kMethodOptions[] = {"Chaos", "Chaos fix", "BandPhases"};
        controls_.addSelect("method", method, kMethodOptions, kMethodCount);
        controls_.addSelect("bands", bands, kBarScaleOptions, kBandsCount);
    }

    /// Start every band from rest, since the state is per band rather than per light.
    void prepare() override {
        clearState();
    }

    /// Advance every band once, then draw each column from its band's cached position.
    void tick() MM_NONBLOCKING override {
        const int sizeX = width();
        const int sizeY = height();

        const AudioFrame* f = AudioService::latestFrame();
        if (!f) return;   // latestFrame returns silence rather than null, but guard regardless

        const draw::Canvas cv = canvas();

        layer()->fadeToBlackBy(fade);

        // The elapsed delta, which drives the decay and the phase integrator.
        const unsigned long now = elapsed();
        const unsigned long deltaMs = now - lastTime;
        lastTime = now;

        // Once a tick, cached, so a band spanning several columns still integrates once a frame.
        const AudioBars bars = barsOf(*f, bands);
        bool    bandActive[kMaxBands] = {};
        uint8_t bandY[kMaxBands]      = {};
        for (int band = 0; band < bars.n; band++) {
            const uint8_t volume = bars.mag[band];
            // Scaled into the 16-bit speed space.
            const uint32_t targetSpeed = static_cast<uint32_t>(volume) * increaser * 257u;

            if (volume > 0) {
                // Rise instantly to a loud hit.
                if (targetSpeed > bandSpeed[band])
                    bandSpeed[band] = static_cast<uint16_t>(targetSpeed > 65535u ? 65535u : targetSpeed);
            } else if (decreaser > 0 && bandSpeed[band] > 0) {
                // Decay toward zero when silent, in proportion to the elapsed time.
                uint32_t decay = (static_cast<uint32_t>(bandSpeed[band]) * deltaMs) /
                                 (static_cast<uint32_t>(decreaser) * 10u);
                if (decay < 1) decay = 1;
                bandSpeed[band] = decay >= bandSpeed[band] ? 0
                                  : static_cast<uint16_t>(bandSpeed[band] - decay);
            }

            if (bandSpeed[band] > 1 || keepOn) {
                bandActive[band] = true;
                // The current speed as a rate, capped at `bpmMax`.
                const uint8_t bpm = static_cast<uint8_t>(map32(bandSpeed[band], 0, 65535, 0, bpmMax));

                if (method == kMethodChaos) {
                    // Straight off the beat, so it jumps as the rate changes.
                    bandY[band] = static_cast<uint8_t>(map32(beat8(bpm, now), 0, 255, 0, sizeY - 1));
                } else if (method == kMethodChaosFix) {
                    // A carried offset, so a rate change continues from where the saw already was.
                    if (bpm != lastBpm[band]) {
                        const uint8_t currentPos = static_cast<uint8_t>(beat8(lastBpm[band], now) + phaseOffset[band]);
                        const uint8_t newPos = beat8(bpm, now);
                        phaseOffset[band] = static_cast<uint8_t>(currentPos - newPos);
                        lastBpm[band] = bpm;
                    }
                    bandY[band] = static_cast<uint8_t>(map32(static_cast<uint8_t>(beat8(bpm, now) + phaseOffset[band]),
                                                            0, 255, 0, sizeY - 1));
                } else {
                    // An integrated accumulator, so the saw advances continuously with no jump.
                    uint32_t phaseInc = (static_cast<uint32_t>(bpm) * static_cast<uint32_t>(deltaMs) * 65536u) /
                                        (60u * 1000u);
                    phaseInc /= 2u;
                    bandPhase[band] = static_cast<uint16_t>(bandPhase[band] + phaseInc);
                    bandY[band] = static_cast<uint8_t>(map32(bandPhase[band] >> 8, 0, 255, 0, sizeY - 1));
                }
            }
        }

        // Each column reads its band's cache, so only the mirroring and the color are per column.
        for (int x = 0; x < sizeX; x++) {
            // This column mapped onto one of the bands.
            int band = map32(x, 0, sizeX, 0, bars.n);
            if (band < 0) band = 0;
            if (band > bars.n - 1) band = bars.n - 1;

            if (!bandActive[band]) continue;

            const uint8_t y = bandY[band];
            // Every other column mirrors top to bottom.
            const int drawY = (invert && (x % 2 == 0)) ? (sizeY - 1 - y) : y;
            const uint8_t colorIndex = static_cast<uint8_t>(map32(x, 0, sizeX - 1, 0, 255));
            const RGB col = colorFromPalette(*Palettes::active(), colorIndex);
            draw::pixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(drawY), 0}, col);
        }
    }

private:
    /// The most bands a scale has, the piano's keys, which the per-band state is sized for.
    static constexpr int kMaxBands = kAudioNoteKeys;

    /// Return every band to rest.
    void clearState() {
        std::memset(bandSpeed, 0, sizeof(bandSpeed));
        std::memset(bandPhase, 0, sizeof(bandPhase));
        std::memset(lastBpm, 0, sizeof(lastBpm));
        std::memset(phaseOffset, 0, sizeof(phaseOffset));
        lastTime = 0;
    }

    uint16_t bandSpeed[kMaxBands]   = {};   ///< each band's current run-rate, per band rather than per light so inline
    uint16_t bandPhase[kMaxBands]   = {};   ///< the BandPhases accumulator
    uint8_t  lastBpm[kMaxBands]     = {};   ///< the Chaos fix method's previous rate
    uint8_t  phaseOffset[kMaxBands] = {};   ///< and its carried offset
    unsigned long lastTime                 = 0;    ///< the previous frame's timestamp
};

} // namespace mm
