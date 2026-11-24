// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// What machine the program runs on, as far as a peripheral choice needs it: an ESP32 (known when the firmware is built), a Raspberry Pi 5, an earlier Raspberry Pi, or any other host.
///
/// A host reads it once from the kernel's device tree, `/proc/device-tree/compatible`, which lists the board's names as NUL-separated strings (`raspberrypi,5-model-b`, `raspberrypi,4-model-b`, with the SoC after them). Where that file cannot be read, as in a container that masks it, the `Model` line of `/proc/cpuinfo` answers instead (@xref{the-two-sources|the two sources}).
///
/// A caller decides by a case tree over the kind, one case per machine and no default, so a machine that has no output for a peripheral says so instead of falling to another (@xref{what-counts-as-a-pi|what counts as a Pi}).
///
/// @technical
///
/// ## What counts as a Pi
///
/// A string that starts `raspberrypi,5` or `brcm,bcm2712` is a Pi 5, including the compute module and the Pi 500. Any other string that starts `raspberrypi,` or `brcm,bcm27` or `brcm,bcm28` is an earlier Pi. A host that has no such file, or a board with any other string, is `Other`: macOS, Windows, a PC, or a Linux board that is not a Pi.
///
/// ## The two sources
///
/// The device tree wins whenever it can be read, whatever it names. Only a host that cannot read it falls to the cpuinfo `Model` line (`Raspberry Pi 5 Model B Rev 1.0`, `Raspberry Pi Compute Module 5 Rev 1.0`, `Raspberry Pi 500 Rev 1.0`).
///
/// A line that starts `Raspberry Pi` and names a 5, a 500 or a Compute Module 5 is a Pi 5, any other `Raspberry Pi` line an earlier Pi. Both sources are the kernel's, so a container that hides both, which Docker's defaults do not, reads as `Other`.

#include <cstddef>
#include <cstdint>

namespace mm::platform {

/// The kinds of machine the peripheral choices tell apart.
enum class Machine : uint8_t {
    Esp32,    ///< an ESP32 of any chip, known at compile time
    Pi5,      ///< a Raspberry Pi 5 or its compute module, whose header the RP1's PIO drives
    PiGpio,   ///< a Raspberry Pi 1 to 4 or Zero, whose header the CPU's GPIO drives
    Other,    ///< any other host, or a board that is not a Pi
};

/// The machine the `Model` line of a `/proc/cpuinfo` text names: `text` is `length` bytes, null or empty for none. Pure, so a test reads every case.
Machine machineFromCpuinfo(const char* text, size_t length);

/// The SoC of a Pi 3 or 4, which the GPIO output drives through its registers: the peripherals sit at another base, the PWM clock runs at another rate and the GPIO wants another slowdown on each.
enum class PiSoc : uint8_t {
    Other,     ///< not a Pi 3 or 4: another board, a Pi 5, or a Pi older than the 3
    Bcm2837,   ///< a Pi 3, a Pi Zero 2 or a compute module 3
    Bcm2711,   ///< a Pi 4, a Pi 400 or a compute module 4
};

/// The SoC a device tree's `compatible` property names, as machineFromCompatible takes it. Pure, so a test reads every case.
PiSoc piSocFromCompatible(const char* data, size_t length);

/// The SoC a `/proc/cpuinfo` text's `Model` line names, where the device tree cannot be read. Pure, so a test reads every case.
PiSoc piSocFromCpuinfo(const char* text, size_t length);

/// The machine a device tree's `compatible` property names: `data` is `length` bytes of NUL-separated strings, null or empty for none. Pure, so a test reads every case.
Machine machineFromCompatible(const char* data, size_t length);

#ifdef ESP_PLATFORM
/// An ESP32 build is an ESP32.
constexpr Machine machine() { return Machine::Esp32; }
#else
/// The machine this host is: what a test set, else the real host.
Machine machine();
/// The real host, read once from the device tree or cpuinfo, whatever a test set: a test that wants the machine it runs on asks for it here.
Machine hostMachine();
/// The SoC of this host: what a test set, else the real host's, which the device tree or cpuinfo names with the machine.
PiSoc piSoc();
/// Make piSoc() answer `soc`, whatever the host is; a test binary rests it at `Other`, as it rests the machine.
void setTestPiSoc(PiSoc soc);
/// Make machine() answer `kind`, whatever the host is, until clearTestMachine().
void setTestMachine(Machine kind);
/// Return machine() to its resting answer: the real host, or the kind setTestMachineRest() named.
void clearTestMachine();
/// Name the answer machine() rests at, so a test binary is `Other` on every machine and no case depends on the board it runs on; none, the default, rests at the real host.
void setTestMachineRest(Machine kind);
#endif

}  // namespace mm::platform
