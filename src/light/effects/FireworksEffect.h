#pragma once

#include "core/util/math16.h"              // BeatPhase, hashInt
#include "light/effects/EffectBase.h"
#include "light/powerfunctions/particles.h"

namespace mm {

/// Shells rise, stall, and burst into sparks that arc over and fall.
/// @card FireworksEffect.gif
///
/// Nothing schedules the apex: the shell decelerates under gravity and bursts when its vertical velocity crosses zero. The physics runs on elapsed time and scales with the grid, so a shell and its sparks follow the same arc at any frame rate and fill any size.
///
/// @attribution Damian Schneider, Particle system
///
/// @technical
///
/// Every stage is one particle kernel call: `spawn` launches, `gravity` slows the rise, `angleEmit` bursts a ring where it stalled, and `age` fades a spark. Trails come from the Layer's own decay rather than a second one in the pool.
///
/// ## Height and tempo apart
///
/// A shell thrown at speed v under gravity g bursts at v² / 2g, so a launch speed and a gravity together set the height, and a strong gravity drops every shell at the floor. So the shell's speed is solved from its burst height, the square root of 2gH: `burstHeight` sets where it bursts and `gravity` how fast it gets there.
///
/// The sparks fly at three fifths of the shell's speed, which grows with the square root of gravity, so a burst opens to the same size at any gravity.
///
/// Gravity, the speeds and the burst all scale with the grid's height, so the same fireworks fill a 16-row grid and a 128-row one.
class FireworksEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫✨"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How often a new shell goes up.
    uint8_t launchRate = 30;
    /// How high a shell bursts, as a share of the grid's height.
    uint8_t burstHeight = 200;
    /// How fast everything rises and falls.
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
        controls_.addControl("burstHeight", burstHeight, 10, 255);
        controls_.addControl("gravity", gravity, 1, 32);
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


        // Gravity in proportion to the grid, so the arcs keep their shape and their tempo at any size: @xref{height-and-tempo-apart}.
        const int32_t scaled = (static_cast<int32_t>(gravity) * h + kRefRows / 2) / kRefRows;
        const draw::pos_t gy = static_cast<draw::pos_t>(scaled > 0 ? scaled : 1);
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
            // The slice's share of each, its remainder carried as the pool carries gravity's, so a fast render's tiny slices lose none.
            const auto sc = [scale](draw::pos_t v, int64_t& carry) {
                carry += static_cast<int64_t>(v) * scale;
                const int64_t whole = carry / particles::FrameTime::kOne;
                carry -= whole * particles::FrameTime::kOne;
                return static_cast<draw::pos_t>(whole);
            };
            shellVy_ = static_cast<draw::pos_t>(shellVy_ + sc(gy, shellCarry_[0]));   // gravity acts on the shell too
            shellY_  = static_cast<draw::pos_t>(shellY_ + sc(shellVy_, shellCarry_[1]));
            shellX_  = static_cast<draw::pos_t>(shellX_ + sc(shellVx_, shellCarry_[2]));
            // Burst when it stops rising, so the apex is where the physics puts it.
            if (shellVy_ >= 0 || shellY_ <= 0) {
                pool_.angleEmit(shellX_, shellY_, 0, static_cast<draw::pos_t>(shellV0_ * 3 / 5),
                                65535, sparks, sparkLife, shellHue_, frame_);
                shellLive_ = false;
            }
        // Over the full range, and in proportion to the time this slice covers, so a fast render rolls more often for a smaller chance each.
        } else if (static_cast<uint32_t>(hashInt(frame_, 0, 0, kSeed)) * 4u < static_cast<uint32_t>(launchRate) * scale) {   // the 16-bit hash against launchRate / 1024 of a reference frame
            // From a point along the floor, thrown up to burst between three quarters and all of `burstHeight`, and a little sideways.
            shellX_  = static_cast<draw::pos_t>(hashInt(frame_, 1, 0, kSeed) % static_cast<uint32_t>(wSub ? wSub : 1));
            shellY_  = hSub;
            const uint32_t rise = static_cast<uint32_t>(hSub) * burstHeight / 255u * (192u + hashInt(frame_, 4, 0, kSeed) % 64u) / 256u;
            shellV0_ = static_cast<draw::pos_t>(isqrt(2u * static_cast<uint32_t>(gy) * rise));
            shellVy_ = -shellV0_;
            shellCarry_[0] = shellCarry_[1] = shellCarry_[2] = 0;
            shellVx_ = static_cast<draw::pos_t>(((static_cast<int32_t>(hashInt(frame_, 2, 0, kSeed) % 33u) - 16) * hSub) / draw::toSub(kRefRows));
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
    /// The grid height `gravity` is written for: on another it pulls in proportion, so the arcs keep their shape.
    static constexpr int32_t kRefRows = 128;


    ScratchBuffer<draw::pos_t> x_{*this}, y_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};   ///< wide, for a life beyond 255 frames
    ScratchBuffer<uint8_t> hue_{*this};    ///< each spark's palette entry
    particles::Pool pool_;                 ///< the view over those arrays
    particles::FrameTime time_{60};        ///< the reference rate the controls are written against

    /// The shell keeps its own state, since bursting is not something the pool models.
    draw::pos_t shellX_ = 0, shellY_ = 0, shellVx_ = 0, shellVy_ = 0;
    draw::pos_t shellV0_ = 0;    ///< the speed it was thrown at, which its burst throws the sparks at three fifths of
    int64_t shellCarry_[3] = {}; ///< the sub-unit remainders of its velocity, height and drift
    uint8_t shellHue_ = 0;       ///< the color its burst will take
    bool shellLive_ = false;     ///< whether a shell is currently rising
    uint32_t frame_ = 0;         ///< hashed for the launch, so two devices agree
};

}  // namespace mm
