#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Modifier folding each box axis onto itself (mirror).
/// @card MirrorModifier.gif
///
/// @moreinfo
///
/// Folds the far half of the logical box back onto the near half per axis.
/// The image is mirrored across the box center.
/// Which axes are enabled decides whether a quadrant reflects into three, or an octant into seven.
/// Halving the box, rounding up, then folding each far-half coordinate back gives a many-to-one mapping.
/// The mirrored physical lights share one logical source, so the fan-out costs nothing under the fold.
///
/// ## How an axis folds
///
/// Per axis the logical extent halves, rounding up, so an odd extent keeps its center column unpaired.
/// A physical coordinate at or past that half-extent then reflects back onto the near half.
/// An even extent maps its far edge to the first logical column.
/// An odd one makes its unpaired center the last logical column, so the far edge maps to the second.
///
/// Prior art: MoonLight's Mirror modifier, with the same halving, the same reflection and the same per-axis flags.
/// Written fresh against our fold interface, the box stashed for the const fold as the base class documents.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class MirrorModifier : public ModifierBase {
public:
    /// The catalog tags this modifier carries.
    const char* tags() const override { return "💫"; }  // MoonLight origin
    /// MirrorX/Y/Z: each axis is independently foldable.
    Dim dimensions() const override { return Dim::D3; }

    /// Whether this axis mirrors, which is a no-op on an axis the layout does not use.
    bool mirrorX = true;
    /// Whether y is mirrored.
    bool mirrorY = true;
    /// Whether z is mirrored.
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
