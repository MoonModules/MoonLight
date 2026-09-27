#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Effect that fills the whole layer with one palette color.
/// @card SolidEffect.gif
///
/// Five color modes: a flat color, the palette across the lights, its RMS average, or banded.
/// Brightness scales the flat and spread results.
///
/// Prior art: MoonLight's Solid, whose modes and shuffle this reproduces.
///
/// @moreinfo
///
/// ## The band modes
///
/// A `minRGB` floor drops near-black palette entries, so a dark gradient still bands visibly.
/// `randomColors` shuffles the survivors with a fixed generator, so the order is reproducible.
/// The flat color's own members go unused in every palette mode.
///
/// ## White is written only where the light has it
///
/// The optional white channel is written when the layer carries a fourth, and ignored otherwise.
/// Every palette mode clears it, since those modes carry no white source of their own.
class SolidEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// D3: the band modes write every axis, which keeps their orientation right in a volume.
    Dim dimensions() const override { return Dim::D3; }

    // Defaults match MoonLight's own Solid.
    /// The flat color's red channel.
    uint8_t red = 182;
    /// Its green channel.
    uint8_t green = 15;
    /// Its blue channel.
    uint8_t blue = 98;
    /// Its white channel, written only where the light carries one.
    uint8_t white = 0;
    /// Scales the flat and palette-spread results.
    uint8_t brightness = 255;
    /// Which of the five color modes fills the layer.
    uint8_t colorMode = 0;
    /// The band modes drop palette entries darker than this on every channel.
    uint8_t minRGB = 10;
    /// The band modes shuffle the surviving entries, reproducibly.
    bool    randomColors = false;

    static constexpr const char* kColorModeOptions[] = {
        "RGB(W)", "Palette", "Palette avg", "Palette rows", "Palette cols"};
    /// How many color modes the select offers.
    static constexpr uint8_t kColorModeCount = 5;

    /// Publish the flat color, the brightness, the mode and the band filters.
    void defineControls() override {
        controls_.addControl("red", red, 0, 255);
        controls_.addControl("green", green, 0, 255);
        controls_.addControl("blue", blue, 0, 255);
        controls_.addControl("white", white, 0, 255);
        controls_.addControl("brightness", brightness, 0, 255);
        controls_.addSelect("colorMode", colorMode, kColorModeOptions, kColorModeCount);
        controls_.addControl("minRGB", minRGB, 0, 255);
        controls_.addControl("randomColors", randomColors);
    }

    /// Allocate the band modes' wheel-index table, kept off the inline footprint.
    void prepare() override {
        // On the heap, since the registerType probe builds a throwaway instance on an 8 KB stack.
        validIndices_.resize(256);
    }

    /// Fill the layer in the selected mode, clearing any stale white behind it.
    void tick() MM_NONBLOCKING override {
        const int w = width();
        const int h = height();
        const int d = depth();

        const draw::Canvas cv = canvas();
        const lengthType dz = d > 0 ? static_cast<lengthType>(d) : 1;
        const Palette& pal = *Palettes::active();
        const uint8_t cpl = channelsPerLight();
        const nrOfLightsType nLights = nrOfLights();

        switch (colorMode) {
            case 0: {  // A flat color, with brightness applied per channel.
                const RGB c{static_cast<uint8_t>(red   * brightness / 255),
                            static_cast<uint8_t>(green * brightness / 255),
                            static_cast<uint8_t>(blue  * brightness / 255)};
                draw::fill(cv, c);
                // Written every frame so a stale white clears, and scaled so the whole color dims together.
                if (cpl >= 4) writeWhite(cv, nLights, cpl, static_cast<uint8_t>(white * brightness / 255));
                break;
            }
            case 1: {  // The palette spread across the lights, one wheel index each.
                uint8_t* data = cv.data;
                const size_t bytes = cv.bytes;
                for (nrOfLightsType i = 0; i < nLights; i++) {
                    const uint8_t idx = static_cast<uint8_t>(mapI(static_cast<int>(i), 0, static_cast<int>(nLights), 0, 256));
                    const RGB c = colorFromPalette(pal, idx, brightness);
                    const size_t off = static_cast<size_t>(i) * cpl;
                    // Only the channels this light has, or the write spills into the next light.
                    const uint8_t write = cpl < 3 ? cpl : 3;
                    if (off + write > bytes) break;
                    if (write >= 1) data[off + 0] = c.r;
                    if (write >= 2) data[off + 1] = c.g;
                    if (write >= 3) data[off + 2] = c.b;
                }
                // No white source here, so clear it rather than leaving a stale value.
                if (cpl >= 4) writeWhite(cv, nLights, cpl, 0);
                break;
            }
            case 2: {  // The RMS average of the palette's non-black colors, filled solid.
                uint32_t sumR = 0, sumG = 0, sumB = 0;
                int n = 0;
                for (int i = 0; i < 256; i++) {
                    const RGB e = colorFromPalette(pal, static_cast<uint8_t>(i));
                    if (e.r == 0 && e.g == 0 && e.b == 0) continue;   // black would drag the average down
                    sumR += static_cast<uint32_t>(e.r) * e.r;
                    sumG += static_cast<uint32_t>(e.g) * e.g;
                    sumB += static_cast<uint32_t>(e.b) * e.b;
                    n++;
                }
                RGB avg{0, 0, 0};
                if (n > 0) {
                    avg.r = static_cast<uint8_t>(sqrtf(static_cast<float>(sumR) / n));
                    avg.g = static_cast<uint8_t>(sqrtf(static_cast<float>(sumG) / n));
                    avg.b = static_cast<uint8_t>(sqrtf(static_cast<float>(sumB) / n));
                }
                draw::fill(cv, avg);
                if (cpl >= 4) writeWhite(cv, nLights, cpl, 0);   // no white source, so clear it
                break;
            }
            default: {  // Band the filtered palette along the rows or the columns.
                const bool rows = (colorMode == 3);
                const int axisSize = rows ? h : w;

                // The wheel indices bright enough on any channel to survive the floor.
                int nrValid = 0;
                if (validIndices_) {
                    for (int i = 0; i < 256; i++) {
                        const RGB e = colorFromPalette(pal, static_cast<uint8_t>(i));
                        if (e.r >= minRGB || e.g >= minRGB || e.b >= minRGB)
                            validIndices_[nrValid++] = static_cast<uint8_t>(i);
                    }
                    if (randomColors && nrValid > 1) {
                        // Fisher-Yates on a fixed generator, so the order reproduces.
                        uint32_t seed = 12345u;
                        for (int i = nrValid - 1; i > 0; i--) {
                            seed = seed * 25173u + 13849u;
                            const int j = static_cast<int>(seed % static_cast<uint32_t>(i + 1));
                            const uint8_t t = validIndices_[i];
                            validIndices_[i] = validIndices_[j];
                            validIndices_[j] = t;
                        }
                    }
                }

                for (int z = 0; z < dz; z++) {
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < w; x++) {
                            const int axisValue = rows ? y : x;
                            uint8_t idx;
                            if (nrValid > 0 && validIndices_) {
                                const int vi = axisSize <= 1 ? 0
                                             : mapI(axisValue, 0, axisSize - 1, 0, nrValid - 1);
                                idx = validIndices_[vi];
                            } else {
                                // Nothing survived the floor, so map the axis straight onto the wheel.
                                idx = axisSize <= 1 ? 0
                                    : static_cast<uint8_t>(mapI(axisValue, 0, axisSize - 1, 0, 255));
                            }
                            const RGB c = colorFromPalette(pal, idx, brightness);
                            draw::pixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(y),
                                                    static_cast<lengthType>(z)}, c);
                        }
                    }
                }
                // No white source here either, so clear it.
                if (cpl >= 4) writeWhite(cv, nLights, cpl, 0);
                break;
            }
        }
    }

private:
    ScratchBuffer<uint8_t> validIndices_{*this};   ///< the band modes' surviving wheel indices

    /// Write the white channel on every light, since the draw primitives touch RGB only.
    static void writeWhite(const draw::Canvas& cv, nrOfLightsType n, uint8_t cpl, uint8_t w) {
        uint8_t* data = cv.data;
        const size_t bytes = cv.bytes;
        for (nrOfLightsType i = 0; i < n; i++) {
            const size_t off = static_cast<size_t>(i) * cpl + 3;
            if (off >= bytes) break;
            data[off] = w;
        }
    }

    /// The standard integer map, guarding a zero span so a degenerate axis still maps.
    static int mapI(int x, int inLo, int inHi, int outLo, int outHi) {
        const int den = inHi - inLo;
        if (den == 0) return outLo;
        return (x - inLo) * (outHi - outLo) / den + outLo;
    }
};

} // namespace mm
