#pragma once

#include "core/util/math16.h"            // map32: the shared, fencepost-safe range map
#include "light/effects/EffectBase.h"
#include "light/powerfunctions/audioReadout.h"   // barsOf: the bands, the thirds or the keys

namespace mm {

/// A 3D-perspective graphic equalizer: audio bands rise as bars with faked depth.
/// @card GEQ3DEffect.gif
///
/// Side and top lines run toward a "projector" vanishing point that sweeps left and right, shortened by `depth`. Bands left of it paint right-to-left and bands right of it left-to-right, with per-face darkening and optional `borders`. @ref GEQEffect draws the same data flat.
///
/// @attribution troyhacks
///
/// @technical
///
/// The converging foreshortening of the lines is the look, so the perspective always points away from the moving vanishing point. The lines are crisp Bresenham, so there is no anti-alias toggle.
class GEQ3DEffect : public EffectBase {
public:
    /// Catalog tags: MoonModules origin, audio-reactive.
    const char* tags() const override { return "🌙🎶"; }
    /// The perspective is faked on a plane, so this is a 2D effect.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast the projector sweeps, the same position on every device.
    uint8_t speed     = 2;
    /// How strongly a bar's front face fills.
    uint8_t frontFill = 228;
    /// How strongly a bar's side faces fill, which is what shows its depth.
    uint8_t sideFill  = 96;
    /// The row the vanishing point (the projector) sits on.
    uint8_t horizon   = 0;
    /// How far the side and top lines reach toward the projector.
    uint8_t depth     = 176;
    /// The bars shown, capped by the source and the grid; fewer are wider.
    uint8_t numBands  = kAudioNoteKeys;
    /// Outline each bar.
    bool    borders   = true;
    /// The bars: the sixteen, the third octaves or the piano's keys.
    uint8_t bands     = kBandsThirds;

    /// Publish the sweep, the perspective, the bar's faces and the bars' source.
    void defineControls() override {
        controls_.addControl("speed", speed, 1, 10);
        controls_.addControl("frontFill", frontFill, 0, 255);
        controls_.addControl("sideFill", sideFill, 0, 255);
        // A fixed slider, clamped to the live row count in tick(): a relative range is not expressible here.
        controls_.addControl("horizon", horizon, 0, 255);
        controls_.addControl("depth", depth, 0, 255);
        controls_.addControl("numBands", numBands, 2, kAudioNoteKeys);
        controls_.addControl("borders", borders);
        controls_.addSelect("bands", bands, kBarScaleOptions, kBandsCount);
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
        const AudioFrame* f = AudioService::latestFrame();
        const AudioBars bars = barsOf(*f, bands);
        // Capped to the column count, or a narrow grid truncates each bar's width to 0 and piles them at x=0.
        const int NUM_BANDS = std::min({static_cast<int>(numBands), static_cast<int>(bars.n), cols});   // no more bars than the source has or the grid can show
        if (NUM_BANDS < 1) return;   // an empty grid has no column to give a bar
        // Bar i spans columns edge(i) up to edge(i + 1), so the bars fill the width whatever it divides into.
        const auto edge = [cols, NUM_BANDS](int i) MM_NONBLOCKING { return i * cols / NUM_BANDS; };
        const int projector = static_cast<int>(static_cast<uint32_t>(sweep) * cols / 255u);
        // The control is clamped to the grid, since it spans a fixed range.
        const int hzn = horizon < rows ? horizon : rows - 1;
        const int split = map32(projector, 0, cols, 0, NUM_BANDS - 1);

        // Each band's magnitude mapped onto the bar height, reduced a little on a small panel.
        uint8_t heights[kAudioNoteKeys] = {0};
        const int maxHeight = lroundf(float(rows) * ((rows < 18) ? 0.75f : 0.85f));
        for (int i = 0; i < NUM_BANDS; i++) {
            int band = map32(i, 0, NUM_BANDS, 0, bars.n);   // spread over every bar the source has
            if (band > bars.n - 1) band = bars.n - 1;
            heights[i] = map8(bars.mag[band], 0, static_cast<uint8_t>(maxHeight));
        }

        const RGB black{0, 0, 0};

        // Right faces and tops, for the bands at or left of the split, painted left to right.
        for (int i = 0; i <= split; i++) {
            const uint16_t colorIndex = map32(edge(i), 0, cols - 1, 0, 255);
            const RGB ledColor = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(colorIndex));
            const int linex = edge(i);

            if (heights[i] > 1) {
                const RGB sideColor = blend(ledColor, black, static_cast<uint8_t>(255 - sideFill));
                const int pPos = MAXi(0, edge(i + 1) - 1);
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
            const uint16_t colorIndex = map32(edge(i), 0, cols - 1, 0, 255);
            const RGB ledColor = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(colorIndex));
            const int linex = edge(i);
            const int pPos = MAXi(0, edge(i + 1) - 1);

            if (heights[i] > 1) {
                const RGB sideColor = blend(ledColor, black, static_cast<uint8_t>(255 - sideFill));
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
            const uint16_t colorIndex = map32(edge(i), 0, cols - 1, 0, 255);
            const RGB ledColor = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(colorIndex));
            const int linex = edge(i);
            const int pPos  = edge(i + 1) - 1;
            const int pPos1 = edge(i + 1);

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
                               {static_cast<lengthType>(edge(i + 1) - 1), static_cast<lengthType>(rows - heights[i] - 1), 0}, frontColor);
                }

                if (borders && (rows - heights[i] > 1)) {
                    const lengthType bottom = static_cast<lengthType>(rows - 1);
                    const lengthType topY   = static_cast<lengthType>(rows - heights[i] - 1);
                    const lengthType topY2  = static_cast<lengthType>(rows - heights[i] - 2);
                    const lengthType lx     = static_cast<lengthType>(linex);
                    const lengthType rx     = static_cast<lengthType>(edge(i + 1) - 1);
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
};

} // namespace mm
