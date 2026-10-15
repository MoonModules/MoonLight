#pragma once

#include "core/util/math16.h"              // hashInt: drop placement without a stream RNG
#include "light/effects/EffectBase.h"

namespace mm {

/// A propagating wave simulation: drops land, their rings spread outward, reflect off the edges and interfere where they cross.
/// @card WaterRippleEffect.gif
///
/// The crossing is what a drawn ripple cannot fake, because two rings meeting have to add and cancel. Distinct from @ref RipplesEffect, which draws clean concentric circles; this behaves like water.
///
/// @attribution Hugo Elias
///
/// @moreinfo
///
/// The next height is the four neighbors averaged, minus the previous height, damped toward zero, so the ripple is emergent rather than drawn. Two int16 buffers scale with the fixture, so a large wall is worth checking against the memory budget; `hashInt` places drops, so two devices agree.
class WaterRippleEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫🧬"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// Simulation steps a second, which is how fast the water itself moves.
    uint8_t speed    = 60;
    /// How often drops land, where 0 leaves the surface still.
    uint8_t dropRate = 24;
    /// How fast waves lose energy, so higher is calmer water.
    uint8_t damping  = 16;
    /// How hard a drop hits.
    uint8_t strength = 200;
    /// Color the surface by height rather than by brightness alone.
    bool    colorByHeight = true;
    /// Where in the palette the still surface sits.
    uint8_t hueBase   = 140;
    /// How far a crest and a trough reach from that.
    uint8_t hueSpread = 110;

    /// Publish the wave's speed and damping, the drops, and how height takes color.
    void defineControls() override {
        controls_.addControl("speed", speed, 1, 120);
        controls_.addControl("dropRate", dropRate, 0, 255);
        controls_.addControl("damping", damping, 1, 64);
        controls_.addControl("strength", strength, 1, 255);
        controls_.addControl("colorByHeight", colorByHeight);
        controls_.addControl("hueBase", hueBase, 0, 255);
        controls_.addControl("hueSpread", hueSpread, 0, 127);
    }

    /// Size both height fields to the grid and reset the drop and step clocks.
    void prepare() override {
        // resize() reallocs only when the grid changes, so a live layout change needs nothing here.
        const size_t n = static_cast<size_t>(width() > 0 ? width() : 0) *
                         static_cast<size_t>(height() > 0 ? height() : 0);
        a_.resize(n);
        b_.resize(n);
        frame_ = 0;
        lastDropMs_ = 0;
        lastStepMs_ = 0;
        started_ = false;
        agc_ = kAgcFloor;
    }

    /// Land any due drop, advance the surface on its own clock, then render it.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        // Clamped neighbors make any grid valid, and a failed allocation degrades rather than crashes.
        if (w < 1 || h < 1 || !a_ || !b_) return;

        // Timed in milliseconds, or the downpour scales with the frame rate and reads as rain.
        const uint32_t now = elapsed();
        const uint32_t interval = dropRate > 0
            ? static_cast<uint32_t>(2000u - (static_cast<uint32_t>(dropRate) * 7u))   // 2000ms..215ms
            : 0u;
        // The first drop lands at once, since a flat surface for two seconds reads as broken.
        const bool dropNow = dropRate > 0 && (!started_ || (now - lastDropMs_) >= interval);
        started_ = true;
        if (dropNow) {
            lastDropMs_ = now;
            frame_++;                       // advances only on a drop, so each drop lands elsewhere
        }

        // hashInt makes placement a function of the drop counter, so two devices agree.
        if (dropNow) {
            const lengthType dx = static_cast<lengthType>(hashInt(frame_, 1, 0, kDropSeed) % w);
            const lengthType dy = static_cast<lengthType>(hashInt(frame_, 2, 0, kDropSeed) % h);
            field()[static_cast<size_t>(dy) * w + dx] = static_cast<int16_t>(strength * 64);
        }

        // The wave advances on wall-clock time, so `speed` means the same on any device.
        const uint32_t stepMs = 1000u / (speed > 0 ? speed : 1);
        if (now - lastStepMs_ >= stepMs) {
            lastStepMs_ = now;
            // Every pixel steps, clamping the neighbor, which is what reflects the wave at an edge.
            for (lengthType y = 0; y < h; y++) {
                for (lengthType x = 0; x < w; x++) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    const size_t left  = static_cast<size_t>(y) * w + (x > 0 ? x - 1 : 0);
                    const size_t right = static_cast<size_t>(y) * w + (x < w - 1 ? x + 1 : w - 1);
                    const size_t up    = static_cast<size_t>(y > 0 ? y - 1 : 0) * w + x;
                    const size_t down  = static_cast<size_t>(y < h - 1 ? y + 1 : h - 1) * w + x;
                    const int32_t neighbors = static_cast<int32_t>(field()[left]) + field()[right]
                                             + field()[up] + field()[down];
                    int32_t next = (neighbors >> 1) - lastField()[i];
                    next -= next * damping >> 10;        // damping: the surface settles
                    // Clamp before narrowing: a wrap flips the wave's sign and turns the surface inside out.
                    if (next > 32767) next = 32767;
                    if (next < -32768) next = -32768;
                    lastField()[i] = static_cast<int16_t>(next);
                }
            }
            flip_ = !flip_;   // the fields exchange roles; no data moves
        }

        // Scaled against the frame's own peak: a drop's energy spreads, so a fixed shift dims with the grid.
        uint32_t peak = 0;
        for (lengthType y = 0; y < h; y++)
            for (lengthType x = 0; x < w; x++) {
                const int32_t v = field()[static_cast<size_t>(y) * w + x];
                const uint32_t m = static_cast<uint32_t>(v < 0 ? -v : v);
                if (m > peak) peak = m;
            }
        // Up instantly and down slowly, with a floor so flat water stays dark.
        if (peak > agc_) agc_ = peak; else agc_ -= (agc_ - peak) >> 5;
        if (agc_ < kAgcFloor) agc_ = kAgcFloor;
        // A fraction of the peak, since the mean ripple measures a twelfth of the tallest pixel.
        const uint32_t ref = agc_ > 8u ? (agc_ / 4u) : 2u;

        // Height becomes brightness, and optionally the index, so a crest and a trough differ.
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                const int32_t v = field()[static_cast<size_t>(y) * w + x];
                const int32_t mag = v < 0 ? -v : v;
                // A square-root response, which lifts the mid-range where the ripples are.
                uint32_t lin = (static_cast<uint32_t>(mag) * 255u) / ref;
                if (lin > 255) lin = 255;
                uint8_t bri = static_cast<uint8_t>(isqrt(lin * 255u));
                if (bri == 0) { draw::pixel(cv, {x, y, 0}, RGB{0, 0, 0}); continue; }
                // The height spans the whole palette, since centering pins every pixel to one hue.
                uint8_t idx;
                if (colorByHeight) {
                    const int32_t rel = (v * static_cast<int32_t>(hueSpread)) / static_cast<int32_t>(ref);
                    const int32_t lo = -static_cast<int32_t>(hueSpread);
                    const int32_t clamped = rel > hueSpread ? hueSpread : (rel < lo ? lo : rel);
                    idx = static_cast<uint8_t>(hueBase + clamped);
                } else {
                    idx = static_cast<uint8_t>(hueBase + (bri >> 1));
                }
                draw::pixel(cv, {x, y, 0}, colorFromPalette(*Palettes::active(), idx, bri));
            }
        }
    }

private:
    static constexpr uint32_t kDropSeed = 0x5EAu;

    /// The two fields exchange roles rather than contents, since copying the grid is a second full pass.
    ScratchBuffer<int16_t>& field()     { return flip_ ? b_ : a_; }
    ScratchBuffer<int16_t>& lastField() { return flip_ ? a_ : b_; }

    static constexpr uint32_t kAgcFloor = 2048;   ///< below this the surface reads as flat, not lit

    uint32_t agc_ = kAgcFloor;          ///< the amplitude brightness is measured against
    ScratchBuffer<int16_t> a_{*this};   ///< a height field, its role alternating with `b_`
    ScratchBuffer<int16_t> b_{*this};   ///< the other height field
    bool flip_ = false;                 ///< which field currently holds this step
    uint32_t frame_ = 0;        ///< counts drops rather than frames, seeding each drop's position
    uint32_t lastDropMs_ = 0;   ///< when the last drop landed
    uint32_t lastStepMs_ = 0;   ///< when the surface last advanced
    bool     started_ = false;  ///< so the first drop does not wait out the interval
};

}  // namespace mm
