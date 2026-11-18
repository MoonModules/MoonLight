// SPDX-License-Identifier: GPL-3.0-or-later

/// The rules the PIO engine's options obey, in plain C++ that builds on every host: the seam's recorder refuses the same options the engine does, whether or not a build has the engine.
///
/// @technical
///
/// ## Option rules
///
/// A check is a table of rows, each a predicate over the options and the reason it gives; the first row that holds refuses. The five rows every Pi engine shares (lanes, panels, rows and columns, scan rate, depth) are `kHub75CommonRules`, and an engine adds its own rows after them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "platform/Hub75Bus.h"   // Hub75Geometry
#include "platform/Hub75Pio.h"   // Hub75PioOptions
#include "platform/rpi/hub75_board_pins.h"   // kHub75Lanes: the board's lane count

namespace mm::platform {

/// The most panels per lane an engine takes, 32: this layer builds independently of light/, so the number is stated here; an engine refuses longer ones rather than allocating a frame for them.
constexpr uint8_t kHub75PanelsPerLaneMax = 32;

/// Whether `value` lies outside lo..hi, the shape of every range rule.
constexpr bool hub75Outside(int value, int lo, int hi) { return value < lo || value > hi; }

/// One option rule: the predicate that holds when the options are refused, and the reason it gives.
template <typename Options>
struct OptionRule {
    bool (*refuses)(const Options&);   ///< true when the options break this rule
    const char* reason;                ///< what the product owner reads when it does
};

/// The first rule of `rules` that `o` breaks fills `error` with its reason and returns false; true when none does.
template <typename Options, size_t N>
bool hub75OptionsPass(const Options& o, const OptionRule<Options> (&rules)[N], std::string& error) {
    for (const OptionRule<Options>& rule : rules) {
        if (!rule.refuses(o)) continue;
        error = rule.reason;
        return false;
    }
    return true;
}

/// The rules both engines apply first: the lane and panel counts, the panel size, the scan rate and the depth (@xref{option-rules|option rules}).
template <typename Options>
inline constexpr OptionRule<Options> kHub75CommonRules[] = {
    {[](const Options& o) { return hub75Outside(o.parallel, 1, kHub75Lanes); }, "parallel must be 1..3 (the board drives 3 lanes)"},
    {[](const Options& o) { return hub75Outside(o.panelsPerLane, 1, kHub75PanelsPerLaneMax); }, "panelsPerLane must be 1..32 panels"},
    {[](const Options& o) { return o.rows == 0 || o.cols == 0; }, "rows and cols must be non-zero"},
    {[](const Options& o) { return o.scanRate != 8 && o.scanRate != 16 && o.scanRate != 32; },
     "scanRate must be 8, 16 or 32 (1/64 is not supported: the encoder has no 6th address line and the board no F line)"},
    {[](const Options& o) { return hub75Outside(o.bitDepth, 2, kHub75EncodeMaxBitDepth); }, "bitDepth must be 2..8 (MoonLight encoder limit)"},
};

/// The encoder's view of one lane: `width` by `height` driven pixels at the options' scan rate and depth, lit at `brightness`.
template <typename Options>
Hub75Geometry hub75LaneGeometry(const Options& o, int width, int height, uint8_t brightness) {
    Hub75Geometry geo;
    geo.width = static_cast<uint16_t>(width);
    geo.height = static_cast<uint16_t>(height);
    geo.scanRate = o.scanRate;
    geo.bitDepth = o.bitDepth;
    geo.brightness = brightness;
    return geo;
}

/// Whether `o` is a wall the engine can drive (panel counts, scan rate, depth, clock, per-lane geometry), else false with the reason in `error`; open() and the test recorder both refuse on it.
bool hub75PioCheckOptions(const Hub75PioOptions& o, std::string& error);

}  // namespace mm::platform
