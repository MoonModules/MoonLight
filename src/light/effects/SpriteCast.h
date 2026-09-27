#pragma once
// Author: MoonLight original
/// @defgroup SpriteCast The shared sprite cast
/// @{
/// Every pixel-art character the effects share, behind one draw call.
///
/// @moreinfo
///
/// Four effects own their art in their own namespaces, each a plain header of pixels.
/// Two effects draw any of them chosen at runtime: the fountain throws a random species, and Pong can use one as its ball.
/// That selection is the same in both, so it lives here, while the pixels stay owned by the effect that introduced them.

#include "core/util/math16.h"
#include "light/powerfunctions/draw.h"
#include "light/effects/FishTankEffect.h"        // fishart:: the reef fish, slim fish, tiny fish
#include "light/effects/FlyingToastersEffect.h"  // toasterart:: the toaster and its toast
#include "light/effects/PacmanEffect.h"          // pacart:: Pacman and the ghosts
#include "light/effects/SpaceInvadersEffect.h"   // invart:: the squid, crab and octopus

namespace mm {
namespace spritecast {

/// The cast, in the order a kind byte selects, the three fish leading since they read best.
enum : uint8_t { kFish = 0, kSlim, kTiny, kPac, kGhost, kToaster, kToast,
                 kSquid, kCrab, kOcto, kKindCount };

/// A fish's palette, built from the active one so a borrowed sprite recolors with the show.
inline void fishPalette(RGB (&pal)[fishart::kPaletteCount], uint8_t entry) {
    const RGB body = colorFromPalette(*Palettes::active(), entry);
    pal[fishart::kClear] = RGB{0, 0, 0};
    pal[fishart::kBody]  = body;
    pal[fishart::kDark]  = blend(body, RGB{0, 0, 0}, 150);
    pal[fishart::kLight] = blend(body, RGB{255, 255, 255}, 120);
    pal[fishart::kFin]   = blend(body, RGB{255, 255, 255}, 60);
    pal[fishart::kEye]   = RGB{20, 20, 24};
    pal[fishart::kBand]  = blend(body, RGB{255, 255, 255}, 200);
}

/// Pacman is yellow, the one color in this cast that is not negotiable.
inline void pacPalette(RGB (&pal)[pacart::kPaletteCount]) {
    pal[pacart::kClear] = RGB{0, 0, 0};
    pal[pacart::kBody]  = RGB{255, 214, 0};
    pal[pacart::kEye]   = RGB{0, 0, 0};
    pal[pacart::kPupil] = RGB{0, 0, 0};
    pal[pacart::kDark]  = RGB{140, 118, 0};
}

/// A ghost's palette, keeping the white eyes that make a colored blob read as a ghost.
inline void ghostPalette(RGB (&pal)[pacart::kPaletteCount], uint8_t entry) {
    const RGB body = colorFromPalette(*Palettes::active(), entry);
    pal[pacart::kClear] = RGB{0, 0, 0};
    pal[pacart::kBody]  = body;
    pal[pacart::kEye]   = RGB{255, 255, 255};
    pal[pacart::kPupil] = RGB{20, 20, 120};
    pal[pacart::kDark]  = blend(body, RGB{0, 0, 0}, 140);
}

/// The invaders' own palette layout, filled from the active palette like the rest.
inline void invaderPalette(RGB (&pal)[invart::kPaletteCount], uint8_t entry) {
    const RGB body = colorFromPalette(*Palettes::active(), entry);
    pal[invart::kClear] = RGB{0, 0, 0};
    pal[invart::kBody]  = body;
    pal[invart::kDark]  = blend(body, RGB{0, 0, 0}, 140);
    pal[invart::kEye]   = RGB{10, 10, 14};
}

/// Center the sprite and draw it: art hanging below and right of the physics reads as lag.
inline void blit(const draw::Canvas& cv, const draw::sprites::Sprite& s, uint8_t frame,
                 lengthType px, lengthType py, uint8_t sc, bool flip) {
    const lengthType ox = static_cast<lengthType>(px - (s.w * sc) / 2);
    const lengthType oy = static_cast<lengthType>(py - (s.h * sc) / 2);
    draw::sprite(cv, s, frame, ox, oy, sc, flip);
}

/// Draw one cast member centered, colored by `entry`, animated by `beat` and faced by `flip`.
inline void draw(const draw::Canvas& cv, uint8_t kind, uint8_t entry,
                 lengthType px, lengthType py, uint8_t sc, bool flip, uint8_t beat) {
    switch (kind) {
        case kFish: {
            RGB pal[fishart::kPaletteCount];
            fishPalette(pal, entry);
            blit(cv, {fishart::kFish, pal, fishart::W, fishart::H, fishart::F,
                      fishart::kPaletteCount}, beat % fishart::F, px, py, sc, flip);
            break;
        }
        case kSlim: {
            RGB pal[fishart::kPaletteCount];
            fishPalette(pal, entry);
            blit(cv, {fishart::kSlim, pal, fishart::SW, fishart::SH, fishart::SF,
                      fishart::kPaletteCount}, beat % fishart::SF, px, py, sc, flip);
            break;
        }
        case kTiny: {
            RGB pal[fishart::kPaletteCount];
            fishPalette(pal, entry);
            blit(cv, {fishart::kTiny, pal, fishart::TW, fishart::TH, 1,
                      fishart::kPaletteCount}, 0, px, py, sc, flip);
            break;
        }
        case kPac: {
            RGB pal[pacart::kPaletteCount];
            pacPalette(pal);
            blit(cv, {pacart::kPac, pal, pacart::W, pacart::H, pacart::F,
                      pacart::kPaletteCount}, beat % pacart::F, px, py, sc, flip);
            break;
        }
        case kGhost: {
            RGB pal[pacart::kPaletteCount];
            ghostPalette(pal, entry);
            blit(cv, {pacart::kGhost, pal, pacart::GW, pacart::GH, pacart::GF,
                      pacart::kPaletteCount}, beat % pacart::GF, px, py, sc, flip);
            break;
        }
        case kToaster:
            // The toaster art exports a ready-made Sprite, so this is a straight reuse.
            blit(cv, toasterart::kToasterSprite, beat % toasterart::F, px, py, sc, flip);
            break;
        case kToast:
            blit(cv, toasterart::kToastSprite, 0, px, py, sc, flip);
            break;
        case kSquid: {
            RGB pal[invart::kPaletteCount];
            invaderPalette(pal, entry);
            blit(cv, {invart::kSquid, pal, invart::W, invart::H, invart::F,
                      invart::kPaletteCount}, beat % invart::F, px, py, sc, flip);
            break;
        }
        case kCrab: {
            RGB pal[invart::kPaletteCount];
            invaderPalette(pal, entry);
            blit(cv, {invart::kCrab, pal, invart::CW, invart::CH, invart::CF,
                      invart::kPaletteCount}, beat % invart::CF, px, py, sc, flip);
            break;
        }
        default: {
            RGB pal[invart::kPaletteCount];
            invaderPalette(pal, entry);
            blit(cv, {invart::kOcto, pal, invart::OW, invart::OH, invart::OF,
                      invart::kPaletteCount}, beat % invart::OF, px, py, sc, flip);
            break;
        }
    }
}

/// The widest the cast gets in art pixels, which a caller leaves room for.
inline constexpr uint8_t kMaxW = 16;
/// And the tallest.
inline constexpr uint8_t kMaxH = 11;

}  // namespace spritecast
/// @}
}  // namespace mm
