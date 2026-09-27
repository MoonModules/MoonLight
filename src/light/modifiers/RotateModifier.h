#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Modifier rotating the 2D image about its center over time.
/// Author: WildCats08 / @Brandon502 (MoonLight), https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
///
/// @moreinfo
///
/// Rotates the 2D image around its center, turning continuously over time.
/// The one DYNAMIC modifier in the set: it overrides modifyLive(), so the Layer re-applies it every frame (a smooth turn, not a stepped LUT rebuild).
/// A static-only chain pays nothing, the per-frame pass runs only because this modifier reports hasModifyLive().
///
/// ## The live pass maps backward
///
/// For each destination logical cell it computes the source cell to gather from.
/// No destination is ever left unfilled, which is the textbook reason image warping samples backward.
/// The source is the inverse rotation R(-θ) of the destination.
///
/// ## The transform-matrix reference
///
/// This modifier is also the codebase's reference for a matrix-backed transform.
/// Rotation is the canonical affine transform, unlike the non-affine folds that mask or repeat.
/// It is written as an explicit rotation matrix applied to the centerd coordinate.
/// The matrix entries are integer fixed point, the trig table's output centerd so subtracting its midpoint gives a signed unit component.
/// A future affine transform combining translation, scale, rotation and shear would compose its matrix the same way, so the fold interface hosts one with no change.
/// A non-affine modifier cannot use a matrix at all, a mask being a predicate and a tile a modulo, which is why only this one is matrix-shaped.
///
/// Prior art: MoonLight M_MoonLight.h Rotate (modifyXYZ per-frame transform).
/// Same per-frame coordinate remap; we name the hook modifyLive and carry an explicit matrix.
class RotateModifier : public ModifierBase {
public:
    /// The catalog tags this modifier carries.
    const char* tags() const override { return "💫"; }
    /// Which axes this modifier works on, advisory for the UI chip.
    Dim dimensions() const override { return Dim::D2; }   // 2D rotation (advisory chip)
    /// Whether this modifier also transforms per frame.
    bool hasModifyLive() const override { return true; }  // animates every frame

    /// Rotation speed, 1..255 (turns faster as it rises).
    uint8_t speed = 1;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("speed", speed, 1, 255);
    }

    // The rotation is applied live rather than baked in, so a speed edit needs no rebuild.
    /// Whether a change here forces the mapping to rebuild.
    bool affectsPrepare(const char* /*controlName*/) const override { return false; }

    // A backward map: each destination is replaced by the source cell it samples.
    /// Transform a position per frame, after the mapping is built.
    void modifyLive(Coord3D& pos, const Coord3D& logical) const override {
        // Center in half-units (×2) so an even-width box rotates about its true center.
        const int32_t cx2 = logical.x - 1;                       // 2·centerX
        const int32_t cy2 = logical.y - 1;                       // 2·centerY
        const int32_t dx2 = 2 * static_cast<int32_t>(pos.x) - cx2;   // 2·(x − center)
        const int32_t dy2 = 2 * static_cast<int32_t>(pos.y) - cy2;

        // The inverse rotation, in signed fixed point about the box center.
        const int32_t c = static_cast<int32_t>(cos8(angle_)) - 128;
        const int32_t s = static_cast<int32_t>(sin8(angle_)) - 128;
        const int32_t sx2 = ( dx2 * c + dy2 * s) >> 7;           // row 0 of the matrix · dest
        const int32_t sy2 = (-dx2 * s + dy2 * c) >> 7;           // row 1 of the matrix · dest

        // Undo the ×2 and center shift, rounding to nearest.
        pos.x = static_cast<lengthType>((sx2 + cx2 + 1) >> 1);
        pos.y = static_cast<lengthType>((sy2 + cy2 + 1) >> 1);
        // z passes through (2D rotation).
    }

    // The angle advances on the timer, applied next frame without any rebuild.
    void tick() MM_NONBLOCKING override {
        const uint32_t now = platform::millis();
        if (lastElapsed_ == 0) lastElapsed_ = now;
        const uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        phaseNum_ += static_cast<uint64_t>(dt) * speed;
        angle_ = static_cast<uint8_t>(phaseNum_ >> 6);   // one turn unit per 64 accumulator units
    }

private:
    /// Current rotation angle, uint8 turn units (256 = full turn).
    uint8_t  angle_ = 0;
    /// Dt·speed accumulator.
    uint64_t phaseNum_ = 0;
    uint32_t lastElapsed_ = 0;
};

} // namespace mm
