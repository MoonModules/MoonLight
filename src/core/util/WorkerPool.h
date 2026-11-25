#pragma once

#include "platform/platform_clock.h"   // micros
#include "platform/platform_tasks.h"   // spawnPinnedTask, stopPinnedTask, notifyTask

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mm {

/// Workers beside the caller that run a batch of items, such as a frame's layers, each item once, returning when all have run.
///
/// The caller takes items too, so a pool with no workers runs the batch in order on the caller, as a single core does and as a board does whose memory could not hold a worker's stack.
///
/// @technical
///
/// ## One word claims an item
///
/// Workers and the caller take items from one shared counter, the textbook work queue for equal jobs. The counter holds the batch's number and its item count beside the next index, and an item is claimed by compare-and-swap: a worker still leaving the last batch finds the number changed and leaves the new one to the others, where a bare index would hand it an item with the old batch's function.
class WorkerPool {
public:
    /// What runs one item: `ctx` is the batch's, `item` its index, `worker` whether a pool worker runs it rather than the caller.
    using ItemFn = void (*)(void* ctx, uint8_t item, bool worker);
    /// The most workers a pool holds: one for each render thread beside the caller, none on a single core.
    static constexpr uint8_t kMaxWorkers = platform::renderThreads - 1;

    /// An empty pool: it runs every batch on the caller until start() adds workers.
    WorkerPool() = default;
    /// Not copied: its workers point back at it.
    WorkerPool(const WorkerPool&) = delete;
    /// Not assigned, for the same reason.
    WorkerPool& operator=(const WorkerPool&) = delete;
    /// Stops the workers, so none outlives the pool.
    ~WorkerPool() { stop(); }

    /// Start up to `want` workers, each a task of `stackBytes` at `priority` pinned to `core` (-1 for any); answers how many started, since a task that cannot be created leaves the pool smaller, down to none.
    uint8_t start(uint8_t want, size_t stackBytes, uint8_t priority, int core) {
        stop();
        stop_.store(false, std::memory_order_relaxed);
        for (uint8_t i = 0; i < want && i < kMaxWorkers; i++) {
            Worker& w = workers_[count_];
            w.pool = this;
            if (!platform::spawnPinnedTask(w.task, "mmRender", &workerBody, &w, stackBytes, priority, core)) break;
            count_++;
        }
        return count_;
    }

    /// Stop every worker and wait until each has ended.
    void stop() {
        stop_.store(true, std::memory_order_release);
        for (uint8_t i = 0; i < count_; i++) platform::stopPinnedTask(workers_[i].task);
        count_ = 0;
    }

    /// How many workers run beside the caller.
    uint8_t workers() const { return count_; }

    /// The time the items took, by who ran them, and the caller's wait for the last worker, in microseconds since the last take: the lanes of the tick time balance sheet.
    struct Load {
        uint32_t callerUs = 0;    ///< items the caller ran itself
        uint32_t workersUs = 0;   ///< items the workers ran, summed over them
        uint32_t joinUs = 0;      ///< the caller waiting for the workers' last items
    };
    /// Hand over the load since the last take and start again; called by the caller's thread, between batches.
    Load takeLoad() {
        const Load l{callerUs_, workersUs_.exchange(0, std::memory_order_relaxed), joinUs_};
        callerUs_ = joinUs_ = 0;
        return l;
    }

    /// Run `fn(ctx, i, worker)` once for every `i` below `items`, the workers and the caller taking items until none is left; returns once every item has run.
    void run(uint8_t items, ItemFn fn, void* ctx) {
        if (items == 0) return;
        if (count_ == 0 || items == 1) {
            const uint32_t t0 = platform::micros();
            for (uint8_t i = 0; i < items; i++) fn(ctx, i, false);
            callerUs_ += platform::micros() - t0;
            return;
        }
        fn_ = fn;
        ctx_ = ctx;
        done_.store(0, std::memory_order_relaxed);
        batch_ = static_cast<uint16_t>(batch_ + 1);
        claim_.store(pack(batch_, items, 0), std::memory_order_release);   // publishes fn_ and ctx_ with it
        for (uint8_t i = 0; i < count_; i++) platform::notifyTask(workers_[i].task);
        takeItems(batch_, true);
        const uint32_t t0 = platform::micros();
        while (done_.load(std::memory_order_acquire) < items) platform::yield();   // the caller's own items are done; the last workers finish theirs
        joinUs_ += platform::micros() - t0;
    }

private:
    /// One worker: its task and the pool it takes items from.
    struct Worker {
        platform::WorkerTask task;
        WorkerPool* pool = nullptr;
    };

    /// The claim word: the batch's number, its item count and the next index.
    static constexpr uint32_t pack(uint16_t batch, uint8_t items, uint8_t next) {
        return (uint32_t{batch} << 16) | (uint32_t{items} << 8) | next;
    }

    /// Claim and run items of `batch` until none is left or another batch has begun, timing each for the load of whoever ran it.
    void takeItems(uint16_t batch, bool caller) {
        uint32_t c = claim_.load(std::memory_order_acquire);
        for (;;) {
            const uint8_t items = static_cast<uint8_t>(c >> 8), next = static_cast<uint8_t>(c);
            if ((c >> 16) != batch || next >= items) return;
            if (!claim_.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel, std::memory_order_acquire)) continue;
            const uint32_t t0 = platform::micros();
            fn_(ctx_, next, !caller);
            const uint32_t us = platform::micros() - t0;
            if (caller) callerUs_ += us;
            else workersUs_.fetch_add(us, std::memory_order_relaxed);
            done_.fetch_add(1, std::memory_order_release);
            c = claim_.load(std::memory_order_acquire);
        }
    }

    /// A worker's loop: wait for a batch, take its items, until the pool stops it.
    static void workerBody(void* user) {
        Worker& w = *static_cast<Worker*>(user);
        WorkerPool& p = *w.pool;
        while (!p.stop_.load(std::memory_order_acquire)) {
            if (!platform::waitNotify(w.task, 100)) continue;
            p.takeItems(static_cast<uint16_t>(p.claim_.load(std::memory_order_acquire) >> 16), false);
        }
    }

    Worker workers_[kMaxWorkers > 0 ? kMaxWorkers : 1];   // an array holds at least one
    uint8_t count_ = 0;                 ///< workers started
    uint16_t batch_ = 0;                ///< the number of the last batch begun, the caller's alone
    std::atomic<uint32_t> claim_{0};    ///< the claim word, @xref{one-word-claims-an-item}
    std::atomic<uint8_t> done_{0};      ///< items of the batch that have run
    std::atomic<bool> stop_{false};
    uint32_t callerUs_ = 0;             ///< the caller's item time since the last take, the caller's alone
    uint32_t joinUs_ = 0;               ///< the caller's wait for the workers since the last take
    std::atomic<uint32_t> workersUs_{0};   ///< the workers' item time since the last take
    ItemFn fn_ = nullptr;               ///< the batch's function, published by the claim word
    void* ctx_ = nullptr;               ///< and its context
};

}  // namespace mm
