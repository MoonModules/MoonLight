#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// A Fire2012-style heat field: sparks at the base rise and cool through the active palette, coldest at its low end.
/// @card FireEffect.gif
///
/// The spark count scales with the width. The flame color comes from the active palette: the Lava palette (black, red, orange, yellow, white) gives the classic fire. An Ocean or Forest palette turns the flame blue or green.
///
/// @attribution Mark Kriegsman, toggledbits, David Jupijn
class FireEffect : public EffectBase {
public:
    /// Catalog tags: FastLED origin, David Jupijn, a simulation.
    const char* tags() const override { return "⚡️🦅🧬"; }
    /// Iterates y and x, so the heat buffer covers the z=0 plane and extrude fills a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast heat dissipates as it rises; higher is shorter flames.
    uint8_t cooling = 55;
    /// Chance of a new spark at the base each frame; higher is livelier.
    uint8_t sparking = 120;

    /// Publish the cooling rate and the spark rate.
    void defineControls() override {
        controls_.addControl("cooling", cooling, 1, 255);
        controls_.addControl("sparking", sparking, 1, 255);
    }

    /// Size the heat grid to the z=0 plane, which extrude fills through a volume.
    void prepare() override {
        heat_.resize(static_cast<size_t>(width()) * height());
    }

    /// Cool the field, let the heat rise, spark the base, then render it through the palette.
    void tick() MM_NONBLOCKING override {
        if (!heat_) return;

        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        // 1. Cool every cell by a small random amount.
        uint8_t coolMax = static_cast<uint8_t>((static_cast<uint16_t>(cooling) * 10) / (h > 0 ? h : 1) + 2);
        for (nrOfLightsType i = 0; i < heat_.count(); i++) {
            uint8_t c = rand8() % coolMax;
            heat_[i] = (heat_[i] > c) ? static_cast<uint8_t>(heat_[i] - c) : 0;
        }

        // 2. Heat rises, each row averaging from the row below it.
        for (lengthType y = 0; y + 1 < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                lengthType yb = y + 1;
                lengthType xl = x > 0 ? x - 1 : 0;
                lengthType xr = (x + 1 < w) ? x + 1 : x;
                uint16_t sum = heat_[yb * w + xl];
                sum += heat_[yb * w + x];
                sum += heat_[yb * w + xr];
                sum += heat_[yb * w + x];
                heat_[y * w + x] = static_cast<uint8_t>(sum >> 2);
            }
        }

        // 3. Sparks at the base, scaled with width: a fixed count barely speckles a wide grid.
        if (h > 0 && w > 0) {
            lengthType bottomRow = static_cast<lengthType>(h - 1);
            lengthType sparks = w / 4;
            if (sparks < 4) sparks = 4;
            for (lengthType i = 0; i < sparks; i++) {
                if (rand8() < sparking) {
                    // A 16-bit random, since a single byte leaves columns past 256 unreachable.
                    const uint16_t r16 = static_cast<uint16_t>((rand8() << 8) | rand8());
                    lengthType sx = static_cast<lengthType>((static_cast<uint32_t>(r16) * w) >> 16);
                    uint8_t add = static_cast<uint8_t>(160 + (rand8() & 0x5F));
                    uint16_t newHeat = static_cast<uint16_t>(heat_[bottomRow * w + sx]) + add;
                    heat_[bottomRow * w + sx] = newHeat > 255 ? 255 : static_cast<uint8_t>(newHeat);
                }
            }
        }

        // 4. Heat is the palette index, and a cold cell stays black rather than tinting the sky.
        const Palette& pal = *Palettes::active();
        for (nrOfLightsType i = 0; i < heat_.count(); i++) {
            RGB c = heat_[i] == 0 ? RGB{0, 0, 0} : colorFromPalette(pal, heat_[i]);
            size_t off = static_cast<size_t>(i) * cpl;
            if (cpl >= 1) buf[off + 0] = c.r;
            if (cpl >= 2) buf[off + 1] = c.g;
            if (cpl >= 3) buf[off + 2] = c.b;
        }
    }

private:
    ScratchBuffer<uint8_t> heat_{*this};   ///< one byte of heat per light on the z=0 plane
    Random8 rng_{0xC0FFEEu};               ///< fixed-seed, so the goldens reproduce
    /// One random byte, the shape the cooling and sparking want.
    uint8_t rand8() { return rng_.next8(); }
};

} // namespace mm
