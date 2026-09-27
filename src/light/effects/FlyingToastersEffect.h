#pragma once
// Author: MoonLight original

#include "core/util/math16.h"      // BeatPhase: the shared wing-flap clock
#include "core/util/math8.h"       // Random8: fixed-seed spawn variation, golden-reproducible
#include "light/effects/EffectBase.h"
#include "light/powerfunctions/particles.h"  // Pool: the movable-things kernel the toasters ride

namespace mm {

namespace toasterart {

/// The shared palette: 0 is transparent, then chrome greys, slot dark, wing grey and toast browns.
inline constexpr RGB kPalette[] = {
    {0, 0, 0},        // 0: transparent key (never read)
    {188, 192, 200},  // 1: chrome body
    {126, 130, 142},  // 2: chrome shade
    {235, 238, 245},  // 3: chrome highlight
    {40, 42, 50},     // 4: slot / outline dark
    {214, 216, 224},  // 5: wing
    {196, 140, 72},   // 6: toast crust
    {230, 186, 118},  // 7: toast face
};
inline constexpr uint8_t kPaletteCount = sizeof(kPalette) / sizeof(kPalette[0]);

/// Toaster, 12x9 in 4 frames of wing up, mid, down and mid, rows top to bottom, the wing upper-left and the slot on the chrome loaf.
inline constexpr uint8_t W = 12, H = 9, F = 4;
inline constexpr uint8_t kToaster[] = {
    // frame 0: wing up
    0,5,5,0,0,4,4,4,4,4,0,0,
    0,5,5,0,4,1,1,1,1,1,4,0,
    0,5,5,4,1,3,1,1,1,1,1,4,
    0,0,5,4,1,1,1,1,1,1,1,4,
    0,0,0,4,1,1,1,1,1,1,2,4,
    0,0,0,4,1,1,1,1,1,2,2,4,
    0,0,0,4,2,1,1,1,2,2,2,4,
    0,0,0,0,4,4,4,4,4,4,4,0,
    0,0,0,0,0,4,0,0,0,4,0,0,
    // frame 1: wing mid
    0,0,0,0,0,4,4,4,4,4,0,0,
    0,0,0,0,4,1,1,1,1,1,4,0,
    5,5,5,4,1,3,1,1,1,1,1,4,
    0,5,5,4,1,1,1,1,1,1,1,4,
    0,0,0,4,1,1,1,1,1,1,2,4,
    0,0,0,4,1,1,1,1,1,2,2,4,
    0,0,0,4,2,1,1,1,2,2,2,4,
    0,0,0,0,4,4,4,4,4,4,4,0,
    0,0,0,0,0,4,0,0,0,4,0,0,
    // frame 2: wing down
    0,0,0,0,0,4,4,4,4,4,0,0,
    0,0,0,0,4,1,1,1,1,1,4,0,
    0,0,0,4,1,3,1,1,1,1,1,4,
    0,5,5,4,1,1,1,1,1,1,1,4,
    0,5,5,4,1,1,1,1,1,1,2,4,
    5,5,0,4,1,1,1,1,1,2,2,4,
    0,0,0,4,2,1,1,1,2,2,2,4,
    0,0,0,0,4,4,4,4,4,4,4,0,
    0,0,0,0,0,4,0,0,0,4,0,0,
    // frame 3: wing mid (return stroke)
    0,0,0,0,0,4,4,4,4,4,0,0,
    0,0,0,0,4,1,1,1,1,1,4,0,
    5,5,5,4,1,3,1,1,1,1,1,4,
    0,5,5,4,1,1,1,1,1,1,1,4,
    0,0,0,4,1,1,1,1,1,1,2,4,
    0,0,0,4,1,1,1,1,1,2,2,4,
    0,0,0,4,2,1,1,1,2,2,2,4,
    0,0,0,0,4,4,4,4,4,4,4,0,
    0,0,0,0,0,4,0,0,0,4,0,0,
};
static_assert(sizeof(kToaster) == static_cast<size_t>(W) * H * F, "toaster: 4 frames of 12x9");

inline constexpr uint8_t TW = 6, TH = 6;
inline constexpr uint8_t kToast[] = {
    0,6,6,6,6,0,
    6,7,7,7,7,6,
    6,7,7,7,7,6,
    6,7,7,7,7,6,
    6,7,7,7,7,6,
    0,6,6,6,6,0,
};
static_assert(sizeof(kToast) == static_cast<size_t>(TW) * TH, "toast: one 6x6 frame");

inline constexpr draw::sprites::Sprite kToasterSprite{kToaster, kPalette, W, H, F, kPaletteCount};
inline constexpr draw::sprites::Sprite kToastSprite{kToast, kPalette, TW, TH, 1, kPaletteCount};

}  // namespace toasterart

/// Effect: chrome toasters with flapping wings and toast drift diagonally across the dark.
/// @card FlyingToastersEffect.gif
///
/// Inspired by After Dark's Flying Toasters, at Frank's suggestion of sprite support.
/// The art is drawn fresh here, since the original is Berkeley Systems' and was famously litigated.
///
/// @moreinfo
///
/// ## The construction
///
/// Movement is `particles::Pool` at a constant diagonal velocity, respawning as it wraps.
/// Appearance is the stateless `draw::sprite`.
/// The wing flap is one BeatPhase, offset per toaster so the flock never syncs.
class FlyingToastersEffect : public EffectBase {
public:
    /// The pool size: both count controls at their maxima.
    static constexpr uint8_t kPool = 20;

    /// Catalog tags: the audio glyph applies when `audioReactive` is set.
    const char* tags() const override { return "💫🎶✨👾"; }
    /// A flock needs a width and a height, so it is a 2D effect.
    Dim dimensions() const override { return Dim::D2; }

    /// How many toasters fly.
    uint8_t toasters = 5;
    /// How many slices of toast fly with them.
    uint8_t toast    = 3;
    /// How fast the flock drifts.
    uint8_t speed    = 96;
    /// Pixels per art pixel, where 0 scales with the grid.
    uint8_t spriteSize = 0;
    /// Move to the music, each sprite on its own band, holding still in silence.
    bool audioReactive = false;

    /// Publish both counts, the drift, the sprite scale and the audio switch.
    void defineControls() override {
        controls_.addControl("toasters", toasters, 1, 12);
        controls_.addControl("toast", toast, 0, 8);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("spriteSize", spriteSize, 0, 12);
        controls_.addControl("audioReactive", audioReactive);
    }

    /// Size the pool's storage, wire the view over it, and fill the sky.
    void prepare() override {
        // Resize all, then test, so every buffer is sized even if an earlier one fails.
        const bool ok = x_.resize(kPool) && y_.resize(kPool) && vx_.resize(kPool) &&
                        vy_.resize(kPool) && ttl_.resize(kPool) && kind_.resize(kPool);
        if (!ok) { pool_ = particles::Pool{}; return; }
        pool_ = particles::Pool{};
        pool_.x = &x_[0]; pool_.y = &y_[0];
        pool_.vx = &vx_[0]; pool_.vy = &vy_[0];
        pool_.ttl = &ttl_[0];
        pool_.hue = &kind_[0];   // the sprite kind: 0 is a toaster, 1 a slice of toast
        pool_.count = kPool;
        pool_.clear();
        rng_.seed(kSeed);
        for (uint16_t i = 0; i < wanted(); i++) launch(i, /*anywhere=*/true);
        time_.reset();
        flap_ = BeatPhase{};
    }

    /// The sprite magnification, grid-proportional when `spriteSize` is 0.
    uint8_t spriteScale() const {
        if (spriteSize > 0) return spriteSize;
        const lengthType m = width() < height() ? width() : height();
        const lengthType autoScale = m / 40;
        return static_cast<uint8_t>(autoScale < 1 ? 1 : (autoScale > 12 ? 12 : autoScale));
    }

    /// Drift the flock, respawn whatever left the frame, and draw each sprite.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType h = height();
        // No grid-size guard: draw::sprite clips per pixel, so a small grid shows a clipped toaster.
        if (!pool_.valid()) return;
        const uint8_t sc = spriteScale();

        draw::fill(cv, RGB{0, 0, 0});

        const uint32_t scale = time_.advance(elapsed());
        if (scale > 0) pool_.stepDriven(scale, audioReactive, wanted());

        // One shared clock, offset per toaster, at about three flaps a second.
        flap_.advanceTo(elapsed(), 180);

        for (uint16_t i = 0; i < pool_.count; i++) {
            if (!pool_.ttl[i]) continue;
            const lengthType px = draw::toPixel(pool_.x[i]);
            const lengthType py = draw::toPixel(pool_.y[i]);
            // Left the lower-left: respawn into the upper-right band.
            if (px < -static_cast<lengthType>(toasterart::W) * sc || py > h) {
                launch(i, /*anywhere=*/false);
                continue;
            }
            if (pool_.hue[i] == 0) {
                const uint8_t frame =
                    static_cast<uint8_t>((flap_.phase(4) + (i * 7) % 4) & 0x03);
                draw::sprite(cv, toasterart::kToasterSprite, frame, px, py, sc);
            } else {
                draw::sprite(cv, toasterart::kToastSprite, 0, px, py, sc);
            }
        }
        // The count controls apply live, topping up or trimming rather than re-preparing.
        syncPopulation();
    }

private:
    static constexpr uint32_t kSeed = 0x70A57E25u;   ///< fixed, so the goldens reproduce

    /// How many sprites both controls ask for.
    uint16_t wanted() const { return static_cast<uint16_t>(toasters + toast); }

    /// Spawn slot `i`, scattered on the first fill and entering off the upper right after.
    void launch(uint16_t i, bool anywhere) {
        const bool isToast = i >= toasters;
        const lengthType w = width(), h = height();
        const uint8_t sc = spriteScale();
        // Diagonal toward the lower left, scaled with the sprite so flight reads alike on any grid.
        const int32_t base = static_cast<int32_t>(speed) * 2 * sc;
        const int32_t vary = base / 4;
        // next16, since the span passes 255 at any real scale and an 8-bit draw clamps it silently.
        const uint32_t span = static_cast<uint32_t>(vary) * 2;
        const int32_t v = base - vary + (span > 0 ? static_cast<int32_t>(rng_.next16() % span) : 0);
        draw::pos_t px, py;
        if (anywhere) {
            px = draw::toSub(static_cast<lengthType>(rng_.next16() % (w > 0 ? w : 1)));
            py = draw::toSub(static_cast<lengthType>(rng_.next16() % (h > 0 ? h : 1)));
        } else if (rng_.next8() & 1) {
            // Off the top edge, anywhere along the width.
            px = draw::toSub(static_cast<lengthType>(rng_.next16() % (w + toasterart::W * sc)));
            py = draw::toSub(static_cast<lengthType>(-toasterart::H * sc));
        } else {
            // Off the right edge, in the upper half.
            px = draw::toSub(static_cast<lengthType>(w));
            py = draw::toSub(static_cast<lengthType>(rng_.next16() % (h > 1 ? h / 2 + 1 : 1)));
        }
        pool_.ttl[i] = 0;
        pool_.spawn(px, py, -v, v / 2, 65535, isToast ? 1 : 0);
        // spawn() takes the first free slot, which is this one, and syncPopulation heals the rest.
    }

    /// Top up and trim slots live, since a full respawn would blink the whole flock.
    void syncPopulation() {
        for (uint16_t i = 0; i < pool_.count; i++) {
            const bool want = i < wanted();
            if (want && !pool_.ttl[i]) launch(i, false);
            else if (!want && pool_.ttl[i]) pool_.ttl[i] = 0;
            if (pool_.ttl[i]) pool_.hue[i] = i >= toasters ? 1 : 0;
        }
    }

    ScratchBuffer<draw::pos_t> x_{*this}, y_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};    ///< which slots are flying
    ScratchBuffer<uint8_t> kind_{*this};    ///< each slot's sprite kind
    particles::Pool pool_;                  ///< the view over those arrays
    particles::FrameTime time_{60};         ///< the drift clock
    BeatPhase flap_;                        ///< the shared wing-flap clock
    Random8 rng_{kSeed};                    ///< fixed-seed, so the goldens reproduce
};

}  // namespace mm
