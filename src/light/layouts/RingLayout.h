#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// Layout of a single ring of evenly-spaced LEDs.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
///
/// @moreinfo
///
/// A ring of lights: nrOfLEDs placed evenly around a circle, optionally a partial arc (rotation < 360, clockwise or counter-clockwise from angleFirst) and scaled out from the center.
/// Every light sits at an integer (x, y, 0).
/// The circle center is placed at ~1.1× the ring radius on both axes so the whole ring lands in the positive quadrant.
///
/// Prior art: MoonLight's RingLayout (MoonModules/projectMM, src light layout nodes).
/// The geometry is reproduced exactly: the radius, the placement angle, the center offset, the partial-arc filter and the integer truncation of every coordinate.
/// MoonLight's pin/wiring plumbing (doNextPin/nextPin) is dropped, a MoonLight layout emits coordinates only; the driver owns pins.
///
/// Float trig runs on the cold build path (placeLights / lightCount, called from a rebuild), never the hot render loop, so it's allowed here.
class RingLayout : public LayoutBase {
public:
    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }
    /// MoonLight defaults and ranges, preserved verbatim.
    uint8_t  nrOfLEDs   = 24;    // 1..255
    uint16_t angleFirst = 0;     // 0..359 — angle of the first LED (0 = top)
    /// The arc span in degrees, under 360 emitting a partial ring.
    uint16_t rotation   = 360;
    /// Which way the arc is walked.
    bool     clockwise  = true;
    /// The spacing multiplier out from the center, 1 to 10.
    uint8_t  scale      = 1;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("nrOfLEDs",    nrOfLEDs,   1, 255);
        controls_.addControl("angleFirst", angleFirst, 0, 359);
        controls_.addControl("rotation",   rotation,   0, 360);
        controls_.addControl("clockwise",    clockwise);
        controls_.addControl("scale",       scale,      1, 10);
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        /// The same predicate as the emit, so a partial arc's count cannot disagree.
        nrOfLightsType n = 0;
        walk([](void*, nrOfLightsType, lengthType, lengthType, lengthType) {}, nullptr, &n);
        return n;
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        walk(sink.cb, sink.ctx, nullptr);
    }

private:
    // MoonLight: getRadius(n) = n / TWO_PI.
    static float getRadius(uint8_t n) {
        return static_cast<float>(n) / (2.0f * std::numbers::pi_v<float>);
    }

    // The one home for the ring geometry, walking every candidate and filtering the arc.
    void walk(CoordCallback cb, void* ctx, nrOfLightsType* count) const {
        const float PI_F     = std::numbers::pi_v<float>;
        const float TWO_PI_F = 2.0f * PI_F;

        // A zero count, reachable by writing the control directly, emits nothing and divides by nothing.
        if (nrOfLEDs == 0) {
            if (count) *count = 0;
            return;
        }

        // Derived inline rather than cached, the integer truncation preserved deliberately.
        const lengthType ringCenterX = static_cast<lengthType>(1.1f * getRadius(nrOfLEDs));
        const lengthType ringCenterY = static_cast<lengthType>(1.1f * getRadius(nrOfLEDs));

        const float radius = getRadius(nrOfLEDs);

        nrOfLightsType idx = 0;
        for (int i = 0; i < nrOfLEDs; i++) {
            float x = scale * ringCenterX;
            float y = scale * ringCenterY;

            // Angle of this LED (for the partial-arc inclusion test).
            const float ledAngle =
                fmodf(angleFirst + (static_cast<float>(i) / nrOfLEDs) * 360.0f, 360.0f);

            // Placement angle for the actual position.
            const float angleRad =
                PI_F + (TWO_PI_F * i) / nrOfLEDs + TWO_PI_F * angleFirst / 360.0f;

            if (nrOfLEDs != 1) {
                x -= scale * sinf(angleRad) * radius;
                y += scale * cosf(angleRad) * radius;
            }

            /// The partial-arc test, integer throughout because the span is an integer.
            bool includeLED = false;
            if (rotation < 1 || rotation >= 360) {
                includeLED = true;  // full circle
            } else {
                float endAngle;
                if (clockwise) {
                    endAngle = fmodf(angleFirst + rotation, 360.0f);
                } else {
                    endAngle = fmodf(angleFirst - rotation + 360.0f, 360.0f);
                }

                if (clockwise) {
                    if (endAngle >= angleFirst) {
                        // No wrap: e.g. angleFirst=180, rotation=90, endAngle=270
                        includeLED = (ledAngle >= angleFirst && ledAngle <= endAngle);
                    } else {
                        // Wraps around 0: e.g. angleFirst=270, rotation=180, endAngle=90
                        includeLED = (ledAngle >= angleFirst || ledAngle <= endAngle);
                    }
                } else {
                    if (endAngle <= angleFirst) {
                        // No wrap: e.g. angleFirst=180, rotation=90, endAngle=90
                        includeLED = (ledAngle <= angleFirst && ledAngle >= endAngle);
                    } else {
                        // Wraps around 0: e.g. angleFirst=90, rotation=180, endAngle=270
                        includeLED = (ledAngle <= angleFirst || ledAngle >= endAngle);
                    }
                }
            }

            if (includeLED) {
                if (cb) {
                    cb(ctx, idx,
                       static_cast<lengthType>(static_cast<int>(x)),
                       static_cast<lengthType>(static_cast<int>(y)),
                       0);  // scale * ringCenter.z, and ringCenter.z is always 0
                }
                idx++;
            }
        }
        if (count) *count = idx;
    }
};

} // namespace mm
