#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Night-sky effect: twinkling stars over a dark field.
/// @card StarSkyEffect.gif
/// Author: limpkin (MoonLight), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
///
/// A pool of stars, sized from the light count, each taking a random cell and brightness.
/// Every frame a star steps toward full and reverses, or toward zero and respawns elsewhere.
/// A small per-frame chance flips its direction early, so the field never pulses in sync.
///
/// Prior art: MoonLight's StarSky, whose pool model and early reverse this reproduces.
///
/// @moreinfo
///
/// ## The color leads the step
///
/// Each frame reads a star's brightness, makes its color, then steps that brightness.
/// So the color drawn is the one from before this frame's step, as the source does it.
/// Stars are white unless `usePalette`, where each carries a palette index of its own.
class StarSkyEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// A star takes any cell of the volume.
    Dim dimensions() const override { return Dim::D3; }

    // Defaults match MoonLight's own StarSky.
    /// How far a star's brightness steps each frame.
    uint8_t speed           = 1;
    /// Stars per ten thousand lights, which sizes the pool.
    uint8_t star_fill_ratio = 42;
    /// Give each star its own palette color rather than white.
    bool    usePalette      = false;

    /// Publish the twinkle rate, the star density and the coloring.
    void defineControls() override {
        controls_.addControl("speed", speed, 0, 42);
        controls_.addControl("star_fill_ratio", star_fill_ratio, 0, 255);
        controls_.addControl("usePalette", usePalette);
    }

    /// Size the per-star arrays on the heap, since an inline one overflows the probe's stack.
    void prepare() override {
        const nrOfLightsType count = nrOfLights();
        const size_t wanted = count > 0
            ? (static_cast<size_t>(star_fill_ratio) * count) / 10000u + 1u
            : 0u;
        if (wanted != nbStars_ || count != lightCount_) {
            // Resize all four, then test: every buffer is sized even if an earlier one fails.
            const bool a = indexes_.resize(wanted);
            const bool b = fadeDir_.resize(wanted);
            const bool c = brightness_.resize(wanted);
            const bool d = colors_.resize(wanted);
            if (wanted > 0 && a && b && c && d) {
                nbStars_    = wanted;
                lightCount_ = count;
                initStars(count);
            } else {
                indexes_.resize(0); fadeDir_.resize(0); brightness_.resize(0); colors_.resize(0);
                nbStars_ = 0; lightCount_ = 0;
            }
        }
    }

    /// Dim the sky, then step and draw every star.
    void tick() MM_NONBLOCKING override {
        if (!indexes_ || !fadeDir_ || !brightness_ || !colors_ || nbStars_ == 0) return;
        const lengthType w = width(), h = height();
        const nrOfLightsType count = nrOfLights();

        const draw::Canvas cv = canvas();

        layer()->fadeToBlackBy(50);

        for (size_t i = 0; i < nbStars_; i++) {
            const nrOfLightsType index = indexes_[i];
            // The linear index back to a cell, which is in bounds by construction.
            const lengthType x = static_cast<lengthType>(index % w);
            const lengthType y = static_cast<lengthType>((index / w) % h);
            const lengthType z = static_cast<lengthType>(index / (static_cast<size_t>(w) * h));
            const Coord3D p{x, y, z};

            // The color comes from the brightness before this frame's step, as the source does it.
            const uint8_t b = brightness_[i];
            const RGB color = usePalette
                ? colorFromPalette(*Palettes::active(), colors_[i], b)
                : RGB{b, b, b};

            if (fadeDir_[i]) {
                // Rising toward full.
                const uint16_t nb = static_cast<uint16_t>(b) + speed;
                brightness_[i] = nb > 255 ? 255 : static_cast<uint8_t>(nb);
                draw::pixel(cv, p, color);
                if (brightness_[i] == 255) fadeDir_[i] = 0;
                if (rng_.next8() < 10) fadeDir_[i] = 0;
            } else {
                // Falling toward black, respawning elsewhere once it lands.
                brightness_[i] = b > speed ? static_cast<uint8_t>(b - speed) : 0;
                draw::pixel(cv, p, color);
                if (brightness_[i] == 0) {
                    indexes_[i] = randomIndex(count);
                    fadeDir_[i] = 1;
                }
                if (rng_.next8() < 10) fadeDir_[i] = 1;
            }
        }
    }

private:
    ScratchBuffer<nrOfLightsType> indexes_{*this};      ///< each star's cell
    ScratchBuffer<uint8_t>        fadeDir_{*this};      ///< whether it is rising or falling
    ScratchBuffer<uint8_t>        brightness_{*this};   ///< its current brightness
    ScratchBuffer<uint8_t>        colors_{*this};       ///< its palette entry, under `usePalette`
    size_t         nbStars_    = 0;                     ///< how many stars the pool holds
    nrOfLightsType lightCount_ = 0;                     ///< the light count it was sized for
    Random8   rng_{0x57A55C1Eu};                        ///< the twinkle's randomness

    /// A uniform cell pick, composed from two draws since the index type outgrows one on a large grid.
    nrOfLightsType randomIndex(nrOfLightsType count) {
        if (count == 0) return 0;
        if constexpr (sizeof(nrOfLightsType) > sizeof(uint16_t)) {
            const uint32_t draw = (static_cast<uint32_t>(rng_.next16()) << 16) | rng_.next16();
            return static_cast<nrOfLightsType>(draw % count);
        } else {
            return static_cast<nrOfLightsType>(rng_.next16() % count);
        }
    }

    /// Seed every star at a random cell, direction, brightness and color.
    void initStars(nrOfLightsType count) {
        for (size_t i = 0; i < nbStars_; i++) {
            indexes_[i]    = randomIndex(count);
            fadeDir_[i]    = rng_.below(2);          // 0 or 1   (random8(2))
            brightness_[i] = rng_.below(1, 254);     // 1..253   (random8(1,254))
            colors_[i]     = rng_.next8();
        }
    }
};

} // namespace mm
