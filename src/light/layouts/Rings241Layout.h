#pragma once

#include "light/layouts/LayoutBase.h"
#include <numbers>

namespace mm {

/// Layout of the 241-LED concentric-rings disc.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
///
/// @moreinfo
///
/// The classic 241-LED concentric-ring disc: nine full circles sharing one center, with ring LED counts 1, 8, 12, 16, 24, 32, 40, 48, 60 (sum 241).
///
/// Prior art: MoonLight's Rings241Layout, which composes MoonLight's RingLayout once per ring.
/// RingLayout places `n` LEDs evenly on a circle of radius n / (2π), starting at the bottom (angleRad = π at i=0) and stepping by 2π/n.
/// This port reproduces that exact per-LED math but emits coordinates only.
/// MoonLight's pin/wiring plumbing (doNextPin/nextPin, and RingLayout's angleFirst/rotation/clockwise/nrOfLEDs UI controls) has no place here, since a MoonLight layout hands positions to the driver and the driver owns pins.
/// The one geometry control that survives is `scale`, RingLayout's spacing multiplier.
/// Every ring is a full circle (MoonLight's rotation = 360), so every LED is emitted; that makes lightCount() the fixed constant 241.
///
/// ## The outside-in control is ours, not MoonLight's
///
/// `outside in` is ours, not MoonLight's: it names which end of the wire is light 0.
/// A disc is soldered either from the center LED outward (MoonLight's order, the default) or from the outer 60-LED ring inward.
/// A layout that only knew one of them would light the wrong ring for the other.
/// Only the ring SEQUENCE flips; the direction around each ring stays as wired.
///
/// Precision is reproduced statement-for-statement, because the disc's integer coordinates depend on it.
/// MoonLight forms `angleRad` from the double macros PI / TWO_PI, stores it in a *float*, then takes float sinf/cosf and multiplies a float radius.
/// Doing the angle wholly in float, or wholly in double, shifts a ring point landing exactly on an integer axis to the wrong side of the truncation.
/// Either way it differs from the source by one unit.
/// The faithful path is MoonLight's own: form the angle in double → narrow to float → float trig → float radius.
class Rings241Layout : public LayoutBase {
public:
    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }
    /// Scales both the ring radii and the shared center.
    uint8_t scale = 2;
    /// Clear puts light 0 at the center with rings outward, set reverses that.
    bool outside_in = false;
    /// Where light 0 of each ring sits, in degrees from the bottom.
    uint16_t angleFirst = 0;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("scale", scale, 1, 10);
        controls_.addControl("outside in", outside_in);
        controls_.addControl("angleFirst", angleFirst, 0, 359);
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        /// Fixed by construction: the nine ring sizes sum to 241 and no light is culled.
        nrOfLightsType total = 0;
        for (uint8_t n : kRingSizes) total += n;
        return total;  // 1+8+12+16+24+32+40+48+60 = 241
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        // The shared center, truncated to an integer as the source does, then scaled per light.
        const uint8_t leftMargin = static_cast<uint8_t>(1.1f * getRadius(60));

        nrOfLightsType idx = 0;
        // Rings run smallest first, or largest first when wired from the outside in.
        constexpr uint8_t kRings = sizeof(kRingSizes) / sizeof(kRingSizes[0]);
        for (uint8_t r = 0; r < kRings; r++) {
            const uint8_t n = kRingSizes[outside_in ? kRings - 1 - r : r];
            const float radius = getRadius(n);
            for (uint8_t i = 0; i < n; i++) {
                float x = static_cast<float>(scale * leftMargin);
                float y = static_cast<float>(scale * leftMargin);
                if (n != 1) {
                    // Formed in double and narrowed, so a zero start angle is bit-identical to the source.
                    const float angleRad = static_cast<float>(
                        kPi + (kTwoPi * static_cast<double>(i)) / static_cast<double>(n)
                            + (kTwoPi * static_cast<double>(angleFirst)) / 360.0);
                    x -= scale * std::sin(angleRad) * radius;
                    y += scale * std::cos(angleRad) * radius;
                }
                // Truncating each axis toward zero is what the source does, so preserve it.
                sink.pixel(idx++,
                   static_cast<lengthType>(static_cast<int>(x)),
                   static_cast<lengthType>(static_cast<int>(y)),
                   0);
            }
        }
    }

private:
    // The radius that spaces n lights one unit apart around a circle.
    static float getRadius(uint8_t n) { return static_cast<float>(n / kTwoPi); }

    static constexpr double kPi = std::numbers::pi;
    static constexpr double kTwoPi = 2.0 * kPi;

    // The nine ring sizes of the 241-LED disc, inner to outer.
    static constexpr uint8_t kRingSizes[9] = {1, 8, 12, 16, 24, 32, 40, 48, 60};
};

} // namespace mm
