#pragma once
// Author: MoonLight original

#include "core/util/math16.h"      // BeatPhase: the shared tail-beat clock
#include "core/util/math8.h"       // Random8: fixed-seed spawn variation, golden-reproducible
#include "light/effects/EffectBase.h"
#include "light/powerfunctions/particles.h"  // Pool: the movable-things kernel the fish ride

namespace mm {

namespace fishart {

/// Every fish sprite indexes these slots, which the effect fills per fish.
enum : uint8_t { kClear = 0, kBody = 1, kDark = 2, kLight = 3, kFin = 4, kEye = 5, kBand = 6 };
inline constexpr uint8_t kPaletteCount = 7;

// A broad reef fish, 16x11 in 3 tail-beat frames, facing right since flipX serves the other way.
inline constexpr uint8_t W = 16, H = 11, F = 3;
inline constexpr uint8_t kFish[] = {
    // frame 0: tail spread
    0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,
    0,0,0,0,2,2,1,1,6,1,2,2,0,0,0,0,
    0,0,4,4,2,1,1,6,6,1,1,1,2,0,0,0,
    0,4,4,4,2,1,1,6,6,1,1,1,1,2,0,0,
    0,0,4,2,1,1,6,6,1,1,1,1,1,1,2,0,
    2,2,2,1,1,1,6,6,1,1,1,3,5,1,1,2,
    0,0,4,2,1,1,6,6,1,1,1,1,1,1,2,0,
    0,4,4,4,2,1,1,6,6,1,1,1,1,2,0,0,
    0,0,4,4,2,1,1,6,6,1,1,1,2,0,0,0,
    0,0,0,0,2,2,1,1,6,1,2,2,0,0,0,0,
    0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,
    // frame 1: tail up
    0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,
    0,0,4,0,2,2,1,1,6,1,2,2,0,0,0,0,
    0,4,4,4,2,1,1,6,6,1,1,1,2,0,0,0,
    0,0,4,4,2,1,1,6,6,1,1,1,1,2,0,0,
    0,0,4,2,1,1,6,6,1,1,1,1,1,1,2,0,
    2,2,2,1,1,1,6,6,1,1,1,3,5,1,1,2,
    0,0,2,2,1,1,6,6,1,1,1,1,1,1,2,0,
    0,0,0,2,2,1,1,6,6,1,1,1,1,2,0,0,
    0,0,0,0,2,1,1,6,6,1,1,1,2,0,0,0,
    0,0,0,0,2,2,1,1,6,1,2,2,0,0,0,0,
    0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,
    // frame 2: tail down
    0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,
    0,0,0,0,2,2,1,1,6,1,2,2,0,0,0,0,
    0,0,0,2,2,1,1,6,6,1,1,1,2,0,0,0,
    0,0,2,2,1,1,1,6,6,1,1,1,1,2,0,0,
    0,0,4,2,1,1,6,6,1,1,1,1,1,1,2,0,
    2,2,2,1,1,1,6,6,1,1,1,3,5,1,1,2,
    0,0,4,2,1,1,6,6,1,1,1,1,1,1,2,0,
    0,0,4,4,2,1,1,6,6,1,1,1,1,2,0,0,
    0,4,4,4,2,1,1,6,6,1,1,1,2,0,0,0,
    0,0,4,0,2,2,1,1,6,1,2,2,0,0,0,0,
    0,0,0,0,0,0,2,2,2,2,0,0,0,0,0,0,
};
static_assert(sizeof(kFish) == static_cast<size_t>(W) * H * F, "fish: 3 frames of 16x11");

// A slender fish, 13x7 in 3 frames: a different silhouette, since one outline reads as a repeat.
inline constexpr uint8_t SW = 13, SH = 7, SF = 3;
inline constexpr uint8_t kSlim[] = {
    0,0,0,0,0,2,2,2,2,2,0,0,0,
    0,4,2,2,2,1,1,1,1,1,2,2,0,
    4,4,2,1,1,1,1,1,1,1,1,1,2,
    4,2,1,1,1,1,1,1,1,3,5,1,2,
    4,4,2,1,1,1,1,1,1,1,1,1,2,
    0,4,2,2,2,1,1,1,1,1,2,2,0,
    0,0,0,0,0,2,2,2,2,2,0,0,0,
    // tail up
    0,0,4,0,0,2,2,2,2,2,0,0,0,
    0,4,4,2,2,1,1,1,1,1,2,2,0,
    0,0,2,1,1,1,1,1,1,1,1,1,2,
    0,2,1,1,1,1,1,1,1,3,5,1,2,
    0,0,2,1,1,1,1,1,1,1,1,1,2,
    0,0,2,2,2,1,1,1,1,1,2,2,0,
    0,0,0,0,0,2,2,2,2,2,0,0,0,
    // tail down
    0,0,0,0,0,2,2,2,2,2,0,0,0,
    0,0,2,2,2,1,1,1,1,1,2,2,0,
    0,0,2,1,1,1,1,1,1,1,1,1,2,
    0,2,1,1,1,1,1,1,1,3,5,1,2,
    0,0,2,1,1,1,1,1,1,1,1,1,2,
    0,4,4,2,2,1,1,1,1,1,2,2,0,
    0,0,4,0,0,2,2,2,2,2,0,0,0,
};
static_assert(sizeof(kSlim) == static_cast<size_t>(SW) * SH * SF, "slim: 3 frames of 13x7");

// A tiny schooling fish, 6x4 in one frame, being too small for a tail beat to read.
inline constexpr uint8_t TW = 6, TH = 4;
inline constexpr uint8_t kTiny[] = {
    0,0,2,2,2,0,
    4,2,1,1,1,2,
    4,2,1,5,1,2,
    0,0,2,2,2,0,
};
static_assert(sizeof(kTiny) == static_cast<size_t>(TW) * TH, "tiny: one 6x4 frame");

}  // namespace fishart

/// Effect: colorful fish swim across a dark tank, each tinted from the active palette.
/// @card FishTankEffect.gif
///
/// Inspired by the aquarium screensavers of the After Dark era, with the art drawn fresh here.
/// Movement is `particles::Pool` and appearance is `draw::sprite`, as FlyingToasters does.
///
/// @moreinfo
///
/// ## One shape, as many colorways as there are fish
///
/// A Sprite points at its palette rather than owning one.
/// So each fish is drawn through a palette this effect fills from the user's active one.
/// That is why the art carries shade roles, body and dark and light, rather than fixed colors.
/// The reference aquarium's appeal is the mix, and a tank of one color is not that.
class FishTankEffect : public EffectBase {
public:
    /// The pool size: the three count controls at their maxima, summed.
    static constexpr uint8_t kPool = 24;

    /// Catalog tags: the audio glyph applies when `audioReactive` is set.
    const char* tags() const override { return "💫🎶✨👾"; }
    /// A tank needs a width and a height, so it is a 2D effect.
    Dim dimensions() const override { return Dim::D2; }

    /// How many of the broad tropical shape swim.
    uint8_t fish  = 3;
    /// How many of the slender shape swim.
    uint8_t slim  = 3;
    /// How many of the tiny schooling fish swim.
    uint8_t tiny  = 5;
    /// How fast the tank swims, in body lengths a second.
    uint8_t speed = 80;
    /// Pixels per art pixel, where 0 scales with the grid.
    uint8_t spriteSize = 0;
    /// Move to the music, each sprite on its own band, holding still in silence.
    bool audioReactive = false;

    /// Publish the three counts, the speed, the sprite scale and the audio switch.
    void defineControls() override {
        controls_.addControl("fish", fish, 0, 8);
        controls_.addControl("slim", slim, 0, 8);
        controls_.addControl("school", tiny, 0, 8);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("spriteSize", spriteSize, 0, 12);
        controls_.addControl("audioReactive", audioReactive);
    }

    /// Size the pool's storage, wire the view over it, and fill the tank.
    void prepare() override {
        const bool ok = x_.resize(kPool) && y_.resize(kPool) && vx_.resize(kPool) &&
                        vy_.resize(kPool) && ttl_.resize(kPool) && kind_.resize(kPool) &&
                        entry_.resize(kPool);
        if (!ok) { pool_ = particles::Pool{}; return; }
        pool_ = particles::Pool{};
        pool_.x = &x_[0]; pool_.y = &y_[0];
        pool_.vx = &vx_[0]; pool_.vy = &vy_[0];
        pool_.ttl = &ttl_[0];
        pool_.hue = &kind_[0];   // the species, since the palette entry has its own array
        pool_.count = kPool;
        pool_.clear();
        rng_.seed(kSeed);
        for (uint16_t i = 0; i < wanted(); i++) launch(i, /*anywhere=*/true);
        time_.reset();
        beat_ = BeatPhase{};
    }

    /// The sprite magnification, grid-proportional when `spriteSize` is 0, as FlyingToasters does.
    uint8_t spriteScale() const {
        if (spriteSize > 0) return spriteSize;
        const lengthType m = width() < height() ? width() : height();
        const lengthType autoScale = m / 40;
        return static_cast<uint8_t>(autoScale < 1 ? 1 : (autoScale > 12 ? 12 : autoScale));
    }

    /// Step every fish, respawn what swam out, and draw each through its own palette.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width();
        // No grid-size guard: draw::sprite clips per pixel.
        if (!pool_.valid()) return;
        const uint8_t sc = spriteScale();

        draw::fill(cv, RGB{0, 0, 0});

        const uint32_t scale = time_.advance(elapsed());
        if (scale > 0) pool_.stepDriven(scale, audioReactive, wanted());

        // One shared tail-beat clock, offset per fish so the tank never pulses in unison.
        beat_.advanceTo(elapsed(), 200);

        syncPopulation();

        for (uint16_t i = 0; i < pool_.count; i++) {
            if (!pool_.ttl[i]) continue;
            const lengthType px = draw::toPixel(pool_.x[i]);
            const lengthType py = draw::toPixel(pool_.y[i]);
            const uint8_t species = pool_.hue[i];
            const uint8_t entry   = entry_[i];      // the full byte, for 256 places on the palette

            // Swum off an edge: respawn as a new fish in the same slot.
            if (px < -static_cast<lengthType>(fishart::W) * sc) { launch(i, /*anywhere=*/false); continue; }
            if (px > w + fishart::W * sc) { launch(i, /*anywhere=*/false); continue; }

            RGB pal[fishart::kPaletteCount];
            paletteFor(entry, pal);
            const uint8_t frame = static_cast<uint8_t>((beat_.phase(3) + (i * 5) % 3) % 3);
            // The art faces right, so a fish swimming left is mirrored or the tank reads as wallpaper.
            const bool flip = pool_.vx[i] < 0;

            if (species == kTiny) {
                const draw::sprites::Sprite s{fishart::kTiny, pal, fishart::TW, fishart::TH, 1,
                                              fishart::kPaletteCount};
                draw::sprite(cv, s, 0, px, py, sc, flip);
            } else if (species == kSlim) {
                const draw::sprites::Sprite s{fishart::kSlim, pal, fishart::SW, fishart::SH,
                                              fishart::SF, fishart::kPaletteCount};
                draw::sprite(cv, s, frame, px, py, sc, flip);
            } else {
                const draw::sprites::Sprite s{fishart::kFish, pal, fishart::W, fishart::H,
                                              fishart::F, fishart::kPaletteCount};
                draw::sprite(cv, s, frame, px, py, sc, flip);
            }
        }
    }

private:
    enum : uint8_t { kBroad = 0, kSlim = 1, kTiny = 2 };
    static constexpr uint32_t kSeed = 0x0F157A9Bu;

    /// Fill one fish's sprite palette from the active palette, its shade roles taking the colors.
    void paletteFor(uint8_t entry, RGB (&pal)[fishart::kPaletteCount]) const {
        const RGB body = colorFromPalette(*Palettes::active(), entry);
        pal[fishart::kClear] = RGB{0, 0, 0};                 // the transparent key, never read
        pal[fishart::kBody]  = body;
        pal[fishart::kDark]  = blend(body, RGB{0, 0, 0}, 150);   // outline / shading
        pal[fishart::kLight] = blend(body, RGB{255, 255, 255}, 120);
        pal[fishart::kFin]   = blend(body, RGB{255, 255, 255}, 60);
        pal[fishart::kEye]   = RGB{20, 20, 24};
        // A paler version of the fish's own color: a second palette pick reads as two fish fused.
        pal[fishart::kBand]  = blend(body, RGB{255, 255, 255}, 200);
    }

    /// How many fish the three controls ask for, capped at the pool.
    uint16_t wanted() const {
        const uint16_t n = static_cast<uint16_t>(fish) + slim + tiny;
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

        // A trade leaves the total unchanged, so restock only the slots whose species moved.
        for (uint16_t i = 0; i < pool_.count; i++)
            if (pool_.ttl[i] && pool_.hue[i] != speciesFor(i)) launch(i, /*anywhere=*/true);
    }

    /// The species slot `i` holds, by slot order, which is the one home for the rule.
    uint8_t speciesFor(uint16_t i) const {
        if (i < fish) return kBroad;
        if (i < static_cast<uint16_t>(fish) + slim) return kSlim;
        return kTiny;
    }

    /// Put fish `i` into the tank, its speed following its size so a small one reads as further back.
    void launch(uint16_t i, bool anywhere) {
        const lengthType w = width(), h = height();
        const uint8_t sc = spriteScale();

        // Positional, so a count change moves one boundary and the rest keep their identity.
        const uint8_t species = speciesFor(i);
        pool_.hue[i] = species;
        entry_[i] = rng_.next8();        // its own place on the palette, across the full byte

        const lengthType sw = species == kBroad ? fishart::W
                            : species == kSlim  ? fishart::SW : fishart::TW;

        // Speed scales with the sprite, so the motion reads the same on any grid.
        const int32_t base = static_cast<int32_t>(speed) * sc *
                             (species == kBroad ? 3 : species == kSlim ? 2 : 1) / 2;
        const int32_t vary = base / 4;
        const uint32_t span = static_cast<uint32_t>(vary) * 2;
        const int32_t v = base - vary + (span > 0 ? static_cast<int32_t>(rng_.next16() % span) : 0);

        // Half swim each way, and the spawn edge follows, so a fish enters from behind itself.
        const bool leftward = (rng_.next8() & 1) != 0;
        pool_.vx[i] = static_cast<draw::pos_t>(leftward ? -v : v);
        // A slight vertical drift, so the tank does not read as horizontal lanes.
        pool_.vy[i] = static_cast<draw::pos_t>(static_cast<int16_t>(rng_.next8()) - 128) / 16;

        // The initial fill strides across the width, since uniform random x clumps into one shape.
        const uint16_t slots = wanted() ? wanted() : 1;
        // A stride by slot sorts by species and bunches the fast ones, so spreadLane interleaves them.
        const lengthType lane = particles::spreadLane(i, slots, w);
        pool_.x[i] = draw::toSub(anywhere
            ? static_cast<lengthType>(lane + static_cast<lengthType>(rng_.next8() % 16) - 8)
            : (leftward ? static_cast<lengthType>(w + sw * sc)
                        : static_cast<lengthType>(-sw * sc)));
        // Vertical lanes too, so the tank fills top to bottom instead of banding.
        const lengthType vlane = particles::spreadLane(static_cast<uint16_t>(i * 2), slots, h);
        pool_.y[i] = draw::toSub(static_cast<lengthType>(
            anywhere ? vlane : static_cast<lengthType>(rng_.next16() % (h > 0 ? h : 1))));
        pool_.ttl[i] = 0xFFFF;   // fish leave by swimming out, not by expiring
    }

    particles::Pool pool_;                  ///< the view over the arrays below
    ScratchBuffer<draw::pos_t> x_{*this}, y_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};    ///< a fish leaves by swimming out, not by expiring
    ScratchBuffer<uint8_t> kind_{*this};    ///< each fish's species
    ScratchBuffer<uint8_t> entry_{*this};   ///< each fish's palette entry
    particles::FrameTime time_;             ///< the physics clock
    BeatPhase beat_;                        ///< the shared tail-beat clock
    Random8 rng_;                           ///< fixed-seed, so the goldens reproduce
};

}  // namespace mm
