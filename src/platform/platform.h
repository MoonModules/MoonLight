#pragma once
/// The one interface every module reaches hardware through, so the same source drives an ESP32, a Teensy, a Raspberry Pi and a host.
///
/// Core and the light domain call these names, and only the platform layer calls a vendor SDK. A module that needs something this interface does not offer gets a new function in the concern header it belongs to rather than a target check at the call site.
///
/// Each concern has its own header, and a file includes the concerns it uses, so an edit to one rebuilds that concern's users alone: the clock, the heap, the tasks, the system's identity, storage, the network, firmware update, video, the LED peripherals, audio and general inputs and outputs. This header gathers them all, for a platform's own implementation.

#include "platform/platform_base.h"    // target flags, MM_NONBLOCKING and the compiler macros
#include "platform/platform_clock.h"   // millis, micros, delays
#include "platform/platform_heap.h"    // alloc, PSRAM, the heap figures
#include "platform/platform_tasks.h"   // the task identity, pinned workers, the task snapshot
#include "platform/platform_system.h"  // MAC, chip, boot record, log level
#include "platform/platform_fs.h"      // the filesystem and the SD card
#include "platform/platform_net.h"     // Ethernet, WiFi, mDNS, sockets
#include "platform/platform_ota.h"     // firmware update and MoonBase
#include "platform/platform_video.h"   // NDI, HDMI, the encoder, video capture
#include "platform/platform_leds.h"    // PWM, RMT, the parallel buses, HUB75
#include "platform/platform_audio.h"   // the codec and the microphone
#include "platform/platform_io.h"      // GPIO, ADC, infrared, I2C, USB MIDI
