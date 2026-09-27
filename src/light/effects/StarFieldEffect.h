#pragma once

#include "core/util/math16.h"            // map32: the shared, fencepost-safe range map
#include "light/effects/EffectBase.h"
#include "light/powerfunctions/shader.h"   // project: the shared pinhole

namespace mm {

/// Star-field effect: drifting points like flying through stars.
/// @card StarFieldEffect.gif
/// Author: @Brandon502 (MoonLight), inspired by Daniel Shiffman / Coding Train, https://www.youtube.com/watch?v=17WoOqgXsRM , https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
///
/// Each star is a 3D point drifting toward the viewer, re-projected onto the panel every advance.
/// So a near star splays outward and flies off the edge while a distant one sits near the center.
/// That is the classic warp look, and a star passing the camera respawns at the far plane.
///
/// Prior art: MoonLight's StarField, whose model and respawn rule this reproduces.
///
/// @moreinfo
///
/// ## Depth drives brightness, and the fade draws the streak
///
/// A nearer star is brighter, and with `usePalette` its depth picks the hue too.
/// `blur` fades the previous frame rather than clearing it, so each star leaves a motion streak.
/// `speed` throttles how often the field advances, and 0 pauses it.
///
/// ## The port is exact on purpose
///
/// The per-star perspective divide keeps floats, running once a star rather than once a pixel.
/// The center offset and the projection scale are integer halves, so an odd-width panel matches.
/// A ported effect that looks different is a regression, so fidelity wins over tidiness here.
class StarFieldEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight origin.
    const char* tags() const override { return "💫🖌️"; }
    /// Writes the z=0 slice.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast the field advances, where 0 pauses it.
    uint8_t speed      = 20;
    /// How many stars are active.
    uint8_t numStars   = 16;
    /// The fade rate, where a stronger fade leaves shorter streaks.
    uint8_t blur       = 128;
    /// Color the stars from the palette rather than in grayscale.
    bool    usePalette = false;

    /// Publish the flight speed, the population, the streaks and the coloring.
    void defineControls() override {
        controls_.addControl("speed", speed, 0, 30);
        controls_.addControl("numStars", numStars, 1, 255);
        controls_.addControl("blur", blur, 0, 255);
        controls_.addControl("usePalette", usePalette);
    }

    /// Size the star table to the control's maximum, re-seeding only when the grid changes.
    void prepare() override {
        const lengthType w = width();
        const lengthType h = height();
        if (w > 0 && h > 0) {
            // A live count change never reallocates, since the table is sized to the maximum.
            stars_.resize(kMaxStars);
            if (stars_ && (w != seedW_ || h != seedH_)) {
                for (uint16_t i = 0; i < kMaxStars; i++) spawn(stars_[i], w, h, /*far=*/false);
                seedW_ = w;
                seedH_ = h;
            }
        } else {
            stars_.resize(0);
            seedW_ = 0;
            seedH_ = 0;
        }
    }

    /// Fade the streaks, then advance and re-project every star when a step is due.
    void tick() MM_NONBLOCKING override {
        if (!stars_) return;

        const lengthType w = width();
        const lengthType h = height();

        // Paused at 0, and otherwise advancing at most once per interval.
        if (speed == 0) return;
        const uint32_t now = elapsed();

        // Every frame, outside the step gate: the Layer already scales this rate by elapsed time.
        layer()->fadeToBlackBy(blur);

        if (now - step_ < 1000u / speed) return;

        const draw::Canvas cv = canvas();

        const int sizeX = w;
        const int sizeY = h;
        // Integer halves, so an odd-width panel projects as the source does.
        const int halfX = sizeX / 2;
        const int halfY = sizeY / 2;

        const uint8_t n = numStars;  // 1..255, never exceeds kMaxStars
        for (uint8_t i = 0; i < n; i++) {
            Star& s = stars_[i];

            // A star behind the camera projects off-grid and is not drawn.
            bool inBounds = false;
            int sx = 0, sy = 0;
            // The shared pinhole, verified identical to the float form it replaces.
            int32_t projX = 0, projY = 0;
            if (shader::project(static_cast<int32_t>(s.x * 65536.0f),
                                static_cast<int32_t>(s.y * 65536.0f),
                                static_cast<int32_t>(s.z * 65536.0f), 65536, projX, projY)) {
                sx = halfX + static_cast<int>((projX * halfX) / 65536);
                sy = halfY + static_cast<int>((projY * halfY) / 65536);
                inBounds = (sx >= 0 && sx < sizeX && sy >= 0 && sy < sizeY);
            }

            if (inBounds) {
                RGB col;
                if (usePalette) {
                    // A nearer star is brighter.
                    const uint8_t bri = static_cast<uint8_t>(map32(static_cast<int>(s.z), 0, sizeX, 255, 150));
                    col = colorFromPalette(*Palettes::active(), s.colorIndex, bri);
                } else {
                    // A base intensity from the star's own index, scaled by its depth.
                    int color = map32(s.colorIndex, 0, 255, 120, 255);
                    const int brightness = map32(static_cast<int>(s.z), 0, sizeX, 7, 10);
                    color = static_cast<int>(color * (brightness / 10.0f));
                    if (color < 0) color = 0;
                    if (color > 255) color = 255;
                    const uint8_t c = static_cast<uint8_t>(color);
                    col = RGB{c, c, c};
                }
                draw::pixel(cv, {static_cast<lengthType>(sx), static_cast<lengthType>(sy), 0}, col);
            }

            // Advance toward the viewer, respawning at the far plane once it passes or leaves.
            s.z -= 1.0f;
            if (s.z <= 0.0f || !inBounds) spawn(s, w, h, /*far=*/true);
        }

        step_ = now;
    }

private:
    /// One star: a position in space and its own palette index.
    struct Star {
        float x, y, z;
        uint8_t colorIndex;
    };
    /// The table's size, which is the `numStars` control's maximum.
    static constexpr uint16_t kMaxStars = 255;

    /// Spawn a star: `far` places it at the far plane, and otherwise at a random depth.
    void spawn(Star& s, lengthType w, lengthType h, bool far) {
        s.x = static_cast<float>(randRange(-w, w));
        s.y = static_cast<float>(randRange(-h, h));
        s.z = far ? static_cast<float>(w)
                  : static_cast<float>(w > 0 ? rng_.next16() % w : 0);
        s.colorIndex = rng_.next8();
    }

    /// A uniform integer in the half-open range.
    int randRange(int lo, int hi) {
        if (hi <= lo) return lo;
        const uint32_t span = static_cast<uint32_t>(hi - lo);
        return lo + static_cast<int>(rng_.next16() % span);
    }

    ScratchBuffer<Star> stars_{*this};   ///< the star table, sized to the control's maximum
    lengthType seedW_ = 0;               ///< the width the stars were spawned for
    lengthType seedH_ = 0;               ///< and the height, so a rebuild re-seeds only on a change
    uint32_t step_ = 0;                  ///< when the field last advanced
    Random8 rng_;                        ///< the spawn's placement and color
};

} // namespace mm