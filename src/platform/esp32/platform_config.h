#pragma once

/// @defgroup platform_config_esp32 ESP32 platform configuration
/// @{
/// What each ESP32 chip can do, derived from the SDK's own capability flags wherever possible.
///
/// @moreinfo
///
/// ## Capabilities, not chip names
///
/// Gating keys off the SDK's own flags, so a new chip works untouched and only a seam the flags cannot answer earns an `is<Chip>` constant.
/// Three chips earn one: the P4 and S3 for their Ethernet defaults, and the S31 for its RGMII PHY.
///
/// ## The i80 gate names LCD_CAM's own mode
///
/// The classic chip sets the broader flag for an unrelated peripheral.
/// Gating on it once wired the parallel driver onto that chip and hung its boot on a bus it lacks.
///
/// ## Fixed pads are published
///
/// The EMAC's data pads are silicon's choice, so one list serves both the driver init and the pin map.
/// A pad nobody declares reads as free while the MAC drives it: an LED lane on GPIO 10 corrupted every frame sent, with the link still reporting 1 Gb.

#include "sdkconfig.h"

#include <cstdint>

// The only per-target definition of the RMT channel count since the SDK dropped its public macro.
#ifdef CONFIG_SOC_RMT_SUPPORTED
#include "hal/rmt_ll.h"
#endif

// Marks a function RAM-resident: a render loop evicts a flash-resident ISR path between firings.
#include "esp_attr.h"
#define MM_RAMFUNC IRAM_ATTR

namespace mm::platform {

#ifdef CONFIG_SPIRAM
constexpr bool hasPsram = true;
#else
constexpr bool hasPsram = false;
#endif

// Gating keys off the SDK's flags, so only a seam they cannot answer earns an `is<Chip>` constant.
#ifdef CONFIG_IDF_TARGET_ESP32P4
constexpr bool isEsp32P4 = true;
#else
constexpr bool isEsp32P4 = false;
#endif

// The S3 has no internal EMAC, so its Ethernet default is W5500 over SPI where others use RMII.
#ifdef CONFIG_IDF_TARGET_ESP32S3
constexpr bool isEsp32S3 = true;
#else
constexpr bool isEsp32S3 = false;
#endif

// The S31 is the only 1 Gb RGMII target, so it defaults to a distinct PHY and pin set.
#ifdef CONFIG_IDF_TARGET_ESP32S31
constexpr bool isEsp32S31 = true;
#else
constexpr bool isEsp32S31 = false;
#endif

/// One pad the EMAC's data interface owns: the signal it carries, and the GPIO silicon fixed it to.
///
/// @moreinfo Neither RGMII on the S31 nor RMII on the P4 is configurable, so these are reported as fixed pins rather than published as controls.
/// One list serves both the driver init and the pin map, which keeps the MAC's wiring and the map's label from drifting apart.
/// A pad nobody declares reads as free while the MAC drives it: an LED lane on GPIO 10 corrupted every frame sent, with the link still reporting 1 Gb.
struct EthFixedPad { const char* name; uint8_t gpio; };
#ifdef CONFIG_IDF_TARGET_ESP32S31
constexpr EthFixedPad ethFixedPads[] = {
    {"ethTxd0",  8}, {"ethTxd1",  9}, {"ethTxd2", 10}, {"ethTxd3", 11},
    {"ethTxCtl", 12}, {"ethTxClk", 13}, {"ethRxClk", 14}, {"ethRxCtl", 15},
    {"ethRxd3", 16}, {"ethRxd2", 17}, {"ethRxd1", 18}, {"ethRxd0", 19},
};
constexpr uint8_t ethFixedPadCount = 12;
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
// P4 RMII data lines; the management pair stays a routable control.
constexpr EthFixedPad ethFixedPads[] = {
    {"ethTxEn", 49}, {"ethTxd0", 34}, {"ethTxd1", 35},
    {"ethCrsDv", 28}, {"ethRxd0", 29}, {"ethRxd1", 30},
};
constexpr uint8_t ethFixedPadCount = 6;
#elif defined(CONFIG_IDF_TARGET_ESP32)
// Classic RMII: one pad choice per signal, so these are the pins rather than a default.
constexpr EthFixedPad ethFixedPads[] = {
    {"ethTxEn", 21}, {"ethTxd0", 19}, {"ethTxd1", 22},
    {"ethCrsDv", 27}, {"ethRxd0", 25}, {"ethRxd1", 26},
};
constexpr uint8_t ethFixedPadCount = 6;
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S31) || defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32)
// Every ESP32 is Xtensa or RISC-V, so a third is an unfinished port rather than a configuration.
#if !defined(__XTENSA__) && !defined(__riscv)
#error "MoonLive ESP32 backend: unsupported ISA (expected Xtensa or RISC-V)"
#endif

static_assert(ethFixedPadCount == sizeof(ethFixedPads) / sizeof(ethFixedPads[0]),
              "the count gates every loop over this list: a mismatch reads past the end");
#else
// Nothing fixed to publish here. A single dummy entry, a zero-size array being an extension MSVC refuses.
constexpr EthFixedPad ethFixedPads[] = {{"", 0}};
constexpr uint8_t ethFixedPadCount = 0;
#endif

// RMT TX channels this chip offers, which the driver guards on rather than on a chip family.
#ifdef CONFIG_SOC_RMT_SUPPORTED
constexpr uint8_t rmtTxChannels = RMT_LL_TX_CANDIDATES_PER_INST;
#else
constexpr uint8_t rmtTxChannels = 0;
#endif

// Lanes over the LCD_CAM i80 bus. The gate names its own mode: the broader flag once hung a boot.
#ifdef CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED
constexpr uint8_t lcdLanes = 16;
#else
constexpr uint8_t lcdLanes = 0;
#endif

// Separate from the lane count because the host reports lanes while having no such silicon.
constexpr bool hasLcdCam = (lcdLanes > 0);

// Lanes over Parlio, which takes the GPIOs directly and runs any count from 1 to 16.
#ifdef CONFIG_SOC_PARLIO_SUPPORTED
constexpr uint8_t parlioLanes = 16;
#else
constexpr uint8_t parlioLanes = 0;
#endif

// Lanes over the classic chip's I2S in i80 mode; the gate excludes LCD_CAM chips, which share a flag.
#if defined(CONFIG_SOC_LCD_I80_SUPPORTED) && !defined(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED)
constexpr uint8_t i2sLanes = 16;
#else
constexpr uint8_t i2sLanes = 0;
#endif

// True on every current ESP32, the gate keeping the audio seam inert on a future I2S-less target.
#ifdef CONFIG_SOC_I2S_SUPPORTED
constexpr bool hasI2sMic = true;
#else
constexpr bool hasI2sMic = false;
#endif

// USB video needs a High-Speed USB PHY, which the S3 lacks, and a hardware JPEG decoder. Only the P4 compiles an implementation, so widen all three together or none.
#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CONFIG_SOC_USB_UTMI_PHY_NUM) && \
    defined(CONFIG_SOC_JPEG_DECODE_SUPPORTED)
constexpr bool hasUsbVideo = true;
#else
constexpr bool hasUsbVideo = false;
#endif

// OS capture devices are a desktop concept; boards use the pin-wired I2S mic above.
constexpr bool hasAudioCapture = false;


/// Which codec sits in front of the microphone, where a board puts one there.
enum class CodecType : uint8_t { None = 0, Es8311 = 1 };
/// How that codec is reached: a fixed board property, so it lives here rather than as a control.
///
/// @moreinfo The microphone's own data pins stay user controls; only the codec's wiring is fixed.
struct AudioCodecPins {
    uint16_t i2cSda;    ///< I2C data
    uint16_t i2cScl;    ///< I2C clock
    uint16_t mclk;      ///< the master clock the codec needs, separate from the bit clock
    uint8_t  i2cAddr;   ///< the codec's I2C address
};

// The S31 CoreBoard's ES8311, bench-confirmed by scan: its schematic labels SDA and SCL the other way.
#ifdef CONFIG_IDF_TARGET_ESP32S31
constexpr CodecType audioCodecType = CodecType::Es8311;
constexpr AudioCodecPins audioCodecPins = { /*sda*/ 51, /*scl*/ 50, /*mclk*/ 52, /*addr*/ 0x18 };
#else
constexpr CodecType audioCodecType = CodecType::None;
constexpr AudioCodecPins audioCodecPins = { 0, 0, 0, 0 };
#endif

// The Ethernet-only profile drops the WiFi components and defines this, there being no SDK switch.
#ifdef MM_NO_WIFI
constexpr bool hasWiFi = false;
#else
constexpr bool hasWiFi = true;
#endif

// The P4's WiFi runs on a co-processor behind an identical API, so only its read-out needs this flag.
constexpr bool hasWifiCoprocessor = isEsp32P4 && hasWiFi;

// Only a variant whose SDK fragment enables the EMAC carries Ethernet; the rest get stubs.
#ifdef MM_NO_ETH
constexpr bool hasEthernet = false;
#else
constexpr bool hasEthernet = true;
#endif

// True when the firmware carries an IP stack at all, which is what UDP interop gates on.
constexpr bool hasNetwork = hasWiFi || hasEthernet;

/// False: the controls follow the hardware, so a board without Ethernet shows nothing to configure.
constexpr bool previewsEthernetControls = false;

// True only under emulation, where a saved type would otherwise select hardware that is not there.
#ifdef CONFIG_ETH_USE_OPENETH
constexpr bool ethPhyIsFixed = true;
#else
constexpr bool ethPhyIsFixed = false;
#endif

// Headroom for a per-pixel float algorithm, from the SDK's FPU capability rather than a chip list.
constexpr bool hasHeavyCompute = SOC_CPU_HAS_FPU;

// A whole effect can be compiled out only by `#if`, which a constexpr cannot drive; keep them in step.
#define MM_HEAVY_COMPUTE SOC_CPU_HAS_FPU

/// False: one MAC per chip, so a raw sender has nothing to choose and the control would do nothing.
constexpr bool hasNamedNetInterfaces = false;

/// False on every ESP32, and not by choice: the runtime is a closed binary for Intel and ARM alone.
constexpr bool hasNdi = false;

// True on the P4 alone, mirroring the build symbol, so flag and dependency cannot disagree.
#if defined(CONFIG_MM_HLS)
constexpr bool hasHls = true;
#else
constexpr bool hasHls = false;
#endif
/// True with the same encoder HLS uses: RTSP ships that encoder's frames without muxing them.
constexpr bool hasRtsp = hasHls;
/// False: one hardware encoder, so there is nothing to choose and the control stays hidden.
constexpr bool hasEncoderChoice = false;
/// False: segments live in a PSRAM ring, flash wear buying nothing for a file stale within seconds.
constexpr bool hasFsSegments = false;

#if defined(CONFIG_ETH_USE_SPI_ETHERNET) && !defined(CONFIG_ETH_USE_ESP32_EMAC)
constexpr bool hasEthW5500 = true;
#else
constexpr bool hasEthW5500 = false;
#endif

/// Which Ethernet PHY a board carries; plain integers, which keeps SDK includes out of this header.
enum EthPhyType {
    ethNone    = 0,  ///< no Ethernet on this board, the default
    ethLan8720 = 1,  ///< RMII, generic PHY
    ethIp101   = 2,  ///< RMII, IP101 PHY, P4 only
    ethW5500   = 3,  ///< SPI, an external W5500 module
    ethYt8531  = 4,  ///< RGMII, the S31's 1 Gb PHY
    ethOpeneth = 5,  ///< the emulator's MAC, which is what gives an emulated device a real IP stack
};

/// How a board wires its Ethernet, set at runtime from the catalog; -1 leaves a field unused.
///
/// @moreinfo The RMII fields serve the internal EMAC and the SPI fields a W5500, with the default below seeding an un-provisioned board.
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

// The per-chip default, so a board with no catalog entry still comes up on its usual wiring.
constexpr EthPinConfig ethConfigDefault =
#ifdef CONFIG_ETH_USE_OPENETH
    // The emulator's MAC has no pins and no PHY to address, so every field is unset.
    EthPinConfig{ /*phyType*/ ethOpeneth, /*addr*/ 1, /*mdc*/ -1, /*mdio*/ -1,
                  /*rst*/ -1, /*rmiiClk*/ -1, /*extIn*/ false,
                  /*miso*/ -1, /*mosi*/ -1, /*sck*/ -1, /*cs*/ -1, /*irq*/ -1 };
#else
    isEsp32P4   ? EthPinConfig{ /*phyType*/ ethIp101, /*addr*/ 1, /*mdc*/ 31, /*mdio*/ 52,
                                /*rst*/ 51, /*rmiiClk*/ 50, /*extIn*/ true,
                                /*miso*/ -1, /*mosi*/ -1, /*sck*/ -1, /*cs*/ -1, /*irq*/ -1 }
  : isEsp32S31  ? EthPinConfig{ /*phyType*/ ethYt8531, /*addr*/ -1, /*mdc*/ 5, /*mdio*/ 6,
                                /*rst*/ 7, /*rmiiClk*/ -1, /*extIn*/ false,
                                /*miso*/ -1, /*mosi*/ -1, /*sck*/ -1, /*cs*/ -1, /*irq*/ -1 }
  : isEsp32S3   ? EthPinConfig{ /*phyType*/ ethW5500, /*addr*/ 1, /*mdc*/ -1, /*mdio*/ -1,
                                /*rst*/ -1, /*rmiiClk*/ -1, /*extIn*/ false,
                                /*miso*/ -1, /*mosi*/ -1, /*sck*/ -1, /*cs*/ -1, /*irq*/ -1 }
    // Stated rather than unset: an unset pin reads as free in the map, hiding a clash with an LED lane.
              :   EthPinConfig{ /*phyType*/ ethLan8720, /*addr*/ 0, /*mdc*/ 23, /*mdio*/ 18,
                                /*rst*/ 5, /*rmiiClk*/ 17, /*extIn*/ false,
                                /*miso*/ -1, /*mosi*/ -1, /*sck*/ -1, /*cs*/ -1, /*irq*/ -1 };
#endif  // CONFIG_ETH_USE_OPENETH

/// True on every ESP32 build, the partition layout reserving both app slots unconditionally.
constexpr bool hasOta = true;

/// True on every ESP32: the serial RPC channel is always there, so the listener always runs.
constexpr bool hasImprov = true;

/// @}

} // namespace mm::platform

// Always 0 here: a device uses its own per-ISA backends, validated on hardware rather than by host tests.
#define MM_MOONLIVE_HAS_HOST_JIT 0

// 0 here: a board links only the drivers its silicon runs, keeping the type picker honest.
#define MM_LINKS_ALL_LED_DRIVERS 0
