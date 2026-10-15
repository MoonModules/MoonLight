#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Folds the far half of the box back onto the near half, mirroring the image across its center on each axis you choose.
/// @card MirrorModifier.gif
///
/// In 2D one quadrant shows in all four, in 3D one octant in all eight.
///
/// @moreinfo
///
/// Per axis the logical extent halves, rounding up, so an odd extent keeps its center column unpaired. A coordinate at or past that half reflects back, and the mirrored lights share one logical source, so the fan-out costs nothing.
///
/// An even extent maps its far edge to the first logical column. An odd one makes its unpaired center the last logical column, so the far edge maps to the second.
class MirrorModifier : public ModifierBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// MirrorX/Y/Z: each axis is independently foldable.
    Dim dimensions() const override { return Dim::D3; }

    /// Mirror across the center along x.
    bool mirrorX = true;
    /// Mirror across the center along y.
    bool mirrorY = true;
    /// Mirror across the center along z.
    bool mirrorZ = true;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("mirrorX", mirrorX);
        controls_.addControl("mirrorY", mirrorY);
        controls_.addControl("mirrorZ", mirrorZ);
    }

    /// Resize the logical box this modifier presents to the effect.
    void modifyLogicalSize(Coord3D& size) override {
        // Each mirrored axis halves, rounding up so an odd extent keeps its center column.
        if (mirrorX) size.x = static_cast<lengthType>((size.x + 1) / 2);
        if (mirrorY) size.y = static_cast<lengthType>((size.y + 1) / 2);
        if (mirrorZ) size.z = static_cast<lengthType>((size.z + 1) / 2);
        half_ = size;
    }

    /// Transform one light's logical position, false dropping it from the mapping.
    bool modifyLogical(Coord3D& pos) const override {
        // A coordinate in the far half reflects back; one already near passes through.
        if (mirrorX && pos.x >= half_.x) pos.x = static_cast<lengthType>(half_.x * 2 - 1 - pos.x);
        if (mirrorY && pos.y >= half_.y) pos.y = static_cast<lengthType>(half_.y * 2 - 1 - pos.y);
        if (mirrorZ && pos.z >= half_.z) pos.z = static_cast<lengthType>(half_.z * 2 - 1 - pos.z);
        return true;
    }

private:
    /// Halved logical box, stashed in modifyLogicalSize for the fold.
    Coord3D half_;
};

} // namespace mm
