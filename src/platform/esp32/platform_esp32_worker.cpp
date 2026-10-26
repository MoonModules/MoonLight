/// The one seam behind the multicore split, so no task-system type escapes this layer.
///
/// A pinned task with a direct notification is the textbook lock-free wake for one producer and one consumer, and the documented lightweight replacement for a binary semaphore.
/// The render split is the first user and the asynchronous send wants the identical primitive, which is why it lives here rather than in either.
///
/// @technical
///
/// ## The watchdog subscription is per task
///
/// Subscribing and feeding act on the current task, so the flag recording whether the caller subscribed is thread-local rather than global.
/// The render loop and the encode worker each feed the watchdog, and a single global would let one call the feed on a task the other subscribed.
/// The system rejects that as an unknown task, flooding the log every tick and starving the network stack.
/// Each task reads and writes its own copy on its own task, so nothing needs synchronizing.
///
/// The configuration runs the watchdog with idle-task checking off, a saturated core being healthy, so nothing is watched unless a task subscribes.
/// If either stops feeding it, a genuine wedge rather than a busy frame, it panics and reboots with a backtrace instead of hanging silently.
///
/// ## Stopping is bounded, never infinite
///
/// Normally one wake suffices, the worker returning once it sees the stop flag.
/// But if the stop runs on the same core the worker is pinned to while the worker is mid-job, it cannot be scheduled until this caller yields.
/// And an unbounded spin would deadlock the device.
/// Yielding lets it drain within a few milliseconds, and the deadline is the robustness floor for a lost wake or a wedged job.
/// On timeout the worker is detached rather than freed, a bounded leak being better than a use-after-free.
/// And its stop flag stays set so it self-deletes cleanly if it ever wakes.
///
/// ## The handle is assigned before the task is created
///
/// A task pinned to the spawner's own core preempts inside the create call, and its function may wait immediately.
/// With the handle still unset that wait returns at once without blocking.
/// And a retry loop becomes a hot spin that starves this caller from ever assigning it, which measured as a board offline at boot.
/// Assigning first closes that window, and a failed create resets it before anyone can be woken.

#include "platform/platform.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"   // the render loop and the encode worker each subscribe themselves so a wedge self-heals (reboot) not hangs
#include "esp_log.h"         // sleepUs says once when it falls back to a scheduler tick
#include "esp_timer.h"       // sleepUs: a one-shot timer wakes the task, finer than the scheduler tick
#include "esp_attr.h"        // IRAM_ATTR: sleepUs' wake runs in the timer interrupt

#if !CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
#error "sleepUs wakes its task from the timer interrupt: set CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD=y (esp32/sdkconfig.defaults)"
#endif

#include <atomic>
#include <new>

namespace mm::platform {

namespace {
// stopPinnedTask's join deadline. A worker normally drains within a few ms of the stop-notify; this is the robustness floor for the pathological case (a same-core worker that can't be scheduled, a lost notify). ≫ any real per-frame job, ≪ a user-perceptible hang.
constexpr uint32_t kStopJoinTimeoutMs = 300;

// The opaque WorkerTask::impl. Holds the RTOS handle, the caller's fn/user, and a stop flag the spawned trampoline checks.
struct EspWorker : InternalRamObject {   // the timer interrupt reads it with the flash cache possibly off
    TaskHandle_t handle = nullptr;
    WorkerFn fn = nullptr;
    void* user = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    // Ownership handshake for the timeout path: normally stopPinnedTask waits for `finished` and frees `w`. If it TIMES OUT (the worker couldn't be scheduled, a same-core teardown), it hands ownership to the trampoline by setting `detached`, and whichever of the two runs last frees `w`. The atomic-exchange below makes exactly one side win, so `w` is freed once and never used-after-free.
    std::atomic<bool> detached{false};
    /// The one-shot timer sleepUs arms, created on the task's first sleep and deleted as the task ends.
    esp_timer_handle_t sleepTimer = nullptr;
    bool sleepFellBack = false;   ///< a timer failed once and the task slept a whole tick, logged that first time
};

// The trampoline: run the caller's function, which owns its own loop and returns once it sees the stop flag, then delete the task. The function manages its own watchdog membership, subscribing at loop start and unsubscribing before returning; one that never subscribes leaves the feed a no-op, so this stays generic.
void workerTrampoline(void* arg) {
    auto* w = static_cast<EspWorker*>(arg);
    w->fn(w->user);                     // runs until stopPinnedTask flips w->stop and wakes it
    if (w->sleepTimer) {
        esp_timer_stop(w->sleepTimer);
        esp_timer_delete(w->sleepTimer);
        w->sleepTimer = nullptr;
    }
    w->finished.store(true, std::memory_order_release);
    // If stopPinnedTask already timed out and detached, IT is gone and won't free `w`, we own it now. The exchange makes exactly one side observe `detached==true` first: whoever sees it frees `w`.
    if (w->detached.exchange(true, std::memory_order_acq_rel)) delete w;
    vTaskDelete(nullptr);
}
}  // namespace

bool spawnPinnedTask(WorkerTask& t, const char* name, WorkerFn fn, void* user,
                     size_t stackBytes, uint8_t priority, int core) {
    auto* w = new (std::nothrow) EspWorker();
    if (!w) return false;
    w->fn = fn;
    w->user = user;
    // The handle is assigned BEFORE the create: @xref{the-handle-is-assigned-before-the-task-is-created|the live-lock that otherwise measured as a board offline at boot}.
    t.impl = w;
    // A core the chip lacks, such as core 1 on a single-core C3, runs the task unpinned rather than failing FreeRTOS's assert.
    const BaseType_t coreId = (core < 0 || core >= portNUM_PROCESSORS) ? tskNO_AFFINITY : static_cast<BaseType_t>(core);
    const BaseType_t ok = xTaskCreatePinnedToCore(
        &workerTrampoline, name, static_cast<uint32_t>(stackBytes), w,
        static_cast<UBaseType_t>(priority), &w->handle, coreId);
    if (ok != pdPASS) { t.impl = nullptr; delete w; return false; }   // caller runs inline (degrade)
    return true;
}

void notifyTask(WorkerTask& t) {
    auto* w = static_cast<EspWorker*>(t.impl);
    if (w && w->handle) xTaskNotifyGive(w->handle);
}

bool waitNotify(WorkerTask& t, uint32_t timeoutMs) {
    auto* w = static_cast<EspWorker*>(t.impl);
    if (!w) return false;
    // ulTaskNotifyTake(pdTRUE, …) clears the notification count on return (the binary-semaphore form). Non-zero return = a notify (or stop-wake) landed; 0 = timed out.
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeoutMs)) != 0;
}

// Without a timer the task sleeps a whole scheduler tick, which stretches a paced frame tenfold, so the first time says so.
static void sleepTick(EspWorker* w) {
    if (!w->sleepFellBack) ESP_LOGW("mm_worker", "sleepUs: no timer, sleeping a scheduler tick per gap");
    w->sleepFellBack = true;
    vTaskDelay(1);
}

// The wake runs in the timer's interrupt, since the esp_timer task shares a core and a priority with a busy render loop and waited for its time slice: a 165 µs gap took 7 ms. In IRAM, the interrupt firing while a flash write has the cache off.
static void IRAM_ATTR sleepWake(void* arg) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(static_cast<EspWorker*>(arg)->handle, &woken);
    if (woken) esp_timer_isr_dispatch_need_yield();   // ESP-IDF's rule for an ISR-dispatched callback, which switches once every due timer ran
}

// A one-shot esp_timer notifies the task, so a gap below the 10 ms scheduler tick costs no busy-wait; a stop's notification ends it early, as it should.
void sleepUs(WorkerTask& t, uint32_t us) {
    auto* w = static_cast<EspWorker*>(t.impl);
    if (!w || w->stop.load(std::memory_order_acquire)) return;
    if (!w->sleepTimer) {
        const esp_timer_create_args_t args = {
            .callback = &sleepWake,
            .arg = w, .dispatch_method = ESP_TIMER_ISR, .name = "sleepUs", .skip_unhandled_events = true};
        if (esp_timer_create(&args, &w->sleepTimer) != ESP_OK) { w->sleepTimer = nullptr; sleepTick(w); return; }
    }
    if (esp_timer_start_once(w->sleepTimer, us) != ESP_OK) { sleepTick(w); return; }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(us / 1000 + 20));   // bounded, so a lost wake costs one gap and no hang
    esp_timer_stop(w->sleepTimer);   // a stop that woke it early leaves the timer armed
}

void stopPinnedTask(WorkerTask& t) {
    auto* w = static_cast<EspWorker*>(t.impl);
    if (!w) return;
    w->stop.store(true, std::memory_order_release);   // the fn re-checks this after its next wake
    if (w->handle) xTaskNotifyGive(w->handle);         // wake it so it observes the stop
    // Wait for the trampoline to run out and delete itself, bounded rather than infinite: @xref{stopping-is-bounded-never-infinite|the deadlock an unbounded spin caused}.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kStopJoinTimeoutMs);
    while (!w->finished.load(std::memory_order_acquire)) {
        if (xTaskGetTickCount() > deadline) {
            // DETACH, hand `w` to the trampoline. The exchange makes exactly one side free it: if the trampoline already ran (raced us to `detached`), WE free `w` here; otherwise the trampoline frees it when it finally exits. Either way `w` is freed once, never used-after-free, and this caller returns instead of deadlocking (see the timeout rationale above).
            if (w->detached.exchange(true, std::memory_order_acq_rel)) delete w;
            t.impl = nullptr;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    // NORMAL path frees through the SAME exchange as the timeout path, not a bare delete. The trampoline touches `w->detached` AFTER storing `finished` (see workerTrampoline), so once we observe finished the trampoline may still be mid-exchange on `w`; a bare delete here would free it under that access. The exchange makes exactly one side win the free, whichever runs the store last.
    if (w->detached.exchange(true, std::memory_order_acq_rel)) delete w;
    t.impl = nullptr;
}

// Whether the CALLING task subscribed, so the feed only ever feeds a real subscription: @xref{the-watchdog-subscription-is-per-task|why this is thread-local rather than global}.
static thread_local bool s_wdtSubscribed = false;

// Subscribe the CURRENT task to the watchdog: @xref{the-watchdog-subscription-is-per-task|what it watches and why}. Idempotent per task, and a failure leaves the flag clear and the feed a no-op, degrading to unwatched rather than crashing.
void taskWdtSubscribe() {
    if (s_wdtSubscribed) return;
    if (esp_task_wdt_add(nullptr) == ESP_OK) s_wdtSubscribed = true;
}

// Unsubscribe THIS task before it exits (esp_task_wdt_delete), so a torn-down task leaves no stale WDT entry the IDF would keep checking. No-op if this task never subscribed.
void taskWdtUnsubscribe() {
    if (!s_wdtSubscribed) return;
    if (esp_task_wdt_delete(nullptr) == ESP_OK) s_wdtSubscribed = false;
}

// Feed THIS task's WDT subscription (esp_task_wdt_reset). No-op until/unless taskWdtSubscribe ran on this same task, so a task that never subscribed (or a build/config without the WDT) is unaffected.
void taskWdtReset() {
    if (s_wdtSubscribed) esp_task_wdt_reset();
}

}  // namespace mm::platform
