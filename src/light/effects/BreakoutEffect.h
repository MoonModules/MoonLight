#pragma once
// Author: MoonLight original (Breakout, Atari 1976, is the inspiration)

#include "core/services/AudioService.h"   // latestFrame: the bands the audio-reactive wall glows with
#include "light/powerfunctions/draw.h"
#include "light/effects/EffectBase.h"
#include "light/effects/PlayerSeat.h"   // a person takes the paddle, the game plays it again later

namespace mm {

/// Effect: a self-playing paddle knocking down a wall of bricks.
/// @card BreakoutEffect.gif
///
/// The attract-mode reading of the 1976 original, so the paddle plays itself.
/// It waits out a reaction delay and aims slightly off center, so steep balls outrun it and it misses.
/// The arcade's pressure is kept: the ball speeds up as bricks fall, and the paddle halves once the ball reaches the back wall.
/// Three misses, or a cleared wall, deal a fresh wall.
/// `player` is the paddle as a control, held through `PlayerSeat`.
/// With `descend` the wall creeps down and new rows enter at the top, as in Super Breakout's progressive game.
/// With `audioReactive` each brick column glows with its bands and the palette drifts with the volume; the game itself keeps its clock.
class BreakoutEffect : public EffectBase {
public:
    /// Catalog tags: the audio glyph applies when `audioReactive` is set.
    const char* tags() const override { return "💫🎵👾"; }
    /// A wall needs a width and a height, so it is a 2D effect.
    Dim dimensions() const override { return Dim::D2; }

    /// Court crossings per minute at the serve, so the grid's size leaves the pace alone.
    uint8_t speedBpm = 30;
    /// Rows of bricks, fewer where the grid has no room for them.
    uint8_t rows = 5;
    /// Paddle width as a percentage of the court.
    uint8_t paddle = 25;
    /// How sharply the paddle chases the ball, from sluggish to instant.
    uint8_t reflex = 160;
    /// The wall creeps down a row every few seconds, new rows entering at the top.
    bool descend = false;
    /// Color the bricks by the music: each column glows with its bands, the palette drifts with the volume.
    bool audioReactive = false;
    /// The paddle's position, left to right; follows the game until an input writes it.
    uint8_t player = 128;

    /// Publish the pace, the wall, the paddle and the audio switch.
    void defineControls() override {
        controls_.addControl("speedBpm", speedBpm, 5, 120);
        controls_.addControl("rows", rows, 1, kMaxRows);
        controls_.addControl("paddle", paddle, 10, 60);
        controls_.addControl("reflex", reflex, 40, 255);
        controls_.addControl("descend", descend);
        controls_.addControl("audioReactive", audioReactive);
        // Live state a fader or an input drives, never written to flash.
        controls_.addControl("player", player, 0, 255);
        controls_.setLive(controls_.count() - 1);
    }

    /// A write to the paddle control is a person playing it.
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "player") == 0) seat_.touch(layer() ? elapsed() : 0);
    }

    /// Start a new game; the wall is dealt on the first frame, once the grid is known.
    void prepare() override { newGame(); }

    /// Advance the ball on its clock, then draw the wall, the paddle and the ball.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        draw::fill(cv, RGB{0, 0, 0});
        const Geo g = geo();
        // A changed `rows`, grid or `descend` deals a fresh wall, which is how all three apply live.
        if (descend != wasDescending_) {
            wasDescending_ = descend;
            needDeal_ = true;
            creepClock_ = BeatPhase{};   // a fresh clock, or the time spent off arrives as one big drop
            lastCreep_ = 0;
        }
        if (needDeal_ || rowsFit(g) != dealtFor_) dealWall(rowsFit(g));
        if (descend) creep(g);

        // A held paddle sits where its control says; a free one is played, and its control follows it.
        human_ = seat_.held(elapsed());
        if (human_) {
            const int32_t half = paddleHalf();
            px_ = static_cast<int32_t>(player) * kCourt / 255;
            if (px_ < half) px_ = half;
            if (px_ > kCourt - half) px_ = kCourt - half;
        }
        // The game runs on its clock; the music only colors the wall.
        rally_.advanceTo(elapsed(), speedBpm);
        const uint32_t travel = rally_.phase(kCourt);
        const uint32_t moved = travel - lastTravel_;
        lastTravel_ = travel;
        if (moved > 0) play(moved * (4 + level_) / 4, g);
        if (!human_) player = static_cast<uint8_t>(px_ * 255 / kCourt);
        render(cv, g, audioReactive ? AudioService::latestFrame() : nullptr);
    }

    /// Bricks still standing, for tests.
    uint16_t bricksLeftForTest() const { return left_; }
    /// The paddle's center in court units, for tests.
    int32_t paddleForTest() const { return px_; }
    /// The ball's horizontal position in court units, for tests.
    int32_t ballXForTest() const { return bx_; }
    /// The ball's vertical position in court units, for tests.
    int32_t ballYForTest() const { return by_; }

private:
    /// The court is fixed point, 0 to kCourt on both axes, scaled to the grid only when drawn.
    static constexpr int32_t  kCourt      = 4096;
    static constexpr uint8_t  kCols       = 8;      ///< bricks per row, the same on every grid
    static constexpr uint8_t  kMaxRows    = 8;      ///< the arcade's own wall height
    static constexpr int32_t  kDriftScale = 256;    ///< sideways travel is a fraction of forward travel
    static constexpr int32_t  kMaxDrift   = 320;    ///< the steepest angle a bounce can produce
    static constexpr int32_t  kMinDrift   = 24;     ///< never straight up, or the ball repeats one column
    static constexpr uint16_t kMaxDelay   = 900;    ///< the longest reaction delay, in court units
    static constexpr uint8_t  kLives      = 3;
    static constexpr uint8_t  kCreepRpm   = 10;     ///< rows the wall descends per minute under `descend`

    /// The grid-dependent sizes, so a brick row and the paddle are at least one pixel on any grid.
    struct Geo {
        int32_t px;          ///< court units per pixel row
        int32_t top;         ///< where the wall starts
        int32_t brickH;      ///< one brick row
        int32_t paddleTop;   ///< where the paddle's top edge is
        lengthType thick;    ///< paddle and ball thickness in pixels
    };

    Geo geo() const {
        const lengthType w = width(), h = height();
        Geo g;
        g.px = h > 0 && kCourt / h > 0 ? kCourt / h : 1;
        const lengthType minSide = w < h ? w : h;
        g.thick = static_cast<lengthType>(minSide / 32 > 1 ? minSide / 32 : 1);
        g.brickH = kCourt / 16 > g.px ? kCourt / 16 : g.px;
        g.top = kCourt / 8 > 2 * g.px ? kCourt / 8 : 2 * g.px;
        g.paddleTop = kCourt - g.thick * g.px;
        return g;
    }

    /// As many of `rows` as fit above the paddle with room to play, so a short grid still gets a game.
    uint8_t rowsFit(const Geo& g) const { return rowsFitFor(g, rows > kMaxRows ? kMaxRows : rows); }

    /// As many of `want` rows as fit, keeping one row of slack for the creep and room to play below.
    uint8_t rowsFitFor(const Geo& g, uint8_t want) const {
        const int32_t room = g.paddleTop - 4 * g.px - g.top - (descend ? g.brickH : 0);
        const int32_t fit = room > 0 ? room / g.brickH : 0;
        return static_cast<uint8_t>(fit < want ? fit : want);
    }

    void newGame() {
        lives_ = kLives;
        needDeal_ = true;
        serve();
    }

    void dealWall(uint8_t n) {
        for (uint8_t r = 0; r < kMaxRows; r++) wall_[r] = r < n ? 0xFF : 0;
        dealtRows_ = n;
        dealtFor_ = n;
        creepAt_ = 0;
        left_ = static_cast<uint16_t>(n * kCols);
        hits_ = 0;
        level_ = 0;
        shrunk_ = false;
        needDeal_ = false;
    }

    /// Lower the wall by elapsed time; each full row of descent shifts the bricks down and adds a row on top.
    void creep(const Geo& g) {
        creepClock_.advanceTo(elapsed(), kCreepRpm);
        const uint32_t now = creepClock_.phase(256);   // 1/256ths of a row
        creepAt_ += now - lastCreep_;
        lastCreep_ = now;
        // As many rows as fit with the paddle's room kept free; the bottom row falls off once the wall is that tall.
        const uint8_t cap = rowsFitFor(g, kMaxRows);
        // No room for a row means nothing to lower, and an unspent creep would only grow until it overflowed.
        if (cap == 0) { creepAt_ = 0; return; }
        while (creepAt_ >= 256 && cap > 0) {
            creepAt_ -= 256;
            if (dealtRows_ < cap) dealtRows_++;
            for (uint8_t r = static_cast<uint8_t>(dealtRows_ - 1); r > 0; r--) wall_[r] = wall_[r - 1];
            wall_[0] = 0xFF;
            left_ = 0;
            for (uint8_t r = 0; r < dealtRows_; r++)
                for (uint8_t b = wall_[r]; b; b &= static_cast<uint8_t>(b - 1)) left_++;
        }
    }

    /// Where the wall's first row starts, lowered by the part of a row the creep has covered.
    int32_t wallTop(const Geo& g) const {
        return g.top + (descend ? static_cast<int32_t>(creepAt_) * g.brickH / 256 : 0);
    }

    /// A new ball from mid-court, below any wall, heading down at the paddle.
    void serve() {
        bx_ = kCourt / 4 + static_cast<int32_t>(hashInt(seed_, 3, 7) % (kCourt / 2));
        by_ = kCourt / 2;
        vy_ = 1;
        const int32_t drift = kMinDrift + static_cast<int32_t>(hashInt(seed_, 5, 11) % (kMaxDrift / 2));
        vx_ = (hashInt(seed_, 13) & 1) ? drift : -drift;
        react();
        seed_++;
    }

    /// The paddle's delay and aim for the coming ball, re-rolled so it is not equally good every time.
    void react() {
        delay_ = static_cast<uint16_t>(hashInt(seed_, 17, 19) % kMaxDelay);
        const int32_t half = paddleHalf();
        aim_ = (static_cast<int32_t>(hashInt(seed_, 23, 29) % 201) - 100) * half / 133;
    }

    int32_t paddleHalf() const {
        const int32_t half = kCourt * paddle / 200;
        return shrunk_ ? half / 2 : half;
    }

    /// Chase once for the frame, then move the ball in steps of half a pixel row, so no brick is tunneled.
    void play(uint32_t moved, const Geo& g) {
        if (moved > static_cast<uint32_t>(kCourt)) moved = kCourt;   // a long stall costs one crossing, not a skip
        if (!human_) chase(moved);
        const int32_t sub = g.px / 2 > 0 ? g.px / 2 : 1;
        int32_t rest = static_cast<int32_t>(moved);
        while (rest > 0) {
            const int32_t d = rest > sub ? sub : rest;
            rest -= d;
            if (!move(d, g)) return;
        }
    }

    /// The paddle chases the ball plus its aiming error, once its delay runs out.
    void chase(uint32_t moved) {
        if (delay_ > moved) { delay_ = static_cast<uint16_t>(delay_ - moved); return; }
        delay_ = 0;
        const int32_t half = paddleHalf();
        int32_t target = bx_ + aim_;
        if (target < half) target = half;
        if (target > kCourt - half) target = kCourt - half;
        const int32_t gap = target - px_;
        const int32_t stepBy = static_cast<int32_t>(moved) * reflex / 255;
        if (gap > stepBy)       px_ += stepBy;
        else if (gap < -stepBy) px_ -= stepBy;
        else                    px_ = target;
    }

    /// One step of the ball against the walls, the bricks and the paddle; false once a serve replaced it.
    bool move(int32_t d, const Geo& g) {
        bx_ += d * vx_ / kDriftScale;
        by_ += d * vy_;
        if (bx_ < 0)      { bx_ = -bx_;              vx_ = -vx_; }
        if (bx_ > kCourt) { bx_ = 2 * kCourt - bx_;  vx_ = -vx_; }
        // The back wall: the ball broke through, and the paddle halves for the rest of this wall.
        if (by_ < 0) { by_ = -by_; vy_ = 1; shrunk_ = true; }

        const int32_t top = wallTop(g);
        const int32_t bottom = top + dealtRows_ * g.brickH;
        if (by_ >= top && by_ < bottom) {
            const uint8_t r = static_cast<uint8_t>((by_ - top) / g.brickH);
            const int32_t c32 = bx_ * kCols / kCourt;
            const uint8_t c = static_cast<uint8_t>(c32 >= kCols ? kCols - 1 : c32);
            if (wall_[r] & (1u << c)) {
                wall_[r] = static_cast<uint8_t>(wall_[r] & ~(1u << c));
                left_--;
                vy_ = -vy_;
                speedUp(r);
                if (left_ == 0) { needDeal_ = true; serve(); return false; }
            }
        }

        if (vy_ > 0 && by_ >= g.paddleTop && !falling_) {
            const int32_t off = bx_ - px_;
            const int32_t half = paddleHalf();
            if (off >= -half && off <= half) {
                // Where it landed on the paddle sets the angle, the arcade's only control.
                by_ = 2 * g.paddleTop - by_;
                vy_ = -1;
                vx_ = half == 0 ? kMinDrift : off * kMaxDrift / half;
                if (vx_ > -kMinDrift && vx_ < kMinDrift) vx_ = vx_ < 0 ? -kMinDrift : kMinDrift;
                react();
                seed_++;
            } else {
                falling_ = true;
            }
        }
        if (by_ > kCourt) {
            falling_ = false;
            if (--lives_ == 0) newGame(); else serve();
            return false;
        }
        return true;
    }

    /// The arcade's three speed-ups: after 4 hits, after 12, and on reaching the back two rows.
    void speedUp(uint8_t row) {
        hits_++;
        if (hits_ >= 4 && level_ < 1) level_ = 1;
        if (hits_ >= 12 && level_ < 2) level_ = 2;
        if (row < 2 && dealtRows_ > 2) level_ = 3;
    }

    lengthType colOf(int32_t x) const {
        const int32_t w = width();
        int32_t c = x * w / kCourt;
        return static_cast<lengthType>(c < 0 ? 0 : (c >= w ? w - 1 : c));
    }
    lengthType rowOf(int32_t y) const {
        const int32_t h = height();
        int32_t r = y * h / kCourt;
        return static_cast<lengthType>(r < 0 ? 0 : (r >= h ? h - 1 : r));
    }

    /// Draw the wall in palette bands, then the paddle and the ball in the cabinet's white.
    void render(const draw::Canvas& cv, const Geo& g, const AudioFrame* audio) {
        const lengthType w = width(), h = height();
        const int32_t top = wallTop(g);
        // The volume drifts the palette, so a loud passage recolors the whole wall.
        const uint8_t drift = audio ? static_cast<uint8_t>((audio->level > 255 ? 255 : audio->level) / 2) : 0;
        for (uint8_t r = 0; r < dealtRows_; r++) {
            const uint8_t entry = static_cast<uint8_t>((dealtRows_ > 1 ? r * 255 / (dealtRows_ - 1) : 0) + drift);
            const RGB rowCol = colorFromPalette(*Palettes::active(), entry);
            const lengthType y0 = rowOf(top + r * g.brickH);
            lengthType bh = static_cast<lengthType>(rowOf(top + (r + 1) * g.brickH) - y0);
            if (bh >= 3) bh--;                                       // a mortar line where there is room
            if (bh < 1) bh = 1;
            for (uint8_t c = 0; c < kCols; c++) {
                if (!(wall_[r] & (1u << c))) continue;
                const lengthType x0 = static_cast<lengthType>(c * w / kCols);
                lengthType bw = static_cast<lengthType>((c + 1) * w / kCols - x0);
                if (bw >= 3) bw--;
                // Each column glows with its two bands, never fully dark, so the wall reads as an equalizer.
                RGB col = rowCol;
                if (audio) {
                    const uint8_t a = audio->bands[c * 2], b = audio->bands[c * 2 + 1];
                    const uint8_t glow = static_cast<uint8_t>(60 + (a > b ? a : b) * 195 / 255);
                    col = blend(rowCol, RGB{0, 0, 0}, static_cast<uint8_t>(255 - glow));
                }
                if (bw > 0) draw::fillRect(cv, x0, y0, bw, bh, col);
            }
        }
        const RGB white{220, 220, 220};
        const int32_t half = paddleHalf();
        const lengthType p0 = colOf(px_ - half), p1 = colOf(px_ + half);
        const lengthType py = static_cast<lengthType>(h > g.thick ? h - g.thick : 0);
        draw::fillRect(cv, p0, py, static_cast<lengthType>(p1 - p0 + 1), g.thick, white);

        const lengthType bxp = colOf(bx_), byp = rowOf(by_);
        const lengthType bs = static_cast<lengthType>(bxp + g.thick > w ? w - bxp : g.thick);
        const lengthType bt = static_cast<lengthType>(byp + g.thick > h ? h - byp : g.thick);
        draw::fillRect(cv, bxp, byp, bs, bt, white);
    }

    BeatPhase rally_;                  ///< the ball's clock, in crossings per minute
    uint32_t  lastTravel_ = 0;         ///< the clock reading the last frame consumed, in court units
    uint8_t   wall_[kMaxRows] = {};    ///< one bit per standing brick
    uint8_t   dealtRows_ = 0;          ///< rows in the current wall, growing under `descend`
    uint8_t   dealtFor_ = 0;           ///< the row count the wall was dealt for, so a grow is not a redeal
    bool      wasDescending_ = false;  ///< `descend` last frame, so a toggle deals a fresh wall
    BeatPhase creepClock_;             ///< the descent clock, in rows per minute
    uint32_t  lastCreep_ = 0;          ///< its last reading
    uint32_t  creepAt_ = 0;            ///< descent since the last row shift, in 1/256ths of a row
    uint16_t  left_ = 0;               ///< bricks still standing
    uint8_t   hits_ = 0, level_ = 0;   ///< bricks taken this wall, and the speed-up reached
    bool      shrunk_ = false;         ///< the ball reached the back wall
    bool      needDeal_ = true;        ///< deal on the next frame, where the grid is known
    bool      falling_ = false;        ///< past the paddle, on its way out
    uint8_t   lives_ = kLives;
    int32_t   bx_ = kCourt / 2, by_ = kCourt / 2;   ///< the ball, in court units
    int32_t   vx_ = kMinDrift;         ///< sideways travel per kDriftScale of forward travel
    int8_t    vy_ = 1;                 ///< down is positive, as on the grid
    int32_t   px_ = kCourt / 2;        ///< the paddle's center
    int32_t   aim_ = 0;                ///< its aiming error for this ball
    uint16_t  delay_ = 0;              ///< its remaining reaction delay
    uint32_t  seed_ = 1;               ///< walked on every serve and bounce
    PlayerSeat seat_;                  ///< who holds the paddle
    bool      human_ = false;          ///< this frame's answer, read once
};

}  // namespace mm
