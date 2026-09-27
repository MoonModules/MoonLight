#pragma once

#include "core/util/math16.h"            // map32: the shared, fencepost-safe range map
#include "light/effects/EffectBase.h"

namespace mm {

/// Audio-reactive effect: sawtooth bands driven by the frequency spectrum.
/// @card FreqSawsEffect.gif
/// Author: @TroyHacks (MoonLight / WLED MoonModules), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
///
/// One vertical saw per band, each running at a rate its own loudness drives.
/// A band's speed rises instantly to a loud hit and decays slowly once the sound stops.
/// So a struck band keeps sawing for a while, and a quiet one winds down.
///
/// Prior art: MoonLight's FreqSaws, whose physics and three position methods this reproduces.
///
/// @moreinfo
///
/// ## Three ways to place the saw
///
/// Chaos reads the position straight off the beat, so the saw teleports as the rate changes.
/// Chaos fix carries a per-band offset, so a rate change continues from where the saw already was.
/// BandPhases integrates a per-band accumulator each frame, so it advances with no jump at all.
///
/// ## The physics run once a tick
///
/// The band loop caches each band's position, and the column loop only reads that cache.
/// So a band spanning many columns integrates once a frame rather than once a column.
/// `invert` mirrors every other column for a woven look, and `keepOn` keeps a decayed band drawn.
class FreqSawsEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, audio-reactive.
    const char* tags() const override { return "💫🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    // Defaults match MoonLight's own FreqSaws.
    /// The per-frame fade, which is the motion trail.
    uint8_t fade      = 4;
    /// How much a band's loudness gains into its target speed.
    uint8_t increaser = 211;
    /// How fast a silent band decays, where 0 leaves it running.
    uint8_t decreaser = 18;
    /// The top rate a fully sped band reaches.
    uint8_t bpmMax    = 198;
    /// Mirror every other column, for a woven look.
    bool    invert    = false;
    /// Keep drawing a band whose speed has decayed to nothing.
    bool    keepOn    = false;
    /// Which of the three position methods places the saw.
    uint8_t method    = 2;

    /// Publish the trail, the band physics and the position method.
    void defineControls() override {
        controls_.addControl("fade", fade, 0, 255);
        controls_.addControl("increaser", increaser, 0, 255);
        controls_.addControl("decreaser", decreaser, 0, 255);
        controls_.addControl("bpmMax", bpmMax, 0, 255);
        controls_.addControl("invert", invert);
        controls_.addControl("keepOn", keepOn);
        static constexpr const char* kMethodOptions[] = {"Chaos", "Chaos fix", "BandPhases"};
        controls_.addSelect("method", method, kMethodOptions, 3);
    }

    /// Start every band from rest, since the state is per band rather than per light.
    void prepare() override {
        clearState();
    }

    /// Advance all sixteen bands once, then draw each column from its band's cached position.
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
        bool    bandActive[NUM_GEQ_CHANNELS] = {};
        uint8_t bandY[NUM_GEQ_CHANNELS]      = {};
        for (int band = 0; band < NUM_GEQ_CHANNELS; band++) {
            const uint8_t volume = f->bands[band];
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

                if (method == 0) {
                    // Straight off the beat, so it jumps as the rate changes.
                    bandY[band] = static_cast<uint8_t>(map32(beat8(bpm, now), 0, 255, 0, sizeY - 1));
                } else if (method == 1) {
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
            // This column mapped onto one of the sixteen bands.
            int band = map32(x, 0, sizeX, 0, NUM_GEQ_CHANNELS);
            if (band < 0) band = 0;
            if (band > NUM_GEQ_CHANNELS - 1) band = NUM_GEQ_CHANNELS - 1;

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
    /// The spectrum's width, which the bands and the columns both map onto.
    static constexpr int NUM_GEQ_CHANNELS = 16;

    /// Return every band to rest.
    void clearState() {
        std::memset(bandSpeed, 0, sizeof(bandSpeed));
        std::memset(bandPhase, 0, sizeof(bandPhase));
        std::memset(lastBpm, 0, sizeof(lastBpm));
        std::memset(phaseOffset, 0, sizeof(phaseOffset));
        lastTime = 0;
    }

    // Per band rather than per light, so these stay inline.
    uint16_t bandSpeed[NUM_GEQ_CHANNELS]   = {};   ///< each band's current run-rate
    uint16_t bandPhase[NUM_GEQ_CHANNELS]   = {};   ///< the BandPhases accumulator
    uint8_t  lastBpm[NUM_GEQ_CHANNELS]     = {};   ///< the Chaos fix method's previous rate
    uint8_t  phaseOffset[NUM_GEQ_CHANNELS] = {};   ///< and its carried offset
    unsigned long lastTime                 = 0;    ///< the previous frame's timestamp
};

} // namespace mm
