#pragma once

#include "light/effects/EffectBase.h"
#include "light/powerfunctions/particles.h"

namespace mm {

/// Particle-system effect with spawned, moving points.
/// @card ParticlesEffect.gif
/// Author: WildCats08 / @Brandon502 (MoonLight), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
///
/// Points drift, bounce off the walls and leave a fading trail behind them.
/// The physics belong to `particles::Pool`, and what stays here is the drift rate and the hue map.
///
/// @moreinfo
///
/// ## Both carries exist for the same reason
///
/// The fade and the motion each spend a fraction of a reference frame per tick.
/// At a high frame rate that fraction truncates to zero, so the remainder accumulates instead.
/// Without it a fade of 240 never decays and the trail turns solid, and a slow `speed` never moves.
class ParticlesEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, David Jupijn and Rising Step.
    const char* tags() const override { return "💫🦅✨"; }
    /// Iterates y and x, so the trail covers the z=0 plane and extrude fills a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// The pool's size, of which `count` are alive at a time.
    static constexpr uint8_t MAX_PARTICLES = 64;

    /// How many particles are alive.
    uint8_t count = 32;
    /// How fast they drift.
    uint8_t speed = 80;
    /// How much of the trail each reference frame keeps.
    uint8_t fade = 240;
    /// Walks every particle's color around the palette.
    uint8_t hue_shift = 0;

    /// Publish the population, the drift, the trail and the palette shift.
    void defineControls() override {
        controls_.addControl("count", count, 1, 255);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("fade", fade, 1, 255);
        controls_.addControl("hue_shift", hue_shift, 0, 255);
    }

    /// Size the trail and the pool, and seed every particle once.
    void prepare() override {
        // The z=0 plane only, since extrude fills a volume from it.
        trail_.resize(static_cast<size_t>(width()) * height() * channelsPerLight());
        // `trail_` is the degenerate-grid gate here: prepare() runs outside Layer::tick's own.
        px_.resize(MAX_PARTICLES); py_.resize(MAX_PARTICLES);
        vx_.resize(MAX_PARTICLES); vy_.resize(MAX_PARTICLES);
        ttl_.resize(MAX_PARTICLES); hue_.resize(MAX_PARTICLES);
        if (px_ && py_ && vx_ && vy_ && ttl_ && hue_) {
            pool_ = particles::Pool{};
            pool_.x = px_.data(); pool_.y = py_.data();
            pool_.vx = vx_.data(); pool_.vy = vy_.data();
            pool_.ttl = ttl_.data(); pool_.hue = hue_.data();
            pool_.count = MAX_PARTICLES;
        } else {
            pool_ = particles::Pool{};   // a failed resize leaves valid() false, not a stale pool
        }
        if (trail_) initParticles();
        time_.reset();
    }

    /// Fade the trail, move the particles, draw them into it, then copy it to the layer.
    void tick() MM_NONBLOCKING override {
        if (!trail_) return;

        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();
        uint8_t* buf = buffer();

        // 1. Fade by the elapsed fraction of a reference frame, which is exponential decay.
        const uint32_t fadeSlice = time_.advance(elapsed());
        if (fadeSlice > 0) {
            // The remainder carries: at a high frame rate a fade of 240 truncates to zero a frame.
            fadeCarry_ += (255u - fade) * fadeSlice;
            const uint32_t lost = fadeCarry_ / particles::FrameTime::kOne;
            if (lost > 0) {
                fadeCarry_ -= lost * particles::FrameTime::kOne;
                const uint8_t keep = static_cast<uint8_t>(lost >= 255 ? 0 : 255 - lost);
                for (size_t i = 0; i < trail_.bytes(); i++) trail_[i] = scale8(trail_[i], keep);
            }
        }

        // 2. Move and draw, where the kernel owns the physics and this owns the drift and the hue.
        if (!pool_.valid()) return;
        const draw::pos_t maxX = draw::toSub(w - 1);
        const draw::pos_t maxY = draw::toSub(h - 1);

        // The tail is parked rather than reallocated when `count` moves.
        const uint8_t live = count < MAX_PARTICLES ? count : MAX_PARTICLES;
        for (uint16_t i = 0; i < MAX_PARTICLES; i++) pool_.ttl[i] = (i < live) ? 255 : 0;

        // One reference frame is scale = speed*4, which the elapsed fraction then scales.
        const uint32_t fs = fadeSlice;
        if (fs > 0) {
            // Carried like the fade: at a high frame rate a slow `speed` divides to zero and stops.
            stepCarry_ += static_cast<uint32_t>(speed) * 4u * fs;
            const uint32_t scale = stepCarry_ / particles::FrameTime::kOne;
            stepCarry_ -= scale * particles::FrameTime::kOne;
            pool_.step(scale);
            pool_.bounce(maxX, maxY, 256);          // 256 is a perfect bounce
        }

        // Into the persistent trail, one whole pixel per particle.
        draw::Canvas trailCv{trail_.data(), trail_.bytes(), {w, h, 1}, cpl};
        for (uint16_t i = 0; i < live; i++) {
            // The active palette, as every other particle effect reads: a raw hsvToRgb ignores it.
            const RGB c = colorFromPalette(*Palettes::active(),
                                           static_cast<uint8_t>(pool_.hue[i] + hue_shift), 255);
            draw::pixel(trailCv, {static_cast<lengthType>(draw::toPixel(pool_.x[i])),
                                  static_cast<lengthType>(draw::toPixel(pool_.y[i])), 0}, c);
        }

        // 3. Copy the trail onto the layer, which cleared its own buffer.
        std::memcpy(buf, trail_.data(), trail_.bytes());
    }

private:
    // The state lives in the kernel's pool rather than a private struct.
    ScratchBuffer<draw::pos_t> px_{*this}, py_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};    ///< which slots are alive
    ScratchBuffer<uint8_t> hue_{*this};     ///< each particle's palette entry
    particles::Pool pool_;                  ///< the view over those arrays
    particles::FrameTime time_{60};         ///< the reference frame rate
    uint32_t fadeCarry_ = 0;   ///< the fade's unspent remainder
    uint32_t stepCarry_ = 0;   ///< the motion's unspent remainder, so a slow speed still moves
    bool initialized_ = false; ///< so the particles seed once rather than on every rebuild
    ScratchBuffer<uint8_t> trail_{*this};   ///< the persistent z=0-plane trail
    Random8 rng_{0xBADF00Du};               ///< the shared PRNG, fixed-seed for the goldens
    /// One random byte, the shape the seeding below wants.
    uint8_t rand8() { return rng_.next8(); }

    /// Scatter every particle across the grid with a random velocity and hue.
    void initParticles() {
        lengthType w = width();
        lengthType h = height();
        if (initialized_) return;
        if (!pool_.valid()) return;
        pool_.clear();
        for (uint16_t i = 0; i < MAX_PARTICLES; i++) {
            int32_t sx = static_cast<int8_t>(rand8()) >> 1;
            int32_t sy = static_cast<int8_t>(rand8()) >> 1;
            if (sx == 0) sx = 1;
            if (sy == 0) sy = 1;
            pool_.x[i] = static_cast<draw::pos_t>((static_cast<uint32_t>(rand8()) * w) >> 8) * draw::kSubOne;
            pool_.y[i] = static_cast<draw::pos_t>((static_cast<uint32_t>(rand8()) * h) >> 8) * draw::kSubOne;
            // Sub-pixel per reference frame: the raw number moves 1/256th of a pixel and freezes.
            pool_.vx[i] = static_cast<draw::pos_t>(sx * 16);
            pool_.vy[i] = static_cast<draw::pos_t>(sy * 16);
            pool_.hue[i] = rand8();
            pool_.ttl[i] = 255;
        }
        initialized_ = true;
    }
};

} // namespace mm
