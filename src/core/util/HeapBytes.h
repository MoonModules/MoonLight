#pragma once
/// Heap bytes split by memory type, the figure every owner reports to the memory sheet.
///
/// A board with PSRAM has two kinds of RAM, and where a buffer lives matters as much as its size. So an owner reports its bytes with the part in PSRAM, read from each allocation's own address, and the sheet balances each type on its own.

#include "platform/platform_heap.h"   // ptrIsPsram

#include <cstddef>
#include <cstdint>

namespace mm {

/// Heap bytes and the part of them in PSRAM; a plain size counts as internal RAM. 32 bits each, as the readout writes them: one owner's heap stays under 4 GB, and a driver keeps one of these per object.
struct HeapBytes {
    uint32_t total = 0;   ///< every byte, internal RAM and PSRAM
    uint32_t psram = 0;   ///< the part of `total` in PSRAM
    /// A size and its PSRAM part; a size alone is internal RAM, which is what an allocation of no known address is counted as.
    HeapBytes(size_t t = 0, size_t p = 0) : total(static_cast<uint32_t>(t)), psram(static_cast<uint32_t>(p)) {}   // NOLINT(google-explicit-constructor)
    /// The part in internal RAM.
    size_t internal() const { return total - psram; }
    /// Add another owner's bytes.
    HeapBytes& operator+=(const HeapBytes& o) { total += o.total; psram += o.psram; return *this; }
    /// Take bytes back out, each part stopping at zero and the PSRAM part within the total, so a buffer that moved between types since it was counted takes out no more than is there.
    HeapBytes& operator-=(const HeapBytes& o) {
        total = total > o.total ? total - o.total : 0;
        psram = psram > o.psram ? psram - o.psram : 0;
        if (psram > total) psram = total;
        return *this;
    }
    /// The sum of two.
    friend HeapBytes operator+(HeapBytes a, const HeapBytes& b) { return a += b; }
    /// The difference of two.
    friend HeapBytes operator-(HeapBytes a, const HeapBytes& b) { return a -= b; }
    /// Equal in both parts.
    bool operator==(const HeapBytes& o) const { return total == o.total && psram == o.psram; }
};

/// The `bytes` at `p`, counted as PSRAM where they live there.
inline HeapBytes heapOf(const void* p, size_t bytes) { return {bytes, platform::ptrIsPsram(p) ? bytes : 0}; }

} // namespace mm
