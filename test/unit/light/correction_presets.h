#pragma once

/// Test-only conveniences for building a Correction: the curated color orders and a 32-channel moving head.
///
/// @technical
///
/// The production Correction has one rebuild, rebuild(brightness, ChannelRole* roles, nChannels), because the real wirings come from the FixtureProfiles library as role arrays.
///
/// The named orders (RGB/GRB/BGR/RGBW/GRBW) live here so a test keeps the terse `rebuildFromPreset(c, 255, PresetOrder::GRB)`; the library's seedBuiltins owns what each order means in production.
///
/// `WideProfile` places a light's color at channels 9 to 11 of 32, for a case that needs a wiring wider than RGB.

#include "light/drivers/Correction.h"
#include "light/drivers/ChannelRole.h"

namespace mm {

/// The curated wire orders a test drives, mapped to role arrays in one place below.
enum class PresetOrder : uint8_t { RGB, GRB, BGR, RGBW, GRBW };

/// Fill a Correction from a named order at `brightness`, via the production role-array rebuild.
inline void rebuildFromPreset(mm::Correction& c, uint8_t brightness, PresetOrder order) {
    using R = mm::ChannelRole;
    // Linear: the ordering and white-maths tests are not about the curve, which has its own.
    c.curve = mm::Correction::Curve::Linear;
    switch (order) {
        case PresetOrder::RGB:  { R r[] = {R::Red, R::Green, R::Blue};            c.rebuild(brightness, r, 3); break; }
        case PresetOrder::GRB:  { R r[] = {R::Green, R::Red, R::Blue};            c.rebuild(brightness, r, 3); break; }
        case PresetOrder::BGR:  { R r[] = {R::Blue, R::Green, R::Red};            c.rebuild(brightness, r, 3); break; }
        case PresetOrder::RGBW: { R r[] = {R::Red, R::Green, R::Blue, R::White};  c.rebuild(brightness, r, 4); break; }
        case PresetOrder::GRBW: { R r[] = {R::Green, R::Red, R::Blue, R::White};  c.rebuild(brightness, r, 4); break; }
    }
}

/// A 32-channel moving head with its color at channels 9 to 11, so correcting one light writes all 32.
struct WideProfile {
    ChannelRole roles[32] = {};   ///< every channel's role, unused but the three
    /// Red, green and blue at 9, 10 and 11.
    WideProfile() {
        roles[9] = ChannelRole::Red;
        roles[10] = ChannelRole::Green;
        roles[11] = ChannelRole::Blue;
    }
};

}  // namespace mm
