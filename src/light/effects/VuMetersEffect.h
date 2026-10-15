#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Sixteen needles, one per band, each with real mass.
/// @card VuMetersEffect.gif
///
/// What makes a VU meter beautiful is the needle: it overshoots a peak, swings back and settles. That is why a mechanical meter reads as alive where a bar graph reads as a readout.
///
/// The bass needles are heavier than the treble ones, so the low end swings and the high end flickers.
///
/// @attribution IEC 60268-17
///
/// @moreinfo
///
/// Each band drives a damped harmonic oscillator, a spring and a damper; IEC 60268-17 specifies 300 ms to 99% with 1 to 1.5% overshoot.
class VuMetersEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶🖌️"; }
    /// A plane of dials, where a cube gets one bank per slice.
    Dim dimensions() const override { return Dim::D3; }

    /// How much the needle overshoots: low swings, high settles.
    uint8_t damping   = 150;
    /// How quickly the needle chases the signal at all.
    uint8_t response  = 120;
    /// How long the peak marker stays up.
    uint8_t peakHold  = 200;
    /// Drive from the smoothed ballistic rather than the raw band.
    bool    smooth    = false;

    /// Publish the needle's mechanics and the peak-hold.
    void defineControls() override {
        controls_.addControl("damping", damping, 0, 255);
        controls_.addControl("response", response, 1, 255);
        controls_.addControl("peakHold", peakHold, 0, 255);
        controls_.addControl("smooth", smooth);
    }

    /// Rest every needle and clear its peak.
    void prepare() override {
        for (uint8_t b = 0; b < 16; b++) { pos_[b] = 0; vel_[b] = 0; peak_[b] = 0; }
        started_ = false;
    }

    /// Integrate every needle, then tile the bank and draw each dial.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height(), dep = depth();
        if (w < 2 || h < 2) return;
        const uint32_t now = elapsed();
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        const AudioFrame* f = AudioService::latestFrame();

        // Time-stepped so the mechanics hold at any frame rate, and clamped against a stall.
        const uint32_t step = dt > 100u ? 100u : dt;
        const int32_t k = 4 + static_cast<int32_t>(response) / 4;          // spring: how hard it pulls
        const int32_t c = 1 + static_cast<int32_t>(damping) / 12;          // damper: how much it fights
        for (uint8_t b = 0; b < 16; b++) {
            const int32_t target = static_cast<int32_t>(f ? (smooth ? f->bandsSmoothed[b] : f->bands[b]) : 0) << 8;
            // Heavier at the bass end, as a real meter bridge is.
            const int32_t mass = 16 + (15 - b);
            const int32_t accel = ((target - pos_[b]) * k) / mass - (vel_[b] * c) / 16;
            vel_[b] += (accel * static_cast<int32_t>(step)) / 64;
            pos_[b] += (vel_[b] * static_cast<int32_t>(step)) / 64;
            if (pos_[b] < 0) { pos_[b] = 0; if (vel_[b] < 0) vel_[b] = -vel_[b] / 3; }   // bounce off the pin
            const int32_t full = 255 << 8;
            if (pos_[b] > full) { pos_[b] = full; if (vel_[b] > 0) vel_[b] = 0; }
            const uint8_t reading = static_cast<uint8_t>(pos_[b] >> 8);
            // A new maximum at once, then a half-life fall counting the real elapsed time, not `step`.
            const uint16_t keep = halfLifeKeep(dt, 200u + static_cast<uint32_t>(peakHold) * 12u);
            peak_[b] = reading > peak_[b]
                     ? reading
                     : static_cast<uint8_t>((static_cast<uint32_t>(peak_[b]) * keep) >> 16);
        }

        // A grid of dials rather than a row of slits, tiled as square as the panel allows.
        lengthType cols = 16, rows = 1;
        bestTiling(w, h, cols, rows);
        const lengthType cellW = w / cols, cellH = h / rows;
        draw::fill(cv, RGB{0, 0, 0});
        if (cellW < 3 || cellH < 3) return;            // no room for a dial at all
        for (lengthType z = 0; z < dep; z++)
            for (lengthType i = 0; i < 16; i++) {
                const lengthType cxi = i % cols, cyi = i / cols;
                if (cyi >= rows) break;                // fewer cells than bands, so draw what fits
                drawMeter(cv, cxi * cellW, cyi * cellH, cellW, cellH, z,
                          static_cast<uint8_t>(i), pos_[i] >> 8, peak_[i]);
            }
    }

private:
    /// The factor pair of 16 whose cell aspect is nearest square, so a wide panel tiles 8x2.
    static void bestTiling(lengthType w, lengthType h, lengthType& cols, lengthType& rows) {
        int32_t bestNum = -1, bestDen = 1;
        for (lengthType c = 1; c <= 16; c++) {
            if (16 % c) continue;
            const lengthType r = 16 / c;
            const int32_t cw = w / c, ch = h / r;
            if (cw < 1 || ch < 1) continue;
            const int32_t lo = cw < ch ? cw : ch, hi = cw < ch ? ch : cw;
            // Compared as a fraction, without floating point.
            if (bestNum < 0 || static_cast<int64_t>(lo) * bestDen > static_cast<int64_t>(bestNum) * hi) {
                bestNum = lo; bestDen = hi; cols = c; rows = r;
            }
        }
        if (bestNum < 0) { cols = 16; rows = 1; }
    }

    /// One meter in its cell: a needle from the bottom center, its peak marker and the red zone.
    void drawMeter(const draw::Canvas& cv, lengthType x0, lengthType y0, lengthType colW,
                   lengthType cellH, lengthType z, uint8_t band, int32_t reading, uint8_t peak) const {
        const lengthType px = x0 + colW / 2;                 // pivot, centered in its own cell
        const lengthType py = static_cast<lengthType>(y0 + cellH - 1);
        // As long as the cell is tall, with the sweep fitting the cell's width.
        const lengthType len = static_cast<lengthType>(cellH - 1 < 2 ? 2 : cellH - 1);
        // The half-sweep keeping the tip inside its cell, from the geometry and capped at 60 degrees.
        const int32_t halfW = colW / 2;
        int32_t half = len > 0 ? (10922 * halfW) / len : 10922;   // small-angle: proportional
        if (half > 10922) half = 10922;                            // never more than 60 degrees
        if (half < 1000) half = 1000;                              // and always a visible swing
        const auto angleFor = [half](int32_t v) -> angle16 {
            const int32_t clamped = v < 0 ? 0 : (v > 255 ? 255 : v);
            return static_cast<angle16>(static_cast<int32_t>(-half + (clamped * 2 * half) / 255));
        };
        const angle16 a = angleFor(reading);
        const lengthType nx = static_cast<lengthType>(px + (static_cast<int32_t>(sin16(a)) * len) / 32768);
        const lengthType ny = static_cast<lengthType>(py - (static_cast<int32_t>(cos16(a)) * len) / 32768);
        // The scale, a faint arc, turning red past three quarters.
        for (int32_t s = 0; s <= 255; s += 8) {
            const angle16 sa = angleFor(s);
            const lengthType sx = static_cast<lengthType>(px + (static_cast<int32_t>(sin16(sa)) * len) / 32768);
            const lengthType sy = static_cast<lengthType>(py - (static_cast<int32_t>(cos16(sa)) * len) / 32768);
            constexpr uint8_t base = 24;
            const RGB tick = s > 191 ? RGB{static_cast<uint8_t>(base + 40), 0, 0}
                                     : RGB{base, base, static_cast<uint8_t>(base + 8)};
            draw::pixel(cv, {sx, sy, z}, tick);
        }
        // The needle, in the band's palette color, and its peak marker above it.
        const RGB c = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(band * 16u), 255);
        draw::line(cv, {px, py, z}, {nx, ny, z}, c);
        if (peak > 4) {
            const angle16 pa = angleFor(peak);
            const lengthType mx = static_cast<lengthType>(px + (static_cast<int32_t>(sin16(pa)) * len) / 32768);
            const lengthType my = static_cast<lengthType>(py - (static_cast<int32_t>(cos16(pa)) * len) / 32768);
            draw::pixel(cv, {mx, my, z}, RGB{255, 255, 255});
        }
    }

    int32_t pos_[16] = {};      ///< needle position, 8.8 fixed point
    int32_t vel_[16] = {};      ///< needle velocity, which is the mass that makes it overshoot
    uint8_t peak_[16] = {};     ///< the peak-hold marker
    bool    started_ = false;   ///< false until a frame has been timed
    uint32_t lastMs_ = 0;       ///< the previous frame's timestamp
};

}  // namespace mm
