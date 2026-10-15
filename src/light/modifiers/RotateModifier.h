#pragma once

#include "light/modifiers/ModifierBase.h"

namespace mm {

/// Turns the 2D image around its center, continuously, at the speed you set.
/// @card RotateModifier.gif
///
/// @attribution Brandon502, Rotation matrix
///
/// @moreinfo
///
/// The one dynamic modifier: it overrides modifyLive, so the Layer applies it every frame for a smooth turn rather than a stepped rebuild.
///
/// The live pass maps backward: for each destination cell it computes the source with the inverse rotation. That leaves no destination unfilled, the textbook reason image warping samples backward.
class RotateModifier : public ModifierBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// A rotation in the plane, so 2D, advisory for the UI chip.
    Dim dimensions() const override { return Dim::D2; }
    /// It animates, so it also transforms every frame.
    bool hasModifyLive() const override { return true; }

    /// How fast the image turns.
    uint8_t speed = 1;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("speed", speed, 1, 255);
    }

    /// No control forces a rebuild: the rotation is applied live rather than baked in.
    bool affectsPrepare(const char* /*controlName*/) const override { return false; }

    /// A backward map, per frame: each destination is replaced by the source cell it samples.
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

    /// The angle advances on the timer, applied next frame without any rebuild.
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
