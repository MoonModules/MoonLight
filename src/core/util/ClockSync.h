#pragma once

#include "platform/platform.h"   // uptimeUs: the local clock fleet time is read against

#include <atomic>
#include <cstdint>

namespace mm {

/// The fleet clock's arithmetic: NTP's on-wire exchange, its clock filter and a discipline that never runs the clock backwards, each a pure function of the times it is given.
///
/// @technical
///
/// ## The exchange
///
/// A device asks the reference at its own time t1; the reference stamps the arrival t2 and its reply t3 on its fleet clock; the device stamps the reply's arrival t4 (RFC 5905 § 8).
///
/// The offset is ((t2 - t1) + (t3 - t4)) / 2 and the round trip (t4 - t1) - (t3 - t2); the true offset lies within half the round trip of the estimate, whatever the paths' asymmetry.
///
/// ## The clock filter
///
/// Queueing only ever adds delay, so of the last eight samples the one with the least round trip carries the least error, NTP's clock filter (RFC 5905 § 10).
///
/// ## Slew, or step
///
/// A correction under kStepUs is slewed at kSlewPpm, so the fleet clock runs at most 0.05 % fast or slow and never backwards under an effect; a larger one, and the first, is stepped, as NTP steps past 128 ms.

/// One exchange's result: the reference's clock minus the asker's, and the round trip, in microseconds.
struct ClockSample {
    int64_t offsetUs = 0;   ///< the reference's fleet time minus the asker's local time
    int64_t delayUs = 0;    ///< the round trip, the reference's own turnaround taken out
};

/// The sample from one exchange's four times: t1 asked and t4 answered on the asker's clock, t2 arrived and t3 replied on the reference's: @xref{the-exchange}.
inline ClockSample clockSample(int64_t t1, int64_t t2, int64_t t3, int64_t t4) {
    return {((t2 - t1) + (t3 - t4)) / 2, (t4 - t1) - (t3 - t2)};
}

/// The last kSamples samples, the one with the least round trip trusted: @xref{the-clock-filter}.
class ClockFilter {
public:
    /// How many samples it keeps.
    static constexpr uint8_t kSamples = 8;

    /// Keep `s`, the oldest giving way; a negative round trip, which no real exchange has, is refused.
    bool add(const ClockSample& s) {
        if (s.delayUs < 0) return false;
        samples_[next_] = s;
        next_ = static_cast<uint8_t>((next_ + 1) % kSamples);
        if (count_ < kSamples) count_++;
        return true;
    }

    /// The sample with the least round trip into `out`; false while none is kept.
    bool best(ClockSample& out) const {
        if (!count_) return false;
        uint8_t at = 0;
        for (uint8_t i = 1; i < count_; i++) if (samples_[i].delayUs < samples_[at].delayUs) at = i;
        out = samples_[at];
        return true;
    }

    /// How many samples it holds.
    uint8_t count() const { return count_; }

    /// Forget every sample, as when the reference changes.
    void clear() { count_ = 0; next_ = 0; }

private:
    ClockSample samples_[kSamples] = {};
    uint8_t count_ = 0, next_ = 0;
};

/// The offset that turns local time into fleet time, steered toward each new estimate: @xref{slew-or-step}.
class ClockDiscipline {
public:
    /// The largest correction slewed; a larger one steps.
    static constexpr int64_t kStepUs = 128000;
    /// The slew rate, in microseconds per second: NTP's 500 ppm.
    static constexpr int64_t kSlewPpm = 500;

    /// Whether an estimate was taken since the start.
    bool synced() const MM_NONBLOCKING { return synced_; }

    /// The offset in force at local time `localUs`.
    int64_t offsetAt(int64_t localUs) const MM_NONBLOCKING {
        if (!remainingUs_) return baseUs_;
        const int64_t elapsed = localUs > baseAtUs_ ? localUs - baseAtUs_ : 0;
        const int64_t reach = elapsed * kSlewPpm / 1000000;
        const int64_t moved = remainingUs_ > 0 ? (reach < remainingUs_ ? reach : remainingUs_)
                                               : (-reach > remainingUs_ ? -reach : remainingUs_);
        return baseUs_ + moved;
    }

    /// Fleet time at local time `localUs`.
    int64_t fleetAt(int64_t localUs) const MM_NONBLOCKING { return localUs + offsetAt(localUs); }

    /// Steer toward `targetOffsetUs`, estimated at local time `localUs`: the first estimate and one past kStepUs step, a smaller one slews.
    void steer(int64_t localUs, int64_t targetOffsetUs) {
        const int64_t now = offsetAt(localUs);
        const int64_t delta = targetOffsetUs - now;
        if (!synced_ || delta > kStepUs || delta < -kStepUs) {
            baseUs_ = targetOffsetUs;
            remainingUs_ = 0;
        } else {
            baseUs_ = now;
            remainingUs_ = delta;
        }
        baseAtUs_ = localUs;
        synced_ = true;
    }

    /// Back to unsynced, offset zero.
    void reset() { *this = ClockDiscipline{}; }

    /// Take this clock as the fleet's own, as the reference does: synced, its offset kept, so a member that becomes the reference keeps the time it had.
    void claim() { synced_ = true; }

    /// The four values that make the discipline, for publishing it to other tasks.
    struct State { int64_t baseUs, baseAtUs, remainingUs; bool synced; };
    /// Its state.
    State state() const MM_NONBLOCKING { return {baseUs_, baseAtUs_, remainingUs_, synced_}; }
    /// A discipline holding `s`.
    static ClockDiscipline from(const State& s) MM_NONBLOCKING {
        ClockDiscipline d;
        d.baseUs_ = s.baseUs; d.baseAtUs_ = s.baseAtUs; d.remainingUs_ = s.remainingUs; d.synced_ = s.synced;
        return d;
    }

private:
    int64_t baseUs_ = 0;        ///< the offset when the current slew began
    int64_t baseAtUs_ = 0;      ///< the local time it began
    int64_t remainingUs_ = 0;   ///< what the slew still has to move
    bool synced_ = false;
};

/// One published copy of the fleet clock's discipline, its count odd while it is written: each 64-bit value as two 32-bit halves, since a 32-bit core has no lock-free 64-bit atomic and the count already keeps the halves together.
struct FleetTimeSlot {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> half[6] = {};   ///< base, baseAt and remaining, low half first
    std::atomic<bool> synced{false};
    /// Store `v` as value `i`'s two halves.
    void put(int i, int64_t v) MM_NONBLOCKING {
        half[2 * i].store(static_cast<uint32_t>(static_cast<uint64_t>(v)), std::memory_order_relaxed);
        half[2 * i + 1].store(static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32), std::memory_order_relaxed);
    }
    /// Value `i`, from its two halves.
    int64_t get(int i) const MM_NONBLOCKING {
        return static_cast<int64_t>((static_cast<uint64_t>(half[2 * i + 1].load(std::memory_order_relaxed)) << 32) | half[2 * i].load(std::memory_order_relaxed));
    }
};

/// The two slots and which one readers are on: the one process-wide state every task reads fleet time from.
struct FleetTimePublished {
    FleetTimeSlot slots[2];
    std::atomic<uint8_t> active{0};
};

/// Fleet time for any task: the local clock plus the discipline the fleet clock's task publishes, read without a lock.
///
/// Two slots: the writer fills the one readers are not on, each slot's count odd while it is written, then points readers at it, so a reader that preempts the writer mid-write still reads a whole discipline.
class FleetTime {
public:
    /// Publish `d`, from the one task that steers it.
    static void publish(const ClockDiscipline& d) MM_NONBLOCKING {
        const ClockDiscipline::State s = d.state();
        FleetTimeSlot& w = published_.slots[published_.active.load(std::memory_order_relaxed) ^ 1];
        w.seq.fetch_add(1, std::memory_order_acq_rel);
        w.put(0, s.baseUs);
        w.put(1, s.baseAtUs);
        w.put(2, s.remainingUs);
        w.synced.store(s.synced, std::memory_order_relaxed);
        w.seq.fetch_add(1, std::memory_order_release);
        published_.active.store(static_cast<uint8_t>(&w - published_.slots), std::memory_order_release);
    }

    /// The published discipline.
    static ClockDiscipline current() MM_NONBLOCKING {
        for (;;) {
            const FleetTimeSlot& r = published_.slots[published_.active.load(std::memory_order_acquire)];
            const uint32_t before = r.seq.load(std::memory_order_acquire);
            if (before & 1) continue;   // only when two publishes ran during this read
            const ClockDiscipline::State s{r.get(0), r.get(1), r.get(2), r.synced.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            if (r.seq.load(std::memory_order_relaxed) == before) return ClockDiscipline::from(s);
        }
    }

    /// Fleet time now, in microseconds; the local clock until the first sync.
    static int64_t nowUs() MM_NONBLOCKING { return current().fleetAt(static_cast<int64_t>(platform::uptimeUs())); }

    /// The offset in force now, which turns any local reading into fleet time.
    static int64_t offsetUs() MM_NONBLOCKING { return current().offsetAt(static_cast<int64_t>(platform::uptimeUs())); }

    /// Whether this device's fleet time follows a reference, or is the reference's own.
    static bool synced() MM_NONBLOCKING { return current().synced(); }

    /// Back to the local clock, for a test.
    static void reset() MM_NONBLOCKING { publish(ClockDiscipline{}); }

private:
    static inline FleetTimePublished published_;
};

}  // namespace mm
