#pragma once

#include "light/effects/EffectBase.h"
#include "light/powerfunctions/particles.h"   // FrameTime: the shared time scale

namespace mm {

/// Physics effect: gravity-bounced balls trailing along the layer.
/// @card BouncingBallsEffect.gif
/// Author: Andrew Tuline (WLED-SR), via the predecessor, https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
///
/// One column of balls per x, each launched upward and bouncing with energy loss.
/// A ball is relaunched once its velocity dies out.
/// Every column runs independently, so a panel shows a forest of bouncing dots.
///
/// Prior art: MoonLight's BouncingBalls, generalizing WLED's own from the FastLED demo lineage.
///
/// @moreinfo
///
/// ## The motion is analytic, not integrated
///
/// A ball's height is the projectile equation evaluated against its own clock.
/// So the trajectory is real physics rather than a frame-step integration that drifts.
/// Only `impactVelocity` and `lastBounceTime` carry between frames, and height is recomputed.
///
/// Reproduced from the source: its gravity, time scale, dampening, kick and palette spacing.
class BouncingBallsEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, WLED lineage.
    const char* tags() const override { return "💫🐙"; }
    /// Writes the z=0 slice, one ball column per x, which extrude duplicates through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// The per-column ceiling, and the stride of the flattened ball array.
    static constexpr uint8_t maxNumBalls = 16;

    /// Gravity's strength, where higher falls faster.
    uint8_t grav     = 128;
    /// How many balls each column carries.
    uint8_t numBalls = 8;

    /// Publish gravity and the ball count.
    void defineControls() override {
        controls_.addControl("grav", grav, 0, 255);
        controls_.addControl("numBalls", numBalls, 1, maxNumBalls);
    }

    /// Size one ball array per column, flattened, which zero-fills into a first-frame bounce.
    void prepare() override {
        const size_t cols = static_cast<size_t>(width() > 0 ? width() : 0);
        balls_.resize(cols * maxNumBalls);
    }

    /// Advance every ball on its own clock, bouncing and relaunching, and draw each one.
    void tick() MM_NONBLOCKING override {
        if (!balls_) return;

        const int cols = width();
        const int rows = height();

        const draw::Canvas cv = canvas();

        // The Layer takes a rate and scales it by the elapsed frame, so the tail holds its length.
        layer()->fadeToBlackBy(100);

        constexpr float gravity = -9.81f;
        const uint32_t time = elapsed();

        // Clamped to the array bound, and this count also divides the dampening and the palette.
        const int nBalls = numBalls < maxNumBalls ? numBalls : maxNumBalls;
        if (nBalls <= 0) return;

        // Divides the elapsed time, so a higher `grav` gives a shorter and faster fall.
        const uint32_t timeScale = static_cast<uint32_t>((255 - grav) / 64 + 1);

        for (int x = 0; x < cols; x++) {
            Ball* column = balls_.data() + static_cast<size_t>(x) * maxNumBalls;
            for (int i = 0; i < nBalls; i++) {
                Ball& ball = column[i];

                // Divided in float: the integer form truncates to zero and freezes the ball.
                const float timeSinceLastBounce =
                    static_cast<float>(time - ball.lastBounceTime) / static_cast<float>(timeScale);
                const float timeSec = timeSinceLastBounce / 1000.0f;
                float height = (0.5f * gravity * timeSec + ball.impactVelocity) * timeSec;

                if (height <= 0.0f) {
                    // On the floor: lose energy by a dampening that grows with the ball's index.
                    height = 0.0f;
                    const float dampening = 0.9f - static_cast<float>(i) / static_cast<float>(nBalls * nBalls);
                    ball.impactVelocity = dampening * ball.impactVelocity;
                    ball.lastBounceTime = time;

                    if (ball.impactVelocity < 0.015f) {
                        // Energy spent, so relaunch with a fresh kick at half to full strength.
                        ball.impactVelocity = std::sqrt(-2.0f * gravity) * static_cast<float>(rng_.below(5, 11)) / 10.0f;
                    }
                } else if (height > 1.0f) {
                    continue;  // off the top of the column this frame, so draw nothing
                }

                // The height mapped onto the column, where 0 is the bottom row.
                const int pos = (rows - 1) - static_cast<int>(lroundf(height * static_cast<float>(rows - 1)));

                // Each ball takes a slice of the palette, floored at 8 so a small count still spreads.
                const int paletteDiv = nBalls > 8 ? nBalls : 8;
                const uint8_t index = static_cast<uint8_t>(i * (256 / paletteDiv));
                const RGB color = colorFromPalette(*Palettes::active(), index);

                draw::pixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(pos), 0}, color);
            }
        }
    }

private:
    /// One ball's persistent state, height being recomputed analytically each frame.
    struct Ball {
        float impactVelocity = 0.0f;   ///< its speed leaving the last bounce
        uint32_t lastBounceTime = 0;   ///< when that bounce happened, which starts its clock
    };

    ScratchBuffer<Ball> balls_{*this};   ///< every column's balls, flattened by `maxNumBalls`
    Random8 rng_;                        ///< the relaunch kick's randomness
};

} // namespace mm
