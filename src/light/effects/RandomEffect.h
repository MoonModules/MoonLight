#pragma once

#include "light/effects/EffectBase.h"
#include "light/powerfunctions/particles.h"   // particles::FrameTime, the shared elapsed-to-scale conversion

namespace mm {

/// Effect that fills the layer with animated random colors.
/// @card RandomEffect.gif
///
/// The buffer dims a little, then one light takes a random palette color.
/// Over many frames that scatters fading sparkles across the whole volume.
/// `fade` sets the density: less lets them linger and fill, more leaves quick specks.
///
/// Prior art: MoonLight's Random effect.
class RandomEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin.
    const char* tags() const override { return "💫✨"; }
    /// The lit light is picked across the whole volume, so it addresses every axis.
    Dim dimensions() const override { return Dim::D3; }

    /// How fast a lit light fades, which sets the field's density.
    uint8_t fade = 70;

    /// Publish the fade.
    void defineControls() override {
        controls_.addControl("fade", fade, 0, 255);
    }

    /// Dim the field, then light as many random pixels as the elapsed time has earned.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const nrOfLightsType n = nrOfLights();
        const uint8_t cpl = cv.cpl;

        // Dim the whole buffer.
        layer()->fadeToBlackBy(fade);

        // One per reference frame rather than per render, or the sparkle rate follows the hardware.
        spawnCarry_ += time_.advance(elapsed());
        uint32_t due = spawnCarry_ / particles::FrameTime::kOne;
        if (due > 64) due = 64;                     // a stall tops up rather than filling the grid
        spawnCarry_ -= due * particles::FrameTime::kOne;

        uint8_t* d = cv.data;
        for (uint32_t k = 0; k < due; k++) {
            // A flat light index, so this writes bytes where draw::pixel would want a coordinate.
            const nrOfLightsType idx = static_cast<nrOfLightsType>(rng_.next16() % n);
            const RGB c = colorFromPalette(*Palettes::active(), rng_.next8());
            const size_t off = static_cast<size_t>(idx) * cpl;
            if (off + (cpl < 3 ? cpl : 3) > cv.bytes) continue;
            d[off + 0] = c.r;
            if (cpl >= 2) d[off + 1] = c.g;
            if (cpl >= 3) d[off + 2] = c.b;
        }
    }

private:
    particles::FrameTime time_{60};   ///< the reference rate the spawn is counted against
    uint32_t spawnCarry_ = 0;         ///< lights earned but not yet placed
    Random8 rng_;                     ///< this effect's own deterministic sequence
};

} // namespace mm