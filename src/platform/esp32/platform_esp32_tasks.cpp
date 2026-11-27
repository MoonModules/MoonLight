/// The one seam that reads the task table, so no RTOS type escapes the platform layer.
///
/// The module above does the domain work; this file answers what the tasks are.
///
/// @technical
///
/// ## Two SDK options gate what it can answer
///
/// The snapshot call is the textbook introspection one and needs the trace facility; with it off the snapshot reads empty and MoonShine's System tasks band stays empty.
///
/// The per-task CPU figure needs run-time stats, a clock read on every context switch, which measured no cost and is on in every build; with it off the figure reads unmeasured.
///
/// A share covers the interval since the last snapshot, as top reports it.
///
/// ## A task's memory
///
/// A task holds its stack and its control block on the heap: the stack's size comes from its two ends, the table's `pxStackBase` and the snapshot's `pxEndOfStack`, read with the scheduler suspended.
///
/// The base is read under the kernel's lock with the table, so the end, read through the handle afterwards, is checked against it; the control block is internal memory wherever the stack lives.
///
/// ## The scratch buffer is lazy and internal
///
/// Allocated once on its first use, when mm_main records the system's memory and walks the task table, and kept for the process, so every later tick is allocation-free, as the one-second tick's no-heap rule asks.
///
/// On the heap through callocCaps rather than a static array, so the memory sheet counts its 1.7 KB as MoonLight's own.
///
/// The snapshot call wants room for every task or it returns nothing, so the ceiling is generous and exceeding it makes that tick empty rather than partial.
///
/// Internal rather than external memory: the call fills this buffer with the kernel lock held, so every write lands inside a critical section, where a cache miss stalls the most.

#include "platform/platform.h"

#include "sdkconfig.h"

#include <cstring>
#include <cstdio>

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   // the scratch's mutex
#include "freertos/freertos_debug.h"   // vTaskGetSnapshot: a stack's high end
#include "esp_heap_caps.h"   // MALLOC_CAP_*, the snapshot scratch's memory type
#include "platform/esp32/platform_esp32_heap.h"   // allocCaps, freeCaps: MoonLight's blocks, counted
#include "esp_memory_utils.h"   // esp_ptr_external_ram: where a stack lives
#endif

namespace mm::platform {

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY

namespace {
TaskState mapState(eTaskState s) {
    switch (s) {
        case eRunning:   return TaskState::Running;
        case eReady:     return TaskState::Ready;
        case eBlocked:   return TaskState::Blocked;
        case eSuspended: return TaskState::Suspended;
        case eDeleted:   return TaskState::Deleted;
        case eInvalid:   return TaskState::Invalid;
        default:         return TaskState::Unknown;
    }
}
/// A task's heap bytes, stack and control block, `psram` the stack's there; a task the other core deletes meanwhile reads as 0 or one stale figure, accepted since locking that core out stalls the LED refill.
uint32_t heapOfTask(const TaskStatus_t& task, uint32_t& psram) {
    static constexpr size_t kMaxStack = 64 * 1024;   // larger than any task MoonLight or ESP-IDF creates
    psram = 0;
    const StackType_t* start = task.pxStackBase;
    TaskSnapshot_t snap{};
    if (!start || vTaskGetSnapshot(task.xHandle, &snap) != pdTRUE || snap.pxEndOfStack <= start) return 0;
    const size_t stack = static_cast<size_t>(snap.pxEndOfStack - start + 1) * sizeof(StackType_t);   // the stack grows down to start from its end
    if (stack > kMaxStack) return 0;
    if (esp_ptr_external_ram(start)) psram = static_cast<uint32_t>(stack);
    return static_cast<uint32_t>(stack + sizeof(StaticTask_t));
}

/// Each task's heap bytes, read with the scheduler suspended, as freertos_debug.h asks of a snapshot while the scheduler runs.
void fillHeapBytes(TaskInfo* out, const TaskStatus_t* raw, size_t n) {
    vTaskSuspendAll();
    for (size_t i = 0; i < n; i++) out[i].heapBytes = heapOfTask(raw[i], out[i].psramBytes);
    xTaskResumeAll();
}

/// The kernel's task table, read under its lock into a scratch taken once: @xref{the-scratch-buffer-is-lazy-and-internal|why not from boot, and why not external memory}. Null when the scratch is refused.
constexpr size_t kScratch = 40;
// One user of the shared scratch at a time: the render loop's snapshot and the network task's heap scope both read it. A bounded wait, so a reader that loses the race skips one read.
class ScratchLock {
public:
    ScratchLock() : held_(xSemaphoreTake(mutex(), pdMS_TO_TICKS(10)) == pdTRUE) {}
    ~ScratchLock() { if (held_) xSemaphoreGive(mutex()); }
    ScratchLock(const ScratchLock&) = delete;
    ScratchLock& operator=(const ScratchLock&) = delete;
    explicit operator bool() const { return held_; }
private:
    static SemaphoreHandle_t mutex() {
        static StaticSemaphore_t buffer;
        static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&buffer);   // a static local's first use is thread-safe
        return m;
    }
    bool held_;
};
TaskStatus_t* systemState(UBaseType_t& got, uint32_t& totalRunTime) {
    static TaskStatus_t* raw = nullptr;
    if (!raw) raw = static_cast<TaskStatus_t*>(callocCaps(kScratch * sizeof(TaskStatus_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    got = raw ? uxTaskGetSystemState(raw, kScratch, &totalRunTime) : 0;
    return raw;
}
// The table, when `lock` holds the scratch; null when another reader does.
TaskStatus_t* lockedState(const ScratchLock& lock, UBaseType_t& got, uint32_t& totalRunTime) { return lock ? systemState(got, totalRunTime) : nullptr; }
}  // namespace

size_t taskSnapshot(TaskInfo* out, size_t maxTasks) {
    if (!out || maxTasks == 0) return 0;
    uint32_t totalRunTime = 0;
    UBaseType_t got = 0;
    const ScratchLock lock;
    TaskStatus_t* raw = lockedState(lock, got, totalRunTime);
    if (!raw) return 0;   // no scratch, or another reader holds it: the table shows empty this time
    const size_t n = got < maxTasks ? got : maxTasks;
#if configGENERATE_RUN_TIME_STATS == 1   // on in every MoonLight build (sdkconfig.defaults); with it off MoonShine shows no CPU time for a task
    // Each task's counter at the previous snapshot, by task number, so a share covers only the interval since.
    static UBaseType_t prevNumber[kScratch] = {};
    static uint32_t prevRunTime[kScratch] = {};
    static UBaseType_t prevCount = 0;
    static uint32_t prevTotal = 0;
    const uint32_t interval = totalRunTime - prevTotal;
#endif
    for (size_t i = 0; i < n; i++) {
        const TaskStatus_t& t = raw[i];
        TaskInfo& o = out[i];
        std::snprintf(o.name, sizeof(o.name), "%s", t.pcTaskName ? t.pcTaskName : "?");
        o.state = mapState(t.eCurrentState);
        o.priority = static_cast<uint8_t>(t.uxBasePriority);   // as created: the current one carries a borrowed priority while the task holds a mutex
        o.stackFreeBytes = t.usStackHighWaterMark;
        // TaskStatus_t.xCoreID exists only when configTASKLIST_INCLUDE_COREID is set (it is not by default, even on a dual-core chip). Without it we can't know the per-task core, so report
        // -1 (unknown); the current-task-per-core view (core0/core1) still works via a separate call.
    #if defined(configTASKLIST_INCLUDE_COREID) && configTASKLIST_INCLUDE_COREID == 1
        o.core = (t.xCoreID == tskNO_AFFINITY) ? -1 : static_cast<int8_t>(t.xCoreID);
    #else
        o.core = -1;
    #endif
    #if configGENERATE_RUN_TIME_STATS == 1
        uint32_t before = 0;
        for (UBaseType_t k = 0; k < prevCount; k++)
            if (prevNumber[k] == t.xTaskNumber) { before = prevRunTime[k]; break; }
        // A share past the whole is a span across the counter's wrap, after a long unread stretch, so it reads unmeasured until the next read.
        const uint64_t share = interval ? 1000ULL * (t.ulRunTimeCounter - before) / interval : 0;
        o.cpuPermille = share > 1000 ? kTaskCpuUnmeasured : static_cast<uint32_t>(share);
    #else
        o.cpuPermille = kTaskCpuUnmeasured;
        (void)totalRunTime;
    #endif
    }
    fillHeapBytes(out, raw, n);
#if configGENERATE_RUN_TIME_STATS == 1
    for (UBaseType_t i = 0; i < got && i < kScratch; i++) {
        prevNumber[i] = raw[i].xTaskNumber;
        prevRunTime[i] = raw[i].ulRunTimeCounter;
    }
    prevCount = got < kScratch ? got : kScratch;
    prevTotal = totalRunTime;
#endif
    return n;
}

void tasksHeap(size_t& total, size_t& psram) {
    total = psram = 0;
    uint32_t totalRunTime = 0;
    UBaseType_t got = 0;
    // The kernel's table, taken under its lock, as both cores move tasks between lists: a list walk of our own would race the other core.
    const ScratchLock lock;
    const TaskStatus_t* raw = lockedState(lock, got, totalRunTime);
    if (!raw) return;
    vTaskSuspendAll();   // as freertos_debug.h asks of a snapshot while the scheduler runs
    for (UBaseType_t i = 0; i < got; i++) {
        uint32_t inPsram = 0;
        total += heapOfTask(raw[i], inPsram);
        psram += inPsram;
    }
    xTaskResumeAll();
}

void currentTaskOnCore(int core, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    if (core < 0 || core > 1) return;
    TaskHandle_t h = xTaskGetCurrentTaskHandleForCore(core);
    if (h) std::snprintf(out, cap, "%s", pcTaskGetName(h));
#else
    (void)core;
#endif
}

// The name of the calling task, which is the render task while everything runs on the one task; a dedicated render task captures its name from inside the render loop.
const char* renderTaskName() { return pcTaskGetName(nullptr); }

#else  // trace facility off: inert stubs, and the task table reads empty.

size_t taskSnapshot(TaskInfo*, size_t) { return 0; }
void tasksHeap(size_t& total, size_t& psram) { total = psram = 0; }
void currentTaskOnCore(int, char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
const char* renderTaskName() { return ""; }   // trace facility off: no task view

#endif

}  // namespace mm::platform
