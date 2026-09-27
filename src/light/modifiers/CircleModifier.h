#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Modifier expanding a 1D effect into concentric circular rings.
///
/// @moreinfo
///
/// Maps a 1D strip onto concentric circular rings: every physical light folds to its Euclidean distance from the box center, so a linear index becomes a radius.
/// The logical box collapses to a single column of `radius+1` cells (x=1, z=1), which an effect then paints as rings.
/// A static (build-time) remap, the fold is baked into the Layer's mapping LUT once per rebuild, nothing runs per frame.
///
/// The geometry is reproduced exactly: each axis offset from the box center is taken in integers, their Euclidean distance in float, and the coordinate becomes that distance alone.
///
/// The logical size is the incoming box run through the same fold, turning its far corner into a distance, then grown by one on every axis.
///
/// Prior art: MoonLight's Circle modifier (M_MoonLight.h), same center-offset + Euclidean-distance fold and the same +1-per-axis size bump.
/// MoonLight tags it 💎; the 💫 here marks the MoonLight origin per MoonLight convention.
/// Written fresh against our ModifierBase fold interface (modifyLogicalSize / modifyLogical) rather than MoonLight's modifySize / modifyPosition Node API.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class CircleModifier : public ModifierBase {
public:
    /// The catalog tags this modifier carries.
    const char* tags() const override { return "💫"; } // MoonLight origin

    /// Two-dimensional: it only ever produces a distance, so it works in the x and y plane.
    Dim dimensions() const override { return Dim::D2; }

    // No controls: the center is always the box middle and the distance always Euclidean.

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

    // Fold a coordinate to its distance from the center, on the cold path so float is fine.
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
