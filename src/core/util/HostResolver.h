#pragma once

#include "core/util/HostList.h"   // kMaxHostName
#include "core/util/TryLock.h"    // the cache is shared between the asking thread and the lookup task
#include "platform/platform.h"    // resolveHost, millis, yield, and the worker task seam

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

namespace mm {

/// A cache that answers a host name's last known address at once, while one low-priority task looks names up.
///
/// The asking side runs on the render thread and only reads the cache, since a lookup there stalls a frame; a new name answers `Waiting`, a failing one `Stale`.
///
/// A board with no worker task looks names up from the asking side instead, one non-blocking step per ask.
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
        if (s.slots && !s.testMode) s.inlineSteps = !platform::spawnPinnedTask(s.task, "hosts", &run, &s, 5120, 1, -1);
    }

    /// The name's address into `ip` when it is Resolved or Stale; the first ask queues a lookup, and nothing here allocates.
    static State lookup(const char* name, uint8_t ip[4]) {
        Self& s = self();
        if (!s.slots) return State::Waiting;   // not started: the owner's prepare starts it
        LockGuard guard(s.lock);
        if (!guard) return State::Busy;
        const uint32_t now = platform::millis();
        if (s.inlineSteps) step(s, now);
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

    // For a test's scope, an empty cache and no task: a blocking resolve driven by passes, or a non-blocking step each lookup drives.
    struct LookupForTest {
        explicit LookupForTest(bool (*resolve)(const char*, uint8_t*)) { use(resolve, &platform::resolveHostStep, false); }
        explicit LookupForTest(platform::LookupStep (*step)(const char*, uint8_t*)) { use(&platform::resolveHost, step, true); }
        ~LookupForTest() { use(&platform::resolveHost, &platform::resolveHostStep, false); }   // so no later test inherits the fake
        LookupForTest(const LookupForTest&) = delete;
        LookupForTest& operator=(const LookupForTest&) = delete;
    private:
        static void use(bool (*resolve)(const char*, uint8_t*), platform::LookupStep (*step)(const char*, uint8_t*), bool inlineSteps) {
            Self& s = self();
            s.resolve = resolve;
            s.resolveStep = step;
            s.inlineSteps = inlineSteps;
            s.inFlight = -1;
            s.testMode = true;
            ensureStarted();
            for (uint8_t i = 0; i < kSlots; i++) s.slots[i] = Slot{};
        }
    };

    // Run one lookup pass on the calling thread, as the task would.
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
        platform::LookupStep (*resolveStep)(const char*, uint8_t*) = &platform::resolveHostStep;
        bool inlineSteps = false;   ///< no worker task: lookup() drives the lookups
        int8_t inFlight = -1;       ///< the slot the inline lookup in flight belongs to
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
                while (!s.lock.tryAcquire()) platform::yield();   // the asking side holds it for microseconds
                Slot& slot = s.slots[i];
                const uint32_t now = platform::millis();
                const bool lookUp = due(slot, now);
                std::memcpy(name, slot.name, sizeof(name));
                s.lock.release();
                if (!lookUp) continue;
            }
            uint8_t ip[4] = {};
            const bool ok = s.resolve(name, ip);   // blocking, with the cache free
            while (!s.lock.tryAcquire()) platform::yield();
            Slot& slot = s.slots[i];
            if (std::strcmp(slot.name, name) == 0) settle(slot, ok, ip, platform::millis());   // the slot may have been reused meanwhile
            s.lock.release();
        }
    }

    /// One non-blocking step on the asking side, with the cache held: continue the lookup in flight, else start the next name due.
    static void step(Self& s, uint32_t now) {
        if (s.inFlight < 0) {
            for (uint8_t i = 0; i < kSlots && s.inFlight < 0; i++)
                if (due(s.slots[i], now)) s.inFlight = static_cast<int8_t>(i);
            if (s.inFlight < 0) return;
        }
        Slot& slot = s.slots[s.inFlight];
        uint8_t ip[4] = {};
        const platform::LookupStep r = slot.name[0] ? s.resolveStep(slot.name, ip) : platform::LookupStep::Failed;
        if (r == platform::LookupStep::Pending) return;
        if (slot.name[0]) settle(slot, r == platform::LookupStep::Resolved, ip, now);
        s.inFlight = -1;
    }

    /// Whether a slot's name is due a lookup, freeing first a name nobody asked for in ten minutes rather than looking it up forever.
    static bool due(Slot& slot, uint32_t now) {
        if (slot.name[0] && now - slot.askedMs > kForgetMs) slot = Slot{};
        return slot.name[0] && (!slot.looked || now - slot.lookedMs >= (slot.known ? kRefreshMs : kRetryMs));
    }

    /// Record a lookup's answer: the address when it resolved, else the last known one marked stale.
    static void settle(Slot& slot, bool ok, const uint8_t ip[4], uint32_t now) {
        slot.looked = true;
        slot.lookedMs = now;
        if (ok) { std::memcpy(slot.ip, ip, 4); slot.known = true; slot.stale = false; }
        else if (slot.known) slot.stale = true;
    }
};

}  // namespace mm
