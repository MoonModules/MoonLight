#pragma once

#include "light/effects/EffectBase.h"
#include "light/powerfunctions/particles.h"   // FrameTime: the shared time scale

#include "platform/platform.h"      // platform::millis (the per-drop start-delay clock)

namespace mm {

/// Tetris-style effect: falling, stacking blocks.
/// Author: Andrew Tuline (WLED-SR), via the predecessor, https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
/// @card TetrixEffect.gif
///
/// Each column drops a brick of light that falls at its own speed onto a growing stack.
/// Once the stack fills a column, that column blanks to black and the cycle restarts.
/// Every column runs its own state machine, so they desync into a rain of stacking bricks.
///
/// Prior art: MoonLight's Tetrix, descended from the WLED effect by Aircoookie and blazoncek.
///
/// @moreinfo
///
/// ## The per-column state machine
///
/// `step` carries the state: 0 idle, 1 start-roll, 2 falling.
/// Above 2 it holds a future millis() timestamp instead, for the start delay and the blank delay.
/// The physics come from the MoonLight spec, written fresh on EffectBase and the draw primitives.
/// With `oneColor` a column's bricks share one slowly-advancing palette index, else each is random.
class TetrixEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, MoonModules.
    const char* tags() const override { return "💫🌙✨"; }
    /// Writes the z=0 slice only, iterating x and y.
    Dim dimensions() const override { return Dim::D2; }

    /// Fall speed, MoonLight's default. 0 gives each brick a random speed.
    uint8_t speedControl = 0;
    /// Brick height. 0 randomizes it, otherwise it derives from this.
    uint8_t widthControl = 0;
    /// Share one slowly-advancing color across a column's bricks.
    bool    oneColor     = false;

    /// Publish the fall speed, the brick height and the one-color switch.
    void defineControls() override {
        controls_.addControl("speed", speedControl, 0, 255);
        controls_.addControl("width", widthControl, 0, 255);
        controls_.addControl("oneColor", oneColor);
    }

    /// Per-column falling-brick state, MoonLight's Tetris struct.
    struct Tetris {
        float    pos   = 0.0f;  ///< head position of the falling brick, in LED rows
        float    speed = 0.0f;  ///< fall speed in rows per frame
        uint8_t  col   = 0;     ///< palette index for this column's bricks
        uint16_t brick = 0;     ///< brick height in LEDs
        uint16_t stack = 0;     ///< stacked height at the bottom
        uint32_t step  = 0;     ///< state machine value, or a future millis() timestamp above 2
        float    roll  = 0.0f;  ///< start-roll carry, paced at the reference frame rate
    };

    /// Size one drop per column and give each a 2 s start delay.
    void prepare() override {
        // resize() reallocs only when the column count changes, and frees on 0.
        const size_t cols = (width() > 0) ? static_cast<size_t>(width()) : 0;
        drops_.resize(cols);
        if (drops_) {
            // resize() already zeroed the block, so only the non-zero fields need setting.
            const uint32_t now = platform::millis();
            for (size_t i = 0; i < drops_.count(); i++) {
                drops_[i].step = now + 2000;
                if (oneColor) drops_[i].col = 0;
            }
        }
    }

    /// Advance every column's state machine and paint its brick and stack.
    void tick() MM_NONBLOCKING override {
        frameScale_ = static_cast<float>(fallTime_.advance(elapsed())) /
                      static_cast<float>(particles::FrameTime::kOne);
        if (!drops_) return;

        const lengthType w = width();
        const lengthType h = height();

        const draw::Canvas cv = canvas();

        const uint32_t now = platform::millis();
        const RGB black{0, 0, 0};
        // The Layer holds last frame, and this effect writes only its own columns, so own the ground.
        draw::fill(cv, black);

        // The live column count, never the allocated max, so a shrink before prepare is safe.
        const nrOfLightsType dropCount = static_cast<nrOfLightsType>(drops_.count());
        const nrOfLightsType nrOfDrops = (static_cast<nrOfLightsType>(w) < dropCount)
                                             ? static_cast<nrOfLightsType>(w) : dropCount;

        for (nrOfLightsType x = 0; x < nrOfDrops; x++) {
            Tetris& d = drops_[x];

            if (d.step == 0) {
                // Idle, so spawn a brick at the control's speed, or a random one when 0.
                const uint8_t in = speedControl ? speedControl : rng_.below(1, 255);
                // A descending map, so the 250 to 40000 result needs a wide type rather than a byte.
                const long mapped = mapRange(in, 1, 255, 40000, 250);
                d.speed = (mapped > 0) ? (static_cast<float>(h) * FRAMETIME) / static_cast<float>(mapped)
                                       : 1.0f;
                d.pos   = static_cast<float>(h);             // start above the top, fall downward
                if (!oneColor) d.col = static_cast<uint8_t>(rng_.below(0, 15) << 4);
                d.step  = 1;
                d.roll  = 0.0f;
                // Brick height from the control, or random 1 to 4, scaled up on tall grids.
                d.brick = static_cast<uint16_t>((widthControl ? ((widthControl >> 5) + 1)
                                                              : rng_.below(1, 5))
                                                * (1 + (h >> 6)));
            } else if (d.step == 1) {
                // Roll once per reference frame, not per frame, or the pause scales with the frame rate.
                d.roll += frameScale_;
                while (d.roll >= 1.0f && d.step == 1) {
                    d.roll -= 1.0f;
                    if (rng_.next8() >> 6) d.step = 2;
                }
            } else if (d.step == 2) {
                // Falling: descend until the brick head reaches the top of the stack.
                if (d.pos > static_cast<float>(d.stack)) {
                    // `speed` is calibrated against a 25 ms frame, so scale it by the elapsed fraction.
                    d.pos -= d.speed * frameScale_;
                    if (d.pos < static_cast<float>(d.stack)) d.pos = static_cast<float>(d.stack);
                    // Rows [pos, pos+brick) take the column's color, and everything above is black.
                    for (lengthType i = static_cast<lengthType>(d.pos); i < h; i++) {
                        const RGB c = (i < static_cast<lengthType>(d.pos) + static_cast<lengthType>(d.brick))
                                          ? colorFromPalette(*Palettes::active(), d.col)
                                          : black;
                        draw::pixel(cv, {static_cast<lengthType>(x),
                                                static_cast<lengthType>(h - 1 - i), 0}, c);
                    }
                } else {
                    // Landed: grow the stack, then blank-delay a full column or idle for the next brick.
                    d.step = 0;
                    d.stack = static_cast<uint16_t>(d.stack + d.brick);
                    if (d.stack >= static_cast<uint16_t>(h)) d.step = now + 2000;
                }
            } else {
                // Full and waiting to blank, the compare being the wrap-safe signed difference.
                d.brick = 0;
                if (static_cast<int32_t>(d.step - now) > 0) {
                    for (lengthType i = 0; i < h; i++)
                        draw::blendPixel(cv, {static_cast<lengthType>(x), i, 0}, black, 25);
                } else {
                    d.stack = 0;
                    d.step  = 0;
                    if (oneColor) d.col = static_cast<uint8_t>(d.col + 8);
                }
            }
        }
    }

private:
    /// MoonLight's 25 ms frame, the step the fall speed is calibrated against.
    static constexpr int FRAMETIME = 1000 / 40;

    /// FastLED's integer map, used with a descending range so the result falls as speed rises.
    static long mapRange(long x, long inMin, long inMax, long outMin, long outMax) {
        const long den = inMax - inMin;
        if (den == 0) return outMin;
        return (x - inMin) * (outMax - outMin) / den + outMin;
    }

    // The *Control suffix keeps these from shadowing the inherited width() and depth() accessors.
    ScratchBuffer<Tetris> drops_{*this};   ///< one drop per X column
    Random8        rng_;

    particles::FrameTime fallTime_{40};   ///< MoonLight's reference rate
    float frameScale_ = 1.0f;             ///< this frame's share of a reference frame
};

} // namespace mm