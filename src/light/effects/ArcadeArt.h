#pragma once

/// The arcade sprite art, one home for every drawing the sprite effects share: MoonMan and the ghosts (moonart), the invaders and the cannon (invart).

#include "light/util/Palette.h"   // colorFromPalette, blend: the palettes recolor the cast with the show

#include <cstddef>
#include <cstdint>

namespace mm {

namespace moonart {

/// The sprites index these and the effect fills them, so a ghost is one drawing in four colors.
enum : uint8_t { kClear = 0, kBody = 1, kEye = 2, kPupil = 3, kDark = 4 };
inline constexpr uint8_t kPaletteCount = 5;

/// MoonMan is yellow, the one color in the cast the palette does not drive.
inline void moonManPalette(RGB (&pal)[kPaletteCount]) {
    pal[kClear] = RGB{0, 0, 0};
    pal[kBody]  = RGB{255, 214, 0};
    pal[kEye]   = RGB{0, 0, 0};
    pal[kPupil] = RGB{0, 0, 0};
    pal[kDark]  = RGB{140, 118, 0};
}

/// A ghost's palette from one entry of the active one, keeping the white eyes that make a colored blob read as a ghost.
inline void ghostPalette(RGB (&pal)[kPaletteCount], uint8_t entry) {
    const RGB body = colorFromPalette(*Palettes::active(), entry);
    pal[kClear] = RGB{0, 0, 0};
    pal[kBody]  = body;
    pal[kEye]   = RGB{255, 255, 255};
    pal[kPupil] = RGB{20, 20, 120};
    pal[kDark]  = blend(body, RGB{0, 0, 0}, 140);
}

/// MoonMan, 11x11 in 4 chomp frames, facing right since flipX serves leftward travel: a crescent moon with one eye (kPupil), chomping as its bite eases between thin and full.
inline constexpr uint8_t W = 11, H = 11, F = 4;
inline constexpr uint8_t kMoonMan[] = {
    // frame 0: mouth wide: the thinnest crescent
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,0,0,0,0,0,
    1,1,1,1,1,0,0,0,0,0,0,
    1,1,3,1,0,0,0,0,0,0,0,
    1,1,1,1,0,0,0,0,0,0,0,
    1,1,1,1,0,0,0,0,0,0,0,
    1,1,1,1,1,0,0,0,0,0,0,
    0,1,1,1,1,1,0,0,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,0,0,0,
    // frame 1: mouth half
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,
    1,1,3,1,1,1,0,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,
    0,1,1,1,1,1,1,0,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,0,0,0,
    // frame 2: mouth closed: almost the full moon
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,0,0,
    1,1,3,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,0,0,0,
    // frame 3: mouth half again (the return stroke)
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,
    1,1,3,1,1,1,0,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,
    0,1,1,1,1,1,1,0,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,0,0,0,
};
static_assert(sizeof(kMoonMan) == static_cast<size_t>(W) * H * F, "moonman: 4 frames of 11x11");

/// A ghost, 11x11 in 2 skirt frames, which is the arcade original's whole walk animation.
inline constexpr uint8_t GW = 11, GH = 11, GF = 2;
inline constexpr uint8_t kGhost[] = {
    // frame 0: skirt down-up-down
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    0,1,2,2,1,1,2,2,1,1,0,
    1,1,2,2,2,1,2,2,2,1,1,
    1,1,2,3,3,1,2,3,3,1,1,
    1,1,2,3,3,1,2,3,3,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,0,1,1,0,1,1,0,1,1,0,
    // frame 1: skirt up-down-up
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    0,1,2,2,1,1,2,2,1,1,0,
    1,1,2,2,2,1,2,2,2,1,1,
    1,1,2,3,3,1,2,3,3,1,1,
    1,1,2,3,3,1,2,3,3,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    0,1,1,0,1,1,0,1,1,0,1,
};
static_assert(sizeof(kGhost) == static_cast<size_t>(GW) * GH * GF, "ghost: 2 frames of 11x11");

/// The ghost at the invaders' 8x8, so a formation of them keeps the march's spacing: 2 skirt frames.
inline constexpr uint8_t SW = 8, SH = 8, SF = 2;
inline constexpr uint8_t kGhostSmall[] = {
    // frame 0: skirt down-up-down
    0,0,1,1,1,1,0,0,
    0,1,1,1,1,1,1,0,
    1,2,2,1,1,2,2,1,
    1,2,3,1,1,2,3,1,
    1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,
    1,0,1,1,0,1,1,0,
    // frame 1: skirt up-down-up
    0,0,1,1,1,1,0,0,
    0,1,1,1,1,1,1,0,
    1,2,2,1,1,2,2,1,
    1,2,3,1,1,2,3,1,
    1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,
    0,1,1,0,1,1,0,1,
};
static_assert(sizeof(kGhostSmall) == static_cast<size_t>(SW) * SH * SF, "small ghost: 2 frames of 8x8");

}  // namespace moonart

namespace invart {

/// The sprites index these and the effect fills them, so one drawing serves every rank in a color.
enum : uint8_t { kClear = 0, kBody = 1, kDark = 2, kEye = 3 };
inline constexpr uint8_t kPaletteCount = 4;

/// An invader's palette from one entry of the active one, with dark eyes.
inline void invaderPalette(RGB (&pal)[kPaletteCount], uint8_t entry) {
    const RGB body = colorFromPalette(*Palettes::active(), entry);
    pal[kClear] = RGB{0, 0, 0};
    pal[kBody]  = body;
    pal[kDark]  = blend(body, RGB{0, 0, 0}, 140);
    pal[kEye]   = RGB{10, 10, 14};
}

/// The squid, top rank: 8x8, and the arcade's own two frames are what the march looks like.
inline constexpr uint8_t W = 8, H = 8, F = 2;
inline constexpr uint8_t kSquid[] = {
    // frame 0: tentacles out
    0,0,0,1,1,0,0,0,
    0,0,1,1,1,1,0,0,
    0,1,1,1,1,1,1,0,
    1,1,3,1,1,3,1,1,
    1,1,1,1,1,1,1,1,
    0,0,1,0,0,1,0,0,
    0,1,0,1,1,0,1,0,
    1,0,1,0,0,1,0,1,
    // frame 1: tentacles tucked
    0,0,0,1,1,0,0,0,
    0,0,1,1,1,1,0,0,
    0,1,1,1,1,1,1,0,
    1,1,3,1,1,3,1,1,
    1,1,1,1,1,1,1,1,
    0,1,0,1,1,0,1,0,
    1,0,0,0,0,0,0,1,
    0,1,0,0,0,0,1,0,
};
static_assert(sizeof(kSquid) == static_cast<size_t>(W) * H * F, "squid: 2 frames of 8x8");

/// The crab (middle ranks): 11x8, the widest of the three, arms up and arms down.
inline constexpr uint8_t CW = 11, CH = 8, CF = 2;
inline constexpr uint8_t kCrab[] = {
    // frame 0: arms up
    0,0,1,0,0,0,0,0,1,0,0,
    0,0,0,1,0,0,0,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,3,1,1,1,3,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,
    1,0,1,1,1,1,1,1,1,0,1,
    1,0,1,0,0,0,0,0,1,0,1,
    0,0,0,1,1,0,1,1,0,0,0,
    // frame 1: arms down
    0,0,1,0,0,0,0,0,1,0,0,
    1,0,0,1,0,0,0,1,0,0,1,
    1,0,1,1,1,1,1,1,1,0,1,
    1,1,1,3,1,1,1,3,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    0,1,1,1,1,1,1,1,1,1,0,
    0,0,1,0,0,0,0,0,1,0,0,
    0,1,0,0,0,0,0,0,0,1,0,
};
static_assert(sizeof(kCrab) == static_cast<size_t>(CW) * CH * CF, "crab: 2 frames of 11x8");

/// The octopus (bottom ranks): 12x8, the squat one worth the fewest points and seen the longest.
inline constexpr uint8_t OW = 12, OH = 8, OF = 2;
inline constexpr uint8_t kOcto[] = {
    // frame 0: legs apart
    0,0,0,0,1,1,1,1,0,0,0,0,
    0,1,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,3,3,1,1,3,3,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,0,1,1,0,0,1,1,0,0,0,
    0,0,1,1,0,1,1,0,1,1,0,0,
    1,1,0,0,0,0,0,0,0,0,1,1,
    // frame 1: legs together
    0,0,0,0,1,1,1,1,0,0,0,0,
    0,1,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,3,3,1,1,3,3,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,1,1,1,0,0,1,1,1,0,0,
    0,1,1,0,0,1,1,0,0,1,1,0,
    0,0,1,1,0,0,0,0,1,1,0,0,
};
static_assert(sizeof(kOcto) == static_cast<size_t>(OW) * OH * OF, "octopus: 2 frames of 12x8");

/// The cannon: 13x8, one frame. It does not animate; it moves.
inline constexpr uint8_t GW = 13, GH = 8, GF = 1;
inline constexpr uint8_t kCannon[] = {
    0,0,0,0,0,0,1,0,0,0,0,0,0,
    0,0,0,0,0,1,1,1,0,0,0,0,0,
    0,0,0,0,0,1,1,1,0,0,0,0,0,
    0,1,1,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
};
static_assert(sizeof(kCannon) == static_cast<size_t>(GW) * GH * GF, "cannon: one 13x8 frame");

}  // namespace invart

}  // namespace mm
