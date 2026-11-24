// SPDX-License-Identifier: GPL-3.0-or-later

/// The test capture for the stub GPIO backend: a thread-safe copy of the last submitted GPIO frame, its dump stream, a submit count and the setup-step log, so an engine test runs on any host.
///
/// The test reads back golden words, pins the dump sequence the hardware backend would drive, and pins the dark-first setup order; all accessors take the capture lock, since the refresh thread submits while the test reads.
///
/// The dump stream is every GPIO write and pulser step the shared dump loop made, in order (@xref{dump-stream|Dump stream}); the setup log is the steps open() and start() ran (@xref{setup-log|Setup log}).
///
/// @technical
///
/// ## Dump stream
///
/// The hardware backend runs the same dump loop against the registers, so the stream is the sequence the panel would have seen: the test decodes it into clock edges, strobes, address changes and OE pulses.
///
/// ## Setup log
///
/// The log is the setup steps the stub's open() and start() ran, in order, comma separated: "dark-values,directions,pwm-config,oe-mux" for a full open + start. The hardware backend runs the same four steps in the same order, which the bench shows; the engine test pins this string for the stub.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "platform/rpi/hub75_gpio_dump.h"   // GpioDumpEvent

namespace mm::platform {

/// The frames the stub has submitted since the last reset.
uint64_t gpioStubSubmittedFrames();

/// Copies the last submitted frame's GPIO words into `out` (at most `capWords`); returns the words copied, 0 on an empty capture.
size_t gpioStubCopyLastFrame(uint32_t* out, size_t capWords);

/// Copies the last submitted frame's dump stream (@xref{dump-stream|Dump stream}) into `out` (at most `capEvents`); returns the events copied, 0 on an empty capture.
size_t gpioStubCopyLastDumpStream(GpioDumpEvent* out, size_t capEvents);

/// The setup steps the stub's open() and start() ran, in order, comma separated (@xref{setup-log|Setup log}).
std::string gpioStubSetupLog();

/// Drops the capture, the count and the setup log.
void gpioStubResetCapture();

}  // namespace mm::platform
