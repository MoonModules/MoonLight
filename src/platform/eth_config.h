#pragma once

#include <cstdint>

namespace mm::platform {

/// Which Ethernet PHY a board carries; plain integers, which keeps SDK includes out of this header.
enum EthPhyType {
    ethNone    = 0,  ///< no Ethernet on this board, the default
    ethLan8720 = 1,  ///< RMII, generic PHY
    ethIp101   = 2,  ///< RMII, IP101 PHY, P4 only
    ethW5500   = 3,  ///< SPI, an external W5500 module
    ethYt8531  = 4,  ///< RGMII, the S31's 1 Gb PHY
    ethOpeneth = 5,  ///< the emulator's MAC, which is what gives an emulated device a real IP stack
    ethDp83825 = 6,  ///< RMII, the Teensy 4.1's on-board PHY
};

/// How a board wires its Ethernet, set at runtime from the catalog; -1 leaves a field unused.
///
/// @moreinfo The RMII fields serve the internal EMAC and the SPI fields a W5500, with each platform's default seeding an un-provisioned board.
/// The RMII data lines are absent because no board varies them: they are fixed in silicon or already defaulted by the SDK.
struct EthPinConfig {
    int phyType;          ///< which EthPhyType
    int phyAddr;          ///< the PHY's address on the management bus
    int mdcGpio;          ///< management clock
    int mdioGpio;         ///< management data
    int rstGpio;          ///< PHY reset
    int rmiiClockGpio;    ///< the 50 MHz reference clock pin
    bool rmiiClockExtIn;  ///< true when the board feeds the clock in, false when the chip drives it out
    int spiMiso;          ///< W5500 data in
    int spiMosi;          ///< W5500 data out
    int spiSck;           ///< W5500 clock
    int spiCs;            ///< W5500 chip select
    int spiIrq;           ///< W5500 interrupt
};

}  // namespace mm::platform
