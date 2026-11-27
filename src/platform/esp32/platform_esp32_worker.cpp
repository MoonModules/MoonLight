/// The one seam behind the multicore split, so no task-system type escapes this layer.
///
/// A pinned task woken by a direct notification is the textbook lock-free wake for one producer and one consumer; the render split and the asynchronous send both use it.
///
/// @technical
///
/// ## The watchdog subscription is per task
///
/// Subscribing and feeding act on the current task, so the flag recording whether the caller subscribed is thread-local, each task reading its own copy.
///
/// The render loop and the encode worker each feed the watchdog; a global flag would let one feed a task the other subscribed, which the system rejects every tick, flooding the log.
///
/// The watchdog runs with idle-task checking off, a saturated core being healthy, so only a subscribed task is watched; one that stops feeding it panics and reboots with a backtrace.
///
/// ## Stopping is bounded
///
/// One wake normally suffices: the worker returns once it sees the stop flag.
///
/// A stop on the worker's own core while it is mid-job leaves it unscheduled until the caller yields, so the caller yields and waits up to a deadline rather than spinning.
///
/// On the deadline the worker is detached rather than freed, a bounded leak being better than a use-after-free, and its stop flag stays set so it deletes itself if it wakes.
///
/// ## The handle is assigned before the task is created
///
/// A task pinned to the spawner's core preempts inside the create call and may wait at once; with its handle unset that wait returns at once, and a retry becomes a hot spin.
///
/// That spin starved the spawner from ever assigning the handle, measured as a board offline at boot, so the handle is assigned first and a failed create resets it.

#include "platform/platform.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   // Signal: the binary semaphore a finished job gives
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
// stopPinnedTask's join deadline: far above any per-frame job, far below a hang a user notices, the floor for a same-core worker left unscheduled or a lost wake.
constexpr uint32_t kStopJoinTimeoutMs = 300;

// The opaque WorkerTask::impl. Holds the RTOS handle, the caller's fn/user, and a stop flag the spawned trampoline checks.
struct EspWorker {
    TaskHandle_t handle = nullptr;
    WorkerFn fn = nullptr;
    void* user = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    // Ownership on a timeout: stopPinnedTask sets `detached` and whichever of it and the trampoline runs last frees `w`, the exchange letting exactly one side win.
    std::atomic<bool> detached{false};
    /// The one-shot timer sleepUs arms, created on the task's first sleep and deleted as the task ends.
    esp_timer_handle_t sleepTimer = nullptr;
    bool sleepFellBack = false;   ///< a timer failed once and the task slept a whole tick, logged that first time
};

// Run the caller's function, which owns its loop and its watchdog membership and returns on the stop flag, then delete the task.
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

// Without a timer the task sleeps a whole scheduler tick, far longer than the gap asked for, so the first time says so.
static void sleepTick(bool& fellBack) {
    if (!fellBack) ESP_LOGW("mm_worker", "sleepUs: no timer, sleeping a scheduler tick per gap");
    fellBack = true;
    vTaskDelay(1);
}

// The wake runs in the timer's interrupt, since the timer task waited behind a busy render loop: a 165 µs gap took 7 ms. In IRAM, firing while a flash write has the cache off; its argument is the task handle, its only read.
static void IRAM_ATTR sleepWake(void* arg) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(arg), &woken);
    if (woken) esp_timer_isr_dispatch_need_yield();   // ESP-IDF's rule for an ISR-dispatched callback, which switches once every due timer ran
}

// Block the calling task for `us` on a one-shot timer that notifies it, finer than the scheduler tick, the core free meanwhile. The timer is the first sleeper's. False when no timer could run.
static bool sleepOnTimer(esp_timer_handle_t& timer, uint32_t us) {
    if (!timer) {
        const esp_timer_create_args_t args = {
            .callback = &sleepWake,
            .arg = xTaskGetCurrentTaskHandle(), .dispatch_method = ESP_TIMER_ISR, .name = "sleepUs", .skip_unhandled_events = true};
        if (esp_timer_create(&args, &timer) != ESP_OK) { timer = nullptr; return false; }
    }
    if (esp_timer_start_once(timer, us) != ESP_OK) return false;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(us / 1000 + 20));   // bounded, so a lost wake costs one gap and no hang
    esp_timer_stop(timer);   // a notification that woke it early leaves the timer armed
    return true;
}

// A stop's notification ends the sleep early, as it should.
void sleepUs(WorkerTask& t, uint32_t us) {
    auto* w = static_cast<EspWorker*>(t.impl);
    if (!w || w->stop.load(std::memory_order_acquire)) return;
    if (!sleepOnTimer(w->sleepTimer, us)) sleepTick(w->sleepFellBack);
}

// The render loop blocks a moment each pass so every task on its core gets a turn: a yield reaches only its own priority and above, and a frame with no wait starved mDNS and Improv. The render loop's alone.
void pauseLoop() {
    // Long enough for the scheduler to switch a waiting task in, short against a frame: half a percent of an 8 ms one.
    static constexpr uint32_t kLoopPauseUs = 50;
    static esp_timer_handle_t timer = nullptr;
    static bool fellBack = false;
    if (!sleepOnTimer(timer, kLoopPauseUs)) sleepTick(fellBack);
}

void stopPinnedTask(WorkerTask& t) {
    auto* w = static_cast<EspWorker*>(t.impl);
    if (!w) return;
    w->stop.store(true, std::memory_order_release);   // the fn re-checks this after its next wake
    if (w->handle) xTaskNotifyGive(w->handle);         // wake it so it observes the stop
    // Wait for the trampoline to run out and delete itself, bounded rather than infinite: @xref{stopping-is-bounded|the deadlock an unbounded spin caused}.
    const TickType_t start = xTaskGetTickCount();
    while (!w->finished.load(std::memory_order_acquire)) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(kStopJoinTimeoutMs)) {   // elapsed, which holds across the counter's wrap
            // Detach: the exchange frees `w` here when the trampoline already ran, else the trampoline frees it on exit, so `w` is freed once and this caller returns.
            if (w->detached.exchange(true, std::memory_order_acq_rel)) delete w;
            t.impl = nullptr;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    // The normal path frees through the same exchange: the trampoline touches `w->detached` after storing `finished`, so a bare delete could free `w` under that access.
    if (w->detached.exchange(true, std::memory_order_acq_rel)) delete w;
    t.impl = nullptr;
}

bool signalCreate(Signal& s) {
    if (!s.impl) s.impl = xSemaphoreCreateBinary();
    return s.impl != nullptr;
}

void signalDestroy(Signal& s) {
    if (s.impl) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(s.impl));
    s.impl = nullptr;
}

void signalGive(Signal& s) {
    if (s.impl) xSemaphoreGive(static_cast<SemaphoreHandle_t>(s.impl));
}

bool signalTake(Signal& s, uint32_t timeoutMs) {
    return s.impl && xSemaphoreTake(static_cast<SemaphoreHandle_t>(s.impl), pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
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
