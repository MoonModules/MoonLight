#pragma once

#include <cmath>   // powf: the gamma presets, cold path only

#include <cstdint>

#include "light/drivers/ChannelRole.h"
#include "light/util/FixtureChannels.h"   // kMotionBase + forEachMotionSlot: the layer-slot packing

namespace mm {

/// @defgroup Correction The per-light output transform
/// @{
/// Brightness, channel reorder and white derivation, resolved once and applied per channel.
///
/// @moreinfo
///
/// A light's wire format is a `ChannelRole` array, resolved from the fixture-profile library into a `Correction` at rebuild time.
/// The curated orders are seeded rows in that library rather than an enum here.

/// More than one algorithm is accepted, so white derivation is a mode rather than a formula.
enum class WhiteMode : uint8_t { None, Min, Accurate };

inline constexpr const char* kWhiteModeOptions[] = {"None", "Min", "Accurate"};
inline constexpr uint8_t kWhiteModeCount =
    sizeof(kWhiteModeOptions) / sizeof(kWhiteModeOptions[0]);


/// The transform itself, its offsets derived cold from the role array so the hot path stays an indexed store per channel.
struct Correction {
    /// Marks a role this light does not carry.
    static constexpr uint8_t kAbsent = 255;   // color role not carried by this light

    // Linear is a REQUIREMENT, not a fallback: correcting twice darkens as the square.
    /// The perceptual curve the output LUT is filled through.
    enum class Curve : uint8_t { Cie = 0, Gamma22, Gamma28, Linear };

    // The constants are load-bearing: they place the toe so the two segments meet in slope.
    /// CIE 1931 lightness, inverted: a control position to a luminance fraction.
    static float cieLuminance(float control255) {
        const float L = control255 * 100.0f / 255.0f;
        return (L <= 8.0f) ? (L / 903.3f)
                           : ((L + 16.0f) / 116.0f) * ((L + 16.0f) / 116.0f) * ((L + 16.0f) / 116.0f);
    }


    uint8_t briLut[256] = {};       // briLut[v] = curve(v * brightness / 255)
    /// Which curve the brightness rebuild fills through; a driver's setting, not a global one.
    Curve curve = Curve::Cie;
    // The output-byte position of each color role, recomputed from the role array.
    /// Output byte position of the red role.
    uint8_t offRed = 1;
    /// Output byte position of the green role.
    uint8_t offGreen = 0;
    /// Output byte position of the blue role.
    uint8_t offBlue = 2;
    uint8_t offWhite = kAbsent;     // derived white at this offset (kAbsent = light has no white)
    // Warm white has a real achromatic basis; amber and UV are eyeball approximations, honestly so.
    /// Output byte positions of the extra emitters beside cold white.
    uint8_t offWarmWhite = kAbsent;
    // Held wide open but always WRITTEN: brightness is already in the colors, and 0 is dark.
    /// Output byte position of the fixture's master dimmer.
    uint8_t offDimmer = kAbsent;
    // Never scaled by brightness: dimming the rig would otherwise swing every head toward zero.
    /// Output byte positions of the fixture's motion channels.
    uint8_t offPan = kAbsent, offTilt = kAbsent, offZoom = kAbsent;
    /// Output byte positions of the rotate and gobo channels.
    uint8_t offRotate = kAbsent, offGobo = kAbsent;
    // Resolved once at rebuild, so the hot path never scans five offsets to find them absent.
    /// Whether this fixture carries any motion channel.
    bool hasMotion = false;
    // Plain rather than atomic: byte-sized so a read cannot tear, and one frame late costs nothing.
    /// Hold the rig's aim, so motion stops reaching the wire and a fixture keeps its position.
    bool motionHeld = false;
    /// Output byte position of the amber role.
    uint8_t offYellow = kAbsent;
    /// Output byte position of the UV emitter.
    uint8_t offUV = kAbsent;
    uint8_t outChannels = 3;        // bytes emitted per light (= channelsPerLight of the wiring)
    WhiteMode whiteMode = WhiteMode::Min;   // how white is synthesized from RGB (white lights only)

    // ORDER is the whole design: brightness is a linear pre-scale and the curve is applied LAST.
    /// Refresh the brightness LUT alone, leaving the channel offsets untouched.
    void rebuildBrightness(uint8_t brightness) {
        for (int v = 0; v < 256; v++) {
            const float linear = static_cast<float>(v) * brightness / 255.0f;   // scale first
            float out = linear;
            switch (curve) {
                case Curve::Cie:     out = cieLuminance(linear) * 255.0f; break;
                case Curve::Gamma22: out = powf(linear / 255.0f, 2.2f) * 255.0f; break;
                case Curve::Gamma28: out = powf(linear / 255.0f, 2.8f) * 255.0f; break;
                case Curve::Linear:  break;
            }
            int q = static_cast<int>(out + 0.5f);
            // A non-zero input never lands on black, or a fade-out snaps off partway down.
            if (q <= 0 && v > 0 && brightness > 0) q = 1;
            briLut[v] = static_cast<uint8_t>(q > 255 ? 255 : q);
        }
    }

    // Cold path: a role at channel i sets that offset to i, and one not present stays absent.
    /// Refresh the LUT and derive every channel offset from the light's role array.
    void rebuild(uint8_t brightness, const ChannelRole* roles, uint8_t nChannels) {
        rebuildBrightness(brightness);
        offRed = offGreen = offBlue = offWhite = kAbsent;
        offWarmWhite = offYellow = offUV = offDimmer = kAbsent;
        offPan = offTilt = offZoom = offRotate = offGobo = kAbsent;
        hasMotion = false;
        for (uint8_t i = 0; i < nChannels; i++) {
            switch (roles[i]) {
                case ChannelRole::Red:       offRed = i;       break;
                case ChannelRole::Green:     offGreen = i;     break;
                case ChannelRole::Blue:      offBlue = i;      break;
                case ChannelRole::White:     offWhite = i;     break;
                case ChannelRole::WarmWhite: offWarmWhite = i; break;
                case ChannelRole::Yellow:    offYellow = i;    break;
                case ChannelRole::UV:        offUV = i;        break;
                case ChannelRole::Dimmer:    offDimmer = i;    break;
                case ChannelRole::Pan:       offPan = i;       break;
                case ChannelRole::Tilt:      offTilt = i;      break;
                case ChannelRole::Zoom:      offZoom = i;      break;
                case ChannelRole::Rotate:    offRotate = i;    break;
                case ChannelRole::Gobo:      offGobo = i;      break;
                default: break;   // ChannelRole::None: a channel this fixture does not use
            }
        }
        hasMotion = offPan != kAbsent || offTilt != kAbsent || offZoom != kAbsent ||
                    offRotate != kAbsent || offGobo != kAbsent;
        outChannels = nChannels;
    }

    // A REMAP, not a copy: the layer's packed slots become the fixture's own offsets.
    /// Hot path: transform one source light into its output bytes, integer-only and allocation-free.
    inline void apply(const uint8_t* src, uint8_t* out, uint8_t srcChannels) const {
        // Wide open, and written every frame, so a profile declaring one cannot be silently unlit.
        if (offDimmer != kAbsent) out[offDimmer] = 255;
        // Unscaled and by ASSIGNMENT: additive semantics do not apply to positional signals.
        if (hasMotion && srcChannels != 0 && !motionHeld) {
            // Read the LAYER slot, write the FIXTURE channel: two layouts, mapped here.
            const bool present[5] = {offPan != kAbsent, offTilt != kAbsent, offZoom != kAbsent,
                                     offRotate != kAbsent, offGobo != kAbsent};
            const uint8_t chan[5] = {offPan, offTilt, offZoom, offRotate, offGobo};
            FixtureChannels::forEachMotionSlot(present, [&](uint8_t role, uint8_t slot) {
                if (slot < srcChannels) out[chan[role]] = src[slot];
            });
        }
        // The white math runs on the LINEAR source: on curved values it would not mean what it says.
        uint8_t r = src[0];
        uint8_t g = src[1];
        uint8_t b = src[2];
        // One gate for every synthesised emitter; None zeroes them, since the buffer is reused.
        if (whiteMode == WhiteMode::None) {
            if (offWhite != kAbsent)     out[offWhite] = 0;
            if (offWarmWhite != kAbsent) out[offWarmWhite] = 0;
            if (offYellow != kAbsent)    out[offYellow] = 0;
            if (offUV != kAbsent)        out[offUV] = 0;
        } else {
            const uint8_t w = r < g ? (r < b ? r : b) : (g < b ? g : b);  // min(r,g,b): the white component
            // Computed off the PRE-subtraction values, which only rebalance the RGB emitters.
            if (offWarmWhite != kAbsent) out[offWarmWhite] = briLut[w];
            // yellow ≈ min(R,G) (the shared red+green component).
            if (offYellow != kAbsent)    out[offYellow] = briLut[r < g ? r : g];
            // Driven from the blue with no red or green to pair with, so it stays dark on warm colors.
            if (offUV != kAbsent) {
                const uint8_t rg = r > g ? r : g;
                out[offUV] = briLut[b > rg ? static_cast<uint8_t>(b - rg) : 0];
            }
            // White last: it is the only emitter that rebalances RGB.
            if (offWhite != kAbsent) {
                if (whiteMode == WhiteMode::Accurate) { r -= w; g -= w; b -= w; }  // pull white out of RGB
                out[offWhite] = briLut[w];
            }
        }
        // The curve, applied ONCE: everything above this line is linear light.
        if (offRed != kAbsent)   out[offRed] = briLut[r];
        if (offGreen != kAbsent) out[offGreen] = briLut[g];
        if (offBlue != kAbsent)  out[offBlue] = briLut[b];
    }
};

/// @}
} // namespace mm
