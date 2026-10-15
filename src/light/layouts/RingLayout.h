#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// A ring of lights evenly spaced around a circle, or an arc of one.
/// @card RingLayout.gif
///
/// The lights start at `angleFirst` and span `rotation` degrees, a full turn drawing the whole circle. A center of its own places the ring anywhere, which is how CarLights and Rings241 build their lamps and discs from rings.
///
/// @attribution Polar coordinates
class RingLayout : public LayoutBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }
    /// How many lights go around the ring.
    uint8_t  nrOfLEDs   = 24;
    /// The angle the first light sits at, in degrees.
    uint16_t angleFirst = 0;
    /// The arc spanned in degrees, less than a full turn a partial ring.
    uint16_t rotation   = 360;
    /// Whether the lights run clockwise.
    bool     clockwise  = true;
    /// The spacing out from the center.
    uint8_t  scale      = 1;
    /// The center's x before scaling; 0 places the ring by its size.
    lengthType centerX  = 0;
    /// The center's y before scaling; 0 places the ring by its size.
    lengthType centerY  = 0;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("nrOfLEDs",    nrOfLEDs,   1, 255);
        controls_.addControl("angleFirst", angleFirst, 0, 359);
        controls_.addControl("rotation",   rotation,   0, 360);
        controls_.addControl("clockwise",    clockwise);
        controls_.addControl("scale",       scale,      1, 10);
        controls_.addControl("centerX",     centerX,    0, 255);
        controls_.addControl("centerY",     centerY,    0, 255);
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        // The same walk as the emit, so a partial arc's count cannot disagree.
        LightEmitter e;
        walk(e);
        return static_cast<nrOfLightsType>(e.idx);
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        LightEmitter e{&sink};
        walk(e);
    }

    /// The radius that spaces n lights one unit apart around a circle, n / 2π.
    static float radius(uint8_t n) { return static_cast<float>(n) / kTwoPi; }

private:
    static constexpr float kPi = std::numbers::pi_v<float>;
    static constexpr float kTwoPi = 2.0f * kPi;

    /// The ring in ring order, a partial arc keeping the lights from `angleFirst` over `rotation` degrees.
    void walk(LightEmitter& e) const {
        // A zero count, reachable by writing the control directly, emits nothing and divides by nothing.
        if (nrOfLEDs == 0) return;
        const float r = radius(nrOfLEDs);
        // About 1.1 radii out on both axes by default, so the whole ring lands in the positive quadrant.
        const lengthType cx = centerX ? centerX : static_cast<lengthType>(1.1f * r);
        const lengthType cy = centerY ? centerY : static_cast<lengthType>(1.1f * r);
        for (int i = 0; i < nrOfLEDs; i++) {
            float x = static_cast<float>(scale * cx);
            float y = static_cast<float>(scale * cy);
            if (nrOfLEDs != 1) {
                const float angleRad = kPi + (kTwoPi * i) / nrOfLEDs + kTwoPi * angleFirst / 360.0f;
                x -= scale * sinf(angleRad) * r;
                y += scale * cosf(angleRad) * r;
            }
            const float ledAngle = fmodf(angleFirst + (static_cast<float>(i) / nrOfLEDs) * 360.0f, 360.0f);
            if (!inArc(ledAngle)) continue;
            // Truncating each axis toward zero is what the source does.
            if (!e.pixel(static_cast<lengthType>(static_cast<int>(x)), static_cast<lengthType>(static_cast<int>(y)), 0)) return;
        }
    }

    /// Whether a light at `ledAngle` falls in the arc from `angleFirst` over `rotation` degrees, a full turn or none keeping every light.
    bool inArc(float ledAngle) const {
        if (rotation < 1 || rotation >= 360) return true;
        if (clockwise) {
            const float endAngle = fmodf(angleFirst + rotation, 360.0f);
            // No wrap: angleFirst 180, rotation 90 ends at 270; a wrap: angleFirst 270, rotation 180 ends at 90.
            return endAngle >= angleFirst ? (ledAngle >= angleFirst && ledAngle <= endAngle)
                                          : (ledAngle >= angleFirst || ledAngle <= endAngle);
        }
        const float endAngle = fmodf(angleFirst - rotation + 360.0f, 360.0f);
        return endAngle <= angleFirst ? (ledAngle <= angleFirst && ledAngle >= endAngle)
                                      : (ledAngle <= angleFirst || ledAngle >= endAngle);
    }
};

} // namespace mm
