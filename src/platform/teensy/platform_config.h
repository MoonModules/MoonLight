#pragma once

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
/// The Teensy 4.1: its built-in microSD socket is a board preset the SDK flags cannot name.
constexpr bool isTeensy41 = true;

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

/// No microphone input.
constexpr bool hasI2sMic = false;
/// No capture device.
constexpr bool hasAudioCapture = false;

/// No WiFi on a Teensy.
constexpr bool hasWiFi = false;

/// The on-board PHY, or on a board without one a W5500 on the SPI port, found at boot.
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
/// No USB MIDI host.
constexpr bool hasUsbMidiHost = false;
/// One encoder at most.
constexpr bool hasEncoderChoice = false;
/// No segment files.
constexpr bool hasFsSegments = false;
/// Whether any IP stack is present, which is what UDP interop gates on.
constexpr bool hasNetwork = hasWiFi || hasEthernet;

/// True: the firmware finds the interface at boot, the board's own PHY or else a W5500, so the wiring is not a setting.
constexpr bool ethPhyIsFixed = true;

/// A 600 MHz core with a double-precision FPU has room for a per-pixel float algorithm.
constexpr bool hasHeavyCompute = true;

// A whole effect can be compiled out only by `#if`, which a constexpr cannot drive; keep the two in step.
#define MM_HEAVY_COMPUTE 1
/// True: the W5500 driver is compiled in beside the on-board PHY's, and used on a board that has no PHY of its own.
constexpr bool hasEthW5500 = true;
/// The W5500's wiring, used only on a board without its own PHY: MISO 12, MOSI 11, SCK 13 and chip select 10, reset and interrupt unwired.
constexpr EthPinConfig ethConfigDefault{ ethW5500, 0, -1, -1, -1, -1, false, 12, 11, 13, 10, -1 };

/// False: no WiFi co-processor.
constexpr bool hasWifiCoprocessor = false;

/// True: an uploaded image is staged above the program and copied over it at the reboot.
constexpr bool hasOta = true;

/// True: the USB serial port takes the installer's state documents (APPLY_OP); there is no WiFi to provision.
constexpr bool hasImprov = true;

/// True: the 4.1's built-in microSD socket, on the chip's own SDIO host.
constexpr bool hasSdCard = true;
/// False: SdCardService offers only the socket this board wires.
constexpr bool previewsSdPresets = false;
/// No decoder linked on Teensy; see src/platform/esp32/platform_config.h for the real gate.
constexpr bool hasZstd = false;

/// The bits a call to emitted code adds to its address. Thumb state: a Cortex-M7 runs only Thumb code, and a call address carries it in bit 0.
constexpr uintptr_t codeAddressBits = 1;

} // namespace mm::platform

// 0: the Thumb-2 backend runs on the board, and the host tests exercise it through its own test file.
#define MM_MOONLIVE_HAS_HOST_JIT 0

// 0: only the drivers this target can drive are linked.
#define MM_LINKS_ALL_LED_DRIVERS 0

// The timer-paced GPIO DMA LED output, this chip's parallel output, a capability the driver registry gates on as an ESP32's CONFIG_SOC_* does.
#define MM_GPIO_DMA_LEDS 1
