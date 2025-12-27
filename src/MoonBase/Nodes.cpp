/**
    @title     MoonLight
    @file      Nodes.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT
  #include "Nodes.h"

  #include <ESP32SvelteKit.h>  //for safeModeMB

void Node::updateControl(const JsonObject& control) {
  // EXT_LOGD(ML_TAG, "onUpdate %s", control["name"].as<const char*>());
  // if (oldValue == "") return;                                                              // newControl, value already set
  if (!control["name"].isNull() && !control["type"].isNull() && !control["p"].isNull()) {  // name and type can be null if control is removed in compareRecursive
    int pointer = control["p"];
    EXT_LOGD(ML_TAG, "%s = %s t:%s p:%p", control["name"].as<const char*>(), control["value"].as<String>().c_str(), control["type"].as<const char*>(), (void*)pointer);

    if (pointer) {
      if (control["type"] == "slider" || control["type"] == "select" || control["type"] == "pin" || control["type"] == "number") {
        if (control["size"] == 8) {
          uint8_t* valuePointer = (uint8_t*)pointer;
          *valuePointer = control["value"];
          // EXT_LOGV(ML_TAG, "%s = %d", control["name"].as<const char*>(), *valuePointer);
        } else if (control["size"] == 16) {
          uint16_t* valuePointer = (uint16_t*)pointer;
          *valuePointer = control["value"];
          // EXT_LOGV(ML_TAG, "%s = %d", control["name"].as<const char*>(), *valuePointer);
        } else if (control["size"] == 32) {
          int* valuePointer = (int*)pointer;
          *valuePointer = control["value"];
          // EXT_LOGV(ML_TAG, "%s = %d", control["name"].as<const char*>(), *valuePointer);
        } else if (control["size"] == 33) {
          float* valuePointer = (float*)pointer;
          *valuePointer = control["value"];
          // EXT_LOGV(ML_TAG, "%s = %d", control["name"].as<const char*>(), *valuePointer);
        } else {
          EXT_LOGW(ML_TAG, "size not supported or not set for %s: %d", control["name"].as<const char*>(), control["size"].as<int>());
        }
      } else if (control["type"] == "selectFile" || control["type"] == "text") {
        char* valuePointer = (char*)pointer;
        size_t maxLen = control["max"].isNull() ? 32 : control["max"].as<size_t>();
        const char* src = control["value"].as<const char*>();
        size_t copyLen = maxLen > 0 ? maxLen - 1 : 0;
        if (copyLen > 0 && src) {
          strncpy(valuePointer, src, copyLen);
          valuePointer[copyLen] = '\0';
        } else {
          valuePointer[0] = '\0';
        }
      } else if (control["type"] == "checkbox" && control["size"] == sizeof(bool)) {
        bool* valuePointer = (bool*)pointer;
        *valuePointer = control["value"].as<bool>();
      } else if (control["type"] == "coord3D" && control["size"] == sizeof(Coord3D)) {
        Coord3D* valuePointer = (Coord3D*)pointer;
        *valuePointer = control["value"].as<Coord3D>();
      } else
        EXT_LOGE(ML_TAG, "type of %s not compatible: %s (%d)", control["name"].as<const char*>(), control["type"].as<const char*>(), control["size"].as<uint8_t>());
    }
  }
};

  #if FT_LIVESCRIPT

    #define USE_FASTLED  // as ESPLiveScript.h calls hsv ! one of the reserved functions!!
    #include "ESPLiveScript.h"

Node* gNode = nullptr;

static void _addControl(uint8_t* var, char* name, char* type, uint8_t min = 0, uint8_t max = UINT8_MAX) {
  EXT_LOGV(ML_TAG, "%s %s %d (%d-%d)", name, type, var, min, max);
  gNode->addControl(*var, name, type, min, max);
}
static void _nextPin() { layerP.nextPin(); }
static void _addLight(uint8_t x, uint8_t y, uint8_t z) { layerP.addLight({x, y, z}); }

static void _modifySize() { gNode->modifySize(); }
static void _modifyPosition(Coord3D& position) { gNode->modifyPosition(position); }  // need &position parameter
// static void _modifyXYZ() {gNode->modifyXYZ();}//need &position parameter

void _fadeToBlackBy(uint8_t fadeValue) { gNode->layer->fadeToBlackBy(fadeValue); }
static void _setRGB(uint16_t indexV, CRGB color) { gNode->layer->setRGB(indexV, color); }
static void _setRGBPal(uint16_t indexV, uint8_t index, uint8_t brightness) { gNode->layer->setRGB(indexV, ColorFromPalette(PartyColors_p, index, brightness)); }
static void _setPan(uint16_t indexV, uint8_t value) { gNode->layer->setPan(indexV, value); }
static void _setTilt(uint16_t indexV, uint8_t value) { gNode->layer->setTilt(indexV, value); }

volatile xSemaphoreHandle WaitAnimationSync = xSemaphoreCreateBinary();

void sync() {
  static uint32_t frameCounter = 0;
  frameCounter++;
  delay(1);  // feed the watchdog, otherwise watchdog will reset the ESP
  // Serial.print("s");
  // 🌙 adding semaphore wait too long logging
  if (xSemaphoreTake(WaitAnimationSync, pdMS_TO_TICKS(100)) == pdFALSE) {
    EXT_LOGW(ML_TAG, "WaitAnimationSync wait too long");
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
    EXT_LOGE(ML_TAG, "Failed to parse function definition: %s", definition.c_str());
  }
}

Parser parser = Parser();

void LiveScriptNode::setup() {
  // EXT_LOGV(ML_TAG, "animation %s", animation);

  if (animation[0] != '/') {  // no sc script
    return;
  }

  // make sure types in below functions are correct !!! otherwise livescript will crash

  // generic functions
  addExternal("uint32_t millis()", (void*)millis);
  addExternal("uint32_t now()", (void*)millis);  // todo: synchronized time (sys->now)
  addExternal("uint16_t random16(uint16_t)", (void*)(uint16_t (*)(uint16_t))random16);
  addExternal("void delay(uint32_t)", (void*)delay);
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
  addExternal("float hypot(float,float)", (void*)(float (*)(float, float))hypot);
  addExternal("float beat8(uint8_t,uint32_t)", (void*)beat8);  // saw wave
  addExternal("uint8_t triangle8(uint8_t)", (void*)triangle8);

  // MoonLight functions
  addExternal("void addControl(void*,char*,char*,uint8_t,uint8_t)", (void*)_addControl);
  addExternal("void nextPin()", (void*)_nextPin);
  addExternal("void addLight(uint8_t,uint8_t,uint8_t)", (void*)_addLight);
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
  addExternal("void setRGB(uint16_t,CRGB)", (void*)_setRGB);
  addExternal("void setRGBPal(uint16_t,uint8_t,uint8_t)", (void*)_setRGBPal);
  addExternal("void setPan(uint16_t,uint8_t)", (void*)_setPan);
  addExternal("void setTilt(uint16_t,uint8_t)", (void*)_setTilt);
  addExternal("uint8_t width", &layer->size.x);
  addExternal("uint8_t height", &layer->size.y);
  addExternal("uint8_t depth", &layer->size.z);
  addExternal("bool on", &on);

  //   for (asm_external el: external_links) {
  //       EXT_LOGV(ML_TAG, "elink %s %s %d", el.shortname.c_str(), el.name.c_str(), el.type);
  //   }

  //   runningPrograms.setPrekill(layerP.ledsDriver.preKill, layerP.ledsDriver.postKill); //for clockless driver...
  runningPrograms.setFunctionToSync(sync);

  compileAndRun();
}

void LiveScriptNode::loop() {
  // Serial.print("l");
  xSemaphoreGive(WaitAnimationSync);
}

void LiveScriptNode::onLayout() {
  if (hasOnLayout()) {
    EXT_LOGV(ML_TAG, "%s", animation);
    scriptRuntime.execute(animation, "onLayout");
  }
}

LiveScriptNode::~LiveScriptNode() {
  EXT_LOGV(ML_TAG, "%s", animation);
  scriptRuntime.kill(animation);
}

// LiveScriptNode functions

void LiveScriptNode::compileAndRun() {
  // send UI spinner

  // run the recompile not in httpd but in main loopTask (otherwise we run out of stack space)
  //  runInAppTask.push_back([&, animation, type, error] {
  EXT_LOGV(ML_TAG, "%s", animation);
  File file = ESPFS.open(animation);
  if (file) {
    Char<32> pre;
    pre.format("#define NUM_LEDS %d\n", layer->nrOfLights);

    std::string scScript = pre.c_str();
    scScript += file.readString().c_str();
    file.close();

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

    EXT_LOGV(ML_TAG, "script \n%s", scScript.c_str());

    // EXT_LOGV(ML_TAG, "parsing %s", scScript.c_str());

    Executable executable = parser.parseScript(&scScript);  // note that this class will be deleted after the function call !!!
    executable.name = animation;
    EXT_LOGV(ML_TAG, "parsing %s done", animation);
    scriptRuntime.addExe(executable);  // if already exists, delete it first
    EXT_LOGV(ML_TAG, "addExe success %s", executable.exeExist ? "true" : "false");

    gNode = this;  // todo: this is not working well with multiple scripts running!!!

    if (executable.exeExist) {
      execute();
    } else
      EXT_LOGV(ML_TAG, "error %s", executable.error.error_message.c_str());

    // send error to client ... not working yet
    //  error.set(executable.error.error_message); //String(executable.error.error_message.c_str());
    //  _state.data["nodes"][2]["error"] = executable.error.error_message;
  }
  // });

  // stop UI spinner
}

void LiveScriptNode::execute() {
  if (safeModeMB) {
    EXT_LOGW(ML_TAG, "Safe mode enabled, not executing script %s", animation);
    return;
  }
  EXT_LOGV(ML_TAG, "%s", animation);

  requestMappings();  // requestMapPhysical and requestMapVirtual will call the script onLayout function (check if this can be done in case the script also has loop running !!!)

  if (hasLoopFunction) {
    // setup : create controls
    // executable.execute("setup");
    // send controls to UI
    // executable.executeAsTask("main"); //background task (async - vs sync)
    EXT_LOGV(ML_TAG, "%s executeAsTask main", animation);
    scriptRuntime.executeAsTask(animation, "main");  // background task (async - vs sync)
    // assert failed: xEventGroupSync event_groups.c:228 (uxBitsToWaitFor != 0)
  } else {
    EXT_LOGV(ML_TAG, "%s execute main", animation);
    scriptRuntime.execute(animation, "main");
  }
  EXT_LOGV(ML_TAG, "%s execute started", animation);
}

void LiveScriptNode::kill() {
  EXT_LOGV(ML_TAG, "%s", animation);
  scriptRuntime.kill(animation);
}

void LiveScriptNode::free() {
  EXT_LOGV(ML_TAG, "%s", animation);
  scriptRuntime.free(animation);
}

void LiveScriptNode::killAndDelete() {
  EXT_LOGV(ML_TAG, "%s", animation);
  scriptRuntime.kill(animation);
  // scriptRuntime.free(animation);
  scriptRuntime.deleteExe(animation);
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
    object["error"] = exec.error.error_message;
    object["kill"] = 0;
    object["free"] = 0;
    object["delete"] = 0;
    object["execute"] = 0;
    // EXT_LOGV(ML_TAG, "scriptRuntime exec %s r:%d h:%d, e:%d h:%d b:%d + d:%d = %d", exec.name.c_str(), exec.isRunning(), exec.isHalted, exec.exeExist, exec.__run_handle_index, exeInfo.binary_size,
    // exeInfo.data_size, exeInfo.total_size);
  }
}

  #endif  // FT_LIVESCRIPT

  #if HP_ALL_DRIVERS
I2SClocklessLedDriver ledsDriver;
  #endif

void DriverNode::setup() {
  addControl(layerP.lights.header.lightPreset, "lightPreset", "select");
  addControlValue("RGB");
  addControlValue("RBG");
  addControlValue("GRB");  // default WS2812
  addControlValue("GBR");
  addControlValue("BRG");
  addControlValue("BGR");
  addControlValue("RGBW");                      // e.g. 4 channel par/dmx light
  addControlValue("GRBW");                      // rgbw LED eg. sk6812
  addControlValue("Curtain GRB6");              // some LED curtains
  addControlValue("Curtain RGB2040");           // curtain RGB2040
  addControlValue("Lightbar RGBWYP");           // 6 channel par/dmx light with UV etc
  addControlValue("MH BeeEyes 150W-15 🐺");     // 15 channels moving head, see https://moonmodules.org/MoonLight/moonlight/drivers/#art-net
  addControlValue("MH BeTopper 19x15W-32 🐺");  // 32 channels moving head
  addControlValue("MH 19x15W-24");              // 24 channels moving heads
}

void DriverNode::loop() {
  LightsHeader* header = &layerP.lights.header;

  // use ledsDriver LUT for super efficient leds dimming 🔥 (used by reOrderAndDimRGBW)

  uint8_t brightness = (header->offsetBrightness == UINT8_MAX) ? header->brightness : 255;  // set brightness to 255 if offsetBrightness is set (fixture will do its own brightness)

  if (brightness != brightnessSaved || layerP.maxPower != maxPowerSaved) {
    // Use FastLED for setMaxPowerInMilliWatts stuff
    uint8_t correctedBrightness = calculate_max_brightness_for_power_mW((CRGB*)&layerP.lights.channelsD, layerP.lights.header.nrOfLights, brightness, layerP.maxPower * 1000);
    // EXT_LOGD(ML_TAG, "setBrightness b:%d + p:%d -> cb:%d", brightness, layerP.maxPower, correctedBrightness);
    ledsDriver.setBrightness(correctedBrightness);
    brightnessSaved = brightness;
    maxPowerSaved = layerP.maxPower;
  }

  #if HP_ALL_DRIVERS
  if (savedColorCorrection.red != layerP.lights.header.red || savedColorCorrection.green != layerP.lights.header.green || savedColorCorrection.blue != layerP.lights.header.blue) {
    ledsDriver.setGamma(layerP.lights.header.red / 255.0, layerP.lights.header.blue / 255.0, layerP.lights.header.green / 255.0, 1.0);
    // EXT_LOGD(ML_TAG, "setColorCorrection r:%d, g:%d, b:%d (%d %d %d)", layerP.lights.header.red, layerP.lights.header.green, layerP.lights.header.blue,
    // savedColorCorrection.red, savedColorCorrection.green, savedColorCorrection.blue);
    savedColorCorrection.red = layerP.lights.header.red;
    savedColorCorrection.green = layerP.lights.header.green;
    savedColorCorrection.blue = layerP.lights.header.blue;
  }
  #else  // ESP32_LEDSDRIVER
  CRGB correction;
  uint8_t white;
  ledsDriver.getColorCorrection(correction.red, correction.green, correction.blue, white);
  if (correction.red != header->red || correction.green != header->green || correction.blue != header->blue) {
    EXT_LOGD(ML_TAG, "setColorCorrection r:%d, g:%d, b:%d (%d %d %d)", header->red, header->green, header->blue, correction.red, correction.green, correction.blue);
    ledsDriver.setColorCorrection(header->red, header->green, header->blue);
  }
  #endif
}

void DriverNode::onUpdate(const Char<20>& oldValue, const JsonObject& control) {
  LightsHeader* header = &layerP.lights.header;

  EXT_LOGD(ML_TAG, "%s: %s ", control["name"].as<const char*>(), control["value"].as<String>().c_str());

  if (control["name"] == "lightPreset") {
    uint8_t oldChannelsPerLight = header->channelsPerLight;

    header->resetOffsets();

    switch (header->lightPreset) {
    case lightPreset_RGB:
      header->channelsPerLight = 3;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      break;
    case lightPreset_RBG:
      header->channelsPerLight = 3;
      header->offsetRed = 0;
      header->offsetGreen = 2;
      header->offsetBlue = 1;
      break;
    case lightPreset_GRB:
      header->channelsPerLight = 3;
      header->offsetRed = 1;
      header->offsetGreen = 0;
      header->offsetBlue = 2;
      break;
    case lightPreset_GBR:
      header->channelsPerLight = 3;
      header->offsetRed = 2;
      header->offsetGreen = 0;
      header->offsetBlue = 1;
      break;
    case lightPreset_BRG:
      header->channelsPerLight = 3;
      header->offsetRed = 1;
      header->offsetGreen = 2;
      header->offsetBlue = 0;
      break;
    case lightPreset_BGR:
      header->channelsPerLight = 3;
      header->offsetRed = 2;
      header->offsetGreen = 1;
      header->offsetBlue = 0;
      break;
    case lightPreset_RGBW:
      header->channelsPerLight = 4;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      header->offsetWhite = 3;
      break;
    case lightPreset_GRBW:
      header->channelsPerLight = 4;
      header->offsetRed = 1;
      header->offsetGreen = 0;
      header->offsetBlue = 2;
      header->offsetWhite = 3;
      break;
    case lightPreset_GRB6:
      header->channelsPerLight = 6;
      header->offsetRed = 1;
      header->offsetGreen = 0;
      header->offsetBlue = 2;
      break;
    case lightPreset_RGB2040:
      // RGB2040 uses standard RGB offsets but has special channel remapping
      // for dual-channel-group architecture (handled in VirtualLayer)
      header->channelsPerLight = 3;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      break;
    case lightPreset_RGBWYP:
      header->channelsPerLight = 6;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      header->offsetWhite = 3;
      break;
    case lightPreset_MHBeeEyes150W15:
      header->channelsPerLight = 15;  // set channels per light to 15 (RGB + Pan + Tilt + Zoom + Brightness)
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      header->offsetRGB = 10;  // set offset for RGB lights in DMX map
      header->offsetPan = 0;
      header->offsetTilt = 1;
      header->offsetZoom = 7;
      header->offsetBrightness = 8;   // set offset for brightness
      header->offsetGobo = 5;         // set offset for color wheel in DMX map
      header->offsetBrightness2 = 3;  // set offset for color wheel brightness in DMX map    } //BGR
      break;
    case lightPreset_MHBeTopper19x15W32:
      header->channelsPerLight = 32;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      header->offsetRGB = 9;
      header->offsetRGB1 = 13;
      header->offsetRGB2 = 17;
      header->offsetRGB3 = 24;
      header->offsetPan = 0;
      header->offsetTilt = 2;
      header->offsetZoom = 5;
      header->offsetBrightness = 6;
      break;
    case lightPreset_MH19x15W24:
      header->channelsPerLight = 24;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      header->offsetPan = 0;
      header->offsetTilt = 1;
      header->offsetBrightness = 3;
      header->offsetRGB = 4;
      header->offsetRGB1 = 8;
      header->offsetRGB2 = 12;
      header->offsetZoom = 17;
      break;
    default:
      EXT_LOGW(ML_TAG, "Invalid lightPreset value: %d", header->lightPreset);
      // Fall back to GRB (most common default)
      header->lightPreset = lightPreset_GRB;
      header->channelsPerLight = 3;
      header->offsetRed = 1;
      header->offsetGreen = 0;
      header->offsetBlue = 2;
      break;
    }

    EXT_LOGI(ML_TAG, "setLightPreset %d (cPL:%d, o:%d,%d,%d,%d)", header->lightPreset, header->channelsPerLight, header->offsetRed, header->offsetGreen, header->offsetBlue, header->offsetWhite);

    // FASTLED_ASSERT(true, "oki");

  #if HP_ALL_DRIVERS
    #ifndef CONFIG_IDF_TARGET_ESP32P4
    if (initDone) {
      // ledsDriver.setOffsets(header->offsetRed, header->offsetGreen, header->offsetBlue, header->offsetWhite);

      // if (oldChannelsPerLight != header->channelsPerLight)
      //   restartNeeded = true; //in case
    }
    #endif

  #else  // ESP32_LEDSDRIVER
    if (ledsDriver.initLedsDone) {
      ledsDriver.setOffsets(header->offsetRed, header->offsetGreen, header->offsetBlue, header->offsetWhite);

      if (oldChannelsPerLight != header->channelsPerLight) restartNeeded = true;  // in case
    }
  #endif

    lightPresetSaved = true;
  }
}

void DriverNode::reOrderAndDimRGBW(uint8_t* packetRGBChannel, uint8_t* lightsRGBChannel) {
  // use ledsDriver.__rbg_map[0]; for super fast brightness and gamma correction! see secondPixel in ESP32-LedDriver!
  // apply the LUT to the RGB channels !

  packetRGBChannel[layerP.lights.header.offsetRed] = ledsDriver.__red_map[lightsRGBChannel[0]];
  packetRGBChannel[layerP.lights.header.offsetGreen] = ledsDriver.__green_map[lightsRGBChannel[1]];
  packetRGBChannel[layerP.lights.header.offsetBlue] = ledsDriver.__blue_map[lightsRGBChannel[2]];
  if (layerP.lights.header.offsetWhite != UINT8_MAX) packetRGBChannel[layerP.lights.header.offsetWhite] = ledsDriver.__white_map[lightsRGBChannel[3]];
}

#endif  // FT_MOONLIGHT