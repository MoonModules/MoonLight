#pragma once
// Author: MoonLight original

#include "core/util/math16.h"      // BeatPhase: the chomp / shuffle clock
#include "core/util/math8.h"       // Random8: fixed-seed spawn variation, golden-reproducible
#include "light/effects/EffectBase.h"
#include "light/powerfunctions/particles.h"  // Pool: the movable-things kernel the cast rides

namespace mm {

namespace pacart {

/// The sprites index these and the effect fills them, so a ghost is one drawing in four colors.
enum : uint8_t { kClear = 0, kBody = 1, kEye = 2, kPupil = 3, kDark = 4 };
inline constexpr uint8_t kPaletteCount = 5;

// Pacman, 11x11 in 4 chomp frames, facing right since flipX serves leftward travel.
inline constexpr uint8_t W = 11, H = 11, F = 4;
inline constexpr uint8_t kPac[] = {
    // frame 0: mouth wide
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,
    1,1,1,1,1,0,0,0,0,0,0,
    1,1,1,1,1,1,0,0,0,0,0,
    1,1,1,1,1,1,1,0,0,0,0,
    0,1,1,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,0,0,0,
    // frame 1: mouth half
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,0,0,
    1,1,1,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,0,0,0,0,
    1,1,1,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,0,0,0,
    // frame 2: mouth closed (a full disc)
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    0,1,1,1,1,1,1,1,1,1,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,0,0,0,
    // frame 3: mouth half again (the return stroke)
    0,0,0,1,1,1,1,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,0,0,
    1,1,1,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,0,0,0,0,
    1,1,1,1,1,1,1,1,0,0,0,
    1,1,1,1,1,1,1,1,1,0,0,
    0,1,1,1,1,1,1,1,1,1,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,0,0,1,1,1,1,1,0,0,0,
};
static_assert(sizeof(kPac) == static_cast<size_t>(W) * H * F, "pacman: 4 frames of 11x11");

// A ghost, 11x11 in 2 skirt frames, which is the arcade original's whole walk animation.
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

}  // namespace pacart

/// Effect: Pacman and the ghosts cross the wall, chomping.
/// @card PacmanEffect.gif
///
/// The characters move independently and do not yet notice each other.
/// Inspired by Namco's Pac-Man, with the pixel art drawn fresh at a size of its own.
///
/// @moreinfo
///
/// ## The construction, and what comes next
///
/// Movement is a `particles::Pool` entry per character, and appearance is `draw::sprite`.
/// The chomp and the ghosts' shuffle share one BeatPhase, offset so the cast never pulses together.
/// The maze, the pellets and the chase are the next iteration, built on these shapes and lanes.
class PacmanEffect : public EffectBase {
public:
    /// The pool size: both count controls at their maxima.
    static constexpr uint8_t kPool = 12;

    /// Catalog tags: the audio glyph applies when `audioReactive` is set.
    const char* tags() const override { return "💫🎶✨👾"; }
    /// A wall needs a width and a height, so it is a 2D effect.
    Dim dimensions() const override { return Dim::D2; }

    /// How many Pacmen cross the wall.
    uint8_t pacmen = 1;
    /// How many ghosts cross it, the arcade cast being four.
    uint8_t ghosts = 4;
    /// How fast the cast travels, in body lengths a second.
    uint8_t speed  = 96;
    /// Pixels per art pixel, where 0 scales with the grid.
    uint8_t spriteSize = 0;
    /// Move to the music, each sprite on its own band, holding still in silence.
    bool audioReactive = false;

    /// Publish both counts, the speed, the sprite scale and the audio switch.
    void defineControls() override {
        controls_.addControl("pacmen", pacmen, 0, 4);
        controls_.addControl("ghosts", ghosts, 0, 8);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("spriteSize", spriteSize, 0, 12);
        controls_.addControl("audioReactive", audioReactive);
    }

    /// Size the pool's storage, wire the view over it, and put the cast on the wall.
    void prepare() override {
        const bool ok = x_.resize(kPool) && y_.resize(kPool) && vx_.resize(kPool) &&
                        vy_.resize(kPool) && ttl_.resize(kPool) && kind_.resize(kPool);
        if (!ok) { pool_ = particles::Pool{}; return; }
        pool_ = particles::Pool{};
        pool_.x = &x_[0]; pool_.y = &y_[0];
        pool_.vx = &vx_[0]; pool_.vy = &vy_[0];
        pool_.ttl = &ttl_[0];
        pool_.hue = &kind_[0];   // the role: 0 is Pacman, and above it a ghost color index
        pool_.count = kPool;
        pool_.clear();
        rng_.seed(kSeed);
        for (uint16_t i = 0; i < wanted(); i++) launch(i, /*anywhere=*/true);
        time_.reset();
        chomp_ = BeatPhase{};
    }

    /// The sprite magnification, grid-proportional when `spriteSize` is 0, as the other sprite effects do.
    uint8_t spriteScale() const {
        if (spriteSize > 0) return spriteSize;
        const lengthType m = width() < height() ? width() : height();
        const lengthType autoScale = m / 40;
        return static_cast<uint8_t>(autoScale < 1 ? 1 : (autoScale > 12 ? 12 : autoScale));
    }

    /// Step the cast, re-enter whoever walked off, and draw each through its own palette.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width();
        if (!pool_.valid()) return;
        const uint8_t sc = spriteScale();

        draw::fill(cv, RGB{0, 0, 0});

        const uint32_t scale = time_.advance(elapsed());
        if (scale > 0) pool_.stepDriven(scale, audioReactive, wanted());

        // One clock for both, since the arcade runs them at the same rate.
        chomp_.advanceTo(elapsed(), 420);

        syncPopulation();

        for (uint16_t i = 0; i < pool_.count; i++) {
            if (!pool_.ttl[i]) continue;
            const lengthType px = draw::toPixel(pool_.x[i]);
            const lengthType py = draw::toPixel(pool_.y[i]);
            const uint8_t role = pool_.hue[i];

            // Off an edge: re-enter from the far side, as the arcade's tunnel does.
            if (px < -static_cast<lengthType>(pacart::W) * sc || px > w + pacart::W * sc) {
                launch(i, /*anywhere=*/false);
                continue;
            }

            RGB pal[pacart::kPaletteCount];
            const bool flip = pool_.vx[i] < 0;

            if (role == 0) {
                pacmanPalette(pal);
                const draw::sprites::Sprite s{pacart::kPac, pal, pacart::W, pacart::H,
                                              pacart::F, pacart::kPaletteCount};
                draw::sprite(cv, s, static_cast<uint8_t>(chomp_.phase(4) & 0x03), px, py, sc, flip);
            } else {
                ghostPalette(static_cast<uint8_t>(role - 1), pal);
                const draw::sprites::Sprite s{pacart::kGhost, pal, pacart::GW, pacart::GH,
                                              pacart::GF, pacart::kPaletteCount};
                const uint8_t f = static_cast<uint8_t>((chomp_.phase(2) + i) & 0x01);
                draw::sprite(cv, s, f, px, py, sc, flip);
            }
        }
    }

private:
    static constexpr uint32_t kSeed = 0x9AC3A17Du;

    /// Pacman is yellow, the one color here the palette does not drive.
    void pacmanPalette(RGB (&pal)[pacart::kPaletteCount]) const {
        pal[pacart::kClear] = RGB{0, 0, 0};
        pal[pacart::kBody]  = RGB{255, 214, 0};
        pal[pacart::kEye]   = RGB{0, 0, 0};
        pal[pacart::kPupil] = RGB{0, 0, 0};
        pal[pacart::kDark]  = RGB{140, 118, 0};
    }

    /// A ghost takes its body from the palette, spread apart, keeping the white eyes that make it a ghost.
    void ghostPalette(uint8_t which, RGB (&pal)[pacart::kPaletteCount]) const {
        pal[pacart::kClear] = RGB{0, 0, 0};
        pal[pacart::kBody]  = colorFromPalette(*Palettes::active(),
                                               static_cast<uint8_t>(which * 64 + 16));
        pal[pacart::kEye]   = RGB{255, 255, 255};
        pal[pacart::kPupil] = RGB{30, 30, 160};
        pal[pacart::kDark]  = blend(pal[pacart::kBody], RGB{0, 0, 0}, 150);
    }

    /// How many characters both controls ask for, capped at the pool.
    uint16_t wanted() const {
        const uint16_t n = static_cast<uint16_t>(pacmen) + ghosts;
        return n > kPool ? kPool : n;
    }

    /// Top up or retire slots when a count control changes, live, without a re-prepare.
    void syncPopulation() {
        const uint16_t want = wanted();
        uint16_t alive = 0;
        for (uint16_t i = 0; i < pool_.count; i++) if (pool_.ttl[i]) alive++;
        for (uint16_t i = 0; i < pool_.count && alive < want; i++)
            if (!pool_.ttl[i]) { launch(i, /*anywhere=*/true); alive++; }
        for (uint16_t i = pool_.count; i-- > 0 && alive > want;)
            if (pool_.ttl[i]) { pool_.ttl[i] = 0; alive--; }

        // A trade leaves the total unchanged, so re-role only the slots whose boundary moved.
        for (uint16_t i = 0; i < pool_.count; i++)
            if (pool_.ttl[i] && pool_.hue[i] != roleFor(i)) launch(i, /*anywhere=*/true);
    }

    /// The role slot `i` holds, by slot order, which is the one home for the rule.
    uint8_t roleFor(uint16_t i) const {
        return (i < pacmen) ? 0 : static_cast<uint8_t>(1 + ((i - pacmen) & 0x03));
    }

    /// Put character `i` on the wall, its lane and speed following its role.
    void launch(uint16_t i, bool anywhere) {
        const lengthType w = width(), h = height();
        const uint8_t sc = spriteScale();

        const uint8_t role = roleFor(i);
        pool_.hue[i] = role;

        // Speed scales with the sprite, and Pacman runs a touch quicker, as in the arcade.
        const int32_t base = static_cast<int32_t>(speed) * sc * (role == 0 ? 5 : 4) / 4;
        const bool leftward = (rng_.next8() & 1) != 0;
        pool_.vx[i] = static_cast<draw::pos_t>(leftward ? -base : base);
        pool_.vy[i] = 0;   // straight lines for now, which the maze will turn

        const uint16_t slots = wanted() ? wanted() : 1;
        // Lanes rather than a scatter, kept distinct at any count by `particles::spreadLane`.
        const lengthType row = particles::spreadLane(i, slots,
                                                     static_cast<lengthType>(h - pacart::H * sc));
        pool_.x[i] = draw::toSub(anywhere
            ? particles::spreadLane(static_cast<uint16_t>(i * 2), slots, w)
            : (leftward ? static_cast<lengthType>(w + pacart::W * sc)
                        : static_cast<lengthType>(-pacart::W * sc)));
        pool_.y[i] = draw::toSub(row);
        pool_.ttl[i] = 0xFFFF;   // they leave by walking off, not by expiring
    }

    particles::Pool pool_;                  ///< the view over the arrays below
    ScratchBuffer<draw::pos_t> x_{*this}, y_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};    ///< a character leaves by walking off, not by expiring
    ScratchBuffer<uint8_t> kind_{*this};    ///< each slot's role
    particles::FrameTime time_;             ///< the movement clock
    BeatPhase chomp_;                       ///< the shared chomp and shuffle clock
    Random8 rng_;                           ///< fixed-seed, so the goldens reproduce
};

}  // namespace mm
