# Live Scripts (Developer)

For end-user documentation (writing scripts, available functions, examples), see [MoonLight / Live Scripts](../moonlight/livescripts.md).

This page documents the internal architecture of MoonLight's LiveScript integration: how scripts are compiled, executed, synchronized with the effect pipeline, and cleaned up.

## Overview

MoonLight uses the [ESPLiveScript](https://github.com/hpwit/ESPLiveScript) library to compile and execute `.sc` scripts directly on the ESP32. The integration is in two files:

| File | Purpose |
|---|---|
| `src/MoonBase/LiveScriptNode.h` | Class declaration, flags, lifecycle method signatures |
| `src/MoonBase/LiveScriptNode.cpp` | Compilation, execution, sync, external function bindings |

`LiveScriptNode` extends `Node` and is guarded by the `FT_LIVESCRIPT` feature flag. It is instantiated by `ModuleEffects` and `ModuleDrivers` when a user selects a `.sc` file as an effect or layout.

## Lifecycle

```text
setup() → startCompile() → [compileTask] → compileAndRun()
                                                ↓
                                          needsExecute = true
                                          compileTask exits (frees 8KB stack)
                                                ↓
                              [loop20ms] → execute() → executeAsTask()
                                                            ↓
                                                   _run_task (FreeRTOS, 8KB)
                                                            ↓
                                              main() { setup(); while(true) { loop(); sync(); } }
```

### 1. setup()

Called when the node is created. Registers all external C++ functions and variables with the ESPLiveScript parser so scripts can call them. Then calls `startCompile()`.

Key registrations:

- **Generic**: `millis()`, `random16()`, `delay()`, `pinMode()`, `digitalWrite()`
- **Math**: `sin()`, `cos()`, `sin8()`, `cos8()`, `atan2()`, `inoise8()`, `beatsin8()`, `beat8()`, `triangle8()`, `hypot()`
- **LED control**: `setRGB()`, `setRGBXY()`, `setRGBXYZ()`, `getRGB()`, `setHSV()`, `setRGBPal()`, `fadeToBlackBy()`, `ColorFromPalette()`
- **Moving heads**: `setPan()`, `setTilt()`
- **Layout**: `addLight()`, `nextPin()`, `addControl()`, `modifySize()`
- **Drawing**: `drawLine()`, `drawCircle()`
- **Palette**: `setPalEntry()`, `setPalEntryHSV()`
- **Variables**: `width`, `height`, `depth`, `on`, `leds`, `bands`, `volume`, `gravityX/Y/Z`, `hour`, `minute`, `second`

The `sync()` function is registered via `runningPrograms.setFunctionToSync(sync)` — ESPLiveScript calls this at each script `sync()` point.

### 2. startCompile()

Spawns a one-shot FreeRTOS task (`compileTask`, 8KB stack, priority 1) to run `compileAndRun()`. Compilation runs off the main task to avoid blocking the HTTP/WS server (the parser needs significant stack space).

If a compile is already in progress (`compileInProgress == true`), sets `needsCompile = true` instead. `NodeManager::loop20ms()` picks this up and calls `startCompile()` again once the current compile finishes.

!!! note "Deferred execution for D0 heap"
    `compileAndRun()` does **not** call `execute()` directly. Instead it sets `needsExecute = true` and returns, allowing the compile task to exit and free its 8KB stack. `NodeManager::loop20ms()` then picks up the flag and calls `execute()`. This ensures the compile task's stack is freed before `executeAsTask()` tries to allocate another 8KB for the run task — critical on ESP32-D0 where heap is tight.

### 3. compileAndRun()

Runs inside the `compileTask`. Steps:

1. Opens the `.sc` file from ESPFS
2. Prepends `#define NUM_LEDS <nrOfLights>` so the script knows the LED count
3. Scans the source for `setup()`, `loop()`, `onLayout()`, `modifyPosition(` to set capability flags
4. Appends a `main()` wrapper:
   ```c
   void main() {
     setup();
     while(true) { if(on) { loop(); sync(); } else delay(1); }
   }
   ```
   The `on` variable allows pausing the script during layout remapping passes.
5. Calls `parser.parseScript()` to compile the script to an `Executable`
6. Calls `scriptRuntime.addExe()` to register it (replaces any previous version)
7. If compilation succeeded (`exeExist == true`), sets `needsExecute = true` (picked up by `loop20ms()` after the compile task exits)

### 4. execute()

Requests physical/virtual layer remapping (which triggers `onLayout()` if the script defines it), then starts the script:

- **Scripts with `loop()`**: Calls `scriptRuntime.executeAsTask()` which creates a FreeRTOS task (`_run_task`, 8KB stack, priority 3, core 0) that runs the compiled `main()` function. After the call, the task handle is verified — if NULL (task creation failed due to low heap), `hasLoopTask` stays `false` and an error is logged.

- **Scripts without `loop()`** (e.g. static palettes): Runs `scriptRuntime.execute()` synchronously.

### 5. Destruction and kill

When the user switches to a different effect or the node is deleted:

- **`~LiveScriptNode()`**: Waits for any in-progress compile to finish (`while (compileInProgress)`), then calls `scriptRuntime.kill()`. Includes a guard against the ESPLiveScript `freeSync()` crash (see Known Issues below).

- **`kill()`**: Sets `hasLoopTask = false`, unregisters the task-to-node mapping, then calls `scriptRuntime.kill()`. Same `freeSync()` guard applied.

- **`killAndDelete()`**: Calls `kill()` then `scriptRuntime.deleteExe()` to also free the compiled binary.

## Frame Synchronization

LiveScript tasks run concurrently with the `effectTask` (both on core 0). Frame synchronization ensures scripts write to the channel buffer at the right time and don't race with the buffer swap.

### Sync flow (per frame)

```text
effectTask (core 0, priority 3)          livescript _run_task (core 0, priority 3)
─────────────────────────────────        ──────────────────────────────────────────
layerP.loop()
  → LiveScriptNode::loop()
    → give WaitAnimationSync semaphore
    → scriptsToSync++
                                          ← script loop() runs, writes pixels
wait: ulTaskNotifyTake()                  → sync()
  ← xTaskNotifyGive(effectTaskHandle)       → notify effectTask
    scriptsToSync--                         → wait on WaitAnimationSync semaphore
[all scripts done]
swap channelsE ↔ channelsD
vTaskDelay(1)
next frame: give semaphore ──────────────→  ← semaphore released, next loop()
```

### Key primitives

| Primitive | Type | Purpose |
|---|---|---|
| `WaitAnimationSync` | Counting semaphore (max 4) | Gates script tasks — each waits here between frames |
| `scriptsToSync` | `volatile uint8_t` | Tracks how many scripts still need to complete their frame |
| `effectTaskHandle` | `TaskHandle_t` | Task notification target — scripts notify this when done |

### Safety mechanisms

- **Dead task detection** (`loop()`): Before signalling the semaphore, checks `exec->_isRunning`. If the script task has exited, sets `hasLoopTask = false` to prevent signalling a semaphore that nobody will consume.

- **1-second timeout** (`effectTask` in `main.cpp`): If 10 consecutive 100ms timeouts pass without any script completing a frame, `scriptsToSync` is forced to 0. This prevents the effect pipeline from blocking forever if a script task dies mid-frame.

- **Task start verification** (`execute()`): After `executeAsTask()`, checks the task handle. If NULL (creation failed), `hasLoopTask` stays false and `_isRunning` is forced false to prevent downstream crashes.

## Concurrent Script Support

Up to 4 LiveScript tasks can run simultaneously (limited by the `WaitAnimationSync` semaphore count and `gTaskNodeMap` size).

### Task-to-node mapping

Each script task needs to know which `LiveScriptNode` instance it belongs to, so that functions like `setRGB()` write to the correct virtual layer. This is handled by `gTaskNodeMap`:

```cpp
struct TaskNodePair { TaskHandle_t task; Node* node; };
static TaskNodePair gTaskNodeMap[MAX_LIVE_SCRIPTS];  // MAX_LIVE_SCRIPTS = 4
```

- `registerNodeForTask(h, this)` — called in `execute()` after task creation succeeds
- `unregisterNodeForTask(h)` — called in `kill()` before the task is deleted
- `currentNode()` — called by every external function wrapper (e.g. `_setRGB`). Looks up the calling task's handle in the map; falls back to `gNode` for synchronous contexts.

## Adding New External Functions

To expose a new C++ function to LiveScript:

1. Write a static wrapper in `LiveScriptNode.cpp` that calls through `currentNode()`:
   ```cpp
   static void _myFunction(uint16_t arg) {
     currentNode()->layer->myFunction(arg);
   }
   ```

2. Register it in `setup()` using `addExternal()`:
   ```cpp
   addExternal("void myFunction(uint16_t)", (void*)_myFunction);
   ```

3. Document the function in the [end-user API reference](../moonlight/livescripts.md#available-functions).

The `addExternal()` helper parses C-style function signatures (e.g. `"CRGB getRGB(uint16_t)"`) and registers them with the ESPLiveScript linker. Variables use the same mechanism without parentheses: `"uint8_t width"`.

!!! warning "Type safety"
    ESPLiveScript does not perform type checking at the ABI level. If the signature string doesn't match the actual function pointer's calling convention, the script will crash at runtime. Always verify parameter types match exactly.

## Known Issues and Workarounds

### freeSync() crash (ESP32-D0)

**Problem**: ESPLiveScript's `Executable::kill()` calls `freeSync()` which calls `xEventGroupSync(getMask())`. If `getMask()` returns 0 (no running task handles) but `_isRunning` is still `true`, FreeRTOS asserts: `uxBitsToWaitFor != 0`.

**Root cause**: In `execute.h`, `_isRunning` is set to `true` before `xTaskCreateUniversal` — if the task creation fails, `_isRunning` stays true but no handle is registered, so `getMask()` returns 0.

**Workaround** (in `~LiveScriptNode()` and `kill()`): Before calling `scriptRuntime.kill()`, check if `exec->_isRunning && runningPrograms.getMask() == 0`. If so, force `exec->_isRunning = false` to skip the `freeSync()` path.

### 0 lps deadlock

**Problem**: If a script task fails to start (task creation failed) or dies mid-execution, `LiveScriptNode::loop()` still signals `WaitAnimationSync` and increments `scriptsToSync`. The `effectTask` then blocks forever in the `while (scriptsToSync > 0)` loop because no script will ever call `sync()` to send the notification.

**Workarounds**:

1. `execute()` verifies the task handle after `executeAsTask()` — only sets `hasLoopTask = true` if the handle is non-NULL.
2. `loop()` checks `exec->_isRunning` each frame — if the task has exited, disables sync signalling.
3. `effectTask` has a 1-second timeout that forces `scriptsToSync = 0` as a last resort.

### Compile task stack

The ESPLiveScript parser requires significant stack space. The compile task is created with 8KB. On ESP32-D0 with tight heap, this allocation can fail — `startCompile()` checks the return value of `xTaskCreate` and logs an error if it fails.

## File Discovery

LiveScript `.sc` files are discovered by `ModuleEffects::addNodes()` and `ModuleDrivers::addNodes()` by scanning the ESPFS filesystem:

- Effect scripts: files with `E_` prefix and `.sc` extension
- Layout/driver scripts: files with `L_` or `D_` prefix and `.sc` extension
- Palette scripts: files with `P_` prefix, discovered by `ModuleLightsControl`

The files appear in the node selection dropdown alongside built-in C++ nodes, under the "LiveScript" category.
