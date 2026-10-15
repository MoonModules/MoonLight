/// The one seam that reads the task table, so no RTOS type escapes the platform layer.
///
/// The module above does the domain work; this file answers what the tasks are.
///
/// @technical
///
/// ## Two SDK options gate what it can answer
///
/// The snapshot call is the textbook introspection one and needs the trace facility: without it the snapshot is empty and the module shows only its own cost table.
/// The per-task CPU figure needs run-time stats, which cost a timer read on every context switch, measured at about 5 percent of the tick.
/// So that one is gated behind its own build flag and reported as unmeasured when compiled out.
///
/// ## The scratch buffer is lazy and internal
///
/// Allocated once on the first snapshot rather than held from boot, and never re-allocated per tick.
/// The no-heap rule applies because this runs from the one-second tick, and one lazy allocation satisfies it: every later tick is allocation-free.
/// As a plain static array it cost over a kilobyte of internal memory from boot on every board, for an opt-in diagnostic that appears in no device model.
///
/// It is kept for the process rather than freed per call, since freeing would put the allocation back on every tick.
/// The snapshot call wants room for every task or it returns nothing, so the ceiling is generous and exceeding it makes that tick empty rather than partial.
///
/// Internal rather than external memory, though the size would fit comfortably: the call fills this buffer with the kernel lock held, so every write lands inside a critical section.
/// A cache miss there stretches that section, which is the one place on this chip where a stall is most expensive.

#include "platform/platform.h"

#include "sdkconfig.h"

#include <cstring>
#include <cstdio>

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"   // heap_caps_calloc — the one-time snapshot scratch
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
}  // namespace

size_t taskSnapshot(TaskInfo* out, size_t maxTasks) {
    if (!out || maxTasks == 0) return 0;
    // Allocated once on the first snapshot: @xref{the-scratch-buffer-is-lazy-and-internal|why not from boot, and why not external memory}.
    static constexpr size_t kScratch = 40;
    static TaskStatus_t* raw = nullptr;
    if (!raw) {
        raw = static_cast<TaskStatus_t*>(
            heap_caps_calloc(kScratch, sizeof(TaskStatus_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!raw) return 0;   // no scratch, no snapshot: the table shows empty, nothing crashes
    }
    uint32_t totalRunTime = 0;
    const UBaseType_t got = uxTaskGetSystemState(raw, kScratch, &totalRunTime);
    const size_t n = got < maxTasks ? got : maxTasks;
    for (size_t i = 0; i < n; i++) {
        const TaskStatus_t& t = raw[i];
        TaskInfo& o = out[i];
        std::snprintf(o.name, sizeof(o.name), "%s", t.pcTaskName ? t.pcTaskName : "?");
        o.state = mapState(t.eCurrentState);
        o.priority = static_cast<uint8_t>(t.uxCurrentPriority);
        o.stackFreeBytes = t.usStackHighWaterMark;
        // TaskStatus_t.xCoreID exists only when configTASKLIST_INCLUDE_COREID is set (it is not by default, even on a dual-core chip). Without it we can't know the per-task core, so report
        // -1 (unknown); the current-task-per-core view (core0/core1) still works via a separate call.
    #if defined(configTASKLIST_INCLUDE_COREID) && configTASKLIST_INCLUDE_COREID == 1
        o.core = (t.xCoreID == tskNO_AFFINITY) ? -1 : static_cast<int8_t>(t.xCoreID);
    #else
        o.core = -1;
    #endif
    #if defined(MM_TASK_CPU_STATS) && MM_TASK_CPU_STATS
        o.cpuPermille = totalRunTime ? static_cast<uint32_t>(1000ULL * t.ulRunTimeCounter / totalRunTime)
                                     : 0;
    #else
        o.cpuPermille = kTaskCpuUnmeasured;
        (void)totalRunTime;
    #endif
    }
    return n;
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

// The name of the calling task, which today IS the render task, everything running on the one task, so the nesting is correct. True only while single-tasked: once a dedicated render task lands, the serialization would still run elsewhere and this seam must capture the name from inside the render loop instead.
const char* renderTaskName() { return pcTaskGetName(nullptr); }

#else  // trace facility off — inert stubs; the module falls back to its cost table only.

size_t taskSnapshot(TaskInfo*, size_t) { return 0; }
void currentTaskOnCore(int, char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
const char* renderTaskName() { return ""; }   // trace facility off — no task view, no nesting anchor

#endif

}  // namespace mm::platform
