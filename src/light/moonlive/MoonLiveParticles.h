#pragma once

#include "core/module/MoonModule.h"
#include "core/util/ScratchBuffer.h"
#include "light/powerfunctions/particles.h"

namespace mm::moonlive {

/// One scripted module's particle pool, with the frame clock that makes the physics run evenly.
///
/// @moreinfo
///
/// ## The pool lives outside the script arena
///
/// A pool is eight parallel arrays, where the script arena is 64 bytes across 8 members.
/// A script could hold about five particles there, against the hundreds a particle look needs.
/// Widening the arena is the wrong answer, since every scripted module holds one by value.
/// A probe on the main task's stack boot-looped the P4 at 1440 bytes.
///
/// Six buffers rather than eight: `acc` and `size` are optional and neither feeds a builtin.
class MoonLiveParticles {
public:
    explicit MoonLiveParticles(MoonModule& owner)
        : x_(owner), y_(owner), vx_(owner), vy_(owner), ttl_(owner), hue_(owner) {}

    // A failed resize leaves `valid()` false rather than a stale pool naming freed memory.
    /// Size the pool to `count` particles, returning the count available and 0 when allocation fails.
    uint16_t resize(uint16_t count) {
        if (count == 0) { release(); return 0; }
        const bool ok = x_.resize(count) && y_.resize(count) && vx_.resize(count) &&
                        vy_.resize(count) && ttl_.resize(count) && hue_.resize(count);
        if (!ok) { release(); return 0; }
        pool_ = particles::Pool{};
        pool_.x = x_.data(); pool_.y = y_.data();
        pool_.vx = vx_.data(); pool_.vy = vy_.data();
        pool_.ttl = ttl_.data(); pool_.hue = hue_.data();
        pool_.count = count;
        pool_.clear();
        time_.reset();
        return count;
    }

    // Called before the binding chains to the base, whose free-list walk would leave these dangling.
    /// Free every buffer and leave the pool invalid.
    void release() {
        x_.resize(0); y_.resize(0); vx_.resize(0); vy_.resize(0); ttl_.resize(0); hue_.resize(0);
        pool_ = particles::Pool{};
        time_.reset();
    }

    /// The particle pool the script's particle calls act on.
    particles::Pool& pool() MM_NONBLOCKING { return pool_; }
    /// The number of live particles.
    uint16_t count() const MM_NONBLOCKING { return pool_.count; }

    // Every per-frame builtin passes this on, so framerate independence is the system's property.
    /// How much of a reference frame this frame covered, in 8.8 fixed point.
    uint32_t advance(uint32_t nowMs) MM_NONBLOCKING { return time_.advance(nowMs); }

private:
    ScratchBuffer<draw::pos_t> x_, y_, vx_, vy_;
    ScratchBuffer<uint16_t>    ttl_;
    ScratchBuffer<uint8_t>     hue_;
    particles::Pool            pool_;
    particles::FrameTime       time_{60};
};

}  // namespace mm::moonlive
