// SPDX-License-Identifier: GPL-3.0-or-later

/// GpioBackend with no hardware, for compile checks, timing simulation and dry runs on a machine with no Pi attached; the stub runs entirely in user memory and maps no /dev/mem.
///
/// submitFrame() runs the shared dump loop (hub75_gpio_dump.h) against a recording sink and sleeps for the time the frame would take on the wire (@xref{what-submitframe-does|What submitFrame does}).
///
/// The stub keeps a copy of the last submitted frame and the setup-step log, so an engine test can read back the GPIO words and pin the dark-first setup order (@xref{the-setup-log|The setup log}, @xref{the-capture|The capture}, see hub75_gpio_backend_stub.h).
///
/// @technical
///
/// ## What submitFrame does
///
/// The recording sink lets the unit test read back the GPIO writes and pulser steps the hardware backend would have driven: the same dump loop, not a copy.
///
/// submitFrame() then sleeps for the time the frame would take on the wire at the configured column time, so the refresh thread paces itself the way it would on hardware and the swap handshake is exercised for real (@xref{wire-time|Wire time}).
///
/// ## Wire time
///
/// Wire time per frame is the columns at the column time, plus the dark interval per row-group (`kHub75GpioDarkIntervalNs`: the address and latch writes), plus the OE time where it outlasts a row's shift (only at large LSB times). It is the same model the engine's predicted refresh uses.
///
/// ## The setup log
///
/// The setup log mirrors the hardware backend's open() and start() step for step: open() logs dark-values then directions; start() logs pwm-config then oe-mux. The order is the dark-first order (values, then directions, then the mux), and the engine test asserts the logged string.
///
/// ## The capture
///
/// The capture is one frame's worth of GPIO words and one frame's dump stream, process-wide, since the engine owns one backend at a time.
///
/// A wall frame is 192 KiB at 6-bit (196,608 bytes); the 1 MiB cap holds the largest frame the engine can build (3 lanes of 512x64 at 6-bit is under 600 KiB). The dump stream is about 3 events a column plus the per-group steps, 8 bytes each, and the recorder keeps the first 131,072, the same 1 MiB: the sign's whole frame, a wall's first part.
///
/// submitFrame() builds the stream locally outside the lock and swaps it in whole, so a test reading it always sees one frame's stream.

#include "platform/rpi/hub75_gpio_backend.h"
#include "platform/rpi/hub75_gpio_backend_stub.h"
#include "platform/rpi/hub75_gpio_dump.h"   // the shared dump loop + GpioDumpEvent
#include "platform/rpi/hub75_rpi_common.h"   // hub75CopyCapturedWords: the read-back both stubs share

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mm::platform {

namespace {

/// The capture cap in bytes, sized for the largest frame the engine can build (@xref{the-capture|The capture}).
constexpr size_t kCaptureMaxBytes = 1024 * 1024;
std::mutex captureMu_;
std::vector<uint32_t> capture_;
std::vector<GpioDumpEvent> stream_;
/// The stream cap in events, the capture's bytes again, so a wall of any size records a bounded stream.
constexpr size_t kStreamMaxEvents = kCaptureMaxBytes / sizeof(GpioDumpEvent);
uint64_t submitted_ = 0;
std::string setupLog_;
std::atomic<uint32_t> failingWaits_{0};   // the OE pulse waits the timeout hook still fails

/// The dump loop's recording sink: every write and pulser step lands in the stream, in order, up to the cap; the LEV reads are the slowdown delays, timing rather than signal, so the sink skips them.
struct RecordSink {
    std::vector<GpioDumpEvent>* stream = nullptr;
    void record(GpioDumpEvent::Kind kind, uint32_t value) {
        if (stream->size() < kStreamMaxEvents) stream->push_back({kind, value});
    }
    void writeSet(uint32_t mask) { record(GpioDumpEvent::Kind::Set, mask); }
    void writeClr(uint32_t mask) { record(GpioDumpEvent::Kind::Clr, mask); }
    uint32_t readLev() { return 0; }
    /// Completes, unless the timeout hook still fails a wait, as a wedged PWM does on hardware.
    bool waitPulseFinished() {
        record(GpioDumpEvent::Kind::PulseWait, 0);
        uint32_t n = failingWaits_.load();
        while (n != 0 && !failingWaits_.compare_exchange_weak(n, n - 1)) {}
        return n == 0;
    }
    void sendPulse(uint8_t plane) { record(GpioDumpEvent::Kind::PulseFire, plane); }
};

void logStep(const char* step) {
    std::lock_guard<std::mutex> lock(captureMu_);
    if (!setupLog_.empty()) setupLog_ += ",";
    setupLog_ += step;
}

class StubBackend : public GpioBackend {
  public:
    bool open(std::string& /*error*/) override {
        // The hardware backend's open() order, logged rather than driven: dark levels first, then the pins made outputs.
        logStep("dark-values");
        logStep("directions");
        opened_ = true;
        return true;
    }

    bool start(const GpioSubmitConfig& config, std::string& error) override {
        if (!hub75GpioCheckStart(opened_, config, error)) return false;
        // The hardware backend's start() order: the PWM configured (idle high), then OE muxed to it.
        logStep("pwm-config");
        logStep("oe-mux");
        config_ = config;
        slowdown_ = hub75GpioResolveSlowdown(config.slowdown, PiSoc::Other);   // the stub has no model: auto is 1
        // Wire time per frame, the model the engine's predicted refresh uses (@xref{wire-time|Wire time}).
        frameNs_ = hub75GpioPredictedFrameNs(config.scanRows, static_cast<uint32_t>(config.laneWidth) * config.pairs,
                                             config.bitDepth, config.lsbNs, hub75GpioColumnTimeNs(slowdown_));
        started_ = true;
        return true;
    }

    int submitFrame(const uint32_t* words, size_t wordCount) override {
        if (!started_) return -1;
        // Run the shared dump loop against the recorder first, outside the lock, so the stream swaps in whole.
        std::vector<GpioDumpEvent> stream;
        if (words != nullptr) {
            RecordSink sink{&stream};
            (void)hub75GpioDumpFrame(sink, config_, words, wordCount, slowdown_, pulseTimeouts_);
        }
        {
            std::lock_guard<std::mutex> lock(captureMu_);
            submitted_++;
            const size_t keep =
                std::min<size_t>(wordCount * 4, kCaptureMaxBytes) / 4;
            capture_.assign(keep, 0);
            if (words != nullptr && keep > 0)
                std::memcpy(capture_.data(), words, keep * 4);
            stream_.swap(stream);
        }
        if (frameNs_ > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(frameNs_));
        return 0;
    }

    bool blankAndStop(const uint32_t* blankWords, size_t blankWordCount,
                      std::string& error) override {
        // Simulate the blank frame's wire time, then stop.
        const bool dumped = !started_ || submitFrame(blankWords, blankWordCount) == 0;
        if (!dumped) error = "the blank frame did not dump";
        started_ = false;
        return dumped;
    }

    void close() override {
        started_ = false;
        opened_ = false;
    }

    const char* name() const override { return "stub (no hardware)"; }

    uint32_t plldHz() const override { return hub75GpioPlldHz(PiSoc::Other); }
    int resolvedSlowdown() const override { return slowdown_; }
    uint64_t pulseTimeouts() const override { return pulseTimeouts_; }

  private:
    uint64_t pulseTimeouts_ = 0;   // the OE waits that timed out, as the hardware backend counts them (the refresh thread's)
    bool opened_ = false;
    bool started_ = false;
    GpioSubmitConfig config_{};
    int slowdown_ = 1;
    uint64_t frameNs_ = 0;   // the frame's wire time
};

}  // namespace

GpioBackend* createGpioStubBackend() { return new StubBackend(); }

uint64_t gpioStubSubmittedFrames() {
    std::lock_guard<std::mutex> lock(captureMu_);
    return submitted_;
}

size_t gpioStubCopyLastFrame(uint32_t* out, size_t capWords) {
    std::lock_guard<std::mutex> lock(captureMu_);
    return hub75CopyCapturedWords(capture_, out, capWords);
}

std::string gpioStubSetupLog() {
    std::lock_guard<std::mutex> lock(captureMu_);
    return setupLog_;
}

size_t gpioStubCopyLastDumpStream(GpioDumpEvent* out, size_t capEvents) {
    std::lock_guard<std::mutex> lock(captureMu_);
    if (!out || stream_.empty()) return 0;
    const size_t n = stream_.size() < capEvents ? stream_.size() : capEvents;
    std::memcpy(out, stream_.data(), n * sizeof(GpioDumpEvent));
    return n;
}

void gpioStubResetCapture() {
    std::lock_guard<std::mutex> lock(captureMu_);
    capture_.clear();
    stream_.clear();
    submitted_ = 0;
    setupLog_.clear();
    failingWaits_.store(0);
}

void gpioStubTimeOutPulseWaits(uint32_t n) { failingWaits_.store(n); }

}  // namespace mm::platform
