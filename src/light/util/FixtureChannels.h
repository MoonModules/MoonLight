#pragma once

#include "light/util/light_types.h"

#include <cstdint>

namespace mm {

/// Where a fixture's non-color channels live inside one light.
///
/// @technical
///
/// A light is `channelsPerLight` bytes wide, and that width is a runtime value: 3 for RGB, 4 for RGBW, 11 for a mini moving head, 25 for a bigger one.
/// The pipeline does not care which, since a wider light simply has more bytes and a driver that cannot use them ignores them.
/// This says which of those bytes are pan, tilt, zoom and so on, so an effect drives a moving head through the same buffer it paints pixels into.
///
/// ## Why color has no entry
///
/// Color is always at offset 0.
/// Every effect and every draw primitive writes RGB or RGBW at the start of a light.
/// `Correction` places those bytes at the fixture's own color channels on the way out.
/// The offsets here are the channels an effect could not otherwise reach at all.
///
/// ## Why an absent channel is safe
///
/// `kAbsent` means the fixture has no such channel, and every setter is then a no-op.
/// That is what keeps a moving-head effect harmless on an LED strip.
/// It calls `setPan`, the strip carries no pan channel, and nothing is written.
/// The same effect on a moving head steers it.
/// MoonLight's LightsHeader is the same idea, credited in the work archive.
///
/// ## Layer slots are not fixture channels
///
/// These are offsets into the layer's light, which is not the fixture's layout.
/// A layer light always begins with RGB or RGBW, because every draw primitive writes there, so motion is packed after the color in a fixed order.
/// The fixture's own offsets live in `Correction`, whose `apply` maps a layer slot to a fixture channel on the way out.
/// Keeping the two apart is what stopped pan from landing on the red channel, which happened when both claimed byte 0.
struct FixtureChannels {
    /// Marks a role this fixture does not carry, making every setter for it a no-op.
    static constexpr uint8_t kAbsent = 255;
    /// Where motion starts in a layer light, after the RGBW the effects paint into.
    static constexpr uint8_t kMotionBase = 4;

    uint8_t pan    = kAbsent;   ///< the slot carrying pan, or absent
    uint8_t tilt   = kAbsent;   ///< the slot carrying tilt, or absent
    uint8_t zoom   = kAbsent;   ///< the slot carrying zoom, or absent
    uint8_t rotate = kAbsent;   ///< the slot carrying rotation, or absent
    uint8_t gobo   = kAbsent;   ///< the slot carrying the gobo, or absent

    /// True when this fixture can be aimed, so an effect asks before doing motion maths a strip would discard.
    bool movable() const { return pan != kAbsent || tilt != kAbsent; }

    /// Walk the motion roles in packing order, the one home for that order, which both Drivers and `Correction::apply` walk.
    template <typename Fn>
    static void forEachMotionSlot(const bool present[5], Fn&& f) {
        uint8_t slot = kMotionBase;
        for (uint8_t role = 0; role < 5; role++)
            if (present[role]) f(role, slot++);
    }
};

} // namespace mm
