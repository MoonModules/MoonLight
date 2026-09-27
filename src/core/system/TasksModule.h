#pragma once


#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"   // instance()->moduleCount()/module(i) — the modules to profile
#include "core/util/JsonSink.h"    // writeListRow emits its row as JSON into the sink
#include "platform/platform.h" // taskSnapshot / TaskInfo — the RTOS task view (behind the boundary)

#include <algorithm>  // std::sort — stable row order so the list doesn't jump each refresh
#include <cstdint>
#include <cstdio>
#include <cstring>   // strcmp — match a task row to the render task

namespace mm {

/// A domain-neutral diagnostic showing what runs where: the tasks, and the modules inside each.
///
/// It is the observability foundation for core-affinity work.
/// You cannot optimize which module runs on which core until you can see it.
/// Header-only: its only platform reach is one seam.
/// A fixed System module, wired by code, and read-only.
///
/// @moreinfo
///
/// ## One nested list
///
/// Each row is a task, carrying its name, state, core, priority and stack mark.
/// A CPU percentage appears only in a profiling build.
/// Expanding a row reveals the modules in that task, each with its live cost.
/// Those figures are the modules' own self-report, so the cost view is free.
/// Two read-only fields name the task on each core.
///
/// ## Why the nesting is ours
///
/// The scheduler runs many modules cooperatively inside one render task.
/// In a normal RTOS a task is the unit, so nesting modules under one is specific to this design.
/// Today every module runs in that task, so its detail is the whole module list.
/// The structure is ready for a future split across tasks.
///
/// Prior art: MoonLight's flat task table, the textbook RTOS introspection pattern.
class TasksModule : public MoonModule {
public:
    /// Declare the task list and the two per-core readouts.
    void defineControls() override {
        MoonModule::defineControls();
        // The nested tree is the view, so the per-module cost lives in a row's detail.
        controls_.addList("tasks", tasks_);
        controls_.addReadOnly("core0", core0_, sizeof(core0_));
        controls_.addReadOnly("core1", core1_, sizeof(core1_));
    }

    /// Re-snapshot the tasks once a second, the module costs being read live on each serialize.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        tasks_.refresh();
        platform::currentTaskOnCore(0, core0_, sizeof(core0_));
        platform::currentTaskOnCore(1, core1_, sizeof(core1_));
    }

private:
    /// The task table, a source over a fixed snapshot the platform fills, with no allocation.
    struct TaskListSource : ListSource {
        static constexpr uint8_t kMaxTasks = 32;   ///< a generous ceiling for one device
        platform::TaskInfo rows_[kMaxTasks];       ///< the snapshot itself
        uint8_t count_ = 0;                        ///< how many rows it holds

        /// Re-snapshot the tasks, then order them so a row stays put between refreshes.
        void refresh() {
            count_ = static_cast<uint8_t>(platform::taskSnapshot(rows_, kMaxTasks));
            // A task's row would otherwise shift with its state, so sort: ours first, then by name.
            std::sort(rows_, rows_ + count_, [](const platform::TaskInfo& a, const platform::TaskInfo& b) {
                const bool oursA = isMoonLightTask(a.name), oursB = isMoonLightTask(b.name);
                if (oursA != oursB) return oursA;                 // our tasks first
                return std::strcmp(a.name, b.name) < 0;           // then alphabetical, stable
            });
        }

        /// Whether we created this task: the render task, or a worker under our own prefix.
        static bool isMoonLightTask(const char* name) {
            const char* render = platform::renderTaskName();
            return (render[0] && std::strcmp(name, render) == 0) || std::strncmp(name, "mm", 2) == 0;
        }
        /// How many tasks the snapshot holds.
        uint8_t listRowCount() const override { return count_; }
        /// Append one task's summary: its name, state, core, priority and stack.
        void writeListRow(JsonSink& sink, uint8_t row) const override {
            if (row >= count_) { sink.append("{}"); return; }
            const platform::TaskInfo& t = rows_[row];
            sink.append("{\"name\":");
            sink.writeJsonString(t.name);
            sink.appendf(",\"state\":\"%s\",\"core\":%d,\"prio\":%u,\"stack\":%u",
                         stateGlyph(t.state), static_cast<int>(t.core),
                         static_cast<unsigned>(t.priority),
                         static_cast<unsigned>(t.stackFreeBytes));
            // Only when it is a real measurement, so the UI shows no column rather than a zero.
            if (t.cpuPermille != platform::kTaskCpuUnmeasured)
                sink.appendf(",\"cpu\":%u.%u", static_cast<unsigned>(t.cpuPermille / 10u),
                             static_cast<unsigned>(t.cpuPermille % 10u));
            sink.append("}");
        }

        /// Append the modules in this task, each one string so the detail view renders chips.
        void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
            if (row >= count_) { sink.append("{}"); return; }
            const bool isRenderTask = platform::renderTaskName()[0] &&
                                      std::strcmp(rows_[row].name, platform::renderTaskName()) == 0;
            sink.append("{\"modules\":[");
            if (isRenderTask) {
                Scheduler* s = Scheduler::instance();
                const uint8_t mc = s ? s->moduleCount() : 0;
                uint32_t sumUs = 0;
                for (uint8_t i = 0; i < mc; i++) {
                    MoonModule* m = s->module(i);
                    if (!m) continue;
                    if (i) sink.append(",");
                    // Top-level only: a parent's time already includes its children's.
                    sumUs += m->tickTimeUs();
                    char line[64];
                    std::snprintf(line, sizeof(line), "%s \xC2\xB7 %uus \xC2\xB7 %uB \xC2\xB7 %uheap",
                                  m->name(),
                                  static_cast<unsigned>(m->tickTimeUs()),
                                  static_cast<unsigned>(m->classSize()),
                                  static_cast<unsigned>(m->dynamicBytes()));
                    sink.writeJsonString(line);
                }
                // The module times should account for nearly all the tick, the rest being output work.
                const uint32_t tick = s ? s->tickTimeUs() : 0;
                const uint32_t untracked = tick > sumUs ? tick - sumUs : 0;
                if (mc) sink.append(",");
                char calc[80];
                std::snprintf(calc, sizeof(calc),
                              "\xE2\x88\x91 modules %uus / tick %uus \xC2\xB7 %uus outside modules",
                              static_cast<unsigned>(sumUs), static_cast<unsigned>(tick),
                              static_cast<unsigned>(untracked));
                sink.writeJsonString(calc);
            }
            sink.append("]}");
        }
        /// The wire label for one task state.
        static const char* stateGlyph(platform::TaskState s) {
            switch (s) {
                case platform::TaskState::Running:   return "running";
                case platform::TaskState::Ready:     return "ready";
                case platform::TaskState::Blocked:   return "blocked";
                case platform::TaskState::Suspended: return "suspended";
                case platform::TaskState::Deleted:   return "deleted";
                default:                             return "?";
            }
        }
    };

    TaskListSource tasks_;   ///< the task table behind the list control
    char core0_[16] = {};    ///< what executes on the first core
    char core1_[16] = {};    ///< what executes on the second, empty on a single-core chip
};

} // namespace mm
