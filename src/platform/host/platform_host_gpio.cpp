// SPDX-License-Identifier: GPL-3.0-or-later
/// The Raspberry Pi 3/4 platform layer behind the GPIO seam (Hub75Gpio.h), on every host: a recorder and the real engine on its stub backend for the tests, and the engine on the hardware backend where the build has it (Linux aarch64).
///
/// A build that lacks the hardware backend refuses every open outside a test mode with that reason, so a GPIO output says why its panels stay dark. A build with it opens the engine, which drives the panels from its own encode and refresh threads (CPU bit-bang + PWM OE pulser; hub75_gpio_engine.h says why it bit-bangs from the CPU instead of streaming by DMA).
///
/// @technical
///
/// ## Availability
///
/// A test mode offers the backend on every host, and a test hook forces it either way; otherwise the host decides: the hardware backend built in AND a Pi 3 or 4 under it, the same refusal the open checks. The /dev/mem access and the audio-driver conflict are checked at open, so their reasons reach the status line and the backend stays offered.
///
/// ## Test modes
///
/// `Hub75GpioMode::Record` stands a recorder in for the panels: an open keeps its options and a push keeps its frame. `Hub75GpioMode::Stub` opens the real engine on the stub backend through the same branch a Pi takes, so a test drives the seam into the engine, its threads and its shutdown on any host. Both skip the host check, since no hardware is involved.
///
/// ## One task
///
/// The seam's one-task rule is in Hub75Gpio.h. `realOpen_` is atomic so that the push and fps readers see a whole value; the rule, not the atomic, keeps a push from overlapping a close. (The engine's own publish guard keeps a push from writing into freed buffers even then.)
///
/// The rule holds because of two behaviors in core: `Drivers::prepare` joins the encode worker before its children prepare, and the HUB75 driver re-prepares the whole tree on a control change (platform_host_pio.cpp, One task).

#include "platform/Hub75Gpio.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "platform/host/hub75_recorder.h"
#include "platform/machine.h"                        // piSoc: the SoC the slowdown resolves by
#include "platform/rpi/hub75_gpio_backend.h"   // the refusals, the gate and the column-time model: plain C++, so it builds on every host
#include "platform/rpi/hub75_gpio_multiplex.h"   // the scan and the driven width the seam answers
#include "platform/rpi/hub75_gpio_options.h"   // hub75GpioCheckOptions, which the recorder refuses on
#ifdef MM_HUB75_GPIO_HAS_ENGINE
    #include "platform/rpi/hub75_gpio_backend_stub.h"
    #include "platform/rpi/hub75_gpio_engine.h"
#endif

namespace mm::platform {

namespace {

Hub75Recorder rec_;   // the stand-in for the panels, which a test reads
std::string lastError_;
std::string warning_;      // what degrades the running panels, empty while the recorder holds them
std::string closeError_;   // what the last close reported: a stuck thread or a failed stop, empty when it finished clean

Hub75GpioMode mode_ = Hub75GpioMode::Off;
int testAvailable_ = -1;   // the test hook's forced availability: -1 the host decides, 0 forced unavailable, 1 forced available
Hub75GpioOptions testOptions_;   // the options of the last successful open
Hub75GpioTestPort testPort_;     // what hub75GpioTestPort() fills and returns
uint32_t opens_ = 0;             // successful opens since the mode was set
uint32_t closes_ = 0;            // closes of an open seam since the mode was set
uint16_t testFps_ = 0;           // what the recorder answers for the rate
uint32_t testSubmitErrors_ = 0;  // what the recorder answers for the failed dumps
bool testStalled_ = false;       // what the recorder answers for a stalled dump
uint32_t realAttempts_ = 0;      // opens outside a test mode that were asked for
bool testOpenFails_ = false;     // whether a test mode refuses the open
std::atomic<uint8_t> carryBits_{kHub75GpioCarryBitsDefault};   // the last carry bits handed over

#ifdef MM_HUB75_GPIO_HAS_ENGINE
// The engine, one per process, lives until the process ends: a refresh thread stuck in a dump may still be reading it at exit. A normal exit still blanks the panels, through the driver's release.
Hub75GpioEngine& engine_ = *new Hub75GpioEngine;
bool gateOpen() { return gpioBackendGateOpen(); }
void setGateOpen(bool open) { setGpioBackendGateOpen(open); }
uint32_t realOpensNow() { return gpioBackendRealOpens(); }
bool realtimeFailed() { return engine_.realtimeFailed(); }
#else
// A build that omits the engine (MSVC): the same calls land on an engine whose every open refuses, so the seam keeps one code path; the recorder refuses what the engine would, and Stub mode refuses with the reason.
struct NoEngine : Hub75NoEngine<Hub75GpioOptions> {
    NoEngine() : Hub75NoEngine("this build has no GPIO engine") {}
    void setCorrection(uint8_t, uint8_t) {}
};
NoEngine& engine_ = *new NoEngine;
bool realtimeFailed() { return false; }
uint32_t realOpensNow() { return 0; }
void stubBackendReset() {}
void setGateOpen(bool open) { engine_.setGateOpen(open); }
bool gateOpen() { return engine_.gateOpen(); }
#endif
std::atomic<bool> realOpen_{false};   // whether the engine, rather than the recorder, holds the panels

bool fail(const std::string& why) {
    lastError_ = why;
    warning_.clear();   // a refused open runs no panel for a warning to describe
    return false;
}

}  // namespace

const char* hub75GpioRefusal() {
#ifdef MM_HUB75_GPIO_HAS_ENGINE
    return hub75GpioHostRefusal();
#else
    return "this build has no GPIO engine";
#endif
}

bool hub75GpioAvailable() {
    // A test mode offers the backend on every host, and a test hook forces it either way; otherwise the host decides (@xref{availability}).
    if (mode_ != Hub75GpioMode::Off) return true;
    if (testAvailable_ >= 0) return testAvailable_ != 0;
#ifdef MM_HUB75_GPIO_HAVE_HW
    return hub75GpioRefusal() == nullptr;
#else
    return false;
#endif
}

int hub75GpioResolvedSlowdown(int8_t slowdown) { return hub75GpioResolveSlowdown(slowdown, piSoc()); }

uint8_t hub75GpioScanRate(uint8_t multiplexing, uint8_t rows) { return hub75GpioMultiplexScanRate(multiplexing, rows); }

bool hub75GpioRowsDriveScan(uint8_t multiplexing, uint8_t rows) {
    const uint8_t scan = hub75GpioMultiplexScanRate(multiplexing, rows);
    const bool whole = scan * hub75GpioMultiplexEntry(multiplexing).scanDivisor == rows;
    return whole && (scan == 8 || scan == 16 || scan == 32);
}

uint32_t hub75GpioDrivenCols(uint8_t multiplexing, uint16_t cols) { return hub75GpioMultiplexDrivenCols(multiplexing, cols); }

uint16_t hub75GpioPredictedColumnTimeNs(int slowdown) { return hub75GpioColumnTimeNs(slowdown); }

// Why the real backend is not opened here, or null when it may be: the test binary's closed gate, a build outside Linux aarch64, or a host that is no Pi 3 or 4 (hub75GpioRefusal). A reopened gate leaves the decision to createGpioBackend.
static const char* realOpenRefusal() {
    if (!gateOpen()) return kGpioTestRefusal;
#ifndef MM_HUB75_GPIO_HAVE_HW
    return kGpioNoBackendRefusal;
#else
    return hub75GpioRefusal();
#endif
}

bool hub75GpioOpen(const Hub75GpioOptions& o) {
    hub75GpioClose();
    lastError_.clear();
    warning_.clear();
    if (mode_ != Hub75GpioMode::Off && testOpenFails_) return fail("the test seam refuses the open");
    // The recorder stands in for panels of the size the options wire, so a test reads what a driver opened and pushed.
    if (mode_ == Hub75GpioMode::Record) {
        if (!recordOpen(rec_, testOptions_, o, hub75GpioCheckOptions, lastError_)) return false;
        opens_++;
        return true;
    }
    // The real branch: the engine on the stub backend in Stub mode, on the hardware backend otherwise.
    const bool stub = mode_ == Hub75GpioMode::Stub;
    if (!stub) {
        // A refusal is the reason, not a warning: the GPIO output has no fallback. A test binary is refused before /dev/mem is looked at, so a case stays clear of the live panels of the machine it runs on.
        realAttempts_++;
        if (const char* why = realOpenRefusal()) return fail(why);
    }
    if (!engineOpen(rec_, engine_, o, lastError_, stub)) return false;
    testOptions_ = o;
    opens_++;
    realOpen_.store(true, std::memory_order_release);
    return true;
}

const char* hub75GpioLastError() { return lastError_.empty() ? nullptr : lastError_.c_str(); }

const char* hub75GpioWarning() {
    // The refresh thread asked for realtime scheduling at loop start and the request failed; the panel still runs, but scheduling jitter lands in the column timing as tearing.
    if (realOpen_.load(std::memory_order_acquire) && realtimeFailed())
        return "refresh thread has no real-time priority: timing may jitter (run as root, or set LimitRTPRIO in the hub75-gpio.conf drop-in of installing-on-linux.md)";
    return warning_.empty() ? nullptr : warning_.c_str();
}

const char* hub75GpioCloseError() { return closeError_.empty() ? nullptr : closeError_.c_str(); }

uint16_t hub75GpioWidth() MM_NONBLOCKING { return rec_.width(); }

uint16_t hub75GpioHeight() MM_NONBLOCKING { return rec_.height(); }

void hub75GpioPush(const uint8_t* rgb) MM_NONBLOCKING {
    if (realOpen_.load(std::memory_order_acquire)) {
        engine_.publishRgb(rgb);   // the encode thread encodes it and the refresh thread dumps it; this call returns at once
        return;
    }
    rec_.push(rgb);
}

void hub75GpioCorrection(uint8_t brightness, uint8_t carryBits) MM_NONBLOCKING {
    rec_.setBrightness(brightness);
    carryBits_.store(carryBits, std::memory_order_relaxed);
    engine_.setCorrection(brightness, carryBits);   // atomic stores the engine keeps across opens, so a value set before the open lights the first frame
}

uint16_t hub75GpioFps() MM_NONBLOCKING {
    if (realOpen_.load(std::memory_order_acquire)) return engine_.fps();
    return rec_.width() != 0 ? testFps_ : 0;   // the recorder's rate, while it holds panels
}

bool hub75GpioStalled() MM_NONBLOCKING {
    if (realOpen_.load(std::memory_order_acquire)) return engine_.stalled();
    return rec_.width() != 0 && testStalled_;   // the recorder's answer, while it holds panels
}

uint32_t hub75GpioSubmitErrors() MM_NONBLOCKING {
    return hub75SeamSubmitErrors(realOpen_.load(std::memory_order_acquire), engine_, rec_, testSubmitErrors_);
}

// A closed seam holds no memory: the frames go back, the recorded options to their defaults.
void hub75GpioClose() {
    if (hub75SeamHoldsPanels(realOpen_.load(), rec_)) closes_++;
    realOpen_.store(false, std::memory_order_release);   // before the shutdown, so no push reaches an engine that is stopping
    // The engine stops with the panel dark; harmless on an engine that is not open. A refresh thread stuck in a dump or a failed stop leaves its text here, whether the shutdown finished or not; a clean stop leaves none.
    engine_.shutdown();
    closeError_ = engine_.shutdownError();
    rec_.close();
    testOptions_ = Hub75GpioOptions{};
    std::string().swap(warning_);
}

void setTestHub75Gpio(Hub75GpioMode mode) {
    hub75GpioClose();
    mode_ = mode;
    rec_.recording = mode == Hub75GpioMode::Record;
    rec_.setBrightness(255);
    engine_.setCorrection(255, kHub75GpioCarryBitsDefault);
    carryBits_.store(kHub75GpioCarryBitsDefault);
    opens_ = 0;
    closes_ = 0;
    testFps_ = 0;
    testSubmitErrors_ = 0;
    testStalled_ = false;
    testOpenFails_ = false;
    engine_.failEncodes(0);
#ifdef MM_HUB75_GPIO_HAS_ENGINE
    gpioStubResetCapture();
#else
    stubBackendReset();
#endif
}

void setTestHub75GpioFps(uint16_t hz) { testFps_ = hz; }

void setTestHub75GpioGateOpen(bool open) { setGateOpen(open); }

uint32_t hub75GpioTestRealAttempts() { return realAttempts_; }

uint32_t hub75GpioTestRealOpens() { return realOpensNow(); }

void setTestHub75GpioStalled(bool stalled) { testStalled_ = stalled; }

void setTestHub75GpioSubmitErrors(uint32_t count) { testSubmitErrors_ = count; }

void setTestHub75GpioOpenFails(bool fails) { testOpenFails_ = fails; }

void setTestHub75GpioEncodeFails(int n) { engine_.failEncodes(n); }

// Force the availability hub75GpioAvailable() reports, whatever the host: -1 the host decides again, 0 unavailable, 1 available.
void setTestHub75GpioAvailable(int available) { testAvailable_ = available < 0 ? -1 : (available ? 1 : 0); }

const Hub75GpioTestPort& hub75GpioTestPort() {
    const bool engine = realOpen_.load(std::memory_order_acquire);
    testPort_.options = testOptions_;
    testPort_.opens = opens_;
    testPort_.closes = closes_;
    testPort_.brightness = rec_.brightness();
    testPort_.carryBits = carryBits_.load();
    testPort_.frames = engine ? static_cast<size_t>(engine_.framesPublished()) : rec_.frames();
    testPort_.engineHoldsPanels = engine;
    return testPort_;
}

// The consumer's side of the triple buffer: the newest frame pushed, always whole.
const uint8_t* hub75GpioTestFrame() { return rec_.newestFrame(); }

}  // namespace mm::platform
