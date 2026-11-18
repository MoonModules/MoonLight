#pragma once

#include "platform/platform.h"

#include <cstdint>

/// A hybrid logical clock and the stamp a replicated value carries, so every device orders edits the same way whatever order they arrive in.

namespace mm {

/// When a value was written and by whom: the clock reading, then the writer's id, which breaks a tie between two devices' equal readings.
struct Stamp {
    uint64_t hlc = 0;      ///< the hybrid logical clock's reading, milliseconds in the high 48 bits and a counter in the low 16
    uint32_t writer = 0;   ///< the writing device's id, the low four bytes of its MAC

    /// Whether this stamp is newer than `o`: the later reading, and the higher writer on a tie, the last-writer-wins order.
    bool newerThan(const Stamp& o) const MM_NONBLOCKING { return hlc != o.hlc ? hlc > o.hlc : writer > o.writer; }
};

/// A hybrid logical clock (Kulkarni et al., *Logical Physical Clocks*, 2014): a reading never runs behind any the device has seen, so an edit made after hearing another is stamped newer.
///
/// @technical
///
/// **Which physical clock.** The device's uptime until clocks are synced across a group, so concurrent edits converge on one value while which one wins is arbitrary; a restored stamp is observed at boot, so the readings keep growing across a restart.
class Hlc {
public:
    /// A fresh reading for a local edit by `writer`.
    Stamp now(uint32_t writer) MM_NONBLOCKING {
        advance(0);
        return {last_, writer};
    }

    /// Take in a reading heard from another device or restored from a file, so later local readings are newer than it.
    void observe(uint64_t seen) MM_NONBLOCKING { advance(seen); }

    /// The latest reading, for a test.
    uint64_t last() const MM_NONBLOCKING { return last_; }

private:
    /// The textbook update: the physical part is the largest of ours, the one seen and the clock's; the counter grows when that part does not move.
    void advance(uint64_t seen) MM_NONBLOCKING {
        const uint64_t pt = static_cast<uint64_t>(platform::millis()) << 16;
        const uint64_t l = last_ & ~kCounter, s = seen & ~kCounter;
        const uint64_t top = l > s ? (l > pt ? l : pt) : (s > pt ? s : pt);
        uint64_t counter = 0;
        if (top == l && top == s) counter = ((last_ > seen ? last_ : seen) & kCounter) + 1;
        else if (top == l) counter = (last_ & kCounter) + 1;
        else if (top == s) counter = (seen & kCounter) + 1;
        last_ = top + counter;   // a counter past 16 bits carries into the milliseconds, so the readings keep growing
    }

    static constexpr uint64_t kCounter = 0xFFFF;
    uint64_t last_ = 0;
};

}  // namespace mm
