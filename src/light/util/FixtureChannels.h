#pragma once

#include "light/util/light_types.h"

#include <cstdint>

namespace mm {

/// Where a fixture's non-color channels live inside one light, @xref{why-color-has-no-entry|color having none}.
///
/// @technical
///
/// A light is `channelsPerLight` bytes wide at runtime (3 for RGB, 4 for RGBW, 11 or 25 for a moving head), and this says which bytes are pan, tilt, zoom and the rest, so an effect steers a moving head through the buffer it paints.
///
/// ## Why color has no entry
///
/// Color is always at offset 0, where every draw primitive writes RGB or RGBW, and `Correction` moves it to the fixture's color channels on the way out; these offsets are the channels an effect has no other way to reach.
///
/// ## Why an absent channel is safe
///
/// An absent role's setters write nothing, so one moving-head effect steers a moving head and leaves an LED strip untouched; the predecessor MoonLight's [LightsHeader](https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Layers/LightsHeader.h) is the same idea.
///
/// ## Layer slots are not fixture channels
///
/// These are offsets into the layer's light, motion packed after the color in a fixed order; `Correction::apply` maps each slot to the fixture's own channel, so motion and color never claim the same byte.
///
/// A slot past the layer's width widens the layer: pan at slot 4 on a 3-channel layer makes every light 5 bytes wide at the next prepare, and `setChannelsPerLight` rejects zero.
struct FixtureChannels {
    /// Marks a role this fixture does not carry, making every setter for it a no-op: @xref{why-an-absent-channel-is-safe}.
    static constexpr uint8_t kAbsent = 255;
    /// Where motion starts in a layer light, after the RGBW the effects paint into: @xref{layer-slots-are-not-fixture-channels}.
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
