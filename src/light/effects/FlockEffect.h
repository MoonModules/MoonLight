#pragma once

#include "light/effects/EffectBase.h"
#include "light/powerfunctions/boids.h"   // the steering and the trail

#if MM_HEAVY_COMPUTE   // only where the SoC declares a hardware FPU, for the reason boids.h carries

namespace mm {

/// A flock of boids that steer by their neighbors and leave glowing trails, with one predator they flee.
/// @card FlockEffect.gif
///
/// Each boid steers toward the heading of the boids near it, toward their center, and away from any that crowd it. The predator hunts the flock, and a rare gust of wind nudges a boid off course.
///
/// Compare with @ref MoonFlockEffect, the same flock with a boid for each piano key.
///
/// @attribution James Hutchinson, Claude, Craig Reynolds, Daniel Shiffman, Jason Coon
///
/// @technical
///
/// ## After Aurora's Flock
///
/// A flock in the spirit of Jason Coon's Aurora Flock, written afresh on Craig Reynolds' boids as [boids.h](../../../src/light/powerfunctions/boids.h) steers them, with Aurora's numbers.
///
/// Two choices follow Marc Merlin's FastLED port of it rather than Aurora: a third of the grid's width flies, where Aurora flies 10 and his port at most 40, and the predator is always there, where Aurora shows one on a coin toss and flips it every 30 seconds.
///
/// The flock and the predator start anywhere, heading anywhere. The flock draws from the active palette, as Aurora's does, moving one entry every 12 steps, so the colors run faster with `speed` and loudness, where Aurora's move by wall time.
///
/// `speed` sets the steps a second and loudness adds to it, so the flock keeps its behavior at any pace, and the trail, which fades per step, keeps its length. Room for the most boids is held from the prepare, so turning `count` up takes no allocation on the tick.
class FlockEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, and reacting to how loud the room is.
    const char* tags() const override { return "💫🎵"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How many boids fly, where 0 is a third of the width.
    uint8_t count = 0;
    /// How many lights wide a boid is drawn, where 0 sizes it to the grid.
    uint8_t size = 0;
    /// Steps a second, where 60 is the reference and 0 holds the flock still.
    uint8_t speed = boids::kReferenceHz;
    /// How much loudness speeds the flock, up to double `speed`.
    uint8_t audioGain = 200;

    /// Publish the population, how big each boid is drawn, and how fast the flock flies.
    void defineControls() override {
        controls_.addControl("count", count, 0, kMaxBoids);
        controls_.addControl("size", size, 0, boids::kMaxSize);
        controls_.addControl("speed", speed, 0, 255);
        controls_.addControl("audioGain", audioGain, 0, 255);
    }

    /// Size and clear the trail, hold room for the most boids, and seed the predator.
    void prepare() override {
        if (!flock_.prepare(*this, width(), height(), channelsPerLight(), kMaxBoids)) return;
        rng_ = Random8{boids::kSeed};                      // every prepare starts the same flight
        boids::seed(predator_, rng_);
        filled_ = 0;
        steps_ = 0;
        clock_.reset();
    }

    /// Run the steps this tick owes within its time, copy the trail's colors to the layer, and draw the flock over them as it is now.
    void tick() MM_NONBLOCKING override {
        const std::optional<boids::Plane> trail = flock_.plane(width(), height(), channelsPerLight());
        if (!trail) return;   // a resize outran the prepare: the frame waits for it

        const uint8_t n = flockSize(trail->w());
        // Tracking the size rather than a one-shot flag: turning `count` up seeds new boids, and down lets the rest go.
        while (filled_ < n) boids::seed(flock_.boids[filled_++], rng_);
        filled_ = n < filled_ ? n : filled_;

        const float r = boids::radius(size, trail->w(), trail->h());
        boids::runSteps(clock_.steps(elapsed(), boids::pace(speed, audioGain)), [&] { step(*trail, n, r); });
        const boids::Plane layer{canvas()};
        trail->copyColorsTo(layer.cv);
        for (uint8_t i = 0; i < n; i++) layer.head(flock_.boids[i].x, flock_.boids[i].y, flockColor(), r);
        layer.head(predator_.x, predator_.y, predatorColor(), r);
    }

private:
    /// The flock's most boids, which is also what `count` accepts.
    static constexpr uint8_t kMaxBoids = 255;
    /// The palette moves one entry every this many steps.
    static constexpr uint8_t kHueEvery = 12;

    boids::Flock flock_{*this};             ///< room for the most boids, and the trail
    boids::Boid predator_;
    uint8_t filled_ = 0;                    ///< how many boids exist, which `count` can move
    uint32_t steps_ = 0;                    ///< steps taken, which time the palette
    boids::StepClock clock_;
    Random8 rng_{boids::kSeed};

    /// How many boids fly: @xref{after-auroras-flock|a third of the width}, unless `count` says.
    uint8_t flockSize(lengthType w) const {
        if (count) return count;
        const lengthType third = w / 3;
        return third < 1 ? 1 : third > kMaxBoids ? kMaxBoids : static_cast<uint8_t>(third);
    }

    /// The flock's color now, and the predator's, opposite it on the palette.
    RGB flockColor() const { return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(steps_ / kHueEvery), 255); }
    RGB predatorColor() const { return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(steps_ / kHueEvery + 128), 255); }

    /// One step: every boid's and the predator's forces gathered from where they all are, the wind, then the trail fades and all of them move, each leaving its streak.
    void step(const boids::Plane& trail, uint8_t n, float r) {
        steps_++;
        const std::span<boids::Boid> flock(flock_.boids.data(), n);
        for (uint8_t i = 0; i < n; i++) boids::gather(flock[i], boids::kBoid, flock, i, &predator_);
        boids::gather(predator_, boids::kPredator, flock, boids::kNone, nullptr);
        boids::gust(flock, rng_);

        trail.fade();
        const RGB color = flockColor();
        for (boids::Boid& b : flock) boids::moveTraced(b, boids::kBoid.maxSpeed, trail, color, r);
        boids::moveTraced(predator_, boids::kPredator.maxSpeed, trail, predatorColor(), r);
    }
};

}  // namespace mm

#endif  // MM_HEAVY_COMPUTE
