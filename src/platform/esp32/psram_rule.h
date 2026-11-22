#pragma once

#include <cstdint>

namespace mm::platform {

/// Whether a classic ESP32's PSRAM may serve the heap: revision 3 fixed the cache bug of 0 and 1, a `CONFIG_SPIRAM_CACHE_WORKAROUND` build is safe on all. `chipRevision`: major x 100 + minor.
constexpr bool psramHeapSafe(uint32_t chipRevision, bool cacheWorkaround) {
    return cacheWorkaround || chipRevision >= 300;
}

/// The usable pads a failed PSRAM probe keeps reserved (ESP-IDF reserves them before the ID read): CS 16 and CLK 17 on the D0WD packages; elsewhere they are built-in flash.
constexpr uint64_t psramPadsFreedWhenAbsent(uint32_t package) {
    constexpr uint64_t kGpio16 = uint64_t{1} << 16, kGpio17 = uint64_t{1} << 17;
    switch (package) {
        case 0: case 1: return kGpio16 | kGpio17;   // ESP32-D0WDQ6, -D0WDQ5
        case 7:         return kGpio16;             // ESP32-D0WDR2-V3
        default:        return 0;                   // D2WD, PICO-D4/U4WDH, PICO-V3-02: the pins are the package's flash
    }
}

}  // namespace mm::platform
