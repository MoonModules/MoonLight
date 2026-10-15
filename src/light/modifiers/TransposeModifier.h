#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Swaps a pair of axes and can flip each one, so the image rotates or mirrors without the effect redrawing.
/// @card TransposeModifier.gif
///
/// @attribution Transpose
///
/// @moreinfo
///
/// The swaps apply to the box and to every coordinate in the order XY, XZ, YZ, so enabling several composes them; XY alone is the matrix transpose. The flips run after the swaps, against the already transposed box.
class TransposeModifier : public ModifierBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// TransposeXY/XZ/YZ: it can swap any pair.
    Dim dimensions() const override { return Dim::D3; }

    /// Swap x and y, rows becoming columns.
    bool transposeXY = true;
    /// Swap x and z.
    bool transposeXZ = false;
    /// Swap y and z.
    bool transposeYZ = false;
    /// Flip x after the swaps, so it reads back to front.
    bool inverseX = false;
    /// Flip y after the swaps.
    bool inverseY = false;
    /// Flip z after the swaps.
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
