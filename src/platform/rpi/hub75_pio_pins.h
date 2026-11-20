// SPDX-License-Identifier: GPL-3.0-or-later

/// The Pi 5 PIO HUB75 backend's view of the active-3 pinout (hub75_board_pins.h): the wide-word bit each GPIO takes in the single state machine's OUT range (hub75.pio), the OE, strobe, address and clock bits, and the blank word.
///
/// The PIO program drives GPIO 2..27 as one contiguous 26-pin OUT range, and every GPIO in 2..27 is used by the board with no gaps, which is what makes the single-SM wide-word design possible (@xref{wide-word-layout|Wide-word layout}).
///
/// @technical
///
/// ## Wide-word layout
///
/// The PIO OUT range starts at GPIO 2 and covers 26 pins, so wide-word bit (gpio - 2) drives that GPIO (OUT shifts right: word bit 0 goes to the OUT base pin). The CLK bit is a don't-care, since side-set overrides OUT on GPIO 17, and the host writes 0 there.

#pragma once

#include <cstdint>
#include <initializer_list>

#include "platform/rpi/hub75_board_pins.h"   // shared signals and lane pins

namespace mm::platform {

/// The first GPIO of the PIO OUT range: wide-word bit 0 (@xref{wide-word-layout|Wide-word layout}).
constexpr uint8_t kOutBaseGpio = 2;
constexpr uint8_t kOutPinCount = 26;   ///< the OUT range size, GPIO 2..27 inclusive
constexpr uint8_t kClkGpio = shared::kClk;   ///< the side-set pin, which overlaps the OUT range

/// Wide-word bit for a GPIO, or -1 if the GPIO is outside the OUT range.
constexpr int wideBitForGpio(uint8_t gpio) {
    return (gpio >= kOutBaseGpio && gpio < kOutBaseGpio + kOutPinCount)
               ? static_cast<int>(gpio - kOutBaseGpio)
               : -1;
}

// Wide-word bit for the shared lines (handy in tests and the repack).

constexpr int kWideBitOe  = 16;   ///< GPIO 18; inverted: set means blanked
constexpr int kWideBitStb = 2;    ///< GPIO 4
constexpr int kWideBitA   = 20;   ///< GPIO 22
constexpr int kWideBitB   = 21;   ///< GPIO 23
constexpr int kWideBitC   = 22;   ///< GPIO 24
constexpr int kWideBitD   = 23;   ///< GPIO 25
constexpr int kWideBitE   = 13;   ///< GPIO 15
constexpr int kWideBitClk = 15;   ///< GPIO 17; don't-care (side-set wins)

/// A word that drives the panel dark (OE pin high, color 0, address 0, no latch, CLK bit 0): the blank frame on shutdown.
constexpr uint32_t kBlankWideWord = (1u << kWideBitOe);

// Sanity: the OUT range must cover every pin the board uses.
static_assert(wideBitForGpio(shared::kOe) == kWideBitOe, "OE bit");
static_assert(wideBitForGpio(shared::kStb) == kWideBitStb, "STB bit");
static_assert(wideBitForGpio(shared::kA) == kWideBitA, "A bit");
static_assert(wideBitForGpio(shared::kB) == kWideBitB, "B bit");
static_assert(wideBitForGpio(shared::kC) == kWideBitC, "C bit");
static_assert(wideBitForGpio(shared::kD) == kWideBitD, "D bit");
static_assert(wideBitForGpio(shared::kE) == kWideBitE, "E bit");
static_assert(wideBitForGpio(shared::kClk) == kWideBitClk, "CLK bit");

/// Whether the board's data pins fit the wide word: every color and shared pin in GPIO 2..27, none on CLK (which side-set drives), no two on one GPIO (@xref{wide-word-layout|Wide-word layout}).
constexpr bool pinsFitWideWord() {
    uint32_t used = 0;
    const auto take = [&](uint8_t gpio) {
        const int bit = wideBitForGpio(gpio);
        if (bit < 0 || gpio == kClkGpio || (used & (1u << bit)) != 0) return false;
        used |= 1u << bit;
        return true;
    };
    bool ok = true;
    for (const LanePins& lane : kLanePins)
        for (const uint8_t gpio : {lane.r1, lane.g1, lane.b1, lane.r2, lane.g2, lane.b2}) ok = take(gpio) && ok;
    for (const uint8_t gpio : {shared::kOe, shared::kStb, shared::kA, shared::kB, shared::kC, shared::kD, shared::kE}) ok = take(gpio) && ok;
    // The 25 data pins and CLK fill the 26-pin range with no gap.
    return ok && (used | (1u << kWideBitClk)) == (1u << kOutPinCount) - 1;
}
static_assert(pinsFitWideWord(), "the lane and shared pins must sit in GPIO 2..27, off CLK, each on its own GPIO, covering the range");

}  // namespace mm::platform
