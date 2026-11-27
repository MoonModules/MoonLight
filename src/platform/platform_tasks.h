#pragma once
/// The platform's tasks: the calling task's identity and core, yielding, pinned worker tasks with their wake, the watchdog and a snapshot of every task.
///
/// One concern of the platform interface ([platform.h](platform.h) gathers them all), so a file includes the concerns it uses and an edit to one rebuilds its users alone.

#include "platform/platform_base.h"   // target flags, MM_NONBLOCKING and the compiler macros

namespace mm::platform {


/// An opaque identity for the calling task, stable for its lifetime and nonzero.
uintptr_t currentThreadId() MM_NONBLOCKING;

/// Hand the processor to a ready task of the caller's priority or above, returning at once.
void yield();

/// Which core the caller runs on, safe in an ISR; a driver seeing core 1 knows the render/encode split is engaged.
uint8_t currentCore();
/// Upper bound on cores running driver code at once, sizing per-CPU scratch; single-core targets leave slice 1 unused.
inline constexpr uint8_t kMaxCores = 2;
/// Pace the render loop for one pass with a short real wait, so every task sharing its core gets a turn.
void pauseLoop();

// --- RTOS task introspection: an allocation-free snapshot, on tick1s rather than per frame ----

/// What a task is doing right now.
enum class TaskState : uint8_t { Running, Ready, Blocked, Suspended, Deleted, Invalid, Unknown };
/// Stands in for cpuPermille where run-time stats are compiled out, so the caller can omit the column.
constexpr uint32_t kTaskCpuUnmeasured = 0xFFFFFFFFu;
/// One RTOS task, as the task table reports it.
struct TaskInfo {
    char      name[16] = {};                     ///< the task's own name
    TaskState state = TaskState::Unknown;        ///< running, blocked, suspended
    int8_t    core = -1;                         ///< 0, 1, or -1 for no affinity
    uint8_t   priority = 0;                      ///< its RTOS priority
    uint32_t  stackFreeBytes = 0;                ///< high-water mark: the least free stack seen
    uint32_t  cpuPermille = kTaskCpuUnmeasured;  ///< 0..1000, or unmeasured when stats are off
    uint32_t  heapBytes = 0;                     ///< what the task holds on the heap, its stack and its control block; 0 where the platform cannot tell
    uint32_t  psramBytes = 0;                    ///< the part of heapBytes in PSRAM: its stack where that lives there, its control block being internal
};
/// Fill `out` with up to `maxTasks` rows, answering how many; 0 where there is no RTOS.
size_t taskSnapshot(TaskInfo* out, size_t maxTasks);
/// What every task holds on the heap, stacks and control blocks: total bytes and the part in PSRAM, 0 where there is no RTOS.
void tasksHeap(size_t& total, size_t& psram);
/// The task running on a core, or empty on a single-core target.
void currentTaskOnCore(int core, char* out, size_t cap);
/// The task the render loop runs in, empty where no distinct one exists.
const char* renderTaskName();
/// Inject a canned snapshot from a test; `tasks` must outlive the use.
void setTestTaskSnapshot(const TaskInfo* tasks, size_t count, const char* renderTask);

// --- Pinned worker task and wake: FreeRTOS's lock-free pairing for a single producer ----------

/// An opaque handle to a spawned task, keeping FreeRTOS types out of the header.
struct WorkerTask { void* impl = nullptr; };
/// The body a pinned task runs; it owns its loop and returns once stopPinnedTask signals it.
using WorkerFn = void(*)(void* user);
/// Spawn `fn(user)` pinned to `core`, where -1 means no affinity; false when the task cannot be created.
bool spawnPinnedTask(WorkerTask& t, const char* name, WorkerFn fn, void* user,
                     size_t stackBytes, uint8_t priority, int core);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// Make spawnPinnedTask refuse, as a board out of task memory does, so a test reaches the inline fallback; false restores.
void setTestSpawnFails(bool fail);
#endif
/// Wake the task blocked in waitNotify, safe from any task on any core.
void notifyTask(WorkerTask& t);
/// Block inside the spawned fn until notifyTask fires or `timeoutMs` elapses; false on timeout.
bool waitNotify(WorkerTask& t, uint32_t timeoutMs);
/// Block inside the spawned fn for `us` microseconds, finer than the scheduler tick and leaving the core free; a stop wakes it early.
void sleepUs(WorkerTask& t, uint32_t us);
/// Signal stop and wake, then block until the worker fn has returned and the task is torn down.
void stopPinnedTask(WorkerTask& t);

/// A binary semaphore: one task gives it when its work is done, another sleeps on it rather than spinning, keeping FreeRTOS types out of the header.
struct Signal { void* impl = nullptr; };
/// Create it, unset; false when memory refuses, and a take on it then returns at once.
bool signalCreate(Signal& s);
/// Free it; safe on an empty one.
void signalDestroy(Signal& s);
/// Set it, from any task; a second give before a take is the same one give.
void signalGive(Signal& s);
/// Sleep until it is given or `timeoutMs` passes, and clear it; false on a timeout or on an empty one.
bool signalTake(Signal& s, uint32_t timeoutMs);
/// Subscribe THIS task to the watchdog, so a wedge reboots instead of hanging silently.
void taskWdtSubscribe();
/// Unsubscribe THIS task before it exits, so a torn-down worker leaves no dangling entry.
void taskWdtUnsubscribe();
/// Feed THIS task's subscription, once a tick.
void taskWdtReset();

}  // namespace mm::platform
