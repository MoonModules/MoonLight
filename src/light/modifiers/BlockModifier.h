#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Modifier expanding a 1D effect into concentric square rings.
///
/// @moreinfo
///
/// Remaps a 1D effect onto the CHEBYSHEV-distance field of a 2D box.
/// Each physical light folds to its block distance from the box center, max(|dx|,|dy|), so the effect draws as concentric SQUARE rings expanding from the middle.
/// It is the square-ring sibling of the Circle modifier (which uses the Euclidean sqrt and draws round rings).
/// Block swaps that for the Chebyshev max, which is why the rings are axis-aligned squares.
/// Z is not part of the distance (this is a 2D remap, exactly as MoonLight defines it).
///
/// The logical box the fold produces is one row wide and (max block distance + 1) tall: a 1D effect painted along y lights the rings from center outward.
///
/// Prior art: MoonLight's BlockModifier (M_MoonLight.h), same center formula ((n+1)/2 - 1, a floor-biased middle), same dx/dy abs deltas, same distance = max(dx, dy), same {0, distance, 0} output.
/// The same box transform (fold the box through the position map, then grow each axis by one).
/// Written fresh against our fold interface: modifySize() -> modifyLogicalSize (stashing the incoming box), modifyPosition() -> the const modifyLogical fold that reads the stash.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class BlockModifier : public ModifierBase {
public:
    /// The catalog tags this modifier carries.
    const char* tags() const override { return "💫"; }  // MoonLight origin

    /// Two-dimensional: it reads x and y and writes into y, leaving z untouched.
    Dim dimensions() const override { return Dim::D2; }

    /// Resize the logical box this modifier presents to the effect.
    void modifyLogicalSize(Coord3D& size) override {
        // Stashed so the const per-light fold can read the center from it.
        modifierSize_ = size;

        // The box folds into block-distance space, inlined because the fold is const.
        const int centerX = (modifierSize_.x + 1) / 2 - 1;
        const int centerY = (modifierSize_.y + 1) / 2 - 1;
        const int dx = std::abs(static_cast<int>(size.x) - centerX);
        const int dy = std::abs(static_cast<int>(size.y) - centerY);
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
    // The physical box, stashed at resize time so the const fold can read its center.
    Coord3D modifierSize_;
};

} // namespace mm
