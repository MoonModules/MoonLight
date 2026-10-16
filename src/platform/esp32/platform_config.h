#pragma once

/// What each ESP32 chip can do, derived from @xref{capabilities-not-chip-names|the SDK's own capability flags} wherever possible, with @xref{the-i80-gate-names-lcd-cams-own-mode|the i80 gate} and @xref{fixed-pads-are-published|the fixed pads} the exceptions.
///
/// @technical
///
/// ## Capabilities, not chip names
///
/// Gating keys off the SDK's own flags, so a new chip works untouched and only a seam the flags cannot answer earns an `is<Chip>` constant. Three chips earn one: the P4 and S3 for their Ethernet defaults, and the S31 for its RGMII PHY.
///
/// ## The i80 gate names LCD_CAM's own mode
///
/// The classic chip sets the broader flag for an unrelated peripheral. Gating on it once wired the parallel driver onto that chip and hung its boot on a bus it lacks.
///
/// ## Fixed pads are published
///
/// The EMAC's data pads are silicon's choice, so one list serves both the driver init and the pin map. A pad nobody declares reads as free while the MAC drives it, so an LED lane there corrupts every frame while the link reports 1 Gb.
///
/// ## An image without the room turns it off
///
/// `CONFIG_SOC_SDMMC_HOST_SUPPORTED` names every chip with the SD host, and `CONFIG_MM_SD_CARD` is the second, project-level gate, the same two-gate shape `CONFIG_MM_USB_MIDI_HOST` uses. It is on by default and off on every 4 MB image, through the 4 MB layout's fragment.

#include "sdkconfig.h"

#include <cstdint>

#include "platform/eth_config.h"   // EthPhyType, EthPinConfig

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
/// Not a Teensy, so its SD socket preset stays out of the list.
constexpr bool isTeensy41 = false;

/// One pad the EMAC's data interface owns: the signal it carries, and the GPIO silicon fixed it to.
///
/// @technical Neither RGMII on the S31 nor RMII on the P4 is configurable, so these are reported as fixed pins, not controls.
/// One list serves the driver init and the pin map, so the MAC's wiring and the map's label cannot drift apart. A pad nobody declares reads as free while the MAC drives it, so an LED lane there corrupts every frame while the link reports 1 Gb.
struct EthFixedPad { const char* name; uint8_t gpio; };
#ifdef CONFIG_IDF_TARGET_ESP32S31
constexpr EthFixedPad ethFixedPads[] = {
    {"ethTxd0",  8}, {"ethTxd1",  9}, {"ethTxd2", 10}, {"ethTxd3", 11},
    {"ethTxCtl", 12}, {"ethTxClk", 13}, {"ethRxClk", 14}, {"ethRxCtl", 15},
    {"ethRxd3", 16}, {"ethRxd2", 17}, {"ethRxd1", 18}, {"ethRxd0", 19},
};
constexpr uint8_t ethFixedPadCount = 12;
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
constexpr EthFixedPad ethFixedPads[] = {
    // P4 RMII data lines; the management pair stays a routable control.
    {"ethTxEn", 49}, {"ethTxd0", 34}, {"ethTxd1", 35},
    {"ethCrsDv", 28}, {"ethRxd0", 29}, {"ethRxd1", 30},
};
constexpr uint8_t ethFixedPadCount = 6;
#elif defined(CONFIG_IDF_TARGET_ESP32)
constexpr EthFixedPad ethFixedPads[] = {
    // Classic RMII: one pad choice per signal, so these are the pins rather than a default.
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
constexpr EthFixedPad ethFixedPads[] = {{"", 0}};   // nothing fixed to publish: a single dummy entry, a zero-size array being an extension MSVC refuses
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

/// Separate from the lane count because the host reports lanes while having no such silicon.
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

/// False: capture devices are a host concept, and a board uses the wired microphone above.
constexpr bool hasAudioCapture = false;


// The Ethernet-only profile drops the WiFi components and defines this, there being no SDK switch.
#ifdef MM_NO_WIFI
constexpr bool hasWiFi = false;
#else
constexpr bool hasWiFi = true;
#endif

/// The P4's WiFi runs on a co-processor behind an identical API, so only its read-out needs this flag.
constexpr bool hasWifiCoprocessor = isEsp32P4 && hasWiFi;

// Only a variant whose SDK fragment enables the EMAC carries Ethernet; the rest get stubs.
#ifdef MM_NO_ETH
constexpr bool hasEthernet = false;
#else
constexpr bool hasEthernet = true;
#endif

/// True when the firmware carries an IP stack at all, which is what UDP interop gates on.
constexpr bool hasNetwork = hasWiFi || hasEthernet;

/// False: the controls follow the hardware, so a board without Ethernet shows nothing to configure.
constexpr bool previewsEthernetControls = false;

// True only under emulation, where a saved type would otherwise select hardware that is not there.
#ifdef CONFIG_ETH_USE_OPENETH
constexpr bool ethPhyIsFixed = true;
#else
constexpr bool ethPhyIsFixed = false;
#endif

// Only `#if` can compile out a whole effect; the SDK defines the FPU capability only on chips that have one, so the C3 leaves it undefined.
#if defined(SOC_CPU_HAS_FPU) && SOC_CPU_HAS_FPU
#define MM_HEAVY_COMPUTE 1
#else
#define MM_HEAVY_COMPUTE 0
#endif

/// Headroom for a per-pixel float algorithm, from the SDK's FPU capability rather than a chip list.
constexpr bool hasHeavyCompute = MM_HEAVY_COMPUTE;

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

// True on the images that set the build symbol, mirroring it so flag and dependency cannot disagree.
#if defined(CONFIG_MM_USB_MIDI_HOST)
constexpr bool hasUsbMidiHost = true;
#else
constexpr bool hasUsbMidiHost = false;
#endif
/// False: one hardware encoder, so there is nothing to choose and the control stays hidden.
constexpr bool hasEncoderChoice = false;
/// False: segments live in a PSRAM ring, flash wear buying nothing for a file stale within seconds.
constexpr bool hasFsSegments = false;

#if defined(CONFIG_ETH_USE_SPI_ETHERNET) && !defined(CONFIG_ETH_USE_ESP32_EMAC)
constexpr bool hasEthW5500 = true;
#else
constexpr bool hasEthW5500 = false;
#endif


/// The per-chip default, so a board with no catalog entry still comes up on its usual wiring.
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

/// The bits a call to emitted code adds to its address. None: Xtensa and RISC-V take the code address as it is.
constexpr uintptr_t codeAddressBits = 0;

/// Chip capability AND room in the image: @xref{an-image-without-the-room-turns-it-off|why the second gate}.
#if defined(CONFIG_SOC_SDMMC_HOST_SUPPORTED) && defined(CONFIG_MM_SD_CARD)
constexpr bool hasSdCard = true;
#else
constexpr bool hasSdCard = false;
#endif
/// False: SdCardService offers only the board presets this chip can wire.
constexpr bool previewsSdPresets = false;

/// No chip gate: zstd is software-only, so the Kconfig flag alone decides.
#if defined(CONFIG_MM_FSEQ_ZSTD)
constexpr bool hasZstd = true;
#else
constexpr bool hasZstd = false;
#endif

} // namespace mm::platform

// Always 0 here: a device uses its own per-ISA backends, validated on hardware rather than by host tests.
#define MM_MOONLIVE_HAS_HOST_JIT 0

// 0 here: a board links only the drivers its silicon runs, keeping the type picker honest.
#define MM_LINKS_ALL_LED_DRIVERS 0
