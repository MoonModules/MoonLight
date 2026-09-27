#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// Author: MoonLight original (concentric rings)
/// Effect of expanding concentric rings from random centers.
/// @card RingsEffect.gif
///
/// Each ring grows continuously and respawns somewhere fresh once it leaves the grid.
/// Several overlap, and where two rings cross their brightness sums.
/// RipplesEffect holds the sine-wave water surface, where this one is the concentric form.
class RingsEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, David Jupijn and Rising Step.
    const char* tags() const override { return "💫🦅🖌️🎡"; }
    /// Iterates y and x, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How many rings the effect can hold at once.
    static constexpr uint8_t MAX_RIPPLES = 8;

    // Calm defaults: a couple of slow rings read as clean circles, where more reads as chaos.
    /// How many rings expand at a time.
    uint8_t count = 2;
    /// How fast each one grows.
    uint8_t speed = 30;
    /// How wide a ring's lit band is.
    uint8_t thickness = 3;
    /// Walks every ring around the palette.
    uint8_t hue_shift = 0;

    /// Publish the population, the growth rate, the band's width and the palette shift.
    void defineControls() override {
        controls_.addControl("count", count, 1, 255);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("thickness", thickness, 1, 255);
        controls_.addControl("hue_shift", hue_shift, 0, 255);
    }

    /// Grow every ring, respawning those that left, then light each pixel near one.
    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        // The true distance to the far corner, kept wide: clamping it stalled every ring short of the edge.
        const uint32_t maxR32 = dist16(static_cast<int32_t>(w), static_cast<int32_t>(h));
        const uint16_t maxR = static_cast<uint16_t>(maxR32 < 1 ? 1 : (maxR32 > 65535 ? 65535 : maxR32));

        if (!initialized_) {
            for (uint8_t i = 0; i < MAX_RIPPLES; i++) {
                spawn(i, w, h);
                // Staggered, so the rings start spread across every size.
                radius_[i] = static_cast<uint16_t>((i * maxR) / MAX_RIPPLES);
            }
            initialized_ = true;
        }

        uint32_t now = elapsed();
        uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        // Growth per frame, scaled by the speed control and the elapsed time.
        uint16_t growth = static_cast<uint16_t>((static_cast<uint32_t>(speed) * dt) >> 7);
        if (growth == 0) growth = 1;

        for (uint8_t i = 0; i < count && i < MAX_RIPPLES; i++) {
            uint32_t next = static_cast<uint32_t>(radius_[i]) + growth;
            if (next > maxR) {
                spawn(i, w, h);
            } else {
                radius_[i] = static_cast<uint16_t>(next);
            }
        }

        for (lengthType y = 0; y < h; y++) {
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                uint16_t r_acc = 0, g_acc = 0, b_acc = 0;
                for (uint8_t i = 0; i < count && i < MAX_RIPPLES; i++) {
                    const int32_t dx = static_cast<int32_t>(x) - cx_[i];
                    const int32_t dy = static_cast<int32_t>(y) - cy_[i];
                    // Wide, and computed rather than tabled since this is a moving ring's center.
                    const uint32_t d = dist16(dx, dy);
                    int32_t diff = static_cast<int32_t>(d) - static_cast<int32_t>(radius_[i]);
                    if (diff < 0) diff = -diff;   // stays wide, since narrowing truncated a large distance
                    if (diff < thickness) {
                        // Brightest on the ring itself, falling off across its band.
                        uint8_t falloff = static_cast<uint8_t>(((thickness - diff) * 255) / thickness);
                        // An older ring, having grown larger, fades out.
                        uint8_t age_fade = static_cast<uint8_t>(255 - ((radius_[i] * 255u) / maxR));
                        uint8_t intensity = scale8(falloff, age_fade);
                        RGB c = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(hue_[i] + hue_shift), intensity);
                        r_acc = static_cast<uint16_t>(r_acc + c.r);
                        g_acc = static_cast<uint16_t>(g_acc + c.g);
                        b_acc = static_cast<uint16_t>(b_acc + c.b);
                    }
                }
                if (cpl >= 1) row[0] = r_acc > 255 ? 255 : static_cast<uint8_t>(r_acc);
                if (cpl >= 2) row[1] = g_acc > 255 ? 255 : static_cast<uint8_t>(g_acc);
                if (cpl >= 3) row[2] = b_acc > 255 ? 255 : static_cast<uint8_t>(b_acc);
                row += cpl;
            }
        }
    }

private:
    lengthType cx_[MAX_RIPPLES] = {};     ///< each ring's center on x
    lengthType cy_[MAX_RIPPLES] = {};     ///< and on y
    uint16_t radius_[MAX_RIPPLES] = {};   ///< wide, since a large panel's far corner exceeds a byte
    uint8_t hue_[MAX_RIPPLES] = {};       ///< each ring's palette entry
    bool initialized_ = false;            ///< false until the first tick seeds the rings
    uint32_t lastElapsed_ = 0;            ///< the previous frame's timestamp
    Random8 rng_{0xC0DECAFEu};            ///< the spawn's placement and color
    /// One random byte, the shape the spawn below wants.
    uint8_t rand8() { return rng_.next8(); }

    /// Put ring `i` somewhere fresh, at zero radius and a new color.
    void spawn(uint8_t i, lengthType w, lengthType h) {
        cx_[i] = static_cast<lengthType>((static_cast<uint16_t>(rand8()) * w) >> 8);
        cy_[i] = static_cast<lengthType>((static_cast<uint16_t>(rand8()) * h) >> 8);
        radius_[i] = 0;
        hue_[i] = rand8();
    }
};

} // namespace mm
