#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Emitters pouring color into a flow that carries and folds it.
/// @card ColorTrailsEffect.gif
///
/// There is no velocity field: one noise value shifts each row sideways, one shifts each column, and the two shears compose into a swirling current. A large grid is steered by a few hundred numbers, which is why it runs where a solver does not. Compare with @ref FluidEffect: the solver when the medium is the subject, this when the color is.
///
/// @attribution Stefan Petrick, 4wheeljive
///
/// @moreinfo
///
/// `inoise16` fills the profiles, `draw::advect16` carries a 16-bit plane and `draw::decay16` fades it, since a value multiplied by slightly less than one dies early or never fades at 8 bits. Both shears read the undisplaced source in one fused pass.
class ColorTrailsEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🖌️💨🌫️"; }
    /// Volumetric: the two profiles steer every slice.
    Dim dimensions() const override { return Dim::D3; }

    /// Which emitters pour color in. `All` is the intended picture, the rest show one at a time.
    enum class Mode : uint8_t { All = 0, Orbital, Lissajous, Border };

    /// How fast the emitters travel.
    uint8_t speed       = 60;
    /// How far a row or column is pushed: the shear amount.
    uint8_t flow        = 128;
    /// How fast the flow itself drifts and reverses.
    uint8_t flowSpeed   = 128;
    /// The profiles' spatial frequency: a few broad bands or many fine ones.
    uint8_t scale       = 85;
    /// How long color survives, as a half-life.
    uint8_t persistence = 60;
    /// How fast the emitters walk the palette.
    uint8_t colorSpeed  = 128;
    /// The orbit's radius and the Lissajous figure's reach.
    uint8_t size        = 128;
    /// All three emitters, or one at a time to see what each adds.
    uint8_t mode        = static_cast<uint8_t>(Mode::All);

    /// Which wind carries the color. `Radial` runs from geometry, so it costs no field and no state.
    enum class Flow : uint8_t { Noise = 0, Radial, RadialIn };
    /// The field carrying them: Noise, Radial out or Radial in.
    uint8_t flowType = static_cast<uint8_t>(Flow::Noise);

    /// Publish the emitters, the flow, the palette walk and the tail's half-life.
    void defineControls() override {
        controls_.addControl("speed", speed, 0, 255);
        controls_.addControl("flow", flow, 0, 255);
        controls_.addControl("flowSpeed", flowSpeed, 0, 255);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("persistence", persistence, 0, 255);
        controls_.addControl("colorSpeed", colorSpeed, 0, 255);
        controls_.addControl("size", size, 0, 255);
        controls_.addSelect("mode", mode, kModes, 4);
        controls_.addSelect("flowType", flowType, kFlows, 3);
    }

    /// Size the wide plane, its spare, the dither error and the two profiles, and clear them.
    void prepare() override {
        const size_t needed = static_cast<size_t>(width()) * height() * depth() * 3u;
        plane_.resize(needed);
        scratch_.resize(needed);
        carry_.resize(needed);
        // The profiles are the whole velocity field: one value per column, one per row.
        xProf_.resize(static_cast<size_t>(width()));
        yProf_.resize(static_cast<size_t>(height()));
        if (plane_)   std::memset(plane_.data(), 0, plane_.bytes());
        if (scratch_) std::memset(scratch_.data(), 0, scratch_.bytes());
        // resize() keeps old contents at an unchanged size, so the dither's error needs clearing too.
        if (carry_)   std::memset(carry_.data(), 0, carry_.bytes());
        started_ = false;
        front_ = true;
    }

    /// Walk both clocks, shear the trail plane in one backward sample, and color it from the palette.
    void tick() MM_NONBLOCKING override {
        if (!plane_ || !scratch_ || !xProf_ || !yProf_) return;
        const lengthType w = width(), h = height(), d = depth();
        const uint32_t now = elapsed();
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        // Both clocks scale from `speed`, and 0 stops them. Their rates differ so they drift apart.
        bank_.set(0, {.rate = static_cast<uint16_t>(speed / 4), .low = 0, .high = 65535,
                      .phaseOffset = 0, .wave = Wave::Saw});
        bank_.set(1, {.rate = static_cast<uint16_t>((speed * 3u) / 22u), .low = 0, .high = 65535,
                      .phaseOffset = 16384, .wave = Wave::Saw});
        // advanceTo() takes an absolute timestamp and computes its own delta, so never pass dt.
        bank_.advanceTo(now);
        // `speed` gates the palette walk as well, so stopping the effect stops the color.
        huePhase_ += (dt * static_cast<uint32_t>(colorSpeed) * static_cast<uint32_t>(speed)) / 24000u;

        sampleProfiles(w, h, now);

        ScratchBuffer<uint16_t>& src = front_ ? plane_ : scratch_;
        ScratchBuffer<uint16_t>& dst = front_ ? scratch_ : plane_;

        // Both shears in one backward sample, which is what keeps this to a single pass.
        const Flow f = static_cast<Flow>(flowType);
        if (f == Flow::Noise) {
            const int32_t* xp = xProf_.data();
            const int32_t* yp = yProf_.data();
            draw::advect16(dst.data(), src.data(), w, h, d,
                           [xp, yp](lengthType x, lengthType y, lengthType,
                                    draw::pos_t& vx, draw::pos_t& vy) {
                               vx = static_cast<draw::pos_t>(yp[y]);
                               vy = static_cast<draw::pos_t>(xp[x]);
                           }, draw::Edge::Wrap);
        } else {
            // Radial: the normalized offset from the center, signed for a fountain or a drain.
            const int32_t cx = w / 2, cy = h / 2;
            const int32_t reach = (static_cast<int32_t>(flow) * draw::kSubOne) / 24;
            const int32_t sign = (f == Flow::Radial) ? 1 : -1;
            draw::advect16(dst.data(), src.data(), w, h, d,
                           [cx, cy, reach, sign](lengthType x, lengthType y, lengthType,
                                                 draw::pos_t& vx, draw::pos_t& vy) {
                               const int32_t dx = static_cast<int32_t>(x) - cx;
                               const int32_t dy = static_cast<int32_t>(y) - cy;
                               // Floored at 1: the center is the one cell whose direction is undefined.
                               const int32_t r = static_cast<int32_t>(isqrt(
                                   static_cast<uint32_t>(dx * dx + dy * dy)));
                               const int32_t rr = r < 1 ? 1 : r;
                               vx = static_cast<draw::pos_t>((dx * reach * sign) / rr);
                               vy = static_cast<draw::pos_t>((dy * reach * sign) / rr);
                           }, draw::Edge::Clamp);
        }
        front_ = !front_;
        ScratchBuffer<uint16_t>& moved = front_ ? plane_ : scratch_;

        draw::decay16(moved.data(), moved.count(), halfLifeMs(), dt);
        emit(moved.data(), w, h, d);
        draw::blit16(canvas(), moved.data(), w, h, d, carry_ ? carry_.data() : nullptr);
    }

private:
    static constexpr const char* kModes[4] = {"All", "Orbital", "Lissajous", "Border"};
    static constexpr const char* kFlows[3] = {"Noise", "Radial out", "Radial in"};

    /// The persistence control as a half-life in milliseconds, squared so the short range gets the travel.
    uint32_t halfLifeMs() const {
        return 20u + static_cast<uint32_t>(persistence) * static_cast<uint32_t>(persistence) / 16u;
    }

    /// Fill the profiles, the whole velocity field: W+H noise samples a frame rather than W*H.
    void sampleProfiles(lengthType w, lengthType h, uint32_t now) {
        const uint32_t phase = (now * static_cast<uint32_t>(flowSpeed)) / 24u;
        // The shear scales with the grid: a fixed pixel count is invisible on a wide panel.
        const int32_t reach = (static_cast<int32_t>(flow) * (w < h ? w : h)) / 220;
        // Breathing per axis at unequal rates, bounded off zero, which would collapse the flow.
        const uint32_t mPhase = (now * static_cast<uint32_t>(flowSpeed)) / 96u;
        const int32_t xBreath = 205 + (static_cast<int32_t>(sin16(static_cast<uint16_t>(mPhase * 7u))) * 102) / 32768;
        const int32_t yBreath = 205 + (static_cast<int32_t>(sin16(static_cast<uint16_t>(mPhase * 5u + 21845u))) * 102) / 32768;
        const int32_t xReach = (reach * xBreath) / 256;
        const int32_t yReach = (reach * yBreath) / 256;
        // Detuned rates: equal ones lock the profiles and slide a diagonal.
        const uint32_t cells = static_cast<uint32_t>(scale) * 24u;
        const uint32_t cellsY = (cells * 32u) / 33u;
        const uint32_t phaseY = (phase * 172u) / 173u;
        for (lengthType x = 0; x < w; x++) {
            const int32_t n = static_cast<int32_t>(inoise16(static_cast<uint32_t>(x) * cells, phase, 0)) - 32768;
            xProf_[static_cast<size_t>(x)] = (n * xReach * draw::kSubOne) / 32768 / 16;
        }
        for (lengthType y = 0; y < h; y++) {
            // A different offset, so the axes are decoupled rather than rising and falling together.
            const int32_t n = static_cast<int32_t>(inoise16(static_cast<uint32_t>(y) * cellsY, phaseY, 32768)) - 32768;
            yProf_[static_cast<size_t>(y)] = (n * yReach * draw::kSubOne) / 32768 / 16;
        }
    }

    /// The emitters, drawn after the transport so this frame's color is sharp.
    void emit(uint16_t* p, lengthType w, lengthType h, lengthType d) {
        const Mode m = static_cast<Mode>(mode);
        // Full brightness every frame: time moves an emitter rather than dimming it, and decay fades.
        const uint8_t hue = static_cast<uint8_t>(huePhase_);
        const int32_t half = (w < h ? w : h) / 2;
        const int32_t radius = (half * static_cast<int32_t>(size)) / 300 + 1;
        const lengthType dot = static_cast<lengthType>((w < h ? w : h) / 24 + 1);

        if (m == Mode::All || m == Mode::Orbital) {
            // Three circles on one orbit, spaced a third apart, each a third of the palette along.
            for (uint8_t i = 0; i < 3; i++) {
                const angle16 a = static_cast<angle16>(bank_.phase(0) + i * 21845);
                const lengthType cx = static_cast<lengthType>(w / 2 + (static_cast<int32_t>(cos16(a)) * radius) / 32768);
                const lengthType cy = static_cast<lengthType>(h / 2 + (static_cast<int32_t>(sin16(a)) * radius) / 32768);
                splat(p, w, h, d, cx, cy, dot, static_cast<uint8_t>(hue + i * 85));
            }
        }
        if (m == Mode::All || m == Mode::Lissajous) {
            // Two sines at 3:2, a figure that repeats each base turn, the flow carrying the trail.
            const angle16 a = static_cast<angle16>(bank_.phase(1));
            const angle16 b = static_cast<angle16>(bank_.phase(1) * 3u / 2u);
            // `size` scales the reach, the control the orbit reads, so both emitters grow together.
            const int32_t reachX = ((w / 2 - 1) * static_cast<int32_t>(size)) / 255;
            const int32_t reachY = ((h / 2 - 1) * static_cast<int32_t>(size)) / 255;
            const lengthType cx = static_cast<lengthType>(w / 2 + (static_cast<int32_t>(sin16(a)) * reachX) / 32768);
            const lengthType cy = static_cast<lengthType>(h / 2 + (static_cast<int32_t>(cos16(b)) * reachY) / 32768);
            splat(p, w, h, d, cx, cy, dot, static_cast<uint8_t>(hue + 128));
        }
        if (m == Mode::All || m == Mode::Border) {
            // The rim, hue walking the perimeter: the flow pulls it inward, feeding the picture.
            for (lengthType x = 0; x < w; x++) {
                const uint8_t c = static_cast<uint8_t>(hue + (x * 255) / (w > 1 ? w : 1));
                put(p, w, h, d, x, 0, c, 255);
                put(p, w, h, d, x, static_cast<lengthType>(h - 1), c, 255);
            }
            for (lengthType y = 0; y < h; y++) {
                const uint8_t c = static_cast<uint8_t>(hue + 128 + (y * 255) / (h > 1 ? h : 1));
                put(p, w, h, d, 0, y, c, 255);
                put(p, w, h, d, static_cast<lengthType>(w - 1), y, c, 255);
            }
        }
    }

    /// A soft round emitter, brightest at its center.
    void splat(uint16_t* p, lengthType w, lengthType h, lengthType d,
               lengthType cx, lengthType cy, lengthType r, uint8_t index) {
        const int32_t r2 = static_cast<int32_t>(r) * r;
        for (lengthType dy = -r; dy <= r; dy++)
            for (lengthType dx = -r; dx <= r; dx++) {
                const int32_t q = static_cast<int32_t>(dx) * dx + static_cast<int32_t>(dy) * dy;
                if (q > r2) continue;
                const uint8_t bri = static_cast<uint8_t>(255 - (q * 255) / (r2 > 0 ? r2 : 1));
                put(p, w, h, d, static_cast<lengthType>(cx + dx), static_cast<lengthType>(cy + dy), index, bri);
            }
    }

    /// Write one light, brighter of the two: a sum would pin a repeatedly covered light at white.
    void put(uint16_t* p, lengthType w, lengthType h, lengthType d,
             lengthType x, lengthType y, uint8_t index, uint8_t bri) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        const RGB c = colorFromPalette(*Palettes::active(), index, bri);
        const uint8_t ch[3] = {c.r, c.g, c.b};
        for (lengthType z = 0; z < d; z++) {
            const size_t o = ((static_cast<size_t>(z) * h + y) * w + x) * 3u;
            for (uint8_t k = 0; k < 3; k++) {
                const uint16_t v = static_cast<uint16_t>(static_cast<uint32_t>(ch[k]) << 8);
                if (v > p[o + k]) p[o + k] = v;
            }
        }
    }

    ScratchBuffer<uint16_t> plane_{*this};     ///< the color itself, three samples per light
    ScratchBuffer<uint16_t> scratch_{*this};   ///< advect's destination; the two alternate roles
    ScratchBuffer<uint8_t>  carry_{*this};     ///< the dither's per-channel error
    ScratchBuffer<int32_t>  xProf_{*this};     ///< one shift per column: the vertical flow
    ScratchBuffer<int32_t>  yProf_{*this};     ///< one shift per row: the horizontal flow
    OscillatorBank<2>       bank_;
    bool     front_ = true, started_ = false;
    uint32_t lastMs_ = 0, huePhase_ = 0;
};

}  // namespace mm
