#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Modifier masking the layer in a checkerboard pattern.
/// Author: WildCats08 / @Brandon502 (MoonLight), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
/// @card CheckerboardModifier.gif
///
/// @moreinfo
///
/// Masks the layer in a checkerboard pattern: lights in the "off" squares are dropped (the physical light maps nowhere), lights in the "on" squares pass through unchanged.
/// `size` sets the square edge in lights; `invert` flips which squares are on.
/// A mask, not a remap, the logical box is unchanged.
///
/// Prior art: MoonLight's Checkerboard modifier (M_MoonLight.h) drops lights by setting position to a sentinel; our fold returns false from modifyLogical.
class CheckerboardModifier : public ModifierBase {
public:
    /// The catalog tags this modifier carries.
    const char* tags() const override { return "💫"; }  // MoonLight origin
    /// Walks x, y and z, so it patterns a volume as readily as a panel.
    Dim dimensions() const override { return Dim::D3; }

    /// Checker square edge, in lights (≥1).
    uint8_t size = 2;
    /// Flip which squares pass through.
    bool invert = false;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("size", size, 1, 64);
        controls_.addControl("invert", invert);
    }

    // A mask leaves the logical box unchanged (no modifyLogicalSize override).

    /// Transform one light's logical position, false dropping it from the mapping.
    bool modifyLogical(Coord3D& pos) const override {
        const lengthType s = size ? size : 1;
        // The parity of the square this light sits in, even passing through by default.
        const int parity = ((pos.x / s) + (pos.y / s) + (pos.z / s)) & 1;
        return parity == (invert ? 1 : 0);   // false → dropped (the mask)
    }
};

} // namespace mm
