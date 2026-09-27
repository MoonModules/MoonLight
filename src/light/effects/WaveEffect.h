#pragma once

#include "core/util/math16.h"            // BeatPhase: the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

/// Effect of a traveling wave across the layer.
/// @card WaveEffect.gif
/// Author: Ewoud Wijma (MoonLight), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
///
/// Each column plots one point of a moving wave, so the lit points trace a scrolling curve.
/// A fading trail follows it, which is the classic oscilloscope look.
///
/// Prior art: MoonLight's Wave effect, whose six shapes and travel this reproduces.
///
/// @moreinfo
///
/// ## The axis convention
///
/// The waveform sets a y, so its shape lives on height and width is the travel axis.
/// A 1-tall grid therefore shows no wave.
/// To drive a strip or a row of lights, lay it out as 1xN, the project's 1D convention.
///
/// ## How a column finds its point
///
/// Each column's phase is the travel plus its own skew, which is what moves the wave sideways.
/// That phase maps through the selected waveform to a y inside the grid, and that pixel lights.
/// A vertical segment then joins the previous column, so a sawtooth or square reads as one line.
class WaveEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🌫️"; }
    /// Writes the z=0 plane, which extrude duplicates through a volume.
    Dim dimensions() const override { return Dim::D2; }

    // Index-aligned with waveY's switch. Sin3 sums three sines, and Noise plots a jittered band.
    static constexpr const char* kTypeOptions[] = {"Sawtooth", "Triangle", "Sine", "Square", "Sin3", "Noise"};
    /// How many waveform shapes the select offers.
    static constexpr uint8_t kTypeCount = 6;

    /// Travel speed, as phase advance per minute.
    uint8_t bpm  = 30;
    /// How much of the trail each frame keeps, so 0 clears the old wave at once.
    uint8_t fade = 32;
    /// Which waveform shape is drawn.
    uint8_t type = 2;

    /// Publish the travel speed, the trail and the waveform shape.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 255);
        controls_.addControl("fade", fade, 0, 255);
        controls_.addSelect("type", type, kTypeOptions, kTypeCount);
    }

    /// Size the persistent trail plane, clearing it when the geometry changed under it.
    void prepare() override {
        const lengthType w = width(), h = height();
        const uint8_t cpl = channelsPerLight();
        const size_t needed = static_cast<size_t>(w) * h * cpl;
        const size_t had = trail_.bytes();
        trail_.resize(needed);
        if (needed > 0 && needed == had && (w != trailW_ || h != trailH_ || cpl != trailCpl_)) {
            // Same byte count, new geometry: the old bytes are laid out for the old extents.
            std::memset(trail_.data(), 0, trail_.bytes());
        }
        trailW_ = w; trailH_ = h; trailCpl_ = cpl;
    }

    /// Fade the trail, advance the travel, then plot each column's point of the wave.
    void tick() MM_NONBLOCKING override {
        if (!trail_) return;
        const lengthType w = width();
        const lengthType h = height();
        const uint8_t cpl = channelsPerLight();
        uint8_t* buf = buffer();

        // 1. Fade the trail toward black, where a smaller `fade` is a shorter tail.
        for (size_t i = 0; i < trail_.bytes(); i++) trail_[i] = scale8(trail_[i], fade);

        // 2. BeatPhase seeds its base on the first tick and keeps its numerator wide until read.
        const uint32_t now = elapsed();
        phase_.advanceTo(now, bpm);
        const uint8_t t = static_cast<uint8_t>(phase_.phase(256));         // uint8 angle (256 = full turn)
        // The color cycles slowly, indexing the active palette.
        const uint8_t colorIndex = static_cast<uint8_t>(now / 50);

        // 3. Plot the wave point per column, joining discontinuous shapes to the previous column.
        const RGB c = waveColor(colorIndex);
        int prevY = -1;
        for (lengthType x = 0; x < w; x++) {
            const uint8_t ph = static_cast<uint8_t>(t + static_cast<uint8_t>(x) * kColumnSkew);
            const lengthType y = waveY(ph, h);
            plot(x, y, c, cpl, w);
            // Filling between the two makes a sawtooth or square read as a connected line.
            if (prevY >= 0) {
                const lengthType lo = prevY < y ? prevY : y;
                const lengthType hi = prevY < y ? y : prevY;
                for (lengthType yy = lo; yy <= hi; yy++) plot(x, yy, c, cpl, w);
            }
            prevY = static_cast<int>(y);
        }

        // 4. Blit the trail (faded history + this frame's wave) into the output buffer.
        std::memcpy(buf, trail_.data(), trail_.bytes());
    }

    /// Test seam: the pure waveform map from phase to y, needing no buffer or clock.
    static lengthType waveYForTest(uint8_t type, uint8_t phase, lengthType h) {
        return waveY(type, phase, h);
    }

private:
    /// How much each column delays the phase, which is the wave's horizontal travel.
    static constexpr uint8_t kColumnSkew = 8;

    ScratchBuffer<uint8_t> trail_{*this};  ///< the persistent z=0-plane trail
    /// The geometry the trail bytes are laid out for, tracked so a same-size reshape can clear it.
    lengthType trailW_ = 0, trailH_ = 0;
    uint8_t  trailCpl_ = 0;                ///< and the channel count they were written at
    BeatPhase phase_;                      ///< the travel clock

    /// This frame's color, from the active palette so the wave recolors with it.
    static RGB waveColor(uint8_t index) { return colorFromPalette(*Palettes::active(), index); }

    /// Map a phase to a y inside the grid for the selected waveform, in integers.
    lengthType waveY(uint8_t phase, lengthType h) const { return waveY(type, phase, h); }
    static lengthType waveY(uint8_t type, uint8_t phase, lengthType h) {
        if (h == 0) return 0;
        uint8_t v;   // the waveform value, 0..255, then scaled to [0, h)
        switch (type) {
            case 0: v = phase; break;                                        // Sawtooth: ramp 0→255
            case 1: v = triangle8(phase); break;                             // Triangle: up then down
            case 2: v = sin8(phase); break;                                  // Sine
            case 3: v = phase < 128 ? 0 : 255; break;                        // Square: low then high
            case 4: v = static_cast<uint8_t>(                                // Sin3: three summed sines
                        (sin8(phase) + sin8(static_cast<uint8_t>(phase * 2))
                                     + sin8(static_cast<uint8_t>(phase * 3))) / 3); break;
            default: v = inoise8(phase); break;                              // Noise (type 5): shared 1D gradient noise
        }
        const lengthType y = static_cast<lengthType>((static_cast<uint32_t>(v) * h) / 256);
        return y < h ? y : static_cast<lengthType>(h - 1);
    }

    /// A triangle wave: the textbook fold of a ramp, up over the first half and down over the second.
    static uint8_t triangle8(uint8_t x) {
        return x < 128 ? static_cast<uint8_t>(x * 2)
                       : static_cast<uint8_t>((255 - x) * 2);
    }

    /// Write one pixel into the trail plane, per channel so a 1- or 2-channel buffer still renders.
    void plot(lengthType x, lengthType y, const RGB& c, uint8_t cpl, lengthType w) {
        if (x < 0 || y < 0 || x >= w) return;
        const size_t off = (static_cast<size_t>(y) * w + x) * cpl;
        const uint8_t write = cpl < 3 ? cpl : 3;
        if (off + write > trail_.bytes()) return;
        if (write >= 1) trail_[off + 0] = c.r;
        if (write >= 2) trail_[off + 1] = c.g;
        if (write >= 3) trail_[off + 2] = c.b;
    }
};

} // namespace mm
