// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// The recorder behind the Pi HUB75 seam on a host (platform_host_pio.cpp): it stands in for the panels, so a test reads the frames a driver pushed.
///
/// The frames travel a lock-free triple buffer with one producer, the driver, and one consumer, the test: `ready_` holds a slot index in its low bits and kFresh when the producer has filled it since the consumer last took it.
///
/// A seam holds one recorder and keeps its own options, warning and engine beside it.

#include "platform/nonblocking.h"   // MM_NONBLOCKING

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mm::platform {

class Hub75Recorder {
public:
    /// Whether the recorder, rather than the host's panels, stands in.
    bool recording = false;

    /// The canvas's width in pixels.
    uint16_t width() const MM_NONBLOCKING { return width_.load(); }
    /// The canvas's height in pixels.
    uint16_t height() const MM_NONBLOCKING { return height_.load(); }

    /// Take the canvas size the open reports, which the real engine and the recorder both set.
    void setSize(uint16_t width, uint16_t height) {
        width_ = width;
        height_ = height;
    }

    /// Stand in for panels of `width` by `height`: the three frame slots a consumer takes from, so a test runs the whole handover.
    void begin(uint16_t width, uint16_t height) {
        setSize(width, height);
        for (auto& s : shared_->slots) s.assign(frameBytes(), 0);
        ready_.store(0, std::memory_order_release);
        writeSlot_ = 1;
        readSlot_ = 2;
        frames_.store(0, std::memory_order_relaxed);
    }

    /// Hand a frame to the consumer's side; a canvas with no slots yet, such as a late open's, takes none.
    void push(const uint8_t* rgb) MM_NONBLOCKING {
        const size_t bytes = frameBytes();
        if (!rgb || bytes == 0 || shared_->slots[writeSlot_].size() < bytes) return;
        std::memcpy(shared_->slots[writeSlot_].data(), rgb, bytes);
        writeSlot_ = ready_.exchange(static_cast<uint8_t>(writeSlot_ | kFresh), std::memory_order_acq_rel) & 3;
        if (recording) frames_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Keep the global brightness the output last handed over.
    void setBrightness(uint8_t brightness) MM_NONBLOCKING { brightness_.store(brightness, std::memory_order_relaxed); }
    /// The brightness the output last handed over.
    uint8_t brightness() const { return brightness_.load(); }

    /// The frames pushed since the last open.
    size_t frames() const { return frames_.load(std::memory_order_relaxed); }

    /// The consumer's side of the triple buffer: the newest frame pushed, always whole; null before the first.
    const uint8_t* newestFrame() {
        if (frames() == 0) return nullptr;
        if (ready_.load(std::memory_order_acquire) & kFresh) readSlot_ = ready_.exchange(readSlot_, std::memory_order_acq_rel) & 3;
        return shared_->slots[readSlot_].data();
    }

    /// A closed seam holds no memory: the canvas and the frames go back.
    void close() {
        setSize(0, 0);
        ready_.store(0, std::memory_order_release);   // the last push stays unshown: its slot goes below
        for (auto& s : shared_->slots) std::vector<uint8_t>().swap(s);
        frames_.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr uint8_t kFresh = 4;

    /// The bytes of one RGB frame of the canvas.
    size_t frameBytes() const MM_NONBLOCKING { return static_cast<size_t>(width_.load()) * height_.load() * 3; }

    // The frame slots, on the heap for the process's life, so a consumer on another thread reads live slots at exit.
    struct Shared {
        std::vector<uint8_t> slots[3];
    };

    std::atomic<uint16_t> width_{0};
    std::atomic<uint16_t> height_{0};
    std::atomic<uint8_t> brightness_{255};
    std::atomic<uint8_t> ready_{0};
    uint8_t writeSlot_ = 1;
    uint8_t readSlot_ = 2;
    std::atomic<size_t> frames_{0};   // written by the producer, read by the consumer
    Shared* const shared_ = new Shared;
};

/// The failed submits a seam answers: the engine's count, saturated to 32 bits, while the engine holds the panels, else the `recorded` count a test set while the recorder holds them.
template <class Engine>
uint32_t hub75SeamSubmitErrors(bool engineHolds, const Engine& engine, const Hub75Recorder& rec, uint32_t recorded) MM_NONBLOCKING {
    if (engineHolds) return static_cast<uint32_t>(std::min<uint64_t>(engine.submitErrors(), UINT32_MAX));
    return rec.width() != 0 ? recorded : 0;
}

/// Whether a seam holds panels, the engine or the recorder: the state whose close counts.
inline bool hub75SeamHoldsPanels(bool engineHolds, const Hub75Recorder& rec) { return engineHolds || rec.width() != 0; }

/// The engine of a build that omits it (MSVC): every open refuses with `why`, every count is 0 and every call lands harmlessly, so a seam keeps one code path. A seam derives its own to take the calls only it makes.
template <class Options>
class Hub75NoEngine {
  public:
    /// An engine whose every open refuses with `why`.
    explicit Hub75NoEngine(const char* why) : why_(why) {}
    /// Refuses with the reason.
    bool open(const Options&, std::string& error, bool) {
        error = why_;
        return false;
    }
    /// Stops at once, since no engine runs.
    bool shutdown() { return true; }
    /// A clean stop.
    const char* shutdownError() const { return ""; }
    /// No canvas.
    uint16_t width() const { return 0; }
    /// No canvas.
    uint16_t height() const { return 0; }
    /// No frame is published.
    uint64_t framesPublished() const { return 0; }
    /// No submit fails.
    uint64_t submitErrors() const { return 0; }
    /// No refresh runs.
    uint16_t fps() const { return 0; }
    /// No submit is in flight.
    bool stalled() const { return false; }
    /// The frame goes nowhere.
    void publishRgb(const uint8_t*) {}
    /// The stall time has no engine to set.
    void setStallMs(uint64_t) {}
    /// The join deadline has no engine to set.
    void setJoinDeadlineMs(int) {}
    /// No encode runs to fail.
    void failEncodes(int) {}
    /// Whether the real-backend gate is open: moot where no real backend exists, so it remembers what a test set, which the seam's early refusal reads.
    bool gateOpen() const { return gateOpen_; }
    /// Closes the gate, or opens it again.
    void setGateOpen(bool open) { gateOpen_ = open; }

  private:
    const char* why_;
    bool gateOpen_ = true;
};

/// Stand in for the panels `o` wires once the seam's `check` accepts them, keeping the options in `kept`; false leaves the reason in `error`.
template <class Options, class Check>
bool recordOpen(Hub75Recorder& rec, Options& kept, const Options& o, Check check, std::string& error) {
    if (!check(o, error)) return false;
    kept = o;
    rec.begin(static_cast<uint16_t>(o.cols * o.panelsPerLane), static_cast<uint16_t>(o.rows * o.parallel));
    return true;
}

/// Open the real `engine` with `o`, on its stub backend when `useStubBackend`, and take the canvas it forms into `rec`; false leaves the reason in `error`.
template <class Engine, class Options>
bool engineOpen(Hub75Recorder& rec, Engine& engine, const Options& o, std::string& error, bool useStubBackend) {
    if (!engine.open(o, error, useStubBackend)) return false;
    rec.setSize(static_cast<uint16_t>(engine.width()), static_cast<uint16_t>(engine.height()));
    return true;
}

}  // namespace mm::platform
