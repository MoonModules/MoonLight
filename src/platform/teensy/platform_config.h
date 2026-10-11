#pragma once

/// @defgroup platform_config_teensy Teensy 4.1 platform configuration
/// @{
/// What a Teensy 4.1 offers: one Cortex-M7 at 600 MHz, 1 MB of RAM, no WiFi, no RTOS; a feature without its seam compiles out as on a desktop.

#include <cstdint>

#include "platform/eth_config.h"   // EthPhyType, EthPinConfig

// Empty, so the LED encode runs from flash through the 32 KB instruction cache; the 8- and 16-lane measurements decide whether it moves to ITCM.
#define MM_RAMFUNC

// One thread and no TLS sections in PJRC's linker script, so per-thread state is plain static state.
#define MM_THREAD_LOCAL static

namespace mm::platform {

/// False: the PSRAM pads are empty on the bench board.
constexpr bool hasPsram = false;

/// Not a P4, so its Ethernet pin map and co-processor WiFi compile out.
constexpr bool isEsp32P4 = false;
/// Not an S3, so its W5500 Ethernet default compiles out.
constexpr bool isEsp32S3 = false;
/// Not an S31, so its RGMII PHY compiles out.
constexpr bool isEsp32S31 = false;

/// No RMT peripheral on the i.MX RT1062.
constexpr uint8_t rmtTxChannels = 0;

/// One pad an EMAC's data interface owns, which the W5500 path has none of.
struct EthFixedPad { const char* name; uint8_t gpio; };
/// A single dummy entry, since a zero-size array is not standard C++.
constexpr EthFixedPad ethFixedPads[] = {{"", 0}};
/// None.
constexpr uint8_t ethFixedPadCount = 0;

/// No LCD_CAM, Parlio or I2S parallel output; the Teensy's own wide output is GPIO-DMA.
constexpr uint8_t lcdLanes = 0;
/// False: no LCD_CAM peripheral.
constexpr bool hasLcdCam = false;
/// No Parlio peripheral.
constexpr uint8_t parlioLanes = 0;
/// No I2S parallel output.
constexpr uint8_t i2sLanes = 0;
/// One DMA channel writes all sixteen lanes, any pins of GPIO1/GPIO6.
constexpr uint8_t gpioDmaLanes = 16;
/// The slots one frame's chain of DMA descriptors carries: 8 descriptors of 32767, over 3,600 RGB lights per lane.
constexpr uint32_t gpioDmaMaxSlots = 8 * 32767;

/// No microphone seam yet.
constexpr bool hasI2sMic = false;
/// No capture device.
constexpr bool hasAudioCapture = false;

/// No WiFi on a Teensy.
constexpr bool hasWiFi = false;

/// The on-board PHY, or a W5500 on the SPI port in the `teensy41-w5500` firmware.
constexpr bool hasEthernet = true;
/// False: no Ethernet controls without an interface.
constexpr bool previewsEthernetControls = false;
/// False: one interface, so a raw sender names none.
constexpr bool hasNamedNetInterfaces = false;

/// No NDI, HLS or RTSP source.
constexpr bool hasNdi = false;
/// No H.264 encoder.
constexpr bool hasHls = false;
/// No RTSP server.
constexpr bool hasRtsp = false;
/// No USB MIDI host yet.
constexpr bool hasUsbMidiHost = false;
/// One encoder at most.
constexpr bool hasEncoderChoice = false;
/// No segment files.
constexpr bool hasFsSegments = false;
/// Whether any IP stack is present, which is what UDP interop gates on.
constexpr bool hasNetwork = hasWiFi || hasEthernet;

/// True: each firmware drives one interface, chosen at build time, so the wiring is not a setting.
constexpr bool ethPhyIsFixed = true;

/// A 600 MHz core with a double-precision FPU has room for a per-pixel float algorithm.
constexpr bool hasHeavyCompute = true;

// A whole effect can be compiled out only by `#if`, which a constexpr cannot drive; keep the two in step.
#define MM_HEAVY_COMPUTE 1
// QNEthernet's own driver switch, which the build sets for the `teensy41-w5500` firmware and leaves unset for the on-board PHY.
#ifdef QNETHERNET_DRIVER_W5500
/// The W5500 driver is compiled in.
constexpr bool hasEthW5500 = true;
/// The SPI port's pins (MISO 12, MOSI 11, SCK 13) and the driver's chip select 10; reset and interrupt unwired.
constexpr EthPinConfig ethConfigDefault{ ethW5500, 0, -1, -1, -1, -1, false, 12, 11, 13, 10, -1 };
#else
/// False: the on-board PHY, through the chip's own Ethernet MAC.
constexpr bool hasEthW5500 = false;
/// The on-board PHY, whose RMII lines reach no header pin.
constexpr EthPinConfig ethConfigDefault{ ethDp83825, 0, -1, -1, -1, -1, false, -1, -1, -1, -1, -1 };
#endif

/// False: no WiFi co-processor.
constexpr bool hasWifiCoprocessor = false;

/// False: updates go over USB until FlasherX.
constexpr bool hasOta = false;

/// False: no serial provisioning; the Teensy has no WiFi to provision.
constexpr bool hasImprov = false;

/// The bits a call to emitted code adds to its address. Thumb state: a Cortex-M7 runs only Thumb code, and a call address carries it in bit 0.
constexpr uintptr_t codeAddressBits = 1;

/// @}

} // namespace mm::platform

// 0: the Thumb-2 backend runs on the board, and the host tests exercise it through its own test file.
#define MM_MOONLIVE_HAS_HOST_JIT 0

// 0: only the drivers this target can drive are linked.
#define MM_LINKS_ALL_LED_DRIVERS 0
