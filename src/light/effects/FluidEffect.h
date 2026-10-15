#pragma once

#include "light/effects/EffectBase.h"
#include "light/powerfunctions/fluid.h"               // the Stam solver: the medium itself

namespace mm {

/// Light poured into a simulated medium and carried by it.
/// @card FluidEffect.gif
///
/// Every other flow here is a function of position and time. This one is state, so a jet fired now changes where everything downstream goes for seconds afterwards. On a cube every depth slice is its own medium, so the slices differ. Sized for the desktop and the P4; an S3 runs it on a small grid.
///
/// @attribution Jos Stam
///
/// @technical
///
/// `Fluid` solves the medium, and `light/powerfunctions/fluid.h` says why that algorithm and why Q16.16. The dye plane is 16-bit because a value multiplied by slightly less than one, many times a second, has nowhere to go at 8 bits.
class FluidEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫🖌️💨"; }
    /// Volumetric: a medium per slice, with the jets wandering in z.
    Dim dimensions() const override { return Dim::D3; }

    /// The jet ceiling, and the width of the oscillator bank.
    static constexpr uint8_t kMaxJets = 4;
    static constexpr uint8_t kClocks = 4;   ///< per jet: position, reach, aim, depth

    /// How many places light is poured in.
    uint8_t jets        = 2;
    /// How hard each jet pushes the medium.
    uint8_t force       = 120;
    /// How fast the jets sweep, which is what stirs vortices.
    uint8_t swirl       = 90;
    /// How much the medium drags on itself; high is syrup, low smoke.
    uint8_t viscosity   = 20;
    /// How long dye survives, as a half-life.
    uint8_t persistence = 150;
    /// Pressure-solve effort, and the cost knob.
    uint8_t iterations  = 5;

    /// Publish the jets, the medium's viscosity, the dye's half-life and the solver's effort.
    void defineControls() override {
        controls_.addControl("jets", jets, 1, kMaxJets);
        controls_.addControl("force", force, 0, 255);
        controls_.addControl("swirl", swirl, 0, 255);
        controls_.addControl("viscosity", viscosity, 0, 255);
        controls_.addControl("persistence", persistence, 0, 255);
        controls_.addControl("iterations", iterations, 1, 20);
    }

    /// Size the medium, both dye planes and the dither error, clearing them when the shape changed.
    void prepare() override {
        const lengthType w = width(), h = height(), d = depth();
        // Every slice is its own medium: a volumetric solve is a different solver.
        const bool medium = fluid_.resize(w, h, d);
        const size_t n = medium ? static_cast<size_t>(w) * h * d * 3 : 0;
        const size_t had = dyeA_.count();
        dyeA_.resize(n);
        dyeB_.resize(n);
        carry_.resize(n);        // the dither's error, sized here: tick() is MM_NONBLOCKING
        // Same count, new shape: both planes hold the old geometry's samples, so clear both.
        if (n > 0 && n == had && (w != planeW_ || h != planeH_ || d != planeD_)) {
            std::memset(dyeA_.data(), 0, dyeA_.bytes());
            std::memset(dyeB_.data(), 0, dyeB_.bytes());
            // The dither's error is per light, so it carries the old geometry too.
            if (carry_) std::memset(carry_.data(), 0, carry_.bytes());
        }
        planeW_ = w; planeH_ = h; planeD_ = d;
        started_ = false;
        pourCarry_ = 0;
        poured_ = false;
    }

    /// Pour the jets, solve the medium, then carry the dye along the field it worked out.
    void tick() MM_NONBLOCKING override {
        // Read every frame, never cached: release() can free these between a disable and its prepare.
        if (!fluid_.valid() || !dyeA_ || !dyeB_) return;
        const lengthType w = width(), h = height(), d = depth();
        const uint32_t now = elapsed();
        // A zero delta on the first tick: the whole uptime would push the solver in one step.
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        // Four clocks a jet, at rates sharing no multiple, so the jets never march together.
        for (uint8_t i = 0; i < kMaxJets; i++) {
            const uint16_t rate = static_cast<uint16_t>(3 + swirl / 12 + i * 2);
            bank_.set(static_cast<uint8_t>(i * kClocks + 0),
                      {.rate = rate, .low = 0, .high = 65535,
                       .phaseOffset = static_cast<angle16>(i * 16384), .wave = Wave::Saw});
            // The radius breathes out to the wall, so forcing covers the panel rather than a band.
            bank_.set(static_cast<uint8_t>(i * kClocks + 1),
                      {.rate = static_cast<uint16_t>(rate / 3 + 1), .low = 0, .high = 255,
                       .phaseOffset = static_cast<angle16>(i * 9000), .wave = Wave::Sine});
            // The aim swings around the tangent, so a jet drives across the middle.
            bank_.set(static_cast<uint8_t>(i * kClocks + 2),
                      {.rate = static_cast<uint16_t>(rate / 2 + 1), .low = 0, .high = 65535,
                       .phaseOffset = static_cast<angle16>(i * 21000), .wave = Wave::Sine});
            // And on a cube the jet drifts through the slices, so each slice is visited in turn.
            bank_.set(static_cast<uint8_t>(i * kClocks + 3),
                      {.rate = static_cast<uint16_t>(rate / 4 + 1), .low = 0, .high = 65535,
                       .phaseOffset = static_cast<angle16>(i * 30000), .wave = Wave::Sine});
        }
        // advanceTo() takes an absolute timestamp and computes its own delta, so never pass dt.
        bank_.advanceTo(now);

        uint16_t* live = front_ ? dyeA_.data() : dyeB_.data();
        uint16_t* spare = front_ ? dyeB_.data() : dyeA_.data();

        // Paced by time: a jet firing per frame pours at the frame rate, and dt scaling reads dark.
        constexpr uint32_t kPourMs = 20;
        pourCarry_ += dt;
        // Unconditional, since dt is 0 on the opening tick and the panel would stay black.
        if (!poured_) {
            poured_ = true;                       // the opening pour, see above
            pour(live, w, h, d, now);
        }
        // Subtracted rather than modulo, and capped: a stall would pour seconds of jets at once.
        constexpr uint32_t kMaxCatchUp = 4;                  // 80 ms of jets in one frame, at most
        if (pourCarry_ > kPourMs * kMaxCatchUp) pourCarry_ = kPourMs * kMaxCatchUp;
        while (pourCarry_ >= kPourMs) {
            pourCarry_ -= kPourMs;
            pour(live, w, h, d, now);
        }

        // The medium first, or the dye follows the previous frame's flow.
        const int32_t dtQ = static_cast<int32_t>((static_cast<uint64_t>(dt) * Fluid::kOne) / 1000u);
        fluid_.step(static_cast<int32_t>(viscosity) * (Fluid::kOne / 4096), dtQ, iterations);

        const int32_t* vx = fluid_.velocityX();
        const int32_t* vy = fluid_.velocityY();
        const size_t plane = fluid_.plane();
        draw::advect16(spare, live, w, h, d,
                       [&](lengthType x, lengthType y, lengthType z, draw::pos_t& ox, draw::pos_t& oy) {
                           const size_t i = static_cast<size_t>(z) * plane + static_cast<size_t>(y) * w + x;
                           // Cells a second into sub-pixels, where draw::pos_t counts 256 to a light.
                           ox = static_cast<draw::pos_t>((static_cast<int64_t>(vx[i]) * dtQ) >> 24);
                           oy = static_cast<draw::pos_t>((static_cast<int64_t>(vy[i]) * dtQ) >> 24);
                       }, draw::Edge::Clamp);
        front_ = !front_;
        uint16_t* dye = front_ ? dyeA_.data() : dyeB_.data();

        draw::decay16(dye, dyeA_.count(), 40u + static_cast<uint32_t>(persistence) * persistence / 10u, dt);
        draw::blit16(canvas(), dye, w, h, d, carry_ ? carry_.data() : nullptr);
    }

    /// Test seam: how many dye samples both planes hold, since a rendered frame cannot show a stale plane.
    std::size_t dyeSamples() const { return dyeA_.count() + dyeB_.count(); }
    /// Test seam: read one dye sample across both planes, where past the end reads 0.
    uint16_t dyeAt(std::size_t i) const {
        const std::size_t n = dyeA_.count();
        if (i < n) return dyeA_.data()[i];
        const std::size_t j = i - n;
        return j < dyeB_.count() ? dyeB_.data()[j] : 0;   // past the end reads 0, never off it
    }

private:
    /// The jets: velocity and dye together, so light enters where the medium is pushed.
    void pour(uint16_t* dye, lengthType w, lengthType h, lengthType d, uint32_t now) {
        const uint8_t n = jets < 1 ? 1 : (jets > kMaxJets ? kMaxJets : jets);
        for (uint8_t i = 0; i < n; i++) {
            const angle16 a = static_cast<angle16>(bank_.unitValue(static_cast<uint8_t>(i * kClocks + 0)));
            // The radius wanders between center and wall rather than sitting on one circle.
            const uint32_t reach = bank_.value(static_cast<uint8_t>(i * kClocks + 1));
            const int32_t rx = static_cast<int32_t>((w / 2 - 1) * reach / 320u);
            const int32_t ry = static_cast<int32_t>((h / 2 - 1) * reach / 320u);
            const lengthType jx = static_cast<lengthType>(w / 2 + (static_cast<int32_t>(cos16(a)) * rx) / 32768);
            const lengthType jy = static_cast<lengthType>(h / 2 + (static_cast<int32_t>(sin16(a)) * ry) / 32768);
            // Either side of the tangent, since a pure tangent produced a hollow ring.
            const int32_t lean = static_cast<int32_t>(bank_.value(static_cast<uint8_t>(i * kClocks + 2))) - 32768;
            const lengthType jz = d > 1
                ? static_cast<lengthType>((bank_.value(static_cast<uint8_t>(i * kClocks + 3)) * (d - 1)) / 65535u)
                : 0;
            // Odd jets sweep the other way, since colliding jets are what roll up vortex pairs.
            const int32_t sense = (i & 1) ? -1 : 1;
            const angle16 dir = static_cast<angle16>(a + static_cast<angle16>(sense * (16384 + lean / 3)));
            // The push scales with the fixture: a single-cell push is mostly divergence, which projection removes.
            const lengthType span = w > h ? w : h;
            const lengthType rad = span / 32 + 1;
            const lengthType rz = d > 1 ? rad : 0;             // a sphere on a cube, a disc on a panel
            const int32_t mag = static_cast<int32_t>(
                (static_cast<int64_t>(force) * span * Fluid::kOne) / (255 * 2));
            const int32_t dvx = static_cast<int32_t>((static_cast<int64_t>(cos16(dir)) * mag) >> 15);
            const int32_t dvy = static_cast<int32_t>((static_cast<int64_t>(sin16(dir)) * mag) >> 15);
            for (lengthType dz = -rz; dz <= rz; dz++)
                for (lengthType dy = -rad; dy <= rad; dy++)
                    for (lengthType dx = -rad; dx <= rad; dx++)
                        if (dx * dx + dy * dy + dz * dz <= rad * rad)
                            fluid_.addVelocity(jx + dx, jy + dy, dvx, dvy, jz + dz);

            const RGB c = colorFromPalette(*Palettes::active(),
                                           static_cast<uint8_t>(i * (255u / n) + (now >> 6)));
            splatDye(dye, w, h, d, jx, jy, jz, c);
        }
    }

    /// Dye into a soft disc at the plane's full width, so a jet has a body the flow can carry.
    void splatDye(uint16_t* dye, lengthType w, lengthType h, lengthType d,
                  lengthType cx, lengthType cy, lengthType cz, RGB c) {
        // The body scales with the fixture, or advection spreads a fixed count into a smear.
        const lengthType span = w > h ? w : h;
        const lengthType rad = span / 32 + 1;
        const lengthType rz = d > 1 ? rad : 0;                 // a sphere on a cube, a disc on a panel
        const int32_t r2 = static_cast<int32_t>(rad) * rad;
        const uint16_t wide[3] = {static_cast<uint16_t>((c.r << 8) | c.r),
                                  static_cast<uint16_t>((c.g << 8) | c.g),
                                  static_cast<uint16_t>((c.b << 8) | c.b)};
        for (lengthType dz = -rz; dz <= rz; dz++)
          for (lengthType dy = -rad; dy <= rad; dy++)
            for (lengthType dx = -rad; dx <= rad; dx++) {
                // A disc, not a square: advection carries a slab's corners into blocky patches.
                const int32_t d2 = static_cast<int32_t>(dx) * dx + static_cast<int32_t>(dy) * dy
                                 + static_cast<int32_t>(dz) * dz;
                if (d2 > r2) continue;
                const lengthType x = cx + dx, y = cy + dy, z = cz + dz;
                if (x < 0 || y < 0 || z < 0 || x >= w || y >= h || z >= d) continue;
                // Tapering to the rim, since a flat slab keeps its hard edge however far it travels.
                const uint32_t fall = static_cast<uint32_t>(((r2 - d2) * 255) / (r2 > 0 ? r2 : 1));
                const size_t o = ((static_cast<size_t>(z) * h + y) * w + x) * 3;
                for (size_t k = 0; k < 3; k++) {
                    // Added, not assigned, so a jet builds where it lingers and blends where two meet.
                    const uint32_t add = (static_cast<uint32_t>(wide[k]) * fall) / 255u;
                    const uint32_t sum = static_cast<uint32_t>(dye[o + k]) + add;
                    dye[o + k] = static_cast<uint16_t>(sum > 65535u ? 65535u : sum);
                }
            }
    }


    Fluid                   fluid_{*this};      ///< the medium itself, one solve per slice
    ScratchBuffer<uint16_t> dyeA_{*this};     ///< the dye; the two alternate roles
    ScratchBuffer<uint16_t> dyeB_{*this};
    ScratchBuffer<uint8_t>  carry_{*this};    ///< the dither's per-channel error
    OscillatorBank<kMaxJets * kClocks> bank_;   ///< four clocks a jet
    bool                    front_ = true;      ///< which dye plane currently holds the picture
    bool                    started_ = false;   ///< false until a frame has been timed
    uint32_t                lastMs_ = 0;        ///< the previous frame's timestamp
    uint32_t                pourCarry_ = 0;   ///< time owed to the jets, in ms
    lengthType              planeW_ = 0, planeH_ = 0, planeD_ = 0;   ///< the shape the planes hold
    bool                    poured_ = false;  ///< has the opening pour happened yet
};

}  // namespace mm
