#pragma once

#include "core/util/math16.h"              // sin16, BeatPhase, easeInOutQuad
#include "light/effects/EffectBase.h"
#include "light/powerfunctions/particles.h"   // FrameTime: the shared time scale

namespace mm {

/// The previous frame fed back through a zoom and rotation, dimmed, with a bright source drawn on top.
/// @card EchoEffect.gif
///
/// The trails spiral away from themselves, like a camera pointed at its own monitor. Feedback is not a primitive: once the grid can be read as a texture (`sampleWrap`), the whole family of trails, zoom blur and smear is a few lines.
///
/// @moreinfo
///
/// This is the clearest use of the gather half of draw.h, where `sampleWrap` reads with wrapping; the cost is one bilinear sample a pixel plus a scratch copy.
class EchoEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫✨"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast the source orbits.
    uint8_t bpm    = 30;
    /// How much the feedback grows each frame.
    uint8_t zoom   = 12;
    /// Rotation per frame, which turns the trail into a spiral.
    uint8_t rotate = 8;
    /// How fast the echo fades; higher is a shorter trail.
    uint8_t decay  = 24;
    /// Radius of the bright source.
    uint8_t size   = 2;

    /// Publish the source's orbit, the feedback transform and the trail's decay.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 120);
        controls_.addControl("zoom", zoom, 0, 64);
        controls_.addControl("rotate", rotate, 0, 64);
        controls_.addControl("decay", decay, 1, 128);
        controls_.addControl("size", size, 0, 16);
    }

    /// Size one frame of history, which the feedback reads as a stable copy.
    void prepare() override {
        // Reading the live buffer would feed partly-updated pixels back as a directional smear.
        const size_t n = static_cast<size_t>(width() > 0 ? width() : 0) *
                         static_cast<size_t>(height() > 0 ? height() : 0) * 3u;
        history_.resize(n);
        time_.reset();
    }

    /// Run any due feedback pass, draw the source, then keep this frame as the next one's history.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        if (w < 2 || h < 2 || !history_) return;

        phase_.advanceTo(elapsed(), bpm);
        const angle16 t = static_cast<angle16>(phase_.phase(65536));

        // Accumulated rather than discarded, since a fast frame's share is a fraction of a unit.
        feedbackAcc_ += time_.advance(elapsed());
        // Every whole unit at once, keeping the fraction: a slow frame stands in for several steps.
        const uint32_t passes = feedbackAcc_ / particles::FrameTime::kOne;
        feedbackAcc_ -= passes * particles::FrameTime::kOne;
        const bool feedbackDue = passes > 0;
        // Feedback compounds, so the surviving fraction is what stays constant per second.

        // The history in its own Canvas, since the previous frame is another texture to the gather.
        draw::Canvas hist{history_.data(), history_.bytes(), {w, h, 1}, 3};

        const draw::pos_t cx = draw::toSub(w) / 2;
        const draw::pos_t cy = draw::toSub(h) / 2;

        // Each pixel finds where it came from, the transform inverted so the image grows outward.
        const int32_t s = 65536 - (static_cast<int32_t>(zoom) * 64);       // 16.16 inverse scale
        const angle16 a = static_cast<angle16>(-(static_cast<int32_t>(rotate) * 64));
        const int32_t cosA = static_cast<int32_t>(cos16(a));       // -32768..32767
        const int32_t sinA = static_cast<int32_t>(sin16(a));

        if (feedbackDue) {
            // The surviving fraction raised to the passes this one frame stands in for.
            uint8_t keep = static_cast<uint8_t>(255 - decay);
            for (uint32_t n = 1; n < passes; n++) keep = scale8(keep, static_cast<uint8_t>(255 - decay));
            for (lengthType y = 0; y < h; y++) {
                for (lengthType x = 0; x < w; x++) {
                    const int32_t px = draw::toSub(x) - cx;
                    const int32_t py = draw::toSub(y) - cy;
                    // Rotated then scaled, in fixed point.
                    const int32_t rx = (px * cosA - py * sinA) >> 15;
                    const int32_t ry = (px * sinA + py * cosA) >> 15;
                    const draw::pos_t sx = static_cast<draw::pos_t>(((static_cast<int64_t>(rx) * s) >> 16) + cx);
                    const draw::pos_t sy = static_cast<draw::pos_t>(((static_cast<int64_t>(ry) * s) >> 16) + cy);

                    RGB c = draw::sampleWrap(hist, sx, sy);
                    // Faded, so a trail dies out rather than accumulating to white.
                    c.r = scale8(c.r, keep);
                    c.g = scale8(c.g, keep);
                    c.b = scale8(c.b, keep);
                    draw::pixel(cv, {x, y, 0}, c);
                }
            }
        }

        // A bright disc orbiting the center, whose path the trail is a record of.
        const int32_t orbit = (w < h ? w : h) * 3 / 8;
        const int32_t ox = w / 2 + ((static_cast<int32_t>(sin16(t))) * orbit) / 32768;
        const int32_t oy = h / 2 + ((static_cast<int32_t>(cos16(t))) * orbit) / 32768;
        const RGB src = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(t >> 8), 255);
        if (size == 0) {
            draw::pixel(cv, {static_cast<lengthType>(ox), static_cast<lengthType>(oy), 0}, src);
        } else {
            draw::fillCircle(cv, static_cast<lengthType>(ox), static_cast<lengthType>(oy),
                             static_cast<lengthType>(size), src);
        }

        // Kept only when the feedback advanced, or an unpaced frame overwrites the trail with itself.
        if (!feedbackDue) return;
        const size_t bytes = history_.bytes();   // the allocation, since a resize outruns prepare()
        for (lengthType y = 0; y < h; y++)
            for (lengthType x = 0; x < w; x++) {
                const RGB c = draw::get(cv, {x, y, 0});
                const size_t i = (static_cast<size_t>(y) * w + x) * 3u;
                if (i + 2 < bytes) { history_[i] = c.r; history_[i + 1] = c.g; history_[i + 2] = c.b; }
            }
    }

private:
    ScratchBuffer<uint8_t> history_{*this};   ///< the previous frame, read as a texture
    particles::FrameTime time_{60};           ///< the reference rate the controls are written against
    uint32_t feedbackAcc_ = 0;                ///< reference frames not yet spent on a pass
    BeatPhase phase_;                         ///< the source's orbit clock
};

}  // namespace mm
