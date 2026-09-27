#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Test effect: axis-aligned planes sweeping in sync, where red, green and blue name x, y and z.
/// @card LinesEffect.gif
///
/// Three planes sweep together, each color naming the axis it moves along.
/// That makes it the effect for verifying a preview's axis orientation.
/// A second mode drops the sweep for a static panel-mapping aid instead.
class LinesEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫"; }
    /// All three planes need all three axes.
    Dim dimensions() const override { return Dim::D3; }

    /// How fast the planes sweep.
    uint8_t speed = 30;
    /// Which axis sweeps, or all three together.
    uint8_t axis  = 0;
    /// Sweep the planes, or show the static panel dots.
    uint8_t mode  = 0;
    /// A panel block's width in lights, for the dots mapping.
    uint8_t panelW = 16;
    /// Its height, kept separate since a non-square panel would otherwise skip whole rows.
    uint8_t panelH = 16;

    /// Publish the sweep, the axis and the panel geometry, hiding whichever mode is inactive.
    void defineControls() override {
        static constexpr const char* kAxisOptions[] = {"all", "x (red)", "y (green)", "z (blue)"};
        static constexpr const char* kModeOptions[] = {"lines", "panel dots"};
        controls_.addSelect("mode", mode, kModeOptions, 2);
        // defineControls reruns on every control change, so toggling `mode` re-hides these.
        const bool dots = (mode == 1);
        controls_.addControl("speed", speed, 1, 240);
        controls_.setHidden(controls_.count() - 1, dots);          // lines only
        controls_.addSelect("axis", axis, kAxisOptions, 4);
        controls_.setHidden(controls_.count() - 1, dots);          // lines only
        controls_.addControl("panelW", panelW, 1, 64);
        controls_.setHidden(controls_.count() - 1, !dots);         // panel dots only
        controls_.addControl("panelH", panelH, 1, 64);
        controls_.setHidden(controls_.count() - 1, !dots);         // panel dots only
    }

    /// Sweep the three planes, or draw the panel dots when that mode is chosen.
    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        const lengthType w   = width();
        const lengthType h   = height();
        const lengthType d   = depth();
        const uint8_t    cpl = channelsPerLight();

        // A zero in any dimension leaves a null buffer, so guard before the clear below.
        if (!buf) return;

        memset(buf, 0, static_cast<size_t>(w) * h * d * cpl);

        // The mapping aid: each block lights one more dot than the last, readable straight off a wall.
        if (mode == 1) {
            const lengthType pw = panelW ? panelW : 1;
            const lengthType ph = panelH ? panelH : 1;
            const lengthType panelsPerRow = (w + pw - 1) / pw;
            for (lengthType py = 0; py < h; py += ph) {
                for (lengthType px = 0; px < w; px += pw) {
                    const lengthType panelIdx = (py / ph) * panelsPerRow + (px / pw);
                    const lengthType dots = panelIdx + 1;   // the first panel shows one dot
                    for (lengthType i = 0; i < dots && (px + i) < w && i < pw; i++) {
                        const size_t off = (static_cast<size_t>(py) * w + (px + i)) * cpl;
                        if (cpl >= 1) buf[off + 0] = 255;   // white, for the clearest read
                        if (cpl >= 2) buf[off + 1] = 255;
                        if (cpl >= 3) buf[off + 2] = 255;
                    }
                }
            }
            return;
        }

        // A sawtooth over the full range, kept wide against an overflow.
        const uint32_t period = 60000u / static_cast<uint32_t>(speed ? speed : 1);
        const uint16_t beat   = static_cast<uint16_t>(
            (static_cast<uint64_t>(elapsed() % period) * 65535u) / period
        );

        auto setRGB = [&](lengthType x, lengthType y, lengthType z,
                          uint8_t r, uint8_t g, uint8_t b) {
            size_t off = (static_cast<size_t>(z) * h * w
                         + static_cast<size_t>(y) * w + x) * cpl;
            if (cpl >= 1) buf[off + 0] = r;
            if (cpl >= 2) buf[off + 1] = g;
            if (cpl >= 3) buf[off + 2] = b;
        };

        // Bucketed so the sweep reaches the last index, where the textbook form falls one short.
        auto sweepIndex = [&](lengthType n) {
            return static_cast<lengthType>(static_cast<uint32_t>(beat) * n / 65536u);
        };

        // Red sweeps along x, left to right.
        if (w > 1 && (axis == 0 || axis == 1)) {
            const lengthType x = sweepIndex(w);
            for (lengthType z = 0; z < d; z++)
                for (lengthType y = 0; y < h; y++)
                    setRGB(x, y, z, 255, 0, 0);
        }

        // Green sweeps along y, top to bottom.
        if (h > 1 && (axis == 0 || axis == 2)) {
            const lengthType y = sweepIndex(h);
            for (lengthType z = 0; z < d; z++)
                for (lengthType x = 0; x < w; x++)
                    setRGB(x, y, z, 0, 255, 0);
        }

        // Blue sweeps along z, front to back, on a volume.
        if (d > 1 && (axis == 0 || axis == 3)) {
            const lengthType z = sweepIndex(d);
            for (lengthType y = 0; y < h; y++)
                for (lengthType x = 0; x < w; x++)
                    setRGB(x, y, z, 0, 0, 255);
        }
    }
};

} // namespace mm
