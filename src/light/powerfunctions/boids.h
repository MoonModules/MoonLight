#pragma once

#include "core/module/MoonModule.h"           // the effect whose status a full memory sets
#include "core/services/AudioService.h"       // latestFrame(): the level `pace` reads
#include "core/util/AudioFrame.h"             // AudioFrame
#include "core/util/ScratchBuffer.h"          // the boids and the trail an effect owns
#include "core/util/color.h"                  // RGB
#include "core/util/math8.h"                  // Random8
#include "light/powerfunctions/draw.h"        // Canvas, fade, blendPixel, pixel, get, bar
#include "light/powerfunctions/particles.h"   // FrameTime
#include "light/util/light_types.h"           // lengthType, and the platform's MM_HEAVY_COMPUTE
#include "platform/platform.h"                // millis(), MM_NONBLOCKING

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

/// Craig Reynolds' boids, and the planes a flock is drawn into: the steering every flocking effect shares.
///
/// @attribution Craig Reynolds, Daniel Shiffman, Jason Coon
///
/// @technical
///
/// ## Steering
///
/// Each boid sums Reynolds' steering behaviors over the boids it can see: separation, alignment, cohesion, and flight from a predator near where it is heading. Each is a steering force as Shiffman's *The Nature of Code* teaches it: the desired velocity at full speed, minus the current one, limited to the boid's force.
///
/// Every boid's forces are gathered from one moment before any boid moves. The space is a torus, seen the short way across an edge.
///
/// The numbers are cited as values on their constants: Shiffman's weights, and Jason Coon's from Aurora's Flock for the rest. The flock lives in Aurora's 32 by 32 space, scaled onto the grid as it is drawn, so it behaves the same on every panel.
///
/// ## Float, deliberately and locally
///
/// Positions and velocities are `float`, a force of 0.015 on a 32-unit field being intrinsically fractional, and every boid tests every other, so this header compiles only where the platform declares a hardware FPU, one of the [bounded exceptions](https://moonmodules.org/MoonLight/moonmodules/light/power-functions.html).
///
/// ## A tick's time
///
/// FrameTime clamps a stalled tick to eight reference frames, Glenn Fiedler's guard in *Fix Your Timestep* against the spiral of death. A step's cost grows with the boids and their size, which on a small chip can outrun a tick, so the steps also stop once a tick has spent `kTickBudgetMs`, after at least one.
///
/// ## Why a private trail plane
///
/// The layer's buffer persists, but live modifiers rewrite it in place each frame, and a flock's trail fades by step where the layer's fades by time. So the streaks live in a plane of their own, faded each step and copied to the layer each frame, and the boids are drawn over them fresh.
#if MM_HEAVY_COMPUTE

namespace mm::boids {

/// The space a flock lives in, Aurora's grid: @xref{steering|the numbers it is written in}.
inline constexpr float kSize = 32.0f;
/// The steps a second the numbers are written for.
inline constexpr uint8_t kReferenceHz = 60;
/// An index past every flock, for a `skip` that passes over every boid.
inline constexpr uint8_t kNone = 255;
/// The widest a `size` control draws a boid, in lights.
inline constexpr uint8_t kMaxSize = 16;
/// The random sequence's start, so every flight from a prepare is the same.
inline constexpr uint32_t kSeed = 0xF10CC5u;
/// What a flocking effect's card says when its trail or its flock does not fit in memory.
inline constexpr const char* kNoMemory = "not enough memory for the flock";

/// One boid, in `float`: @xref{float-deliberately-and-locally|why}.
struct Boid {
    /// Its position in the flock's space.
    float x = 0, y = 0;
    /// Its velocity, in flock units a step.
    float vx = 0, vy = 0;
    /// The acceleration gathered for its next move.
    float ax = 0, ay = 0;
};

/// What a boid and a predator differ in.
struct Kind {
    /// The top speed and the strongest steering force, a step.
    float maxSpeed, maxForce;
    /// How close is crowding, and how far it sees.
    float separation, sight;
};
/// Aurora's numbers: a boid's top speed 0.38 and force 0.015, crowded within 4, seeing 8; a predator's 0.385 and 0.020, seeing 16.
inline constexpr Kind kBoid{0.380f, 0.015f, 4.0f, 8.0f};
inline constexpr Kind kPredator{0.385f, 0.020f, 0.0f, 16.0f};

/// Shiffman's weight for separation, against 1 for alignment and cohesion.
inline constexpr float kSeparationWeight = 1.5f;
/// A boid flees a predator within Aurora's 10 of where the boid is heading.
inline constexpr float kFear = 10.0f;
/// How much harder than its own steering a boid flees: Aurora's seven.
inline constexpr float kFearForce = 7.0f;
/// The wind: on about one step in 64, near Aurora's 4 in 255, one boid is pushed by up to half Aurora's 0.015 either way.
inline constexpr float kGustChance = 1.0f / 64;
inline constexpr float kGust = 0.015f;
/// Each step keeps this much of the trail, out of 255: Aurora's 230.
inline constexpr uint8_t kTrailKeep = 230;
/// A quarter turn, in radians.
inline constexpr float kQuarterTurn = 1.5707964f;

/// The short way from one point to another along an axis of the torus, for points inside it: the minimum image.
inline float across(float d) { return d > kSize / 2 ? d - kSize : d < -kSize / 2 ? d + kSize : d; }

/// Scale (x, y) to `length`, keeping its direction; the zero vector stays as it is.
inline void setLength(float& x, float& y, float length) {
    const float now = std::sqrt(x * x + y * y);
    if (now > 0) { x *= length / now; y *= length / now; }
}

/// Shorten (x, y) to at most `length`, keeping its direction.
inline void clampLength(float& x, float& y, float length) {
    const float now = std::sqrt(x * x + y * y);
    if (now > length) { x *= length / now; y *= length / now; }
}

/// Add to `b`'s acceleration Reynolds' steering force toward (dx, dy): full speed that way, minus its velocity, no stronger than `maxForce`, times `weight`; a force only where there is a direction.
inline void steerToward(Boid& b, float dx, float dy, float maxSpeed, float maxForce, float weight) {
    if (dx == 0 && dy == 0) return;
    setLength(dx, dy, maxSpeed);
    float fx = dx - b.vx, fy = dy - b.vy;
    clampLength(fx, fy, maxForce);
    b.ax += fx * weight;
    b.ay += fy * weight;
}

/// What one boid sees of the others, as offsets the short way across the torus.
struct Neighborhood {
    /// The pushes away from the ones that crowd it, each the harder the closer.
    float awayX = 0, awayY = 0;
    /// The summed heading of the ones in sight, and the summed offset to them.
    float headingX = 0, headingY = 0, towardX = 0, towardY = 0;
    /// How many crowd it, and how many it sees.
    uint16_t crowding = 0, inSight = 0;
};

/// Look from `b`, of kind `k`, at every boid in `all` but itself and the one at `skip`.
inline Neighborhood look(const Boid& b, const Kind& k, std::span<const Boid> all, uint8_t skip) {
    Neighborhood seen;
    const float crowd2 = k.separation * k.separation, sight2 = k.sight * k.sight;
    for (size_t i = 0; i < all.size(); i++) {
        if (i == skip) continue;
        const float dx = across(all[i].x - b.x), dy = across(all[i].y - b.y);
        const float d2 = dx * dx + dy * dy;
        if (d2 == 0) continue;   // itself, or one in the same place, which gives no direction
        if (d2 < crowd2) {
            const float inv = 1.0f / d2;
            seen.awayX -= dx * inv;
            seen.awayY -= dy * inv;
            seen.crowding++;
        }
        if (d2 < sight2) { seen.headingX += all[i].vx; seen.headingY += all[i].vy; seen.towardX += dx; seen.towardY += dy; seen.inSight++; }
    }
    return seen;
}

/// Gather `b`'s acceleration for this step: separation, alignment and cohesion over every boid in `all` but the one at `skip`, and flight from `predator` when one is given.
inline void gather(Boid& b, const Kind& k, std::span<const Boid> all, uint8_t skip, const Boid* predator) {
    const Neighborhood seen = look(b, k, all, skip);
    if (seen.crowding) steerToward(b, seen.awayX, seen.awayY, k.maxSpeed, k.maxForce, kSeparationWeight);
    if (seen.inSight) {
        steerToward(b, seen.headingX, seen.headingY, k.maxSpeed, k.maxForce, 1);   // alignment: the sum points where the average does
        steerToward(b, seen.towardX, seen.towardY, k.maxSpeed, k.maxForce, 1);     // cohesion: toward the center they sit around
    }
    if (!predator) return;
    const float aheadX = across(b.x + b.vx - predator->x), aheadY = across(b.y + b.vy - predator->y);
    if (aheadX * aheadX + aheadY * aheadY <= kFear * kFear)
        steerToward(b, across(b.x - predator->x), across(b.y - predator->y), k.maxSpeed, k.maxForce * kFearForce, 1);
}

/// Spend `b`'s gathered acceleration: add it to the velocity, hold that to `maxSpeed`, move, and come round the torus.
inline void move(Boid& b, float maxSpeed) {
    b.vx += b.ax;
    b.vy += b.ay;
    b.ax = b.ay = 0;
    clampLength(b.vx, b.vy, maxSpeed);
    b.x = std::fmod(b.x + b.vx + kSize, kSize);
    b.y = std::fmod(b.y + b.vy + kSize, kSize);
}

/// A uniform draw from 0 up to 1.
inline float uniform(Random8& rng) { return static_cast<float>(rng.next8()) / 256.0f; }

/// A boid somewhere in the space, heading anywhere at half a boid's top speed.
inline void seed(Boid& b, Random8& rng) {
    b = Boid{};
    b.x = uniform(rng) * kSize;
    b.y = uniform(rng) * kSize;
    const float turn = uniform(rng) * 4 * kQuarterTurn;
    b.vx = std::cos(turn) * kBoid.maxSpeed / 2;
    b.vy = std::sin(turn) * kBoid.maxSpeed / 2;
}

/// The wind: now and then, a push for one boid of `all`.
inline void gust(std::span<Boid> all, Random8& rng) {
    if (all.empty() || uniform(rng) >= kGustChance) return;
    Boid& b = all[rng.next8() % all.size()];
    b.ax += (uniform(rng) - 0.5f) * kGust;
    b.ay += (uniform(rng) - 0.5f) * kGust;
}

/// Steps a second: `speed`, raised by the smoothed level in proportion to `audioGain`, up to twice `speed` at 255.
inline uint32_t pace(uint8_t speed, uint8_t audioGain) MM_NONBLOCKING {
    const AudioFrame* f = AudioService::latestFrame();
    const uint32_t level = f->levelSmoothed > 255 ? 255 : f->levelSmoothed;
    return static_cast<uint32_t>(speed) * (255u * 255u + level * audioGain) / (255u * 255u);
}

/// The time a tick may spend on steps, in milliseconds, after which the steps it still owes are dropped: @xref{a-ticks-time|why a second bound}.
inline constexpr uint32_t kTickBudgetMs = 8;

/// Elapsed time turned into steps at a pace, the part of a step left over kept for the next tick, so the same settings fly the same at any frame rate.
struct StepClock {
    /// The reference frames elapsed, clamped after a stall.
    particles::FrameTime time{kReferenceHz};
    uint32_t owed = 0;   ///< reference frames in 8.8, times steps a second: a step is `kOne * kReferenceHz` of it

    /// Start over, as a prepare does.
    void reset() { time.reset(); owed = 0; }

    /// The steps owed at `now` at `stepsPerSecond`, a stall bounded by FrameTime's clamp.
    uint32_t steps(uint32_t now, uint32_t stepsPerSecond) {
        owed += time.advance(now) * stepsPerSecond;
        constexpr uint32_t kStep = particles::FrameTime::kOne * kReferenceHz;
        const uint32_t n = owed / kStep;
        owed -= n * kStep;
        return n;
    }
};

/// Run `step()` `owed` times, or until the tick has spent `kTickBudgetMs` by the platform's clock, whichever comes first, and at least once when any is owed; returns how many ran.
template <typename Step>
uint32_t runSteps(uint32_t owed, Step step) {
    const uint32_t start = platform::millis();
    uint32_t ran = 0;
    while (ran < owed) {
        step();
        ran++;
        if (platform::millis() - start >= kTickBudgetMs) break;
    }
    return ran;
}

/// How wide a boid is drawn, as a radius in lights: half of `size` when set, else sized to the grid.
inline float radius(uint8_t size, lengthType w, lengthType h) {
    if (size) return size * 0.5f;
    return std::max(0.5f, std::min(static_cast<float>(w), static_cast<float>(h)) / kSize * 0.4f);
}

/// How much of a light a disc with a mouth covers.
struct Cover {
    /// The whole disc's cover, soft at its rim.
    float rim;
    /// The body's: the disc less its mouth, soft at the mouth's edges too.
    float body;
};
/// The cover of the light at (dx, dy) from the center of a disc of `radius`, its mouth `halfAngle` either side of the heading (hx, hy).
inline Cover mouthCovers(float dx, float dy, float hx, float hy, float radius, float halfAngle) {
    const float d = std::sqrt(dx * dx + dy * dy);
    const float rim = std::clamp(radius + 0.5f - d, 0.0f, 1.0f);
    const float heading = std::sqrt(hx * hx + hy * hy);
    if (rim == 0 || d == 0 || heading == 0) return {rim, rim};
    const float theta = std::acos(std::clamp((dx * hx + dy * hy) / (d * heading), -1.0f, 1.0f));
    // How far the light is past the mouth's edge, in lights: negative inside the mouth.
    const float past = d * std::sin(std::min(theta - halfAngle, kQuarterTurn));
    return {rim, rim * std::clamp(past + 0.5f, 0.0f, 1.0f)};
}

/// A canvas a flock draws on, with the flock's space scaled onto it: the trail, kept @xref{why-a-private-trail-plane|apart from the layer}, or the layer itself, for what is drawn fresh each frame. A light's center sits at its integer coordinate, and an axis one light long draws at that light.
struct Plane {
    /// The draw.h canvas under it.
    draw::Canvas cv;

    /// Its width in lights.
    lengthType w() const { return cv.dims.x; }
    /// Its height in lights.
    lengthType h() const { return cv.dims.y; }
    /// Flock units to lights, across.
    float scaleX() const { return static_cast<float>(w()) / kSize; }
    /// Flock units to lights, down.
    float scaleY() const { return static_cast<float>(h()) / kSize; }
    /// A position across the flock's space as a light coordinate.
    float lightX(float x) const { return w() > 1 ? x * scaleX() - 0.5f : 0; }
    /// A position down the flock's space as a light coordinate.
    float lightY(float y) const { return h() > 1 ? y * scaleY() - 0.5f : 0; }
    /// Whether a boid of `radius` draws as the one light it stands on: one light wide, on a grid no wider than the space.
    bool onePixel(float radius) const { return radius <= 0.5f && scaleX() <= 1 && scaleY() <= 1; }

    /// Each step's fade, keeping `kTrailKeep` of every channel.
    void fade() const { draw::fade(cv, static_cast<uint8_t>(255 - kTrailKeep)); }

    /// Copy the colors into `layer`, a canvas of the same size, leaving its other channels as they are.
    void copyColorsTo(const draw::Canvas& layer) const {
        const size_t lights = static_cast<size_t>(w()) * h();
        for (size_t i = 0; i < lights; i++) draw::pixel(layer, i, draw::get(cv, i));
    }

    /// Move the light at (px, py) toward `c` by `cover`, from 0 to 1; clipped to the grid.
    void blend(int32_t px, int32_t py, RGB c, float cover) const {
        if (cover <= 0) return;
        draw::blendPixel(cv, Coord3D{static_cast<lengthType>(px), static_cast<lengthType>(py), 0}, c,
                         cover >= 1 ? 255 : static_cast<uint8_t>(cover * 255 + 0.5f));
    }

    /// Call `paint(px, py, dx, dy)` for every light within `radius` of light coordinate (x, y), (dx, dy) being the light's offset from it.
    template <typename Paint>
    void each(float x, float y, float radius, Paint paint) const {
        const int32_t y0 = std::max<int32_t>(0, static_cast<int32_t>(std::ceil(y - radius - 0.5f)));
        const int32_t y1 = std::min<int32_t>(h() - 1, static_cast<int32_t>(std::floor(y + radius + 0.5f)));
        const int32_t x0 = std::max<int32_t>(0, static_cast<int32_t>(std::ceil(x - radius - 0.5f)));
        const int32_t x1 = std::min<int32_t>(w() - 1, static_cast<int32_t>(std::floor(x + radius + 0.5f)));
        for (int32_t py = y0; py <= y1; py++)
            for (int32_t px = x0; px <= x1; px++) paint(px, py, px - x, py - y);
    }

    /// A soft dot at light coordinate (x, y): each light takes the color in proportion to how much of the dot covers it.
    void dot(float x, float y, RGB c, float radius) const {
        each(x, y, radius, [&](int32_t px, int32_t py, float dx, float dy) {
            blend(px, py, c, std::clamp(radius + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f));
        });
    }

    /// A disc with a mouth at (x, y) in the flock's space, heading (vx, vy): the body takes `c` and the mouth is painted dark over what was under it.
    void mouth(float x, float y, float vx, float vy, RGB c, float radius, float halfAngle) const {
        const float hx = vx * scaleX(), hy = vy * scaleY();
        each(lightX(x), lightY(y), radius, [&](int32_t px, int32_t py, float dx, float dy) {
            const Cover cover = mouthCovers(dx, dy, hx, hy, radius, halfAngle);
            blend(px, py, RGB{0, 0, 0}, cover.rim - cover.body);
            blend(px, py, c, cover.body);
        });
    }

    /// A boid at (x, y) in the flock's space, as it is now: the one light it stands on, or a soft dot.
    void head(float x, float y, RGB c, float radius) const {
        if (onePixel(radius)) blend(static_cast<int32_t>(x * scaleX()), static_cast<int32_t>(y * scaleY()), c, 1.0f);
        else dot(lightX(x), lightY(y), c, radius);
    }

    /// A boid's move from (x0, y0) to (x1, y1) in the flock's space as a streak of soft dots, spaced by its radius; its end alone when it is one light or the move came round the torus.
    void trace(float x0, float y0, float x1, float y1, RGB c, float radius) const {
        const bool cameRound = std::fabs(x1 - x0) > kSize / 2 || std::fabs(y1 - y0) > kSize / 2;
        if (onePixel(radius) || cameRound) { head(x1, y1, c, radius); return; }
        const float ax = lightX(x0), ay = lightY(y0), bx = lightX(x1), by = lightY(y1);
        const float length = std::max(std::fabs(bx - ax), std::fabs(by - ay));
        const int32_t dots = static_cast<int32_t>(std::ceil(length / std::max(1.0f, radius)));
        for (int32_t i = 0; i <= dots; i++) {
            const float t = dots ? static_cast<float>(i) / dots : 1.0f;
            dot(ax + (bx - ax) * t, ay + (by - ay) * t, c, radius);
        }
    }
};

/// Move boid `b` one step at `maxSpeed` and draw its streak on `trail`.
inline void moveTraced(Boid& b, float maxSpeed, const Plane& trail, RGB c, float radius) {
    const float x0 = b.x, y0 = b.y;
    move(b, maxSpeed);
    trail.trace(x0, y0, b.x, b.y, c, radius);
}

/// A flock effect's memory: room for its boids and the persistent trail, held from the prepare, so the tick works in memory already held.
struct Flock {
    ScratchBuffer<Boid> boids;      ///< every boid the effect may fly
    ScratchBuffer<uint8_t> trail;   ///< the persistent trail, for @xref{why-a-private-trail-plane|the reason above}

    /// Both buffers, owned by the effect `owner`.
    explicit Flock(MoonModule& owner) : boids{owner}, trail{owner} {}

    /// Hold room for `n` boids and a cleared trail the size of `owner`'s grid, warning on `owner` when memory runs out and clearing that warning once it fits.
    bool prepare(MoonModule& owner, lengthType w, lengthType h, uint8_t cpl, uint8_t n) {
        if (!trail.resize(static_cast<size_t>(w) * h * cpl) || !boids.resize(n)) {
            owner.setStatus(kNoMemory, MoonModule::Severity::Warning);
            return false;
        }
        if (owner.status() && std::strcmp(owner.status(), kNoMemory) == 0) owner.clearStatus();
        if (trail) std::memset(trail.data(), 0, trail.bytes());   // a prepare at the same size keeps the allocation, not the picture
        return true;
    }

    /// The trail as a plane on a `w` by `h` grid of `cpl` channels, or none while a resize outruns the prepare, which skips the frame until it catches up.
    std::optional<Plane> plane(lengthType w, lengthType h, uint8_t cpl) {
        if (!trail || !boids || trail.bytes() != static_cast<size_t>(w) * h * cpl) return std::nullopt;
        return Plane{draw::Canvas{trail.data(), trail.bytes(), Coord3D{w, h, 1}, cpl}};
    }
};

}  // namespace mm::boids

#endif  // MM_HEAVY_COMPUTE
