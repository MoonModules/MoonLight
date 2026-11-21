// SPDX-License-Identifier: GPL-3.0-or-later

/// The active-3 board's pinout the Pi HUB75 engine drives: the shared signals and the three lanes' color pins as GPIO numbers; pin facts: @xref{source-of-the-pin-facts|Source of the pin facts}; the pins a kernel driver may own: @xref{pins-the-kernel-may-own|Pins the kernel may own}.
///
/// The Pi 5 PIO engine builds its wide-word bits from these numbers in hub75_pio_pins.h.
///
/// @attribution rpi-rgb-led-matrix
///
/// @technical
///
/// ## Source of the pin facts
///
/// The pin facts are from hzeller rpi-rgb-led-matrix, lib/hardware-mapping.c:30-66 (the `regular` mapping table) and wiring.md:96-175 (40-pin header table); the attribution catalog records that concepts are taken and no code is adapted.
///
/// The active-3 adapter uses the `regular` mapping: active vs passive is electrical (74HCT245 buffering) and the pinout is the same (adapter/active-3/README.md:52-55). The Adafruit HAT mapping is a different single-lane pinout and is not used here.
///
/// ## Pins the kernel may own
///
/// The PIO engine muxes all of GPIO 2 to 27 to PIO, and the board uses every one of them. Some belong to kernel drivers when those are enabled: I2C1 on GPIO 2 and 3, SPI0 on GPIO 7 to 11 (lane 1's color pins), UART0 on GPIO 14 and 15 (lane 3's R1 and the E line), 1-wire on GPIO 4 (the strobe).
///
/// And I2S on GPIO 18 to 21 (OE is 18; an I2S microphone is what an audio-reactive install adds) and PWM on GPIO 12, 13, 18 and 19 when a PWM overlay is loaded.
///
/// Those drivers lose their pins when the panels open, and a driver that muxes them again later corrupts the picture. Turn them off for the header in /boot/firmware/config.txt before wiring a panel: `dtparam=i2c_arm=off`, `dtparam=spi=off`, `enable_uart=0`, no `dtoverlay=w1-gpio`, no `dtparam=i2s=on` and no PWM overlay (`dtoverlay=pwm` or its variants). The code assumes them; check them yourself.
///
/// ## Shared signals
///
/// The shared signals fan out to all three lanes and are buffered per connector on the active board. E (row address bit 4, 1/32 only) sits on HUB75 pin 4 or 8 depending on the panel, and the board has a jumper for it.
///
/// ## Lane pins
///
/// Lane 1 is hzeller's chain 1, listed as [1] in his wiring.md, lane 2 his chain 2 as [2], lane 3 his chain 3 as [3]. R1/G1/B1 feed the upper half of the panel, R2/G2/B2 the lower half. A `kLanePins` row lists GPIO numbers in the order R1, G1, B1, R2, G2, B2; its comment gives the 40-pin header pins in the same order.

#pragma once

#include <cstdint>

#include "platform/Hub75Pio.h"   // kHub75Lanes: the board's lane count

namespace mm::platform {

/// The signals fanned out to all three lanes: each is a GPIO, with its 40-pin header pin in the trailing comment (@xref{shared-signals|Shared signals}).
namespace shared {
constexpr uint8_t kOe  = 18;   ///< header pin 12, active low (panel OE-); PWM0-capable (ALT5), which the GPIO engine's OE pulser needs
constexpr uint8_t kClk = 17;   ///< header pin 11, pixel clock (the PIO engine's side-set pin)
constexpr uint8_t kStb = 4;    ///< header pin 7, strobe / latch
constexpr uint8_t kA   = 22;   ///< header pin 15, row address bit 0
constexpr uint8_t kB   = 23;   ///< header pin 16, row address bit 1
constexpr uint8_t kC   = 24;   ///< header pin 18, row address bit 2
constexpr uint8_t kD   = 25;   ///< header pin 22, row address bit 3 (1/16 and up)
constexpr uint8_t kE   = 15;   ///< header pin 10, row address bit 4 (1/32 only)
}  // namespace shared

/// One lane's color signals as GPIO numbers: r1, g1, b1 feed the upper half of the panel, r2, g2, b2 the lower half (@xref{lane-pins|Lane pins}).
struct LanePins {
    uint8_t r1, g1, b1;   ///< upper half
    uint8_t r2, g2, b2;   ///< lower half
};

/// The lanes' color pins in LanePins order; each row's comment gives the 40-pin header pins (@xref{lane-pins|Lane pins}).
constexpr LanePins kLanePins[kHub75Lanes] = {
    {11, 27, 7, 8, 9, 10},    // lane 1: hdr 23, 13, 26, 24, 21, 19
    {12, 5, 6, 19, 13, 20},   // lane 2: hdr 32, 29, 31, 35, 33, 38
    {14, 2, 3, 26, 16, 21},   // lane 3: hdr  8,  3,  5, 37, 36, 40
};

}  // namespace mm::platform
