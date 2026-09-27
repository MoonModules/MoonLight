#pragma once

#include "light/modifiers/ModifierBase.h"
#include <numbers>

namespace mm {

/// Modifier remapping the grid into radial petals.
///
/// @moreinfo
///
/// Polar remap: folds the logical box into `petals` angular wedges radiating from the box center, a pinwheel.
/// Each physical light's angle about the center picks a petal (x), and (in 2D+) its radius becomes the along-petal coordinate (y).
/// A 1D effect scrolls outward along every spoke and a 2D effect paints across the wheel.
/// The swirl shears the angle by radius, giving a spiral, and the symmetry divides the full turn into a repeating sub-arc.
/// The twist rotates the wheel per z slice, and the reverse flag flips the petal order.
///
/// ## Prior art
///
/// Prior art: MoonLight's Pinwheel modifier (M_MoonLight.h), same atan2 petal binning, hypot swirl shear, FACTORS[]-of-360 symmetry table, and per-slice zTwist.
/// Written fresh against our fold interface.
/// The source gates on the effect's own dimensionality, which a modifier here cannot see, so the question becomes whether the incoming box is two-dimensional.
/// Pins/wiring plumbing is dropped; this emits coordinates only.
/// Float math (atan2/hypot/sqrt) runs on the build path (modifyLogical is called at rebuild, not in the hot render loop).
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class PinwheelModifier : public ModifierBase {
public:
    /// The catalog tags this modifier carries.
    const char* tags() const override { return "💫"; }  // MoonLight origin
    /// Polar around a center in the x/y plane.
    Dim dimensions() const override { return Dim::D2; }

    /// How many petals the wheel is divided into.
    uint8_t petals = 60;
    /// How far a petal twists along its radius, a negative value reversing the swirl.
    int16_t swirl = 30;
    /// Whether the wheel turns the other way.
    bool reverse = false;
    /// How many times the pattern repeats around the turn.
    uint8_t symmetry = 1;
    /// How much the pattern advances per z layer.
    uint8_t zTwist = 0;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("petals", petals);
        controls_.addControl("swirl", swirl, -127, 127);
        controls_.addControl("reverse", reverse);
        controls_.addControl("symmetry", symmetry);
        controls_.addControl("zTwist", zTwist);
    }

    /// Resize the logical box this modifier presents to the effect.
    void modifyLogicalSize(Coord3D& size) override {
        // The original box and center are stashed before the reshape, since the fold reads them.
        modifierSize_ = size;
        middle_ = {static_cast<lengthType>(size.x / 2),
                   static_cast<lengthType>(size.y / 2),
                   static_cast<lengthType>(size.z / 2)};

        if (petals < 1) petals = 1;  // Ensure at least one petal (guards value %= petals)

        // A modifier cannot see the effect's dimension, so the incoming box decides.
        if (modifierSize_.y > 1) {
            // Adjust y before x (MoonLight order): furthest radius from center + 1.
            const int rx = std::max(size.x - middle_.x, static_cast<int>(middle_.x));
            const int ry = std::max(size.y - middle_.y, static_cast<int>(middle_.y));
            size.y = static_cast<lengthType>(std::sqrt(static_cast<float>(rx * rx + ry * ry)) + 1);
            size.x = static_cast<lengthType>(petals);
            size.z = 1;
        } else {
            size.x = 1;
            size.y = static_cast<lengthType>(petals);
            size.z = 1;
        }

        // The petal width: a full turn, or a factor of one, divided across the petals.
        const int FACTORS[23] = {360, 180, 120, 90, 72, 60, 45, 40, 36, 30, 24, 20,
                                 18, 15, 12, 10, 9, 8, 6, 5, 4, 3, 2};
        int factor;
        if (symmetry > 23)
            factor = 2;  // Default to 2 if symmetry is greater than 23
        else if (symmetry > 0)
            factor = FACTORS[symmetry - 1];  // Convert symmetry to a factor of 360
        else
            factor = 360;  // Default to 360 if symmetry is <= 0
        petalWidth_ = factor / static_cast<float>(petals);
    }

    /// Transform one light's logical position, false dropping it from the mapping.
    bool modifyLogical(Coord3D& pos) const override {
        // Polar coordinates relative to the (original box) center.
        const int dx = pos.x - middle_.x;
        const int dy = pos.y - middle_.y;

        // Swirl shears the angle by radius (only computed when swirl != 0).
        const int swirlFactor =
            swirl == 0 ? 0 : static_cast<int>(std::hypot(static_cast<float>(dy), static_cast<float>(dx)) * std::abs(swirl));

        // Angle 0..360 (atan2 returns -180..180, degrees() then +180 shifts it).
        int angle = static_cast<int>(degrees_(std::atan2(static_cast<float>(dy), static_cast<float>(dx)))) + 180;

        if (swirl < 0) angle = 360 - angle;  // Reverse swirl

        int value = angle + swirlFactor + (zTwist * pos.z);
        value = static_cast<int>(value / petalWidth_);
        value %= petals;

        if (reverse) value = petals - value - 1;  // Reverse movement

        // The petal index goes on whichever axis the reshape used: x in 2D, y in 1D.
        if (modifierSize_.y > 1) {
            pos.x = static_cast<lengthType>(value);
            pos.y = static_cast<lengthType>(std::sqrt(static_cast<float>(dx * dx + dy * dy)));
        } else {
            pos.x = 0;
            pos.y = static_cast<lengthType>(value);
        }
        pos.z = 0;
        return true;
    }

private:
    /// Original (pre-resize) box, stashed for the 2D+ test.
    Coord3D modifierSize_;
    /// Center of the original box (MoonLight's layer->middle).
    Coord3D middle_;
    /// Degrees per petal, from symmetry factor / petals.
    float petalWidth_ = 6.0f;

    // Radians to degrees, as a plain helper so the header carries no Arduino dependency.
    static float degrees_(float rad) { return rad * (180.0f / std::numbers::pi_v<float>); }
};

} // namespace mm
