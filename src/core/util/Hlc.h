#pragma once

#include "platform/platform.h"

#include <cstdint>
#include <cstdlib>

/// A hybrid logical clock and the stamp a replicated value carries, so every device orders edits the same way whatever order they arrive in.

namespace mm {

/// When a value was written and by whom: the clock reading, then the writer's id, which breaks a tie between two devices' equal readings.
struct Stamp {
    uint64_t hlc = 0;      ///< the hybrid logical clock's reading, milliseconds in the high 48 bits and a counter in the low 16
    uint32_t writer = 0;   ///< the writing device's id, the low four bytes of its MAC

    /// Whether this stamp is newer than `o`: the later reading, and the higher writer on a tie, the last-writer-wins order.
    bool newerThan(const Stamp& o) const MM_NONBLOCKING { return hlc != o.hlc ? hlc > o.hlc : writer > o.writer; }

    /// The largest valid reading, 2^63 - 1: thousands of years of milliseconds, which no clock reaches and only a corrupt or crafted stamp passes.
    static constexpr uint64_t kMaxHlc = (uint64_t{1} << 63) - 1;

    /// Whether the reading is one a clock reaches, at most kMaxHlc.
    bool valid() const MM_NONBLOCKING { return hlc <= kMaxHlc; }

    /// Read a stamp saved as "hlc:writer" in hex, the whole of `text`; false when it is not one or not valid.
    static bool parse(const char* text, Stamp& out) {
        char* end = nullptr;
        const unsigned long long h = std::strtoull(text, &end, 16);
        if (end == text || *end != ':') return false;
        const char* w = end + 1;
        const unsigned long long writer = std::strtoull(w, &end, 16);
        if (end == w || *end || writer > UINT32_MAX) return false;
        const Stamp s{static_cast<uint64_t>(h), static_cast<uint32_t>(writer)};
        if (!s.valid()) return false;
        out = s;
        return true;
    }
};

/// A hybrid logical clock (Kulkarni et al., *Logical Physical Clocks*, 2014): each reading is newer than every reading the device has seen, so an edit made after hearing another is stamped newer.
///
/// @technical
///
/// **Which physical clock.** The device's uptime until clocks are synced across a group, so concurrent edits converge on one value while which one wins is arbitrary; a restored stamp is observed at boot, so the readings keep growing across a restart.
///
/// **Where the readings stop.** At Stamp::kMaxHlc, the largest reading every peer accepts: a clock a crafted stamp drove there still stamps edits the peers take, the writer breaking ties among them.
class Hlc {
public:
    /// A fresh reading for a local edit by `writer`.
    Stamp now(uint32_t writer) MM_NONBLOCKING {
        advance(0);
        return {last_, writer};
    }

    /// Take in a reading heard from another device or restored from a file, so later local readings are newer than it; an invalid one is ignored, so the clock keeps room to count up.
    void observe(uint64_t seen) MM_NONBLOCKING { if (Stamp{seen, 0}.valid()) advance(seen); }

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
        if (last_ > Stamp::kMaxHlc) last_ = Stamp::kMaxHlc;   // the sum reaches 2^63 at most, well inside a uint64_t
    }

    static constexpr uint64_t kCounter = 0xFFFF;
    uint64_t last_ = 0;
};

}  // namespace mm
