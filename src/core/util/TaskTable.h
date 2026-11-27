#pragma once
/// The device's tasks, taken from the RTOS when someone reads them, at most once a second: the textbook RTOS introspection pattern.
///
/// A row per task with its state, core, priority, least free stack, CPU share and what it holds, MoonLight's tasks first and then by name, so a row stays put. The rows grow on the first read: a device nobody watches holds none.

#include "core/util/ScratchBuffer.h"   // the rows, grown on the first read
#include "platform/platform_clock.h"   // millis: how old the snapshot is
#include "platform/platform_tasks.h"   // taskSnapshot, currentTaskOnCore, renderTaskName, TaskInfo

#include <algorithm>   // std::sort: a stable row order
#include <cstdint>
#include <cstring>     // strcmp, strncmp: our tasks first, then by name

namespace mm {

class TaskTable {
public:
    static constexpr uint8_t kMaxTasks = 32;   ///< a generous ceiling for one device

    /// The table's rows count toward `owner`'s memory.
    explicit TaskTable(MoonModule& owner) : rows_(owner) {}

    /// Take a new snapshot when the last is a second old, or on the first read.
    void ensureFresh() const {
        const uint32_t now = platform::millis();
        if (takenMs_ != 0 && now - takenMs_ < 1000) return;
        takenMs_ = now ? now : 1;
        refresh();
    }
    /// How many tasks the last snapshot holds.
    uint8_t count() const { return count_; }
    /// One task of the last snapshot.
    const platform::TaskInfo& task(uint8_t i) const { return rows_[i]; }
    /// Whether task `t` is the one its core ran when the snapshot was taken.
    bool runningNow(const platform::TaskInfo& t) const {
        return std::strcmp(t.name, running_[0]) == 0 || std::strcmp(t.name, running_[1]) == 0;   // an unpinned task runs on either
    }

    /// The wire label for one task state.
    static const char* stateName(platform::TaskState s) {
        switch (s) {
            case platform::TaskState::Running:   return "running";
            case platform::TaskState::Ready:     return "ready";
            case platform::TaskState::Blocked:   return "blocked";
            case platform::TaskState::Suspended: return "suspended";
            case platform::TaskState::Deleted:   return "deleted";
            default:                             return "?";
        }
    }

private:
    /// Re-snapshot the tasks and the one each core runs, then order the rows.
    void refresh() const {
        count_ = rows_.ensure(kMaxTasks) ? static_cast<uint8_t>(platform::taskSnapshot(rows_.data(), kMaxTasks)) : 0;
        platform::currentTaskOnCore(0, running_[0], sizeof(running_[0]));
        platform::currentTaskOnCore(1, running_[1], sizeof(running_[1]));
        std::sort(rows_.data(), rows_.data() + count_, [](const platform::TaskInfo& a, const platform::TaskInfo& b) {
            const bool oursA = ours(a.name), oursB = ours(b.name);
            if (oursA != oursB) return oursA;
            return std::strcmp(a.name, b.name) < 0;
        });
    }
    /// Whether we created this task: the render task, or a worker under our own prefix.
    static bool ours(const char* name) {
        const char* render = platform::renderTaskName();
        return (render[0] && std::strcmp(name, render) == 0) || std::strncmp(name, "mm", 2) == 0;
    }

    mutable ScratchBuffer<platform::TaskInfo> rows_;   ///< the snapshot, a cache a read refreshes
    mutable uint8_t count_ = 0;                        ///< how many rows it holds
    mutable uint32_t takenMs_ = 0;                     ///< when it was taken, 0 before the first
    mutable char running_[2][16] = {};                 ///< the task each core ran at the snapshot
};

} // namespace mm
