#pragma once

/// @defgroup platform_config_desktop Desktop platform configuration
/// @{
/// What the host build claims to be: generous memory, every driver linked, no real silicon.
///
/// @moreinfo
///
/// ## The lane counts are not zero
///
/// A zero makes every parallel backend report "not my silicon", so the driver idles and its body never runs off-device: not runnable, not unit-testable, invisible to every check.
/// Sixteen is the widest real rig, so the host exercises the same lane-splitting arithmetic the hardware does.
/// The bus behind them is a heap buffer, holding real WS2812 patterns in real memory, with only the DMA hand-off absent.
///
/// ## Types exist even where the feature does not
///
/// The platform header declares one signature per seam for every target, so the Ethernet and codec types must resolve here even though a host has neither.
/// The stubs ignore them, and shared code gates on the capability flags instead.

#include <cstdint>

// Desktop code always executes from RAM, so the ISR-residency attribute is empty here.
#define MM_RAMFUNC

namespace mm::platform {

/// A host has memory to spare, so it answers the PSRAM question the generous way.
constexpr bool hasPsram = true;

/// Not a P4, so its Ethernet pin map and co-processor WiFi compile out.
constexpr bool isEsp32P4 = false;
/// Not an S3, so its W5500 Ethernet default compiles out.
constexpr bool isEsp32S3 = false;
/// Not an S31, so its RGMII PHY compiles out.
constexpr bool isEsp32S31 = false;

/// RMT channels the host reports, matching the S3 and P4 count rather than the classic chip's eight.
constexpr uint8_t rmtTxChannels = 4;

/// One pad an EMAC's data interface owns, which a host has none of.
struct EthFixedPad { const char* name; uint8_t gpio; };
/// A single dummy entry: a zero-size array is an extension MSVC refuses, and Windows CI compiles this.
constexpr EthFixedPad ethFixedPads[] = {{"", 0}};
/// None, a host's Ethernet being a named interface rather than wired signals.
constexpr uint8_t ethFixedPadCount = 0;

/// Parallel lanes the host reports, deliberately not zero.
constexpr uint8_t lcdLanes = 16;

/// True on the host, which emulates the peripheral rather than declaring itself incapable.
constexpr bool hasLcdCam = true;
/// Parlio lanes the host reports, on the same reasoning as lcdLanes.
constexpr uint8_t parlioLanes = 16;

/// Zero, because the i80 backend sums this with lcdLanes and would otherwise claim 32 lanes.
constexpr uint8_t i2sLanes = 0;

/// No pin-wired microphone on a host; live audio arrives through capture devices instead.
constexpr bool hasI2sMic = false;

// No USB host on desktop; VideoService keeps its test-pattern and file sources.
constexpr bool hasUsbVideo = false;

/// OS capture devices, reaching the same audioMicRead seam a wired microphone would.
constexpr bool hasAudioCapture = true;

/// Which codec sits in front of the microphone; a host has none.
enum class CodecType : uint8_t { None = 0, Es8311 = 1 };
/// How a codec is reached, mirrored from the ESP32 config so the names resolve everywhere.
struct AudioCodecPins { uint16_t i2cSda; uint16_t i2cScl; uint16_t mclk; uint8_t i2cAddr; };
/// None, a host having no codec to configure.
constexpr CodecType audioCodecType = CodecType::None;
/// Unused, there being no codec to reach.
constexpr AudioCodecPins audioCodecPins = { 0, 0, 0, 0 };

/// True, so the host compiles the WiFi path even though it ships stubs behind it.
constexpr bool hasWiFi = true;

/// Which Ethernet PHY a board carries; kept in step with the ESP32 list so every name resolves here.
enum EthPhyType { ethNone = 0, ethLan8720 = 1, ethIp101 = 2, ethW5500 = 3,
                  ethYt8531 = 4, ethOpeneth = 5 };
/// How that PHY is wired, mirrored from the ESP32 struct; the host stub ignores it.
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
/// False, so shared code compiles its Ethernet controls out and seeds itself from the default below.
constexpr bool hasEthernet = false;

/// True: the Ethernet CONTROLS are built though no interface exists, so the board presets can be exercised without a device. A preview of a device-only card, tagged developer-mode and never consulted by a bring-up path, which stays keyed on hasEthernet.
constexpr bool previewsEthernetControls = true;

/// True: a host has several NICs, so a raw sender must name the one it binds.
constexpr bool hasNamedNetInterfaces = true;

/// True: a host can be an NDI video source.
constexpr bool hasNdi = true;

/// True: the host streams H.264 by piping frames to the ffmpeg on PATH, a dependency of the user's.
constexpr bool hasHls = true;

/// True: ffmpeg encodes the elementary stream and MoonLight's own RTSP server ships it.
constexpr bool hasRtsp = true;
/// True: ffmpeg offers several encoders, so the pick is the user's.
constexpr bool hasEncoderChoice = true;
/// True: ffmpeg writes the playlist to disk, so the server serves segments as files.
constexpr bool hasFsSegments = true;
/// Whether any IP stack is present, which is what UDP interop gates on.
constexpr bool hasNetwork = hasWiFi || hasEthernet;

/// False: a host has no Ethernet to fix, so nothing overrides a catalog-supplied PHY type.
constexpr bool ethPhyIsFixed = false;

/// Headroom for a per-pixel float algorithm, which a desktop CPU has to spare.
constexpr bool hasHeavyCompute = true;

// A whole effect can be compiled out only by `#if`, which a constexpr cannot drive; keep the two in step.
#define MM_HEAVY_COMPUTE 1
/// False: no SPI-Ethernet driver here, which the live-reconfigure path gates on.
constexpr bool hasEthW5500 = false;
/// No Ethernet to seed, so every field is unset.
constexpr EthPinConfig ethConfigDefault{ ethNone, 0, -1, -1, -1, -1, false, -1, -1, -1, -1, -1 };

/// False: no separate WiFi co-processor, so its read-out and control compile out.
constexpr bool hasWifiCoprocessor = false;

/// False: no OTA partition to write, so the firmware routes answer a stub instead.
constexpr bool hasOta = false;

/// False: no UART and no WiFi stack, so the provisioning listener is never installed.
constexpr bool hasImprov = false;

/// @}

} // namespace mm::platform

// 1 on arm64 and x86-64; any other host ISA falls through and compiles scripts to a clean failure.
#if (defined(__aarch64__) || defined(__x86_64__) || defined(_M_X64)) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)
    #define MM_MOONLIVE_HAS_HOST_JIT 1
#else
    #define MM_MOONLIVE_HAS_HOST_JIT 0
#endif

// 1 here: a driver missing from the host binary cannot be unit-tested and runs only on hardware.
#define MM_LINKS_ALL_LED_DRIVERS 1
