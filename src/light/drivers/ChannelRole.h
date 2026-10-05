#pragma once

#include <cstdint>

namespace mm {

/// @defgroup ChannelRole What one output channel of a light carries
/// @{
/// The shared vocabulary naming each channel's role, so a wiring description and the writers that drive it cannot drift.
///
/// @moreinfo
///
/// A light is a run of channels, and this names the role of each: the color roles a strip or panel needs, and the fixture roles a moving head adds.
/// Two sides use it.
/// `Correction` describes a light's wiring as an array of these, where `roles[i]` is what channel `i` is.
/// The effect-side writers such as `setRGB` and `setPan` name the role they drive.
///
/// `None` marks a channel carrying no role we drive, a spacer or a fixture channel set elsewhere.
/// The color roles come first, so a plain RGB or RGBW light only ever uses the low values and the fixture roles extend the list without disturbing them.
///
/// ## The enum is append-only
///
/// A persisted fixture profile stores each channel's role as this enum's byte value, so a role is never renumbered.
/// A new one goes at the end, since the seeded moving-head profiles persist fixture roles too.
///
/// `White` is a normal or cold white and `WarmWhite` the second white a CCT fixture adds, while `Yellow` and `UV` are the extra par-can colors of a six-channel RGBWYP lightbar.

enum class ChannelRole : uint8_t {
    None,
    Red, Green, Blue, White, WarmWhite, Yellow, UV,  // color roles — strips/panels/PARs (White = cold)
    Pan, Tilt, Zoom, Rotate, Gobo, Dimmer,           // fixture roles — moving heads
    RedFine, GreenFine, BlueFine, WhiteFine, WarmWhiteFine,   // the low bytes of 16-bit color channels
    PanFine, TiltFine, DimmerFine,                   // the low bytes of 16-bit aim and master dimmer
};

/// Option strings for a Select bound to a role, index-aligned with the enum so a control's byte casts straight across.
inline constexpr const char* kChannelRoleOptions[] = {
    "—", "R", "G", "B", "W", "WW", "Y", "UV", "Pan", "Tilt", "Zoom", "Rotate", "Gobo", "Dimmer",
    "R fine", "G fine", "B fine", "W fine", "WW fine", "Pan fine", "Tilt fine", "Dimmer fine",
};
inline constexpr uint8_t kChannelRoleCount =
    sizeof(kChannelRoleOptions) / sizeof(kChannelRoleOptions[0]);

// The two must stay the same length: a missed string breaks the build here rather than mislabelling a role at runtime.
static_assert(kChannelRoleCount == static_cast<uint8_t>(ChannelRole::DimmerFine) + 1,
              "kChannelRoleOptions must have one string per ChannelRole value (index-aligned)");

/// @}
} // namespace mm
