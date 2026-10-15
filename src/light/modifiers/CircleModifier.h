#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Expands a 1D effect into concentric circular rings: its position along the strip becomes the radius, so a gradient draws nested circles.
/// @card CircleModifier.gif
///
/// The round sibling of @ref BlockModifier, which draws square rings.
///
/// @attribution Euclidean distance
///
/// @technical
///
/// The logical box is one column of radius plus one cells, which an effect paints as rings.
class CircleModifier : public ModifierBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }

    /// Two-dimensional: it only ever produces a distance, so it works in the x and y plane.
    Dim dimensions() const override { return Dim::D2; }

    /// Resize the logical box this modifier presents to the effect.
    void modifyLogicalSize(Coord3D& size) override {
        // Stashed so the const per-light fold can read the center from it.
        modifierSize_ = size;

        // The box's far corner runs through the same fold, then every axis grows by one.
        Coord3D corner = size;
        fold(corner);
        size = corner;

        // Change the size to be one bigger in each dimension.
        size.x++;
        size.y++;
        size.z++;
    }

    /// Transform one light's logical position, false dropping it from the mapping.
    bool modifyLogical(Coord3D& pos) const override {
        fold(pos);
        return true; // Circle never rejects a coordinate.
    }

private:
    /// The box seen at build time, stashed for the fold.
    Coord3D modifierSize_;

    /// Fold a coordinate to its distance from the center, on the cold path so float is fine.
    void fold(Coord3D& position) const {
        // The offset from the center, by integer division on the box size.
        const int dx = position.x - modifierSize_.x / 2;
        const int dy = position.y - modifierSize_.y / 2;
        const int dz = position.z - modifierSize_.z / 2;

        // Euclidean distance from the center.
        const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy + dz * dz));

        position.x = 0;
        position.y = static_cast<lengthType>(distance);
        position.z = 0;
    }
};

} // namespace mm
