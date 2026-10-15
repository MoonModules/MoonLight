#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Tiles the image across the box, a number of times per axis, and can mirror every other tile into a kaleidoscope.
/// @card MultiplyModifier.gif
///
/// Two tiles with mirroring on is a plain mirror.
///
/// @moreinfo
///
/// The logical box is the physical box divided by each axis's factor. Every physical light folds onto its logical cell with a modulo, so the lights of all the tiles share one source and the fan-out is free.
///
/// Mirroring is per axis and the factors are too, where MoonLight's original has one mirror flag; light past the last whole tile is dropped.
class MultiplyModifier : public ModifierBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// Tiles on all three axes; on a 2D grid the z factor is simply 1.
    Dim dimensions() const override { return Dim::D3; }

    /// Tiles along x, one meaning none.
    uint8_t multiplyX = 2;
    /// Tiles along y, one meaning none.
    uint8_t multiplyY = 2;
    /// Tiles along z, one meaning none.
    uint8_t multiplyZ = 2;
    /// Reflect every other tile along x.
    bool mirrorX = true;
    /// Reflect every other tile along y.
    bool mirrorY = true;
    /// Reflect every other tile along z.
    bool mirrorZ = true;

    /// The controls a user sets on the card.
    void defineControls() override {
        // More tiles than the axis has pixels simply yields single-pixel tiles.
        controls_.addControl("multiplyX", multiplyX, 1, 64);
        controls_.addControl("multiplyY", multiplyY, 1, 64);
        controls_.addControl("multiplyZ", multiplyZ, 1, 64);
        controls_.addControl("mirrorX", mirrorX);
        controls_.addControl("mirrorY", mirrorY);
        controls_.addControl("mirrorZ", mirrorZ);
    }

    /// Resize the logical box this modifier presents to the effect.
    void modifyLogicalSize(Coord3D& size) override {
        // The box divided by the effective multiplier, the uncovered high edge recorded.
        const lengthType mX = eff(multiplyX, size.x), mY = eff(multiplyY, size.y), mZ = eff(multiplyZ, size.z);
        size.x /= mX; size.y /= mY; size.z /= mZ;
        tile_ = size;
        covered_ = {static_cast<lengthType>(size.x * mX), static_cast<lengthType>(size.y * mY),
                    static_cast<lengthType>(size.z * mZ)};
    }

    /// Transform one light's logical position, false dropping it from the mapping.
    bool modifyLogical(Coord3D& pos) const override {
        // A coordinate in the leftover edge strip has no tile, so drop rather than wrap it.
        if ((covered_.x > 0 && pos.x >= covered_.x) ||
            (covered_.y > 0 && pos.y >= covered_.y) ||
            (covered_.z > 0 && pos.z >= covered_.z)) return false;
        // The tile index decides whether to reflect, then the coordinate wraps into it.
        pos.x = foldAxis(pos.x, tile_.x, mirrorX);
        pos.y = foldAxis(pos.y, tile_.y, mirrorY);
        pos.z = foldAxis(pos.z, tile_.z, mirrorZ);
        return true;
    }

private:
    /// Output tile size, stashed in modifyLogicalSize for the fold.
    Coord3D tile_;
    /// Pixels the tiles actually cover (tile*mult); the leftover edge is dropped.
    Coord3D covered_;

    /// The control clamped to the axis extent, so it never divides by zero or blanks a layer.
    static lengthType eff(uint8_t mult, lengthType extent) {
        lengthType m = mult ? mult : 1;
        if (extent > 0 && m > extent) m = extent;
        return m;
    }

    /// Fold a coordinate into one tile, reflecting the odd ones when mirroring.
    static lengthType foldAxis(lengthType p, lengthType logical, bool mirror) {
        if (logical <= 0) return p;
        const lengthType tile   = static_cast<lengthType>(p / logical);
        const lengthType within = static_cast<lengthType>(p % logical);
        return (mirror && (tile & 1)) ? static_cast<lengthType>(logical - 1 - within) : within;
    }
};

} // namespace mm
