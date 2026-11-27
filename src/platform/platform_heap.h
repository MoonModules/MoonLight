#pragma once
/// The platform's heap: allocation, PSRAM against internal RAM, executable memory and the free and total figures per type.
///
/// One concern of the platform interface ([platform.h](platform.h) gathers them all), so a file includes the concerns it uses and an edit to one rebuilds its users alone.
///
/// @technical
///
/// ## Internal RAM for what an interrupt reads
///
/// A PSRAM-resident encode source measured about 595 microseconds per slice refill against a 151 microsecond drain budget. So a buffer an interrupt reads per byte comes from `allocInternal` rather than the PSRAM-first `alloc`.
///

#include "platform/platform_base.h"   // target flags, MM_NONBLOCKING and the compiler macros
#include <new>      // std::nothrow_t, for InternalRamObject's allocation

namespace mm::platform {

#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// Report this much free internal RAM, as a PSRAM-less board under load would; 0 restores the host's unreported figure.
void setTestFreeInternalHeap(size_t bytes);
/// Report addresses in [begin, end) as PSRAM, as a board with PSRAM places a buffer; null restores the host's none.
void setTestPsramRange(const void* begin, const void* end);
#endif

/// Allocate, preferring PSRAM where the target has it.
void* alloc(size_t bytes);
/// Release what `alloc` returned.
void free(void* ptr);

/// Allocate internal RAM only, for buffers a hot ISR reads per byte; free with `free`: @xref{internal-ram-for-what-an-interrupt-reads}.
void* allocInternal(size_t bytes);

/// A type a cache-safe interrupt reads inherits this, so `new` places it in internal RAM. PSRAM sits behind the flash cache, which a flash write on the other core can switch off mid-interrupt.
struct InternalRamObject {
    /// Internal RAM, or null when there is none to give.
    static void* operator new(size_t bytes) noexcept { return allocInternal(bytes); }
    /// The same, for `new (std::nothrow)`.
    static void* operator new(size_t bytes, const std::nothrow_t&) noexcept { return allocInternal(bytes); }
    /// Return the allocation above.
    static void operator delete(void* ptr) noexcept { free(ptr); }
};

/// Record what the system holds before MoonLight starts, and on a computer the thread it starts on, once, at the top of mm_main.
void recordSystemHeap();
/// What the system took before MoonLight started, less the task stacks then running, which the task snapshot counts: total and the PSRAM part, 0 where the platform cannot tell.
void systemHeap(size_t& total, size_t& psram);
/// One memory type's heap read in one pass: its size, what is free, its largest free block, and its block headers (used less allocated).
struct HeapType { size_t total = 0, free = 0, largest = 0, overhead = 0; };
/// Internal RAM's and PSRAM's heap, one walk of each, so a sheet reads them together; PSRAM's zeros where the board has none, and zeros where the platform reports no heap.
void heapTypes(HeapType& internal, HeapType& psram);
/// What MoonLight's own code holds, counted where it allocates (alloc, allocInternal, new, and on an ESP32 allocCaps): total and the PSRAM part, 0 where it is uncounted.
void moonlightHeap(size_t& total, size_t& psram);

/// A computer's own memory sheet: the process's resident memory and the machine's available memory, 0 on a board, whose heap figures say it.
void processMemory(size_t& resident, size_t& available);

/// Bytes taken on purpose through alloc and allocInternal.
size_t allocatedBytes();
/// The high-water mark of allocatedBytes.
size_t allocatedPeak();
/// Live block count, separating one grown buffer from a per-frame allocation.
uint32_t allocatedCount();

/// True when the pointer resolves to external (PSRAM) memory; always false on host.
bool ptrIsPsram(const void* p);

/// Allocate memory the CPU can fetch from, for MoonLive's emitted code; nullptr when exhausted.
void* allocExec(size_t bytes);
/// Release an allocExec block; the size is for backends that need it, and ESP32 ignores it.
void  freeExec(void* ptr, size_t bytes);

/// Copy emitted code into an allocExec block and make it executable; `len` may be any length.
void  writeExec(void* dst, const void* src, size_t len);
/// Total free heap, internal plus PSRAM where the target has it.
size_t freeHeap();
/// Free internal RAM, which is what the stack, HTTP and WiFi reserve is checked against.
size_t freeInternalHeap();
/// Largest contiguous block of any memory type, PSRAM included.
size_t maxAllocBlock();
/// Largest contiguous block of internal RAM, the scarce one, so this is the memory-pressure KPI.
size_t maxInternalAllocBlock();

/// Largest contiguous block of executable memory, which is what bounds a MoonLive script's size.
size_t maxExecAllocBlock();

/// Cap what maxAllocBlock reports, 0 being no cap, so a test reaches the paged-destinations fallback on an unfragmented heap.
void setTestMaxAllocBlock(size_t bytes);
/// Refuse every allocation of at least `bytes`, 0 being none refused, so a test meets out-of-memory on a growing buffer. Host only.
void setTestAllocRefusedFrom(size_t bytes);
/// Total heap capacity, internal plus PSRAM.
size_t totalHeap();
/// Total internal heap capacity.
size_t totalInternalHeap();

/// Heap to keep free for stack, HTTP, WiFi and overhead; any allocator checks against this before committing.
constexpr size_t HEAP_RESERVE = 32768;
/// Whether `bytes` fits one free block with the reserve left intact, the check every growing allocation makes first; a platform reporting 0 for a figure sets no limit there.
inline bool fitsWithReserve(size_t bytes) {
    const size_t block = maxAllocBlock();
    if (block != 0 && block < bytes) return false;
    const size_t free = freeHeap();
    return free == 0 || (free > HEAP_RESERVE && free - HEAP_RESERVE >= bytes);
}
/// The internal RAM a board keeps free, which an allocator that takes what is left stops at: WiFi, HTTP and the UI need it.
constexpr size_t kInternalFreeFloor = 100 * 1024;

}  // namespace mm::platform
