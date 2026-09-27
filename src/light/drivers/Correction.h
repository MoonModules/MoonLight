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
/// A light's wire format is a `ChannelRole` array, resolved from the preset library into a `Correction` at rebuild time.
/// The curated orders are seeded rows in that library rather than an enum here.

/// More than one algorithm is accepted, so white derivation is a mode rather than a formula.
enum class WhiteMode : uint8_t { None, Min, Accurate };

/// Dropdown labels for WhiteMode, in enum order.
inline constexpr const char* kWhiteModeOptions[] = {"None", "Min", "Accurate"};
/// How many modes are on offer.
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


    // White, amber and UV are their own dies, so an RGB trim must not reach them; the white dies carry a trim of their own.
    /// Row of `briLut` carrying no trim, for a channel no RGB balance may reach.
    static constexpr uint8_t kNeutral = 3;
    /// Row of `briLut` carrying the white die's own trim.
    static constexpr uint8_t kWhite = 4;
    /// briLut[ch][v] = curve(v * brightness * balance[ch]), with rows 0=R, 1=G, 2=B, 3=untrimmed, 4=white.
    uint8_t briLut[5][256] = {};
    /// Per-channel white balance, 255 = untouched. Trim DOWN only: there is no headroom above 255, so raising clips instead of balancing.
    uint8_t balRed = 255, balGreen = 255, balBlue = 255;
    /// The white die's trim, 255 = untouched: a separate emitter, often brighter than the RGB trio, that the three trims above cannot reach. Pre-scales like them, so the curve still lands last.
    uint8_t whiteLevel = 255;
    /// Which curve the brightness rebuild fills through; a driver's setting, not a global one.
    Curve curve = Curve::Cie;
    // The output-byte position of each color role, recomputed from the role array.
    /// Output byte position of the red role.
    uint8_t offRed = 1;
    /// Output byte position of the green role.
    uint8_t offGreen = 0;
    /// Output byte position of the blue role.
    uint8_t offBlue = 2;
    /// Output byte position of the derived white, or kAbsent when the light has none.
    uint8_t offWhite = kAbsent;
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
    /// Bytes emitted per light, which is the wiring's channelsPerLight.
    uint8_t outChannels = 3;
    /// How white is synthesized from RGB, on lights that carry a white die.
    WhiteMode whiteMode = WhiteMode::Min;

    // Priced per CHANNEL rather than per light, since under-reporting the draw browns out a supply.
    /// The supply budget a frame is priced against, in milliamps; 0 disables the limiter.
    uint16_t budgetMa = 0;
    /// Draw of one R/G/B channel at 255, in milliamps.
    uint8_t mAColor = 8;
    /// Draw of one white channel at 255, which is about twice a color one.
    uint8_t mAWhite = 16;
    /// Draw of one amber channel at 255, assumed rather than measured.
    uint8_t mAYellow = 8;
    /// Draw of one UV channel at 255, assumed rather than measured.
    uint8_t mAUV = 8;
    /// What measure() set; 256 = unity, so an unlimited frame is bit-exact.
    uint16_t limit = 256;

    // ORDER is the whole design: brightness is a linear pre-scale and the curve is applied LAST.
    /// Refresh the brightness LUT alone, leaving the channel offsets untouched.
    void rebuildBrightness(uint8_t brightness) {
        // The trim pre-scales like brightness, so the curve still lands last.
        const uint8_t balance[5] = {balRed, balGreen, balBlue, 255, whiteLevel};
        for (int ch = 0; ch < 5; ch++) {
            const float scale = static_cast<float>(brightness) * balance[ch] / 255.0f;
            for (int v = 0; v < 256; v++) {
                const float linear = static_cast<float>(v) * scale / 255.0f;   // scale first
                float out = linear;
                switch (curve) {
                    case Curve::Cie:     out = cieLuminance(linear) * 255.0f; break;
                    case Curve::Gamma22: out = powf(linear / 255.0f, 2.2f) * 255.0f; break;
                    case Curve::Gamma28: out = powf(linear / 255.0f, 2.8f) * 255.0f; break;
                    case Curve::Linear:  break;
                }
                int q = static_cast<int>(out + 0.5f);
                // A non-zero input never lands on black, or a fade-out snaps off partway down.
                if (q <= 0 && v > 0 && scale > 0.0f) q = 1;
                briLut[ch][v] = static_cast<uint8_t>(q > 255 ? 255 : q);
            }
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

    /// The white component of a source triple, min(r,g,b); 0 when nothing is synthesized. Shared by measure() and apply(), so the estimate cannot drift from what is emitted.
    uint8_t whiteOf(uint8_t r, uint8_t g, uint8_t b) const {
        if (whiteMode == WhiteMode::None) return 0;
        return r < g ? (r < b ? r : b) : (g < b ? g : b);
    }

    /// Scale one emitted byte by the frame's current limit.
    uint8_t lim(uint8_t v) const { return static_cast<uint8_t>((v * limit) >> 8); }

    // Prices what apply() EMITS, curve included: the draw follows the die, not the source byte.
    /// Once per frame before the emit loop: price `n` lights and set `limit`.
    void measure(const uint8_t* src, uint8_t srcCh, uint32_t n) {
        limit = 256;
        if (budgetMa == 0) return;

        uint32_t whiteMa = 0;
        if (offWhite != kAbsent) whiteMa += mAWhite;
        if (offWarmWhite != kAbsent) whiteMa += mAWhite;
        const bool subtractWhite = offWhite != kAbsent && whiteMode == WhiteMode::Accurate;
        const bool anyWhite = whiteMode != WhiteMode::None;
        const uint32_t yellowMa = (anyWhite && offYellow != kAbsent) ? mAYellow : 0;
        const uint32_t uvMa = (anyWhite && offUV != kAbsent) ? mAUV : 0;

        uint64_t sum = 0;
        for (uint32_t i = 0; i < n; i++, src += srcCh) {
            uint8_t r = src[0], g = src[1], b = src[2];
            const uint8_t w = whiteOf(r, g, b);
            // Off the PRE-subtraction values, as apply() reads them.
            if (yellowMa) sum += static_cast<uint32_t>(briLut[kNeutral][r < g ? r : g]) * yellowMa;
            if (uvMa) {
                const uint8_t rg = r > g ? r : g;
                sum += static_cast<uint32_t>(briLut[kNeutral][b > rg ? static_cast<uint8_t>(b - rg) : 0]) * uvMa;
            }
            if (subtractWhite) {
                r -= w;
                g -= w;
                b -= w;
            }
            sum += (static_cast<uint64_t>(briLut[0][r]) + briLut[1][g] + briLut[2][b]) * mAColor
                   + static_cast<uint64_t>(briLut[kWhite][w]) * whiteMa;
        }
        // Rounded UP: a cap that understates is not a cap.
        const uint64_t scalableMa = (sum + 254) / 255;
        // The dimmer and motion are not priced: DMX control values draw nothing from this rail.
        if (scalableMa > budgetMa) limit = static_cast<uint16_t>((budgetMa * 256u) / scalableMa);
    }

    // A REMAP, not a copy: the layer's packed slots become the fixture's own offsets.
    /// Hot path: transform one source light into its output bytes, integer-only and allocation-free.
    inline void apply(const uint8_t* src, uint8_t* out, uint8_t srcChannels) const {
        // Wide open, and written every frame, so a preset declaring one cannot be silently unlit.
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
        // One gate for every synthesized emitter; None zeroes them, since the buffer is reused.
        if (whiteMode == WhiteMode::None) {
            if (offWhite != kAbsent)     out[offWhite] = 0;
            if (offWarmWhite != kAbsent) out[offWarmWhite] = 0;
            if (offYellow != kAbsent)    out[offYellow] = 0;
            if (offUV != kAbsent)        out[offUV] = 0;
        } else {
            const uint8_t w = whiteOf(r, g, b);
            // Computed off the PRE-subtraction values, which only rebalance the RGB emitters.
            if (offWarmWhite != kAbsent) out[offWarmWhite] = lim(briLut[kWhite][w]);
            // yellow ≈ min(R,G) (the shared red+green component).
            if (offYellow != kAbsent)    out[offYellow] = lim(briLut[kNeutral][r < g ? r : g]);
            // Driven from the blue with no red or green to pair with, so it stays dark on warm colors.
            if (offUV != kAbsent) {
                const uint8_t rg = r > g ? r : g;
                out[offUV] = lim(briLut[kNeutral][b > rg ? static_cast<uint8_t>(b - rg) : 0]);
            }
            // White last: it is the only emitter that rebalances RGB.
            if (offWhite != kAbsent) {
                if (whiteMode == WhiteMode::Accurate) { r -= w; g -= w; b -= w; }  // pull white out of RGB
                out[offWhite] = lim(briLut[kWhite][w]);
            }
        }
        // The curve, applied ONCE: everything above this line is linear light.
        if (offRed != kAbsent)   out[offRed] = lim(briLut[0][r]);
        if (offGreen != kAbsent) out[offGreen] = lim(briLut[1][g]);
        if (offBlue != kAbsent)  out[offBlue] = lim(briLut[2][b]);
    }
};

/// @}
} // namespace mm
