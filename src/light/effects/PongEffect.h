#pragma once
// Author: MoonLight original

#include "core/util/format.h"   // formatTo: the score digits
#include "light/powerfunctions/draw.h"
#include "light/effects/EffectBase.h"
#include "light/effects/SpriteCast.h"   // the shared cast, when the ball is a sprite
#include "light/effects/PlayerSeat.h"   // a person takes a paddle, the game plays it again later
#include "light/powerfunctions/fonts.h"     // the score digits

namespace mm {

/// Effect: two self-playing paddles rallying a ball across the grid.
/// @card PongEffect.gif
///
/// The attract-mode reading of the 1972 original, so both paddles play themselves.
/// A perfect tracker would rally forever and never look like a game.
/// So each paddle waits out a reaction delay and aims slightly off center.
/// That is what produces the near-misses, edge hits and occasional points.
/// The score shows at the top, and the first to 11 starts a new game, as the original played.
/// `player1` and `player2` are the paddles as controls, held through `PlayerSeat`.
///
/// @moreinfo
///
/// ## The ball is the shared sprite cast
///
/// It is either the classic square or one member of the cast in SpriteCast.h.
/// A Pacman crossing the court is the same code path as the fountain throwing one.
/// That is why the cast lives in its own header rather than in either effect.
class PongEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, a game.
    const char* tags() const override { return "💫👾"; }
    /// A court needs a width and a height, so it is a 2D effect.
    Dim dimensions() const override { return Dim::D2; }

    /// Rally speed in crossings per minute, so the court's width leaves it alone.
    uint8_t rallyBpm = 40;
    /// Paddle length as a percentage of the court's height, since short paddles miss more.
    uint8_t paddle = 30;
    /// How sharply a paddle chases the ball, from sluggish to instant.
    uint8_t reflex = 150;
    /// Swap the square ball for a member of the shared sprite cast, re-picked on every hit.
    bool spriteBall = false;
    /// Pixels per art pixel, when the ball is a sprite.
    uint8_t size = 1;
    /// The left paddle's height, bottom to top as a fader reads; follows the game until an input writes it.
    uint8_t player1 = 128;
    /// The right paddle's height, the same way.
    uint8_t player2 = 128;

    /// Publish the rally speed, the paddles and the ball's look, its size only while it is a sprite.
    void defineControls() override {
        controls_.addControl("rallyBpm", rallyBpm, 5, 200);
        controls_.addControl("paddle", paddle, 10, 60);
        controls_.addControl("reflex", reflex, 40, 255);
        controls_.addControl("spriteBall", spriteBall);
        controls_.addControl("size", size, 1, 4);
        // defineControls reruns on every control change, so toggling `spriteBall` re-hides it.
        controls_.setHidden(controls_.count() - 1, !spriteBall);
        // Live state a fader or an input drives, never written to flash.
        controls_.addControl("player1", player1, 0, 255);
        controls_.setLive(controls_.count() - 1);
        controls_.addControl("player2", player2, 0, 255);
        controls_.setLive(controls_.count() - 1);
    }

    /// A write to a paddle control is a person playing it.
    void onControlChanged(const char* name) override {
        const uint32_t now = layer() ? elapsed() : 0;
        if (std::strcmp(name, "player1") == 0) seat_[0].touch(now);
        else if (std::strcmp(name, "player2") == 0) seat_[1].touch(now);
    }

    /// Serve the opening ball of a fresh game once the module is built.
    void setup() override {
        EffectBase::setup();
        score_[0] = score_[1] = 0;
        serve(true);
    }

    /// Advance the rally on its clock, step the game, then draw the court.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        if (width() == 0 || height() == 0) return;
        draw::fill(cv, RGB{0, 0, 0});

        // Motion by elapsed time, so the ball crosses in the same wall-clock time at any frame rate.
        rally_.advanceTo(elapsed(), rallyBpm);
        // A held paddle sits where its control says; a free one is played, and its control follows it.
        uint8_t* players[2] = {&player1, &player2};
        for (uint8_t p = 0; p < 2; p++) {
            human_[p] = seat_[p].held(elapsed());
            // The court counts down from the top, a fader up from the bottom.
            if (human_[p]) py_[p] = static_cast<int32_t>(255 - *players[p]) * kCourtScale / 255;
        }
        step(rally_.phase(kCourtScale));
        for (uint8_t p = 0; p < 2; p++)
            if (!human_[p]) *players[p] = static_cast<uint8_t>(255 - py_[p] * 255 / kCourtScale);
        render(cv);
    }

    /// A paddle's center in court units, for tests.
    int32_t paddleForTest(uint8_t side) const { return py_[side < 2 ? side : 1]; }
    /// One side's points this game, for tests.
    uint8_t scoreForTest(uint8_t side) const { return score_[side < 2 ? side : 1]; }
    /// Score one point for a side, as a miss would, for tests.
    void pointForTest(uint8_t side) { point(side < 2 ? side : 1); }

private:
    /// One update of the ball and both paddles, in fixed point across a court of kCourtScale units.
    void step(uint32_t travel) {
        const uint32_t moved = travel - lastTravel_;
        if (moved == 0) return;                  // no time passed
        lastTravel_ = travel;

        // Both are fractions of the court, so the trajectory is identical whatever the grid.
        bx_ += static_cast<int32_t>(moved) * dirX_;
        by_ += static_cast<int32_t>(moved) * driftY_ / kDriftScale;

        // The top and bottom walls reflect, the way the original does.
        if (by_ < 0) { by_ = -by_; driftY_ = static_cast<int16_t>(-driftY_); }
        if (by_ > kCourtScale) { by_ = 2 * kCourtScale - by_; driftY_ = static_cast<int16_t>(-driftY_); }

        if (!human_[0]) chase(0, moved);
        if (!human_[1]) chase(1, moved);

        // A hit sends the ball back with a new drift, and a miss is a point for the other side.
        if (dirX_ < 0 && bx_ <= kPaddleX) {
            if (hits(0)) bounce(0); else point(1);
        } else if (dirX_ > 0 && bx_ >= kCourtScale - kPaddleX) {
            if (hits(1)) bounce(1); else point(0);
        }
    }

    /// The receiving paddle chases the ball once its delay runs out; the other drifts back to its own spot.
    void chase(uint8_t p, uint32_t moved) {
        if (delay_[p] > moved) { delay_[p] = static_cast<uint16_t>(delay_[p] - moved); return; }
        delay_[p] = 0;
        // Both tracking the ball would mirror each other, so only the side it is heading for follows it.
        const bool receiving = (p == 0) == (dirX_ < 0);
        // The ball plus this paddle's error, since one aiming true would never miss.
        const int32_t target = receiving ? by_ + aim_[p] : home_[p];
        const int32_t gap = target - py_[p];
        const int32_t stepBy = static_cast<int32_t>(moved) * reflex / 255;
        if (gap > stepBy)       py_[p] += stepBy;
        else if (gap < -stepBy) py_[p] -= stepBy;
        else                    py_[p] = target;
        if (py_[p] < 0) py_[p] = 0;
        if (py_[p] > kCourtScale) py_[p] = kCourtScale;
    }

    /// Did paddle `p` get there in time? Its reach is half its length either side of its center.
    bool hits(uint8_t p) const {
        const int32_t half = kCourtScale * paddle / 200;   // paddle% of the court, halved
        const int32_t d = by_ - py_[p];
        return d >= -half && d <= half;
    }

    /// Send the ball back, where it landed on the paddle setting the new drift.
    void bounce(uint8_t p) {
        const int32_t half = kCourtScale * paddle / 200;
        const int32_t off = half == 0 ? 0 : (by_ - py_[p]) * kMaxDrift / half;
        driftY_ = static_cast<int16_t>(off);
        dirX_ = static_cast<int8_t>(-dirX_);
        bx_ = dirX_ > 0 ? kPaddleX : kCourtScale - kPaddleX;
        // Both react to the turn, since a paddle starting instantly is an unbeatable player.
        delay_[0] = react(0);
        delay_[1] = react(1);
        pickBall();
        seed_++;
    }

    /// A new character for the ball, re-rolled on every hit so the swap lands on the impact.
    void pickBall() {
        kind_ = static_cast<uint8_t>(hashInt(seed_, 5, 11) % spritecast::kKindCount);
        entry_ = static_cast<uint8_t>(hashInt(seed_, 9, 13) & 0xFF);
    }

    /// Score a point for `side`, starting a new game at 11, then serve at whoever conceded it.
    void point(uint8_t side) {
        if (++score_[side] >= kWinScore) score_[0] = score_[1] = 0;
        serve(side == 0);
    }

    /// The ball restarts from the middle, heading at whoever conceded it.
    void serve(bool toRight) {
        bx_ = kCourtScale / 2;
        by_ = kCourtScale / 2;
        dirX_ = toRight ? 1 : -1;
        driftY_ = static_cast<int16_t>(static_cast<int32_t>(hashInt(seed_, 3, 7) % (2 * kMaxDrift)) - kMaxDrift);
        // A played paddle re-centers for the serve; a held one stays where its player put it.
        for (uint8_t p = 0; p < 2; p++)
            if (!human_[p]) py_[p] = kCourtScale / 2;
        delay_[0] = react(0);
        delay_[1] = react(1);
        pickBall();
        seed_++;
    }

    /// This paddle's delay, aim and resting spot for the coming exchange, re-rolled so neither stays the weaker.
    uint16_t react(uint8_t p) {
        aim_[p] = static_cast<int16_t>(static_cast<int32_t>(hashInt(seed_, p, 17) % (2 * kMaxAim)) - kMaxAim);
        // Somewhere in the middle half of the court, so the waiting paddle settles on its own line.
        home_[p] = kCourtScale / 4 + static_cast<int32_t>(hashInt(seed_, p + 4, 23) % (kCourtScale / 2));
        return static_cast<uint16_t>(hashInt(seed_, p + 2, 19) % kMaxDelay);
    }

    /// Draw the net, both paddles and the ball, scaling the court onto the grid.
    void render(const draw::Canvas& cv) {
        const lengthType w = width(), h = height();
        const int32_t courtW = w > 1 ? w - 1 : 1;
        const int32_t courtH = h > 1 ? h - 1 : 1;
        const RGB fg = colorFromPalette(*Palettes::active(), 200);

        // The dashed center line, which is what says this is a court rather than two blocks.
        const lengthType netX = static_cast<lengthType>(w / 2);
        const RGB net = blend(fg, RGB{0, 0, 0}, 170);
        for (lengthType y = 0; y < h; y += 3) draw::pixel(cv, {netX, y, 0}, net);

        // Each side's score centered in its half, where the court has room for the digits.
        const fonts::Font& font = fonts::kFont4x6;
        if (h >= 3 * font.height && w >= 4 * 2 * font.width) {
            for (uint8_t p = 0; p < 2; p++) {
                char digits[4];
                mm::formatTo(digits, sizeof(digits), "%u", static_cast<unsigned>(score_[p]));
                const lengthType tw = static_cast<lengthType>(std::strlen(digits) * font.width);
                const lengthType cx = static_cast<lengthType>(p == 0 ? w / 4 : w * 3 / 4);
                draw::text(cv, font, digits, static_cast<lengthType>(cx - tw / 2), 1, net);
            }
        }

        // A column at each end, `paddle` percent of the court tall.
        const int32_t half = courtH * paddle / 200;
        for (uint8_t p = 0; p < 2; p++) {
            const lengthType px = p == 0 ? 0 : static_cast<lengthType>(w - 1);
            const int32_t cy = py_[p] * courtH / kCourtScale;
            for (int32_t y = cy - half; y <= cy + half; y++)
                if (y >= 0 && y < h) draw::pixel(cv, {px, static_cast<lengthType>(y), 0}, fg);
        }

        const lengthType bxp = static_cast<lengthType>(bx_ * courtW / kCourtScale);
        const lengthType byp = static_cast<lengthType>(by_ * courtH / kCourtScale);
        if (spriteBall) {
            // The sprite faces its travel, as every other sprite in the project does.
            spritecast::draw(cv, kind_, entry_, bxp, byp, size == 0 ? 1 : size, dirX_ < 0,
                             static_cast<uint8_t>(rally_.phase(4) & 0xFF));
        } else {
            draw::pixel(cv, {bxp, byp, 0}, fg);
        }
    }

    /// The court is fixed point, so a position is a fraction scaled to the grid only when drawn.
    static constexpr int32_t  kCourtScale = 4096;
    static constexpr int32_t  kPaddleX    = 96;     ///< how far in from each end a paddle sits
    static constexpr int32_t  kDriftScale = 256;    ///< drift is a fraction of forward travel
    static constexpr int32_t  kMaxDrift   = 320;    ///< the steepest angle a bounce can produce
    static constexpr int32_t  kMaxAim     = 220;    ///< how far off center a paddle aims
    static constexpr uint16_t kMaxDelay   = 260;    ///< the longest reaction delay, in court units
    static constexpr uint8_t  kWinScore   = 11;     ///< the original's game length

    BeatPhase rally_;            ///< the rally clock, in crossings per minute
    uint32_t  lastTravel_ = 0;   ///< the clock reading step() last consumed, in court units
    uint8_t   score_[2] = {0, 0};   ///< left and right points this game
    PlayerSeat seat_[2];            ///< who holds each paddle
    bool      human_[2] = {false, false};   ///< this frame's answer, read once so a hand-back mid-frame cannot split it
    int32_t   bx_ = kCourtScale / 2, by_ = kCourtScale / 2;   ///< the ball, in court units
    int8_t    dirX_ = 1;                 ///< which paddle the ball is heading for
    int16_t   driftY_ = 90;              ///< its drift across the court
    int32_t   py_[2] = {kCourtScale / 2, kCourtScale / 2};   ///< each paddle's center
    uint16_t  delay_[2] = {0, 0};        ///< each paddle's remaining reaction delay
    int16_t   aim_[2] = {0, 0};          ///< each paddle's aiming error this exchange
    int32_t   home_[2] = {kCourtScale / 2, kCourtScale / 2};   ///< where each paddle waits while the ball is away
    uint8_t   kind_ = 0, entry_ = 0;     ///< the sprite ball's character and color
    uint32_t  seed_ = 1;                 ///< walked on every serve and hit
};

}  // namespace mm
