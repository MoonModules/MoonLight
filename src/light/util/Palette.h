#pragma once

#include "core/util/color.h"   // RGB, scale8, hsvToRgb
#include "core/util/JsonSink.h" // paletteOptions emits the dropdown {name,colors} objects

#include <cstddef>
#include <cstdint>
#include <cstdio>    // snprintf: the seam copies the names it is handed

namespace mm {

/// @defgroup palette Color palettes
/// @{
/// Sixteen evenly-spaced entries a wheel index reads by interpolating between the two that bracket it.
///
/// @moreinfo
///
/// ## Gradients expand once
///
/// A gradient is a stop list in flash that expands into the sixteen entries on selection, off the hot path.
/// The per-light lookup is then a single blend.
///
/// ## Prior art
///
/// FastLED's gradient palettes, the convention WLED and MoonLight share, so the recognizable names and model are carried.
/// The implementation is our own; the gradient data is a public palette set, reformatted.
struct Palette {
    /// Entries a palette holds, evenly spaced across the wheel.
    static constexpr uint8_t kEntries = 16;
    RGB entry[kEntries] = {};

    /// Build the 16 entries from a gradient-stop list.
    void fromGradient(const uint8_t* stops, size_t count) {
        const size_t nStops = count / 4;
        if (nStops == 0) { for (auto& e : entry) e = {0, 0, 0}; return; }
        for (uint8_t i = 0; i < kEntries; i++) {
            /// The sample position for entry i, spread 0..255 across the 16 entries.
            const uint8_t pos = static_cast<uint8_t>((static_cast<uint16_t>(i) * 255) / (kEntries - 1));
            entry[i] = sampleGradient(stops, nStops, pos);
        }
    }

private:
    /// The color at `pos` (0..255) on the gradient: find the bracketing stops and lerp.
    static RGB sampleGradient(const uint8_t* stops, size_t nStops, uint8_t pos) {
        // Before the first stop (a gradient whose first stop sits above 0).
        if (pos <= stops[0]) return {stops[1], stops[2], stops[3]};
        /// Walk to the last stop whose position <= pos.
        size_t s = 0;
        while (s + 1 < nStops && stops[(s + 1) * 4] <= pos) s++;
        const uint8_t* lo = stops + s * 4;
        if (s + 1 >= nStops) return {lo[1], lo[2], lo[3]};   // at/after the last stop
        const uint8_t* hi = stops + (s + 1) * 4;
        const uint8_t p0 = lo[0], p1 = hi[0];
        if (p1 == p0) return {lo[1], lo[2], lo[3]};
        /// Fraction of the way from lo to hi, as 0..255 for scale8.
        const uint8_t frac = static_cast<uint8_t>((static_cast<uint16_t>(pos - p0) * 255) / (p1 - p0));
        return lerpRGB({lo[1], lo[2], lo[3]}, {hi[1], hi[2], hi[3]}, frac);
    }

public:
    /// Linear blend a→b by frac (0 = a, 255 = b).
    static RGB lerpRGB(const RGB& a, const RGB& b, uint8_t frac) {
        const uint8_t inv = static_cast<uint8_t>(255 - frac);
        return { static_cast<uint8_t>(scale8(a.r, inv) + scale8(b.r, frac)),
                 static_cast<uint8_t>(scale8(a.g, inv) + scale8(b.g, frac)),
                 static_cast<uint8_t>(scale8(a.b, inv) + scale8(b.b, frac)) };
    }
};

/// The per-light lookup: `index` is a 0..255 wheel position (wraps), mapped across the 16 entries;
inline RGB colorFromPalette(const Palette& p, uint8_t index, uint8_t brightness = 255) {
    /// Position across 16 entries: the high nibble selects the entry, the low byte the blend.
    const uint8_t hi = static_cast<uint8_t>(index >> 4);                 // 0..15, bracket start
    const uint8_t frac = static_cast<uint8_t>((index & 0x0F) * 17);      // 0..255 within the bracket
    const RGB& a = p.entry[hi];
    const RGB& b = p.entry[(hi + 1) & 0x0F];                             // wrap 15→0
    RGB c = Palette::lerpRGB(a, b, frac);
    if (brightness != 255) {
        c.r = scale8(c.r, brightness);
        c.g = scale8(c.g, brightness);
        c.b = scale8(c.b, brightness);
    }
    return c;
}

/// Cross-fade two colors: `amt`/255 of the way from `a` to `b` (amt 0 = a, 255 = b).
inline RGB blend(RGB a, RGB b, uint8_t amt) { return Palette::lerpRGB(a, b, amt); }

/// Dim a color toward black by `amt`/255: the per-frame fade that gives effects a decaying trail.
inline void fadeToBlackBy(RGB& c, uint8_t amt) {
    const uint8_t keep = static_cast<uint8_t>(255 - amt);
    c.r = scale8(c.r, keep);
    c.g = scale8(c.g, keep);
    c.b = scale8(c.b, keep);
}

// --- Built-in palettes -------------------------------------------------------------------------
/// Gradient-stop definitions in flash ({pos,R,G,B,…}).
namespace palettes {

/// Gradient definitions, verbatim from MoonLight's palettes.h, with names kept recognizable.
inline constexpr uint8_t kParty[]       = {0,85,0,171, 42,150,0,107, 85,201,0,42, 128,212,32,0, 170,191,98,0, 213,128,160,0, 255,85,212,0};   // FastLED party-colors stops
inline constexpr uint8_t kForest[]      = {0,0,100,0, 64,34,139,34, 128,0,128,0, 192,107,142,35, 255,0,100,0};
inline constexpr uint8_t kLava[]        = {0,0,0,0, 46,18,0,0, 96,113,0,0, 108,142,3,1, 119,175,17,1, 146,213,44,2, 174,255,82,4, 188,255,115,4, 202,255,156,4, 218,255,203,4, 234,255,255,4, 244,255,255,71, 255,255,255,255};
inline constexpr uint8_t kOceanBreeze[] = {0,1,6,7, 89,1,99,111, 153,144,209,255, 255,0,73,82};
inline constexpr uint8_t kFierceIce[]   = {0,0,0,0, 59,0,9,45, 119,0,38,255, 149,3,100,255, 180,23,199,255, 217,100,235,255, 255,255,255,255};
inline constexpr uint8_t kSunset[]      = {0,120,0,0, 22,179,22,0, 51,255,104,0, 85,167,22,18, 135,100,0,103, 198,16,0,130, 255,0,0,160};
inline constexpr uint8_t kSunset2[]     = {0,10,62,123, 36,56,130,103, 87,153,225,85, 100,199,217,68, 107,255,207,54, 115,247,152,57, 120,239,107,61, 128,247,152,57, 180,255,207,54, 223,255,227,48, 255,255,248,42};
inline constexpr uint8_t kOrangeTeal[]  = {0,0,150,92, 55,0,150,92, 200,255,72,0, 255,255,72,0};
inline constexpr uint8_t kAurora[]      = {0,1,5,45, 64,0,200,23, 128,0,255,0, 170,0,243,45, 200,0,135,7, 255,1,5,45};
inline constexpr uint8_t kAurora2[]     = {0,17,177,13, 64,121,242,5, 128,25,173,121, 192,250,77,127, 255,171,101,221};
inline constexpr uint8_t kAtlantica[]   = {0,0,28,112, 50,32,96,255, 100,0,243,45, 150,12,95,82, 200,25,190,95, 255,40,170,80};
inline constexpr uint8_t kAnalogous[]   = {0,3,0,255, 63,23,0,255, 127,67,0,255, 191,142,0,45, 255,255,0,0};
inline constexpr uint8_t kAprilNight[]  = {0,1,5,45, 10,1,5,45, 25,5,169,175, 40,1,5,45, 61,1,5,45, 76,45,175,31, 91,1,5,45, 112,1,5,45, 127,249,150,5, 143,1,5,45, 162,1,5,45, 178,255,92,0, 193,1,5,45, 214,1,5,45, 229,223,45,72, 244,1,5,45, 255,1,5,45};
inline constexpr uint8_t kAquaFlash[]   = {0,0,0,0, 66,57,227,233, 96,255,255,8, 124,255,255,255, 153,255,255,8, 188,57,227,233, 255,0,0,0};
inline constexpr uint8_t kAutumn[]      = {0,26,1,1, 51,67,4,1, 84,118,14,1, 104,137,152,52, 112,113,65,1, 122,133,149,59, 124,137,152,52, 135,113,65,1, 142,139,154,46, 163,113,13,1, 204,55,3,1, 249,17,1,1, 255,17,1,1};
inline constexpr uint8_t kBeech[]       = {0,255,252,214, 12,255,252,214, 22,255,252,214, 26,190,191,115, 28,137,141,52, 28,112,255,205, 50,51,246,214, 71,17,235,226, 93,2,193,199, 120,0,156,174, 133,1,101,115, 136,1,59,71, 136,7,131,170, 208,1,90,151, 255,0,56,133};
inline constexpr uint8_t kBlinkRed[]    = {0,1,1,1, 43,4,1,11, 76,10,1,3, 109,161,4,29, 127,255,86,123, 165,125,16,160, 204,35,13,223, 255,18,2,18};
inline constexpr uint8_t kC9[]          = {0,184,4,0, 60,184,4,0, 65,144,44,2, 125,144,44,2, 130,4,96,2, 190,4,96,2, 195,7,7,88, 255,7,7,88};
inline constexpr uint8_t kC9_2[]        = {0,6,126,2, 45,6,126,2, 45,4,30,114, 90,4,30,114, 90,255,5,0, 135,255,5,0, 135,196,57,2, 180,196,57,2, 180,137,85,2, 255,137,85,2};
inline constexpr uint8_t kC9New[]       = {0,255,5,0, 60,255,5,0, 60,196,57,2, 120,196,57,2, 120,6,126,2, 180,6,126,2, 180,4,30,114, 255,4,30,114};
inline constexpr uint8_t kCandy[]       = {0,229,227,1, 15,227,101,3, 142,40,1,80, 198,17,1,79, 255,0,0,45};
inline constexpr uint8_t kCandy2[]      = {0,39,33,34, 25,4,6,15, 48,49,29,22, 73,224,173,1, 89,177,35,5, 130,4,6,15, 163,255,114,6, 186,224,173,1, 211,39,33,34, 255,1,1,1};
inline constexpr uint8_t kColorfull[]   = {0,10,85,5, 25,29,109,18, 60,59,138,42, 93,83,99,52, 106,110,66,64, 109,123,49,65, 113,139,35,66, 116,192,117,98, 124,255,255,137, 168,100,180,155, 255,22,121,174};
inline constexpr uint8_t kDeparture[]   = {0,8,3,0, 42,23,7,0, 63,75,38,6, 84,169,99,38, 106,213,169,119, 116,255,255,255, 138,135,255,138, 148,22,255,24, 170,0,255,0, 191,0,136,0, 212,0,55,0, 255,0,55,0};
inline constexpr uint8_t kDrywet[]      = {0,47,30,2, 42,213,147,24, 84,103,219,52, 127,3,219,207, 170,1,48,214, 212,1,1,111, 255,1,7,33};
inline constexpr uint8_t kFairyReaf[]   = {0,184,1,128, 160,1,193,182, 219,153,227,190, 255,255,255,255};
inline constexpr uint8_t kGrintage[]    = {0,2,1,1, 53,18,1,0, 104,69,29,1, 153,167,135,10, 255,46,56,4};   // es_vintage_57
inline constexpr uint8_t kHult[]        = {0,247,176,247, 48,255,136,255, 89,220,29,226, 160,7,82,178, 216,1,124,109, 255,1,124,109};
inline constexpr uint8_t kHult64[]      = {0,1,124,109, 66,1,93,79, 104,52,65,1, 130,115,127,1, 150,52,65,1, 201,1,86,72, 239,0,55,45, 255,0,55,45};
inline constexpr uint8_t kJul[]         = {0,194,1,1, 94,1,29,18, 132,57,131,28, 255,113,1,1};
inline constexpr uint8_t kLandscape[]   = {0,0,0,0, 37,2,25,1, 76,15,115,5, 127,79,213,1, 128,126,211,47, 130,188,209,247, 153,144,182,205, 204,59,117,250, 255,1,37,192};
inline constexpr uint8_t kLightPink[]   = {0,19,2,39, 25,26,4,45, 51,33,6,52, 76,68,62,125, 102,118,187,240, 109,163,215,247, 114,217,244,255, 122,159,149,221, 149,113,78,188, 183,128,57,155, 255,146,40,123};   // Pink_Purple
inline constexpr uint8_t kLiteLight[]   = {0,0,0,0, 9,1,1,1, 40,5,5,6, 66,5,5,6, 101,10,1,12, 255,0,0,0};
inline constexpr uint8_t kMagenta[]     = {0,0,0,0, 42,0,0,45, 84,0,0,255, 127,42,0,255, 170,255,0,255, 212,255,55,255, 255,255,255,255};   // BlacK_Blue_Magenta_White
inline constexpr uint8_t kMagred[]      = {0,0,0,0, 63,42,0,45, 127,255,0,255, 191,255,0,45, 255,255,0,0};   // BlacK_Magenta_Red
inline constexpr uint8_t kOrangery[]    = {0,255,95,23, 30,255,82,0, 60,223,13,8, 90,144,44,2, 120,255,110,17, 150,255,69,0, 180,158,13,11, 210,241,82,17, 255,213,37,4};
inline constexpr uint8_t kPastel[]      = {0,0,0,255, 63,0,55,255, 127,0,255,255, 191,42,255,45, 255,255,255,0};   // Blue_Cyan_Yellow
inline constexpr uint8_t kPinkCandy[]   = {0,255,255,255, 45,7,12,255, 112,227,1,127, 112,227,1,127, 140,255,255,255, 155,227,1,127, 196,45,1,99, 255,255,255,255};
inline constexpr uint8_t kRedBlue[]     = {0,0,0,0, 42,42,0,0, 84,255,0,0, 127,255,0,45, 170,255,0,255, 212,255,55,45, 255,255,255,0};   // BlacK_Red_Magenta_Yellow
inline constexpr uint8_t kRedFlash[]    = {0,0,0,0, 99,227,1,1, 130,249,199,95, 155,227,1,1, 255,0,0,0};
inline constexpr uint8_t kRedReaf[]     = {0,3,13,43, 104,78,141,240, 188,255,0,0, 255,28,1,1};
inline constexpr uint8_t kRedShift[]    = {0,31,1,27, 45,34,1,16, 99,137,5,9, 132,213,128,10, 175,199,22,1, 201,199,9,6, 255,1,0,1};
inline constexpr uint8_t kRedTide[]     = {0,247,5,0, 28,255,67,1, 43,234,88,11, 58,234,176,51, 84,229,28,1, 114,113,12,1, 140,255,225,44, 168,113,12,1, 196,244,209,88, 216,255,28,1, 255,53,1,1};
inline constexpr uint8_t kRetroClown[]  = {0,227,101,3, 117,194,18,19, 255,92,8,192};
inline constexpr uint8_t kRewhi[]       = {0,188,135,1, 255,46,7,1};   // retro2_16
inline constexpr uint8_t kRivendell[]   = {0,1,14,5, 101,16,36,14, 165,56,68,30, 242,150,156,99, 255,150,156,99};
inline constexpr uint8_t kSakura[]      = {0,196,19,10, 65,255,69,45, 130,223,45,72, 195,255,82,103, 255,223,13,17};
inline constexpr uint8_t kSemiBlue[]    = {0,0,0,0, 12,1,1,3, 53,8,1,22, 80,4,6,89, 119,2,25,216, 145,7,10,99, 186,15,2,31, 233,2,1,5, 255,0,0,0};
inline constexpr uint8_t kSherbet[]     = {0,255,33,4, 43,255,68,25, 86,255,7,25, 127,255,82,103, 170,255,255,242, 209,42,255,22, 255,87,255,65};   // rainbowsherbet
inline constexpr uint8_t kSplash[]      = {0,126,11,255, 127,197,1,22, 175,210,157,172, 221,157,3,112, 255,157,3,112};   // es_pinksplash_08
inline constexpr uint8_t kTemperature[] = {0,1,27,105, 14,1,40,127, 28,1,70,168, 42,1,92,197, 56,1,119,221, 70,3,130,151, 84,23,156,149, 99,67,182,112, 113,121,201,52, 127,142,203,11, 141,224,223,1, 155,252,187,2, 170,247,147,1, 184,237,87,1, 198,229,43,1, 226,171,2,2, 240,80,3,3, 255,80,3,3};
inline constexpr uint8_t kTertiary[]    = {0,0,1,255, 63,3,68,45, 127,23,255,0, 191,100,68,1, 255,255,1,4};
inline constexpr uint8_t kTiamat[]      = {0,1,2,14, 33,2,5,35, 100,13,135,92, 120,43,255,193, 140,247,7,249, 160,193,17,208, 180,39,255,154, 200,4,213,236, 220,39,252,135, 240,193,213,253, 255,255,249,255};
inline constexpr uint8_t kToxyReaf[]    = {0,1,221,53, 255,73,3,178};
inline constexpr uint8_t kVintage[]     = {0,4,1,1, 51,16,0,1, 76,97,104,3, 101,255,131,19, 127,67,9,4, 153,16,0,1, 229,4,1,1, 255,4,1,1};   // es_vintage_01
inline constexpr uint8_t kYelbluHot[]   = {0,4,2,9, 58,16,0,47, 122,24,0,16, 158,144,9,1, 183,179,45,1, 219,220,114,2, 255,234,237,1};
inline constexpr uint8_t kYelblu[]      = {0,0,0,0, 42,0,0,45, 84,0,0,255, 127,42,0,255, 170,255,0,255, 212,255,55,255, 255,255,255,255};   // GMT_drywet-adjacent yelblu lineage
inline constexpr uint8_t kYelmag[]      = {0,4,1,70, 31,55,1,30, 63,255,4,7, 95,59,2,29, 127,11,3,50, 159,39,8,60, 191,112,19,40, 223,78,11,39, 255,29,8,59};   // rgi_15
inline constexpr uint8_t kYellowout[]   = {0,0,1,255, 63,0,55,255, 127,0,255,255, 191,42,255,45, 255,255,0,0};

// A built-in is a gradient ({stops,len}) or the special "rainbow" (generated via hsvToRgb).
/// A built-in palette: its name, its gradient, and the tags the picker filters on.
///
/// `tags` describes what the palette LOOKS LIKE, which is the question someone scrolling sixty entries is asking.
/// Warm or cold, one hue or many, calm or loud.
/// It is not a category system to be completed, and an untagged palette stays findable by name and swatch.
struct Builtin { const char* name; const uint8_t* stops; size_t len; bool rainbow;
                 const char* tags = ""; };

#define MM_PAL(name, arr) {name, arr, sizeof(arr), false}
/// The same, with tags: two macros rather than one with a default, the table running two to a line.
#define MM_PAL_T(name, arr, tags) {name, arr, sizeof(arr), false, tags}
inline constexpr Builtin kBuiltins[] = {
    {"Rainbow",       nullptr, 0, true, "🌈⚡️"},
    MM_PAL_T("Party", kParty, "🌈⚡️"),       MM_PAL_T("Lava", kLava, "🔥💫"),
    MM_PAL_T("Ocean", kOceanBreeze, "❄️💫"), MM_PAL_T("Forest", kForest, "❄️💫"),
    MM_PAL_T("Fierce Ice", kFierceIce, "❄️💫"),   MM_PAL_T("Sunset", kSunset, "🔥💫"),
    MM_PAL_T("Sunset 2", kSunset2, "🔥💫"),     MM_PAL_T("Orange & Teal", kOrangeTeal, "🌈💫"),
    MM_PAL_T("Aurora", kAurora, "❄️💫"),      MM_PAL_T("Aurora 2", kAurora2, "🌈💫"),
    MM_PAL_T("Atlantica", kAtlantica, "❄️💫"),   MM_PAL_T("Analogous", kAnalogous, "🌈💫"),
    MM_PAL_T("April Night", kAprilNight, "🌑💫"),  MM_PAL_T("Aqua Flash", kAquaFlash, "❄️💫"),
    MM_PAL_T("Autumn", kAutumn, "🔥💫"),      MM_PAL_T("Beech", kBeech, "🌑💫"),
    MM_PAL_T("Blink Red", kBlinkRed, "🔥💫"),    MM_PAL_T("C9", kC9, "🌈💫"),
    MM_PAL_T("C9 2", kC9_2, "💫"),        MM_PAL_T("C9 New", kC9New, "💫"),
    MM_PAL_T("Candy", kCandy, "💫"),       MM_PAL_T("Candy2", kCandy2, "💫"),
    MM_PAL_T("Colorfull", kColorfull, "💫"),   MM_PAL_T("Departure", kDeparture, "💫"),
    MM_PAL_T("Drywet", kDrywet, "💫"),      MM_PAL_T("Fairy Reaf", kFairyReaf, "💫"),
    MM_PAL_T("Grintage", kGrintage, "💫"),    MM_PAL_T("Hult", kHult, "💫"),
    MM_PAL_T("Hult 64", kHult64, "💫"),      MM_PAL_T("Jul", kJul, "💫"),
    MM_PAL_T("Landscape", kLandscape, "💫"),   MM_PAL_T("Light Pink", kLightPink, "💫"),
    MM_PAL_T("Lite Light", kLiteLight, "💫"),   MM_PAL_T("Magenta", kMagenta, "💫"),
    MM_PAL_T("Magred", kMagred, "💫"),      MM_PAL_T("Orangery", kOrangery, "💫"),
    MM_PAL_T("Pastel", kPastel, "💫"),      MM_PAL_T("Pink Candy", kPinkCandy, "💫"),
    MM_PAL_T("Red & Blue", kRedBlue, "💫"),     MM_PAL_T("Red Flash", kRedFlash, "💫"),
    MM_PAL_T("Red Reaf", kRedReaf, "💫"),     MM_PAL_T("Red Shift", kRedShift, "💫"),
    MM_PAL_T("Red Tide", kRedTide, "💫"),     MM_PAL_T("Retro Clown", kRetroClown, "💫"),
    MM_PAL_T("Rewhi", kRewhi, "💫"),       MM_PAL_T("Rivendell", kRivendell, "💫"),
    MM_PAL_T("Sakura", kSakura, "💫"),      MM_PAL_T("Semi Blue", kSemiBlue, "💫"),
    MM_PAL_T("Sherbet", kSherbet, "💫"),     MM_PAL_T("Splash", kSplash, "💫"),
    MM_PAL_T("Temperature", kTemperature, "💫"), MM_PAL_T("Tertiary", kTertiary, "💫"),
    MM_PAL_T("Tiamat", kTiamat, "💫"),      MM_PAL_T("Toxy Reaf", kToxyReaf, "💫"),
    MM_PAL_T("Vintage", kVintage, "💫"),     MM_PAL_T("Yelblu Hot", kYelbluHot, "💫"),
    MM_PAL_T("Yelblu", kYelblu, "💫"),      MM_PAL_T("Yelmag", kYelmag, "💫"),
    MM_PAL_T("Yellowout", kYellowout, "💫"),
};
#undef MM_PAL
inline constexpr uint8_t kCount = sizeof(kBuiltins) / sizeof(kBuiltins[0]);

}  // namespace palettes

/// The global active palette effects read: the AudioService::latestFrame() static-seam pattern.
class Palettes {
public:
    /// The palette effects read, which the drivers module sets.
    static const Palette* active() { return &active_; }

    /// Restore a previously captured palette verbatim. For tests that must hand back the global they disturbed.
    static void setActiveDirect(const Palette& p) { active_ = p; }

    /// Expand built-in `index` into the active palette, off the hot path, on selection.
    static void setActive(uint8_t index) {
        active_ = fromBuiltin(index);
    }

    /// Build the 16-entry palette for built-in `index` (rainbow generated via hsvToRgb, the rest expanded from their gradient stops).
    static Palette fromBuiltin(uint8_t index) {
        if (index >= palettes::kCount) index = 0;
        const auto& b = palettes::kBuiltins[index];
        Palette p;
        if (b.rainbow) {
            for (uint8_t i = 0; i < Palette::kEntries; i++)
                p.entry[i] = hsvToRgb(static_cast<uint8_t>((static_cast<uint16_t>(i) * 256) / Palette::kEntries), 255, 255);
        } else {
            p.fromGradient(b.stops, b.len);
        }
        return p;
    }

    // --- Representative color, for mapping an external color wheel onto a palette --- 

    /// Representative hue (0..359) of built-in `index`.
    static uint16_t representativeHue(uint8_t index) {
        uint16_t hue = 0, sat = 0;
        representativeHueSat(index, hue, sat);
        return hue;
    }

    /// Representative RGB color of built-in `index`: the palette's identity color, for any external
    static RGB representativeRgb(uint8_t index) {
        uint16_t hue = 0, sat = 0;
        representativeHueSat(index, hue, sat);
        // hue 0..359 → h 0..255 for hsvToRgb's 6-sector integer map (Palette.h convention.
        return hsvToRgb(static_cast<uint8_t>((hue * 256u) / 360u),
                        static_cast<uint8_t>(sat), 255);
    }

    /// RGB to the nearest palette: the one call every RGB-input consumer should reach for rather
    static uint8_t nearestForRgb(uint8_t r, uint8_t g, uint8_t b) {
        uint16_t hue = 0, sat = 0;
        rgbToHueSat(r, g, b, hue, sat);
        return nearestForHue(hue, static_cast<uint8_t>(sat));
    }

    /// The palette whose representative (hue, sat) is closest to the target, by 2D distance in (circular-hue, saturation) space.
    static uint8_t nearestForHue(uint16_t hue, uint8_t sat) {
        hue %= 360;   // fold any caller's value into 0..359, since a >=360 hue would make the
                      // circular-distance fold below underflow and the squared distance overflow int32
        uint8_t best = 0;
        uint32_t bestDist = 0xFFFFFFFF;
        for (uint8_t i = 0; i < palettes::kCount; i++) {
            uint16_t ph = 0, ps = 0;
            representativeHueSat(i, ph, ps);
            // Circular hue distance (0..180), scaled so hue and sat weigh comparably (hue 0..359 vs sat 0..255).
            uint16_t dh = static_cast<uint16_t>((hue > ph) ? (hue - ph) : (ph - hue));
            if (dh > 180) dh = static_cast<uint16_t>(360 - dh);
            const int32_t hueDelta = (static_cast<int32_t>(dh) * 255) / 180;   // 0..255
            const int32_t satDelta = static_cast<int32_t>(sat) - static_cast<int32_t>(ps);
            const uint32_t dist = static_cast<uint32_t>(hueDelta * hueDelta + satDelta * satDelta);
            if (dist < bestDist) { bestDist = dist; best = i; }
        }
        return best;
    }

    /// Compute a palette's representative hue (0..359) + saturation (0..255) from the average of its expanded RGB entries.
    static void representativeHueSat(uint8_t index, uint16_t& hueOut, uint16_t& satOut) {
        const Palette p = fromBuiltin(index);
        uint32_t rs = 0, gs = 0, bs = 0;
        for (uint8_t i = 0; i < Palette::kEntries; i++) {
            rs += p.entry[i].r; gs += p.entry[i].g; bs += p.entry[i].b;
        }
        const uint8_t r = static_cast<uint8_t>(rs / Palette::kEntries);
        const uint8_t g = static_cast<uint8_t>(gs / Palette::kEntries);
        const uint8_t b = static_cast<uint8_t>(bs / Palette::kEntries);
        rgbToHueSat(r, g, b, hueOut, satOut);
    }

private:
    /// Integer RGB → (hue 0..359, saturation 0..255).
    static void rgbToHueSat(uint8_t r, uint8_t g, uint8_t b, uint16_t& hue, uint16_t& sat) {
        const uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const uint8_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        const uint8_t delta = static_cast<uint8_t>(mx - mn);
        if (delta == 0 || mx == 0) { hue = 0; sat = 0; return; }
        sat = static_cast<uint16_t>((static_cast<uint32_t>(delta) * 255) / mx);
        int32_t h;
        if (mx == r)      h = 60 * (static_cast<int32_t>(g) - b) / delta;
        else if (mx == g) h = 120 + 60 * (static_cast<int32_t>(b) - r) / delta;
        else              h = 240 + 60 * (static_cast<int32_t>(r) - g) / delta;
        if (h < 0) h += 360;
        hue = static_cast<uint16_t>(h % 360);
    }

    /// Default to a full rainbow (index 0).
    static inline Palette active_ = fromBuiltin(0);
};

/// How many SCRIPTED palettes the picker offers, and their names. Set by Drivers from the catalog plus whatever `.mlp` files the device carries, because Palette.h knows nothing about the filesystem and must not learn.
/// This is the same one-pointer seam `Palettes::active()` is.
///
/// They come AFTER the built-ins, whose indices the WLED API and a knob stepping through the list rely on, so adding a script renumbers only the scripted tail.
struct LivePalettes {
    /// How many scripted palettes the picker offers.
    static uint8_t count() { return count_; }
    /// One scripted palette's name, or empty when the index names none.
    static const char* nameAt(uint8_t i) { return (i < count_ && names_ && names_[i]) ? names_[i] : ""; }
    /// What the script declared about itself, for the picker's emoji filter. Empty when the device carries a `.mlp` the catalog does not know, which is a script the user wrote.
    static const char* tagsAt(uint8_t i) { return (i < count_ && tags_ && tags_[i]) ? tags_[i] : ""; }

    /// REFERENCES the caller's arrays; it does not copy them.
    static void set(const char* const* names, const char* const* tags, uint8_t n) {
        names_ = names;
        tags_ = tags;
        count_ = names ? (n > kMax ? kMax : n) : 0;
    }
    /// Detach, for a publisher whose storage is about to go away.
    static void clear(const char* const* names = nullptr) {
        if (names && names_ != names) return;      // someone else owns the seam now: leave it
        names_ = nullptr;
        tags_ = nullptr;
        count_ = 0;
    }

    /// Scripted palettes sort LAST, after the built-ins, and the position is the whole point.
    static bool isLive(uint8_t pickerIndex) { return pickerIndex >= palettes::kCount; }
    /// The index into the underlying array (scripted or built-in) behind a picker index.
    static uint8_t sourceIndex(uint8_t pickerIndex) {
        return isLive(pickerIndex) ? static_cast<uint8_t>(pickerIndex - palettes::kCount)
                                   : pickerIndex;
    }

    /// Case-insensitive name order, for sorting the scripted names among themselves.
    static int cmpName(const char* a, const char* b) {
        for (; *a && *b; ++a, ++b) {
            const char la = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
            const char lb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
            if (la != lb) return la < lb ? -1 : 1;
        }
        return *a ? 1 : (*b ? -1 : 0);
    }

    /// The most scripted palettes a device carries, which keeps the picker readable.
    static constexpr uint8_t kMax = 16;
private:
    static inline const char* const* names_ = nullptr;
    static inline const char* const* tags_ = nullptr;
    static inline uint8_t count_ = 0;
};

/// Emit a palette control's options, or one option's name when the sink asks for it.
inline void paletteOptions(JsonSink& sink) {
    /// A NAME REQUEST rather than an options dump.
    const uint8_t live = LivePalettes::count();
    if (sink.nameIndex() >= 0) {
        const uint8_t i = static_cast<uint8_t>(sink.nameIndex());
        const uint8_t src = LivePalettes::sourceIndex(i);
        // A RAW name here, not a JSON string: this path fills a char buffer for the display strip (ControlModule::paletteNameAt), so quoting it would put the quotes on the strip.
        if (LivePalettes::isLive(i)) { sink.append(LivePalettes::nameAt(src)); return; }
        if (src < palettes::kCount) sink.append(palettes::kBuiltins[src].name);
        return;
    }
    // The BUILT-INS first, keeping the indices every saved selection and the WLED API depend on.
    for (uint8_t i = 0; i < palettes::kCount; i++) {
        const Palette p = Palettes::fromBuiltin(i);
        sink.appendf("%s{\"name\":\"%s\",\"tags\":\"%s\",\"colors\":\"", i > 0 ? "," : "",
                     palettes::kBuiltins[i].name, palettes::kBuiltins[i].tags);
        for (uint8_t e = 0; e < Palette::kEntries; e++)
            sink.appendf("%s%02x%02x%02x", e > 0 ? " " : "", p.entry[e].r, p.entry[e].g, p.entry[e].b);
        sink.append("\"}");
    }
    // Then the scripted tail, each marked with 🎨 so the list says at a glance which entries are code rather than a fixed gradient.
    for (uint8_t i = 0; i < live; i++) {
        const Palette& p = *Palettes::active();
        // The escaping writer for the NAME: a script names its palette, and a quote in that name would end the string early and cost the UI the whole control frame.
        sink.append(",{\"name\":");
        sink.writeJsonString(LivePalettes::nameAt(i));
        sink.appendf(",\"live\":true,\"tags\":\"%s\",\"colors\":\"", LivePalettes::tagsAt(i));
        for (uint8_t e = 0; e < Palette::kEntries; e++)
            sink.appendf("%s%02x%02x%02x", e > 0 ? " " : "", p.entry[e].r, p.entry[e].g, p.entry[e].b);
        sink.append("\"}");
    }
}

/// Emit the built-in palette names as a bare JSON string array, the element list WLED's /json
inline void paletteNames(JsonSink& sink) {
    for (uint8_t i = 0; i < palettes::kCount; i++)
        sink.appendf("%s\"%s\"", i > 0 ? "," : "", palettes::kBuiltins[i].name);
    // The scripted tail too, in the SAME order the picker and `seg[0].pal` use.
    for (uint8_t i = 0; i < LivePalettes::count(); i++) {
        // The escaping writer: a user-supplied name with a quote would end the string early.
        sink.append(",");
        sink.writeJsonString(LivePalettes::nameAt(i));
    }
}

/// How many entries paletteNames() writes.
inline uint8_t paletteCount() {
    return static_cast<uint8_t>(palettes::kCount + LivePalettes::count());
}

/// @}

}  // namespace mm
