#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Crops the layer to a rectangle given in percentages of each axis, so it holds when the layout is resized; outside it is dark.
/// @card RegionModifier.gif
///
/// Beyond 0 and 100 the window slides partly or wholly out of view.
///
/// @technical
///
/// The crop node of any compositor: the logical box shrinks to the window, and each light folds into window space by subtracting the start. A light outside the window is dropped.
///
/// The interval is half-open, a start flooring to the lower light and an end ceiling to an exclusive one. So 0 to 50 and 50 to 100 split a 128-wide axis into two halves with no overlap and no gap.
///
/// A whole-box region is correct but not free: with no modifier the Layer copies the layer straight, so add one only to crop.
class RegionModifier : public ModifierBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// Clips a box, which is an extent on every axis.
    Dim dimensions() const override { return Dim::D3; }
    /// Where the region starts along x, in percent of the axis.
    lengthType startX = 0;
    /// Where it starts along y, in percent.
    lengthType startY = 0;
    /// Where it starts along z, in percent.
    lengthType startZ = 0;
    /// Where the region ends along x, in percent of the axis.
    lengthType endX = 100;
    /// Where it ends along y, in percent.
    lengthType endY = 100;
    /// Where it ends along z, in percent.
    lengthType endZ = 100;

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

    /// The lower edge, floored toward negative infinity so a negative percentage rounds consistently.
    static lengthType floorPx(lengthType pct, lengthType extent) {
        if (extent <= 0) return 0;
        long num = static_cast<long>(pct) * extent;
        long q = num / 100;
        if (num % 100 != 0 && num < 0) q -= 1;   // floor toward −∞ for negatives
        return static_cast<lengthType>(q);
    }

    /// The upper edge, exclusive and ceiled, which may exceed the extent.
    static lengthType ceilPx(lengthType pct, lengthType extent) {
        if (extent <= 0) return 0;
        long num = static_cast<long>(pct) * extent;
        long q = num / 100;
        if (num % 100 != 0 && num > 0) q += 1;    // ceil toward +∞ for positives
        return static_cast<lengthType>(q);
    }

    /// The span between the edges, floored to one on a non-empty axis.
    static lengthType span(lengthType startPx, lengthType endPx, lengthType extent) {
        if (extent <= 0) return 0;
        lengthType s = static_cast<lengthType>(endPx - startPx);
        return s >= 1 ? s : 1;
    }
};

} // namespace mm
