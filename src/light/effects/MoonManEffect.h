#pragma once

#include "core/util/math16.h"      // BeatPhase: the chomp / shuffle clock
#include "core/util/math8.h"       // Random8: fixed-seed spawn variation, golden-reproducible
#include "light/effects/AudioEffectBase.h"   // the analysis, while this effect reads it
#include "light/effects/ArcadeArt.h"         // moonart:: and invart::, MoonMan and the invaders he meets
#include "light/powerfunctions/particles.h"  // Pool: the movable-things kernel the cast rides

namespace mm {

/// The arcade cast crossing a light wall: MoonMan chomps along while the invaders drift past, wrapping around the edges forever.
/// @card MoonManEffect.gif
///
/// The characters travel independently, each on its own path. MoonMan keeps his yellow, and the invaders take their colors from the active palette.
///
/// @technical
///
/// Movement is a `particles::Pool` entry per character, and appearance is `draw::sprite`. The chomp and the invaders' march share one BeatPhase, offset so the cast pulses out of step.
class MoonManEffect : public AudioEffectBase {
public:
    /// The pool size: both count controls at their maxima.
    static constexpr uint8_t kPool = 12;

    /// Catalog tags: the audio glyph applies when `audioReactive` is set.
    const char* tags() const override { return "💫🎶✨👾"; }
    /// A wall needs a width and a height, so it is a 2D effect.
    Dim dimensions() const override { return Dim::D2; }

    /// How many MoonMen cross the wall, up to four.
    uint8_t moonmen = 1;
    /// How many invaders cross the wall, the three kinds taking turns.
    uint8_t invaders = 4;
    /// Travel rate in sprite widths, the same on any grid.
    uint8_t speed  = 96;
    /// Sprite magnification, scaled with the grid at zero.
    uint8_t spriteSize = 0;
    /// Each sprite follows its own band, so the scene breathes.
    bool audioReactive = false;
    bool readsAnalysis() const override { return audioReactive; }   ///< the analysis while the audio drives this effect

    /// Publish both counts, the speed, the sprite scale and the audio switch.
    void defineControls() override {
        controls_.addControl("moonmen", moonmen, 0, 4);
        controls_.addControl("invaders", invaders, 0, 8);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("spriteSize", spriteSize, 0, 12);
        controls_.addControl("audioReactive", audioReactive);
    }

    /// Size the pool's storage, wire the view over it, and put the cast on the wall.
    void prepare() override {
        AudioEffectBase::prepare();
        const bool ok = x_.resize(kPool) && y_.resize(kPool) && vx_.resize(kPool) &&
                        vy_.resize(kPool) && ttl_.resize(kPool) && kind_.resize(kPool);
        if (!ok) { pool_ = particles::Pool{}; return; }
        pool_ = particles::Pool{};
        pool_.x = &x_[0]; pool_.y = &y_[0];
        pool_.vx = &vx_[0]; pool_.vy = &vy_[0];
        pool_.ttl = &ttl_[0];
        pool_.hue = &kind_[0];   // the role: 0 is MoonMan, and above it an invader's color index
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

            // Off an edge: re-enter from the far side, as the arcade's tunnel does, once the widest of the cast, the octopus, is gone.
            if (px < -static_cast<lengthType>(kWidest) * sc || px > w + kWidest * sc) {
                launch(i, /*anywhere=*/false);
                continue;
            }

            const bool flip = pool_.vx[i] < 0;

            if (role == 0) {
                RGB pal[moonart::kPaletteCount];
                moonart::moonManPalette(pal);
                const draw::sprites::Sprite s{moonart::kMoonMan, pal, moonart::W, moonart::H,
                                              moonart::F, moonart::kPaletteCount};
                draw::sprite(cv, s, static_cast<uint8_t>(chomp_.phase(4) & 0x03), px, py, sc, flip);
            } else {
                const uint8_t which = static_cast<uint8_t>(role - 1);
                RGB pal[invart::kPaletteCount];
                invart::invaderPalette(pal, static_cast<uint8_t>(which * 64 + 16));   // spread apart on the palette
                const uint8_t f = static_cast<uint8_t>((chomp_.phase(2) + i) & 0x01);
                const lengthType iy = static_cast<lengthType>(py + (moonart::H - invart::H) / 2 * sc);   // 8 high, centered on MoonMan's 11
                drawInvader(cv, static_cast<uint8_t>(which % 3), pal, f, px, iy, sc, flip);
            }
        }
    }

private:
    static constexpr uint32_t kSeed = 0x9AC3A17Du;
    static constexpr uint8_t kWidest = invart::OW > moonart::W ? invart::OW : moonart::W;   ///< the widest sprite, so none leaves the wall partly drawn

    /// One invader in its two-frame march: the squid, the crab or the octopus, each its own drawing.
    static void drawInvader(const draw::Canvas& cv, uint8_t species, const RGB (&pal)[invart::kPaletteCount], uint8_t f,
                            lengthType px, lengthType py, uint8_t sc, bool flip) {
        if (species == 0) draw::sprite(cv, {invart::kSquid, pal, invart::W, invart::H, invart::F, invart::kPaletteCount}, f, px, py, sc, flip);
        else if (species == 1) draw::sprite(cv, {invart::kCrab, pal, invart::CW, invart::CH, invart::CF, invart::kPaletteCount}, f, px, py, sc, flip);
        else draw::sprite(cv, {invart::kOcto, pal, invart::OW, invart::OH, invart::OF, invart::kPaletteCount}, f, px, py, sc, flip);
    }

    /// How many characters both controls ask for, capped at the pool.
    uint16_t wanted() const {
        const uint16_t n = static_cast<uint16_t>(moonmen) + invaders;
        return n > kPool ? kPool : n;
    }

    /// Top up or retire slots when a count control changes, live, in place of a re-prepare.
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
        return (i < moonmen) ? 0 : static_cast<uint8_t>(1 + ((i - moonmen) & 0x03));
    }

    /// Put character `i` on the wall, its lane and speed following its role.
    void launch(uint16_t i, bool anywhere) {
        const lengthType w = width(), h = height();
        const uint8_t sc = spriteScale();

        const uint8_t role = roleFor(i);
        pool_.hue[i] = role;

        // Speed scales with the sprite, and MoonMan runs a touch quicker, as in the arcade.
        const int32_t base = static_cast<int32_t>(speed) * sc * (role == 0 ? 5 : 4) / 4;
        const bool leftward = (rng_.next8() & 1) != 0;
        pool_.vx[i] = static_cast<draw::pos_t>(leftward ? -base : base);
        pool_.vy[i] = 0;   // straight lines for now, which the maze will turn

        const uint16_t slots = wanted() ? wanted() : 1;
        // Lanes rather than a scatter, kept distinct at any count by `particles::spreadLane`.
        const lengthType row = particles::spreadLane(i, slots,
                                                     static_cast<lengthType>(h - moonart::H * sc));
        pool_.x[i] = draw::toSub(anywhere
            ? particles::spreadLane(static_cast<uint16_t>(i * 2), slots, w)
            : (leftward ? static_cast<lengthType>(w + moonart::W * sc)
                        : static_cast<lengthType>(-moonart::W * sc)));
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
