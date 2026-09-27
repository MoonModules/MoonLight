#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Modifier carving the layer to a percentage sub-rectangle.
///
/// @moreinfo
///
/// Carves the layer down to a sub-region of the physical bounding box: the effect renders only into the region, everything outside is dark.
/// The region is given as PERCENTAGES of the physical extent on each axis (start 0 / end 100 = the full box, an identity carve).
/// It survives a physical resize, a 0..50 region stays the left half whether the panel is 64 or 128 wide.
///
/// ## A crop, not a remap
///
/// It is a region crop, the textbook crop node of any compositor.
/// It shrinks the logical box to the region and folds each physical light into region-local space by subtracting the start offset.
/// A light landing outside the region is then rejected, which is what makes everything beyond it dark.
/// A 1:1 fold, same family as CheckerboardModifier.
///
/// ## The rounding rule
///
/// The interval is half-open: a start percentage floors to the lower pixel, and an end percentage ceils to an exclusive one.
/// That makes abutting regions tile exactly: a 0..50 and a 50..100 layer split a 128-wide axis into two halves with no overlap and no gap.
///
/// Off-screen windows. start/end percentages may go **negative or past 100** (Int16 on the wire, default UI range −100..200) to slide the window partly or fully out of the visible box.
/// The logical box is the full window span, so the effect renders at a consistent scale.
/// A physical light outside the window is dropped, and a window cell with none under it is simply dark.
/// A window entirely off-box renders nothing, the layer goes dark, which is how you move an effect completely out of view.
///
/// Fast path: the cheapest carve is *no modifier*, then Layer::rebuildLUT takes its identity/memcpy path with zero carving cost.
/// Adding a full-region (0/100) RegionModifier is correct but not free; the default is to not add one.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class RegionModifier : public ModifierBase {
public:
    /// The catalog tags this modifier carries.
    const char* tags() const override { return "💫"; }
    /// Clips a box, which is an extent on every axis.
    Dim dimensions() const override { return Dim::D3; }
    /// The region's low corner.
    lengthType startX = 0,   startY = 0,   startZ = 0;
    /// Its high corner.
    lengthType endX   = 100, endY   = 100, endZ   = 100;

    /// The controls a user sets on the card.
    void defineControls() override {
        // Int16 so negative / >100 percentages round-trip; the carve math clamps.
        controls_.addControl("startX", startX);
        controls_.addControl("startY", startY);
        controls_.addControl("startZ", startZ);
        controls_.addControl("endX",   endX);
        controls_.addControl("endY",   endY);
        controls_.addControl("endZ",   endZ);
    }

    /// Resize the logical box this modifier presents to the effect.
    void modifyLogicalSize(Coord3D& size) override {
        // Unclamped, so the window may sit partly or wholly outside the box.
        start_ = {floorPx(startX, size.x), floorPx(startY, size.y), floorPx(startZ, size.z)};
        const Coord3D end{ceilPx(endX, size.x), ceilPx(endY, size.y), ceilPx(endZ, size.z)};
        // Floored to one on a non-empty axis, so the effect always has a box to render into.
        region_ = {span(start_.x, end.x, size.x), span(start_.y, end.y, size.y),
                   span(start_.z, end.z, size.z)};
        size = region_;
    }

    /// Transform one light's logical position, false dropping it from the mapping.
    bool modifyLogical(Coord3D& pos) const override {
        // Folded into window-local space, and dropped when it lands outside the window.
        pos = pos - start_;
        return pos.x >= 0 && pos.x < region_.x &&
               pos.y >= 0 && pos.y < region_.y &&
               pos.z >= 0 && pos.z < region_.z;
    }

private:
    /// Window start pixel (may be negative), stashed for the fold.
    Coord3D start_;
    /// Window size (logical box), stashed for the bound check.
    Coord3D region_;

    // The lower edge, floored toward negative infinity so a negative percentage rounds consistently.
    static lengthType floorPx(lengthType pct, lengthType extent) {
        if (extent <= 0) return 0;
        long num = static_cast<long>(pct) * extent;
        long q = num / 100;
        if (num % 100 != 0 && num < 0) q -= 1;   // floor toward −∞ for negatives
        return static_cast<lengthType>(q);
    }

    // The upper edge, exclusive and ceiled, which may exceed the extent.
    static lengthType ceilPx(lengthType pct, lengthType extent) {
        if (extent <= 0) return 0;
        long num = static_cast<long>(pct) * extent;
        long q = num / 100;
        if (num % 100 != 0 && num > 0) q += 1;    // ceil toward +∞ for positives
        return static_cast<lengthType>(q);
    }

    // The span between the edges, floored to one on a non-empty axis.
    static lengthType span(lengthType startPx, lengthType endPx, lengthType extent) {
        if (extent <= 0) return 0;
        lengthType s = static_cast<lengthType>(endPx - startPx);
        return s >= 1 ? s : 1;
    }
};

} // namespace mm
