#pragma once

#include <atomic>

namespace mm {

/// A non-blocking latch guarding a resource two threads reach, in this codebase the sender one core drains while another streams into it.
///
/// @moreinfo
///
/// ## Only try-acquire, never a blocking lock
///
/// The hot-path rule forbids a render or encode thread blocking on a peer, so a caller that loses the race skips its slot rather than waiting.
/// With nothing to sleep on or wake, each side is one atomic read-modify-write, with no mutex, no lifecycle and no recursion.
///
/// ## Release is an exchange too, not a store
///
/// On an ESP32 with PSRAM, a read-modify-write on external memory is emulated as a read and a write under a lock that a plain store bypasses.
/// A release landing between the two is overwritten, leaving the latch set with no owner, so release is an exchange, under the same lock.
///
/// ## Why it lives in core rather than the platform layer
///
/// It is portable code with no hardware behind it, so it fails the platform layer's charter, and any module coordinating two threads can take it.
class TryLock {
public:
    /// Take the latch when it is free, false when another thread holds it.
    bool tryAcquire() { return !flag_.exchange(true, std::memory_order_acquire); }
    /// Give it back, as a read-modify-write: @xref{release-is-an-exchange-too-not-a-store}.
    void release() { flag_.exchange(false, std::memory_order_release); }

private:
    std::atomic<bool> flag_{false};
};

/// A scope guard releasing on exit and doing nothing when the latch was busy, the standard try-to-lock shape.
class LockGuard {
public:
    /// Tries the latch once; test the guard to learn whether it took.
    explicit LockGuard(TryLock& l) : lock_(l), held_(l.tryAcquire()) {}
    /// Releases only what it took.
    ~LockGuard() { if (held_) lock_.release(); }
    /// Whether this guard holds the latch.
    explicit operator bool() const { return held_; }

    /// Non-copyable: two guards would release one latch twice.
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    TryLock& lock_;
    bool held_;
};

}  // namespace mm
