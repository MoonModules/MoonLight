// SPDX-License-Identifier: GPL-3.0-or-later

/// The rules the PIO engine's options obey, in plain C++ that builds on every host: the seam's recorder refuses the same options the engine does, whether or not a build has the engine.
///
/// @technical
///
/// ## Option rules
///
/// A check is a table of rows, each a predicate over the options and the reason it gives, with an optional detail that adds the figures; the first row that holds refuses. The five rows every Pi engine shares (lanes, panels, rows and columns, scan rate, depth) are `kHub75CommonRules`, and an engine adds its own rows after them.
///
/// ## Limits
///
/// The rules hold two stated limits, one constant each.
///
/// `kPioMinPixelClockHz` is 50 kHz. One word takes one pixel clock (the program spends 5 PIO cycles on it), so a 64 KiB chunk of 16,384 words takes the 6.12 kernel's 1 s wait at 16.4 kHz, and below that a transfer can return 0 with the frame cut short. 50 kHz keeps three times the margin.
///
/// `kHub75MaxBufferBytes` is 256 MiB, the one home of the memory limit both engines share. The smallest Pi 5 has 2 GB and the smallest Pi 3 1 GB, and the figure is about 140 times the 1,884,160 bytes (1.80 MiB) a 256 x 192 wall at 8 bits takes on the PIO.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "platform/Hub75Bus.h"   // Hub75Geometry
#include "platform/Hub75Pio.h"   // Hub75PioOptions

namespace mm::platform {

/// Whether `value` lies outside lo..hi, the shape of every range rule.
constexpr bool hub75Outside(int value, int lo, int hi) { return value < lo || value > hi; }

/// One option rule: the predicate that holds when the options are refused, and the reason it gives.
template <typename Options>
struct OptionRule {
    bool (*refuses)(const Options&);   ///< true when the options break this rule
    const char* reason;                ///< what the product owner reads when it does
    std::string (*detail)(const Options&) = nullptr;   ///< the figures that follow the reason, when the rule has any
};

/// The first rule of `rules` that `o` breaks fills `error` with its reason and returns false; true when none does.
template <typename Options, size_t N>
bool hub75OptionsPass(const Options& o, const OptionRule<Options> (&rules)[N], std::string& error) {
    for (const OptionRule<Options>& rule : rules) {
        if (!rule.refuses(o)) continue;
        error = rule.reason;
        if (rule.detail != nullptr) error += rule.detail(o);
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

/// The lowest pixel clock the engine takes, 50 kHz, so a 64 KiB chunk fits the 6.12 kernel's 1 s wait with margin (@xref{limits|limits}).
constexpr uint32_t kPioMinPixelClockHz = 50'000;

/// The most bytes the wall's buffers may total on either engine, 256 MiB, the one home of the memory limit (@xref{limits|limits}).
constexpr uint64_t kHub75MaxBufferBytes = 256ull * 1024 * 1024;

/// The DMA bounce buffer size the engine asks for at most, 64 KiB, and how many it asks for, the kernel's limit of 4.
constexpr uint32_t kPioXferBufSize = 65536;
constexpr uint32_t kPioXferBufCount = 4;   ///< the kernel's limit of DMA bounce buffers

/// Whether `bytes` of buffers pass the ceiling: exactly the ceiling is allowed.
constexpr bool hub75BufferBytesExceedCeiling(uint64_t bytes) { return bytes > kHub75MaxBufferBytes; }

/// The ceiling rule's figures for a wall that needs `bytes`: the need and the limit, in MB, the need rounded up.
inline std::string hub75BufferCeilingDetail(uint64_t bytes) {
    constexpr uint64_t kMegabyte = 1024 * 1024;
    return " (this wall's buffers need " + std::to_string((bytes + kMegabyte - 1) / kMegabyte) + " MB, the limit is " +
           std::to_string(kHub75MaxBufferBytes / kMegabyte) + " MB)";
}

/// The bytes the wall's buffers total for `o`: the RGB and encoded triple buffers, each lane's slots and the ring-deep blank; the engine allocates this much, and the ceiling rule reads it. Valid for options that pass the geometry rule.
uint64_t hub75PioBufferBytes(const Hub75PioOptions& o);

/// Whether `o` is a wall the engine can drive (panel counts, scan rate, depth, clock, per-lane geometry), else false with the reason in `error`; open() and the test recorder both refuse on it.
bool hub75PioCheckOptions(const Hub75PioOptions& o, std::string& error);

}  // namespace mm::platform
