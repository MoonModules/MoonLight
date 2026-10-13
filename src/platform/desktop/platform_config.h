#pragma once

/// @defgroup platform_config_desktop Desktop platform configuration
/// @{
/// What the host build claims to be: generous memory, every driver linked, no real silicon, with @xref{the-lane-counts-are-not-zero|lanes to drive} and @xref{types-exist-even-where-the-feature-does-not|every seam's types}.
///
/// @moreinfo
///
/// ## The lane counts are not zero
///
/// A zero makes every parallel backend report "not my silicon", so the driver idles and its body never runs off-device: not runnable, not unit-testable, invisible to every check.
///
/// Sixteen is the widest real rig, so the host exercises the same lane-splitting arithmetic the hardware does. The bus behind them is a heap buffer, holding real WS2812 patterns in real memory, with only the DMA hand-off absent.
///
/// ## Types exist even where the feature does not
///
/// The platform header declares one signature per seam for every target, so the Ethernet and codec types must resolve here even though a host has neither. The stubs ignore them, and shared code gates on the capability flags instead.

#include <cstdint>

#include "platform/eth_config.h"   // EthPhyType, EthPinConfig

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
/// Not a Teensy, so its SD socket preset only shows as a preview.
constexpr bool isTeensy41 = false;

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
/// GPIO-DMA lanes the host reports, on the same reasoning as lcdLanes.
constexpr uint8_t gpioDmaLanes = 16;
/// The Teensy's chain capacity, so the host sizes the same frames the board refuses.
constexpr uint32_t gpioDmaMaxSlots = 8 * 32767;

/// No pin-wired microphone on a host; live audio arrives through capture devices instead.
constexpr bool hasI2sMic = false;

/// OS capture devices, reaching the same audioMicRead seam a wired microphone would.
constexpr bool hasAudioCapture = true;

/// True, so the host compiles the WiFi path even though it ships stubs behind it.
constexpr bool hasWiFi = true;

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
/// False: a desk on the computer reaches the MIDI service through the browser.
constexpr bool hasUsbMidiHost = false;
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

/// The bits a call to emitted code adds to its address. None: a host call takes the code address as it is.
constexpr uintptr_t codeAddressBits = 0;

/// True: unlike every other board gate here, the mount/remount/format logic under test needs a real host path, not a stub.
constexpr bool hasSdCard = true;
/// True: no slot is wired, so SdCardService offers every board preset, for the tests to exercise them.
constexpr bool previewsSdPresets = true;

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
