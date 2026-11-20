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
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
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
    bool stopped = false;      // a stop has stopped the stub, until the next start
    uint32_t realInFlightAtStop = 0;   // chunks of real frames still in flight when the last stop ran
    uint32_t submitsAfterStop = 0;
    uint32_t submitsWithStopOpen = 0;   // frame submits made by a thread that did not block SIGTERM
    uint32_t blankAttempts = 0;         // blank submits tried, the failed ones included
    std::thread::id blankThread;        // the thread of the last blank submit that was recorded
    std::map<std::thread::id, uint32_t> submitThreads;   // the submits (frames and blanks) each thread made
    bool pinsDark = false;              // a stop has set the pins to the dark word, until the next start
    uint32_t stops = 0;                 // stops that stopped a started stub, since the last reset
    bool lastStopDrained = false;       // whether the last of them had `drain` set
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
            state_.stopped = false;
            state_.pinsDark = false;   // a started SM drives its own pins, so the dark word of an earlier stop is gone
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
        dark_ = false;   // a started SM drives its own pins, so a stop owes the dark word
        return true;
    }

    int submitFrame(const void* data, uint32_t bytes) override {
        if (!started_) return refuseAfterStop();
        return submit(data, bytes, false);
    }

    int submitBlank(const void* data, uint32_t bytes) override {
        if (!started_) return refuseAfterStop();
        return submit(data, bytes, true);
    }

    bool stop(bool drain, std::string& error) override {
        error.clear();   // this stop's note or failure alone
        // The flag goes first, so a submit released late is refused; a stop that finds the stub stopped and dark is a no-op, and one that finds a failed stop runs it again.
        const bool wasStarted = started_.exchange(false);
        if (!wasStarted && dark_) return true;
        StubFaults faults;
        {
            std::lock_guard<std::mutex> lock(stateMu_);
            if (wasStarted) {
                state_.stops++;
                state_.lastStopDrained = drain;
            }
            faults = state_.faults;
        }
        // A stop with `drain` clear leaves the queue alone: the refresh thread may be inside it.
        const uint32_t realInFlight = wasStarted && drain ? drainFor(faults, error) : 0;
        std::lock_guard<std::mutex> lock(stateMu_);
        state_.stopped = true;
        if (wasStarted) state_.realInFlightAtStop = realInFlight;
        if (faults.failStop) {
            addNote(error, "stub stop failed (test fault): the SM was not disabled and the pins were not set");
            return false;   // the pins are not darkened
        }
        state_.pinsDark = true;
        dark_ = true;
        return true;
    }

    void close() override {
        started_ = false;
        dark_ = true;
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
        state_.submitThreads[std::this_thread::get_id()]++;
        if (blank) {
            state_.blankThread = std::this_thread::get_id();
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
        {
            std::lock_guard<std::mutex> lock(stateMu_);
            if (blank) state_.blankAttempts++;
        }
        hang();
        {
            std::lock_guard<std::mutex> lock(stateMu_);
            model = state_.model;
            if (takeSubmitFault() || (blank && state_.faults.failBlank)) return state_.faults.submitError;
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

    // A submit after a stop: counted, and answered as the piolib backend answers before its start.
    int refuseAfterStop() {
        std::lock_guard<std::mutex> lock(stateMu_);
        if (state_.stopped) state_.submitsAfterStop++;
        return -ENODEV;
    }

    // Adds one note or failure to the text the stop reports, after any earlier one.
    static void addNote(std::string& error, const char* what) {
        if (!error.empty()) error += "; ";
        error += what;
    }

    // The wait a stop with `drain` makes for the queue, or its skip under `fifoEmptyEarly`; returns the chunks of real frames still in flight, and notes a FIFO that `drainTimesOut`.
    uint32_t drainFor(const StubFaults& faults, std::string& error) {
        const uint32_t realInFlight = faults.fifoEmptyEarly ? realChunksInFlight() : 0;
        if (!faults.fifoEmptyEarly) drainQueue();
        if (faults.drainTimesOut) addNote(error, "stub TX FIFO did not drain (test fault); disabling SM anyway");
        return realInFlight;
    }

    // Wait until every queued chunk has gone out: the TX FIFO is empty.
    void drainQueue() {
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
    bool dark_ = true;   // no SM runs or its pins are dark; false from start() until a stop has succeeded once
    std::atomic<bool> started_{false};   // the closing thread's stop() writes it while the refresh thread submits
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

uint32_t stubBackendSubmitsAfterStop() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.submitsAfterStop;
}

uint32_t stubBackendRealChunksInFlightAtStop() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.realInFlightAtStop;
}

uint32_t stubBackendSubmitsWithStopOpen() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.submitsWithStopOpen;
}

uint32_t stubBackendBlankAttempts() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.blankAttempts;
}

std::thread::id stubBackendBlankThread() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.blankThread;
}

uint32_t stubBackendSubmitsFromThread(std::thread::id thread) {
    std::lock_guard<std::mutex> lock(stateMu_);
    const auto it = state_.submitThreads.find(thread);
    return it == state_.submitThreads.end() ? 0 : it->second;
}

uint32_t stubBackendStops() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.stops;
}

bool stubBackendLastStopDrained() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.lastStopDrained;
}

bool stubBackendPinsDark() {
    std::lock_guard<std::mutex> lock(stateMu_);
    return state_.pinsDark;
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
    state_.stopped = false;
    state_.realInFlightAtStop = 0;
    state_.submitsAfterStop = 0;
    state_.submitsWithStopOpen = 0;
    state_.blankAttempts = 0;
    state_.blankThread = std::thread::id{};
    state_.submitThreads.clear();
    state_.pinsDark = false;
    state_.stops = 0;
    state_.lastStopDrained = false;
    state_.faults = StubFaults{};
    state_.model = StubQueueModel::Blocking;
}

}  // namespace mm::platform
