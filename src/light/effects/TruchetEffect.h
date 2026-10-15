#pragma once

#include "core/util/math16.h"              // BeatPhase, hashInt
#include "light/effects/EffectBase.h"
#include "light/powerfunctions/shader.h"

namespace mm {

/// A maze of interlocking arcs that never repeats, drawn without storing a single tile.
/// @card TruchetEffect.gif
///
/// Randomly turned tiles join into continuous winding paths across the surface: the pattern looks designed, and nothing designed it. This is the representative 2D shader: no 3D, no rays, no float, and cheap on any target.
///
/// @attribution Sébastien Truchet
///
/// @technical
///
/// One function of position and time, evaluated per pixel. Its `repeat` folds the plane into one tile, and `hashInt` on the cell index turns it, so two devices agree on tile (7, 3). A smoothstep across the distance to an arc stops a curve reading as a staircase.
class TruchetEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫🖌️"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast the pattern drifts.
    uint8_t bpm      = 6;
    /// Tiles across the short side.
    uint8_t scale    = 3;
    /// How fat the arcs are.
    uint8_t thickness = 60;
    /// Edge softness, which is the anti-aliasing width.
    uint8_t softness = 40;
    /// Reshuffles which way the tiles face.
    uint8_t shuffle  = 0;
    /// Slide the pattern rather than holding it still.
    bool    drift    = true;

    /// Publish the drift, the tile size, the arc shape and the shuffle.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 60);
        controls_.addControl("scale", scale, 1, 16);
        controls_.addControl("thickness", thickness, 5, 200);
        controls_.addControl("softness", softness, 1, 200);
        controls_.addControl("shuffle", shuffle, 0, 255);
        controls_.addControl("drift", drift);
    }

    /// Evaluate the tiling per pixel: which cell, which way it turns, and how near an arc.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        if (cv.dims.x < 1 || cv.dims.y < 1) return;

        phase_.advanceTo(elapsed(), bpm);
        const uint32_t t = phase_.phase(65536);

        // `scale` counts tiles over the short side, which spans -1 to 1, so a cell is 2/scale.
        const int32_t cell = (2 * 65536) / (scale > 0 ? scale : 1);
        // Drift moves the plane rather than the tiles, so paths slide and stay connected.
        const int32_t driftX = drift ? static_cast<int32_t>(t / 4u) : 0;
        const int32_t driftY = drift ? static_cast<int32_t>(t / 7u) : 0;

        const int32_t arcR = cell / 2;                                    // arcs meet at cell edges
        const int32_t thick = (static_cast<int32_t>(thickness) * cell) / 1000;
        const int32_t soft = (static_cast<int32_t>(softness) * cell) / 2000;

        shader::each(cv, 0, [&](int32_t sx, int32_t sy, angle16) -> RGB {
            const int32_t wx = sx + driftX;
            const int32_t wy = sy + driftY;

            // floorDiv, so negative space tiles the same way as positive.
            const int32_t cx = floorDiv(wx, cell);
            const int32_t cy = floorDiv(wy, cell);
            const int32_t lx = shader::repeat(wx, cell);
            const int32_t ly = shader::repeat(wy, cell);

            // A pure function of which tile it is: flipping swaps the arcs, the whole Truchet trick.
            const bool flip = (hashInt(static_cast<uint32_t>(cx + 32768),
                                       static_cast<uint32_t>(cy + 32768), 0, shuffle) & 1) != 0;
            const int32_t ax = flip ? -lx : lx;

            // Two quarter arcs meeting the boundary at the same points however the tile turns.
            const int32_t half = cell / 2;
            const int32_t d1 = ringDistance(ax + half, ly + half, arcR);
            const int32_t d2 = ringDistance(ax - half, ly - half, arcR);
            const int32_t d = shader::opUnion(d1, d2) - thick;

            // Inside the arc is lit and outside dark, with a soft ramp rather than a jagged edge.
            const frac16 cover = 65535 - shader::smoothstep(-soft, soft, d);
            if (cover == 0) return RGB{0, 0, 0};

            // Color rides the cell index and time, so neighboring paths differ.
            const uint8_t idx = static_cast<uint8_t>(
                hashInt(static_cast<uint32_t>(cx + 32768), static_cast<uint32_t>(cy + 32768), 1, shuffle)
                + (t >> 8));
            return colorFromPalette(*Palettes::active(), idx, static_cast<uint8_t>(cover >> 8));
        });
    }

private:
    /// Distance to a ring of radius `r` at the origin: zero on the line, positive either side.
    static int32_t ringDistance(int32_t x, int32_t y, int32_t r) {
        const int32_t d = shader::length(static_cast<draw::pos_t>(x), static_cast<draw::pos_t>(y));
        return d - r < 0 ? r - d : d - r;
    }

    /// Divide flooring toward negative infinity: truncation would seam the cell index at the origin.
    static int32_t floorDiv(int32_t a, int32_t b) {
        if (b <= 0) return 0;
        // Adjust the quotient rather than negating `a`: -INT32_MIN is undefined behavior.
        const int32_t q = a / b;
        return (a % b != 0 && a < 0) ? q - 1 : q;
    }

    BeatPhase phase_;   ///< the drift clock
};

}  // namespace mm
