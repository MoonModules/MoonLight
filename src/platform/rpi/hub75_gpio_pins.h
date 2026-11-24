// SPDX-License-Identifier: GPL-3.0-or-later

/// The Pi 3/4 GPIO backend's view of the active-3 board pinout (hub75_board_pins.h): the raw GPIO bit masks built from the pin numbers, and the GPFSEL register helpers.
///
/// This engine writes the BCM GPIO set/clear registers directly, so the masks are raw GPIO bit positions (1u << gpio) (@xref{gpio-bit-masks|GPIO bit masks}); the Pi 5 PIO engine drives GPIO 2..27 as one wide word, and its hub75_pio_pins.h carries wide-word bits.
///
/// @technical
///
/// ## GPIO bit masks
///
/// The dump loop writes the masks to GPSET0/GPCLR0; the setup writes the dark levels through them before any pin becomes an output (values, then directions, then the mux).

#pragma once

#include <cstdint>

#include "platform/rpi/hub75_board_pins.h"   // shared signals and lane pins

namespace mm::platform {

// GPIO bit masks, raw GPIO positions.

constexpr uint32_t kGpioClkBit = (1u << shared::kClk);  ///< the pixel clock line
constexpr uint32_t kGpioStbBit = (1u << shared::kStb);  ///< the strobe / latch line
constexpr uint32_t kGpioOeBit  = (1u << shared::kOe);   ///< the output enable line

/// The five row-address lines as one mask.
constexpr uint32_t kGpioAddressMask = (1u << shared::kA) | (1u << shared::kB) |
                                     (1u << shared::kC) | (1u << shared::kD) |
                                     (1u << shared::kE);

/// All eighteen color lines (three lanes x six) as one mask.
constexpr uint32_t kGpioColorMask =
    (1u << kLanePins[0].r1) | (1u << kLanePins[0].g1) | (1u << kLanePins[0].b1) |
    (1u << kLanePins[0].r2) | (1u << kLanePins[0].g2) | (1u << kLanePins[0].b2) |
    (1u << kLanePins[1].r1) | (1u << kLanePins[1].g1) | (1u << kLanePins[1].b1) |
    (1u << kLanePins[1].r2) | (1u << kLanePins[1].g2) | (1u << kLanePins[1].b2) |
    (1u << kLanePins[2].r1) | (1u << kLanePins[2].g1) | (1u << kLanePins[2].b1) |
    (1u << kLanePins[2].r2) | (1u << kLanePins[2].g2) | (1u << kLanePins[2].b2);

/// Every GPIO the board drives: color + address + clock + latch + OE.
constexpr uint32_t kGpioAllPinsMask = kGpioColorMask | kGpioAddressMask | kGpioClkBit |
                                     kGpioStbBit | kGpioOeBit;

/// The row-address lines for address `row` (0..31) as a GPIO mask: A is bit 0 of the row, E is bit 4.
constexpr uint32_t gpioAddressMaskForRow(uint8_t row) {
    uint32_t m = 0;
    if (row & 0x01) m |= (1u << shared::kA);
    if (row & 0x02) m |= (1u << shared::kB);
    if (row & 0x04) m |= (1u << shared::kC);
    if (row & 0x08) m |= (1u << shared::kD);
    if (row & 0x10) m |= (1u << shared::kE);
    return m;
}

/// The GPFSEL register index (0..5) for a GPIO pin: ten pins per register (BCM2835 ARM Peripherals section 6).
constexpr int gpioGpfselReg(uint8_t gpio) { return gpio / 10; }
/// The bit shift of a GPIO pin within its GPFSEL register: three bits per pin.
constexpr int gpioGpfselShift(uint8_t gpio) { return (gpio % 10) * 3; }

// GPFSEL function values (BCM2835 section 6.1).

constexpr uint32_t kGpioGpioOutput = 1;  ///< 001: output
constexpr uint32_t kGpioGpioAlt5 = 2;    ///< 010: alternate function 5 (PWM0 on GPIO 18)

static_assert((kGpioColorMask & kGpioAddressMask) == 0, "color and address pins are distinct");
static_assert((kGpioAllPinsMask & kGpioClkBit) != 0 && (kGpioAllPinsMask & kGpioStbBit) != 0 &&
                  (kGpioAllPinsMask & kGpioOeBit) != 0,
              "the shared lines are in the board mask");

}  // namespace mm::platform
