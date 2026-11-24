// SPDX-License-Identifier: GPL-3.0-or-later

/// The GPIO engine's option check (hub75_gpio_options.h), in a file every host builds, so the seam's recorder refuses what the engine refuses.

#include "platform/rpi/hub75_gpio_options.h"

namespace mm::platform {

namespace {

// The driven lane the encoder validates, lit at full brightness.
Hub75Geometry drivenGeometry(const Hub75GpioOptions& o) {
    return hub75LaneGeometry(o, static_cast<int>(hub75GpioDrivenWidth(o)), hub75GpioDrivenHeight(o), 255);   // the width rule runs first, so it fits
}

// Whether the driven lane breaks the encoder's contract: height a multiple of the scan rate with an even rows-per-scan (each address step drives an upper and a lower row together).
bool laneGeometryInvalid(const Hub75GpioOptions& o) { return !drivenGeometry(o).valid(); }

// The ceiling rule's figures for this wall.
std::string bufferCeilingDetail(const Hub75GpioOptions& o) { return hub75BufferCeilingDetail(hub75GpioBufferBytes(o)); }

// The options the engine refuses beyond the rules both engines share, in the order they are checked.
constexpr OptionRule<Hub75GpioOptions> kGpioRules[] = {
    {[](const Hub75GpioOptions& o) { return o.multiplexing >= kHub75MultiplexCount; }, "multiplexing must be 0 (normal) or 1 (Stripe)"},
    // Stripe drives the halved panel at its scan, rows/4 (a 32-high panel at 1/8, a 64-high one at 1/16): a direct scan addresses rows the panel lacks, and a taller panel maps rows past the driven one.
    {[](const Hub75GpioOptions& o) { return o.multiplexing == kHub75MultiplexStripe && !hub75GpioMultiplexScanCoversRows(o.multiplexing, o.rows, o.scanRate); },
     "Stripe drives rows/4 address steps (a 32-high panel at 1/8, a 64-high one at 1/16): rows must be 4 times scanRate"},
    {[](const Hub75GpioOptions& o) { return hub75Outside(o.pwmLsbNs, kHub75GpioPwmLsbMinNs, kHub75GpioPwmLsbMaxNs); }, "pwmLsbNs must be 50..3000 ns"},
    {[](const Hub75GpioOptions& o) { return hub75Outside(o.slowdown, -1, kHub75GpioSlowdownMax); }, "slowdown must be -1 (auto) or 0..4"},
    {[](const Hub75GpioOptions& o) { return hub75GpioDrivenWidth(o) > 65535; },
     "the driven panels form a lane wider than 65,535"},
    {[](const Hub75GpioOptions& o) { return o.multiplexing == kHub75MultiplexStripe && laneGeometryInvalid(o); },
     "invalid Stripe lane geometry: the driven panel (rows/2 high, cols*2 wide) must be a multiple of the scan rate with an even rows-per-scan (e.g. 32-high at 1/8, 64-high at 1/16)"},
    {laneGeometryInvalid,
     "invalid lane geometry: height must be a multiple of the scan rate with an even rows-per-scan (e.g. 64-high at 1/32, 32-high at 1/16)"},
    {[](const Hub75GpioOptions& o) { return hub75BufferBytesExceedCeiling(hub75GpioBufferBytes(o)); }, "the wall's buffers exceed the engine's memory limit", bufferCeilingDetail},
};

}  // namespace

uint64_t hub75GpioBufferBytes(const Hub75GpioOptions& o) {
    const uint64_t lanes = o.parallel;
    const uint64_t canvasPixels = static_cast<uint64_t>(o.cols) * o.panelsPerLane * o.rows * lanes;
    const uint64_t laneBytes = static_cast<uint64_t>(hub75GpioDrivenWidth(o)) * hub75GpioDrivenHeight(o) * 3;
    const uint64_t frameSlots = drivenGeometry(o).frameSlots();
    const uint64_t rgb = 3 * canvasPixels * 3;                         // the RGB triple buffer
    const uint64_t strips = laneBytes + lanes * laneBytes;             // one lane's strip, then each lane's carried remainders
    const uint64_t laneSlots = lanes * frameSlots * sizeof(uint16_t);  // each lane's slot stream
    const uint64_t encoded = 3 * frameSlots * sizeof(uint32_t);        // the encoded triple buffer
    const uint64_t blank = frameSlots * sizeof(uint32_t);              // the dark frame
    return rgb + strips + laneSlots + encoded + blank;
}

bool hub75GpioCheckOptions(const Hub75GpioOptions& o, std::string& error) {
    return hub75OptionsPass(o, kHub75CommonRules<Hub75GpioOptions>, error) &&
           hub75OptionsPass(o, kGpioRules, error);
}

}  // namespace mm::platform
