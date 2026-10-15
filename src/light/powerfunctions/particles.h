#pragma once

#include "core/util/math16.h"      // sin16/cos16 for angleEmit, hashInt for spray, isqrt for attract
#include "core/util/AudioFrame.h"   // AudioFrame: the spectrum audioDrive() reads
#include "core/services/AudioService.h" // latestFrame(): the live spectrum stepDriven() consumes
#include "light/powerfunctions/draw.h"       // pos_t, splat, Canvas: the sub-pixel writer
#include "light/util/Palette.h"    // colorFromPalette

namespace mm::particles {

/// One home for things that move under forces and get drawn.
///
/// @attribution Damian Schneider, Bill Reeves, Semi-implicit Euler
///
/// @technical
///
/// Its vocabulary of emitters, forces and walls over one pool follows the WLED Particle System, written fresh in fixed point.

/// Converts elapsed time into a per-frame scale, so physics runs at one speed on every target.
///
/// Why time is scaled rather than quantised: power-functions.md#particles
class FrameTime {
public:
    /// `referenceHz` is the rate the effect's numbers are written against; 60 is the convention.
    explicit FrameTime(uint16_t referenceHz = 60) : refHz_(referenceHz > 0 ? referenceHz : 60) {}

    /// Call once per frame. Returns the scale in 8.8 fixed point (256 == one reference frame).
    uint32_t advance(uint32_t nowMs) {
        if (!started_) { started_ = true; lastMs_ = nowMs; return kOne; }
        const uint32_t dt = nowMs - lastMs_;      // unsigned: correct across the millis() wrap
        lastMs_ = nowMs;
        // A frame faster than the timer can resolve reports dt == 0.
        num_ += static_cast<uint64_t>(dt) * kOne * refHz_;
        const uint64_t units = num_ / 1000u;
        if (units == 0) return 0;                 // not enough time yet; the numerator keeps it
        num_ -= units * 1000u;
        uint32_t s = static_cast<uint32_t>(units > kMaxScale ? kMaxScale : units);
        // Cap a long stall (WiFi reconnect, reflash) so one frame cannot teleport every particle.
        return s > kMaxScale ? kMaxScale : s;
    }

    /// Forget the accumulated time, so the next frame starts the clock again.
    void reset() { started_ = false; num_ = 0; }

    static constexpr uint32_t kOne = 256;         ///< one reference frame
    /// The largest scale a frame may report, which bounds a long stall to eight reference frames.
    static constexpr uint32_t kMaxScale = 256 * 8;

private:
    uint16_t refHz_;
    uint64_t num_ = 0;   // undivided time: dt * kOne * refHz_, spent in whole units
    uint32_t lastMs_ = 0;
    bool started_ = false;
};

/// Scale a signed value by `num/den` WITHOUT a right shift.
inline constexpr int32_t scaleSigned(int32_t v, int32_t num, int32_t den) {
    return static_cast<int32_t>((static_cast<int64_t>(v) * num) / den);
}

/// How a particle is drawn.
enum class RenderStyle : uint8_t {
    Splat,   ///< sub-pixel and additive, the default: light adds where particles overlap
    Hard,    ///< one whole pixel, replacing: the retro look, and cheaper
};

/// A pool of particles as a view over caller-owned arrays. POD: copy it freely, it owns nothing.
inline lengthType spreadLane(uint16_t i, uint16_t slots, lengthType extent) {
    if (slots == 0) return 0;
    // A step coprime with `slots`, chosen as near HALF of it as possible.
    uint16_t step = 1;
    if (slots > 2) {
        for (uint16_t d = 0; d < slots / 2; d++) {
            const uint16_t lo = static_cast<uint16_t>(slots / 2 - d);
            const uint16_t hi = static_cast<uint16_t>(slots / 2 + d);
            uint16_t pick = 0;
            for (uint16_t c : {hi, lo}) {
                if (c <= 1 || c >= slots) continue;
                uint16_t a = c, b = slots;
                while (b) { const uint16_t t = a % b; a = b; b = t; }
                if (a == 1) { pick = c; break; }     // gcd(c, slots) == 1
            }
            if (pick) { step = pick; break; }
        }
    }
    const uint16_t lane = static_cast<uint16_t>((static_cast<uint32_t>(i) * step) % slots);
    return static_cast<lengthType>((static_cast<int32_t>(extent) * lane) / slots);
}

/// Per-sprite audio drive.
inline uint32_t audioDrive(const AudioFrame* frame, uint16_t i, uint16_t slots) {
    if (!frame) return FrameTime::kOne;       // no audio source: move normally, never freeze

    // Below this the input is room noise rather than music, and the scene stands still.
    constexpr uint16_t kSilence = 8;
    if (frame->levelSmoothed < kSilence) return 0;

    const uint8_t band = slots ? static_cast<uint8_t>((static_cast<uint32_t>(i) * 16u) / slots) : 0;
    const uint32_t mag = frame->bands[band > 15 ? 15 : band];

    // A floor under the band keeps a sprite whose own band is quiet drifting slowly rather than frozen mid-air while the music plays.
    return FrameTime::kOne / 4 + (mag * FrameTime::kOne * 7) / (255 * 4);
}

struct Pool {
    draw::pos_t* x = nullptr;    ///< position along the first axis, in sub-pixels
    draw::pos_t* y = nullptr;    ///< position along the second axis, in sub-pixels
    draw::pos_t* vx = nullptr;   ///< velocity along the first axis
    draw::pos_t* vy = nullptr;   ///< velocity along the second axis
    /// Lifetime in reference frames; 0 = dead. SIXTEEN bits, not eight.
    uint16_t*    ttl = nullptr;
    uint8_t*     hue = nullptr;    ///< palette index per particle
    /// Per-particle radius in WHOLE pixels, 0 = a single sub-pixel splat. OPTIONAL.
    uint8_t*     size = nullptr;
    /// Sub-unit force accumulator, 3.4 fixed point, one nibble per axis (low = x, high = y).
    uint8_t*     acc = nullptr;
    uint16_t     count = 0;        ///< how many slots the arrays hold
    uint32_t     ageCarry_ = 0;    ///< sub-frame aging remainder (see age())
    int64_t      gCarry_ = 0;      ///< sub-unit gravity remainder (see gravity())

    /// Whether every lane is allocated and the pool holds room for at least one particle.
    bool valid() const { return x && y && vx && vy && ttl && hue && count > 0; }
    // `acc` and `size` are optional: a pool without them loses sub-unit forces and per-particle radius, rather than being invalid.

    /// Kill every particle: the state a pool starts in, and what prepare() should leave behind.
    void clear() {
        for (uint16_t i = 0; i < count; i++) {
            ttl[i] = 0; x[i] = y[i] = vx[i] = vy[i] = 0; hue[i] = 0;
            if (acc) acc[i] = 0;
            if (size) size[i] = 0;
        }
    }

    /// Index of a free slot, or `count` when the pool is full. Linear.
    uint16_t findFree() const {
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i] == 0) return i;
        return count;
    }

    /// Bring one particle to life. Returns false when the pool is full, so an emitter can stop rather than overwrite a living particle.
    bool spawn(draw::pos_t px, draw::pos_t py, draw::pos_t svx, draw::pos_t svy,
               uint16_t life, uint8_t color, uint8_t radius = 0) {
        const uint16_t i = findFree();
        if (i >= count) return false;
        x[i] = px; y[i] = py; vx[i] = svx; vy[i] = svy;
        ttl[i] = life == 0 ? 1 : life;      // life 0 would be born dead; clamp so a spawn always shows
        hue[i] = color;
        if (size) size[i] = radius;
        return true;
    }

    // --- Forces ---------------------------------------------------------------------------------

    /// Constant acceleration, the usual case being gravity. `g` is in sub-pixel units per frame².
    void gravity(draw::pos_t g, uint32_t scale = FrameTime::kOne) {
        // Accumulate the sub-unit part rather than dropping it.
        gCarry_ += static_cast<int64_t>(g) * scale;
        const int32_t dv = static_cast<int32_t>(gCarry_ / FrameTime::kOne);
        if (dv == 0) return;
        gCarry_ -= static_cast<int64_t>(dv) * FrameTime::kOne;
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i]) vy[i] = static_cast<draw::pos_t>(vy[i] + dv);
    }

    /// A constant push in any direction: wind, a tilt control, a thrust.
    void force(draw::pos_t fx, draw::pos_t fy, uint32_t scale = FrameTime::kOne) {
        const int32_t dx = scaleSigned(fx, static_cast<int32_t>(scale), FrameTime::kOne);
        const int32_t dy = scaleSigned(fy, static_cast<int32_t>(scale), FrameTime::kOne);
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i]) {
                vx[i] = static_cast<draw::pos_t>(vx[i] + dx);
                vy[i] = static_cast<draw::pos_t>(vy[i] + dy);
            }
    }

    /// A force too SMALL to move a velocity by one unit per frame, accumulated until it does.
    void forceSmall(int8_t fx, int8_t fy) {
        if (!acc) return;
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            int32_t ax = (acc[i] & 0x0F) + fx;         // x accumulator, 4 bits
            int32_t ay = ((acc[i] >> 4) & 0x0F) + fy;  // y accumulator
            // FLOOR toward negative infinity, not toward zero.
            const int32_t cx = (ax >= 0) ? (ax / 16) : -((15 - ax) / 16);
            const int32_t cy = (ay >= 0) ? (ay / 16) : -((15 - ay) / 16);
            vx[i] = static_cast<draw::pos_t>(vx[i] + cx);
            vy[i] = static_cast<draw::pos_t>(vy[i] + cy);
            ax -= cx * 16; ay -= cy * 16;              // the remainder is now always 0..15
            acc[i] = static_cast<uint8_t>(((ay & 0x0F) << 4) | (ax & 0x0F));
        }
    }

    /// Velocity damping, `v *= (256 - k) / 256`: air resistance.
    void drag(uint8_t k, uint32_t scale = FrameTime::kOne) {
        if (k == 0) return;
        // Exponential decay per unit TIME, not per frame.
        int32_t loss = static_cast<int32_t>((static_cast<uint32_t>(k) * scale) / FrameTime::kOne);
        if (loss > 255) loss = 255;
        const int32_t keep = 256 - loss;
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i]) {
                vx[i] = static_cast<draw::pos_t>(scaleSigned(vx[i], keep, 256));
                vy[i] = static_cast<draw::pos_t>(scaleSigned(vy[i], keep, 256));
            }
    }

    /// Pull every particle toward a point with an inverse-square falloff, clamped near the center so a particle sitting on the attractor does not receive an unbounded impulse.
    void attract(draw::pos_t ax, draw::pos_t ay, int32_t strength) {
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            const int32_t dx = ax - x[i];
            const int32_t dy = ay - y[i];
            // Work in whole pixels.
            const int32_t px = dx / draw::kSubOne, py = dy / draw::kSubOne;   // divide, not shift
            int32_t d2 = px * px + py * py;
            if (d2 < 1) d2 = 1;                       // the near-field clamp
            const int32_t a = strength / d2;
            // Direction times magnitude, normalized by the distance so diagonal pull is not stronger.
            const int32_t d = static_cast<int32_t>(isqrt(static_cast<uint32_t>(d2)));
            if (d == 0) continue;
            vx[i] += static_cast<draw::pos_t>((px * a) / d);
            vy[i] += static_cast<draw::pos_t>((py * a) / d);
        }
    }

    // --- Integration ----------------------------------------------------------------------------

    /// Advance every live particle by one frame: position from the CURRENT velocity.
    void step(uint32_t scale = FrameTime::kOne) {
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i]) {
                x[i] = static_cast<draw::pos_t>(x[i] + scaleSigned(vx[i], static_cast<int32_t>(scale), FrameTime::kOne));
                y[i] = static_cast<draw::pos_t>(y[i] + scaleSigned(vy[i], static_cast<int32_t>(scale), FrameTime::kOne));
            }
    }

    /// step() with a PER-PARTICLE time scale.
    void stepDriven(uint32_t scale, bool audioReactive, uint16_t live) {
        if (!audioReactive) { step(scale); return; }
        const AudioFrame* f = AudioService::latestFrame();
        stepEach(scale, [f, live](uint16_t i) { return audioDrive(f, i, live); });
    }

    /// Advance every live particle, taking each one's drive from `drive`.
    template <typename Drive>
    void stepEach(uint32_t scale, Drive drive) {
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i]) {
                const uint32_t s = (scale * drive(i)) / FrameTime::kOne;
                x[i] = static_cast<draw::pos_t>(x[i] + scaleSigned(vx[i], static_cast<int32_t>(s), FrameTime::kOne));
                y[i] = static_cast<draw::pos_t>(y[i] + scaleSigned(vy[i], static_cast<int32_t>(s), FrameTime::kOne));
            }
    }

    /// Count down every particle's life; a particle reaching zero is dead and its slot is reusable.
    void age(uint16_t rate = 1, uint32_t scale = FrameTime::kOne) {
        if (rate == 0) return;
        // ttl counts reference frames, so a fast device must age a fraction per frame. The carry makes those fractions add up instead of rounding to nothing and living forever.
        ageCarry_ += static_cast<uint32_t>(rate) * scale;
        const uint32_t whole = ageCarry_ / FrameTime::kOne;
        if (whole == 0) return;
        ageCarry_ -= whole * FrameTime::kOne;
        const uint16_t d = whole > 65535 ? 65535 : static_cast<uint16_t>(whole);
        for (uint16_t i = 0; i < count; i++)
            if (ttl[i]) ttl[i] = ttl[i] > d ? static_cast<uint16_t>(ttl[i] - d) : 0;
    }

    // --- Boundaries -----------------------------------------------------------------------------

    /// Reflect particles off the walls of a `w` by `h` grid, keeping a fraction `e` of the speed (restitution.
    void bounce(draw::pos_t w, draw::pos_t h, uint16_t e, uint8_t roughness = 0, uint32_t seed = 0) {
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            bool hit = false;
            if (x[i] < 0)      { x[i] = 0;     vx[i] = static_cast<draw::pos_t>(-scaleSigned(vx[i], e, 256)); hit = true; }
            else if (x[i] > w) { x[i] = w;     vx[i] = static_cast<draw::pos_t>(-scaleSigned(vx[i], e, 256)); hit = true; }
            if (y[i] < 0)      { y[i] = 0;     vy[i] = static_cast<draw::pos_t>(-scaleSigned(vy[i], e, 256)); hit = true; }
            else if (y[i] > h) { y[i] = h;     vy[i] = static_cast<draw::pos_t>(-scaleSigned(vy[i], e, 256)); hit = true; }
            if (hit && roughness) {
                // Scatter the tangent component. hashInt keeps this reproducible across devices, which a stream RNG would not be.
                const int32_t jitter = (static_cast<int32_t>(hashInt(i, x[i], y[i], seed) & 0xFF) - 128)
                                     * roughness / 128;
                vx[i] = static_cast<draw::pos_t>(vx[i] + jitter);
            }
        }
    }

    /// Reduce one coordinate into 0..span, in constant time.
    static draw::pos_t wrapCoord(draw::pos_t v, draw::pos_t span) {
        if (v >= 0 && v <= span) return v;                       // the common case, no divide
        // The two edges are not symmetric, which is what the loops this replaced showed.
        const draw::pos_t m = static_cast<draw::pos_t>(v % span);
        if (v > span) return static_cast<draw::pos_t>(m == 0 ? span : m);
        return static_cast<draw::pos_t>(m == 0 ? 0 : m + span);
    }

    /// Wrap particles around the grid edges: a particle leaving one side re-enters the other.
    void wrap(draw::pos_t w, draw::pos_t h, bool wrapX = true, bool wrapY = true) {
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            if (wrapX && w > 0) x[i] = wrapCoord(x[i], w);
            if (wrapY && h > 0) y[i] = wrapCoord(y[i], h);
        }
    }

    /// Kill any particle that has left the grid, the alternative to bouncing for sparks meant to fly away and vanish.
    void killOutside(draw::pos_t w, draw::pos_t h, draw::pos_t margin = 0) {
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            if (x[i] < -margin || x[i] > w + margin || y[i] < -margin || y[i] > h + margin) ttl[i] = 0;
        }
    }

    // --- Emitters -------------------------------------------------------------------------------

    /// Emit `n` particles from a point in a cone around `angle`, at `speed` ± `spread`. The classic spark/firework burst. `seed` makes the pattern reproducible.
    void angleEmit(draw::pos_t px, draw::pos_t py, angle16 angle, draw::pos_t speed,
                   angle16 cone, uint8_t n, uint16_t life, uint8_t color, uint32_t seed) {
        for (uint8_t k = 0; k < n; k++) {
            const uint16_t r1 = hashInt(k, seed, 1);
            const uint16_t r2 = hashInt(k, seed, 2);
            // Spread the emission across the cone rather than all along one ray.
            const angle16 a = static_cast<angle16>(angle + static_cast<int32_t>(r1 % (cone ? cone : 1)) - cone / 2);
            // Vary the speed so the burst has depth instead of a single expanding ring.
            const draw::pos_t s = speed - static_cast<draw::pos_t>(scaleSigned(speed, r2 & 0x3F, 256));
            const int32_t cx = static_cast<int32_t>(cos16(a));   // -32768..32767
            const int32_t sy = static_cast<int32_t>(sin16(a));
            if (!spawn(px, py,
                       static_cast<draw::pos_t>((cx * s) / 32768),
                       static_cast<draw::pos_t>((sy * s) / 32768),
                       life, color)) return;                            // pool full: stop emitting
        }
    }

    /// Emit `n` particles from a point with random velocities inside a box: a fountain, a spray, a burst of confetti. `angleEmit` gives a directed cone.
    void spray(draw::pos_t px, draw::pos_t py, draw::pos_t speed,
               uint8_t n, uint16_t life, uint8_t color, uint32_t seed) {
        for (uint8_t k = 0; k < n; k++) {
            // hashInt rather than a stream RNG.
            const int32_t rx = static_cast<int32_t>(hashInt(k, seed, 3) & 0x1FF) - 256;   // -256..255
            const int32_t ry = static_cast<int32_t>(hashInt(k, seed, 4) & 0x1FF) - 256;
            if (!spawn(px, py,
                       static_cast<draw::pos_t>((rx * speed) / 256),
                       static_cast<draw::pos_t>((ry * speed) / 256),
                       life, color)) return;                     // pool full: stop emitting
        }
    }

    // --- Collisions -------------------------------------------------------------------------------

    /// Make live particles bounce off each other. `radius` is the contact distance in sub-pixel units.
    void collide(draw::pos_t radius, uint16_t e = 200, uint32_t seed = 0) {
        if (radius <= 0) return;
        const int64_t r2 = static_cast<int64_t>(radius) * radius;
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            for (uint16_t j = static_cast<uint16_t>(i + 1); j < count; j++) {
                if (!ttl[j]) continue;
                // Broad phase.
                const int32_t dx = x[j] - x[i];
                if (dx > radius || dx < -radius) continue;
                const int32_t dy = y[j] - y[i];
                if (dy > radius || dy < -radius) continue;
                // 64-bit: a large contact radius squares past int32 (dx and dy are sub-pixel).
                const int64_t d2 = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
                if (d2 > r2 || d2 == 0) continue;               // not touching, or exactly coincident

                // Elastic response along the line of centers, equal masses.
                const int32_t d = static_cast<int32_t>(isqrt64(static_cast<uint64_t>(d2)));
                if (d == 0) continue;
                const int32_t nx = (dx * 256) / d;              // unit normal, 8.8
                const int32_t ny = (dy * 256) / d;
                const int32_t dvx = vx[j] - vx[i];
                const int32_t dvy = vy[j] - vy[i];
                const int32_t along = (dvx * nx + dvy * ny) / 256;
                if (along > 0) continue;                        // already separating; leave them be

                const int32_t impulse = scaleSigned(along, e, 256);
                vx[i] = static_cast<draw::pos_t>(vx[i] + (impulse * nx) / 256);
                vy[i] = static_cast<draw::pos_t>(vy[i] + (impulse * ny) / 256);
                vx[j] = static_cast<draw::pos_t>(vx[j] - (impulse * nx) / 256);
                vy[j] = static_cast<draw::pos_t>(vy[j] - (impulse * ny) / 256);

                // Separate the overlap by moving exactly ONE of the pair, chosen by a free pseudo-random bit.
                const int32_t overlap = radius - d;
                if (overlap > 0) {
                    const bool pushJ = (hashInt(i, j, 0, seed) & 1) != 0;
                    const int32_t ox = (nx * overlap) / 256;
                    const int32_t oy = (ny * overlap) / 256;
                    if (pushJ) { x[j] = static_cast<draw::pos_t>(x[j] + ox); y[j] = static_cast<draw::pos_t>(y[j] + oy); }
                    else       { x[i] = static_cast<draw::pos_t>(x[i] - ox); y[i] = static_cast<draw::pos_t>(y[i] - oy); }
                }
            }
        }
    }

    // --- Rendering ------------------------------------------------------------------------------

    /// Draw every live particle. Additive sub-pixel by default, so overlapping particles brighten and motion is smooth.
    void render(const draw::Canvas& cv, uint16_t maxLife = 255,
                RenderStyle style = RenderStyle::Splat) const {
        const uint16_t scale = maxLife == 0 ? 1 : maxLife;
        for (uint16_t i = 0; i < count; i++) {
            if (!ttl[i]) continue;
            const uint8_t bri = ttl[i] >= scale
                ? 255
                : static_cast<uint8_t>((static_cast<uint32_t>(ttl[i]) * 255u) / scale);
            const RGB c = colorFromPalette(*Palettes::active(), hue[i], bri);
            const uint8_t radius = size ? size[i] : 0;
            if (radius > 0) {
                // A sized particle is a disc, which is what makes a pool read as blobs rather than as a scatter of points: the signature look of a particle system.
                draw::fillCircle(cv, static_cast<lengthType>(draw::toPixel(x[i])),
                                 static_cast<lengthType>(draw::toPixel(y[i])),
                                 static_cast<lengthType>(radius), c);
            } else if (style == RenderStyle::Splat) {
                draw::splat(cv, x[i], y[i], c);
            } else {
                draw::pixel(cv, {static_cast<lengthType>(draw::toPixel(x[i])),
                                 static_cast<lengthType>(draw::toPixel(y[i])), 0}, c);
            }
        }
    }

    /// How many particles are alive: for a status line, or an effect that tops the pool up.
    uint16_t liveCount() const {
        uint16_t n = 0;
        for (uint16_t i = 0; i < count; i++) if (ttl[i]) n++;
        return n;
    }
};

}  // namespace mm::particles
