#pragma once
/// Luna and Momo as sprites: Luna presents MoonLight, Momo is the MoonModules logo. They carry their own colors, as the toaster does, so the palette never recolors them.

#include "light/powerfunctions/draw.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace mm {
namespace mascotart {

/// The shared palette: 0 is transparent, then the hood, the face, the moon, the glasses and hands, pink, black and the legs.
inline constexpr RGB kPalette[] = {
    {0, 0, 0},         // 0: transparent key (never read)
    {30, 40, 100},     // 1: hood navy
    {8, 10, 28},       // 2: face dark
    {110, 215, 240},   // 3: moon cyan
    {200, 240, 250},   // 4: moon light
    {240, 250, 255},   // 5: glasses, hands, mustache
    {255, 70, 150},    // 6: pink: lips, palms, heels
    {40, 40, 52},      // 7: skirt, boots
    {80, 95, 130},     // 8: legs
};
inline constexpr uint8_t kPaletteCount = sizeof(kPalette) / sizeof(kPalette[0]);

/// Both stand 24x24 in one frame, peace signs up, the crescent moon behind them.
inline constexpr uint8_t W = 24, H = 24;

/// The hood, the glasses and the peace signs, which both share.
inline constexpr uint8_t kHead[] = {
    0,0,0,0,0,0,0,0,0,3,0,0,1,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,3,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,3,3,0,1,1,1,1,1,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,3,3,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,
    0,0,0,0,0,3,3,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,
    0,0,5,0,5,3,3,0,1,1,1,1,1,1,1,1,1,0,0,0,5,0,5,0,
    0,0,5,0,5,3,3,1,2,5,5,5,2,5,5,5,2,1,0,0,5,0,5,0,
    0,0,5,5,5,3,3,1,2,4,5,4,2,4,5,4,2,1,0,0,5,5,5,0,
    0,0,5,6,5,3,3,1,2,2,2,2,2,2,2,2,2,1,0,0,5,6,5,0,
};

/// Luna's pink lips.
inline constexpr uint8_t kLunaFace[] = {
    0,0,0,5,0,3,3,1,2,2,2,6,6,6,2,2,2,1,0,0,0,5,0,0,
    0,0,0,1,0,0,3,1,1,2,2,2,2,2,2,2,1,1,0,0,0,1,0,0,
    0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,
};

/// Momo's white mustache and goatee.
inline constexpr uint8_t kMomoFace[] = {
    0,0,0,5,0,3,3,1,2,5,5,5,5,5,5,5,2,1,0,0,0,5,0,0,
    0,0,0,1,0,0,3,1,1,2,2,2,5,2,2,2,1,1,0,0,0,1,0,0,
    0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,
};

/// The crescent passing under the hoodie, which both share.
inline constexpr uint8_t kWaist[] = {
    0,0,0,0,0,0,3,1,1,1,1,1,1,1,1,1,1,1,3,0,0,0,0,0,
    0,0,0,0,0,0,3,4,4,4,4,4,4,4,4,4,4,4,3,0,0,0,0,0,
    0,0,0,0,0,0,0,3,4,4,4,4,4,4,4,4,4,3,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,3,3,3,3,3,3,3,0,0,0,0,0,0,0,0,
};

/// Luna's pleated skirt and pink heels.
inline constexpr uint8_t kLunaLegs[] = {
    0,0,0,0,0,0,0,0,7,7,7,7,7,7,7,7,7,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,7,8,7,8,7,8,7,8,7,8,7,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,8,0,8,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,8,0,8,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,8,0,8,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,8,0,8,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,6,6,0,6,6,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,6,6,6,0,6,6,6,0,0,0,0,0,0,0,0,
};

/// Momo's jointed legs and boots.
inline constexpr uint8_t kMomoLegs[] = {
    0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,0,0,0,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,7,0,0,0,7,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,0,0,0,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,7,0,0,0,7,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,7,7,7,0,7,7,7,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,7,7,7,7,0,7,7,7,7,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,5,5,5,5,0,5,5,5,5,0,0,0,0,0,0,0,
};

/// The parts' rows, top to bottom, as one frame.
template <size_t... N>
constexpr std::array<uint8_t, (N + ...)> stack(const uint8_t (&... parts)[N]) {
    std::array<uint8_t, (N + ...)> out{};
    size_t at = 0;
    ((std::copy(parts, parts + N, out.begin() + at), at += N), ...);
    return out;
}

inline constexpr auto kLuna = stack(kHead, kLunaFace, kWaist, kLunaLegs);
inline constexpr auto kMomo = stack(kHead, kMomoFace, kWaist, kMomoLegs);
static_assert(kLuna.size() == static_cast<size_t>(W) * H && kMomo.size() == kLuna.size(), "one 24x24 frame each");

inline constexpr draw::sprites::Sprite kLunaSprite{kLuna.data(), kPalette, W, H, 1, kPaletteCount};
inline constexpr draw::sprites::Sprite kMomoSprite{kMomo.data(), kPalette, W, H, 1, kPaletteCount};

}  // namespace mascotart
}  // namespace mm
