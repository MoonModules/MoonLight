#pragma once
/// The Ethernet wirings boards follow, each a pin map rather than one product, shared by the app's Ethernet card and MoonBase, so a board new to the app answers on its cable in recovery.

#include <cstdint>

namespace mm::eth {

/// One board's Ethernet wiring; -1 leaves a line unused.
struct Preset {
    const char* label;    ///< the name the Ethernet card lists
    int8_t type;          ///< the app's PHY type: 1 LAN8720, 2 IP101, 3 W5500, 4 YT8531, 0 none
    int8_t phyAddr;       ///< the PHY's address, -1 to scan the MDIO bus
    int8_t mdc, mdio, rst;   ///< the RMII management clock and data lines, and the PHY's reset
    int8_t rmiiClock;     ///< the RMII clock pin
    bool   rmiiClockExtIn;   ///< the clock fed in rather than driven out
    int8_t miso, mosi, sck, cs, irq;   ///< a SPI PHY's bus, chip select and interrupt lines
    bool   editable;      ///< false on a soldered map: those lines are not the user's to set
};

/// The presets; Custom keeps whatever is in the fields, for a hand-wired board.
inline constexpr Preset kPresets[] = {
    // The LAN8720 reference wiring most classic boards follow, which is also the chip default.
    {"Classic RMII",            1,  0, 23, 18,  5, 17, false, -1, -1, -1, -1, -1, false},
    // The same wiring with no reset, for a board using GPIO 5 as an LED lane: the PHY resets by jumper.
    {"Classic RMII (no reset)", 1,  0, 23, 18, -1, 17, false, -1, -1, -1, -1, -1, false},
    // The IP101 wiring the Waveshare P4 boards and the shields on their pinout follow, the clock fed in.
    {"P4 RMII",                 2,  1, 31, 52, 51, 50, true,  -1, -1, -1, -1, -1, false},
    // The S31's 1 Gb YT8531 PHY, addressed by scan rather than by a fixed address.
    {"S31 RGMII",               4, -1,  5,  6,  7, -1, false, -1, -1, -1, -1, -1, false},
    {"Custom",                  0, -1, -1, -1, -1, -1, false, -1, -1, -1, -1, -1, true},
};
inline constexpr uint8_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);   ///< the rows of kPresets
inline constexpr uint8_t kP4Rmii = 2;   ///< the P4's row: the one preset a P4 is offered, so its default
static_assert(kPresets[kP4Rmii].type == 2, "kP4Rmii names the IP101 row");

}  // namespace mm::eth
