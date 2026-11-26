#pragma once
/// The platform's clock: milliseconds and microseconds since boot, a cycle counter and short waits.
///
/// One concern of the platform interface ([platform.h](platform.h) gathers them all), so a file includes the concerns it uses and an edit to one rebuilds its users alone.

#include "platform/platform_base.h"   // target flags, MM_NONBLOCKING and the compiler macros

#include <ctime>

namespace mm::platform {


/// A civil reading a frame can copy. `sampleCivilTime` fills it, outside the render path, because `localtime` takes a lock.
struct CivilReading {
    /// Four-digit year. 1970 until the clock has been sampled.
    int year = 1970;
    /// Month number. January is 1.
    int month = 1;
    /// Day of the month.
    int day = 1;
    /// Hour from midnight.
    int hour = 0;
    /// Minute past the hour.
    int minute = 0;
    /// Second past the minute.
    int second = 0;
    /// Weekday. Sunday is 0.
    int weekday = 0;
};

/// The last civil reading. A frame copies it; the render path does not call `localtime`.
inline CivilReading civilReading_{};

/// The last civil reading, a copy a frame may keep.
inline CivilReading civilReading() MM_NONBLOCKING { return civilReading_; }

/// Fill `out` with the device's local civil time. False when the clock cannot be read.
inline bool localTm(std::tm& out) {
    const std::time_t t = std::time(nullptr);
    out = {};
#if defined(_WIN32)
    return localtime_s(&out, &t) == 0;   // MSVC takes the arguments in the other order
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}

/// Read the device clock into `civilReading_`. Called from the loop that ticks, not from a tick.
inline void sampleCivilTime() {
    std::tm tm{};
    if (!localTm(tm)) return;
    civilReading_.year = tm.tm_year + 1900;
    civilReading_.month = tm.tm_mon + 1;
    civilReading_.day = tm.tm_mday;
    civilReading_.hour = tm.tm_hour;
    civilReading_.minute = tm.tm_min;
    civilReading_.second = tm.tm_sec;
    civilReading_.weekday = tm.tm_wday;
}

/// Milliseconds since boot, the clock every animation is written against.
uint32_t millis() MM_NONBLOCKING;
/// Microseconds since boot, for measuring a tick rather than driving one.
uint32_t micros() MM_NONBLOCKING;
/// Microseconds since boot, 64 bits wide, so it wraps after half a million years: the fleet clock's local time.
uint64_t uptimeUs() MM_NONBLOCKING;

/// Drive `millis()` from a test, on a device too so a scenario run on real hardware can freeze time. Zero restores the real clock, which a test must do in release.
void setTestNowMs(uint32_t ms);

/// CPU cycle counter, safe in an ISR; wraps at 2^32, so callers difference two reads.
uint32_t cycleCount();
/// Blocking sleep; only use outside the hot path.
void delayMs(uint32_t ms);
/// Blocking busy-wait for a sub-ms protocol gap such as the WS2812 latch, up to a few hundred µs.
void delayUs(uint32_t us);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// Move a frozen clock by `ms` on every reading, so a test sees two readings within one tick differ; zero stops it.
void setTestClockStep(uint32_t ms);
#endif

}  // namespace mm::platform
