/**
    @title     MoonLight
    @file      LiveScriptNode.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_LIVESCRIPT

  #include "LiveScriptNode.h"

  #include <ESP32SvelteKit.h>  //for safeModeMB
  #include <ESPFS.h>

  #define USE_FASTLED  // as ESPLiveScript.h calls hsv ! one of the reserved functions!!
  #include "ESPLiveScript.h"

Node* gNode = nullptr;  // fallback for synchronous (non-task) contexts such as onLayout

// Per-task node map: maps each script's FreeRTOS TaskHandle to its LiveScriptNode. 🌙
// Fixes the gNode race when multiple scripts run concurrently as separate tasks.
// Max 4 matches the WaitAnimationSync semaphore count.
static const int MAX_LIVE_SCRIPTS = 4;
struct TaskNodePair { TaskHandle_t task = nullptr; Node* node = nullptr; };
static TaskNodePair gTaskNodeMap[MAX_LIVE_SCRIPTS];

// Returns the Node for the calling task; falls back to gNode for synchronous contexts. 🌙
static Node* currentNode() {
  TaskHandle_t h = xTaskGetCurrentTaskHandle();
  for (const auto& m : gTaskNodeMap)
    if (m.task == h) return m.node;
  return gNode;
}
static void registerNodeForTask(TaskHandle_t task, Node* node) {
  for (auto& m : gTaskNodeMap)
    if (m.task == nullptr) { m.task = task; m.node = node; return; }
  EXT_LOGE(MB_TAG, "gTaskNodeMap full");
}
static void unregisterNodeForTask(TaskHandle_t task) {
  for (auto& m : gTaskNodeMap)
    if (m.task == task) { m.task = nullptr; m.node = nullptr; return; }
}

static void _addControl(uint8_t* var, char* name, char* type, uint8_t min = 0, uint8_t max = UINT8_MAX) {
  EXT_LOGV(MB_TAG, "%s %s %p (%d-%d)", name, type, (void*)var, min, max);
  currentNode()->addControl(*var, name, type, min, max);
}
static void _nextPin() { layerP.nextPin(); }
static void _addLight(uint16_t x, uint16_t y, uint16_t z) { layerP.addLight({x, y, z}); }

static void _modifySize() { currentNode()->modifySize(); }
static void _modifyPosition(Coord3D& position) { currentNode()->modifyPosition(position); }  // need &position parameter
// static void _modifyXYZ() {currentNode()->modifyXYZ();}//need &position parameter

void _fadeToBlackBy(uint8_t fadeValue) { currentNode()->layer->fadeToBlackBy(fadeValue); }
static CRGB _getRGB(uint16_t indexV) { return currentNode()->layer->getRGB(indexV); }
static void _setRGB(uint16_t indexV, CRGB color) { currentNode()->layer->setRGB(indexV, color); }
static void _setRGBXY(int x, int y, CRGB color) { currentNode()->layer->setRGB(Coord3D{x, y}, color); }  // 🌙 coordinate-based setRGB, exposed via preamble-injected setRGB(Coord3D,CRGB) wrapper
static void _setRGBXYZ(int x, int y, int z, CRGB color) { currentNode()->layer->setRGB(Coord3D{x, y, z}, color); }  // 🌙 coordinate-based setRGB, exposed via preamble-injected setRGB(Coord3D,CRGB) wrapper
static CRGB _colorFromPalette(uint8_t index, uint8_t bri) { return ColorFromPalette(layerP.palette, index, bri); }  // 🌙
static void _setRGBPal(uint16_t indexV, uint8_t index, uint8_t brightness) { currentNode()->layer->setRGB(indexV, ColorFromPalette(layerP.palette, index, brightness)); }
static void _setPan(uint16_t indexV, uint8_t value) { currentNode()->layer->setPan(indexV, value); }
static void _setTilt(uint16_t indexV, uint8_t value) { currentNode()->layer->setTilt(indexV, value); }
static void _setPalEntry(uint8_t index, uint8_t r, uint8_t g, uint8_t b) { if (index < 16) layerP.palette.entries[index] = CRGB(r, g, b); }
static void _setPalEntryHSV(uint8_t index, uint8_t h, uint8_t s, uint8_t v) { if (index < 16) layerP.palette.entries[index] = CHSV(h, s, v); }
static void _setHSV(uint16_t indexV, uint8_t h, uint8_t s, uint8_t v) { currentNode()->layer->setRGB(indexV, CHSV(h, s, v)); }
static void _setHSVXY(int x, int y, uint8_t h, uint8_t s, uint8_t v) { currentNode()->layer->setRGB(Coord3D{x, y}, CHSV(h, s, v)); }

// 2D drawing
static void _drawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, CRGB color) { currentNode()->layer->drawLine(x0, y0, x1, y1, color); }
static void _drawCircle(int cx, int cy, uint8_t radius, CRGB color) { currentNode()->layer->drawCircle(cx, cy, radius, color, false); }

// time of day
static uint8_t _lsHour = 0;
static uint8_t _lsMinute = 0;
static uint8_t _lsSecond = 0;
static void _updateTime() {
  time_t now = time(nullptr);
  struct tm t;
  if (localtime_r(&now, &t)) { _lsHour = t.tm_hour; _lsMinute = t.tm_min; _lsSecond = t.tm_sec; }
}

volatile SemaphoreHandle_t WaitAnimationSync = xSemaphoreCreateCounting(4, 0);  // max 4 concurrent scripts
volatile uint8_t scriptsToSync = 0;                                             // count of scripts that still need to finish their frame
extern TaskHandle_t effectTaskHandle;
static volatile bool compileInProgress = false;  // 🌙 declared here so destructor can reference it

// 🌙 Stack sizes for livescript tasks. Tune per board after checking actual usage:
//   - compile task: serial log "compileTask stack high-water mark" after each compile
//   - run task: "stack" column in Live Scripts module UI
// S3/P4: larger stacks in PSRAM. D0: smaller stacks in internal RAM.
// Both compile and run tasks use __LS_STACK_CAPS from ESPLiveScript (PSRAM on S3/P4, internal on D0).
// File I/O (ESPFS.open) is done in startCompile() on the main task, so the compile task is flash-I/O-free.
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4)
  static constexpr uint32_t compileTaskStackSize = 16384;
  static constexpr uint32_t runTaskStackSize = 8192;
#else
  static constexpr uint32_t compileTaskStackSize = 8192;
  static constexpr uint32_t runTaskStackSize = 4096;
#endif

void sync() {
  static uint32_t frameCounter = 0;
  frameCounter++;
  xTaskNotifyGive(effectTaskHandle);  // signal "frame done" to effectTask
  delay(1);                           // feed the watchdog, otherwise watchdog will reset the ESP
  // 🌙 adding semaphore wait too long logging
  if (xSemaphoreTake(WaitAnimationSync, pdMS_TO_TICKS(500)) == pdFALSE) {
    EXT_LOGW(MB_TAG, "WaitAnimationSync wait too long");
    xSemaphoreTake(WaitAnimationSync, portMAX_DELAY);
  }
}

void addExternal(string definition, void* ptr) {
  bool success = false;
  size_t firstSpace = definition.find(' ');
  if (firstSpace != std::string::npos) {
    string returnType = definition.substr(0, firstSpace);

    string parameters = "";
    string functionName = "";

    size_t openParen = definition.find('(', firstSpace + 1);
    if (openParen != std::string::npos) {  // function
      functionName = definition.substr(firstSpace + 1, openParen - firstSpace - 1);

      size_t closeParen = definition.find(')', openParen + 1);
      if (closeParen != std::string::npos) {  // function with parameters
        parameters = definition.substr(openParen + 1, closeParen - openParen - 1);
        success = true;
        if (findLink(functionName, externalType::function) == -1)  // not allready added earlier
          addExternalFunction(functionName, returnType, parameters, ptr);
      }
    } else {  // variable
      functionName = definition.substr(firstSpace + 1);
      success = true;
      if (findLink(functionName, externalType::value) == -1)  // not allready added earlier
        addExternalVariable(functionName, returnType, "", ptr);
    }
  }
  if (!success) {
    EXT_LOGE(MB_TAG, "Failed to parse function definition: %s", definition.c_str());
  }
}

Parser parser = Parser();

void LiveScriptNode::setup() {
  // EXT_LOGV(MB_TAG, "animation %s", animation.c_str());

  if (animation[0] != '/') {  // no sc script
    return;
  }

  // make sure types in below functions are correct !!! otherwise livescript will crash

  // generic functions
  addExternal("uint32_t millis()", (void*)millis);
  addExternal("uint32_t now()", (void*)millis);  // todo: synchronized time (sys->now)
  addExternal("uint8_t random8(uint8_t)", (void*)(uint8_t (*)(uint8_t))random8);
  addExternal("uint16_t random16(uint16_t)", (void*)(uint16_t (*)(uint16_t))random16);
  addExternal("void delay(uint32_t)", (void*)((void (*)(uint32_t))delay));
  addExternal("void pinMode(uint8_t,uint8_t)", (void*)pinMode);
  addExternal("void digitalWrite(uint8_t,uint8_t)", (void*)digitalWrite);

  // trigonometric functions
  addExternal("float sin(float)", (void*)(float (*)(float))sin);
  addExternal("float cos(float)", (void*)(float (*)(float))cos);
  addExternal("uint8_t sin8(uint8_t)", (void*)sin8);
  addExternal("uint8_t cos8(uint8_t)", (void*)cos8);
  addExternal("float atan2(float,float)", (void*)(float (*)(float, float))atan2);
  addExternal("uint8_t inoise8(uint16_t,uint16_t,uint16_t)", (void*)(uint8_t (*)(uint16_t, uint16_t, uint16_t))inoise8);
  addExternal("uint8_t beatsin8(uint16_t,uint8_t,uint8_t,uint32_t,uint8_t)", (void*)beatsin8);
  addExternal("uint8_t beatsin16(uint16_t,uint16_t,uint16_t,uint32_t,uint8_t)", (void*)beatsin16);
  addExternal("float hypot(float,float)", (void*)(float (*)(float, float))hypot);
  addExternal("uint8_t beat8(uint16_t,uint32_t)", (void*)(uint8_t (*)(uint16_t, uint32_t))beat8);  // saw wave
  addExternal("uint8_t triangle8(uint8_t)", (void*)triangle8);

  // MoonLight functions
  addExternal("void addControl(void*,char*,char*,uint8_t,uint8_t)", (void*)_addControl);
  addExternal("void nextPin()", (void*)_nextPin);
  addExternal("void addLight(uint16_t,uint16_t,uint16_t)", (void*)_addLight);
  addExternal("void modifySize()", (void*)_modifySize);
  //   addExternal(    "void modifyPosition(Coord3D &position)", (void *)_modifyPosition);
  //   addExternal(    "void modifyXYZ(uint16_t,uint16_t,uint16_t)", (void *)_modifyXYZ);

  // MoonLight Parallel LED Driver vars
  //   but keep enabled to avoid compile errors when used in non virtual context
  //   addExternal( "uint8_t colorOrder", &layerP.ledsDriver.colorOrder);
  //   addExternal( "uint8_t clockPin", &layerP.ledsDriver.clockPin);
  //   addExternal( "uint8_t latchPin", &layerP.ledsDriver.latchPin);
  //   addExternal( "uint8_t clockFreq", &layerP.ledsDriver.clockFreq);
  //   addExternal( "uint8_t dmaBuffer", &layerP.ledsDriver.dmaBuffer);

  addExternal("void fadeToBlackBy(uint8_t)", (void*)_fadeToBlackBy);
  addExternal("CRGB* leds", (void*)(CRGB*)layerP.lights.channelsE);
  addExternal("CRGB getRGB(uint16_t)", (void*)_getRGB);
  addExternal("void setRGB(uint16_t,CRGB)", (void*)_setRGB);
  addExternal("void setRGBXY(int,int,CRGB)", (void*)_setRGBXY);  // 🌙 called by preamble-injected setRGB(Coord3D,CRGB)
  addExternal("void setRGBXYZ(int,int,int,CRGB)", (void*)_setRGBXYZ);  // 🌙 called by preamble-injected setRGB(Coord3D,CRGB)
  addExternal("CRGB ColorFromPalette(uint8_t,uint8_t)", (void*)_colorFromPalette);  // 🌙
  addExternal("void setRGBPal(uint16_t,uint8_t,uint8_t)", (void*)_setRGBPal);
  addExternal("void setPan(uint16_t,uint8_t)", (void*)_setPan);
  addExternal("void setTilt(uint16_t,uint8_t)", (void*)_setTilt);
  addExternal("void setPalEntry(uint8_t,uint8_t,uint8_t,uint8_t)", (void*)_setPalEntry);
  addExternal("void setPalEntryHSV(uint8_t,uint8_t,uint8_t,uint8_t)", (void*)_setPalEntryHSV);
  addExternal("void setHSV(uint16_t,uint8_t,uint8_t,uint8_t)", (void*)_setHSV);
  addExternal("void setHSVXY(int,int,uint8_t,uint8_t,uint8_t)", (void*)_setHSVXY);
  addExternal("uint8_t width", &layer->size.x);
  addExternal("uint8_t height", &layer->size.y);
  addExternal("uint8_t depth", &layer->size.z);
  addExternal("bool on", &on);

  // audio sync
  addExternal("uint8_t* bands", (void*)sharedData.bands);
  addExternal("float volume", &sharedData.volume);

  // gyro / IMU
  addExternal("int gravityX", &sharedData.gravity.x);
  addExternal("int gravityY", &sharedData.gravity.y);
  addExternal("int gravityZ", &sharedData.gravity.z);

  // 2D drawing
  addExternal("void drawLine(uint8_t,uint8_t,uint8_t,uint8_t,CRGB)", (void*)_drawLine);
  addExternal("void drawCircle(int,int,uint8_t,CRGB)", (void*)_drawCircle);

  // time of day
  _updateTime();
  addExternal("uint8_t hour", &_lsHour);
  addExternal("uint8_t minute", &_lsMinute);
  addExternal("uint8_t second", &_lsSecond);

  //   for (asm_external el: external_links) {
  //       EXT_LOGV(MB_TAG, "elink %s %s %d", el.shortname.c_str(), el.name.c_str(), el.type);
  //   }

  //   runningPrograms.setPrekill(layerP.ledsDriver.preKill, layerP.ledsDriver.postKill); //for clockless driver...
  runningPrograms.setFunctionToSync(sync);

  startCompile();
}

void LiveScriptNode::loop() {
  _updateTime();  // keep hour/minute/second current for clock scripts
  if (!hasLoopTask) return;  // 🌙 only sync scripts whose loop task is actually running
  // 🌙 Check if the livescript task is still alive. ESPLiveScript sets _isRunning=false and
  // removes the handle when the task exits (normally or via error). If that happened,
  // stop signalling WaitAnimationSync to prevent effectTask deadlock (0 lps).
  Executable* exec = scriptRuntime.findExecutable(animation.c_str());
  if (!exec || !exec->_isRunning) {
    EXT_LOGW(MB_TAG, "%s: livescript task no longer running, disabling sync", animation.c_str());
    hasLoopTask = false;
    return;
  }
  // 🌙 Only increment scriptsToSync when the give succeeds. If the semaphore is full
  // (more than 4 concurrent loop scripts) the give fails and we skip this frame rather
  // than incrementing a counter that effectTask will wait on but never receive.
  if (xSemaphoreGive(WaitAnimationSync) == pdTRUE) {
    scriptsToSync += 1;
  } else {
    EXT_LOGW(MB_TAG, "WaitAnimationSync full — skipping frame for %s", animation.c_str());
  }
}

void LiveScriptNode::onLayout() {
  if (hasOnLayout()) {
    EXT_LOGV(MB_TAG, "%s", animation.c_str());
    // Call onLayout directly without @__footer, which would reset global variables
    // (including control values) to their compiled defaults
    Executable* exec = scriptRuntime.findExecutable(animation.c_str());
    if (exec) {
      Arguments args;
      executeBinary("@__onLayout", exec->_executecmd, 9999, exec, args);
    }
  }
}

LiveScriptNode::~LiveScriptNode() {
  EXT_LOGV(MB_TAG, "%s", animation.c_str());
  // 🌙 Wait for any in-progress compile task to finish before freeing this node,
  // to prevent the compileTask from accessing a dangling this pointer.
  while (compileInProgress) delay(10);
  // 🌙 Guard against ESPLiveScript freeSync() crash: if getMask()==0 but _isRunning
  // is true, xEventGroupSync asserts (uxBitsToWaitFor != 0). Force _isRunning false
  // so Executable::kill() skips the sync path.
  Executable* exec = scriptRuntime.findExecutable(animation.c_str());
  if (exec && exec->_isRunning && runningPrograms.getMask() == 0) {
    EXT_LOGW(MB_TAG, "%s: mask=0 but _isRunning=true, forcing stop to avoid assert", animation.c_str());
    exec->_isRunning = false;
  }
  scriptRuntime.kill(animation.c_str());
}

// LiveScriptNode functions

static TaskHandle_t compileTaskHandle = nullptr;
static std::string compileScript;  // script source read by startCompile(), consumed by compileTask

static void compileTask(void* param) {
  LiveScriptNode* node = static_cast<LiveScriptNode*>(param);
  node->compileAndRun();
  compileScript.clear();
  compileScript.shrink_to_fit();  // free the string memory
  // 🌙 Log stack high-water mark so we can tune the compile task stack size.
  UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
  EXT_LOGI(MB_TAG, "compileTask stack high-water mark: %u bytes free (of %u)", hwm * sizeof(StackType_t), compileTaskStackSize);
  compileInProgress = false;
  TaskHandle_t h = compileTaskHandle;
  compileTaskHandle = nullptr;
  vTaskDeleteWithCaps(h);
}

void LiveScriptNode::startCompile() {
  if (!compileInProgress) {
    // 🌙 Read the .sc file here (on the main task with internal RAM stack) so the compile task
    // doesn't do SPI flash I/O — allowing it to use a PSRAM stack on S3/P4.
    File file = ESPFS.open(animation.c_str());
    if (!file) {
      EXT_LOGE(MB_TAG, "startCompile: cannot open %s", animation.c_str());
      return;
    }
    Char<32> pre;
    pre.format("#define NUM_LEDS %d\n", layer->nrOfLights);
    compileScript = pre.c_str();
    compileScript += file.readString().c_str();
    file.close();

    compileInProgress = true;
    if (xTaskCreateWithCaps(compileTask, "lsCompile", compileTaskStackSize, this, 1, &compileTaskHandle, __LS_STACK_CAPS) != pdPASS) {
      EXT_LOGE(MB_TAG, "startCompile xTaskCreate failed");  // 🌙
      compileInProgress = false;                            // 🌙 prevent permanent lock-out on task creation failure
      compileScript.clear();
      compileScript.shrink_to_fit();
    }
  } else {
    needsCompile = true;  // picked up by NodeManager::loop20ms when current compile finishes
  }
}

void LiveScriptNode::compileAndRun() {
  // send UI spinner

  // File reading is done in startCompile() (on main task) so this task doesn't do flash I/O.
  EXT_LOGV(MB_TAG, "%s", animation.c_str());
  if (!compileScript.empty()) {
    std::string& scScript = compileScript;  // use the pre-read script source

    hasSetupFunction = false;
    hasLoopFunction = false;
    hasLoopTask = false;  // 🌙 reset before recompile; set again in execute() if task starts
    hasOnLayoutFunction = false;
    hasModifyFunction = false;

    if (scScript.find("setup()") != std::string::npos) hasSetupFunction = true;
    if (scScript.find("loop()") != std::string::npos) hasLoopFunction = true;
    if (scScript.find("onLayout()") != std::string::npos) hasOnLayoutFunction = true;
    if (scScript.find("modifyPosition(") != std::string::npos) hasModifyFunction = true;
    //   if (scScript.find("modifyXYZ(") != std::string::npos) hasModifier = true;

    // add main function
    scScript += "void main(){";
    if (hasSetupFunction) scScript += "setup();";
    if (hasLoopFunction) scScript += "while(true){if(on){loop();sync();}else delay(1);}";  // loop must pauze when layout changes pass == 1! delay to avoid idle
    scScript += "}";

    EXT_LOGV(MB_TAG, "script \n%s", scScript.c_str());

    // EXT_LOGV(MB_TAG, "parsing %s", scScript.c_str());

    uint32_t parseStart = millis();
    Executable executable = parser.parseScript(&scScript);  // note that this class will be deleted after the function call !!!
    executable.name = animation.c_str();
    uint32_t parseTime = millis() - parseStart;
    EXT_LOGI(MB_TAG, "parsing %s done in %dms (exeExist=%s)", animation.c_str(), parseTime,
             executable.exeExist ? "true" : "false");
    scriptRuntime.addExe(executable);  // if already exists, delete it first
    EXT_LOGV(MB_TAG, "addExe success %s", executable.exeExist ? "true" : "false");

    gNode = this;  // fallback for the brief window before registerNodeForTask() and for synchronous scripts

    if (executable.exeExist) {
      // 🌙 Don't call execute() here — we're still inside the compileTask (8KB stack).
      // Deferring to needsExecute lets the compile task exit first, freeing its stack
      // before executeAsTask() tries to allocate another 8KB for the run task.
      // On D0 with tight heap, this makes the difference between success and failure.
      needsExecute = true;
    } else
      EXT_LOGV(MB_TAG, "error %s", executable.error.error_message.c_str());

    // send error to client ... not working yet
    //  error.set(executable.error.error_message); //String(executable.error.error_message.c_str());
    //  _state.data["nodes"][2]["error"] = executable.error.error_message;
  }
  // });

  // stop UI spinner
}

void LiveScriptNode::execute() {
  if (safeModeMB) {
    EXT_LOGW(MB_TAG, "Safe mode enabled, not executing script %s", animation.c_str());
    return;
  }
  EXT_LOGV(MB_TAG, "%s", animation.c_str());

  requestMappings();  // requestMapPhysical and requestMapVirtual will call the script onLayout function (check if this can be done in case the script also has loop running !)

  if (hasLoopFunction) {
    // setup : create controls
    // executable.execute("setup");
    // send controls to UI
    // executable.executeAsTask("main"); //background task (async - vs sync)
    EXT_LOGV(MB_TAG, "%s executeAsTask main (stack %u)", animation.c_str(), runTaskStackSize);
    scriptRuntime.executeAsTask(animation.c_str(), "main", runTaskStackSize);  // background task (async - vs sync)
    // 🌙 Verify the task actually started. ESPLiveScript's _run() doesn't check the return
    // value of xTaskCreateUniversal — if it fails (e.g. low heap on D0), _isRunning is true
    // but the task handle is NULL. Setting hasLoopTask in that case would cause effectTask
    // to deadlock in the scriptsToSync wait loop (0 lps).
    Executable* exec = scriptRuntime.findExecutable(animation.c_str());
    bool taskStarted = false;
    if (exec && exec->__run_handle_index != 9999) {
      TaskHandle_t h = *runningPrograms.getHandleByIndex(exec->__run_handle_index);
      if (h) {
        registerNodeForTask(h, this);
        taskStarted = true;
      } else {
        EXT_LOGE(MB_TAG, "%s: task handle NULL after executeAsTask — task creation likely failed (low heap?)", animation.c_str());
        exec->_isRunning = false;  // prevent freeSync crash later
      }
    } else {
      EXT_LOGE(MB_TAG, "%s: no valid handle index after executeAsTask (index=%d)", animation.c_str(),
               exec ? exec->__run_handle_index : -1);
    }
    hasLoopTask = taskStarted;  // 🌙 only signal WaitAnimationSync if the task is actually running
  } else {
    EXT_LOGV(MB_TAG, "%s execute main", animation.c_str());
    scriptRuntime.execute(animation.c_str(), "main");
    // 🌙 No loop — script ran synchronously. If it also has no onLayout, it's fully
    // done and we can free the executable now. Layout scripts must keep the executable
    // alive until onLayout() runs (triggered asynchronously by requestMapPhysical).
    if (!hasOnLayoutFunction) {
      scriptRuntime.deleteExe(animation.c_str());
      EXT_LOGI(MB_TAG, "%s: no-loop script finished, executable freed", animation.c_str());
    }
  }
  EXT_LOGV(MB_TAG, "%s execute started", animation.c_str());
}

void LiveScriptNode::kill() {
  EXT_LOGV(MB_TAG, "%s", animation.c_str());
  hasLoopTask = false;  // 🌙 task is being killed; loop() must not signal WaitAnimationSync
  // 🌙 Unregister task → node mapping before the task is deleted.
  Executable* exec = scriptRuntime.findExecutable(animation.c_str());
  if (exec && exec->__run_handle_index != 9999) {
    TaskHandle_t h = *runningPrograms.getHandleByIndex(exec->__run_handle_index);
    if (h) unregisterNodeForTask(h);
  }
  // 🌙 Guard against ESPLiveScript freeSync() crash: if getMask()==0 but _isRunning
  // is true, xEventGroupSync asserts (uxBitsToWaitFor != 0). Force _isRunning false
  // so Executable::kill() skips the sync path.
  if (exec && exec->_isRunning && runningPrograms.getMask() == 0) {
    EXT_LOGW(MB_TAG, "%s: mask=0 but _isRunning=true, forcing stop to avoid assert", animation.c_str());
    exec->_isRunning = false;
  }
  scriptRuntime.kill(animation.c_str());
}

void LiveScriptNode::free() {
  EXT_LOGV(MB_TAG, "%s", animation.c_str());
  scriptRuntime.free(animation.c_str());
}

void LiveScriptNode::killAndDelete() {
  EXT_LOGV(MB_TAG, "%s", animation.c_str());
  kill();
  // scriptRuntime.free(animation.c_str());
  scriptRuntime.deleteExe(animation.c_str());
};

void LiveScriptNode::getScriptsJson(JsonArray scripts) {
  for (Executable& exec : scriptRuntime._scExecutables) {
    exe_info exeInfo = scriptRuntime.getExecutableInfo(exec.name);
    JsonObject object = scripts.add<JsonObject>();
    object["name"] = exec.name;
    object["isRunning"] = exec.isRunning();
    object["isHalted"] = exec.isHalted;
    object["exeExist"] = exec.exeExist;
    object["handle"] = exec.__run_handle_index;
    object["binary_size"] = exeInfo.binary_size;
    object["data_size"] = exeInfo.data_size;
    // 🌙 Show run task stack usage: "free / total" (only when task is running)
    Char<32> stackText;
    if (exec.isRunning() && exec.__run_handle_index != 9999) {
      TaskHandle_t h = *runningPrograms.getHandleByIndex(exec.__run_handle_index);
      if (h) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(h);
        stackText.format("%u / %u", (unsigned)(hwm * sizeof(StackType_t)), runTaskStackSize);
      } else {
        stackText = "no handle";
      }
    } else {
      stackText = "-";
    }
    object["stack"] = stackText.c_str();
    object["error"] = exec.error.error_message;
    object["kill"] = 0;
    object["free"] = 0;
    object["delete"] = 0;
    object["execute"] = 0;
  }
}

#endif  // FT_LIVESCRIPT
