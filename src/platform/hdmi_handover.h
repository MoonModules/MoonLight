#pragma once

/// The two lock-free handovers between the render tick and an HDMI backend's own thread, written once for every platform: the frame going out, and what the monitor said coming back.

#include "edid.h"
#include "nonblocking.h"

#include <atomic>
#include <cstdint>

namespace mm::platform {

/// One frame between the tick and the output thread, each side claiming it with a compare-exchange so neither waits. A frame not shown yet is replaced by a newer one, and a frame arriving while one is shown is dropped.
class FrameSlot {
public:
    /// The tick claims the slot to write a frame; false while the thread shows one, so this frame is dropped.
    bool claimWrite() MM_NONBLOCKING {
        uint8_t expect = kFree;
        if (state_.compare_exchange_strong(expect, kWriting, std::memory_order_acquire, std::memory_order_relaxed)) return true;
        expect = kReady;   // a frame not shown yet is replaced by the newer one
        return state_.compare_exchange_strong(expect, kWriting, std::memory_order_acquire, std::memory_order_relaxed);
    }
    /// The written frame is ready to show.
    void publish() MM_NONBLOCKING { state_.store(kReady, std::memory_order_release); }
    /// The thread claims a ready frame to show.
    bool claimShow() MM_NONBLOCKING {
        uint8_t expect = kReady;
        return state_.compare_exchange_strong(expect, kShowing, std::memory_order_acquire, std::memory_order_relaxed);
    }
    /// The thread claims the slot while nothing is in it, to resize what it guards.
    bool claimIdle() MM_NONBLOCKING {
        uint8_t expect = kFree;
        return state_.compare_exchange_strong(expect, kShowing, std::memory_order_acquire, std::memory_order_relaxed);
    }
    /// Claim the slot unless the tick is writing it, for a close that frees what it guards; it stays claimed, refusing every write, until reset.
    bool claimUnlessWriting() MM_NONBLOCKING {
        uint8_t now = state_.load(std::memory_order_relaxed);
        return now != kWriting && state_.compare_exchange_strong(now, kShowing, std::memory_order_acquire, std::memory_order_relaxed);
    }
    /// Hand the slot back empty: after a show, a write given up, or an open.
    void release() MM_NONBLOCKING { state_.store(kFree, std::memory_order_release); }

private:
    enum : uint8_t { kFree, kWriting, kReady, kShowing };
    std::atomic<uint8_t> state_{kFree};
};

/// A seqlock, as Linux's seqlock_t: one writer publishes a value, readers copy it without waiting, and a copy a write tore is seen and dropped, its reader trying again on its next poll.
template <typename T>
class Seqlock {
public:
    /// The writer's publish: the count is odd while the value changes.
    void write(const T& value) MM_NONBLOCKING {
        seq_.fetch_add(1, std::memory_order_acq_rel);
        value_ = value;
        seq_.fetch_add(1, std::memory_order_release);
    }
    /// A reader's copy; false when a write tore it.
    bool read(T& out) const MM_NONBLOCKING {
        const uint32_t before = seq_.load(std::memory_order_acquire);
        if (before & 1) return false;
        out = value_;
        // A read-don't-modify-write orders the copy before the second look at the count, as a fence would, and thread sanitizers accept it: Boehm's seqlock reader.
        return seq_.fetch_add(0, std::memory_order_release) == before;
    }
    /// The writer's own view, which no write can tear since only the writer writes.
    const T& written() const MM_NONBLOCKING { return value_; }

private:
    mutable std::atomic<uint32_t> seq_{0};   // mutable: a reader's read-don't-modify-write changes nothing it can see
    T value_{};
};

/// What the monitor said, as a backend publishes it through a Seqlock: its EDID reduced, and whether one was read.
struct MonitorReport {
    edid::Monitor monitor;   ///< the reduced EDID, empty while unread
    bool read = false;       ///< whether an EDID was read since the monitor connected
};

}  // namespace mm::platform
