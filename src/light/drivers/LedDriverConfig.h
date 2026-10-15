#pragma once

#include <cstdint>

namespace mm {

/// Wire timing for a clockless addressable-LED chipset. Pure data, with no platform include, so the encoder that reads it is host-testable. The defaults satisfy WS2812, WS2812B and SK6812.
///
/// @attribution WS2812
struct LedDriverConfig {
    /// How long a 0 bit stays high, in nanoseconds.
    uint32_t t0h_ns    = 350;
    /// How long a 1 bit stays high, in nanoseconds.
    uint32_t t1h_ns    = 700;
    uint32_t period_ns = 1250;   // full bit cell (t?h + the trailing LOW)
    uint32_t reset_us  = 300;    // idle-LOW latch between frames (>= 300 us, current silicon)
    /// Flip the output polarity, for an inverting level shifter.
    bool     invert    = false;
};

} // namespace mm
