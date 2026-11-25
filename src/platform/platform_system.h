#pragma once
/// The platform's identity and state: MAC address, chip, SDK and CPU facts, the reset reason and boot record, the log level and the firmware's size.
///
/// One concern of the platform interface ([platform.h](platform.h) gathers them all), so a file includes the concerns it uses and an edit to one rebuilds its users alone.

#include "platform/platform_base.h"   // target flags, MM_NONBLOCKING and the compiler macros
#include <cstdio>   // std::snprintf, for macString

namespace mm::platform {


/// The device's MAC address.
void getMacAddress(uint8_t mac[6]);
/// The MAC as canonical "XX:XX:XX:XX:XX:XX", in a static buffer a ReadOnly control can bind straight to: formatted once, since the MAC is fixed for the device's life.
inline const char* macString() {
    static char buf[18] = {};
    if (!buf[0]) {
        uint8_t m[6];
        getMacAddress(m);
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    return buf;
}
/// The chip this runs on, as a static string.
const char* chipModel();
/// The chip's silicon revision, such as v3.1, read from the chip; empty where the platform reports none.
const char* chipRevision();
/// The chip's package, such as D0WD-V3 or PICO-V3-02 on a classic ESP32, read from the efuses; empty on a chip with one package.
const char* chipPackage();
/// The bytes of PSRAM the chip found at boot, whether in the heap or left unused, as a revision 0 or 1 classic leaves it on a build lacking the cache workaround.
size_t psramFoundBytes();

/// The catalog's generic device model for the system a host runs on, such as "Generic macOS" or "Docker container"; empty on a device, where tooling injects `deviceModel` instead.
const char* hostDeviceModel();
/// The SDK this was built against, as a static string.
const char* sdkVersion();

/// CPU frequency and core count, read from the running hardware so a downclock is visible in the UI.
const char* cpuInfo();
/// The cores this machine runs code on, at least one.
uint8_t cpuCores();

/// PSRAM interface type and clock, such as "octal, 80 MHz", and empty where this build has no PSRAM.
const char* psramType();

/// How the chip reads its program flash and at what clock, such as "QIO, 80 MHz": the mode read from the running hardware, the clock from the build; empty where the host has none to report.
const char* flashMode();

/// Why the device last reset, which the UI reads to flag a crashed prior boot.
const char* resetReason();

/// How long a boot stays up before it counts as good, and its failed boots start again from one.
constexpr uint32_t kBootStableMs = 60 * 1000;

/// How the last boots went, this one included: the switch-on gesture and a crash loop read it.
struct BootRecord {
    uint8_t quickPowerOns = 0;      ///< switch-ons in a row, each within a few seconds of the last
    uint8_t abnormalRestarts = 0;   ///< failed boots in a row: panics, watchdogs, brownouts, and power losses of a boot younger than a minute
    bool booted = false;            ///< this run is a device boot, false in a host process
};
/// How the last boots went, counted once on the first call. The count lives in flash and clears once the device stays up, or on a restart, an update or a deep sleep.
const BootRecord& bootRecord();
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// What bootRecord answers on the host, which has no boot to count.
void setTestBootRecord(const BootRecord& r);
#endif

/// Serial log verbosity, low to high, ordered as syslog and ESP-IDF order it.
enum class LogLevel : uint8_t { None = 0, Error, Warn, Info, Debug, Verbose };
/// Apply a verbosity to the logger and to the KPI-line gate.
void setLogLevel(LogLevel level);

/// Firmware image bytes.
size_t firmwareSize();
/// App partition size, which is the firmware capacity.
size_t firmwarePartition();
/// Total flash chip capacity.
size_t flashChipSize();

/// Restart the device: a hardware reset on ESP32 and Teensy; on a host the program starts again (in place on macOS and Linux, in the same console on Windows, by its sticky service on Android), the newly installed one when an update waits.
[[noreturn]] void reboot();

}  // namespace mm::platform
