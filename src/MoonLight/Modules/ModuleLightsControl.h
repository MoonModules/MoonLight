/**
    @title     MoonLight
    @file      ModuleLightsControl.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/lightscontrol/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleLightsControl_h
#define ModuleLightsControl_h

#if FT_MOONLIGHT

  #include "FastLED.h"
  #include "ModuleEffects.h"
  #include "MoonBase/Module.h"
  #include "MoonBase/Modules/FileManager.h"
  #include "MoonBase/Utilities.h"  //for isInPSRAM

class ModuleLightsControl : public Module {
 public:
  PsychicHttpServer* _server;
  FileManager* _fileManager;
  ModuleIO* _moduleIO;
  uint8_t pinRelayLightsOn = UINT8_MAX;
  uint8_t pinPushButtonLightsOn = UINT8_MAX;
  uint8_t pinToggleButtonLightsOn = UINT8_MAX;

  ModuleLightsControl(PsychicHttpServer* server, ESP32SvelteKit* sveltekit, FileManager* fileManager, ModuleIO* moduleIO) : Module("lightscontrol", server, sveltekit) {
    EXT_LOGV(ML_TAG, "constructor");
    _server = server;
    _fileManager = fileManager;
    _moduleIO = moduleIO;
  }

  void begin() override {
    Module::begin();

    EXT_LOGI(ML_TAG, "Lights:%d(Header:%d) L-H:%d Node:%d PL:%d(PL-L:%d) VL:%d PM:%d C3D:%d", sizeof(Lights), sizeof(LightsHeader), sizeof(Lights) - sizeof(LightsHeader), sizeof(Node), sizeof(PhysicalLayer), sizeof(PhysicalLayer) - sizeof(Lights), sizeof(VirtualLayer), sizeof(PhysMap), sizeof(Coord3D));

    EXT_LOGI(ML_TAG, "isInPSRAM: mt:%d mti:%d ch:%d", isInPSRAM(layerP.layers[0]->mappingTable), isInPSRAM(layerP.layers[0]->mappingTableIndexes.data()), isInPSRAM(layerP.lights.channelsE));

    setPresetsFromFolder();  // set the right values during boot

    // update presets if files changed in presets folder
    _fileManager->addUpdateHandler([this](const String& originId) {
      EXT_LOGV(ML_TAG, "FileManager::updateHandler %s", originId.c_str());
      // read the file state (read all files and folders on FS and collect changes)
      _fileManager->read([&](FilesState& filesState) {
        // loop over all changed files (normally only one)
        bool presetChanged = false;
        for (auto updatedItem : filesState.updatedItems) {
          // if file is the current live script, recompile it (to do: multiple live effects)
          EXT_LOGV(ML_TAG, "updateHandler updatedItem %s", updatedItem.c_str());
          if (strstr(updatedItem.c_str(), "/.config/presets")) {
            EXT_LOGV(ML_TAG, " preset.json updated -> call update %s", updatedItem.c_str());
            presetChanged = true;
          }
        }
        if (presetChanged) {
          EXT_LOGV(ML_TAG, "setPresetsFromFolder");
          setPresetsFromFolder();  // update the presets from the folder
        }
      });
    });
    moduleIO.addUpdateHandler([this](const String& originId) { readPins(); }, false);
    readPins();  // initially
  }

  void readPins() {
    moduleIO.read([&](ModuleState& state) {
      pinRelayLightsOn = UINT8_MAX;
      pinPushButtonLightsOn = UINT8_MAX;
      pinToggleButtonLightsOn = UINT8_MAX;
      for (JsonObject pinObject : state.data["pins"].as<JsonArray>()) {
        uint8_t usage = pinObject["usage"];
        uint8_t gpio = pinObject["GPIO"];

        if (usage == pin_Relay_LightsOn) {
          if (GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
            pinRelayLightsOn = gpio;
            pinMode(pinRelayLightsOn, OUTPUT);
            uint8_t newBri = _state.data["lightsOn"] ? _state.data["brightness"] : 0;
            digitalWrite(pinRelayLightsOn, newBri > 0 ? HIGH : LOW);
            EXT_LOGD(ML_TAG, "pinRelayLightsOn found %d", pinRelayLightsOn);
          } else
            EXT_LOGE(MB_TAG, "gpio %d not valid", pinRelayLightsOn);
        } else if (usage == pin_Button_Push_LightsOn) {
          if (GPIO_IS_VALID_GPIO(gpio)) {
            pinPushButtonLightsOn = gpio;
            pinMode(pinPushButtonLightsOn, INPUT_PULLUP);
            EXT_LOGD(ML_TAG, "pinPushButtonLightsOn found %d", pinPushButtonLightsOn);
          } else
            EXT_LOGE(MB_TAG, "gpio %d not valid", pinPushButtonLightsOn);
        } else if (usage == pin_Button_Toggle_LightsOn) {
          if (GPIO_IS_VALID_GPIO(gpio)) {
            pinToggleButtonLightsOn = gpio;
            pinMode(pinToggleButtonLightsOn, INPUT_PULLUP);
            EXT_LOGD(ML_TAG, "pinToggleButtonLightsOn found %d", pinToggleButtonLightsOn);
          } else
            EXT_LOGE(MB_TAG, "gpio %d not valid", pinToggleButtonLightsOn);
        }
      }
      // for (int i = 0; i < sizeof(pins); i++) EXT_LOGD(ML_TAG, "pin %d = %d", i, pins[i]);
    });
  }

  // define the data model
  void setupDefinition(const JsonArray& controls) override {
    EXT_LOGV(ML_TAG, "");
    JsonObject control;  // state.data has one or more properties

    control = addControl(controls, "lightsOn", "checkbox");
    control["default"] = true;
    control = addControl(controls, "brightness", "slider");
    control["default"] = 20;
    control = addControl(controls, "red", "slider");
    control["default"] = 255;
    control["color"] = "Red";
    control = addControl(controls, "green", "slider");
    control["default"] = 255;
    control["color"] = "Green";
    control = addControl(controls, "blue", "slider");
    control["default"] = 255;
    control["color"] = "Blue";
    control = addControl(controls, "palette", "select");
    control["default"] = 9;
    addControlValue(control, "Cloud");
    addControlValue(control, "Lava");
    addControlValue(control, "Ocean");
    addControlValue(control, "Forest");
    addControlValue(control, "Rainbow");
    addControlValue(control, "RainbowStripe");
    addControlValue(control, "Party");
    addControlValue(control, "Heat");
    addControlValue(control, "Random");
    addControlValue(control, "MoonModules");
    addControlValue(control, "Orange");

    control = addControl(controls, "preset", "pad");
    control["width"] = 8;
    control["size"] = 18;
    control["default"].to<JsonObject>();  // clear the preset array before adding new presets
    control["default"]["list"].to<JsonArray>();
    control["default"]["count"] = 64;

    control = addControl(controls, "presetLoop", "slider");
    control["default"] = 0;
    control = addControl(controls, "firstPreset", "slider", 1, 64);
    control["default"] = 1;
    control = addControl(controls, "lastPreset", "slider", 1, 64);
    control["default"] = 64;

  #if FT_ENABLED(FT_MONITOR)
    control = addControl(controls, "monitorOn", "checkbox");
    control["default"] = true;
  #endif
  }

  // implement business logic
  void onUpdate(const UpdatedItem& updatedItem) override {
    // EXT_LOGD(ML_TAG, "handle %s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
    if (updatedItem.name == "red") {
      layerP.lights.header.red = _state.data["red"];
    } else if (updatedItem.name == "green") {
      layerP.lights.header.green = _state.data["green"];
    } else if (updatedItem.name == "blue") {
      layerP.lights.header.blue = _state.data["blue"];
    } else if (updatedItem.name == "lightsOn" || updatedItem.name == "brightness") {
      uint8_t newBri = _state.data["lightsOn"] ? _state.data["brightness"] : 0;
      if (!!layerP.lights.header.brightness != !!newBri && pinRelayLightsOn != UINT8_MAX) {
        EXT_LOGD(ML_TAG, "pinRelayLightsOn %s", !!newBri ? "On" : "Off");
        digitalWrite(pinRelayLightsOn, newBri > 0 ? HIGH : LOW);
      };
      layerP.lights.header.brightness = newBri;
    } else if (updatedItem.name == "palette") {
      const size_t nrOfPaletteEntries = sizeof(layerP.palette.entries) / sizeof(CRGB);

      if (updatedItem.value == 0)
        layerP.palette = CloudColors_p;
      else if (updatedItem.value == 1)
        layerP.palette = LavaColors_p;
      else if (updatedItem.value == 2)
        layerP.palette = OceanColors_p;
      else if (updatedItem.value == 3)
        layerP.palette = ForestColors_p;
      else if (updatedItem.value == 4)
        layerP.palette = RainbowColors_p;
      else if (updatedItem.value == 5)
        layerP.palette = RainbowStripeColors_p;
      else if (updatedItem.value == 6)
        layerP.palette = PartyColors_p;
      else if (updatedItem.value == 7)
        layerP.palette = HeatColors_p;
      else if (updatedItem.value == 8) {
        for (int i = 0; i < sizeof(layerP.palette.entries) / sizeof(CRGB); i++) {
          layerP.palette[i] = CHSV(random8(), 255, 255);  // take the max saturation, max brightness of the colorwheel
        }
      } else if (updatedItem.value == 9) {  // MoonModules palette
        for (int i = 0; i < nrOfPaletteEntries; i++) {
          layerP.palette[i] = CRGB(map(i, 0, nrOfPaletteEntries - 1, 255, 0), map(i, 0, nrOfPaletteEntries - 1, 31, 0), map(i, 0, nrOfPaletteEntries - 1, 0, 255));  // from orange to blue
        }
      } else if (updatedItem.value == 10) {  // Orange palette
        for (int i = 0; i < nrOfPaletteEntries; i++) {
          layerP.palette[i] = CRGB(255, map(i, 0, nrOfPaletteEntries - 1, 0, 255), 0);  // from red via orange to yellow
        }
      } else {
        layerP.palette = PartyColors_p;  // should never occur
      }
      // layerP.palette = LavaColors_p;
    } else if (updatedItem.name == "preset") {
      // copy /.config/effects.json to the hidden folder /.config/presets/preset[x].json
      // do not set preset at boot...
      if (updatedItem.oldValue != "" && !updatedItem.value["action"].isNull()) {
        uint16_t select = updatedItem.value["select"];
        Char<32> presetFile;
        presetFile.format("/.config/presets/preset%02d.json", select);

        if (updatedItem.value["action"] == "click") {
          updatedItem.value["selected"] = select;  // store the selected preset
          if (arrayContainsValue(updatedItem.value["list"], select)) {
            copyFile(presetFile.c_str(), "/.config/effects.json");

            // trigger notification of update of effects.json
            _fileManager->update(
                [&](FilesState& state) {
                  state.updatedItems.clear();
                  state.updatedItems.push_back("/.config/effects.json");
                  return StateUpdateResult::CHANGED;  // notify StatefulService by returning CHANGED
                },
                _moduleName);

          } else {
            copyFile("/.config/effects.json", presetFile.c_str());
            setPresetsFromFolder();  // update presets in UI
          }
        } else if (updatedItem.value["action"] == "dblclick") {
          ESPFS.remove(presetFile.c_str());
          setPresetsFromFolder();  // update presets in UI
        }
      }
    }
  }

  // update _state.data["preset"]["list"] and send update to endpoints
  void setPresetsFromFolder() {
    // loop over all files in the presets folder and add them to the preset array
    File rootFolder = ESPFS.open("/.config/presets/");
    _state.data["preset"]["list"].clear();  //.to<JsonArray>(); // clear the active preset array before adding new presets
    bool changed = false;
    walkThroughFiles(rootFolder, [&](File folder, File file) {
      int seq = -1;
      if (sscanf(file.name(), "preset%02d.json", &seq) == 1) {
        // seq now contains the 2-digit number, e.g., 34
        EXT_LOGV(ML_TAG, "Preset %d found", seq);
        _state.data["preset"]["list"].add(seq);  // add the preset to the preset array
        changed = true;
      }
    });

    if (changed) {
      // requestUIUpdate ...
      update(
          [&](ModuleState& state) {
            return StateUpdateResult::CHANGED;  // notify StatefulService by returning CHANGED
          },
          _moduleName);
    }
  }

  unsigned long lastPresetTime = 0;
  // see pinPushButtonLightsOn
  unsigned long lastDebounceTime = 0;
  static constexpr unsigned long debounceDelay = 50;  // 50ms debounce
  int lastState = HIGH;

  void loop() override {
    Module::loop();
    // process presetLoop
    uint8_t presetLoop = _state.data["presetLoop"];
    if (presetLoop && millis() - lastPresetTime > presetLoop * 1000) {  // every presetLoop seconds
      lastPresetTime = millis();

      // bugfix;
      //  runInAppTask.push_back([&]() {
      //  load the xth preset from FS
      JsonArray presetList = _state.data["preset"]["list"];

      if (_state.data["firstPreset"] <= _state.data["lastPreset"]) {
        uint8_t nextPreset = 0;

        nextPreset = getNextItemInArray(presetList, _state.data["preset"]["selected"]);
        // EXT_LOGD(ML_TAG, "loading next preset %d ", nextPreset);
        while (nextPreset < _state.data["firstPreset"] || nextPreset > _state.data["lastPreset"]) {
          nextPreset = getNextItemInArray(presetList, nextPreset);
        }

        // EXT_LOGD(ML_TAG, "loading next preset %d ", nextPreset);

        // trigger file manager notification of update of effects.json
        _fileManager->update(
            [&](FilesState& state) {
              state.updatedItems.push_back("/.config/effects.json");
              // EXT_LOGD(ML_TAG, "   preset files %d %s", lastPresetLooped, presetFile.c_str());
              return StateUpdateResult::CHANGED;  // notify StatefulService by returning CHANGED
            },
            _moduleName);

        JsonDocument doc;
        JsonObject newState = doc.to<JsonObject>();
        newState["preset"] = _state.data["preset"];
        newState["preset"]["action"] = "click";
        newState["preset"]["select"] = nextPreset;

        // update the state and ModuleState::update processes the changes behind the scenes
        if (newState.size()) {
          // serializeJson(doc, Serial);
          // Serial.println();
          update(newState, ModuleState::update, _moduleName + "server");
        }
      }
    }

    if (pinPushButtonLightsOn != UINT8_MAX) {
      int state = digitalRead(pinPushButtonLightsOn);
      if ((state != lastState) && ((((millis() - lastDebounceTime) > debounceDelay) || (millis() < lastDebounceTime)))) {
        lastDebounceTime = millis();
        // Trigger only on button press (HIGH to LOW transition for INPUT_PULLUP)
        if (state == LOW) {
          JsonDocument doc;
          JsonObject newState = doc.to<JsonObject>();
          newState["lightsOn"] = !_state.data["lightsOn"];
          update(newState, ModuleState::update, _moduleName + "server");
        }
        lastState = state;
      }
    }

    if (pinToggleButtonLightsOn != UINT8_MAX) {
      int state = digitalRead(pinToggleButtonLightsOn);
      if ((state != lastState) && ((((millis() - lastDebounceTime) > debounceDelay) || (millis() < lastDebounceTime)))) {
        lastDebounceTime = millis();
        JsonDocument doc;
        JsonObject newState = doc.to<JsonObject>();
        newState["lightsOn"] = !_state.data["lightsOn"];
        update(newState, ModuleState::update, _moduleName + "server");
        lastState = state;
      }
    }

  #if FT_ENABLED(FT_MONITOR)
    if (layerP.lights.header.isPositions == 2) {  // send to UI
      read([&](ModuleState& _state) {
        if (_socket->getConnectedClients() && _state.data["monitorOn"]) {
          _socket->emitEvent("monitor", (char*)&layerP.lights.header, 37);                                                                    // sizeof(LightsHeader)); //sizeof(LightsHeader), nearest prime nr above 32 to avoid monitor data to be seen as header
          _socket->emitEvent("monitor", (char*)layerP.lights.channelsE, MIN(layerP.lights.header.nrOfLights * 3, layerP.lights.maxChannels));  //*3 is for 3 bytes position
        }
        memset(layerP.lights.channelsE, 0, layerP.lights.maxChannels);  // set all the channels to 0 //cleaning the positions
        EXT_LOGD(ML_TAG, "positions sent to monitor (2 -> 3, #L:%d maxC:%d)", layerP.lights.header.nrOfLights, layerP.lights.maxChannels);
        layerP.lights.header.isPositions = 3;
      });
    } else if (layerP.lights.header.isPositions == 0 && layerP.lights.header.nrOfLights) {  // send to UI
      EVERY_N_MILLIS(layerP.lights.header.nrOfLights / 12) {
        
        read([&](ModuleState& _state) {
          if (_socket->getConnectedClients() && _state.data["monitorOn"]) {

            //protect emit by swapMutex, see main.cpp
            extern SemaphoreHandle_t swapMutex;
            
            xSemaphoreTake(swapMutex, portMAX_DELAY);
            _socket->emitEvent("monitor", (char*)layerP.lights.channelsE, MIN(layerP.lights.header.nrOfChannels, layerP.lights.maxChannels));
            xSemaphoreGive(swapMutex);
          }
        });

      }
    }
  #endif
  }

};  // class ModuleLightsControl

#endif
#endif