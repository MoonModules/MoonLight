#pragma once
/// The platform's clock: milliseconds and microseconds since boot, a cycle counter and short waits.
///
/// One concern of the platform interface ([platform.h](platform.h) gathers them all), so a file includes the concerns it uses and an edit to one rebuilds its users alone.

#include "platform/platform_base.h"   // target flags, MM_NONBLOCKING and the compiler macros

namespace mm::platform {


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
