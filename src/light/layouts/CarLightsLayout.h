#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// Layout mapping automotive light-strip coordinates.
/// @card CarLightsLayout.gif
/// Author: Eric Marciniak (Discord), custom car-lights fixture, reconstructed for MoonLight
///
/// @moreinfo
///
/// A stylised pair of car headlights: four concentric-ring "lamps" (inner/outer left, inner/outer right) plus two side strips joined by 90° arcs, all scaled by a single `scale` control.
/// A 2D layout emitting each LED's (x, y, 0) in physical wiring order.
///
/// Prior art: MoonLight's CarLightsLayout (Node "Car Lights", tags 🚥; MoonModules/projectMM, src light layout nodes).
/// MoonLight builds this by instantiating a RingLayout object and calling its onLayout() repeatedly with different ringCenter / nrOfLEDs / angle settings.
/// Every ring and both strips' coordinates are reproduced here EXACTLY, in the same wiring order. tags 💫 marks the MoonLight lineage.
///
/// ## What had to be reconstructed
///
/// RECONSTRUCTED: MoonLight's RingLayout is a standalone module, it has no onLayout()/addLight() and derives its ring center INTERNALLY from nrOfLEDs.
/// It cannot be driven the way MoonLight drives its RingLayout (which takes an EXTERNALLY set ringCenter per headlight).
/// The ring-emitting trig is therefore inlined below, reproducing the source's own ring placement verbatim.
/// That covers its radius, its placement angle, its partial-arc inclusion filter and its integer truncation of each coordinate.
/// The external-ringCenter form is the one CarLights needs, which is why we don't delegate to RingLayout.h.
///
/// Float trig runs on the cold build path (placeLights / lightCount, called from a rebuild), never the hot render loop, so it's allowed here.
/// MoonLight's pin/wiring plumbing (nextPin / doNextPin) is dropped, a MoonLight layout emits coordinates only; the driver owns pins.
class CarLightsLayout : public LayoutBase {
public:
    // MoonLight's default and range, its inactive spoke controls dropped.
    /// The spacing multiplier out from each center, 1 to 10.
    uint8_t scale = 2;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("scale", scale, 1, 10);
    }

    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D2; }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        /// The same walk as the emit, so a partial arc's count can never disagree.
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

    // Threaded through the build so each ring and strip appends after the previous one.
    struct Emit {
        CoordCallback cb;
        void* ctx;
        nrOfLightsType idx;
        // Truncating to int is what matches MoonLight's own coordinate rounding.
        void add(float x, float y) {
            if (cb) cb(ctx, idx,
                       static_cast<lengthType>(static_cast<int>(x)),
                       static_cast<lengthType>(static_cast<int>(y)),
                       0);
            idx++;
        }
        void add(lengthType x, lengthType y) {
            if (cb) cb(ctx, idx, x, y, 0);
            idx++;
        }
    };

    // One ring's included lights, in ring order, about an externally supplied center.
    void emitRing(Emit& e, lengthType cx, lengthType cy, uint8_t nrOfLEDs,
                  uint16_t angleFirst, uint16_t rotation, bool clockwise) const {
        if (nrOfLEDs == 0) return;

        const float PI_F     = std::numbers::pi_v<float>;
        const float TWO_PI_F = 2.0f * PI_F;
        const float radius   = getRadius(nrOfLEDs);

        for (int i = 0; i < nrOfLEDs; i++) {
            float x = scale * static_cast<float>(cx);
            float y = scale * static_cast<float>(cy);

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

            /// Partial-circle inclusion test (MoonLight, verbatim logic).
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
                        // No wrap: angleFirst=180, rotation=90, endAngle=270
                        includeLED = (ledAngle >= angleFirst && ledAngle <= endAngle);
                    } else {
                        // Wraps around 0: angleFirst=270, rotation=180, endAngle=90
                        includeLED = (ledAngle >= angleFirst || ledAngle <= endAngle);
                    }
                } else {
                    if (endAngle <= angleFirst) {
                        // No wrap: angleFirst=180, rotation=90, endAngle=90
                        includeLED = (ledAngle <= angleFirst && ledAngle >= endAngle);
                    } else {
                        // Wraps around 0: angleFirst=90, rotation=180, endAngle=270
                        includeLED = (ledAngle <= angleFirst || ledAngle >= endAngle);
                    }
                }
            }

            if (includeLED) e.add(x, y);
        }
    }

    // The one home for the geometry: every ring and both strips, in wiring order.
    void walk(CoordCallback cb, void* ctx, nrOfLightsType* count) const {
        Emit e{cb, ctx, 0};

        const lengthType leftMargin = 9;  // MoonLight: uint8_t leftMargin = 9;

        // --- Headlights: angleFirst=90, rotation=360, clockwise (defaults) ---
        constexpr uint16_t kFirst = 90;
        constexpr uint16_t kFull  = 360;
        constexpr bool     kCW    = true;

        // inner light left, center {leftMargin + 11, 8}
        emitRing(e, leftMargin + 11, 8,  1, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 11, 8,  8, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 11, 8, 12, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 11, 8, 16, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 11, 8, 24, kFirst, kFull, kCW);

        // outer light left, center {leftMargin, 6}
        emitRing(e, leftMargin, 6,  1, kFirst, kFull, kCW);
        emitRing(e, leftMargin, 6,  8, kFirst, kFull, kCW);
        emitRing(e, leftMargin, 6, 12, kFirst, kFull, kCW);
        emitRing(e, leftMargin, 6, 16, kFirst, kFull, kCW);
        emitRing(e, leftMargin, 6, 24, kFirst, kFull, kCW);
        emitRing(e, leftMargin, 6, 32, kFirst, kFull, kCW);

        // (MoonLight nextPin() here — dropped)

        // inner light right, center {leftMargin + 25, 8}
        emitRing(e, leftMargin + 25, 8,  1, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 25, 8,  8, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 25, 8, 12, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 25, 8, 16, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 25, 8, 24, kFirst, kFull, kCW);

        // outer light right, center {leftMargin + 36, 6}
        emitRing(e, leftMargin + 36, 6,  1, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 36, 6,  8, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 36, 6, 12, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 36, 6, 16, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 36, 6, 24, kFirst, kFull, kCW);
        emitRing(e, leftMargin + 36, 6, 32, kFirst, kFull, kCW);

        // (MoonLight nextPin() here — dropped)

        // The left strip, walked from its high-x end back.
        for (int x = (leftMargin + 16) * scale; x >= leftMargin * scale; x--) {
            e.add(static_cast<lengthType>(x), static_cast<lengthType>(15 * scale));
        }
        // 52-LED arc, center {leftMargin, 6}, angleFirst=180, rotation=90, clockwise
        emitRing(e, leftMargin, 6, 52, 180, 90, true);
        // for (y = 5; y >= 1; y--) addLight({0, y*scale})
        for (int y = 5; y >= 1; y--) {
            e.add(static_cast<lengthType>(0), static_cast<lengthType>(y * scale));
        }

        // (MoonLight nextPin() here — dropped)

        // The right strip, walked outward.
        for (int x = (leftMargin + 19) * scale; x <= (leftMargin + 35) * scale; x++) {
            e.add(static_cast<lengthType>(x), static_cast<lengthType>(15 * scale));
        }
        // 52-LED arc, center {leftMargin + 36, 6}, angleFirst=180, rotation=90, counter-clockwise
        emitRing(e, leftMargin + 36, 6, 52, 180, 90, false);
        // for (y = 5; y >= 1; y--) addLight({(leftMargin+44)*scale, y*scale})
        for (int y = 5; y >= 1; y--) {
            e.add(static_cast<lengthType>((leftMargin + 44) * scale),
                  static_cast<lengthType>(y * scale));
        }

        // (MoonLight nextPin() here — dropped)

        if (count) *count = e.idx;
    }
};

} // namespace mm
