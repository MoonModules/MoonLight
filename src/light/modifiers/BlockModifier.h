#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Expands a 1D effect into concentric square rings: its position along the strip becomes the ring, so a gradient draws nested squares.
/// @card BlockModifier.gif
///
/// The square sibling of @ref CircleModifier, which draws round rings.
///
/// @attribution Chebyshev distance
///
/// @moreinfo
///
/// Each light folds to its Chebyshev distance from the box center, max(|dx|, |dy|), which is why the rings are axis-aligned squares; z is not part of the distance.
///
/// The logical box is one row wide and the largest distance plus one tall, so a 1D effect painted along y lights the rings from the center outward. The center is (n+1)/2 - 1 per axis, a floor-biased middle.
class BlockModifier : public ModifierBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }

    /// Two-dimensional: it reads x and y and writes into y, leaving z untouched.
    Dim dimensions() const override { return Dim::D2; }

    /// Resize the logical box this modifier presents to the effect.
    void modifyLogicalSize(Coord3D& size) override {
        // Stashed so the const per-light fold can read the center from it.
        modifierSize_ = size;

        // The farthest light folds the box, the last index rather than one past it, or an empty ring is added.
        const int centerX = (modifierSize_.x + 1) / 2 - 1;
        const int centerY = (modifierSize_.y + 1) / 2 - 1;
        const int dx = std::abs(static_cast<int>(size.x) - 1 - centerX);
        const int dy = std::abs(static_cast<int>(size.y) - 1 - centerY);
        const int distance = std::max(dx, dy);

        size.x = 0;
        size.y = static_cast<lengthType>(distance);
        size.z = 0;

        // Each axis grows by one, so the outermost ring index is inclusive.
        size.x++;
        size.y++;
        size.z++;
    }

    /// Transform one light's logical position, false dropping it from the mapping.
    bool modifyLogical(Coord3D& pos) const override {
        // Chebyshev distance from the center, z playing no part: the rings are square.
        const int centerX = (modifierSize_.x + 1) / 2 - 1;
        const int centerY = (modifierSize_.y + 1) / 2 - 1;

        const int dx = std::abs(static_cast<int>(pos.x) - centerX);
        const int dy = std::abs(static_cast<int>(pos.y) - centerY);

        // Block distance is the maximum of the two deltas (creates square rings).
        const int distance = std::max(dx, dy);

        pos.x = 0;
        pos.y = static_cast<lengthType>(distance);
        pos.z = 0;
        return true; // never rejects a coord
    }

private:
    /// The physical box, stashed at resize time so the const fold can read its center.
    Coord3D modifierSize_;
};

} // namespace mm
