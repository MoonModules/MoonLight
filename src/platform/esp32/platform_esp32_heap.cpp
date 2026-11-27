/// MoonLight's own heap on an ESP32: the counted allocator behind alloc and new, the heap_caps blocks the platform takes, and the figures MoonShine's memory sheet reads, each counted where it is taken.

#include "platform/platform.h"
#include "platform/esp32/platform_esp32_heap.h"

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"   // esp_ptr_external_ram: the ptrIsPsram residency probe
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"      // uxTaskGetNumberOfTasks: the network scope walks the task table when it moved

#include <algorithm>
#include <atomic>
#include <cstddef>   // std::max_align_t, the alignment allocInternal asks for
#include <cstdlib>
#include <new>       // std::nothrow_t, the replaced allocation functions

namespace mm::platform {
// MoonLight's own heap, counted where it allocates: alloc, allocInternal, new and allocCaps. The size is the block's own, read from its header, so taking and giving back agree.
static std::atomic<size_t> ownBytes_{0}, ownPsram_{0};
static void countTaken(void* p) {
    if (!p) return;
    const size_t n = heap_caps_get_allocated_size(p);
    ownBytes_.fetch_add(n, std::memory_order_relaxed);
    if (esp_ptr_external_ram(p)) ownPsram_.fetch_add(n, std::memory_order_relaxed);
}
static void countGiven(void* p) {
    if (!p) return;
    const size_t n = heap_caps_get_allocated_size(p);
    ownBytes_.fetch_sub(n, std::memory_order_relaxed);
    if (esp_ptr_external_ram(p)) ownPsram_.fetch_sub(n, std::memory_order_relaxed);
}
static void* counted(void* p) { countTaken(p); return p; }
}  // namespace mm::platform

// Every form of new and delete is replaced ([new.delete.single]), so the count covers each module, vector and string MoonLight creates.

// A failed `new (std::nothrow)` returns null, not abort(): libstdc++'s form throws, which ESP-IDF aborts on (found by Funkelfetisch).
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return mm::platform::counted(std::malloc(size ? size : 1)); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return mm::platform::counted(std::malloc(size ? size : 1)); }
// The throwing forms abort on a refusal, as libstdc++'s do in a build with exceptions off.
void* operator new(std::size_t size) {
    void* p = mm::platform::counted(std::malloc(size ? size : 1));
    if (!p) std::abort();
    return p;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { mm::platform::countGiven(p); std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace mm::platform {

void* alloc(size_t bytes) {
#ifdef CONFIG_SPIRAM
    // Try PSRAM first, fall back to internal RAM
    void* ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) return counted(ptr);
#endif
    return counted(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
}

void* allocInternal(size_t bytes) {
    // Internal only, no PSRAM fallback: the caller chose this seam because PSRAM latency breaks it. Aligned as malloc promises, since InternalRamObject constructs objects here.
    return counted(heap_caps_aligned_alloc(alignof(std::max_align_t), bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

bool ptrIsPsram(const void* p) {
    return p != nullptr && esp_ptr_external_ram(p);
}

void free(void* ptr) {
    countGiven(ptr);
    heap_caps_free(ptr);
}

void* allocCaps(size_t bytes, uint32_t caps, size_t align) {
    return counted(align ? heap_caps_aligned_alloc(align, bytes, caps) : heap_caps_malloc(bytes, caps));
}

void* callocCaps(size_t bytes, uint32_t caps, size_t align) {
    return counted(align ? heap_caps_aligned_calloc(align, 1, bytes, caps) : heap_caps_calloc(1, bytes, caps));
}

void* adoptCaps(void* p) { return counted(p); }

void freeCaps(void* p) { free(p); }

// The same three numbers the host counts by hand, from the allocator that already tracks them. USED rather than free, so the figure means the same on a 320 KB board and a laptop.
size_t allocatedBytes() {
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    return info.total_allocated_bytes;
}
size_t allocatedPeak() {
    // The minimum-ever free, expressed as a peak used: IDF tracks the low-water mark of free heap, which is the same fact from the other side.
    const size_t total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    const size_t minFree = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    return total > minFree ? total - minFree : 0;
}
uint32_t allocatedCount() {
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    return static_cast<uint32_t>(info.allocated_blocks);
}

size_t freeHeap() {
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

size_t freeInternalHeap() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// One memory type from one heap walk: free, the largest block and the block headers (used but allocated to no one) all come from the same info.
static HeapType readType(uint32_t caps) {
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, caps);
    HeapType t;
    t.total = heap_caps_get_total_size(caps);
    t.free = info.total_free_bytes;
    t.largest = info.largest_free_block;
    const size_t used = t.total > t.free ? t.total - t.free : 0;
    t.overhead = used > info.total_allocated_bytes ? used - info.total_allocated_bytes : 0;
    return t;
}

void heapTypes(HeapType& internal, HeapType& psram) {
    internal = readType(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    psram = readType(MALLOC_CAP_SPIRAM);
}

void processMemory(size_t& resident, size_t& available) { resident = available = 0; }   // the heap figures say it

void moonlightHeap(size_t& total, size_t& psram) {
    total = ownBytes_.load(std::memory_order_relaxed);
    psram = std::min(total, ownPsram_.load(std::memory_order_relaxed));
}

static size_t systemHeap_[2] = {};   // total, PSRAM

// Free PSRAM, asked of the heap in one read: the difference of two free sizes read apart wraps when memory is freed between them.
static size_t freePsramHeap() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

void recordSystemHeap() {
    // The task table first: its first read takes a counted scratch, which `used` then holds as `own` does.
    size_t tasks = 0, tasksPsram = 0;
    tasksHeap(tasks, tasksPsram);
    HeapType in, ps;
    heapTypes(in, ps);
    const size_t usedPsram = ps.total - ps.free;
    const size_t used = in.total - in.free + usedPsram;
    const size_t over = in.overhead + ps.overhead, overPsram = ps.overhead;
    // What MoonLight's counted allocations took before mm_main, such as the settings store and static constructors, is MoonLight's line, so the system's leaves it out.
    size_t own = 0, ownPsram = 0;
    moonlightHeap(own, ownPsram);
    const size_t owned = tasks + over + own, ownedPsram = tasksPsram + overPsram + ownPsram;
    systemHeap_[0] = used > owned ? used - owned : 0;
    systemHeap_[1] = std::min(systemHeap_[0], usedPsram > ownedPsram ? usedPsram - ownedPsram : 0);
}

void systemHeap(size_t& total, size_t& psram) {
    total = systemHeap_[0];
    psram = systemHeap_[1];
}

// Test-only cap on the largest-free block, 0 = none. It only lowers the reported value (min with the real block), so a forced-paging test stays honest. Atomic to match the host seam's cross-thread contract.
static std::atomic<size_t> testMaxBlock{0};
void setTestMaxAllocBlock(size_t bytes) { testMaxBlock.store(bytes, std::memory_order_relaxed); }

size_t maxAllocBlock() {
    size_t real = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t cap = testMaxBlock.load(std::memory_order_relaxed);
    return (cap != 0 && cap < real) ? cap : real;
}

size_t maxInternalAllocBlock() {
    // MALLOC_CAP_INTERNAL excludes PSRAM. The internal heap is the scarce resource (WiFi, TCP/IP, FreeRTOS stacks all draw from it); PSRAM is huge by construction so its largest-free-block tells you nothing about memory pressure.
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t maxExecAllocBlock() {
    // The pool allocExec draws from: IRAM on a classic ESP32, tens of KB and invisible in freeInternalHeap. A script that did not fit reported "codegen failed" with 80 KB of DRAM free.
    return heap_caps_get_largest_free_block(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
}

size_t totalHeap() {
    return heap_caps_get_total_size(MALLOC_CAP_8BIT);
}

size_t totalInternalHeap() {
    return heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// What the network's bring-ups took and its shutdowns gave back, apart from the task stacks they started or stopped, which the task snapshot counts. Its calls run on the one task NetworkModule ticks on.
static size_t netHeapTotal_ = 0, netHeapPsram_ = 0;
static uint8_t netScopeDepth_ = 0;
// The task heap at the last walk, walked again only when the task count moved: each walk holds the kernel lock with interrupts off.
static UBaseType_t netTaskCount_ = 0;
static size_t netTasks_ = 0, netTasksPsram_ = 0;

static void readNet(NetHeapScope::Reading& r) {
    r.free = freeHeap();
    r.freePsram = freePsramHeap();
    const UBaseType_t count = uxTaskGetNumberOfTasks();
    if (count != netTaskCount_) { tasksHeap(netTasks_, netTasksPsram_); netTaskCount_ = count; }
    r.tasks = netTasks_;
    r.tasksPsram = netTasksPsram_;
    moonlightHeap(r.own, r.ownPsram);
}

NetHeapScope::NetHeapScope() { if (netScopeDepth_++ == 0) readNet(before_); }

NetHeapScope::~NetHeapScope() {
    if (--netScopeDepth_ != 0) return;
    Reading after;
    readNet(after);
    // Less what the tasks and MoonLight's own counted allocations took meanwhile, an output's frame among them, which their own lines count.
    const long long took = static_cast<long long>(before_.free) - after.free - (static_cast<long long>(after.tasks) - before_.tasks) - (static_cast<long long>(after.own) - before_.own);
    const long long tookPsram = static_cast<long long>(before_.freePsram) - after.freePsram - (static_cast<long long>(after.tasksPsram) - before_.tasksPsram)
                                - (static_cast<long long>(after.ownPsram) - before_.ownPsram);
    netHeapTotal_ = static_cast<size_t>(std::max(0LL, static_cast<long long>(netHeapTotal_) + took));
    netHeapPsram_ = std::min(netHeapTotal_, static_cast<size_t>(std::max(0LL, static_cast<long long>(netHeapPsram_) + tookPsram)));
}

void networkHeap(size_t& total, size_t& psram) {
    total = netHeapTotal_;
    psram = netHeapPsram_;
}

}  // namespace mm::platform
