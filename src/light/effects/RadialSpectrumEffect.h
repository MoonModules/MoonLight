#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// The spectrum as ripples: each band owns a sector around the center, mirrored left and right with the bass at top and bottom.
/// @card RadialSpectrumEffect.gif
///
/// Sound is born at the center and travels outward, so the radius is time and a ring's length is that band's recent history. On a cube the ripples are expanding shells. `polarTable`, `polarTable16` and `mapping` set the polar address and its shape.
///
/// @technical
///
/// Nothing is transported: a short history of band frames holds one entry per ring. A light's angle picks its band and its radius its age, two table reads a light.
class RadialSpectrumEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶🖌️🎡"; }
    /// Volumetric: a cube's rings become expanding shells.
    Dim dimensions() const override { return Dim::D3; }

    static constexpr uint16_t kMaxHistory = 128;   ///< rings of history, and the largest radius read

    /// How fast sound travels outward.
    uint8_t speed       = 85;
    /// How far out a ripple stays visible.
    uint8_t persistence = 128;
    /// Read the meter ballistic rather than the raw bands.
    bool    smooth      = false;
    /// A white shockwave born at the center on every onset.
    bool    beat        = true;
    /// The polar address: the table, its precision, and the mapping a cube takes.
    PolarLut::Controls polar;

    /// Publish the ripple's speed and reach, the source, the shockwave and the address.
    void defineControls() override {
        controls_.addControl("speed", speed, 5, 100);
        controls_.addControl("persistence", persistence, 0, 255);
        controls_.addControl("smooth", smooth);
        controls_.addControl("beat", beat);
        PolarLut::addControls(controls_, polar);
    }

    /// Build the polar address table and clear the ring history.
    void prepare() override {
        lut_.prepareFor(polar, width(), height(), EffectBase::depth());
        std::memset(history_, 0, sizeof(history_));
        std::memset(beats_, 0, sizeof(beats_));
        head_ = 0;
        carry_ = 0;
        started_ = false;
        onsetSeen_ = false;
    }

    /// Birth any due rings from the current spectrum, then read one out per light.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height(), dep = depth();
        const uint32_t now = elapsed();
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        const AudioFrame* f = AudioService::latestFrame();
        const uint8_t* src = f ? (smooth ? f->bandsSmoothed : f->bands) : nullptr;
        // Edge-detected and latched, so a hit landing between two rings is not lost.
        const bool onsetNow = f && f->onset != 0;
        if (onsetNow && !onsetSeen_) pendingBeat_ = 255;
        onsetSeen_ = onsetNow;

        // A ring on a fixed period, the remainder carried and a stall capped.
        const uint32_t ringMs = 110u - static_cast<uint32_t>(speed);
        carry_ += dt;
        uint8_t born = 0;
        while (carry_ >= ringMs && born < 8) {
            carry_ -= ringMs;
            head_ = static_cast<uint16_t>((head_ + 1) % kMaxHistory);
            if (src) std::memcpy(history_[head_], src, 16);
            else     std::memset(history_[head_], 0, 16);
            beats_[head_] = beat ? pendingBeat_ : 0;
            pendingBeat_ = 0;
            born++;
        }
        if (born > 0) carry_ %= ringMs;

        // The keep fraction per ring, rebuilt only when `persistence` moves.
        if (persistence != fadeFor_) {
            const uint32_t keep = 236u + (static_cast<uint32_t>(persistence) * 19u) / 255u;
            uint32_t k = 256;
            for (uint16_t r = 0; r < kMaxHistory; r++) { fade_[r] = static_cast<uint8_t>(k > 255 ? 255 : k); k = (k * keep) >> 8; }
            fadeFor_ = persistence;
        }

        const bool table = lut_.ready();
        const int32_t cx = w / 2, cy = h / 2, cz = dep / 2;
        const auto m = PolarLut::mappingOf(polar);
        std::size_t idx = 0;
        for (lengthType z = 0; z < dep; z++)
            for (lengthType y = 0; y < h; y++)
                for (lengthType x = 0; x < w; x++, idx++) {
                    angle16 a; uint32_t r;
                    if (table) { a = lut_.angle(idx); r = lut_.radiusPixels(idx); }
                    else {
                        const auto ad = PolarLut::addressOf(m, static_cast<int32_t>(x) - cx,
                                                            static_cast<int32_t>(y) - cy,
                                                            static_cast<int32_t>(z) - cz);
                        a = ad.angle; r = ad.radius;
                    }
                    // Folded so left and right mirror, since a spectrum has no side of its own.
                    const uint32_t folded = a < 32768u ? a : 65535u - a;            // 0..32767
                    const uint8_t band = static_cast<uint8_t>((folded * 16u) >> 15);  // 0..15
                    // The age from the radius, since a ring that far out was born that long ago.
                    if (r >= kMaxHistory) { draw::pixel(cv, {x, y, z}, RGB{0, 0, 0}); continue; }
                    const uint16_t slot = static_cast<uint16_t>((head_ + kMaxHistory - r) % kMaxHistory);
                    const uint32_t v = (static_cast<uint32_t>(history_[slot][band]) * fade_[r]) >> 8;
                    RGB c = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(band * 16u),
                                             static_cast<uint8_t>(v));
                    // The shockwave, fading with the same age as the ring carrying it.
                    const uint32_t bw = (static_cast<uint32_t>(beats_[slot]) * fade_[r]) >> 8;
                    if (bw) {
                        c.r = static_cast<uint8_t>(c.r + bw > 255u ? 255u : c.r + bw);
                        c.g = static_cast<uint8_t>(c.g + bw > 255u ? 255u : c.g + bw);
                        c.b = static_cast<uint8_t>(c.b + bw > 255u ? 255u : c.b + bw);
                    }
                    draw::pixel(cv, {x, y, z}, c);
                }
    }

private:
    PolarLut lut_{*this};
    uint8_t  history_[kMaxHistory][16] = {};   ///< the rings: one band frame per step
    uint8_t  beats_[kMaxHistory] = {};         ///< the shockwave strength born with each ring
    uint8_t  fade_[kMaxHistory] = {};           ///< the keep fraction per ring of age
    uint8_t  fadeFor_ = 255;                    ///< the `persistence` fade_[] was built for
    uint16_t head_ = 0;                         ///< the newest ring
    uint32_t carry_ = 0;                        ///< time owed toward the next ring, in ms
    uint8_t  pendingBeat_ = 0;                  ///< a hit waiting for the next ring
    bool     started_ = false;      ///< false until a frame has been timed
    bool     onsetSeen_ = false;    ///< the previous frame's onset, for the edge detect
    uint32_t lastMs_ = 0;           ///< the previous frame's timestamp
};

}  // namespace mm
