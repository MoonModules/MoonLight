#pragma once
// Author: MoonLight original

#include "core/services/AudioService.h"   // latestFrame: the beat the audio-reactive mode volleys on
#include "core/util/math16.h"
#include "light/powerfunctions/draw.h"
#include "light/effects/EffectBase.h"
#include "light/effects/SpriteCast.h"   // the shared cast, when the ball is a sprite

namespace mm {

/// Effect: two self-playing paddles rallying a ball across the grid.
/// @card PongEffect.gif
///
/// The attract-mode reading of the 1972 original, so both paddles play themselves.
/// A perfect tracker would rally forever and never look like a game.
/// So each paddle waits out a reaction delay and aims slightly off center.
/// That is what produces the near-misses, edge hits and occasional points.
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
    /// Catalog tags: the audio glyph applies when `audioReactive` is set.
    const char* tags() const override { return "💫🎵👾"; }
    /// A court needs a width and a height, so it is a 2D effect.
    Dim dimensions() const override { return Dim::D2; }

    /// Rally speed in crossings per minute, so the court's width leaves it alone.
    uint8_t rallyBpm = 40;
    /// Paddle length as a percentage of the court's height, since short paddles miss more.
    uint8_t paddle = 30;
    /// How sharply a paddle chases the ball, from sluggish to instant.
    uint8_t reflex = 150;
    /// Pixels per art pixel, when the ball is a sprite.
    uint8_t size = 1;
    /// Swap the square ball for a member of the shared sprite cast, re-picked on every hit.
    bool spriteBall = false;
    /// Advance the ball only on the beat, so it stands still in silence.
    bool audioReactive = false;

    /// Publish the rally speed, the paddles, the ball's look and the audio switch.
    void defineControls() override {
        controls_.addControl("rallyBpm", rallyBpm, 5, 200);
        controls_.addControl("paddle", paddle, 10, 60);
        controls_.addControl("reflex", reflex, 40, 255);
        controls_.addControl("size", size, 1, 4);
        controls_.addControl("spriteBall", spriteBall);
        controls_.addControl("audioReactive", audioReactive);
    }

    /// Serve the opening ball once the module is built.
    void setup() override {
        EffectBase::setup();
        serve(true);
    }

    /// Advance the rally on its clock, step the game, then draw the court.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        if (width() == 0 || height() == 0) return;
        draw::fill(cv, RGB{0, 0, 0});

        const AudioFrame* audio = audioReactive ? AudioService::latestFrame() : nullptr;
        // The project's own beat test: level against its smoothed average, read once a frame.
        const bool live = audio && audio->levelSmoothed >= kSilence;
        const bool beat = live && audio->level > audio->levelSmoothed + kBeatMargin;

        // Motion by elapsed time, so the ball crosses in the same wall-clock time at any frame rate.
        rally_.advanceTo(elapsed(), rallyBpm);
        const uint32_t travel = rally_.phase(kCourtScale);
        // Two clocks: a switch mid-rally teleports the ball one way and underflows the other.
        if (audioReactive != wasReactive_) {
            wasReactive_ = audioReactive;
            lastTravel_ = travelAt_;
        }
        if (audioReactive) {
            // The ball jumps a slice of the court on each beat, and silence holds it still.
            if (beat) travelAt_ += kCourtScale / kBeatSteps;
        } else {
            // The baseline moves with the clock, so step() sees this frame's travel alone.
            travelAt_ += travel - lastFreeTravel_;
        }
        lastFreeTravel_ = travel;
        step();
        render(cv);
    }

private:
    /// One update of the ball and both paddles, in fixed point across a court of kCourtScale units.
    void step() {
        const uint32_t now = travelAt_;
        const uint32_t moved = now - lastTravel_;
        if (moved == 0) return;                  // no time passed, or the beat has not landed yet
        lastTravel_ = now;

        // Both are fractions of the court, so the trajectory is identical whatever the grid.
        bx_ += static_cast<int32_t>(moved) * dirX_;
        by_ += static_cast<int32_t>(moved) * driftY_ / kDriftScale;

        // The top and bottom walls reflect, the way the original does.
        if (by_ < 0) { by_ = -by_; driftY_ = static_cast<int16_t>(-driftY_); }
        if (by_ > kCourtScale) { by_ = 2 * kCourtScale - by_; driftY_ = static_cast<int16_t>(-driftY_); }

        chase(0, moved);
        chase(1, moved);

        // A hit sends the ball back with a new drift, and a miss is a point and a fresh serve.
        if (dirX_ < 0 && bx_ <= kPaddleX) {
            if (hits(0)) bounce(0); else serve(false);
        } else if (dirX_ > 0 && bx_ >= kCourtScale - kPaddleX) {
            if (hits(1)) bounce(1); else serve(true);
        }
    }

    /// A paddle chases the ball once its delay runs out, as fast as `reflex` allows.
    void chase(uint8_t p, uint32_t moved) {
        if (delay_[p] > moved) { delay_[p] = static_cast<uint16_t>(delay_[p] - moved); return; }
        delay_[p] = 0;
        // The ball plus this paddle's error, since one aiming true would never miss.
        const int32_t target = by_ + aim_[p];
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

    /// A point: the ball restarts from the middle, heading at whoever conceded it.
    void serve(bool toRight) {
        bx_ = kCourtScale / 2;
        by_ = kCourtScale / 2;
        dirX_ = toRight ? 1 : -1;
        driftY_ = static_cast<int16_t>(static_cast<int32_t>(hashInt(seed_, 3, 7) % (2 * kMaxDrift)) - kMaxDrift);
        py_[0] = py_[1] = kCourtScale / 2;
        delay_[0] = react(0);
        delay_[1] = react(1);
        pickBall();
        seed_++;
    }

    /// This paddle's delay and aim for the coming exchange, re-rolled so neither stays the weaker.
    uint16_t react(uint8_t p) {
        aim_[p] = static_cast<int16_t>(static_cast<int32_t>(hashInt(seed_, p, 17) % (2 * kMaxAim)) - kMaxAim);
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
    static constexpr uint8_t  kBeatSteps  = 12;     ///< beats to cross the court in reactive mode
    static constexpr uint16_t kSilence    = 8;      ///< below this the room is quiet, not playing
    static constexpr uint16_t kBeatMargin = 24;     ///< a transient this far over the average is a beat

    BeatPhase rally_;            ///< the rally clock, in crossings per minute
    uint32_t  travelAt_ = 0;     ///< how far the rally has traveled, in court units
    uint32_t  lastTravel_ = 0;   ///< the reading step() last consumed
    uint32_t  lastFreeTravel_ = 0;   ///< last free-running reading, so a toggle costs no distance
    bool      wasReactive_ = false;  ///< which clock ran last frame; a change rebases the baseline
    int32_t   bx_ = kCourtScale / 2, by_ = kCourtScale / 2;   ///< the ball, in court units
    int8_t    dirX_ = 1;                 ///< which paddle the ball is heading for
    int16_t   driftY_ = 90;              ///< its drift across the court
    int32_t   py_[2] = {kCourtScale / 2, kCourtScale / 2};   ///< each paddle's center
    uint16_t  delay_[2] = {0, 0};        ///< each paddle's remaining reaction delay
    int16_t   aim_[2] = {0, 0};          ///< each paddle's aiming error this exchange
    uint8_t   kind_ = 0, entry_ = 0;     ///< the sprite ball's character and color
    uint32_t  seed_ = 1;                 ///< walked on every serve and hit
};

}  // namespace mm
