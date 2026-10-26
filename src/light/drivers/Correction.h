#pragma once

#include <cmath>   // powf: the gamma presets, off the per-frame path; the soft start rebakes them each tick for five seconds
#include "platform/nonblocking.h"   // MM_NONBLOCKING: the brightness table rebakes on the soft start's tick
#include "platform_config.h"   // MM_RAMFUNC: the ring's refill interrupt corrects every light it encodes

#include <cstdint>
#include <initializer_list>   // anyPresent: the offsets a fixture may carry

#include "light/drivers/ChannelRole.h"
#include "light/util/FixtureChannels.h"   // kMotionBase + forEachMotionSlot: the layer-slot packing

namespace mm {

/// Brightness, channel reorder and white derivation, resolved once and applied per channel.
///
/// @technical
///
/// A light's wire format is a `ChannelRole` array, resolved from the fixture-profile library into a `Correction` at rebuild time. The curated orders are seeded rows in that library rather than an enum here.

/// More than one algorithm is accepted, so white derivation is a mode rather than a formula.
enum class WhiteMode : uint8_t { None, Min, Accurate };

inline constexpr const char* kWhiteModeOptions[] = {"None", "Min", "Accurate"};
inline constexpr uint8_t kWhiteModeCount =
    sizeof(kWhiteModeOptions) / sizeof(kWhiteModeOptions[0]);


/// The transform itself, its offsets derived cold from the role array so the hot path stays an indexed store per channel.
struct Correction {
    /// Marks a role this light does not carry.
    static constexpr uint8_t kAbsent = 255;   // color role not carried by this light

    /// The perceptual curve the output LUT is filled through; Linear is a requirement, not a fallback, since correcting twice darkens as the square.
    enum class Curve : uint8_t { Cie = 0, Gamma22, Gamma28, Linear };

    /// CIE 1931 lightness, inverted: a control position to a luminance fraction; the constants place the toe so the two segments meet in slope.
    static float cieLuminance(float control255) MM_NONBLOCKING {
        const float L = control255 * 100.0f / 255.0f;
        return (L <= 8.0f) ? (L / 903.3f)
                           : ((L + 16.0f) / 116.0f) * ((L + 16.0f) / 116.0f) * ((L + 16.0f) / 116.0f);
    }


    uint8_t briLut[256] = {};       // briLut[v] = curve(v * brightness / 255)
    /// The same curve to 16 bits, filled only when the table is wanted; the driver owns the 256 entries.
    uint16_t* lut16 = nullptr;
    /// Set by a driver whose output resolves more than a byte, so the 16-bit table is filled whatever the profile.
    bool wide = false;
    /// Per-channel white balance, 255 = untouched; trimmed down only, since there is no headroom above 255 and raising clips instead.
    uint8_t balRed = 255, balGreen = 255, balBlue = 255;
    /// The white die's trim, 255 = untouched; a separate emitter, often brighter than the RGB trio, that the three trims above cannot reach.
    uint8_t whiteLevel = 255;
    /// Which curve the brightness rebuild fills through; a driver's setting, not a global one.
    Curve curve = Curve::Cie;
    /// Output byte position of the red role; each color role's position is recomputed from the role array.
    uint8_t offRed = 1;
    /// Output byte position of the green role.
    uint8_t offGreen = 0;
    /// Output byte position of the blue role.
    uint8_t offBlue = 2;
    uint8_t offWhite = kAbsent;     // derived white at this offset (kAbsent = light has no white)
    /// Output byte positions of the extra emitters beside cold white; warm white has a real achromatic basis, amber and UV are eyeball approximations.
    uint8_t offWarmWhite = kAbsent;
    /// Output byte position of the fixture's master dimmer, held wide open but always written: brightness is already in the colors, and 0 is dark.
    uint8_t offDimmer = kAbsent;
    /// Output byte positions of the fixture's motion channels, never scaled by brightness: dimming the rig would otherwise swing every head toward zero.
    uint8_t offPan = kAbsent, offTilt = kAbsent, offZoom = kAbsent;
    /// Output byte positions of the rotate and gobo channels.
    uint8_t offRotate = kAbsent, offGobo = kAbsent;
    /// Whether this fixture carries any motion channel, resolved once at rebuild so the hot path never scans five offsets.
    bool hasMotion = false;
    /// Hold the rig's aim, so motion stops reaching the wire and a fixture keeps its position; a plain byte, since one frame late costs nothing.
    bool motionHeld = false;
    /// Output byte position of the amber role.
    uint8_t offYellow = kAbsent;
    /// Output byte position of the UV emitter.
    uint8_t offUV = kAbsent;
    /// Output byte positions of the fine (low) bytes of 16-bit red, green, blue, white and warm white.
    uint8_t offRedFine = kAbsent, offGreenFine = kAbsent, offBlueFine = kAbsent, offWhiteFine = kAbsent, offWarmWhiteFine = kAbsent;
    /// Output byte positions of the fine bytes of 16-bit pan, tilt and master dimmer.
    uint8_t offPanFine = kAbsent, offTiltFine = kAbsent, offDimmerFine = kAbsent;
    /// Whether any color or dimmer channel is 16-bit, resolved at rebuild so an 8-bit profile pays one predictable branch per light.
    bool hasFine = false;
    /// Whether the fixture carries red, green and blue alone, resolved at rebuild so a plain strip skips every optional role per light.
    bool plainRgb = true;
    uint8_t outChannels = 3;        // bytes emitted per light (= channelsPerLight of the wiring)
    WhiteMode whiteMode = WhiteMode::Min;   // how white is synthesized from RGB (white lights only)

    /// Refresh the brightness LUT alone, leaving the channel offsets untouched; order is the design: brightness is a linear pre-scale and the curve is applied last.
    void rebuildBrightness(uint8_t brightness) MM_NONBLOCKING {
        for (int v = 0; v < 256; v++) {
            const float out = shape(static_cast<float>(v) * brightness / 255.0f);   // scale first, then curve
            const bool lit = v > 0 && brightness > 0;
            briLut[v] = static_cast<uint8_t>(quantize(out, 255, lit));
            if (wantsTable() && lut16) lut16[v] = static_cast<uint16_t>(quantize(out * 257.0f, 65535, lit));   // 255 * 257 = 65535
        }
    }

    /// Whether the 16-bit table is filled: a profile with fine roles, or a wide driver.
    bool wantsTable() const MM_NONBLOCKING { return hasFine || wide; }

    /// The curve applied to a linear 0..255 value, still on the 0..255 scale.
    float shape(float linear) const MM_NONBLOCKING {
        switch (curve) {
            case Curve::Cie:     return cieLuminance(linear) * 255.0f;
            case Curve::Gamma22: return powf(linear / 255.0f, 2.2f) * 255.0f;
            case Curve::Gamma28: return powf(linear / 255.0f, 2.8f) * 255.0f;
            case Curve::Linear:  break;
        }
        return linear;
    }

    /// One channel trimmed down by `bal`, where 255 leaves the byte alone. A pre-scale on the index, not a table per trim, which would cost every driver a kilobyte of its inline budget.
    static uint8_t trim(uint8_t v, uint8_t bal) MM_NONBLOCKING {
        return bal == 255 ? v : static_cast<uint8_t>((static_cast<uint16_t>(v) * bal + 127) / 255);
    }

    /// Round a curved value to an integer of at most `top`, keeping a lit input above zero so a fade-out does not snap off partway down.
    static int quantize(float out, int top, bool lit) MM_NONBLOCKING {
        int q = static_cast<int>(out + 0.5f);
        if (q <= 0 && lit) q = 1;
        return q > top ? top : q;
    }

    /// Whether any of these offsets is carried.
    static bool anyPresent(std::initializer_list<uint8_t> offsets) {
        for (const uint8_t o : offsets) if (o != kAbsent) return true;
        return false;
    }

    /// Refresh the LUT and derive each channel offset from the role array (cold path): a role at channel i sets offset i, an absent role stays absent.
    void rebuild(uint8_t brightness, const ChannelRole* roles, uint8_t nChannels) {
        // The offset each role sets, in ChannelRole order; None sets nothing.
        static constexpr uint8_t Correction::* kOffsetOf[] = {
            nullptr, &Correction::offRed, &Correction::offGreen, &Correction::offBlue, &Correction::offWhite,
            &Correction::offWarmWhite, &Correction::offYellow, &Correction::offUV,
            &Correction::offPan, &Correction::offTilt, &Correction::offZoom, &Correction::offRotate, &Correction::offGobo,
            &Correction::offDimmer, &Correction::offRedFine, &Correction::offGreenFine, &Correction::offBlueFine,
            &Correction::offWhiteFine, &Correction::offWarmWhiteFine, &Correction::offPanFine, &Correction::offTiltFine,
            &Correction::offDimmerFine};
        static_assert(sizeof(kOffsetOf) / sizeof(kOffsetOf[0]) == kChannelRoleCount, "one offset per ChannelRole, in enum order");
        for (auto m : kOffsetOf) if (m) this->*m = kAbsent;
        for (uint8_t i = 0; i < nChannels; i++) {
            const uint8_t r = static_cast<uint8_t>(roles[i]);
            if (r < kChannelRoleCount && kOffsetOf[r]) this->*kOffsetOf[r] = i;   // the last channel of a role wins
        }
        hasMotion = anyPresent({offPan, offTilt, offZoom, offRotate, offGobo});
        hasFine = anyPresent({offRedFine, offGreenFine, offBlueFine, offWhiteFine, offWarmWhiteFine, offDimmerFine});
        plainRgb = !hasMotion && !hasFine && offDimmer == kAbsent && !anyPresent({offWhite, offWarmWhite, offYellow, offUV});
        outChannels = nChannels;
        // After the offsets: whether the 16-bit table is filled depends on hasFine.
        rebuildBrightness(brightness);
    }

    /// Hot path: transform one source light into its output bytes, integer-only and allocation-free.
    inline void apply(const uint8_t* src, uint8_t* out, uint8_t srcChannels) const { applyThrough(briLut, src, out, srcChannels); }

    /// Hot path, wide drivers only: the same transform through the 16-bit table, one value per channel; needs `lut16`.
    inline void applyWide(const uint8_t* src, uint16_t* out, uint8_t srcChannels) const { applyThrough(lut16, src, out, srcChannels); }

    /// The transform through a curve table of either width, so the 8-bit and 16-bit outputs share every line. A remap, not a copy: the layer's packed slots become the fixture's own offsets.
    template <typename T>
    inline void MM_RAMFUNC applyThrough(const T* lut, const uint8_t* src, T* out, uint8_t srcChannels) const {
        // A plain strip: the curve on each color and nothing optional, the drivers' commonest light.
        if (plainRgb) {
            if (offRed != kAbsent)   out[offRed] = lut[trim(src[0], balRed)];
            if (offGreen != kAbsent) out[offGreen] = lut[trim(src[1], balGreen)];
            if (offBlue != kAbsent)  out[offBlue] = lut[trim(src[2], balBlue)];
            return;
        }
        constexpr T kFull = static_cast<T>(~T{0});   // 255 or 65535
        // Wide open, and written every frame, so a profile declaring one cannot be silently unlit.
        if (offDimmer != kAbsent) out[offDimmer] = kFull;
        // Unscaled and by ASSIGNMENT: additive semantics do not apply to positional signals.
        if (hasMotion && srcChannels != 0 && !motionHeld) {
            // Read the LAYER slot, write the FIXTURE channel: two layouts, mapped here.
            const bool present[5] = {offPan != kAbsent, offTilt != kAbsent, offZoom != kAbsent,
                                     offRotate != kAbsent, offGobo != kAbsent};
            const uint8_t chan[5] = {offPan, offTilt, offZoom, offRotate, offGobo};
            FixtureChannels::forEachMotionSlot(present, [&](uint8_t role, uint8_t slot) {
                if (slot < srcChannels) out[chan[role]] = static_cast<T>(src[slot] * (kFull / 255));
            });
            // The 8-bit aim widened by repeating its byte, so 255 reaches the fixture's 65535 rather than 65280.
            if (offPanFine != kAbsent && offPan != kAbsent)   out[offPanFine] = out[offPan];
            if (offTiltFine != kAbsent && offTilt != kAbsent) out[offTiltFine] = out[offTilt];
        }
        // The white math runs on the LINEAR source: on curved values it would not mean what it says.
        uint8_t r = src[0];
        uint8_t g = src[1];
        uint8_t b = src[2];
        uint8_t w = 0;   // the white component; stays 0 when no white is synthesized
        // One gate for every synthesised emitter; None zeroes them, since the buffer is reused.
        if (whiteMode == WhiteMode::None) {
            if (offWhite != kAbsent)     out[offWhite] = 0;
            if (offWarmWhite != kAbsent) out[offWarmWhite] = 0;
            if (offYellow != kAbsent)    out[offYellow] = 0;
            if (offUV != kAbsent)        out[offUV] = 0;
        } else {
            w = r < g ? (r < b ? r : b) : (g < b ? g : b);  // min(r,g,b): the white component
            // Computed off the PRE-subtraction values, which only rebalance the RGB emitters.
            if (offWarmWhite != kAbsent) out[offWarmWhite] = lut[trim(w, whiteLevel)];
            // yellow ≈ min(R,G) (the shared red+green component).
            if (offYellow != kAbsent)    out[offYellow] = lut[r < g ? r : g];
            // Driven from the blue with no red or green to pair with, so it stays dark on warm colors.
            if (offUV != kAbsent) {
                const uint8_t rg = r > g ? r : g;
                out[offUV] = lut[b > rg ? static_cast<uint8_t>(b - rg) : 0];
            }
            // White last: it is the only emitter that rebalances RGB.
            if (offWhite != kAbsent) {
                if (whiteMode == WhiteMode::Accurate) { r -= w; g -= w; b -= w; }  // pull white out of RGB
                out[offWhite] = lut[trim(w, whiteLevel)];
            }
        }
        // The curve, applied ONCE: everything above this line is linear light.
        if (offRed != kAbsent)   out[offRed] = lut[trim(r, balRed)];
        if (offGreen != kAbsent) out[offGreen] = lut[trim(g, balGreen)];
        if (offBlue != kAbsent)  out[offBlue] = lut[trim(b, balBlue)];
        if constexpr (sizeof(T) == 1) { if (hasFine) applyFine(r, g, b, w, out); }
    }

    /// Hot path, 16-bit profiles only: each color channel with a fine role as its high and low byte from the same table, and a fine dimmer wide open.
    void applyFine(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t* out) const {
        if (offDimmerFine != kAbsent) out[offDimmerFine] = 255;   // wide open is 65535, not 65280
        // Trimmed here as the coarse bytes are, so a fine channel is balanced exactly as an 8-bit one.
        const uint8_t value[5] = {trim(r, balRed), trim(g, balGreen), trim(b, balBlue),
                                  trim(w, whiteLevel), trim(w, whiteLevel)};
        const uint8_t coarse[5] = {offRed, offGreen, offBlue, offWhite, offWarmWhite};
        const uint8_t fine[5] = {offRedFine, offGreenFine, offBlueFine, offWhiteFine, offWarmWhiteFine};
        for (uint8_t k = 0; k < 5; k++) {
            if (fine[k] == kAbsent) continue;
            // Without its table (an allocation that failed) the channel stays 8-bit: the coarse byte as written, the fine one 0.
            const uint16_t q = lut16 ? lut16[value[k]] : static_cast<uint16_t>(briLut[value[k]] << 8);
            if (coarse[k] != kAbsent) out[coarse[k]] = static_cast<uint8_t>(q >> 8);
            out[fine[k]] = static_cast<uint8_t>(q & 0xFF);
        }
    }
};

} // namespace mm
