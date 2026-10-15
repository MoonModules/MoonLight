#pragma once

#include "core/util/math16.h"              // hashInt
#include "light/effects/EffectBase.h"
#include "light/powerfunctions/particles.h"

namespace mm {

/// Falling balls that pile up and shove each other aside.
/// @card BallpitEffect.gif
///
/// The heap is emergent: gravity pulls, the floor stops, and contact between neighbors makes the shape. `tilt` turns the pit into a slope and the pile slides and re-settles.
///
/// @attribution Damian Schneider
///
/// @technical
///
/// Collisions are the one non-linear part of the particle kernel, so the pool is small. `collide` runs once a frame before the move, and nearly every pair rejects on one axis alone.
class BallpitEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫✨"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How many balls share the pit.
    uint8_t balls    = 30;
    /// How hard they fall.
    uint8_t gravity  = 8;
    /// Contact radius in pixels: how far apart balls sit when touching.
    uint8_t size     = 2;
    /// How much speed a contact keeps.
    uint8_t bounce   = 120;
    /// Sideways force, turning the pit into a slope.
    uint8_t tilt     = 0;
    /// Damping, so the heap settles instead of sloshing.
    uint8_t drag     = 6;

    /// Publish the population, the physics and the slope.
    void defineControls() override {
        controls_.addControl("balls", balls, 1, kPool);
        controls_.addControl("gravity", gravity, 0, 64);
        controls_.addControl("size", size, 1, 8);
        controls_.addControl("bounce", bounce, 0, 255);
        controls_.addControl("tilt", tilt, 0, 255);
        controls_.addControl("drag", drag, 0, 64);
    }

    /// Size the pool's storage and wire the view over it.
    void prepare() override {
        x_.resize(kPool); y_.resize(kPool); vx_.resize(kPool); vy_.resize(kPool);
        ttl_.resize(kPool); hue_.resize(kPool); acc_.resize(kPool);
        if (x_ && y_ && vx_ && vy_ && ttl_ && hue_ && acc_) {
            pool_ = particles::Pool{};
            pool_.x = x_.data(); pool_.y = y_.data();
            pool_.vx = vx_.data(); pool_.vy = vy_.data();
            pool_.ttl = ttl_.data(); pool_.hue = hue_.data(); pool_.acc = acc_.data();
            pool_.count = kPool;
            pool_.clear();
        } else {
            pool_ = particles::Pool{};   // a failed resize leaves valid() false, not a stale pool
        }
        time_.reset();
        filledCount_ = 0;
    }

    /// Fill the pit when the count changes, run the physics, then draw each ball as a disc.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        if (w < 2 || h < 2 || !pool_.valid()) return;

        const draw::pos_t wSub = draw::toSub(w - 1);
        const draw::pos_t hSub = draw::toSub(h - 1);

        // Tracking the count rather than a one-shot flag, so turning `balls` up adds balls.
        const uint8_t n = balls < kPool ? balls : kPool;
        if (n != filledCount_) {
            pool_.clear();
            for (uint8_t i = 0; i < n; i++)
                pool_.spawn(static_cast<draw::pos_t>(hashInt(i, 1, 0, kSeed) % static_cast<uint32_t>(wSub ? wSub : 1)),
                            static_cast<draw::pos_t>(hashInt(i, 2, 0, kSeed) % static_cast<uint32_t>(hSub ? hSub : 1)),
                            0, 0, 255, static_cast<uint8_t>(hashInt(i, 3, 0, kSeed)));
            filledCount_ = n;
        }

        draw::fill(cv, RGB{0, 0, 0});

        // Every frame simulates its own share, so the pit behaves the same at any frame rate.
        const uint32_t scale = time_.advance(elapsed());
        if (scale > 0) {
            pool_.gravity(static_cast<draw::pos_t>(gravity), scale);
            if (tilt) pool_.force(static_cast<draw::pos_t>(tilt) / 8, 0, scale);
            pool_.drag(drag, scale);
            // Contacts resolve first: a shove after the move pushes a ball through a checked wall.
            pool_.collide(draw::toSub(size), bounce, frame_++);
            pool_.step(scale);
            pool_.bounce(wSub, hSub, bounce);
        }

        // Discs rather than points, since two touching points read as one pixel.
        for (uint16_t i = 0; i < pool_.count; i++) {
            if (!pool_.ttl[i]) continue;
            draw::fillCircle(cv, static_cast<lengthType>(draw::toPixel(pool_.x[i])),
                             static_cast<lengthType>(draw::toPixel(pool_.y[i])),
                             static_cast<lengthType>(size),
                             colorFromPalette(*Palettes::active(), pool_.hue[i], 255));
        }
    }

private:
    /// The pool's ceiling, kept small because collisions cost pair checks.
    static constexpr uint8_t kPool = 40;
    static constexpr uint32_t kSeed = 0xBA11;   ///< fixed, so the goldens reproduce

    ScratchBuffer<draw::pos_t> x_{*this}, y_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};   ///< wide, for a life beyond 255 frames
    ScratchBuffer<uint8_t> hue_{*this}, acc_{*this};   ///< each ball's color and contact accumulator
    particles::Pool pool_;                 ///< the view over those arrays
    particles::FrameTime time_{60};        ///< the reference rate the physics is written against
    uint32_t frame_ = 0;                   ///< passed to collide, so its pair order varies
    uint8_t filledCount_ = 0;              ///< the `balls` value the pit currently holds
};

}  // namespace mm
