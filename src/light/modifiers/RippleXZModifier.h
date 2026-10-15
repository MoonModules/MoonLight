#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Collapses an axis to a single plane, so an effect drawn along the other axes ripples across the whole box.
/// @card RippleXZModifier.gif
///
/// @attribution troyhacks
///
/// @moreinfo
///
/// With `shrink` on, each axis chosen flattens to one slice and every coordinate on it folds to zero. The effect is painted once and shown on every slice of that axis.
class RippleXZModifier : public ModifierBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// Folds x or z into a ripple, so it needs the third axis.
    Dim dimensions() const override { return Dim::D3; }

    /// Collapse the chosen axes to a single plane.
    bool shrink = true;
    /// Collapse x, so each column shares one light.
    bool towardsX = true;
    /// Collapse z, so each x-y plane is shared across depth.
    bool towardsZ = false;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("shrink", shrink);
        controls_.addControl("towardsX", towardsX);
        controls_.addControl("towardsZ", towardsZ);
    }

    /// Resize the logical box this modifier presents to the effect.
    void modifyLogicalSize(Coord3D& size) override {
        // The same collapse the source applies, here on the running logical box.
        if (shrink) {
            if (towardsX) size.x = 1;
            if (towardsZ) size.z = 1;
        }
    }

    /// Transform one light's logical position, false dropping it from the mapping.
    bool modifyLogical(Coord3D& pos) const override {
        // A collapsed axis folds to zero, so every coordinate lands on the single slice.
        if (shrink) {
            if (towardsX) pos.x = 0;
            if (towardsZ) pos.z = 0;
        }
        return true;  // never rejects: a collapse maps every coord onto a slice
    }
};

} // namespace mm
