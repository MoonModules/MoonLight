#pragma once

#include "light/effects/EffectBase.h"   // and core/noise.h with it: fbm8, curl16


namespace mm {

/// A noise field decides where light is born, a curl flow decides where it goes, and the cloud keeps folding into itself.
/// @card NebulaEffect.gif
///
/// The emitter is a field rather than a handful of dots, so light enters everywhere at once and the flow shapes a whole cloud instead of drawing trails. The cloud is held at 16 bits and dithered on the way out, which keeps a slow fade smooth.
///
/// @attribution Stefan Petrick, Robert Bridson
///
/// @guide
///
/// ## Cheaper fields
///
/// `fieldScale` and `fieldRate` make the field cheaper, smaller or less often computed. That costs detail, not smoothness, since the flow still carries the cloud every frame.
///
/// @moreinfo
///
/// The cost is one curl per light per frame, and one fbm per light at whatever the two levers ask for. The curl flow is divergence-free, so nothing piles up.
class NebulaEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫🖌️💨🌫️"; }
    /// Volumetric: the flow and the field both carry z.
    Dim dimensions() const override { return Dim::D3; }

    /// How fast the medium moves, and with it the whole cloud.
    uint8_t speed       = 40;
    /// The field's cell size: low is broad clouds, high is wisps.
    uint8_t scale       = 40;
    /// What fraction of the field is bright enough to be born.
    uint8_t contrast    = 192;
    /// How long light survives in the flow, as a half-life.
    uint8_t persistence = 140;
    /// Detail within the field, and its cost knob.
    uint8_t octaves     = 2;
    /// Compute the field at a fraction of the grid and stretch it.
    uint8_t fieldScale  = 1;
    /// Recompute the field every N frames.
    uint8_t fieldRate   = 1;

    /// Publish the flow, the field's shape, the birth window and the two cost levers.
    void defineControls() override {
        controls_.addControl("speed", speed, 0, 255);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("contrast", contrast, 0, 255);
        controls_.addControl("persistence", persistence, 0, 255);
        controls_.addControl("octaves", octaves, 1, 4);
        controls_.addControl("fieldScale", fieldScale, 1, 4);
        controls_.addControl("fieldRate", fieldRate, 1, 8);
    }

    /// Only `fieldScale` resizes anything, so only it forces a rebuild.
    bool affectsPrepare(const char* name) const override {
        // The rest are read per frame, and `fieldRate` only skips them.
        return std::strcmp(name, "fieldScale") == 0;
    }

    /// The lever, clamped once: prepare() sizes the field from it and tick() picks its path by it.
    uint8_t fieldScaleClamped() const { return fieldScale < 1 ? 1 : (fieldScale > 4 ? 4 : fieldScale); }

    /// Size both cloud planes, the field at its own resolution, and everything the render path writes.
    void prepare() override {
        const lengthType w = width(), h = height(), d = depth();
        const size_t n = static_cast<size_t>(w) * h * d * 3;
        const size_t had = planeA_.count();
        // The cloud itself: two planes, because advection reads one and writes the other.
        planeA_.resize(n);
        planeB_.resize(n);
        // Sized here because tick() is MM_NONBLOCKING and a resize is heap work in the hot path.
        carry_.resize(n);
        scratch_.resize(fieldScaleClamped() > 1 ? n : 0);
        taps_.resize(static_cast<size_t>(w));
        // Same count, new shape: both planes hold the old geometry's samples, so clear both.
        if (n > 0 && n == had && (w != planeW_ || h != planeH_ || d != planeD_)) {
            std::memset(planeA_.data(), 0, planeA_.bytes());
            std::memset(planeB_.data(), 0, planeB_.bytes());
        }
        // At fieldScale 1 the field is the fixture, so the lever costs nothing when it is off.
        const uint8_t s = fieldScaleClamped();
        fw_ = w / s > 0 ? w / s : 1;
        fh_ = h / s > 0 ? h / s : 1;
        fd_ = d / s > 0 ? d / s : 1;
        field_.resize(s > 1 ? static_cast<size_t>(fw_) * fh_ * fd_ * 3 : 0);
        planeW_ = w; planeH_ = h; planeD_ = d;
        frame_ = 0;
        started_ = false;
    }

    /// Carry the cloud along the curl flow, decay it, then birth new light into it.
    void tick() MM_NONBLOCKING override {
        if (!planeA_ || !planeB_) return;
        const lengthType w = width(), h = height(), d = depth();
        // A zero delta on the first tick: `now - 0` would hand the flow the whole uptime.
        const uint32_t now = elapsed();
        const uint32_t t = now;              // the flow field's third axis, read inside the lambda
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        bank_.set(0, {.rate = static_cast<uint16_t>(4 + speed / 6), .low = 0, .high = 65535,
                      .phaseOffset = 0, .wave = Wave::Saw});
        // advanceTo() takes an absolute timestamp and computes its own delta, so never pass dt.
        bank_.advanceTo(now);

        uint16_t* live = front_ ? planeA_.data() : planeB_.data();
        uint16_t* spare = front_ ? planeB_.data() : planeA_.data();

        // 1. Transport, every frame: this is the motion, and it is what fieldRate must not skip.
        const uint32_t cells = static_cast<uint32_t>(scale) * 256u;
        const int32_t push = static_cast<int32_t>(speed) * static_cast<int32_t>(dt) / 4;
        draw::advect16(spare, live, w, h, d,
                       [&](lengthType x, lengthType y, lengthType z,
                           draw::pos_t& vx, draw::pos_t& vy) {
                           int32_t cx = 0, cy = 0;
                           // eps must reach the next sample, or the cloud comes out striped.
                           curl16(static_cast<uint32_t>(x) * cells,
                                  static_cast<uint32_t>(y) * cells,
                                  static_cast<uint32_t>(z) * cells + t / 8u, push, cx, cy,
                                  cells / 2u);
                           vx = static_cast<draw::pos_t>(cx);
                           vy = static_cast<draw::pos_t>(cy);
                       }, draw::Edge::Clamp);
        front_ = !front_;
        uint16_t* cloud = front_ ? planeA_.data() : planeB_.data();

        // 2. Decay, every frame: the half-life is what makes the cloud a cloud rather than a smear.
        draw::decay16(cloud, planeA_.count(), 40u + static_cast<uint32_t>(persistence) * persistence / 12u, dt);

        // 3. Birth, every fieldRate frames: the expensive half, and the one the levers exist for.
        const uint8_t rate = fieldRate < 1 ? 1 : fieldRate;
        if (frame_ % rate == 0) birth(cloud, w, h, d, now);
        frame_++;

        // 4. Onto the layer, dithered: the cloud's dark half is where banding lives.
        draw::blit16(canvas(), cloud, w, h, d, carry_ ? carry_.data() : nullptr);
    }

private:
    /// Where light is born: the field, thresholded so only its top survives, added to the cloud.
    void birth(uint16_t* cloud, lengthType w, lengthType h, lengthType d, uint32_t now) {
        const uint32_t drift = bank_.unitValue(0) >> 6;
        const uint32_t cells = static_cast<uint32_t>(scale) * 192u;
        const uint8_t oct = octaves < 1 ? 1 : (octaves > 4 ? 4 : octaves);
        // The window rides the field's measured range, spanning its top 60% since this emitter accumulates.
        const uint8_t lowEnd = floor_ < peak_ ? floor_ : static_cast<uint8_t>(peak_ > 8 ? peak_ - 8 : 0);
        const uint32_t span16 = static_cast<uint32_t>(peak_ - lowEnd);
        const uint32_t frac = 154u + (static_cast<uint32_t>(contrast) * 101u) / 255u;   // 60%..100%
        const uint8_t threshold = static_cast<uint8_t>(lowEnd + (span16 * frac) / 255u);
        uint8_t frameMax = 0, frameMin = 255;

        const bool small = field_.count() > 0;
        uint16_t* dst = small ? field_.data() : cloud;
        const lengthType dw = small ? fw_ : w, dh = small ? fh_ : h, dd = small ? fd_ : d;
        const lengthType sx = w / (dw > 0 ? dw : 1), sy = h / (dh > 0 ? dh : 1), sz = d / (dd > 0 ? dd : 1);
        std::size_t i = 0;
        for (lengthType z = 0; z < dd; z++)
            for (lengthType y = 0; y < dh; y++)
                for (lengthType x = 0; x < dw; x++, i += 3) {
                    const uint8_t v = fbm8(static_cast<uint32_t>(x * sx) * cells / 256u + drift,
                                           static_cast<uint32_t>(y * sy) * cells / 256u,
                                           static_cast<uint32_t>(z * sz) * cells / 256u + now / 32u, oct);
                    // Below the window nothing is born, and above it stretches over the full range.
                    if (v > frameMax) frameMax = v;
                    if (v < frameMin) frameMin = v;
                    if (v <= threshold) {
                        if (small) { dst[i] = dst[i + 1] = dst[i + 2] = 0; }
                        continue;
                    }
                    const uint32_t span = peak_ > threshold ? peak_ - threshold : 1u;
                    const uint32_t over = static_cast<uint32_t>(v - threshold) * 255u / span;
                    const uint8_t bri = static_cast<uint8_t>(over > 255u ? 255u : over);
                    // The stretched position within the window, since the raw value spans a fifth of the palette.
                    const RGB c = colorFromPalette(*Palettes::active(), bri, bri);
                    // Written at full width, so the decay has somewhere to go.
                    if (small) {
                        dst[i + 0] = static_cast<uint16_t>((c.r << 8) | c.r);
                        dst[i + 1] = static_cast<uint16_t>((c.g << 8) | c.g);
                        dst[i + 2] = static_cast<uint16_t>((c.b << 8) | c.b);
                    } else {
                        // Added, so new light joins what is already flowing rather than erasing it.
                        addWide(dst, i + 0, static_cast<uint16_t>((c.r << 8) | c.r));
                        addWide(dst, i + 1, static_cast<uint16_t>((c.g << 8) | c.g));
                        addWide(dst, i + 2, static_cast<uint16_t>((c.b << 8) | c.b));
                    }
                }
        // Eased into the next frame's window, so one bright frame cannot make the cloud flinch.
        peak_  = static_cast<uint8_t>((peak_ * 7u + (frameMax < 32 ? 32 : frameMax)) / 8u);
        floor_ = static_cast<uint8_t>((floor_ * 7u + frameMin) / 8u);

        if (!small) return;
        // Stretch the small field over the cloud, adding as it goes.
        if (!scratch_ || !taps_) return;          // sized in prepare(); the render path never allocates
        draw::upscale16(scratch_.data(), w, h, d, field_.data(), fw_, fh_, fd_,
                        taps_.data(), taps_.count());
        const size_t n = static_cast<size_t>(w) * h * d * 3;
        for (size_t k = 0; k < n; k++) addWide(cloud, k, scratch_[k]);
    }

    /// Saturating add at 16 bits: light adds, and a bright cloud must not wrap to black.
    static void addWide(uint16_t* p, size_t i, uint16_t v) {
        const uint32_t s = static_cast<uint32_t>(p[i]) + v;
        p[i] = static_cast<uint16_t>(s > 65535u ? 65535u : s);
    }


    ScratchBuffer<uint16_t> planeA_{*this};    ///< the cloud; the two alternate roles
    ScratchBuffer<uint16_t> planeB_{*this};    ///< advect's destination
    ScratchBuffer<uint16_t> field_{*this};     ///< the field at its own resolution (empty at 1:1)
    ScratchBuffer<uint16_t> scratch_{*this};   ///< the stretched field, when one is used
    ScratchBuffer<draw::UpscaleTap> taps_{*this};  ///< upscale16's per-column blend table
    ScratchBuffer<uint8_t>  carry_{*this};     ///< the dither's per-channel error
    OscillatorBank<1>       bank_;             ///< the field's drift
    lengthType              planeW_ = 0, planeH_ = 0, planeD_ = 0;   ///< the shape the planes hold
    lengthType              fw_ = 0, fh_ = 0, fd_ = 0;               ///< the field's own extents
    bool                    front_ = true;     ///< which plane currently holds the cloud
    uint32_t                lastMs_ = 0;       ///< the previous frame's timestamp
    uint32_t                frame_ = 0;        ///< counted so `fieldRate` can skip births
    uint8_t                 peak_ = 200;    ///< the field's high water mark, eased per frame
    uint8_t                 floor_ = 40;    ///< and its low, so the window spans what is there
    bool                    started_ = false;   ///< false until a frame has been timed
};

}  // namespace mm
