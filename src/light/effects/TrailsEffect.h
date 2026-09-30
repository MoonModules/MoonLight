#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Effect: bright dots thrown into a flowing medium, leaving tails the flow carries and bends.
/// @card TrailsEffect.gif
///
/// An emitter draws bright points, `draw::advect16` carries the plane, `draw::decay16` dims it.
/// Run every frame, that loop is what a trail is.
/// The tail is last frame's dots moved and dimmed, so the flow's shape is visible in it.
///
/// Prior art: 4wheeljive's FlowFields from a Stefan Petrick concept, and Stam's backward advection.
///
/// @moreinfo
///
/// ## The plane is 16-bit, and that is the point
///
/// A trail is a value multiplied by slightly less than one, hundreds of times a second.
/// At 8 bits that truncates to nothing, or rounds back up and the effect turns solid.
/// `draw::decay` carries the measurements.
/// So the plane is wider than the layer it writes to and narrows once on the way out.
/// The precision belongs in the accumulator rather than in the frame buffer.
///
/// ## Cost and the volume
///
/// One advect, a bilinear sample per light, plus one noise sample per light for the flow.
/// The flow rule dominates.
/// On a cube each slice gets its own flow, sampled at that slice's depth.
/// Light travels within a slice: `advect16` yields vx and vy, so a trail stays in its plane.
/// Transport through the volume needs a trilinear sampler and a vz, which is its own change.
class TrailsEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫🖌️💨🌫️"; }
    /// Volumetric, and each slice gets its own flow.
    Dim dimensions() const override { return Dim::D3; }

    static constexpr uint8_t kMaxDots = 8;         ///< the `dots` control's ceiling
    static constexpr uint32_t kMaxEmitters = 64;   ///< the ceiling once `dots` is scaled by the grid

    /// The head's radius: one light on a panel, larger as the grid grows, capped for a wall.
    static lengthType spanRadius(lengthType span) {
        const lengthType r = static_cast<lengthType>(span / 256 + 1);
        return r > 4 ? 4 : r;
    }

    /// How fast the medium moves, and with it every tail.
    uint8_t speed      = 40;
    /// Emitter density: the count scales with the grid.
    uint8_t dots       = 3;
    /// The flow field's cell size. Low gives broad sweeps, high gives eddies.
    uint8_t scale      = 30;
    /// How long a tail survives, as a half-life.
    uint8_t persistence = 90;
    /// How much the flow's strength rises and falls.
    uint8_t breathe    = 40;

    /// Publish the flow, the emitter density and the tail's half-life.
    void defineControls() override {
        controls_.addControl("speed", speed, 0, 255);
        controls_.addControl("dots", dots, 1, kMaxDots);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("persistence", persistence, 0, 255);
        controls_.addControl("breathe", breathe, 0, 255);
    }

    /// Size the wide plane and its spare, and clear both when the grid's shape changed.
    void prepare() override {
        // The plane holds three channels per light whatever the layer's width is.
        const lengthType w = width(), h = height(), d = depth();
        const size_t needed = static_cast<size_t>(w) * h * d * 3;
        const size_t had = plane_.count();
        plane_.resize(needed);
        scratch_.resize(needed);
        carry_.resize(needed);         // the dither's error, one byte per sample
        // Same count, new shape: both planes hold the old geometry's samples, so clear both.
        if (needed > 0 && needed == had && (w != planeW_ || h != planeH_ || d != planeD_)) {
            std::memset(plane_.data(), 0, plane_.bytes());
            std::memset(scratch_.data(), 0, scratch_.bytes());
        }
        planeW_ = w; planeH_ = h; planeD_ = d;
        started_ = false;      // the next tick is the first: it has no previous frame to measure
    }

    void tick() MM_NONBLOCKING override {
        if (!plane_ || !scratch_) return;               // a zero grid, or an allocation that failed
        const lengthType w = width(), h = height(), d = depth();
        // A zero delta on the first tick: `elapsed() - 0` would hand the flow the whole uptime.
        const uint32_t now = elapsed();
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        // Two oscillators: one walks the emitters, one breathes the flow's strength.
        bank_.set(0, {.rate = static_cast<uint16_t>(8 + speed / 8), .low = 0, .high = 65535,
                      .phaseOffset = 0, .wave = Wave::Saw});
        bank_.set(1, {.rate = 7, .low = static_cast<int32_t>(256 - breathe),
                      .high = static_cast<int32_t>(256 + breathe),
                      .phaseOffset = 0, .wave = Wave::Sine});
        // advanceTo() takes an absolute timestamp and computes its own delta, so never pass dt.
        bank_.advanceTo(now);

        // A ScratchBuffer is fixed to its module, so the ping-pong swaps a flag rather than buffers.
        ScratchBuffer<uint16_t>& src = front_ ? plane_ : scratch_;
        ScratchBuffer<uint16_t>& dst = front_ ? scratch_ : plane_;

        // 1. Transport, backward-sampled: neighboring pixels take different paths, which bends the trail.
        const uint32_t t = now;
        const uint32_t cells = static_cast<uint32_t>(scale) * 256u;
        const int32_t strength = static_cast<int32_t>(bank_.value(1));   // the breathing multiplier
        const uint32_t step = (static_cast<uint32_t>(speed) * dt) / 8u;  // sub-pixels this frame
        draw::advect16(dst.data(), src.data(), w, h, d,
                       [&](lengthType x, lengthType y, lengthType z,
                           draw::pos_t& vx, draw::pos_t& vy) {
                           flowAt(x, y, z, t, cells, strength, step, vx, vy);
                       }, draw::Edge::Clamp);
        front_ = !front_;                       // the destination now holds the trail
        ScratchBuffer<uint16_t>& moved = front_ ? plane_ : scratch_;

        // 2. Decay by half-life, the step needing the wide plane, so a tail lasts equal seconds anywhere.
        draw::decay16(moved.data(), moved.count(), halfLifeMs(), dt);

        // 3. Emit after the transport, so this frame's dots are sharp and only older ones carried.
        emitDots(moved.data(), w, h, d, dt);

        // 4. Narrow onto the layer, dithered: a trail's slow fade is where 8-bit truncation bands.
        draw::blit16(canvas(), moved.data(), w, h, d, carry_ ? carry_.data() : nullptr);
    }

    /// Test seam: how many trail samples both planes hold, since a rendered frame cannot show a stale plane.
    std::size_t trailSamples() const { return plane_.count() + scratch_.count(); }
    /// Test seam: read one trail sample across both planes, where past the end reads 0.
    uint16_t trailAt(std::size_t i) const {
        const std::size_t n = plane_.count();
        if (i < n) return plane_.data()[i];
        const std::size_t j = i - n;
        return j < scratch_.count() ? scratch_.data()[j] : 0;   // past the end reads 0, never off it
    }

private:
    /// The persistence control as a half-life in milliseconds, squared so the short range gets the travel.
    uint32_t halfLifeMs() const {
        return 20u + static_cast<uint32_t>(persistence) * static_cast<uint32_t>(persistence) / 16u;
    }

    /// The flow in sub-pixels: one noise field at two offsets, since one read slides the diagonal.
    void flowAt(lengthType x, lengthType y, lengthType z, uint32_t t, uint32_t cells,
                int32_t strength, uint32_t step, draw::pos_t& vx, draw::pos_t& vy) const {
        const uint32_t fx = static_cast<uint32_t>(x) * cells;
        const uint32_t fy = static_cast<uint32_t>(y) * cells;
        const uint32_t fz = static_cast<uint32_t>(z) * cells + t / 4u;
        // Centered on zero, so the field pushes both ways rather than only along the axes.
        const int32_t nx = static_cast<int32_t>(inoise16(fx, fy, fz)) - 32768;
        const int32_t ny = static_cast<int32_t>(inoise16(fx + 0x9E37u, fy + 0x7C15u, fz)) - 32768;
        // strength is the breathing multiplier in 1/256ths; step is the frame's travel budget.
        const int32_t amp = static_cast<int32_t>(step) * strength / 256;
        // 64-bit product: `amp` grows with a stalled dt, and a 32-bit multiply wraps and reverses the flow.
        vx = static_cast<draw::pos_t>((static_cast<int64_t>(nx) * amp) >> 15);
        vy = static_cast<draw::pos_t>((static_cast<int64_t>(ny) * amp) >> 15);
    }

    /// The heads: bright points on a time budget, at full brightness since `writeWide` sets rather than adds.
    void emitDots(uint16_t* plane, lengthType w, lengthType h, lengthType d, uint32_t dt) {
        // `dots` is a density: heads that fill a 64x64 panel are lost on a wall, so they scale.
        const lengthType span = w > h ? w : h;
        const lengthType rad = spanRadius(span);
        const uint32_t area = static_cast<uint32_t>(rad * 2 + 1) * (rad * 2 + 1);
        const uint8_t want = dots < 1 ? 1 : (dots > kMaxDots ? kMaxDots : dots);
        uint32_t scaled = static_cast<uint32_t>(want) * w * h / (4096u * (area > 8u ? area / 9u : 1u));
        if (scaled < 1) scaled = 1;
        if (scaled > kMaxEmitters) scaled = kMaxEmitters;
        const uint8_t n = static_cast<uint8_t>(scaled);
        constexpr uint32_t kReferenceMs = 20;
        emitCarry_ += dt;
        // One batch per reference frame at most: each head is a disc, so a stall would flood.
        if (emitCarry_ < kReferenceMs) return;
        emitCarry_ %= kReferenceMs;
        const uint32_t walk = bank_.unitValue(0);
        for (uint8_t i = 0; i < n; i++) {
            // A Lissajous walk: each dot rides the clock at its own ratio, so they visit the whole grid.
            const angle16 a = static_cast<angle16>(walk + i * (65536u / n));
            const angle16 b = static_cast<angle16>(walk * 3u + i * 9973u);
            const lengthType px = mapAxis(sin16(a), w);
            const lengthType py = mapAxis(cos16(b), h);
            const lengthType pz = d > 1 ? mapAxis(sin16(static_cast<angle16>(b * 2u)), d) : 0;
            const RGB c = colorFromPalette(*Palettes::active(),
                                           static_cast<uint8_t>(i * (255u / n) + (walk >> 9)));
            // Written at the plane's width, so the decay has somewhere to go below the top 8 bits.
            writeWide(plane, w, h, d, px, py, pz, c, rad);
        }
    }

    /// A signed 16-bit sine mapped onto an axis, centered, with the ends reachable.
    static lengthType mapAxis(int16_t s, lengthType extent) {
        if (extent <= 1) return 0;
        const int32_t v = (static_cast<int32_t>(s) + 32768) * (extent - 1) / 65535;
        return static_cast<lengthType>(v);
    }

    /// One light at the plane's width: the byte repeats, so 255 widens to 65535 rather than 65280.
    static void writeWide(uint16_t* plane, lengthType w, lengthType h, lengthType d,
                          lengthType x, lengthType y, lengthType z, RGB c, lengthType rad) {
        // A disc, not a pixel: advection spreads a head bilinearly, so a single pixel arrives as a smear.
        const lengthType rz = d > 1 ? rad : 0;
        for (lengthType dz = -rz; dz <= rz; dz++)
            for (lengthType dy = -rad; dy <= rad; dy++)
                for (lengthType dx = -rad; dx <= rad; dx++) {
                    if (dx * dx + dy * dy + dz * dz > rad * rad) continue;
                    const lengthType px = x + dx, py = y + dy, pz = z + dz;
                    if (px < 0 || py < 0 || pz < 0 || px >= w || py >= h || pz >= d) continue;
                    const size_t off = (static_cast<size_t>(pz) * h * w
                                      + static_cast<size_t>(py) * w + px) * 3;
                    plane[off + 0] = static_cast<uint16_t>((c.r << 8) | c.r);
                    plane[off + 1] = static_cast<uint16_t>((c.g << 8) | c.g);
                    plane[off + 2] = static_cast<uint16_t>((c.b << 8) | c.b);
                }
    }


    ScratchBuffer<uint16_t> plane_{*this};     ///< the trail itself, three samples per light
    ScratchBuffer<uint16_t> scratch_{*this};   ///< advect's destination; the two alternate roles
    ScratchBuffer<uint8_t>  carry_{*this};     ///< the dither's per-channel error
    bool                    front_ = true;     ///< which of the two currently holds the trail
    OscillatorBank<2>       bank_;
    lengthType              planeW_ = 0, planeH_ = 0, planeD_ = 0;
    uint32_t                lastMs_ = 0;
    bool                    started_ = false;   ///< false until a frame has been timed
    uint32_t                emitCarry_ = 0;     ///< time owed to the emitters, in ms
};

}  // namespace mm
