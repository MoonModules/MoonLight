// SPDX-License-Identifier: GPL-3.0-or-later

/// The PIO engine's option check (hub75_pio_options.h), in a file every host builds, so the seam's recorder refuses what the engine refuses.

#include "platform/rpi/hub75_pio_options.h"

namespace mm::platform {

namespace {

// The per-lane geometry the encoder validates: height a multiple of scanRate with an even rows-per-scan (each address step drives an upper and a lower row together), so a 32-high panel at 1/32 scan fails here; use 1/16.
bool laneGeometryInvalid(const Hub75PioOptions& o) {
    return !hub75LaneGeometry(o, o.cols * o.panelsPerLane, o.rows, 255).valid();
}

// The options the engine refuses beyond the rules both engines share, in the order they are checked.
constexpr OptionRule<Hub75PioOptions> kPioRules[] = {
    {[](const Hub75PioOptions& o) { return o.latchBlankWords > 8; }, "latchBlankWords must be 0..8"},
    {[](const Hub75PioOptions& o) { return o.pixelClockHz == 0; }, "pixelClockHz must be non-zero"},
    {[](const Hub75PioOptions& o) { return o.cols * o.panelsPerLane > 65535; },
     "the panels form a canvas wider than 65,535"},
    {laneGeometryInvalid,
     "invalid lane geometry: height must be a multiple of the scan rate with an even rows-per-scan (e.g. 64-high at 1/32, 32-high at 1/16)"},
};

}  // namespace

bool hub75PioCheckOptions(const Hub75PioOptions& o, std::string& error) {
    return hub75OptionsPass(o, kHub75CommonRules<Hub75PioOptions>, error) &&
           hub75OptionsPass(o, kPioRules, error);
}

}  // namespace mm::platform
