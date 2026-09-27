#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Modifier tiling the image across the box, optionally mirrored.
/// @card MultiplyModifier.png
///
/// @moreinfo
///
/// Tiles the logical image across the physical box `multiply` times per axis, optionally mirroring alternate tiles.
/// The logical box is the physical box divided by the per-axis multiplier.
/// Under the fold build the fan-out is free: every physical light folds (`pos % logicalSize`) onto its logical cell.
/// The N physical lights of N tiles all land on the same logical light, N:1 emerges, no fan-out list.
/// With multiply 2 + mirror on, an axis folds in half, the classic kaleidoscope mirror (this subsumes a standalone Mirror: it's just multiply 2 + mirror true).
///
/// Prior art: MoonLight's Multiply modifier (M_MoonLight.h), same tile+mirror fold (`position % modifierSize`, odd tiles reflected).
/// We expose per-axis mirror bools (3) instead of MoonLight's single mirror flag, and per-axis multipliers, so X/Y/Z can fold and tile independently.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class MultiplyModifier : public ModifierBase {
public:
    /// The catalog tags this modifier carries.
    const char* tags() const override { return "💫"; }  // MoonLight origin
    /// Tiles on all three axes; on a 2D grid the z factor is simply 1.
    Dim dimensions() const override { return Dim::D3; }

    /// How many times the box repeats on this axis, one meaning no multiplication.
    uint8_t multiplyX = 2;
    /// Along y.
    uint8_t multiplyY = 2;
    /// Along z.
    uint8_t multiplyZ = 2;
    /// Whether alternate tiles reflect on this axis, giving a kaleidoscope.
    bool mirrorX = true;
    /// Along y.
    bool mirrorY = true;
    /// Along z.
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

    // The control clamped to the axis extent, so it never divides by zero or blanks a layer.
    static lengthType eff(uint8_t mult, lengthType extent) {
        lengthType m = mult ? mult : 1;
        if (extent > 0 && m > extent) m = extent;
        return m;
    }

    // Fold a coordinate into one tile, reflecting the odd ones when mirroring.
    static lengthType foldAxis(lengthType p, lengthType logical, bool mirror) {
        if (logical <= 0) return p;
        const lengthType tile   = static_cast<lengthType>(p / logical);
        const lengthType within = static_cast<lengthType>(p % logical);
        return (mirror && (tile & 1)) ? static_cast<lengthType>(logical - 1 - within) : within;
    }
};

} // namespace mm
