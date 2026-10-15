#pragma once

#include "core/util/math16.h"              // BeatPhase, sin16/cos16
#include "light/effects/EffectBase.h"
#include "light/powerfunctions/shader.h"             // project, depthFade, rotate

namespace mm {

/// A rotating 3D object drawn as shaded spheres, the demoscene classic that named the technique.
/// @card VectorBallsEffect.gif
///
/// The smallest complete demonstration of putting 3D on a panel: rotate, project, sort back to front, shade by distance, draw. Without painter's ordering a far ball paints over a near one and the object turns inside out.
///
/// @attribution Demoscene
///
/// @technical
///
/// The shape is a point table, a cube's eight corners plus its six face centers; a different shape is a different table. Every step is a shared power function.
class VectorBallsEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫🖌️"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// Rotation speed.
    uint8_t bpm      = 12;
    /// A ball's radius at the object's center, in pixels.
    uint8_t size     = 2;
    /// How far apart the balls sit.
    uint8_t spread   = 90;
    /// How far the object is from the viewer.
    uint8_t distance = 200;
    /// Dim the far balls, which is what reads as depth.
    bool    fade     = true;

    /// Publish the rotation, the object's shape and the camera.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 60);
        controls_.addControl("size", size, 1, 12);
        controls_.addControl("spread", spread, 20, 255);
        controls_.addControl("distance", distance, 40, 255);
        controls_.addControl("fade", fade);
    }

    /// Rotate and project every point, sort them back to front, then draw each as a disc.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();

        phase_.advanceTo(elapsed(), bpm);
        const uint32_t t = phase_.phase(65536);
        const angle16 yaw   = static_cast<angle16>(t);
        const angle16 pitch = static_cast<angle16>(t * 2 / 3);   // a second axis, so it tumbles

        const int32_t s = static_cast<int32_t>(spread) * 256;
        const int32_t camZ = static_cast<int32_t>(distance) * 512;

        // Depth is kept, so the draw below can be sorted by it.
        struct Ball { int32_t sx, sy, z; uint8_t hue; };
        Ball balls[kPoints];
        uint8_t live = 0;
        for (uint8_t i = 0; i < kPoints; i++) {
            int32_t px = kShape[i][0] * s;
            int32_t py = kShape[i][1] * s;
            int32_t pz = kShape[i][2] * s;

            // Two rotations through the shared helper, yaw then pitch.
            shader::rotate(px, pz, yaw);
            shader::rotate(py, pz, pitch);

            int32_t sx, sy;
            if (!shader::project(px, py, pz + camZ, 65536, sx, sy)) continue;   // behind the viewer
            balls[live++] = {sx, sy, pz + camZ, static_cast<uint8_t>(i * (256 / kPoints))};
        }

        // Far balls first, so near ones cover them and the object holds its shape.
        for (uint8_t i = 1; i < live; i++) {
            const Ball key = balls[i];
            int8_t j = static_cast<int8_t>(i - 1);
            while (j >= 0 && balls[j].z < key.z) { balls[j + 1] = balls[j]; j--; }
            balls[j + 1] = key;
        }

        draw::fill(cv, RGB{0, 0, 0});

        const int32_t shortSide = (w < h ? w : h);
        for (uint8_t i = 0; i < live; i++) {
            // Back to pixels against the short side, so a circle stays circular.
            const lengthType px = static_cast<lengthType>(
                w / 2 + (balls[i].sx * shortSide) / (2 * 65536));
            const lengthType py = static_cast<lengthType>(
                h / 2 + (balls[i].sy * shortSide) / (2 * 65536));

            // Shrunk by the divide that placed it, and scaled so `size` is a radius in pixels.
            const int32_t r = (static_cast<int32_t>(size) * camZ) / (balls[i].z > 0 ? balls[i].z : 1);
            const lengthType maxR = static_cast<lengthType>(shortSide / 4);
            const lengthType radius = static_cast<lengthType>(r < 1 ? 1 : (r > maxR ? maxR : r));

            const uint8_t bri = fade ? shader::depthFade(balls[i].z, camZ * 3) : 255;
            if (bri == 0) continue;
            draw::fillCircle(cv, px, py, radius,
                             colorFromPalette(*Palettes::active(), balls[i].hue, bri));
        }
    }

private:
    /// How many points the shape holds.
    static constexpr uint8_t kPoints = 14;

    /// A cube's eight corners plus its six face centers, in units of `spread`.
    static constexpr int8_t kShape[kPoints][3] = {
        {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
        {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
        { 0,  0, -2}, { 0,  0,  2}, { 0, -2,  0}, { 0,  2,  0}, {-2,  0,  0}, { 2,  0,  0},
    };


    BeatPhase phase_;   ///< the rotation clock
};

}  // namespace mm
