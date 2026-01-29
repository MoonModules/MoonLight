/**
    @title     MoonBase
    @file      ModuleTasks.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/tasks/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleTasks_h
#define ModuleTasks_h

#if FT_MOONBASE == 1

  #include "MoonBase/Module.h"

class ModuleTasks : public Module {
 public:
  ModuleTasks(PsychicHttpServer* server, ESP32SvelteKit* sveltekit) : Module("tasks", server, sveltekit) { EXT_LOGV(MB_TAG, "constructor"); }

  void setupDefinition(const JsonArray& controls) override {
    EXT_LOGV(MB_TAG, "");
    JsonObject control;  // state.data has one or more properties
    JsonArray rows;      // if a control is an array, this is the rows of the array

  #ifndef CONFIG_IDF_TARGET_ESP32C3
    addControl(controls, "core0", "text", 0, 32, true);
    addControl(controls, "core1", "text", 0, 32, true);
  #endif

    control = addControl(controls, "tasks", "rows");
    control["filter"] = "";
    control["crud"] = "r";
    rows = control["n"].to<JsonArray>();
    {
      addControl(rows, "name", "text", 0, 32, true);
      addControl(rows, "summary", "text", 0, 32, true);
      // addControl(rows, "state", "text", 0, 32, true);
      // addControl(rows, "cpu", "text", 0, 32, true);
      // addControl(rows, "prio", "number", 0, 255, true);
      addControl(rows, "stack", "text", 0, 32, true);
      addControl(rows, "runtime", "text", 0, 32, true);
      // addControl(rows, "core", "number", 0, 65538, true);
    }
  }

  void loop1s() {
    if (!_socket->getConnectedClients()) return;  // 🌙 No need for UI tasks
    if (!WiFi.localIP() && !ETH.localIP()) return;

  #define MAX_TASKS 30

    TaskStatus_t taskStatusArray[MAX_TASKS];
    UBaseType_t taskCount;
    uint32_t totalRunTime = 1;

    // Get all tasks' info
    taskCount = uxTaskGetSystemState(taskStatusArray, MAX_TASKS, &totalRunTime);

    // Sort the taskStatusArray by task name
    std::sort(taskStatusArray, taskStatusArray + taskCount, [](const TaskStatus_t& a, const TaskStatus_t& b) { return strcmp(a.pcTaskName, b.pcTaskName) < 0; });

    // printf("Found %d tasks\n", taskCount);
    // printf("Name\t\tState\tPrio\tStack\tRun Time\tCPU %%\tCore\n");

    JsonDocument doc;
    doc["tasks"].to<JsonArray>();
    JsonObject newState = doc.as<JsonObject>();

    for (UBaseType_t i = 0; i < taskCount; i++) {
      JsonObject task = newState["tasks"].as<JsonArray>().add<JsonObject>();

      TaskStatus_t* ts = &taskStatusArray[i];

      const char* state;
      switch (ts->eCurrentState) {
      case eRunning:
        state = "🏃‍♂️";
        break;  // Running
      case eReady:
        state = "🧍‍♂️";
        break;  // Ready
      case eBlocked:
        state = "🚧";
        break;  // Blocked
      case eSuspended:
        state = "⏸️";
        break;  // Suspended
      case eDeleted:
        state = "🗑️";
        break;  // Deleted
      default:
        state = "❓";
        break;  // Unknown
      }

      Char<32> text;
      text.format("%s %d%% @P%d @C%d", state, (uint32_t)(100ULL * ts->ulRunTimeCounter / totalRunTime), ts->uxCurrentPriority, ts->xCoreID == tskNO_AFFINITY ? -1 : ts->xCoreID);

      task["name"] = ts->pcTaskName;
      task["summary"] = text.c_str();
      // task["state"] = state;
      // task["cpu"] = cpu_percent.c_str();
      // task["prio"] = ts->uxCurrentPriority;
      if (equal(ts->pcTaskName, "AppEffects"))
        text.format("%d - %d", ts->usStackHighWaterMark, EFFECTS_STACK_SIZE);
      else if (equal(ts->pcTaskName, "AppDrivers"))
        text.format("%d - %d", ts->usStackHighWaterMark, DRIVERS_STACK_SIZE);
      else if (equal(ts->pcTaskName, "ESP32 SvelteKit"))
        text.format("%d - %d", ts->usStackHighWaterMark, SVELTEKIT_STACK_SIZE);
      else if (equal(ts->pcTaskName, "httpd"))
        text.format("%d - %d", ts->usStackHighWaterMark, HTTPD_STACK_SIZE);
      else
        text.format("%d", ts->usStackHighWaterMark);

      task["stack"] = text.c_str();
      task["runtime"] = ts->ulRunTimeCounter / 1000000;  // in seconds
      // task["core"] = ts->xCoreID == tskNO_AFFINITY ? -1 : ts->xCoreID;

      // printf("%-12s %-10s %4u\t%5u\t%10lu\t%s\t%d\n",
      //     ts->pcTaskName,
      //     state,
      //     ts->uxCurrentPriority,
      //     ts->usStackHighWaterMark,
      //     ts->ulRunTimeCounter,
      //     cpu_percent.c_str(), ts->xCoreID==tskNO_AFFINITY?-1:ts->xCoreID);
    }

  #ifndef CONFIG_IDF_TARGET_ESP32C3
    TaskHandle_t current0 = xTaskGetCurrentTaskHandleForCore(0);
    TaskHandle_t current1 = xTaskGetCurrentTaskHandleForCore(1);

    newState["core0"] = pcTaskGetName(current0);
    newState["core1"] = pcTaskGetName(current1);
  #endif

    // UpdatedItem updatedItem;
    // _state.compareRecursive("", _state.data, newState, updatedItem); //fill data with doc

    // _socket->emitEvent(_moduleName, newState);
    update(newState, ModuleState::update, _moduleName);
  }
};

#endif
#endif