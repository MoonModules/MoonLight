#pragma once

#include "light/effects/EffectBase.h"   // pulls in platform_config.h via light_types.h

#if MM_HEAVY_COMPUTE   // only where the SoC declares a hardware FPU, for the reason raymarch.h carries

#include "core/util/math16.h"              // BeatPhase
#include "light/powerfunctions/raymarch.h"
#include "light/powerfunctions/shader.h"

namespace mm {

/// A lit 3D scene rendered by marching a ray through a distance field, one ray per pixel.
/// @card RaymarchEffect.gif
///
/// Nothing draws a sphere: the scene is a function returning the distance to the nearest surface, and the spheres emerge where each ray stops. Writing a different 3D effect means writing a different `scene()`, which is the whole API. Cost is per pixel, and `steps` trades quality against it.
///
/// @attribution Iñigo Quilez, John Hart
///
/// @guide
///
/// ## Where it runs
///
/// It is compiled only for a chip with a hardware FPU, so a classic ESP32 does not offer it.
///
/// @technical
///
/// The tracing, the primitives, the normal, the camera and the lighting live in `raymarch.h`. This effect is a scene of two spheres and a floor plus a choice of colors.
class RaymarchEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫🖌️"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast the scene animates.
    uint8_t bpm      = 10;
    /// Ray marching steps: the quality and cost knob.
    uint8_t steps    = 40;
    /// How much the two spheres melt into each other.
    uint8_t blend    = 80;
    /// Camera height above the floor.
    uint8_t cameraY  = 128;
    /// Include the ground plane.
    bool    showFloor = true;

    /// Publish the animation, the ray budget, the melt and the camera.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 60);
        controls_.addControl("steps", steps, 8, 96);
        controls_.addControl("blend", blend, 0, 255);
        controls_.addControl("cameraY", cameraY, 0, 255);
        controls_.addControl("showFloor", showFloor);
    }

    /// Place the spheres and the camera, then march one ray per pixel into the scene.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        if (cv.dims.x < 1 || cv.dims.y < 1) return;

        phase_.advanceTo(elapsed(), bpm);

        // Each oscillator wraps in its own angle, since scaling one wrapped angle snaps per cycle.
        const auto wave = [this](uint32_t rate, uint16_t offset) {
            const angle16 a = static_cast<angle16>((phase_.phase(65536) * rate) / 100u + offset);
            return static_cast<float>(static_cast<int32_t>(sin16(a))) / 32768.0f;
        };

        // The spheres orbit each other, and `blend` sets how far apart they still merge.
        a_ = {wave(100, 16384) * 1.2f, 0.3f + wave(130, 0) * 0.4f, wave(100, 0) * 1.2f};
        b_ = {-wave(80, 16384) * 1.1f, 0.5f + wave(110, 16384) * 0.3f, -wave(80, 0) * 1.1f};
        k_ = 0.05f + static_cast<float>(blend) / 255.0f * 1.2f;

        // Aimed at the subject rather than level, or the top half of the panel is empty sky.
        raymarch::Camera cam;
        cam.position = {0.0f, 0.4f + static_cast<float>(cameraY) / 255.0f * 2.5f, -3.0f};
        cam.target   = {0.0f, 0.2f, 0.0f};
        cam.focal    = 1.1f;

        const raymarch::Vec3 light{0.5f, 0.8f, -0.3f};

        shader::each(cv, 0, [&](int32_t sx, int32_t sy, angle16) -> RGB {
            const raymarch::Hit h = raymarch::march(
                [this](raymarch::Vec3 p) { return scene(p); },
                cam.position, cam.ray(sx, sy), steps);
            if (!h.hit) return RGB{0, 0, 0};
            // Depth tints the palette, which gives the scene its sense of space.
            const uint8_t idx = static_cast<uint8_t>(static_cast<int>(h.dist * 40.0f) & 0xFF);
            return colorFromPalette(*Palettes::active(), idx,
                                    raymarch::diffuse(h.normal, light));
        });
    }

private:
    /// The scene: the distance to the nearest surface, which is the whole world this renders.
    float scene(raymarch::Vec3 p) const {
        float d = raymarch::smin(raymarch::sdSphere(p, a_, 0.8f),
                                 raymarch::sdSphere(p, b_, 0.7f), k_);
        if (showFloor) d = raymarch::opUnion(d, raymarch::sdPlane(p, -1.0f));
        return d;
    }

    raymarch::Vec3 a_, b_;   ///< the two spheres' centers this frame
    float k_ = 0.5f;         ///< the melt radius between them
    BeatPhase phase_;        ///< the orbit clock
};

}  // namespace mm

#endif  // MM_HEAVY_COMPUTE
