#pragma once

#include "platform/platform.h"   // currentThreadId: the key a slot is claimed under

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mm {

/// @defgroup ThreadSlot Per-thread slots
/// @{
/// A slot a thread claims by its id, for state a callback reaches without a receiver.
///
/// `thread_local` dies on an ESP32 task without TLS, so a small fixed table keyed on the thread id stands in for it.
/// A slot type carries `std::atomic<uintptr_t> owner`, where zero means free.

// `owner` is claimed with compare_exchange, since a load then a store let two threads share a slot.
/// The slot this thread owns in `slots`, taking a free one when `claim` is set; null when none is free.
template <class Slot, size_t N>
inline Slot* ownedThreadSlot(Slot (&slots)[N], bool claim) MM_NONBLOCKING {
    const uintptr_t me = platform::currentThreadId();
    for (size_t i = 0; i < N; i++)
        if (slots[i].owner.load(std::memory_order_acquire) == me) return &slots[i];
    if (!claim) return nullptr;
    for (size_t i = 0; i < N; i++) {
        uintptr_t free = 0;
        if (slots[i].owner.compare_exchange_strong(free, me, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed))
            return &slots[i];
    }
    return nullptr;
}

/// Hand a slot back, once everything it carries is detached.
template <class Slot>
inline void releaseThreadSlot(Slot* s) MM_NONBLOCKING {
    if (s) s->owner.store(0, std::memory_order_release);
}

/// @}

}  // namespace mm
