/**
    @title     MoonLight
    @file      Nodes.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/general/utilities/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact moonmodules@icloud.com
**/

#if FT_MOONLIGHT
#include "Nodes.h"

void Node::updateControl(JsonObject control) {
    if (!control["name"].isNull() && !control["type"].isNull() && !control["p"].isNull()) { //name and type can be null if controll is removed in compareRecursive
        ESP_LOGD(TAG, "%s = %s t:%s p:%s", control["name"].as<String>().c_str(), control["value"].as<String>().c_str(), control["type"].as<String>().c_str(), control["p"].as<String>().c_str());
        int pointer = control["p"];

        if (pointer) {
        if (control["type"] == "range" || control["type"] == "select" || control["type"] == "pin") {
            uint8_t *valuePointer = (uint8_t *)pointer;
            *valuePointer = control["value"];
            ESP_LOGD(TAG, "%s = %d", control["name"].as<String>().c_str(), *valuePointer);
        }
        else if (control["type"] == "selectFile") {
            char *valuePointer = (char *)pointer;
            strncpy(valuePointer, control["value"].as<String>().c_str(), control["max"].isNull()?32:control["max"]);
        }
        else if (control["type"] == "number") {
            uint16_t *valuePointer = (uint16_t *)pointer;
            *valuePointer = control["value"];
        }
        else if (control["type"] == "checkbox") {
            bool *valuePointer = (bool *)pointer;
            *valuePointer = control["value"];
        }
        // else if (control["type"] == "coord3D") {
        //   Coord3D *valuePointer = (Coord3D *)pointer;
        //   *valuePointer = value;
        // }
        else
            ESP_LOGE(TAG, "type not supported yet %s", control["type"].as<String>().c_str());
        }
    }

    //if changed run map
    if (hasLayout) {
      ESP_LOGD(TAG, "layout control changed -> remap layout %s", name());
      layerV->requestMapPhysical = true;
      layerV->requestMapVirtual = true;
    }
};

#if FT_LIVESCRIPT

#define USE_FASTLED //as ESPLiveScript.h calls hsv ! one of the reserved functions!!
#include "ESPLiveScript.h"

Node *gNode = nullptr;

static void _addControl(void * ptr, char *name, char* type, int defaul, int min = 0, int max = 255) {ESP_LOGD(TAG, "%p %s %s %d (%d-%d)", ptr,  name, type, defaul, min, max);gNode->addControl(ptr, name, type, defaul, min, max);}
static void _addPin(uint8_t pinNr) {gNode->layerV->layerP->addPin(pinNr);}
static void _addLight(uint8_t x, uint8_t y, uint8_t z) {gNode->layerV->layerP->addLight({x, y, z});}

static void _modifyLayout() {gNode->modifyLayout();}
// static void _modifyLight() {gNode->modifyLight();} //need &position parameter
// static void _modifyXYZ() {gNode->modifyXYZ();}//need &position parameter

void _fadeToBlackBy(uint8_t fadeValue) { gNode->layerV->fadeToBlackBy(fadeValue);}
static void _setLight(uint16_t indexV, CRGB color) {gNode->layerV->setLight(indexV, color);}
static void _setLightPal(uint16_t indexV, uint8_t index, uint8_t brightness) { gNode->layerV->setLight(indexV, ColorFromPalette(PartyColors_p, index, brightness));}

static float _time(float j) {
    float myVal = millis();
    myVal = myVal / 65535 / j;           // PixelBlaze uses 1000/65535 = .015259. 
    myVal = fmod(myVal, 1.0);               // ewowi: with 0.015 as input, you get fmod(millis/1000,1.0), which has a period of 1 second, sounds right
    return myVal;
}

volatile xSemaphoreHandle WaitAnimationSync = xSemaphoreCreateBinary();

void sync() {
    static uint32_t frameCounter = 0;
    frameCounter++;
    delay(1); //feed the watchdog, otherwise watchdog will reset the ESP
    // Serial.print("s");
    xSemaphoreTake(WaitAnimationSync, portMAX_DELAY);
}

void addExternal(string definition, void * ptr) {
    bool success = false;
    size_t firstSpace = definition.find(' ');
    if (firstSpace != std::string::npos) {
        string returnType = definition.substr(0, firstSpace);

        string parameters = "";
        string functionName = "";

        size_t openParen = definition.find('(', firstSpace + 1);
        if (openParen != std::string::npos) { // function
            functionName = definition.substr(firstSpace + 1, openParen - firstSpace - 1);

            size_t closeParen = definition.find(')', openParen + 1);
            if (closeParen != std::string::npos) { // function with parameters
                parameters = definition.substr(openParen + 1, closeParen - openParen - 1);
                success = true;
                if (findLink(functionName, externalType::function) == -1) //not allready added earlier
                    addExternalFunction(functionName, returnType, parameters, ptr);
            }
        } else { // variable
            functionName = definition.substr(firstSpace + 1);
            success = true;
            if (findLink(functionName, externalType::value) == -1) //not allready added earlier
                addExternalVariable(functionName, returnType, "", ptr);
        }
    }
    if (!success) {
        ESP_LOGE(TAG, "Failed to parse function definition: %s", definition.c_str());
    }
}
  
Parser parser = Parser();

void LiveScriptNode::setup() {
    
  // ESP_LOGD(TAG, "animation %s", animation);

  if (animation[0] != '/') { //no sc script
      return;
  }

  //generic functions
  addExternal("uint32_t millis()", (void *)millis);
  addExternal("uint32_t now()", (void *)millis); //todo: synchronized time (sys->now)
  addExternal("uint16_t random16(uint16_t)", (void *)(uint16_t (*)(uint16_t))random16);
  addExternal(    "void delay(uint8_t)", (void *)delay);
  addExternal(    "void pinMode(int,int)", (void *)pinMode);
  addExternal(    "void digitalWrite(int,int)", (void *)digitalWrite);

  //trigonometric functions
  addExternal(   "float sin(float)", (void *)(float (*)(float))sin);
  addExternal(   "float cos(float)", (void *)(float (*)(float))cos);
  addExternal( "uint8_t sin8(uint8_t)", (void *)sin8);
  addExternal( "uint8_t cos8(uint8_t)", (void *)cos8);
  addExternal(   "float atan2(float,float)", (void*)(float (*)(float,float))atan2);
  addExternal( "uint8_t inoise8(uint16_t,uint16_t,uint16_t)", (void *)(uint8_t (*)(uint16_t,uint16_t,uint16_t))inoise8);
  addExternal(   "float hypot(float,float)", (void*)(float (*)(float,float))hypot);
  addExternal(   "float time(float)", (void *)_time);
  addExternal( "uint8_t triangle8(uint8_t)", (void *)triangle8);

  //MoonLight functions
  addExternal(    "void addControl(void*,char*,char*,int, int, int)", (void *)_addControl);
  addExternal(    "void addPin(uint8_t)", (void *)_addPin);
  addExternal(    "void addLight(uint8_t,uint8_t,uint8_t)", (void *)_addLight);
  addExternal(    "void modifyLayout()", (void *)_modifyLayout);
//   addExternal(    "void modifyLight(uint16_t,uint16_t,uint16_t)", (void *)_modifyLight);
//   addExternal(    "void modifyXYZ(uint16_t,uint16_t,uint16_t)", (void *)_modifyXYZ);


  //MoonLight physical and virtual driver vars
  //  but keep enabled to avoid compile errors when used in non virtual context
  addExternal( "uint8_t colorOrder", &layerV->layerP->ledsDriver.colorOrder);
  addExternal( "uint8_t clockPin", &layerV->layerP->ledsDriver.clockPin);
  addExternal( "uint8_t latchPin", &layerV->layerP->ledsDriver.latchPin);
  addExternal( "uint8_t clockFreq", &layerV->layerP->ledsDriver.clockFreq);
  addExternal( "uint8_t dmaBuffer", &layerV->layerP->ledsDriver.dmaBuffer);

  addExternal(    "void fadeToBlackBy(uint8_t)", (void *)_fadeToBlackBy);
  addExternal(   "CRGB* leds", (void *)layerV->layerP->lights.leds);
  addExternal(    "void setLight(uint16_t,CRGB)", (void *)_setLight);
  addExternal(    "void setLightPal(uint16_t,uint8_t,uint8_t)", (void *)_setLightPal);
  addExternal("uint16_t width", &layerV->size.x);
  addExternal("uint16_t height", &layerV->size.y);
  addExternal("uint16_t depth", &layerV->size.z);
  addExternal("bool on", &on);

  for (asm_external el: external_links) {
      ESP_LOGD(TAG, "elink %s %s %d", el.shortname.c_str(), el.name.c_str(), el.type);
  }

  runningPrograms.setPrekill(layerV->layerP->ledsDriver.preKill, layerV->layerP->ledsDriver.postKill); //for clockless driver...
  runningPrograms.setFunctionToSync(sync);

  compileAndRun();

    //   Node::setup(); //call Node::setup to handle requestMapLayout: no need to run as 
}

void LiveScriptNode::loop() {
    // Serial.print("l");
    xSemaphoreGive(WaitAnimationSync);
}

void LiveScriptNode::addLayout() {
    if (hasLayout) {
        scriptRuntime.execute(animation, "addLayout"); 
    }
}

void LiveScriptNode::destructor() {
    ESP_LOGD(TAG, "%s", animation);
    scriptRuntime.kill(animation);
}

// LiveScriptNode functions

void LiveScriptNode::compileAndRun() {
  //send UI spinner

  //run the recompile not in httpd but in main loopTask (otherwise we run out of stack space)
  // runInLoopTask.push_back([&, animation, type, error] {
      ESP_LOGD(TAG, "%s", animation);
      File file = ESPFS.open(animation);
      if (file) {
        Char<32> pre;
        pre.format("#define NUM_LEDS %d\n", layerV->nrOfLights);

          std::string scScript = pre.c_str();
          scScript += file.readString().c_str();
          file.close();

          if (scScript.find("setup()") != std::string::npos) hasSetup = true;
          if (scScript.find("loop()") != std::string::npos) hasLoop = true;
          if (scScript.find("addLayout()") != std::string::npos) hasLayout = true;
          if (scScript.find("modifyLight(") != std::string::npos) hasModifier = true;
        //   if (scScript.find("modifyXYZ(") != std::string::npos) hasModifier = true;

          //add main function
          scScript += "void main(){";
          if (hasSetup) scScript += "setup();";
          if (hasLoop) scScript += "while(true){if(on){loop();sync();}else delay(1);}"; //loop must pauze when layout changes pass == 1! delay to avoid idle
          scScript += "}";

          ESP_LOGD(TAG, "script \n%s", scScript.c_str());

          TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
          ESP_LOGI(TAG, "task %s %d", pcTaskGetName(currentTask), uxTaskGetStackHighWaterMark(currentTask));
  
          // ESP_LOGD(TAG, "parsing %s", scScript.c_str());

          Executable executable = parser.parseScript(&scScript); // note that this class will be deleted after the function call !!!
          executable.name = animation;
          ESP_LOGD(TAG, "parsing %s done", animation);
          scriptRuntime.addExe(executable); //if already exists, delete it first
          ESP_LOGD(TAG, "addExe success %s", executable.exeExist?"true":"false");

          gNode = this; //todo: this is not working well with multiple scripts running!!!

          if (executable.exeExist) {
            execute();
          } else
              ESP_LOGD(TAG, "error %s", executable.error.error_message.c_str());

          //send error to client ... not working yet
          // error.set(executable.error.error_message); //String(executable.error.error_message.c_str());
          // _state.data["nodes"][2]["error"] = executable.error.error_message;
      }
  // });
  
  //stop UI spinner      
}

void LiveScriptNode::execute() {
    ESP_LOGD(TAG, "%s", animation);

    //similar to the requestMapLayout in layout.setup.
    if (hasLayout) {
        layerV->mapLayout(); //which calls also livescript addLayout
    }

    if (hasLoop) {
        // setup : create controls
        // executable.execute("setup"); 
        // send controls to UI
        // executable.executeAsTask("main"); //background task (async - vs sync)
        ESP_LOGD(TAG, "%s executeAsTask main", animation);
        scriptRuntime.executeAsTask(animation, "main"); //background task (async - vs sync)
        //assert failed: xEventGroupSync event_groups.c:228 (uxBitsToWaitFor != 0)
    } else {
        ESP_LOGD(TAG, "%s execute main", animation);
        scriptRuntime.execute(animation, "main");
    }
    ESP_LOGD(TAG, "%s execute started", animation);
}

void LiveScriptNode::kill() {
    ESP_LOGD(TAG, "%s", animation);
    scriptRuntime.kill(animation);
}

void LiveScriptNode::free() {
    ESP_LOGD(TAG, "%s", animation);
    scriptRuntime.free(animation);
}

void LiveScriptNode::killAndDelete() {
    ESP_LOGD(TAG, "%s", animation);
    scriptRuntime.kill(animation);
    // scriptRuntime.free(animation);
    scriptRuntime.deleteExe(animation);
};

void LiveScriptNode::getScriptsJson(JsonArray scripts) {
    for (Executable &exec: scriptRuntime._scExecutables) {
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
        // ESP_LOGD(TAG, "scriptRuntime exec %s r:%d h:%d, e:%d h:%d b:%d + d:%d = %d", exec.name.c_str(), exec.isRunning(), exec.isHalted, exec.exeExist, exec.__run_handle_index, exeInfo.binary_size, exeInfo.data_size, exeInfo.total_size);
    }
}

#endif //FT_LIVESCRIPT

#endif //FT_MOONLIGHT