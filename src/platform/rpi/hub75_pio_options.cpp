// SPDX-License-Identifier: GPL-3.0-or-later

/// The PIO engine's option check (hub75_pio_options.h), in a file every host builds, so the seam's recorder refuses what the engine refuses.

#include "platform/rpi/hub75_pio_options.h"

#include <algorithm>

#include "platform/rpi/hub75_pio_backend.h"   // kBytesPerPixelClock

namespace mm::platform {

uint64_t hub75PioBufferBytes(const Hub75PioOptions& o) {
    const uint64_t lanes = o.parallel;
    const uint64_t width = static_cast<uint64_t>(o.cols) * o.panelsPerLane;
    const uint64_t height = o.rows;
    const uint64_t frameSlots = hub75LaneGeometry(o, static_cast<int>(width), static_cast<int>(height), 255).frameSlots();
    const uint64_t rgb = 3 * width * height * lanes * 3;                    // the RGB triple buffer
    const uint64_t laneSlots = lanes * frameSlots * sizeof(uint16_t);       // each lane's slot stream
    const uint64_t encoded = 3 * frameSlots * sizeof(uint32_t);             // the encoded triple buffer
    const uint64_t ringBuffer = std::min<uint64_t>(kPioXferBufSize, frameSlots * kBytesPerPixelClock);
    return rgb + laneSlots + encoded + kPioXferBufCount * ringBuffer;       // the blank, as deep as the DMA ring
}

namespace {

// The per-lane geometry the encoder validates: height a multiple of scanRate with an even rows-per-scan (each address step drives an upper and a lower row together), so a 32-high panel at 1/32 scan fails here; use 1/16.
bool laneGeometryInvalid(const Hub75PioOptions& o) {
    return !hub75LaneGeometry(o, o.cols * o.panelsPerLane, o.rows, 255).valid();
}

// The floor rule's figure: the least pixel clock, which keeps a 64 KiB chunk inside the 6.12 kernel's 1 s wait.
std::string pixelClockFloorDetail(const Hub75PioOptions&) {
    return " (it must be at least " + std::to_string(kPioMinPixelClockHz) + " Hz, which keeps a 64 KiB chunk inside the 6.12 kernel's 1 s wait)";
}

// The ceiling rule's figures for this wall.
std::string bufferCeilingDetail(const Hub75PioOptions& o) { return hub75BufferCeilingDetail(hub75PioBufferBytes(o)); }

// The options the engine refuses beyond the rules both engines share, in the order they are checked.
constexpr OptionRule<Hub75PioOptions> kPioRules[] = {
    {[](const Hub75PioOptions& o) { return o.latchBlankWords > 8; }, "latchBlankWords must be 0..8"},
    {[](const Hub75PioOptions& o) { return o.pixelClockHz < kPioMinPixelClockHz; }, "pixelClockHz is under the engine's floor", pixelClockFloorDetail},
    {[](const Hub75PioOptions& o) { return o.cols * o.panelsPerLane > 65535; },
     "the panels form a canvas wider than 65,535"},
    {laneGeometryInvalid,
     "invalid lane geometry: height must be a multiple of the scan rate with an even rows-per-scan (e.g. 64-high at 1/32, 32-high at 1/16)"},
    {[](const Hub75PioOptions& o) { return hub75BufferBytesExceedCeiling(hub75PioBufferBytes(o)); }, "the wall's buffers exceed the engine's memory limit", bufferCeilingDetail},
};

}  // namespace

bool hub75PioCheckOptions(const Hub75PioOptions& o, std::string& error) {
    return hub75OptionsPass(o, kHub75CommonRules<Hub75PioOptions>, error) &&
           hub75OptionsPass(o, kPioRules, error);
}

}  // namespace mm::platform
