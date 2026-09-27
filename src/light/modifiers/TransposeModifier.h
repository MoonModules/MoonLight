#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Modifier swapping a pair of box axes.
///
/// @moreinfo
///
/// Swaps a pair of axes of the logical box and every coordinate folded through it, then optionally inverts (flips) each axis.
/// Enable XY to swap the x and y axes (a matrix transpose, rows become columns), XZ or YZ likewise.
/// The swaps apply in that order, so enabling several composes them.
/// The inverse-X/Y/Z bools flip a coordinate within the (already-transposed) box: x → size.x - 1 - x, so the axis reads back-to-front.
/// A static, build-time remap, no per-frame work.
///
/// Prior art: MoonLight's Transpose modifier (M_MoonLight.h), same pairwise axis swap on both the box and the coordinate, plus per-axis inverse.
/// The inverse reads the box AFTER the swaps (MoonLight's modifyPosition reads the transposed `layer->size`), reproduced here by inverting against the stashed box.
/// Written fresh against our fold interface.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class TransposeModifier : public ModifierBase {
public:
    /// The catalog tags this modifier carries.
    const char* tags() const override { return "💫"; }  // MoonLight origin
    /// TransposeXY/XZ/YZ: it can swap any pair.
    Dim dimensions() const override { return Dim::D3; }

    /// Pairwise axis swaps. XY on by default (the common 2D transpose).
    bool transposeXY = true;
    /// Whether x and z swap.
    bool transposeXZ = false;
    /// Whether y and z swap.
    bool transposeYZ = false;
    /// Flip each axis within the transposed box (back-to-front).
    bool inverseX = false;
    /// Whether y is inverted after the swap.
    bool inverseY = false;
    /// Whether z is inverted after the swap.
    bool inverseZ = false;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("XY", transposeXY);
        controls_.addControl("XZ", transposeXZ);
        controls_.addControl("YZ", transposeYZ);
        controls_.addControl("inverse X", inverseX);
        controls_.addControl("inverse Y", inverseY);
        controls_.addControl("inverse Z", inverseZ);
    }

    /// Resize the logical box this modifier presents to the effect.
    void modifyLogicalSize(Coord3D& size) override {
        // The box swaps in the same order the coordinates do, so the two stay in step.
        if (transposeXY) { lengthType t = size.x; size.x = size.y; size.y = t; }
        if (transposeXZ) { lengthType t = size.x; size.x = size.z; size.z = t; }
        if (transposeYZ) { lengthType t = size.y; size.y = size.z; size.z = t; }
        // The transposed box is stashed, since the fold inverts against it.
        modifierSize_ = size;
    }

    /// Transform one light's logical position, false dropping it from the mapping.
    bool modifyLogical(Coord3D& pos) const override {
        if (transposeXY) { lengthType t = pos.x; pos.x = pos.y; pos.y = t; }
        if (transposeXZ) { lengthType t = pos.x; pos.x = pos.z; pos.z = t; }
        if (transposeYZ) { lengthType t = pos.y; pos.y = pos.z; pos.z = t; }

        if (inverseX) pos.x = static_cast<lengthType>(modifierSize_.x - pos.x - 1);
        if (inverseY) pos.y = static_cast<lengthType>(modifierSize_.y - pos.y - 1);
        if (inverseZ) pos.z = static_cast<lengthType>(modifierSize_.z - pos.z - 1);
        return true;  // Transpose never rejects a coordinate.
    }

private:
    /// Transposed box, stashed in modifyLogicalSize for the inverse fold.
    Coord3D modifierSize_;
};

} // namespace mm
