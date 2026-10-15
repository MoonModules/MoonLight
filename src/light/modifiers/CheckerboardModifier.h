#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Masks the layer in a checkerboard: the lights on the "off" squares stay dark, the rest show the effect unchanged.
/// @card CheckerboardModifier.gif
///
/// @attribution Brandon502
class CheckerboardModifier : public ModifierBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// Walks x, y and z, so it patterns a volume as readily as a panel.
    Dim dimensions() const override { return Dim::D3; }

    /// The edge of a checker square, in lights.
    uint8_t size = 2;
    /// Swap which squares show the effect and which stay dark.
    bool invert = false;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("size", size, 1, 64);
        controls_.addControl("invert", invert);
    }

    /// A mask: a light on a square of the other parity is dropped, and the logical box stays as it is.
    bool modifyLogical(Coord3D& pos) const override {
        const lengthType s = size ? size : 1;
        // The parity of the square this light sits in, even passing through by default.
        const int parity = ((pos.x / s) + (pos.y / s) + (pos.z / s)) & 1;
        return parity == (invert ? 1 : 0);   // false → dropped (the mask)
    }
};

} // namespace mm
