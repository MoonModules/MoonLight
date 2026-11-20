#pragma once

#include "light/effects/EffectBase.h"
#include "core/util/math16.h"                // hashInt: each ball's kick and phase

#include <cmath>

namespace mm {

/// A row of balls per column bounce under gravity, each losing energy on impact and relaunching when it stops.
/// @card BouncingBallsEffect.gif
///
/// Every column runs independently, so a panel shows a forest of bouncing dots, palette-colored by ball index over a fading background.
///
/// @attribution Andrew Tuline
///
/// @technical
///
/// A ball is a function of its column, its index and the time, so every device of a wall draws one column alike. Its kick and its phase are a hash of column and index.
///
/// One relaunch cycle is the bounces until its speed falls below 0.015: each bounce lasts 2v/g and the dampening k scales v, so bounce n starts at span (1 - k^n), span being bounce0 / (1 - k).
///
/// A ball's kick, span, cycle and phase are worked out once, and each frame steps the bounce on from where it was, a modulo and a few multiplies per ball. k^n is carried as a product of k's from 1, so a device that joins late arrives at the same value as one that ran all along.
class BouncingBallsEffect : public EffectBase {
public:
    /// Catalog tags: WLED origin, and a part of one picture in a wall.
    const char* tags() const override { return "🐙🧩"; }
    /// Writes the z=0 slice, one ball column per x, which extrude duplicates through a volume.
    Dim dimensions() const override { return Dim::D2; }
    /// A ball's height is a function of its column and the time, so each device of a wall draws its part of one picture.
    Footprint footprint() const MM_NONBLOCKING override { return Footprint::Region; }

    /// The per-column ceiling, and the stride of the flattened ball array.
    static constexpr uint8_t maxNumBalls = 16;

    /// Gravity strength: higher falls faster and bounces snappier.
    uint8_t grav     = 128;
    /// Balls per column.
    uint8_t numBalls = 8;

    /// Publish gravity and the ball count.
    void defineControls() override {
        controls_.addControl("grav", grav, 0, 255);
        controls_.addControl("numBalls", numBalls, 1, maxNumBalls);
    }

    /// Size one ball array per column, flattened, worked out again on the first frame.
    void prepare() override {
        const size_t cols = static_cast<size_t>(width() > 0 ? width() : 0);
        balls_.resize(cols * maxNumBalls);
        key_ = Key{};
    }

    /// Place every ball of each of this device's columns on its arc at this moment, and draw it.
    void tick() MM_NONBLOCKING override {
        if (!balls_) return;
        const draw::Canvas cv = canvas();
        const Coord3D whole = cv.extent();

        // The Layer takes a rate and scales it by the elapsed frame, so the tail holds its length.
        layer()->fadeToBlackBy(100);

        // Clamped to the array bound, and this count also divides the dampening and the palette.
        const int nBalls = numBalls < maxNumBalls ? numBalls : maxNumBalls;
        if (nBalls <= 0 || whole.y <= 0) return;

        // Divides the elapsed time, so a higher `grav` gives a shorter and faster fall.
        const uint32_t timeScale = static_cast<uint32_t>((255 - grav) / 64 + 1);
        const Key key{nBalls, timeScale, cv.origin.x};
        if (!(key == key_)) { cycles(cv, key); key_ = key; }

        const uint32_t ms = elapsed();
        const float perMs = 1.0f / (1000.0f * static_cast<float>(timeScale));   // seconds of the ball's clock per millisecond
        const float stepK = 1.0f / static_cast<float>(nBalls * nBalls);
        const Palette& pal = *Palettes::active();
        const int paletteDiv = nBalls > 8 ? nBalls : 8;   // a slice of the palette per ball, floored so a small count still spreads
        for (lengthType x = 0; x < cv.dims.x; x++) {
            const lengthType wx = static_cast<lengthType>(x + cv.origin.x);
            Ball* column = balls_.data() + static_cast<size_t>(x) * maxNumBalls;
            for (int i = 0; i < nBalls; i++) {
                const float height = heightAt(column[i], 0.9f - static_cast<float>(i) * stepK, ms, perMs);
                if (height > 1.0f) continue;   // off the top of the column this frame
                const lengthType row = static_cast<lengthType>((whole.y - 1) - static_cast<int>(lroundf(height * static_cast<float>(whole.y - 1))));
                draw::pixel(cv, cv.local(wx, row), colorFromPalette(pal, static_cast<uint8_t>(i * (256 / paletteDiv))));
            }
        }
    }

private:
    static constexpr float kGravity = 9.81f;   ///< in heights per second squared, a full-height arc being a kick of sqrt(2g)
    static constexpr float kRest = 0.015f;     ///< the speed below which a ball relaunches

    /// One ball's cycle, worked out once, and the bounce it was in last frame.
    struct Ball {
        uint32_t cycleMs = 1;   ///< its relaunch cycle, in milliseconds of fleet time
        uint32_t phaseMs = 0;   ///< where in the cycle it was at time zero
        float v0 = 0.0f;        ///< its kick
        float span = 0.0f;      ///< bounce0 / (1 - k), so bounce n starts at span (1 - k^n)
        float kn = 1.0f;        ///< k^n for the bounce under way, a product of k's from 1
        uint8_t n = 0;          ///< the bounce under way
        uint8_t bounces = 1;    ///< the bounces in a cycle
    };
    /// What the cycles depend on beyond the column and the index.
    struct Key {
        int nBalls = 0;
        uint32_t timeScale = 0;
        lengthType originX = -1;
        bool operator==(const Key& o) const { return nBalls == o.nBalls && timeScale == o.timeScale && originX == o.originX; }
    };

    /// Work out every ball's cycle for this device's columns: its kick, half to full strength in tenths as a relaunch drew it, and its dampening, which grows with the index.
    void cycles(const draw::Canvas& cv, const Key& key) {
        for (lengthType x = 0; x < cv.dims.x; x++) {
            const uint16_t wx = static_cast<uint16_t>(x + cv.origin.x);
            for (int i = 0; i < key.nBalls; i++) {
                const float v0 = std::sqrt(2.0f * kGravity) * static_cast<float>(5 + hashInt(wx, static_cast<uint16_t>(i), 0) % 6) / 10.0f;
                const float k = 0.9f - static_cast<float>(i) / static_cast<float>(key.nBalls * key.nBalls);
                const float span = 2.0f * v0 / kGravity / (1.0f - k);
                const int bounces = static_cast<int>(std::ceil(std::log(kRest / v0) / std::log(k)));
                const float cycle = span * (1.0f - std::pow(k, static_cast<float>(bounces)));
                const uint32_t cycleMs = static_cast<uint32_t>(cycle * 1000.0f * static_cast<float>(key.timeScale)) + 1;
                balls_[static_cast<size_t>(x) * maxNumBalls + i] =
                    Ball{cycleMs, hashInt(wx, static_cast<uint16_t>(i), 1) % cycleMs, v0, span, 1.0f, 0, static_cast<uint8_t>(bounces < 255 ? bounces : 255)};
            }
        }
    }

    /// Ball `b` with dampening `k` at `ms`, in column heights above the floor, its bounce stepped on from last frame.
    static float heightAt(Ball& b, float k, uint32_t ms, float perMs) MM_NONBLOCKING {
        const float t = static_cast<float>((ms + b.phaseMs) % b.cycleMs) * perMs;
        if (t < b.span * (1.0f - b.kn)) { b.n = 0; b.kn = 1.0f; }   // the cycle came round
        while (b.n + 1 < b.bounces && t >= b.span * (1.0f - b.kn * k)) { b.kn *= k; b.n++; }
        const float since = t - b.span * (1.0f - b.kn);
        const float h = (b.v0 * b.kn - 0.5f * kGravity * since) * since;
        return h > 0.0f ? h : 0.0f;
    }

    ScratchBuffer<Ball> balls_{*this};   ///< every column's balls, flattened by `maxNumBalls`
    Key key_;                            ///< what the cycles in `balls_` were worked out for
};

} // namespace mm
