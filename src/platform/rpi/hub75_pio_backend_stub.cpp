// SPDX-License-Identifier: GPL-3.0-or-later

/// PioBackend with no hardware, for compile checks, timing simulation and dry runs on a machine with no Pi attached; the stub opens no device.
///
/// A submit takes the time the frame would take on the wire at the configured clock divider, either all of it (the blocking model) or only the wait for a free place in a ring of in-flight chunks (the ring model), so the refresh thread paces itself the way it would on hardware and the swap handshake is exercised for real.
///
/// It also keeps a copy of the last submitted frame, the records of what the engine handed it and the faults a test raises (see hub75_pio_backend_stub.h).

#include "platform/rpi/hub75_pio_backend.h"
#include "platform/rpi/hub75_pio_backend_stub.h"
#include "platform/rpi/hub75_rpi_common.h"   // hub75CopyCapturedWords: the read-back

#if defined(__unix__) || defined(__APPLE__)
    #include <csignal>
    #include <pthread.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace mm::platform {

namespace {

/// The capture cap: one frame of wide words, process-wide; a wall frame is 192 KiB at 6-bit, a larger one (up to about 16 MiB) is read back truncated to its first 1 MiB, its checksum and identity still covering every word.
constexpr size_t kCaptureMaxBytes = 1024 * 1024;

/// Everything the stub shares with the tests, under one lock.
struct StubState {
    std::vector<uint32_t> capture;
    uint64_t submitted = 0;
    StubFrameId ids[kStubFrameIdRing];
    StubStartRecord start;
    uint32_t blanks = 0;
    uint32_t lastBlankBytes = 0;
    bool blankStopped = false;      // the blank has stopped the stub, until the next start
    uint32_t realInFlightAtStop = 0;   // chunks of real frames still in flight when the last blank stopped the stub
    uint32_t submitsAfterBlank = 0;
    uint32_t submitsWithStopOpen = 0;   // frame submits made by a thread that did not block SIGTERM
    StubFaults faults;
    StubQueueModel model = StubQueueModel::Blocking;
};
std::mutex stateMu_;
StubState state_;

// One RP1 PIO clock cycle in ns (200 MHz: 5 ns).
constexpr uint64_t kRp1CycleNs = 1'000'000'000 / kRp1PioClockHz;

using Clock = std::chrono::steady_clock;

// A hash of the words, so two frames of different words differ in it: FNV-1a over 32-bit words.
constexpr uint64_t kChecksumBasis = 0xcbf29ce484222325ull;
uint64_t checksumOf(const uint32_t* words, size_t count, uint64_t h = kChecksumBasis) {
    for (size_t i = 0; i < count; i++) h = (h ^ words[i]) * 0x100000001b3ull;
    return h;
}

// Whether the calling thread blocks SIGTERM: always, where there are no POSIX signals to block.
bool blocksStopSignal() {
#if defined(__unix__) || defined(__APPLE__)
    sigset_t now;
    pthread_sigmask(SIG_BLOCK, nullptr, &now);
    return sigismember(&now, SIGTERM) == 1;
#else
    return true;
#endif
}

// Whether a submit fails now: the next `failSubmits` do, every one at -1. The caller holds the lock.
bool takeSubmitFault() {
    int& n = state_.faults.failSubmits;
    if (n == 0) return false;
    if (n > 0) n--;
    return true;
}

class StubBackend : public PioBackend {
  public:
    bool open(std::string& error) override {
        {
            std::lock_guard<std::mutex> lock(stateMu_);
            if (state_.faults.failOpen) {
                error = "stub open failed (test fault)";
                return false;
            }
        }
        opened_ = true;
        return true;
    }

    bool start(const PioStartConfig& config, std::string& error) override {
        if (!opened_) {
            error = "stub backend not opened";
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(stateMu_);
            if (state_.faults.failStart) {
                error = "stub start failed (test fault)";
                return false;
            }
            // What the kernel refuses with -EINVAL.
            if (config.xferBufCount > 4 || config.xferBufSize == 0 || config.xferBufSize % 4 != 0) {
                error = "stub start refused: -EINVAL (more than 4 bounce buffers, or a buffer size of 0 or not a multiple of 4)";
                return false;
            }
            state_.blankStopped = false;
            state_.start.starts++;
            state_.start.config = config;
            state_.start.program.assign(config.programInstructions,
                                        config.programInstructions + (config.programInstructions ? config.programLength : 0));
        }
        // SM clock = 200 MHz / divider; one word per 5 SM cycles, so a word takes 5 x 5 ns x divider.
        dividerQ8_ = static_cast<uint32_t>(config.clkdivInt) << 8 | config.clkdivFrac;
        ringBytes_ = config.xferBufSize;
        ringCount_ = config.xferBufCount ? config.xferBufCount : 1;
        inFlight_.clear();
        busyUntil_ = Clock::now();
        started_ = true;
        return true;
    }

    int submitFrame(const void* data, uint32_t bytes) override {
        if (!started_) {
            std::lock_guard<std::mutex> lock(stateMu_);
            if (state_.blankStopped) state_.submitsAfterBlank++;
            return -ENODEV;   // as the piolib backend answers before its start
        }
        return submit(data, bytes, false);
    }

    bool blankAndStop(const void* blankData, uint32_t blankBytes, std::string& error) override {
        bool ok = true;
        bool early = false;
        uint32_t realInFlight = 0;
        if (started_) {
            // The blank is queued, then the FIFO drains and the SM stops: an early FIFO-empty poll stops it first.
            if (submit(blankData, blankBytes, true) != 0) {
                error = "stub blank submit failed";
                ok = false;
            }
            {
                std::lock_guard<std::mutex> lock(stateMu_);
                early = state_.faults.fifoEmptyEarly;
            }
            if (early) realInFlight = realChunksInFlight();
            else drain();
        }
        started_ = false;
        std::lock_guard<std::mutex> lock(stateMu_);
        state_.blankStopped = true;
        state_.realInFlightAtStop = realInFlight;
        return ok;
    }

    void close() override {
        started_ = false;
        opened_ = false;
    }

    const char* name() const override { return "stub (no hardware)"; }

  private:
    // What one submit captures of its frame: the first 1 MiB, a checksum over the whole frame, and its first and last words.
    struct FrameCapture {
        std::vector<uint32_t> copy;
        uint64_t sum = 0;
        uint32_t first = 0;
        uint32_t last = 0;
        uint32_t words = 0;
    };

    // The kernel's copy out of the caller's buffer: the second half is read after the delay, so a buffer written meanwhile arrives torn.
    FrameCapture captureFrame(const void* data, uint32_t bytes) {
        const uint32_t* all = static_cast<const uint32_t*>(data);
        const size_t words = data ? bytes / kBytesPerPixelClock : 0;
        uint32_t copyMicros;
        {
            std::lock_guard<std::mutex> lock(stateMu_);
            copyMicros = state_.faults.copyMicros;
        }
        const size_t half = words / 2;
        const size_t keep = std::min<size_t>(words, kCaptureMaxBytes / 4);
        FrameCapture c;
        c.copy.resize(words > 0 ? keep : 0);
        if (half > 0) std::copy(all, all + std::min(half, keep), c.copy.begin());
        c.sum = checksumOf(all, half);
        if (copyMicros > 0) std::this_thread::sleep_for(std::chrono::microseconds(copyMicros));
        if (words > half) {
            if (keep > half) std::copy(all + half, all + keep, c.copy.begin() + static_cast<std::ptrdiff_t>(half));
            c.sum = checksumOf(all + half, words - half, c.sum);
        }
        c.first = words ? all[0] : 0;
        c.last = words ? all[words - 1] : 0;
        c.words = static_cast<uint32_t>(words);
        return c;
    }

    // Record one accepted frame, or blank, in the state; the caller holds stateMu_.
    void record(FrameCapture&& c, uint32_t bytes, bool blank) {
        if (blank) {
            state_.blanks++;
            state_.lastBlankBytes = bytes;
        } else {
            if (!blocksStopSignal()) state_.submitsWithStopOpen++;
            const uint64_t index = ++state_.submitted;
            StubFrameId& id = state_.ids[(index - 1) % kStubFrameIdRing];
            id.index = index;
            id.checksum = c.sum;
            id.first = c.first;
            id.last = c.last;
            id.words = c.words;
        }
        state_.capture = std::move(c.copy);
    }

    // Capture, record and pace one frame: 0, or the fault's code when one is raised.
    int submit(const void* data, uint32_t bytes, bool blank) {
        FrameCapture frame = captureFrame(data, bytes);
        StubQueueModel model;
        hang();
        {
            std::lock_guard<std::mutex> lock(stateMu_);
            model = state_.model;
            if (takeSubmitFault()) return state_.faults.submitError;
            record(std::move(frame), bytes, blank);
        }
        if (model == StubQueueModel::Blocking) {
            const uint64_t wireNs = wireNsOf(bytes);
            if (wireNs > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(wireNs));
        } else {
            queue(bytes, blank);
        }
        return 0;
    }

    // A stuck submit: blocks until a test clears the hang.
    static void hang() {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(stateMu_);
                if (!state_.faults.hangSubmits) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // The nanoseconds `bytes` take on the wire at the configured divider.
    uint64_t wireNsOf(uint64_t bytes) const {
        return bytes / kBytesPerPixelClock * dividerQ8_ * kCyclesPerPixelClock * kRp1CycleNs / 256;
    }

    // The ring model: each chunk takes a place until its wire time has passed, and the submit waits only for a free place; `blank` marks the chunks of a blank.
    void queue(uint32_t bytes, bool blank) {
        uint32_t done = 0;
        while (done < bytes) {
            const uint32_t chunk = std::min(bytes - done, ringBytes_);
            auto now = Clock::now();
            while (!inFlight_.empty() && inFlight_.front().finish <= now) inFlight_.pop_front();
            if (inFlight_.size() >= ringCount_) {
                std::this_thread::sleep_until(inFlight_.front().finish);
                inFlight_.pop_front();
                now = Clock::now();
            }
            busyUntil_ = std::max(now, busyUntil_) + std::chrono::nanoseconds(wireNsOf(chunk));
            done += chunk;
            inFlight_.push_back({busyUntil_, blank});
        }
    }

    // Wait until every queued chunk has gone out: the TX FIFO is empty.
    void drain() {
        if (!inFlight_.empty()) std::this_thread::sleep_until(busyUntil_);
        inFlight_.clear();
    }

    // The chunks of real frames, not of the blank, that have not gone out yet.
    uint32_t realChunksInFlight() {
        const auto now = Clock::now();
        uint32_t real = 0;
        for (const Chunk& chunk : inFlight_)
            if (chunk.finish > now && !chunk.blank) real++;
        return real;
    }

    struct Chunk {
        Clock::time_point finish;   // when the chunk has gone out
        bool blank;                 // the chunk belongs to a blank, not to a real frame
    };

    bool opened_ = false;
    bool started_ = false;
    uint32_t dividerQ8_ = 0;   // the SM clock divider in 1/256ths
    uint32_t ringBytes_ = UINT32_MAX;
    uint32_t ringCount_ = 1;
    std::deque<Chunk> inFlight_;   // the queued chunks, oldest first
    Clock::time_point busyUntil_;
};

}  // namespace

PioBackend* createStubBackend() { return new StubBackend(); }

uint64_t stubBackendSubmittedFrames() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.submitted;
}

size_t stubBackendCopyLastFrame(uint32_t* out, size_t capWords) {
    std::lock_guard<std::mutex> lock(stateMu_);
    return hub75CopyCapturedWords(state_.capture, out, capWords);
}

void stubBackendResetCapture() {
    std::lock_guard<std::mutex> lock(stateMu_);
    state_.capture.clear();
    state_.submitted = 0;
}

size_t stubBackendFrameIds(StubFrameId* out, size_t cap) {
    if (!out) return 0;
    std::lock_guard<std::mutex> lock(stateMu_);
    const size_t kept = static_cast<size_t>(std::min<uint64_t>(state_.submitted, kStubFrameIdRing));
    const size_t n = std::min(kept, cap);
    // The newest `n` of the kept, oldest first.
    for (size_t i = 0; i < n; i++) out[i] = state_.ids[(state_.submitted - n + i) % kStubFrameIdRing];
    return n;
}

StubStartRecord stubBackendLastStart() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.start;
}

uint32_t stubBackendSubmitsAfterBlank() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.submitsAfterBlank;
}

uint32_t stubBackendRealChunksInFlightAtStop() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.realInFlightAtStop;
}

uint32_t stubBackendSubmitsWithStopOpen() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.submitsWithStopOpen;
}

uint32_t stubBackendBlanks() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.blanks;
}

uint32_t stubBackendLastBlankBytes() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.lastBlankBytes;
}

void stubBackendSetFaults(const StubFaults& faults) {
    std::lock_guard<std::mutex> lock(stateMu_);
    state_.faults = faults;
}

void stubBackendSetQueueModel(StubQueueModel model) {
    std::lock_guard<std::mutex> lock(stateMu_);
    state_.model = model;
}

void stubBackendReset() {
    std::lock_guard<std::mutex> lock(stateMu_);
    // Field by field: a whole fresh StubState would sit on this function's stack, with its frame identities.
    state_.capture.clear();
    state_.submitted = 0;
    for (StubFrameId& id : state_.ids) id = StubFrameId{};
    state_.start = StubStartRecord{};
    state_.blanks = 0;
    state_.lastBlankBytes = 0;
    state_.blankStopped = false;
    state_.realInFlightAtStop = 0;
    state_.submitsAfterBlank = 0;
    state_.submitsWithStopOpen = 0;
    state_.faults = StubFaults{};
    state_.model = StubQueueModel::Blocking;
}

}  // namespace mm::platform
