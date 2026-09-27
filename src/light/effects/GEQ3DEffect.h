#pragma once

#include "core/util/math16.h"            // map32: the shared, fencepost-safe range map
#include "light/effects/EffectBase.h"

namespace mm {

/// Audio-reactive 3D graphic-equalizer effect.
/// @card GEQ3DEffect.gif
/// Author: @TroyHacks (MoonModules, GPLv3), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Effects/E_MoonModules.h
///
/// The 16 bands rise as bars on a 2D grid, drawn with faked depth.
/// GEQEffect in this folder draws the same data flat instead.
///
/// Prior art: MoonLight's GEQ3D, descended from the WLED-MM effect.
///
/// @moreinfo
///
/// ## The projector is the vanishing point
///
/// Each bar's side and top faces are lines running from the bar toward a sweeping projector.
/// `depth` shortens each line so it stops partway, and that converging foreshortening is the look.
/// The projector sweeps left and right, and each band is painted away from it.
/// So the perspective always points away from the moving vanishing point.
/// The front faces fill flat, and a border optionally outlines each bar.
///
/// MoonLight's anti-alias toggle is dropped, since `draw::line` is a crisp Bresenham.
class GEQ3DEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin, MoonModules, audio-reactive.
    const char* tags() const override { return "💫🌙🎶"; }
    /// The perspective is faked on a plane, so this is a 2D effect.
    Dim dimensions() const override { return Dim::D2; }

    /// The projector's sweep rate, timed in BPM so every device agrees on its position.
    uint8_t speed     = 2;
    /// How strongly a bar's front face fills.
    uint8_t frontFill = 228;
    /// The row the vanishing point sits on.
    uint8_t horizon   = 0;
    /// How far the side and top lines reach toward the projector.
    uint8_t depth     = 176;
    /// How many bands are shown, where fewer gives wider bars.
    uint8_t numBands  = 16;
    /// Outline each bar.
    bool    borders   = true;

    /// Publish the sweep, the perspective and the bar's faces.
    void defineControls() override {
        controls_.addControl("speed", speed, 1, 10);
        controls_.addControl("frontFill", frontFill, 0, 255);
        // A fixed slider, clamped to the live row count in tick(): a relative range is not expressible here.
        controls_.addControl("horizon", horizon, 0, 255);
        controls_.addControl("depth", depth, 0, 255);
        controls_.addControl("numBands", numBands, 2, 16);
        controls_.addControl("borders", borders);
    }

    /// Sweep the projector, then draw each bar's faces converging toward it.
    void tick() MM_NONBLOCKING override {
        if (numBands == 0) return;

        const int cols = width();
        const int rows = height();

        const draw::Canvas cv = canvas();

        // The motion trail: dim the buffer each frame rather than clearing it.
        layer()->fadeToBlackBy(16);

        // A time-based triangle, so every device shows the projector at the same place.
        const uint8_t bpm = static_cast<uint8_t>(speed * 3);
        const uint8_t sweep = triwave8(beat8(bpm, elapsed()));   // a triangle over time
        // Capped to the column count, or a narrow grid truncates each bar's width to 0 and piles them at x=0.
        const int NUM_BANDS = numBands <= cols ? static_cast<int>(numBands) : cols;
        const int projector = static_cast<int>(static_cast<uint32_t>(sweep) * cols / 255u);
        // The control is clamped to the grid, since it spans a fixed range.
        const int hzn = horizon < rows ? horizon : rows - 1;
        const int split = map32(projector, 0, cols, 0, NUM_BANDS - 1);

        const AudioFrame* f = AudioService::latestFrame();

        // Each band's magnitude mapped onto the bar height, reduced a little on a small panel.
        uint8_t heights[16] = {0};
        const int maxHeight = lroundf(float(rows) * ((rows < 18) ? 0.75f : 0.85f));
        for (int i = 0; i < NUM_BANDS; i++) {
            int band = i;
            if (NUM_BANDS < 16) band = map32(band, 0, NUM_BANDS, 0, 16);  // spread over all 16 bands
            if (band > 15) band = 15;
            heights[i] = map8(f->bands[band], 0, static_cast<uint8_t>(maxHeight));
        }

        const RGB black{0, 0, 0};

        // Right faces and tops, for the bands at or left of the split, painted left to right.
        for (int i = 0; i <= split; i++) {
            const uint16_t colorIndex = map32(cols / NUM_BANDS * i, 0, cols, 0, 256);
            const RGB ledColor = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(colorIndex));
            const int linex = i * (cols / NUM_BANDS);

            if (heights[i] > 1) {
                const RGB sideColor = blend(ledColor, black, static_cast<uint8_t>(255 - 32));
                const int pPos = MAXi(0, linex + (cols / NUM_BANDS) - 1);
                // Stacked perspective lines from the bar's right edge toward the projector.
                for (int y = (i < NUM_BANDS - 1) ? heights[i + 1] : 0; y <= heights[i]; y++) {
                    if (rows - y > 0)
                        draw::line(cv, {static_cast<lengthType>(pPos), static_cast<lengthType>(rows - y - 1), 0},
                                   {static_cast<lengthType>(projector), static_cast<lengthType>(hzn), 0}, sideColor, depth);
                }

                const RGB topColor = blend(ledColor, black, static_cast<uint8_t>(255 - 128));
                // Skipped directly under the projector, which the pass below handles.
                if (heights[i] < rows - hzn && (projector <= linex || projector >= pPos)) {
                    if (rows - heights[i] > 1) {
                        for (int x = linex; x <= pPos; x++)
                            draw::line(cv, {static_cast<lengthType>(x), static_cast<lengthType>(rows - heights[i] - 2), 0},
                                       {static_cast<lengthType>(projector), static_cast<lengthType>(hzn), 0}, topColor, depth);
                    }
                }
            }
        }

        // Left faces and tops, for the bands right of the split, painted right to left.
        for (int i = NUM_BANDS - 1; i > split; i--) {
            const uint16_t colorIndex = map32(cols / NUM_BANDS * i, 0, cols - 1, 0, 255);
            const RGB ledColor = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(colorIndex));
            const int linex = i * (cols / NUM_BANDS);
            const int pPos = MAXi(0, linex + (cols / NUM_BANDS) - 1);

            if (heights[i] > 1) {
                const RGB sideColor = blend(ledColor, black, static_cast<uint8_t>(255 - 32));
                // Stacked perspective lines from the bar's left edge toward the projector.
                for (int y = (i > 0) ? heights[i - 1] : 0; y <= heights[i]; y++) {
                    if (rows - y > 0)
                        draw::line(cv, {static_cast<lengthType>(linex), static_cast<lengthType>(rows - y - 1), 0},
                                   {static_cast<lengthType>(projector), static_cast<lengthType>(hzn), 0}, sideColor, depth);
                }

                const RGB topColor = blend(ledColor, black, static_cast<uint8_t>(255 - 128));
                if (heights[i] < rows - hzn && (projector <= linex || projector >= pPos)) {
                    if (rows - heights[i] > 1) {
                        for (int x = linex; x <= pPos; x++)
                            draw::line(cv, {static_cast<lengthType>(x), static_cast<lengthType>(rows - heights[i] - 2), 0},
                                       {static_cast<lengthType>(projector), static_cast<lengthType>(hzn), 0}, topColor, depth);
                    }
                }
            }
        }

        // The projector's own bar, the front fills and the borders, all bands left to right.
        for (int i = 0; i < NUM_BANDS; i++) {
            const uint16_t colorIndex = map32(cols / NUM_BANDS * i, 0, cols - 1, 0, 255);
            const RGB ledColor = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(colorIndex));
            const int linex = i * (cols / NUM_BANDS);
            const int pPos  = linex + (cols / NUM_BANDS) - 1;
            const int pPos1 = linex + (cols / NUM_BANDS);

            // The top of the bar directly under the projector, which the passes above skipped.
            if (projector >= linex && projector <= pPos) {
                if ((heights[i] > 1) && (heights[i] < rows - hzn) && (rows - heights[i] > 1)) {
                    const RGB topColor = blend(ledColor, black, static_cast<uint8_t>(255 - 128));
                    for (int x = linex; x <= pPos; x++)
                        draw::line(cv, {static_cast<lengthType>(x), static_cast<lengthType>(rows - heights[i] - 2), 0},
                                   {static_cast<lengthType>(projector), static_cast<lengthType>(hzn), 0}, topColor, depth);
                }
            }

            if ((heights[i] > 1) && (rows - heights[i] > 0)) {
                RGB frontColor = blend(ledColor, black, static_cast<uint8_t>(255 - frontFill));
                // Vertical lines across the bar face, from the floor up to its height.
                for (int x = linex; x < pPos1; x++)
                    draw::line(cv, {static_cast<lengthType>(x), static_cast<lengthType>(rows - 1), 0},
                               {static_cast<lengthType>(x), static_cast<lengthType>(rows - heights[i] - 1), 0}, frontColor);

                if (!borders && heights[i] > rows - hzn) {
                    // Match the side fill, then a top line standing in for the hidden top.
                    if (frontFill == 0) frontColor = blend(ledColor, black, static_cast<uint8_t>(255 - 32));
                    draw::line(cv, {static_cast<lengthType>(linex), static_cast<lengthType>(rows - heights[i] - 1), 0},
                               {static_cast<lengthType>(linex + (cols / NUM_BANDS) - 1), static_cast<lengthType>(rows - heights[i] - 1), 0}, frontColor);
                }

                if (borders && (rows - heights[i] > 1)) {
                    const lengthType bottom = static_cast<lengthType>(rows - 1);
                    const lengthType topY   = static_cast<lengthType>(rows - heights[i] - 1);
                    const lengthType topY2  = static_cast<lengthType>(rows - heights[i] - 2);
                    const lengthType lx     = static_cast<lengthType>(linex);
                    const lengthType rx     = static_cast<lengthType>(linex + (cols / NUM_BANDS) - 1);
                    draw::line(cv, {lx, bottom, 0}, {lx, topY, 0}, ledColor);   // left side line
                    draw::line(cv, {rx, bottom, 0}, {rx, topY, 0}, ledColor);   // right side line
                    draw::line(cv, {lx, topY2, 0}, {rx, topY2, 0}, ledColor);   // top line
                    draw::line(cv, {lx, bottom, 0}, {rx, bottom, 0}, ledColor); // bottom line
                }
            }
        }
    }

private:
    /// The larger of two, kept local so the loops above read like the source they follow.
    static int MAXi(int a, int b) { return a > b ? a : b; }
    // The `depth` control hides the inherited grid-depth accessor, so qualify that one where used.
};

} // namespace mm
