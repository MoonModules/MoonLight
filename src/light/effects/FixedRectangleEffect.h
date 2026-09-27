#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Test effect: draws a fixed rectangle at set coordinates.
/// @card FixedRectangleEffect.gif
/// Author: limpkin (MoonLight), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
///
/// A solid box at a dialed-in position and extent, over a slow motion-trail fade.
/// So it serves as a static fixture, an alignment aid, or a painted region.
///
/// Prior art: MoonLight's FixedRectangle, whose clamping and checkerboard this reproduces.
///
/// @moreinfo
///
/// ## The checkerboard follows the box's longer side
///
/// With `alternateWhite` the box renders as white and colored tiles.
/// The toggle flips every cell when the box is wider than tall, and once a row when taller.
/// So the pattern runs along whichever side is longer.
///
/// On an RGBW grid a white tile carries the white value and a colored tile clears it.
/// That keeps a colored cell off the white LED, and leaves no stale value from a prior frame.
class FixedRectangleEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// The box has an extent on all three axes.
    Dim dimensions() const override { return Dim::D3; }

    /// The box's red channel.
    uint8_t red   = 182;
    /// Its green channel.
    uint8_t green = 15;
    /// Its blue channel.
    uint8_t blue  = 98;
    /// Its white channel, used only on an RGBW grid.
    uint8_t white = 0;

    // Named rect* so they do not hide the inherited width(), height() and depth() accessors.
    /// The box's origin on x.
    int16_t rectX = 0;
    /// Its origin on y.
    int16_t rectY = 0;
    /// Its origin on z.
    int16_t rectZ = 0;
    /// Its extent along x, clamped to the grid so a small panel fills to the edge.
    int16_t rectW = 15;
    /// Its extent along y.
    int16_t rectH = 15;
    /// Its extent along z.
    int16_t rectD = 15;

    /// Render the box as a checkerboard of white and its color.
    bool alternateWhite = false;

    /// Publish the box's color, its origin and extent, and the checkerboard.
    void defineControls() override {
        controls_.addControl("red",   red);
        controls_.addControl("green", green);
        controls_.addControl("blue",  blue);
        controls_.addControl("white", white);
        controls_.addControl("X position", rectX, 0, INT16_MAX);
        controls_.addControl("Y position", rectY, 0, INT16_MAX);
        controls_.addControl("Z position", rectZ, 0, INT16_MAX);
        controls_.addControl("Rectangle width",  rectW, 1, INT16_MAX);
        controls_.addControl("Rectangle height", rectH, 1, INT16_MAX);
        controls_.addControl("Rectangle depth",  rectD, 1, INT16_MAX);
        controls_.addControl("alternateWhite", alternateWhite);
    }

    /// Paint the box, clamped to the grid, over the trail fade.
    void tick() MM_NONBLOCKING override {
        const int w = width();
        const int h = height();
        const int d = depth();

        const draw::Canvas cv = canvas();
        const uint8_t cpl = channelsPerLight();

        // The motion trail: dim the whole buffer each frame.
        layer()->fadeToBlackBy(10);

        // Per-frame state, so a local rather than a member reset on every draw.
        bool alternate = false;

        const RGB rgb{red, green, blue};

        // The box clamped to the live grid, so an oversized extent fills to the edge.
        const int zEnd = MINi(rectZ + rectD, d > 0 ? d : 1);
        const int yEnd = MINi(rectY + rectH, h);
        const int xEnd = MINi(rectX + rectW, w);

        for (int z = rectZ; z < zEnd; z++) {
            for (int y = rectY; y < yEnd; y++) {
                for (int x = rectX; x < xEnd; x++) {
                    // One decision drives both the color and the white channel.
                    const bool isWhiteTile = alternateWhite && alternate;
                    const Coord3D p{static_cast<lengthType>(x), static_cast<lengthType>(y), static_cast<lengthType>(z)};
                    // Always written, which also clears any stale pixel from a prior frame.
                    draw::pixel(cv, p, isWhiteTile ? RGB{255, 255, 255} : rgb);
                    // Only on an RGBW grid, and cleared on a colored tile since draw::pixel writes RGB.
                    if (cpl >= 4) {
                        const size_t off = draw::offsetOf(cv, p);
                        if (off + 3 < cv.bytes) cv.data[off + 3] = isWhiteTile ? white : 0;
                    }
                    // Wider than tall, so flip the toggle every cell.
                    if (rectH < rectW) alternate = !alternate;
                }
                // Taller than wide, so flip once a row instead.
                if (rectH > rectW) alternate = !alternate;
            }
        }
    }

private:
    /// The smaller of two, which clamps each axis and keeps a degenerate depth one deep.
    static int MINi(int a, int b) { return a < b ? a : b; }
};

} // namespace mm