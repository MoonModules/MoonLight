/// @module TaskTable

#include "doctest.h"
#include "core/util/TaskTable.h"
#include "core/module/MoonModule.h"
#include "platform/platform_clock.h"   // setTestNowMs: how old the snapshot is
#include "platform/platform_heap.h"    // processMemory
#include "platform/platform_tasks.h"   // taskSnapshot, setTestTaskSnapshot, TaskInfo

#include <cstring>

using namespace mm;

namespace {

// The injected snapshot for one test's scope, cleared on exit so no later reader follows it into a dead stack frame.
struct TaskSeam {
    TaskSeam(const platform::TaskInfo* tasks, size_t count, const char* render) { platform::setTestTaskSnapshot(tasks, count, render); }
    ~TaskSeam() { platform::setTestTaskSnapshot(nullptr, 0, ""); platform::setTestNowMs(0); }
};

struct Owner : MoonModule {};

} // namespace

// The task table is taken when someone reads it, at most once a second, so a device nobody watches walks none.
TEST_CASE("TaskTable: a read takes the task table, and reads within a second share it") {
    const platform::TaskInfo one[] = {{"main", platform::TaskState::Running, 0, 1, 2340, platform::kTaskCpuUnmeasured}};
    const platform::TaskInfo two[] = {{"main", platform::TaskState::Running, 0, 1, 2340, platform::kTaskCpuUnmeasured},
                                      {"wifi", platform::TaskState::Ready, 0, 1, 1200, platform::kTaskCpuUnmeasured}};
    platform::setTestNowMs(5000);
    TaskSeam seam(one, 1, "main");
    Owner owner;
    TaskTable table(owner);
    CHECK(table.count() == 0);   // a read takes the first snapshot
    table.ensureFresh();
    CHECK(table.count() == 1);
    platform::setTestTaskSnapshot(two, 2, "main");
    platform::setTestNowMs(5500);
    table.ensureFresh();
    CHECK(table.count() == 1);   // half a second on, the same snapshot
    platform::setTestNowMs(6000);
    table.ensureFresh();
    CHECK(table.count() == 2);   // a second on, a new one
}

// The snapshot's order shifts from frame to frame; a stable order keeps a row in place: MoonLight's own tasks first, then the system's, alphabetical within each.
TEST_CASE("TaskTable: MoonLight's tasks come first, the system's below, alphabetical within each") {
    const platform::TaskInfo snap[] = {
        {"Tmr Svc", platform::TaskState::Blocked, -1, 1, 900, platform::kTaskCpuUnmeasured},
        {"mmSend",  platform::TaskState::Blocked,  1, 5, 700, platform::kTaskCpuUnmeasured},
        {"IDLE0",   platform::TaskState::Ready,    0, 0, 500, platform::kTaskCpuUnmeasured},
        {"main",    platform::TaskState::Running,  0, 1, 2340, platform::kTaskCpuUnmeasured},
        {"esp_timer", platform::TaskState::Blocked, -1, 22, 800, platform::kTaskCpuUnmeasured},
        {"mmEncode", platform::TaskState::Ready,   1, 5, 600, platform::kTaskCpuUnmeasured},
    };
    TaskSeam seam(snap, 6, "main");
    Owner owner;
    TaskTable table(owner);
    table.ensureFresh();
    REQUIRE(table.count() == 6);
    const char* want[] = {"main", "mmEncode", "mmSend", "IDLE0", "Tmr Svc", "esp_timer"};
    for (uint8_t i = 0; i < 6; i++) CHECK(std::strcmp(table.task(i).name, want[i]) == 0);
    CHECK(std::strcmp(TaskTable::stateName(table.task(0).state), "running") == 0);
    CHECK(std::strcmp(TaskTable::stateName(table.task(4).state), "blocked") == 0);
}

// Each core's running task is marked, so the page names what each core runs.
TEST_CASE("TaskTable: the task each core ran at the snapshot is marked running now") {
    const platform::TaskInfo snap[] = {
        {"main",  platform::TaskState::Running, 0, 1, 2340, platform::kTaskCpuUnmeasured},
        {"IDLE1", platform::TaskState::Running, 1, 0, 900, platform::kTaskCpuUnmeasured},
        {"wifi",  platform::TaskState::Blocked, 0, 23, 4000, platform::kTaskCpuUnmeasured},
    };
    TaskSeam seam(snap, 3, "main");
    Owner owner;
    TaskTable table(owner);
    table.ensureFresh();
    REQUIRE(table.count() == 3);
    for (uint8_t i = 0; i < table.count(); i++) {
        const bool running = std::strcmp(table.task(i).name, "wifi") != 0;
        CHECK_MESSAGE(table.runningNow(table.task(i)) == running, table.task(i).name);
    }
}

#ifndef ESP_PLATFORM
// A computer has no RTOS, so its task table is the process's threads: the one mm_main runs on reads render, each with a CPU share once a second snapshot has an earlier one to compare.
TEST_CASE("TaskTable: on a computer the table is the process's threads, the render loop's named render, with CPU shares from the second snapshot on") {
    platform::setTestTaskSnapshot(nullptr, 0, "");
    platform::recordSystemHeap();   // as mm_main does first: this thread is the render loop's
    platform::TaskInfo rows[64];
    REQUIRE(platform::taskSnapshot(rows, 64) >= 1);
    // This thread runs, so its share rises above zero; a busy runner charges CPU time late, so the spin goes on until a share shows, bounded.
    bool render = false, measured = false;
    const uint32_t t0 = platform::micros();
    while (!measured && platform::micros() - t0 < 500000) {
        const uint32_t s0 = platform::micros();
        while (platform::micros() - s0 < 20000) {}
        const size_t n = platform::taskSnapshot(rows, 64);
        for (size_t i = 0; i < n; i++) {
            render = render || std::strcmp(rows[i].name, "render") == 0;
            measured = measured || (rows[i].cpuPermille != platform::kTaskCpuUnmeasured && rows[i].cpuPermille > 0);
        }
    }
    CHECK(render);
    CHECK(measured);
    CHECK(std::strcmp(platform::renderTaskName(), "render") == 0);   // the thread mm_main started on, named in the table
}

// A computer's memory sheet: the process's resident memory and what the machine has available.
TEST_CASE("platform: a computer reports its process's resident memory and the machine's available memory") {
    size_t resident = 0, available = 0;
    platform::processMemory(resident, available);
    CHECK(resident > 0);
    CHECK(available > 0);
}
#endif
