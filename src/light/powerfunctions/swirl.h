#pragma once

#include "core/module/MoonModule.h"           // the effect whose status a full memory sets
#include "core/util/ScratchBuffer.h"          // the pool, the depth and the trail an effect owns
#include "core/util/color.h"                  // RGB
#include "core/util/math8.h"                  // Random8
#include "core/util/noise.h"                  // curl16, the flow and the turbulence
#include "light/powerfunctions/boids.h"       // Plane: the trail canvas; kSize, the shared space
#include "light/powerfunctions/draw.h"        // Canvas, fill
#include "light/util/Palette.h"               // colorFromPalette, Palettes::active
#include "light/util/light_types.h"           // lengthType, and the platform's MM_HEAVY_COMPUTE
#include "platform/noinline.h"                // MM_NOINLINE: the birth helpers, called from every emitter
#include "platform/platform.h"                // MM_NONBLOCKING

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

/// One procedural particle engine: a packed pool, the step, the trail and the render, with @xref{the-motion|an emitter and a field} that set what the particles do.
///
/// A particle's whole life is two choices: how it is born, the emitter, and what moves it, the field. Any emitter pairs with any field, so five of each give twenty-five motions from one step loop, and two shared controls, @xref{the-frame|wander and spin}, move the whole motion around the space.
///
/// It sits beside particles.h's fixed-point pool, which runs on every chip, because its fields are fractional by nature, as boids.h's forces are: an attractor's derivatives and a curl field's velocities are fractions of a unit a step.
///
/// @attribution James Hutchinson, Claude, Robert Bridson, Arne Halvorsen, Bill Reeves
///
/// @technical
///
/// ## The space
///
/// The particles live in the flock's 32 by 32 space, so the trail plane maps them as it maps a boid, and a field's numbers read the same on every panel. The attractor's third axis lives in a side buffer, held only while the attractor is the field, so the other fields carry no unrendered axis.
///
/// ## The pool
///
/// The live particles sit packed in the pool's first slots: a birth takes the next slot, and a death moves the last live particle into the dead one's slot, the swap-and-pop of a dense object pool. So a birth, a death and a lower count each cost one move, and the step runs over the live particles only. The pool holds what `count` can reach, sized by the prepare.
///
/// ## The motion
///
/// A motion is two methods the engine calls each step: `emit`, which spawns the step's particles, and `drive`, which sets or adjusts one particle's velocity from its place. The engine then integrates semi-implicit Euler, position from the already-updated velocity, applies the border, ages the particle, and draws its move as a streak into the trail.
///
/// A field that sets the velocity outright, such as the attractor or the flow, and one that nudges it, such as ballistic or orbit, are the same call. A prepare settles the pattern with `settle`, the same steps with the drawing left out, so the first frame shows the pattern already formed.
///
/// ## The rescue rule
///
/// Any knob may sit at either end of its range, so a particle may be flung far out, fall into a sink, or overflow. One rule keeps every setting alive: a particle that leaves the space past the border's reach, goes non-finite, escapes the attractor or falls into the origin dies, and its emitter refills the pool.
///
/// A move is capped at two engine units a step, so a wild field's streak stays a short dash rather than a line across the panel, and its trace's cost stays bounded.
///
/// ## The trail
///
/// The streaks live in a plane of their own, faded each step and copied to the layer each frame, and the particles are drawn over them fresh: the reason boids.h gives for keeping a trail apart from the layer.
///
/// Each step fades the plane by `trail` before it moves the particles, so a trail is as long at any frame rate. At `trail` 0 no plane is held, and each particle draws its last step's streak straight into the layer.
///
/// ## Float, deliberately and locally
///
/// Positions and velocities are `float`, a field's numbers being intrinsically fractional, so this header compiles only where the platform declares a hardware FPU, one of the bounded exceptions the power functions page names.
#if MM_HEAVY_COMPUTE

namespace mm::swirl {

/// The space the particles live in, the flock's: @xref{the-space}.
inline constexpr float kSize = boids::kSize;
/// The last position inside the space on either axis: the largest float below kSize, written out because std::nextafter is not constexpr in C++20 and the edge emitter's side table needs it at compile time.
inline constexpr float kLast = kSize - kSize * std::numeric_limits<float>::epsilon() / 2;
/// The space's center, where the origin rests.
inline constexpr float kCenter = kSize / 2;
/// The steps a second the numbers are written for.
inline constexpr uint8_t kReferenceHz = boids::kReferenceHz;
/// The random sequence's start, so every run from a prepare is the same.
inline constexpr uint32_t kSeed = 0x57117u;
/// What a swirl effect's card says when its buffers do not fit in memory.
inline constexpr const char* kNoMemory = "not enough memory for the particles";
/// The longest move a step takes, in engine units: @xref{the-rescue-rule}.
inline constexpr float kMaxMove = 2.0f;
/// Palette entries per engine unit a step of speed, for the speed color: half a unit a step reaches the palette's end.
inline constexpr float kSpeedHue = 510.0f;

/// One particle, 24 bytes, in `float`: @xref{float-deliberately-and-locally|why}.
struct Particle {
    /// Its position in the engine's space.
    float x = 0, y = 0;
    /// Its velocity, in engine units a step.
    float vx = 0, vy = 0;
    /// The steps it has lived.
    uint16_t age = 0;
    /// The steps it lives; 0 is immortal.
    uint16_t life = 0;
    /// Its palette entry from birth.
    uint8_t hue = 0;

    /// The share of full brightness it draws at: full while young, fading to none as its life runs out; an immortal particle stays full.
    float brightness() const { return life == 0 ? 1.0f : std::clamp(1.0f - static_cast<float>(age) / life, 0.0f, 1.0f); }
};

/// What the border does to a particle that crosses it.
enum class Edge : uint8_t {
    kFree,     ///< flies on off the panel, and dies one space beyond it
    kDie,      ///< dies at the border
    kWrap,     ///< comes back on the far side, the flock's torus
    kBounce,   ///< reflects off the border
    kCount
};
/// The option names, in the enum's order.
inline constexpr const char* kEdgeOptions[] = {"free", "die", "wrap", "bounce"};
static_assert(sizeof(kEdgeOptions) / sizeof(kEdgeOptions[0]) == static_cast<size_t>(Edge::kCount), "one option per edge");

/// What picks a particle's palette entry when it is drawn.
enum class Color : uint8_t {
    kBirth,   ///< the entry it was born with
    kAge,     ///< how long it has lived, one turn of the palette in 256 steps
    kSpeed,   ///< how fast it moves, still at the palette's start
    kPlace,   ///< where it is, a diagonal gradient across the space
    kCount
};
/// The option names, in the enum's order.
inline constexpr const char* kColorOptions[] = {"birth", "age", "speed", "place"};
static_assert(sizeof(kColorOptions) / sizeof(kColorOptions[0]) == static_cast<size_t>(Color::kCount), "one option per color");

/// How a step draws: the border, the color, and the particle's radius in lights.
struct Look {
    Edge edge = Edge::kFree;     ///< what the border does
    Color color = Color::kBirth; ///< what picks the palette entry
    float radius = 0.5f;         ///< how wide a particle is drawn
};

/// A swirl effect's memory: the packed pool, the attractor's depth, and the persistent trail, with the step that moves the particles and the render that draws them.
struct Engine {
    ScratchBuffer<Particle> pool;   ///< the particles, the live ones packed first: @xref{the-pool}
    ScratchBuffer<float> depth;     ///< the third axis, one a pool slot, held only for the attractor
    ScratchBuffer<uint8_t> trail;   ///< the persistent trail, held only while `trail` can be above 0: @xref{the-trail}
    uint16_t live = 0;              ///< how many particles are alive, in slots 0 to live - 1

    /// The three buffers, owned by the effect `owner`.
    explicit Engine(MoonModule& owner) : pool{owner}, depth{owner}, trail{owner} {}

    /// Hold room for `capacity` particles, a depth for each when `withDepth`, and a cleared `trailBytes` trail, warning on `owner` when memory runs out and clearing that warning once it fits.
    bool prepare(MoonModule& owner, uint16_t capacity, bool withDepth, size_t trailBytes) {
        live = 0;
        if (depth.resize(withDepth ? capacity : 0) && boids::prepareTrailAndPool(owner, trail, pool, trailBytes, capacity, kNoMemory)) return true;
        // Any refusal frees all three, so no buffer is left sized for a different pool than the others.
        pool.resize(0);
        depth.resize(0);
        trail.resize(0);
        owner.setStatus(kNoMemory, MoonModule::Severity::Warning);
        return false;
    }

    /// Whether a trail is held.
    bool trailHeld() const MM_NONBLOCKING { return trail.bytes() > 0; }
    /// Whether the attractor's depth is held.
    bool depthHeld() const MM_NONBLOCKING { return depth.count() > 0; }

    /// The trail as a plane on a `w` by `h` grid of `cpl` channels, or none while a resize outruns the prepare, which skips the frame until it catches up.
    std::optional<boids::Plane> plane(lengthType w, lengthType h, uint8_t cpl) {
        if (trail.bytes() != static_cast<size_t>(w) * h * cpl) return std::nullopt;
        return boids::Plane{draw::Canvas{trail.data(), trail.bytes(), Coord3D{w, h, 1}, cpl}};
    }

    /// The next free slot, its depth zeroed, or nullptr when the pool is full, so an emitter stops rather than overwriting a living particle.
    Particle* spawn() MM_NONBLOCKING {
        if (live >= pool.count()) return nullptr;
        if (depthHeld()) depth[live] = 0;
        pool[live] = Particle{};
        return &pool[live++];
    }

    /// The depth of the particle at `p`, or nullptr while no depth is held.
    float* depthOf(const Particle* p) MM_NONBLOCKING { return depthHeld() ? &depth[static_cast<size_t>(p - pool.data())] : nullptr; }

    /// Kill the particle in slot `i`: the last live particle takes its slot.
    void kill(uint16_t i) MM_NONBLOCKING {
        live--;
        if (i == live) return;
        pool[i] = pool[live];
        if (depthHeld()) depth[i] = depth[live];
    }

    /// Let the particles past `cap` go, so a count turned down takes effect at once.
    void trim(uint16_t cap) MM_NONBLOCKING { live = std::min(live, cap); }

    /// One step: the motion emits, then every live particle takes its velocity from the motion, moves, ages and leaves its streak on `trailPlane` when one is held.
    template <typename Motion>
    void step(Motion& motion, const boids::Plane* trailPlane, const Look& look) MM_NONBLOCKING {
        motion.emit(*this);
        const Palette& pal = *Palettes::active();
        for (uint16_t i = 0; i < live;) {
            Particle& p = pool[i];
            float* z = depthHeld() ? &depth[i] : nullptr;
            const float x0 = p.x, y0 = p.y;   // where it stood before the move, which the streak starts from
            if (!motion.drive(p, z) || !advance(p, z, look.edge)) { kill(i); continue; }   // the last particle now in slot i takes its step next
            if (trailPlane) {
                const uint8_t bri = static_cast<uint8_t>(p.brightness() * 255.0f);
                if (bri) trailPlane->trace(x0, y0, p.x, p.y, colorFromPalette(pal, hue(p, look.color), bri), look.radius);
            }
            i++;
        }
    }

    /// Run `steps` steps of the motion with the drawing left out, so the first frame already shows the pattern settled.
    template <typename Motion>
    void settle(Motion& motion, uint16_t steps, Edge edge) {
        const Look look{edge};
        for (uint16_t s = 0; s < steps; s++) step(motion, nullptr, look);
    }

    /// Move one particle a step under @xref{the-rescue-rule}: the move capped, the position integrated, the border applied, the age counted; false when it dies.
    static bool advance(Particle& p, const float* z, Edge edge) MM_NONBLOCKING {
        const float v2 = p.vx * p.vx + p.vy * p.vy;
        if (!std::isfinite(v2) || (z && !std::isfinite(*z))) return false;
        if (v2 > kMaxMove * kMaxMove) {
            const float k = kMaxMove / std::sqrt(v2);
            p.vx *= k;
            p.vy *= k;
        }
        p.x += p.vx;
        p.y += p.vy;
        if (!border(p, edge)) return false;
        p.age++;   // an immortal particle's age wraps, which the age color turns through seamlessly
        return p.life == 0 || p.age < p.life;
    }

    /// Apply the border to a particle that crossed it; false when it dies.
    static bool border(Particle& p, Edge edge) MM_NONBLOCKING {
        if (p.x >= 0 && p.x < kSize && p.y >= 0 && p.y < kSize) return true;
        switch (edge) {
            case Edge::kDie: return false;
            case Edge::kWrap: wrap(p); return true;
            case Edge::kBounce: bounce(p); return true;
            default: return withinReach(p);   // free
        }
    }

    /// The far side of the space, the flock's torus.
    static void wrap(Particle& p) MM_NONBLOCKING {
        p.x = std::min(p.x - kSize * std::floor(p.x / kSize), kLast);   // a tiny negative wraps to kSize itself in float
        p.y = std::min(p.y - kSize * std::floor(p.y / kSize), kLast);
    }

    /// Reflected off the border, its velocity turned back on the axis it crossed.
    static void bounce(Particle& p) MM_NONBLOCKING {
        reflect(p.x, p.vx);
        reflect(p.y, p.vy);
    }

    /// One axis reflected off each border it crossed, from either side, its velocity turned back at each, then held inside [0, kSize).
    static void reflect(float& x, float& v) MM_NONBLOCKING {
        if (x < 0) { x = -x; v = -v; }
        if (x >= kSize) { x = 2 * kSize - x; v = -v; }
        if (x < 0) { x = -x; v = -v; }   // from a whole space past the far border, off the near one too
        x = std::clamp(x, 0.0f, kLast);
    }

    /// Whether a free particle is still within one space of the border, the end of its reach.
    static bool withinReach(const Particle& p) MM_NONBLOCKING {
        return p.x > -kSize && p.x < 2 * kSize && p.y > -kSize && p.y < 2 * kSize;
    }

    /// The palette entry particle `p` draws with under `mode`.
    static uint8_t hue(const Particle& p, Color mode) MM_NONBLOCKING {
        switch (mode) {
            case Color::kAge: return static_cast<uint8_t>(p.hue + p.age);
            case Color::kSpeed: return static_cast<uint8_t>(std::min(255.0f, std::sqrt(p.vx * p.vx + p.vy * p.vy) * kSpeedHue));
            case Color::kPlace: return static_cast<uint8_t>(std::clamp((p.x + p.y) * (255.0f / (2 * kSize)), 0.0f, 255.0f));
            default: return p.hue;
        }
    }

    /// Draw the frame on `layer`: the trail's colors and every live particle over them, or, with no trail held, a black layer and each particle's last step as a streak.
    void present(const boids::Plane* trailPlane, const draw::Canvas& layer, const Look& look) const MM_NONBLOCKING {
        const boids::Plane layerPlane{layer};
        if (trailPlane) trailPlane->copyColorsTo(layer);
        else draw::fill(layer, RGB{0, 0, 0});
        const Palette& pal = *Palettes::active();
        for (uint16_t i = 0; i < live; i++) {
            const Particle& p = pool[i];
            const uint8_t bri = static_cast<uint8_t>(p.brightness() * 255.0f);
            if (bri == 0) continue;
            const RGB c = colorFromPalette(pal, hue(p, look.color), bri);
            if (trailPlane) layerPlane.head(p.x, p.y, c, look.radius);
            else layerPlane.trace(p.x - p.vx, p.y - p.vy, p.x, p.y, c, look.radius);
        }
    }
};

/// How a particle is born: @xref{the-motion}.
enum class Emitter : uint8_t {
    kScatter,   ///< anywhere in the space
    kCloud,     ///< a small cloud at the origin
    kNozzle,    ///< a jet from the origin, aimed along the frame
    kRing,      ///< whole rings from the origin, one every period
    kEdge,      ///< along the border, moving inward
    kCount
};
/// The option names, in the enum's order.
inline constexpr const char* kEmitterOptions[] = {"scatter", "cloud", "nozzle", "ring", "edge"};
static_assert(sizeof(kEmitterOptions) / sizeof(kEmitterOptions[0]) == static_cast<size_t>(Emitter::kCount), "one option per emitter");

/// What moves a particle: @xref{the-motion}.
enum class Field : uint8_t {
    kAttractor,   ///< Halvorsen's attractor, in 3D, seen along its turning frame
    kFlow,        ///< curl noise, which carries particles at an even density: @xref{the-flow}
    kVortex,      ///< an expansion and a turn about the origin
    kBallistic,   ///< drag and gravity
    kOrbit,       ///< a pull toward the origin with a tangential push
    kCount
};
/// The option names, in the enum's order.
inline constexpr const char* kFieldOptions[] = {"attractor", "flow", "vortex", "ballistic", "orbit"};
static_assert(sizeof(kFieldOptions) / sizeof(kFieldOptions[0]) == static_cast<size_t>(Field::kCount), "one option per field");

/// A mask of parts, one bit an emitter or a field by its enum value, for a knob that belongs to some of them.
template <typename Part>
constexpr uint8_t mask(Part a) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(a)); }
/// The mask of two parts.
template <typename Part>
constexpr uint8_t mask(Part a, Part b) { return static_cast<uint8_t>(mask(a) | mask(b)); }

/// An emitter paired with a field, and the frame they share: the parameters an effect resolves once a tick, and the state that runs from step to step.
///
/// @technical
///
/// ## The frame
///
/// The origin is where the emitters at a point sit and what the fields turn about. `wander` sends it along a slow Lissajous figure over the space, so the whole motion tours the panel; at 0 it rests at the center.
///
/// `spin` turns the frame: the nozzle's aim, the attractor's view about its vertical axis, and the flow's pattern about the origin. The vortex and the orbit look the same at any turn, so their own `swirl` is their turn.
///
/// ## The attractor
///
/// Halvorsen's system, integrated by Euler at a hundredth of a time unit a step: dx/dt is -a*x - 4y - 4z - y^2, dy/dt is -a*y - 4z - 4x - z^2, dz/dt is -a*z - 4x - 4y - x^2. The attractor spans about -12 to 6 on each axis, so its frame centers on its middle, near -3, which sits at the origin.
///
/// The field turns by the frame before it is read and back after, so the knot rolls as the frame turns. A particle thirty attractor units from the center has escaped and dies.
///
/// ## The flow
///
/// Bridson's curl noise: the velocity is the curl of a noise potential, so the flow keeps the particles' density even. `scale` sets the features' size and `drift` how fast the potential changes. `turbulence` adds the same curl, finer and slower, over any field.
struct Motion {
    Emitter emitter = Emitter::kCloud;   ///< how particles are born
    Field field = Field::kAttractor;     ///< what moves them
    uint8_t count = 120;          ///< the population the emitters fill to
    uint16_t life = 0;            ///< a particle's lifetime in steps, 0 immortal
    float spread = 0.28f;         ///< the nozzle's half angle, in radians
    uint8_t period = 30;          ///< steps between rings
    uint8_t ringSize = 24;        ///< particles in a ring
    float a = 1.89f;              ///< the attractor's constant
    float factor = 1.2f;          ///< engine units per attractor unit
    uint32_t coordScale = 5248;   ///< the flow's noise units per engine unit
    uint8_t drift = 96;           ///< the flow's noise time a step
    float expand = 0.12f;         ///< the vortex's outward speed, inward below 0
    float swirl = 0.088f;         ///< the vortex's turn speed, the other way below 0; the orbit's push
    float drag = 0.012f;          ///< the share of velocity drag takes a step
    float gravity = 0.005f;       ///< the downward pull, engine units a step a step
    float pull = 0.005f;          ///< the orbit's pull toward the origin
    float spinRate = 0;           ///< the frame's turn a step, in radians
    float wander = 0;             ///< how far the origin travels from the center
    int32_t turbulence = 0;       ///< the curl strength added over any field
    Random8 rng{kSeed};           ///< the random sequence, fixed-seed so a run from a prepare repeats

    /// Advance the frame, then emit the step's particles.
    void emit(Engine& e) MM_NONBLOCKING {
        advanceFrame();
        switch (emitter) {
            case Emitter::kScatter:
                while (e.live < count) {
                    const float x = uniform() * kSize;   // each draw its own statement: argument order is the compiler's
                    const float y = uniform() * kSize;
                    const uint8_t hue = rng.next8();
                    if (!born(e, x, y, 0, 0, hue, lifetime())) break;
                }
                break;
            case Emitter::kCloud:
                while (e.live < count) {
                    const float x = ox_ + signedUniform() * kCloud;
                    const float y = oy_ + signedUniform() * kCloud;
                    const float z = signedUniform() * kCloud;
                    const uint8_t hue = rng.next8();
                    Particle* p = born(e, x, y, 0, 0, hue, lifetime());
                    if (!p) break;
                    if (float* d = e.depthOf(p)) *d = z;
                }
                break;
            case Emitter::kNozzle: emitNozzle(e); break;
            case Emitter::kRing: emitRing(e); break;
            default: emitEdge(e); break;
        }
    }

    /// Set or nudge the particle's velocity from the field, then add the turbulence; false when it dies by @xref{the-rescue-rule}.
    bool drive(Particle& p, float* z) MM_NONBLOCKING {
        switch (field) {
            case Field::kAttractor: if (!attract(p, z)) return false; break;
            case Field::kFlow: flow(p); break;
            case Field::kVortex: if (!vortex(p)) return false; break;
            case Field::kBallistic:
                p.vx *= 1.0f - drag;
                p.vy = p.vy * (1.0f - drag) + gravity;
                break;
            default: if (!orbit(p)) return false; break;
        }
        if (turbulence) {
            int32_t cx = 0, cy = 0;
            curl16(noiseCoord(p.x * kTurbScale), noiseCoord(p.y * kTurbScale), turbTime_, turbulence, cx, cy);
            p.vx += cx * (1.0f / 65536);
            p.vy += cy * (1.0f / 65536);
        }
        return true;
    }

    /// The origin now, for tests.
    float originXForTest() const { return ox_; }
    /// The frame's turn now, for tests.
    float thetaForTest() const { return theta_; }

    /// The curl strength at full turbulence: a mean push of 0.02 engine units a step, measured.
    static constexpr int32_t kTurbStrength = 11000;

private:
    /// Half the side of the cloud the cloud emitter seeds, in engine units.
    static constexpr float kCloud = 1.2f;
    /// The nozzle's thrust, in engine units a step, varied by 30% either way.
    static constexpr float kThrust = 0.4f;
    /// The steps the nozzle spreads `count` births over when particles are immortal.
    static constexpr uint16_t kNozzleSteps = 150;
    /// A ring's radius when it is blown, in engine units.
    static constexpr float kRingRadius = 1.5f;
    /// A ring particle's outward kick, for the fields that only nudge.
    static constexpr float kRingKick = 0.05f;
    /// How far a ring's hue steps from the last: eight rings run the palette.
    static constexpr uint8_t kRingHueStep = 32;
    /// The edge emitter's inward speed, in engine units a step.
    static constexpr float kEdgeSpeed = 0.15f;
    /// Attractor time a step integrates: the textbook hundredth, small enough that Euler holds the loop.
    static constexpr float kStep = 0.01f;
    /// The attractor's center on each axis, measured: the box from -12 to 6 centers near -3.
    static constexpr float kOffset = -2.93f;
    /// The squared distance from the attractor's center past which a particle has escaped, thirty attractor units.
    static constexpr float kEscape2 = 900.0f;
    /// The flow's curl strength: a mean flow of 0.12 engine units a step, measured.
    static constexpr int32_t kFlowStrength = 65536;
    /// Noise units per engine unit for the turbulence: features eight units across.
    static constexpr float kTurbScale = 8192.0f;
    /// The share of `swirl` the orbit pushes with, a force rather than the vortex's speed.
    static constexpr float kOrbitSwirl = 0.05f;
    /// Below this distance from the origin a particle has no direction from it, and an inward field has taken it.
    static constexpr float kSink = 0.05f;
    /// The origin's two turns a step along its Lissajous figure: about 34 and 24 seconds a lap at the reference pace.
    static constexpr float kWanderX = 0.0031f, kWanderY = 0.0043f;

    float wanderX_ = 0, wanderY_ = 1.0f;   ///< the origin's two phases along its path, wrapped like the frame's turn
    float ox_ = kCenter, oy_ = kCenter;   ///< the origin
    float theta_ = 0;          ///< the frame's turn
    float cos_ = 1, sin_ = 0;  ///< its cosine and sine, once a step
    float inv_ = 1 / 1.2f;     ///< attractor units per engine unit, once a step, so a particle multiplies rather than divides
    uint32_t flowTime_ = 0;    ///< the flow's noise clock
    uint32_t turbTime_ = 0;    ///< the turbulence's noise clock, in steps
    uint32_t carry_ = 0;       ///< the nozzle's unspent births, in step-counts
    uint8_t ringStep_ = 254;   ///< steps since the last ring: a ring is due on the first step
    uint8_t ringHue_ = 0;      ///< the next ring's hue

    /// An angle brought back into one turn, so a phase that runs for days keeps its resolution.
    static float wrapTurn(float a) {
        constexpr float kTurn = 4 * boids::kQuarterTurn;
        return a >= kTurn ? a - kTurn : a < 0 ? a + kTurn : a;
    }
    /// A draw in [0, 1); out of line, since only births draw.
    MM_NOINLINE float uniform() { return boids::uniform(rng); }
    /// A draw in [-1, 1).
    float signedUniform() { return uniform() * 2.0f - 1.0f; }
    /// A position as a noise coordinate, kept clear of zero so a particle past the border still samples the field.
    static uint32_t noiseCoord(float v) { return static_cast<uint32_t>(std::max(0.0f, v + 64.0f * 65536.0f)); }

    /// Move the origin along its path, turn the frame, and run the noise clocks.
    void advanceFrame() {
        wanderX_ = wrapTurn(wanderX_ + kWanderX);
        wanderY_ = wrapTurn(wanderY_ + kWanderY);
        ox_ = kCenter + wander * std::sin(wanderX_);
        oy_ = kCenter + wander * std::sin(wanderY_);
        theta_ = wrapTurn(theta_ + spinRate);
        cos_ = std::cos(theta_);
        sin_ = std::sin(theta_);
        inv_ = 1.0f / factor;
        flowTime_ += drift;
        turbTime_++;
    }

    /// A lifetime of `life` steps varied by a quarter either way, so a generation does not die at once; 0 stays immortal. Out of line, called once a birth.
    MM_NOINLINE uint16_t lifetime() { return life ? static_cast<uint16_t>(life * (0.75f + 0.5f * uniform())) + 1 : 0; }

    /// A particle born at (x, y) with velocity (vx, vy), `hue` and `steps` to live; nullptr when the pool is full. One copy for every emitter, a birth running once a particle's life.
    MM_NOINLINE static Particle* born(Engine& e, float x, float y, float vx, float vy, uint8_t hue, uint16_t steps) {
        Particle* p = e.spawn();
        if (!p) return nullptr;
        p->x = x;
        p->y = y;
        p->vx = vx;
        p->vy = vy;
        p->hue = hue;
        p->life = steps;
        return p;
    }

    /// Emit `count` particles over each lifetime, aimed along the frame within the spread.
    void emitNozzle(Engine& e) {
        const uint32_t per = life ? life : kNozzleSteps;
        carry_ += count;
        while (carry_ >= per) {
            if (e.live >= count) { carry_ %= per; return; }   // the spray is full at this count
            carry_ -= per;
            const float aim = theta_ + signedUniform() * spread;
            const float thrust = kThrust * (0.7f + 0.6f * uniform());
            const uint8_t hue = rng.next8();
            if (!born(e, ox_, oy_, std::cos(aim) * thrust, std::sin(aim) * thrust, hue, lifetime())) return;
        }
    }

    /// Blow a ring when its step comes, whole or not at all.
    void emitRing(Engine& e) {
        if (++ringStep_ < period) return;
        ringStep_ = 0;
        const uint8_t n = std::min(ringSize, count);   // a count below the ring's size blows a smaller ring
        if (e.live + n > count) return;   // no room for a whole ring
        const uint8_t hue = ringHue_;
        ringHue_ = static_cast<uint8_t>(ringHue_ + kRingHueStep);
        const uint16_t steps = lifetime();   // one life a ring, so a ring fades whole
        for (uint8_t i = 0; i < n; i++) {
            const float turn = theta_ + (i * 4 * boids::kQuarterTurn) / n;
            const float ux = std::cos(turn), uy = std::sin(turn);
            if (!born(e, ox_ + ux * kRingRadius, oy_ + uy * kRingRadius, ux * kRingKick, uy * kRingKick, hue, steps)) return;
        }
    }

    /// One side of the border: where a birth sits across it (a fixed coordinate, or -1 for the draw along it) and its inward velocity's direction.
    struct Side { float x, y, dx, dy; };
    /// The four sides: left, right, top, bottom.
    static constexpr Side kSides[4] = {
        {0, -1, 1, 0}, {kLast, -1, -1, 0}, {-1, 0, 0, 1}, {-1, kLast, 0, -1},
    };

    /// Fill to the count along the border, each particle moving inward.
    void emitEdge(Engine& e) {
        while (e.live < count) {
            const float t = uniform() * kSize;
            const Side& side = kSides[rng.below(4)];
            const uint8_t hue = rng.next8();
            if (!born(e, side.x < 0 ? t : side.x, side.y < 0 ? t : side.y, side.dx * kEdgeSpeed, side.dy * kEdgeSpeed, hue, lifetime())) return;
        }
    }

    /// The attractor at the particle, read in the turned frame: @xref{the-attractor}.
    bool attract(Particle& p, float* z) {
        const float lx = (p.x - ox_) * inv_, ly = (p.y - oy_) * inv_, lz = z ? *z * inv_ : 0.0f;
        const float rx = cos_ * lx + sin_ * lz, rz = cos_ * lz - sin_ * lx;   // the frame turned back, about the vertical axis
        if (rx * rx + ly * ly + rz * rz > kEscape2) return false;
        const float ax = rx + kOffset, ay = ly + kOffset, az = rz + kOffset;
        const float dx = -a * ax - 4 * ay - 4 * az - ay * ay;
        const float dy = -a * ay - 4 * az - 4 * ax - az * az;
        const float dz = -a * az - 4 * ax - 4 * ay - ax * ax;
        const float s = factor * kStep;
        p.vx = (cos_ * dx - sin_ * dz) * s;
        p.vy = dy * s;
        if (z) *z += (sin_ * dx + cos_ * dz) * s;   // the depth has no velocity to keep: it integrates here
        return true;
    }

    /// The flow at the particle, its pattern turned with the frame about the origin.
    void flow(Particle& p) {
        const float lx = p.x - ox_, ly = p.y - oy_;
        const float rx = cos_ * lx + sin_ * ly, ry = cos_ * ly - sin_ * lx;
        int32_t cx = 0, cy = 0;
        curl16(noiseCoord(rx * coordScale), noiseCoord(ry * coordScale), flowTime_, kFlowStrength, cx, cy);
        const float fx = cx * (1.0f / 65536), fy = cy * (1.0f / 65536);
        p.vx = cos_ * fx - sin_ * fy;
        p.vy = sin_ * fx + cos_ * fy;
    }

    /// Outward and around the origin; false for a particle an inward field has taken into it.
    bool vortex(Particle& p) {
        const float dx = p.x - ox_, dy = p.y - oy_;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (expand < 0 && d <= -expand) return false;   // this step would reach the origin: the sink has taken it
        if (d < kSink) {
            p.vx = expand;   // at the origin, no direction: push out along x
            p.vy = 0;
            return true;
        }
        const float ux = dx / d, uy = dy / d;
        p.vx = ux * expand - uy * swirl;
        p.vy = uy * expand + ux * swirl;
        return true;
    }

    /// A pull toward the origin and a push around it, with drag; false for a particle that has fallen into it.
    bool orbit(Particle& p) {
        const float dx = p.x - ox_, dy = p.y - oy_;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < kSink) {
            // At the origin there is no direction to pull along: one slower than a step's pull cannot climb out and has fallen in; one leaving it, a nozzle's newborn, flies on.
            return p.vx * p.vx + p.vy * p.vy > pull * pull;
        }
        const float ux = dx / d, uy = dy / d, push = swirl * kOrbitSwirl;
        p.vx = (p.vx - ux * pull - uy * push) * (1.0f - drag);
        p.vy = (p.vy - uy * pull + ux * push) * (1.0f - drag);
        return true;
    }
};

}  // namespace mm::swirl

#endif  // MM_HEAVY_COMPUTE
