#pragma once

#include "core/util/math16.h"              // BeatPhase, hashInt
#include "light/effects/EffectBase.h"
#include "light/powerfunctions/particles.h"

namespace mm {

/// Shells rise, stall, and burst into sparks that arc over and fall.
/// @card FireworksEffect.gif
///
/// Nothing schedules the apex: the shell decelerates under gravity and bursts when its vertical velocity crosses zero, so a faster launch bursts higher without a second control. The physics runs on elapsed time, so a shell and its sparks follow the same arc at any frame rate.
///
/// @attribution Damian Schneider, Particle system
///
/// @moreinfo
///
/// Every stage is one particle kernel call: `spawn` launches, `gravity` slows the rise, `angleEmit` bursts a ring where it stalled, and `age` fades a spark. Trails come from the Layer's own decay rather than a second one in the pool.
class FireworksEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫✨"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How often a new shell goes up.
    uint8_t launchRate = 30;
    /// How hard a shell is thrown, and so how high it bursts.
    uint8_t launchSpeed = 150;
    /// How fast everything falls, per 60 Hz of simulated time.
    uint8_t gravity = 6;
    /// How many sparks each burst throws.
    uint8_t sparks = 20;
    /// How long a spark survives.
    uint8_t sparkLife = 200;
    /// Air resistance, which flattens the arc.
    uint8_t drag = 3;
    /// Trail length, which is the Layer's decay rather than the pool's.
    uint8_t fade = 25;

    /// Publish the launch, the physics, the burst and the trail.
    void defineControls() override {
        controls_.addControl("launchRate", launchRate, 1, 255);
        controls_.addControl("launchSpeed", launchSpeed, 10, 255);
        controls_.addControl("gravity", gravity, 1, 128);
        controls_.addControl("sparks", sparks, 1, 64);
        controls_.addControl("sparkLife", sparkLife, 10, 255);
        controls_.addControl("drag", drag, 0, 64);
        controls_.addControl("fade", fade, 1, 255);
    }

    /// Size the spark pool and reset the shell.
    void prepare() override {
        // A shell is tracked apart from the pool, since it bursts rather than dying.
        x_.resize(kPool); y_.resize(kPool); vx_.resize(kPool); vy_.resize(kPool);
        ttl_.resize(kPool); hue_.resize(kPool);
        if (x_ && y_ && vx_ && vy_ && ttl_ && hue_) {
            pool_ = particles::Pool{};
            pool_.x = x_.data(); pool_.y = y_.data();
            pool_.vx = vx_.data(); pool_.vy = vy_.data();
            pool_.ttl = ttl_.data(); pool_.hue = hue_.data();
            pool_.count = kPool;
            pool_.clear();
        }
        shellLive_ = false;
        frame_ = 0;
        time_.reset();
    }

    /// Simulate this frame's slice of time, then draw the sparks and any rising shell.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        if (w < 2 || h < 2 || !pool_.valid()) return;


        const draw::pos_t gy = static_cast<draw::pos_t>(gravity);
        const draw::pos_t wSub = draw::toSub(w - 1);
        const draw::pos_t hSub = draw::toSub(h - 1);

        // Every frame simulates its own fraction, so a fast device gets the same arc more smoothly.
        const uint32_t scale = time_.advance(elapsed());
        if (scale > 0) {
            frame_++;
            simulate(gy, wSub, hSub, scale);
            // The Layer takes a rate and scales it by the elapsed frame, as every fading effect does.
            layer()->fadeToBlackBy(fade);
        }

        pool_.render(cv, sparkLife);

        // One bright point, so a launch reads as a shell rather than as nothing until the burst.
        if (shellLive_)
            draw::splat(cv, shellX_, shellY_, colorFromPalette(*Palettes::active(), shellHue_, 255));
    }

private:
    /// One fixed slice of simulated time: launch, rise, burst, fall, age. No drawing.
    void simulate(draw::pos_t gy, draw::pos_t wSub, draw::pos_t hSub, uint32_t scale) MM_NONBLOCKING {
        // --- The shell ---------------------------------------------------------------------
        if (shellLive_) {
            const auto sc = [scale](draw::pos_t v) {
                return static_cast<draw::pos_t>(particles::scaleSigned(v, static_cast<int32_t>(scale),
                                                                       particles::FrameTime::kOne));
            };
            shellVy_ = static_cast<draw::pos_t>(shellVy_ + sc(gy));   // gravity acts on the shell too
            shellY_  = static_cast<draw::pos_t>(shellY_ + sc(shellVy_));
            shellX_  = static_cast<draw::pos_t>(shellX_ + sc(shellVx_));
            // Burst when it stops rising, so the apex is where the physics puts it.
            if (shellVy_ >= 0 || shellY_ <= 0) {
                pool_.angleEmit(shellX_, shellY_, 0, static_cast<draw::pos_t>(launchSpeed * 2),
                                65535, sparks, sparkLife, shellHue_, frame_);
                shellLive_ = false;
            }
        // Compared over the full range: dividing the rate truncated its slowest settings to never launching.
        } else if ((hashInt(frame_, 0, 0, kSeed) % 1024u) < launchRate) {
            // From a point along the floor, thrown up and a little sideways.
            shellX_  = static_cast<draw::pos_t>(hashInt(frame_, 1, 0, kSeed) % static_cast<uint32_t>(wSub ? wSub : 1));
            shellY_  = hSub;
            shellVy_ = -static_cast<draw::pos_t>(launchSpeed);
            shellVx_ = static_cast<draw::pos_t>((hashInt(frame_, 2, 0, kSeed) % 33u)) - 16;
            shellHue_ = static_cast<uint8_t>(hashInt(frame_, 3, 0, kSeed));
            shellLive_ = true;
        }

        // --- The sparks --------------------------------------------------------------------
        pool_.gravity(gy, scale);
        pool_.drag(drag, scale);
        pool_.step(scale);
        pool_.killOutside(wSub, hSub, draw::toSub(2));
        pool_.age(1, scale);
    }

    /// The spark pool's size, which the default burst count sits well inside.
    static constexpr uint16_t kPool = 120;
    /// Fixed, so two devices launch alike.
    static constexpr uint32_t kSeed = 0xF17E;

    ScratchBuffer<draw::pos_t> x_{*this}, y_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};   ///< wide, for a life beyond 255 frames
    ScratchBuffer<uint8_t> hue_{*this};    ///< each spark's palette entry
    particles::Pool pool_;                 ///< the view over those arrays
    particles::FrameTime time_{60};        ///< the reference rate the controls are written against

    /// The shell keeps its own state, since bursting is not something the pool models.
    draw::pos_t shellX_ = 0, shellY_ = 0, shellVx_ = 0, shellVy_ = 0;
    uint8_t shellHue_ = 0;       ///< the color its burst will take
    bool shellLive_ = false;     ///< whether a shell is currently rising
    uint32_t frame_ = 0;         ///< hashed for the launch, so two devices agree
};

}  // namespace mm
