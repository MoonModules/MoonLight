#pragma once

#include "core/util/ActiveInstance.h"   // the seat the light domain claims
#include "core/util/color.h"            // RGB and the hue conversions the palette helpers use

#include <cstdint>

namespace mm {

class JsonSink;

/// @defgroup LightOutput The light output, as core sees it
/// @{
/// What the domain-neutral transports read from the light domain: the device's shape, and its palettes as colors.
///
/// @moreinfo
///
/// ## Core declares, light implements
///
/// The light domain's `Drivers` container implements `LightOutput` and claims its seat from prepare, and the WLED shim, MQTT and MoonStats read through it.
/// So core reaches the light pipeline without including a light header: the dependency points inward, as the architecture's dependency rule asks.
/// On a device with no light output the seat is empty, and every reader degrades to the all-zero summary and palette 0.
///
/// ## The summary is plain data
///
/// `LightSummary` is a flat struct of small integers, overwritten in place on each rebuild, the same pull pattern as `AudioFrame`.
/// `lightCount` is a `uint32_t` to hold any count on any board, the light-side `nrOfLightsType` being a `uint16_t` without PSRAM and a `uint32_t` with it.
/// A new field means the producer fills it in one place and every consumer that wants it reads it, with no new seam.
///
/// ## Palettes as colors
///
/// A color wheel (Home Assistant, HomeKit through MQTT, the WLED app) picks a palette by color.
/// Matching happens in hue and saturation, so the interface carries those two, and the RGB forms are conversions on top.

/// A small plain-data summary of the light pipeline's output.
struct LightSummary {
    uint32_t lightCount = 0;              ///< total physical lights driven (Layer::physicalLightCount()).
    uint8_t  channelsPerLight = 3;        ///< 3 = RGB, 4 = RGBW, more = multi-channel DMX fixtures.
};

/// The light domain as the domain-neutral transports reach it.
class LightOutput {
public:
    /// The live pipeline's shape.
    virtual const LightSummary& summary() const = 0;
    /// The palette whose representative color is nearest a hue (0..359) and saturation (0..255).
    virtual uint8_t nearestPalette(uint16_t hue, uint8_t sat) const = 0;
    /// A palette's representative hue (0..359) and saturation (0..255).
    virtual void paletteHueSat(uint8_t index, uint16_t& hue, uint16_t& sat) const = 0;
    /// How many palettes the picker offers, the built-ins and the scripted tail.
    virtual uint8_t paletteCount() const = 0;
    /// Every palette name as JSON strings, comma-separated, in picker order.
    virtual void writePaletteNames(JsonSink& sink) const = 0;

    // Full value, so brightness stays the caller's concern and nothing dims twice.
    /// A palette's representative color.
    RGB paletteRgb(uint8_t index) const {
        uint16_t hue = 0, sat = 0;
        paletteHueSat(index, hue, sat);
        return hsvToRgb(static_cast<uint8_t>((hue * 256u) / 360u), static_cast<uint8_t>(sat), 255);
    }
    /// The palette nearest an RGB color, through the same conversion the palettes' own colors take.
    uint8_t nearestPalette(RGB c) const {
        uint16_t hue = 0, sat = 0;
        rgbToHueSat(c.r, c.g, c.b, hue, sat);
        return nearestPalette(hue, static_cast<uint8_t>(sat));
    }

    /// The live light output, or null on a device with none.
    static const LightOutput* active() { return ActiveInstance<LightOutput>::active(); }

protected:
    ~LightOutput() = default;
};

/// The live summary, or the all-zero one when no light output is running.
inline const LightSummary& lightSummary() {
    static const LightSummary kNone{};
    const LightOutput* out = LightOutput::active();
    return out ? out->summary() : kNone;
}

/// @}

}  // namespace mm
