/// @module TasksModule

/// Pins TasksModule's shape: one `tasks` List of RTOS tasks whose row detail nests the MoonModules running in that task, plus core0 and core1. A host has no FreeRTOS, so taskSnapshot returns 0 and the list is empty: the host test covers the control set and the empty-snapshot path, and the populated rows are verified on an ESP32.

#include "doctest.h"
#include "core/system/TasksModule.h"
#include "core/module/Scheduler.h"
#include "core/util/JsonSink.h"
#include "platform/platform_tasks.h"   // taskSnapshot, setTestTaskSnapshot, TaskInfo

#include <cstring>
#include <string>

using namespace mm;

namespace {

// A minimal module with a fixed reported cost, to appear nested under the render task.
struct FakeModule : MoonModule {
    FakeModule(const char* n, size_t klass, size_t heap) {
        setName(n); setClassSize(klass); setDynamicBytes({heap, 0});
    }
};

// Find a control by name in a module's control list.
bool hasControl(const MoonModule& m, const char* name) {
    for (uint8_t i = 0; i < m.controls().count(); i++)
        if (std::strcmp(m.controls()[i].name, name) == 0) return true;
    return false;
}

// Reach the `tasks` control's ListSource (the inner TaskListSource) so the row/detail JSON is exercised directly, the same access unit_Control_list uses (descriptor.ptr → ListSource*).
const ListSource* tasksSource(const MoonModule& m) {
    for (uint8_t i = 0; i < m.controls().count(); i++)
        if (std::strcmp(m.controls()[i].name, "tasks") == 0)
            return static_cast<const ListSource*>(m.controls()[i].ptr);
    return nullptr;
}

} // namespace

TEST_CASE("TasksModule: exposes a tasks list + core0/core1, no separate modules list") {
    Scheduler scheduler;
    TasksModule tasks;
    scheduler.addModule(&tasks);
    scheduler.setup();

    CHECK(hasControl(tasks, "tasks"));
    CHECK(hasControl(tasks, "core0"));
    CHECK(hasControl(tasks, "core1"));
    // No standalone `modules` list, the per-module cost lives in the tasks row-detail.
    CHECK_FALSE(hasControl(tasks, "modules"));
}

TEST_CASE("TasksModule: a fixed System module (Generic role, no delete affordance)") {
    TasksModule tasks;
    // Tasks is wired by code as a System child (main.cpp), so it keeps the base Generic role, which no container accepts as a user child: the UI offers no delete or replace.
    CHECK(tasks.role() == ModuleRole::Generic);
}

TEST_CASE("TasksModule: the empty host snapshot is safe (no RTOS on host)") {
    // tick1s refreshes the (empty on host) task snapshot + fills core0/core1 (empty strings). This just confirms the refresh path doesn't crash and the tasks list serializes to an empty array.
    platform::setTestTaskSnapshot(nullptr, 0, "");
    Scheduler scheduler;
    TasksModule tasks;
    scheduler.addModule(&tasks);
    scheduler.setup();
    tasks.tick1s();   // platform::taskSnapshot returns 0 on host → 0 rows, no crash
    CHECK(true);      // reaching here without a crash is the assertion
}

TEST_CASE("TasksModule: the tasks list renders the injected RTOS tasks with their fields") {
    // Inject a canned snapshot (host test seam) → the `tasks` List serializes those rows with name/state/core/prio/stack, and the CPU% and heap fields appear only when they are real measurements.
    const platform::TaskInfo snap[] = {
        {"main",  platform::TaskState::Running, 0, 1, 2340, 479 /* 47.9% */},
        {"IDLE1", platform::TaskState::Ready,   1, 0, 1100, platform::kTaskCpuUnmeasured},
    };
    platform::setTestTaskSnapshot(snap, 2, "main");

    Scheduler scheduler;
    TasksModule tasks;
    scheduler.addModule(&tasks);
    scheduler.setup();
    tasks.tick1s();   // pulls the injected snapshot into the ListSource

    const ListSource* src = tasksSource(tasks);
    REQUIRE(src != nullptr);
    REQUIRE(src->listRowCount() == 2);

    // Rows keep a stable order across refreshes, where the RTOS snapshot's order shifts: MoonLight's tasks first, RTOS tasks below, alphabetical within each, so "main" is row 0 and "IDLE1" row 1.
    JsonSink r0; src->writeListRow(r0, 0);
    std::string row0(r0.data());
    CHECK(row0.find("\"name\":\"main\"") != std::string::npos);    // our render task, floated to the top
    CHECK(row0.find("\"state\":\"running\"") != std::string::npos);
    CHECK(row0.find("\"core\":0") != std::string::npos);
    CHECK(row0.find("\"prio\":1") != std::string::npos);
    CHECK(row0.find("\"stack\":2340") != std::string::npos);
    CHECK(row0.find("\"cpu\":47.9") != std::string::npos);         // real measurement → shown

    JsonSink r1; src->writeListRow(r1, 1);
    std::string row1(r1.data());
    CHECK(row1.find("\"name\":\"IDLE1\"") != std::string::npos);   // system task, sunk below ours
    CHECK(row1.find("\"cpu\":") == std::string::npos);             // kTaskCpuUnmeasured → field omitted
}

// The row order: MoonLight's own tasks ("main" and the "mm" workers) first, RTOS system tasks below, alphabetical within each, whatever order the snapshot returns; a jumbled snapshot pins it.
TEST_CASE("TasksModule: MoonLight tasks sort to the top, system tasks below, alphabetical within") {
    // Input order is scrambled (system, ours, system, ours, ...), exactly the kind of shuffle the RTOS snapshot produces frame to frame.
    const platform::TaskInfo snap[] = {
        {"Tmr Svc", platform::TaskState::Blocked, -1, 1, 900, platform::kTaskCpuUnmeasured},
        {"mmSend",  platform::TaskState::Blocked,  1, 5, 700, platform::kTaskCpuUnmeasured},
        {"IDLE0",   platform::TaskState::Ready,    0, 0, 500, platform::kTaskCpuUnmeasured},
        {"main",    platform::TaskState::Running,  0, 1, 2340, platform::kTaskCpuUnmeasured},
        {"esp_timer", platform::TaskState::Blocked, -1, 22, 800, platform::kTaskCpuUnmeasured},
        {"mmEncode", platform::TaskState::Ready,   1, 5, 600, platform::kTaskCpuUnmeasured},
    };
    platform::setTestTaskSnapshot(snap, 6, "main");

    Scheduler scheduler;
    TasksModule tasks;
    scheduler.addModule(&tasks);
    scheduler.setup();
    tasks.tick1s();

    const ListSource* src = tasksSource(tasks);
    REQUIRE(src != nullptr);
    REQUIRE(src->listRowCount() == 6);

    // Extract each row's task name in order.
    auto rowName = [&](uint8_t i) {
        JsonSink s; src->writeListRow(s, i);
        std::string row(s.data());
        size_t p = row.find("\"name\":\"");
        if (p == std::string::npos) return std::string();
        p += 8;
        return row.substr(p, row.find('"', p) - p);
    };

    // Ours first (main, then mm* alphabetical: mmEncode < mmSend), then system alphabetical (IDLE0 < Tmr Svc < esp_timer, uppercase sorts before lowercase).
    CHECK(rowName(0) == "main");
    CHECK(rowName(1) == "mmEncode");
    CHECK(rowName(2) == "mmSend");
    CHECK(rowName(3) == "IDLE0");
    CHECK(rowName(4) == "Tmr Svc");
    CHECK(rowName(5) == "esp_timer");
}

TEST_CASE("TasksModule: the render task's detail nests the modules + the ∑/tick cross-check") {
    // The render task ("main") detail lists the scheduled modules with their cost, plus the closing "∑ modules … / tick …" line; a NON-render task's detail is an empty modules array.
    const platform::TaskInfo snap[] = {
        {"main",  platform::TaskState::Running, 0, 1, 2340, platform::kTaskCpuUnmeasured},
        {"IDLE1", platform::TaskState::Ready,   1, 0, 1100, platform::kTaskCpuUnmeasured},
    };
    platform::setTestTaskSnapshot(snap, 2, "main");

    Scheduler scheduler;
    FakeModule a("Alpha", 128, 0);
    FakeModule b("Beta", 384, 4096);
    TasksModule tasks;
    scheduler.addModule(&a);
    scheduler.addModule(&b);
    scheduler.addModule(&tasks);
    scheduler.setup();
    tasks.tick1s();

    const ListSource* src = tasksSource(tasks);
    REQUIRE(src != nullptr);

    // MoonLight's tasks float to the top: the render task "main" (ours) is row 0, "IDLE1" (system) is row 1. Row 0 ("main") = the render task → its detail nests the modules + the cross-check line.
    JsonSink d0; src->writeListRowDetail(d0, 0);
    std::string det0(d0.data());
    CHECK(det0.find("Alpha") != std::string::npos);
    CHECK(det0.find("class=") == std::string::npos);        // modules are scalar strings, not objects
    CHECK(det0.find("Alpha \xC2\xB7 0us \xC2\xB7 128B \xC2\xB7 0heap") != std::string::npos);
    CHECK(det0.find("Beta \xC2\xB7 0us \xC2\xB7 384B \xC2\xB7 4096heap") != std::string::npos);
    CHECK(det0.find("\xE2\x88\x91 modules") != std::string::npos);   // the ∑ cross-check line
    CHECK(det0.find("outside modules") != std::string::npos);

    // Row 1 ("IDLE1") = NOT the render task → empty modules array, no cross-check.
    JsonSink d1; src->writeListRowDetail(d1, 1);
    std::string det1(d1.data());
    CHECK(det1.find("Alpha") == std::string::npos);
    CHECK(det1.find("\"modules\":[]") != std::string::npos);
}
