#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Modifier that collapses one axis of the box to a single plane.
/// Author: @Troy (WLEDMM Art-Net), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
///
/// @moreinfo
///
/// Collapses one axis of the logical box to a single plane, so a lower-dimensional effect drawn on the collapsed axis maps identically onto every slice of the physical box.
/// With the shrink flag on, an enabled axis flattens to a single slice and every coordinate on it folds to zero.
/// A one-dimensional effect running along that axis is therefore painted once and broadcast across it.
/// `towardsZ` does the same for the Z extent (each XY plane shared across depth).
/// It is the static "collapse an axis" fold: the logical box loses the axis, and every coordinate on that axis folds to 0.
///
/// Prior art: MoonLight's RippleXZ modifier (M_MoonLight.h), same shrink/towardsX/ towardsZ axis collapse (`modifySize` sets the axis to 1, `modifyPosition` sets the coordinate to 0).
/// MoonLight additionally runs a per-frame buffer shift in tick() that propagates a ripple wave along the collapsed axis by reading and writing the layer's RGB pixels (setRGB/getRGB).
/// That is a render-buffer effect rather than a coordinate transform, so it has no place in the static fold this base class expresses.
/// A modifier emits coordinates only, so the wave is dropped and the axis collapse preserved exactly.
/// Defaults match MoonLight: shrink=true, towardsX=true, towardsZ=false.
class RippleXZModifier : public ModifierBase {
public:
    /// The catalog tags this modifier carries.
    const char* tags() const override { return "💫"; }  // MoonLight origin
    /// Folds x or z into a ripple, so it needs the third axis.
    Dim dimensions() const override { return Dim::D3; }

    /// Whether the box collapses, and on which axes; y is never collapsed.
    bool shrink = true;
    /// Whether the ripple runs along x.
    bool towardsX = true;
    /// Whether it runs along z.
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
        return true;  // never rejects — a collapse maps every coord onto a slice
    }
};

} // namespace mm
