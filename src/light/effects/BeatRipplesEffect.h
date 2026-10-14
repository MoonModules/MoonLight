#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Effect: a wave surface where every detected beat drops a stone, rippling and interfering.
/// @card BeatRipplesEffect.gif
///
/// The surface is a real wave simulation, the classic two-buffer scheme: ripples pass through each other, reflect off the walls and interfere, and a detected onset drops a stone.
///
/// Prior art: Gomez 2000, the water effect the demoscene settled on.
///
/// @moreinfo
///
/// ## The wave equation, and rendering by slope
///
/// Each cell's next height is its four neighbors averaged and doubled, minus its previous height, damped; the field renders by slope, so crests read as bright lines.
///
/// The loudest band picks where a stone lands, bass near the center and treble at the rim; a cube takes the plane extruded, as the equation is 2D.
class BeatRipplesEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶🖌️"; }
    /// The wave equation is 2D, and a cube takes the plane extruded.
    Dim dimensions() const override { return Dim::D2; }

    /// How long the water keeps ringing.
    uint8_t damping   = 200;
    /// How deep a beat's stone falls.
    uint8_t drop      = 180;
    /// Idle drops, for when there is no music at all.
    uint8_t rain      = 30;
    /// How strongly the slope lights the surface.
    uint8_t shine     = 150;

    /// Publish the damping, the stone's depth, the idle rain and the slope's brightness.
    void defineControls() override {
        controls_.addControl("damping", damping, 0, 255);
        controls_.addControl("drop", drop, 0, 255);
        controls_.addControl("rain", rain, 0, 255);
        controls_.addControl("shine", shine, 0, 255);
    }

    /// Size both height buffers, clear them, and arm the rain clock.
    void prepare() override {
        const lengthType w = width(), h = height();
        const size_t n = static_cast<size_t>(w) * h;
        cur_.resize(n); prev_.resize(n);
        if (cur_) std::memset(cur_.data(), 0, cur_.bytes());
        if (prev_) std::memset(prev_.data(), 0, prev_.bytes());
        onsetSeen_ = false;
        started_ = false;
        seq_ = 0;
        // Starts full, so the first drop lands on the opening frame rather than two seconds in.
        carry_ = 2000;
    }

    /// Drop a stone on each onset, rain when idle, then step the surface and render it.
    void tick() MM_NONBLOCKING override {
        if (!cur_ || !prev_) return;
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        if (w < 3 || h < 3) return;
        const uint32_t now = elapsed();
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        lastMs_ = now;
        started_ = true;

        const AudioFrame* f = AudioService::latestFrame();
        const bool onsetNow = f && f->onset != 0;
        if (onsetNow && !onsetSeen_) {
            // The loudest band picks the radius, and the angle walks so successive hits spread out.
            uint8_t loudest = 0, best = 0;
            for (uint8_t b = 0; b < 16; b++) if (f->bands[b] > best) { best = f->bands[b]; loudest = b; }
            const angle16 a = static_cast<angle16>(hashInt(seq_, 7) << 8);
            const int32_t maxR = (w < h ? w : h) / 2 - 2;
            const int32_t r = (maxR * (loudest + 1)) / 17;
            const lengthType sx = static_cast<lengthType>(w / 2 + (static_cast<int32_t>(cos16(a)) * r) / 32768);
            const lengthType sy = static_cast<lengthType>(h / 2 + (static_cast<int32_t>(sin16(a)) * r) / 32768);
            // The onset scales the stone, with a floor that keeps a weak hit visible.
            const int32_t hit = 96 + (static_cast<int32_t>(f->onset) * 159) / 255;
            splash(sx, sy, static_cast<int16_t>(-(static_cast<int32_t>(drop) * kSplashScale / 255) * hit / 255));
            seq_++;
        }
        onsetSeen_ = onsetNow;

        // Idle rain, so the surface is alive with no music. Time-paced, not per frame.
        if (rain > 0) {
            carry_ += dt;
            const uint32_t every = 2000u - static_cast<uint32_t>(rain) * 7u;
            if (carry_ >= every) {
                carry_ = 0;
                const lengthType sx = static_cast<lengthType>(hashInt(seq_, 11) % static_cast<uint32_t>(w));
                const lengthType sy = static_cast<lengthType>(hashInt(seq_, 13) % static_cast<uint32_t>(h));
                splash(sx, sy, static_cast<int16_t>(-(static_cast<int32_t>(rain) * kSplashScale) / 255));
                seq_++;
            }
        }

        // A fixed timestep: a simulation's speed is its step count, so per-frame rings at the frame rate.
        stepCarry_ += dt;
        constexpr uint32_t kStepMs = 16;          // ~60 physics steps a second
        uint8_t steps = 0;
        while (stepCarry_ >= kStepMs && steps < 4) { stepCarry_ -= kStepMs; steps++; }
        if (stepCarry_ > kStepMs * 4) stepCarry_ = 0;
        for (uint8_t it = 0; it < steps; it++) waveStep();
        renderSurface(cv, w, h);
    }

    /// One step of the wave equation, which is what makes ripples pass through each other. @xref{the-wave-equation-and-rendering-by-slope}
    void waveStep() {
        const lengthType w = width(), h = height();
        int16_t* c = cur_.data();
        int16_t* p = prev_.data();
        const int32_t keep = 224 + static_cast<int32_t>(damping) / 8;      // 224..255 of 256
        for (lengthType y = 1; y < h - 1; y++)
            for (lengthType x = 1; x < w - 1; x++) {
                const size_t o = static_cast<size_t>(y) * w + x;
                const int32_t sum = static_cast<int32_t>(c[o - 1]) + c[o + 1] + c[o - w] + c[o + w];
                int32_t v = (sum / 2) - static_cast<int32_t>(p[o]);
                v = (v * keep) / 256;
                p[o] = static_cast<int16_t>(v < -20000 ? -20000 : (v > 20000 ? 20000 : v));
            }
        // The two buffers exchange roles: `prev_` now holds the new surface.
        for (size_t i = 0, n = cur_.count(); i < n; i++) { const int16_t tmp = cur_[i]; cur_[i] = prev_[i]; prev_[i] = tmp; }
    }

    /// Render by slope: the gradient between neighbors lights a pixel, not the height itself.
    void renderSurface(const draw::Canvas& cv, lengthType w, lengthType h) {
        const int16_t* s = cur_.data();
        for (lengthType y = 0; y < h; y++)
            for (lengthType x = 0; x < w; x++) {
                const size_t o = static_cast<size_t>(y) * w + x;
                const int32_t gx = (x > 0 && x < w - 1) ? static_cast<int32_t>(s[o + 1]) - s[o - 1] : 0;
                const int32_t gy = (y > 0 && y < h - 1) ? static_cast<int32_t>(s[o + w]) - s[o - w] : 0;
                int32_t mag = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
                mag = (mag * shine) / 512;
                const uint8_t bri = static_cast<uint8_t>(mag > 255 ? 255 : mag);
                draw::pixel(cv, {x, y, 0}, colorFromPalette(*Palettes::active(), paletteIndex(s[o], seq_), bri));
            }
    }

    /// The palette index at a height: it wraps with height, so ripples a few hundred high walk the whole palette, and each stone moves the start along it.
    static uint8_t paletteIndex(int16_t height, uint32_t stones) {
        return static_cast<uint8_t>(128u + stones * 8u + static_cast<uint32_t>(height / 8));   // unsigned, so the wrap is modulo 256 by definition
    }

private:
    /// Scales the 0..255 knobs onto the int16 field, pressing a third of the way to the clamp at 255.
    static constexpr int32_t kSplashScale = 6000;

    /// A stone: a dish pressed into the surface, since a single spike smears into noise.
    void splash(lengthType cx, lengthType cy, int16_t depth) {
        const lengthType w = width(), h = height();
        const lengthType rad = static_cast<lengthType>((w < h ? w : h) / 24 + 1);
        const int32_t r2 = static_cast<int32_t>(rad) * rad;
        for (lengthType dy = -rad; dy <= rad; dy++)
            for (lengthType dx = -rad; dx <= rad; dx++) {
                const int32_t q = static_cast<int32_t>(dx) * dx + static_cast<int32_t>(dy) * dy;
                if (q > r2) continue;
                const lengthType x = cx + dx, y = cy + dy;
                if (x < 1 || y < 1 || x >= w - 1 || y >= h - 1) continue;
                const int32_t fall = ((r2 - q) * 100) / (r2 > 0 ? r2 : 1);
                // Accumulated: writing the dish flat would erase a wave already passing through.
                const size_t o = static_cast<size_t>(y) * w + x;
                const int32_t v = static_cast<int32_t>(cur_[o]) + (depth * fall) / 100;
                cur_[o] = static_cast<int16_t>(v < -20000 ? -20000 : (v > 20000 ? 20000 : v));
            }
    }

    ScratchBuffer<int16_t> cur_{*this}, prev_{*this};   ///< the two height fields, exchanging roles
    bool     onsetSeen_ = false, started_ = false;
    uint32_t lastMs_ = 0, carry_ = 2000, seq_ = 0, stepCarry_ = 0;
};

}  // namespace mm
