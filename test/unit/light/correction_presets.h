#pragma once

/// @defgroup correction_presets Building a correction from a named order
/// @{
/// A test-only convenience for the curated color orders.
///
/// @moreinfo
///
/// Test-only convenience for building a Correction from a named color order (RGB/GRB/BGR/RGBW/GRBW).
/// The PRODUCTION Correction has one rebuild, rebuild(brightness, ChannelRole* roles, nChannels), because the real wirings come from the LightPresets library as role arrays. The curated-order enum + a rebuild(brightness, preset) overload used to live in Correction.h purely so tests could write `corr.rebuild(255, GRB)` tersely without hand-building a role array.
/// That was a test convenience sitting in production code (zero production callers, and it duplicated "what does GRB mean", which the library's seedBuiltins now owns). It lives here instead: the tests keep the terse form, and Correction.h carries only the one role-array rebuild the device actually uses.

#include "light/drivers/Correction.h"
#include "light/drivers/ChannelRole.h"

namespace mm::test {

/// The curated wire orders a test drives, mapped to role arrays in one place below.
enum class PresetOrder : uint8_t { RGB, GRB, BGR, RGBW, GRBW };

// Fill a Correction from a named order at `brightness`, leaving `curve` as the caller set it.
inline void rebuildFromPresetKeepingCurve(mm::Correction& c, uint8_t brightness, PresetOrder order) {
    using R = mm::ChannelRole;
    switch (order) {
        case PresetOrder::RGB:  { R r[] = {R::Red, R::Green, R::Blue};            c.rebuild(brightness, r, 3); break; }
        case PresetOrder::GRB:  { R r[] = {R::Green, R::Red, R::Blue};            c.rebuild(brightness, r, 3); break; }
        case PresetOrder::BGR:  { R r[] = {R::Blue, R::Green, R::Red};            c.rebuild(brightness, r, 3); break; }
        case PresetOrder::RGBW: { R r[] = {R::Red, R::Green, R::Blue, R::White};  c.rebuild(brightness, r, 4); break; }
        case PresetOrder::GRBW: { R r[] = {R::Green, R::Red, R::Blue, R::White};  c.rebuild(brightness, r, 4); break; }
    }
}

// Fill a Correction from a named order at `brightness`, via the production role-array rebuild.
inline void rebuildFromPreset(mm::Correction& c, uint8_t brightness, PresetOrder order) {
    // Linear: the ordering and white-maths tests are not about the curve, which has its own.
    c.curve = mm::Correction::Curve::Linear;
    rebuildFromPresetKeepingCurve(c, brightness, order);
}

/// @}
}  // namespace mm::test
