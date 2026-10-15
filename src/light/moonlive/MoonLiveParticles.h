#pragma once

#include "core/module/MoonModule.h"
#include "core/util/ScratchBuffer.h"
#include "light/powerfunctions/particles.h"

namespace mm::moonlive {

/// One scripted module's particle pool, with the frame clock that makes the physics run evenly.
///
/// @technical
///
/// ## The pool lives outside the script arena
///
/// A pool is eight parallel arrays, while the script arena is 64 bytes over 8 members, about five particles. Widening the arena is wrong: every scripted module holds one by value, and a 1440-byte probe on the main stack would boot-loop the P4.
///
/// Six buffers rather than eight: `acc` and `size` are optional and neither feeds a builtin.
class MoonLiveParticles {
public:
    explicit MoonLiveParticles(MoonModule& owner)
        : x_(owner), y_(owner), vx_(owner), vy_(owner), ttl_(owner), hue_(owner) {}

    /// Size the pool to `count` particles, returning the count available and 0 when allocation fails, which leaves `valid()` false rather than a stale pool naming freed memory.
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

    /// Free every buffer and leave the pool invalid; called before the binding chains to the base, whose free-list walk would leave these dangling.
    void release() {
        x_.resize(0); y_.resize(0); vx_.resize(0); vy_.resize(0); ttl_.resize(0); hue_.resize(0);
        pool_ = particles::Pool{};
        time_.reset();
    }

    /// The particle pool the script's particle calls act on.
    particles::Pool& pool() MM_NONBLOCKING { return pool_; }
    /// The number of live particles.
    uint16_t count() const MM_NONBLOCKING { return pool_.count; }

    /// How much of a reference frame this frame covered, in 8.8 fixed point, which every per-frame builtin passes on so framerate independence is the system's property.
    uint32_t advance(uint32_t nowMs) MM_NONBLOCKING { return time_.advance(nowMs); }

private:
    ScratchBuffer<draw::pos_t> x_, y_, vx_, vy_;
    ScratchBuffer<uint16_t>    ttl_;
    ScratchBuffer<uint8_t>     hue_;
    particles::Pool            pool_;
    particles::FrameTime       time_{60};
};

}  // namespace mm::moonlive
