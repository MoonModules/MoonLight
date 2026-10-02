#pragma once

#include "core/util/HostList.h"   // kMaxHostName
#include "core/util/TryLock.h"    // the cache is shared between the asking thread and the lookup task
#include "platform/platform.h"    // resolveHost, millis, and the worker task seam

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

namespace mm {

/// A cache that answers a host name's last known address at once, while one low-priority task looks names up.
///
/// The asking side runs on the render thread, so it only ever reads the cache: a lookup there would stall a frame for the resolver's timeout.
/// A new name answers `Waiting` until it first resolves; a name that stops resolving answers `Stale` with its last address.
/// Names are looked up again about once a minute, and the cache costs nothing until an owner with a name starts it.
class HostResolver {
public:
    /// What the cache knows about a name.
    enum class State : uint8_t {
        Busy,       ///< the lookup task held the cache this instant; keep what you had
        Waiting,    ///< no address yet, the first lookup pending
        Resolved,   ///< the address, confirmed by the latest lookup
        Stale,      ///< the last known address, the latest lookup having failed
        Full,       ///< every slot holds a name asked for recently
    };

    /// Allocate the cache and start the lookup task, once, from a path that may allocate: a prepare or a control edit.
    static void ensureStarted() {
        Self& s = self();
        if (s.slots) return;
        s.slots = new (std::nothrow) Slot[kSlots];
        if (s.slots && !s.testMode) platform::spawnPinnedTask(s.task, "hosts", &run, &s, 5120, 1, -1);
    }

    /// The name's address into `ip` when it is Resolved or Stale; the first ask queues a lookup, and nothing here allocates.
    static State lookup(const char* name, uint8_t ip[4]) {
        Self& s = self();
        if (!s.slots) return State::Waiting;   // not started: the owner's prepare starts it
        LockGuard guard(s.lock);
        if (!guard) return State::Busy;
        const uint32_t now = platform::millis();
        Slot* free = nullptr;
        for (uint8_t i = 0; i < kSlots; i++) {
            Slot& slot = s.slots[i];
            if (!slot.name[0] || now - slot.askedMs > kForgetMs) { if (!free) free = &slot; continue; }
            if (std::strcmp(slot.name, name) != 0) continue;
            slot.askedMs = now;
            if (!slot.known) return State::Waiting;
            std::memcpy(ip, slot.ip, 4);
            return slot.stale ? State::Stale : State::Resolved;
        }
        if (!free) return State::Full;
        *free = Slot{};
        const size_t n = strnlen(name, kMaxHostName);
        std::memcpy(free->name, name, n);   // the slot is zeroed above, so the name ends there
        free->askedMs = now;
        if (s.task.impl) platform::notifyTask(s.task);
        return State::Waiting;
    }

    /// Replace the platform lookup and keep the task from starting, so a test drives the passes itself.
    static void useForTest(bool (*resolve)(const char*, uint8_t*)) {
        Self& s = self();
        s.resolve = resolve;
        s.testMode = true;
        ensureStarted();
        for (uint8_t i = 0; i < kSlots; i++) s.slots[i] = Slot{};
    }

    /// Run one lookup pass on the calling thread, as the task would.
    static void passForTest() { pass(self()); }

private:
    static constexpr uint8_t  kSlots = 16;
    static constexpr uint32_t kRefreshMs = 60u * 1000u;        ///< look a resolved name up again after this
    static constexpr uint32_t kRetryMs = 5u * 1000u;           ///< retry a name that has not resolved yet after this
    static constexpr uint32_t kForgetMs = 10u * 60u * 1000u;   ///< free a slot nobody asked about for this long

    struct Slot {
        char     name[kMaxHostName + 1] = {};
        uint8_t  ip[4] = {};
        uint32_t askedMs = 0;     ///< when a caller last asked, which keeps the slot
        uint32_t lookedMs = 0;    ///< when the task last looked it up
        bool     looked = false;  ///< the task has tried at least once
        bool     known = false;   ///< an address has been resolved at least once
        bool     stale = false;   ///< the latest lookup failed, the address being the last known
    };

    struct Self {
        Slot* slots = nullptr;
        TryLock lock;
        platform::WorkerTask task;
        bool (*resolve)(const char*, uint8_t*) = &platform::resolveHost;
        bool testMode = false;
    };

    static Self& self() { static Self s; return s; }

    /// The task: a pass whenever a name is added, and every few seconds for the refreshes.
    static void run(void* user) {
        Self& s = *static_cast<Self*>(user);
        for (;;) {
            platform::waitNotify(s.task, kRetryMs);
            pass(s);
        }
    }

    /// Look up every name that is due, holding the cache only to copy a name in and an answer out.
    static void pass(Self& s) {
        for (uint8_t i = 0; i < kSlots; i++) {
            char name[kMaxHostName + 1];
            {
                while (!s.lock.tryAcquire()) {}   // the asking side holds it for microseconds
                Slot& slot = s.slots[i];
                const uint32_t now = platform::millis();
                // A name nobody asked for in ten minutes is freed rather than looked up forever.
                if (slot.name[0] && now - slot.askedMs > kForgetMs) slot = Slot{};
                const bool due = slot.name[0]
                    && (!slot.looked || now - slot.lookedMs >= (slot.known ? kRefreshMs : kRetryMs));
                std::memcpy(name, slot.name, sizeof(name));
                s.lock.release();
                if (!due) continue;
            }
            uint8_t ip[4] = {};
            const bool ok = s.resolve(name, ip);   // blocking, with the cache free
            while (!s.lock.tryAcquire()) {}
            Slot& slot = s.slots[i];
            if (std::strcmp(slot.name, name) == 0) {   // the slot may have been reused meanwhile
                slot.looked = true;
                slot.lookedMs = platform::millis();
                if (ok) { std::memcpy(slot.ip, ip, 4); slot.known = true; slot.stale = false; }
                else if (slot.known) slot.stale = true;
            }
            s.lock.release();
        }
    }
};

}  // namespace mm
