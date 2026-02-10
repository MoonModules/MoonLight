/**
    @title     MoonBase
    @file      Module.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/develop/modules/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef Module_h
#define Module_h

#if FT_MOONBASE == 1

  #include <ESP32SvelteKit.h>
  #include <FSPersistence.h>
  #include <PsychicHttp.h>

  #include "Utilities.h"

// sizeof was 160 chars -> 80 -> 68 -> 88
struct UpdatedItem {
  Char<20> parent[2];  // 24 -> 2*20
  uint8_t index[2];    // 2*1
  Char<20> name;       // 16 -> 16 -> 20
  Char<20> oldValue;   // 32 -> 16 -> 20, smaller then 11 bytes mostly
  JsonVariant value;   // 8->16->4

  UpdatedItem() {
    parent[0] = "";  // will be checked in onUpdate
    parent[1] = "";
    index[0] = UINT8_MAX;
    index[1] = UINT8_MAX;
  }
};

typedef std::function<void(JsonObject data)> ReadHook;

extern JsonDocument* gModulesDoc;  // shared document for all modules, to save RAM

class ModuleState {
 public:
  JsonObject data = JsonObject();  // isNull()

  ModuleState() {
    EXT_LOGD(MB_TAG, "ModuleState constructor");

    if (!gModulesDoc) {
      EXT_LOGD(MB_TAG, "Creating doc");
      if (psramFound())
        gModulesDoc = new JsonDocument(JsonRAMAllocator::instance());  // crashed on non psram esp32-d0
      else
        gModulesDoc = new JsonDocument();
    }
    if (gModulesDoc) {
      // doc = new JsonDocument();
      data = gModulesDoc->add<JsonObject>();
      // data = doc->to<JsonObject>();
    } else {
      EXT_LOGE(MB_TAG, "Failed to create doc");
    }
  }

  ~ModuleState() {
    EXT_LOGD(MB_TAG, "ModuleState destructor");

    // delete data from doc
    if (gModulesDoc) {
      JsonArray arr = gModulesDoc->as<JsonArray>();
      for (size_t i = 0; i < arr.size(); i++) {
        JsonObject obj = arr[i];
        if (obj == data) {  // same object (identity check)
          EXT_LOGD(MB_TAG, "Deleting data from doc");
          arr.remove(i);
          break;  // optional, if only one match
        }
      }
    }
  }

  std::function<void(const JsonArray& controls)> setupDefinition = nullptr;

  void setupData();

  // called from ModuleState::update and ModuleState::setupData()
  bool compareRecursive(const JsonString& parent, const JsonVariant& oldData, const JsonVariant& newData, UpdatedItem& updatedItem, const String& originId, uint8_t depth = UINT8_MAX, uint8_t index = UINT8_MAX);

  // called from ModuleState::update
  bool checkReOrderSwap(const JsonString& parent, const JsonVariant& oldData, const JsonVariant& newData, UpdatedItem& updatedItem, const String& originId, uint8_t depth = UINT8_MAX, uint8_t index = UINT8_MAX);

  std::function<void(const UpdatedItem&, const String&)> processUpdatedItem = nullptr;

  static void read(ModuleState& state, JsonObject& stateJson);
  static StateUpdateResult update(JsonObject& newData, ModuleState& state, const String& originId);  //, const String& originId

  ReadHook readHook = nullptr;  // called when the UI requests the state, can be used to update the state before sending it to the UI
};

class Module : public StatefulService<ModuleState> {
 public:
  const char* _moduleName = "";
  bool requestUIUpdate = false;

  Module(const char* moduleName, PsychicHttpServer* server, ESP32SvelteKit* sveltekit);

  // any Module that overrides begin() must continue to call Module::begin() (e.g., at the start of its own begin()
  virtual void begin();

  // any Module that overrides loop() must continue to call Module::loop() (e.g., at the start of its own loop()
  virtual void loop() {
    // run in sveltekit task

    if (requestUIUpdate) {
      requestUIUpdate = false;  // reset the flag
      EXT_LOGD(ML_TAG, "requestUIUpdate");

      // update state to UI
      update(
          [&](ModuleState& state) {
            return StateUpdateResult::CHANGED;  // notify StatefulService by returning CHANGED
          },
          _moduleName);
    }
  }

  void processUpdatedItem(const UpdatedItem& updatedItem, const String& originId) {
    if (updatedItem.name == "swap") {
      onReOrderSwap(updatedItem.index[0], updatedItem.index[1]);
      if (originId.toInt())
        saveNeeded = true;
    } else {
      // if (updatedItem.parent[0] != "devices" && updatedItem.parent[0] != "tasks" && updatedItem.name != "core0") EXT_LOGD(ML_TAG, "%s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
      if (updatedItem.name != "channel") {  // todo: fix the problem at channel, not here...
        if (originId.toInt()) {             // Front-end client IDs are numeric; internal origins ("module", etc.) return 0
          saveNeeded = true;
        }
      }
      onUpdate(updatedItem, originId);
    }
  }

  virtual void setupDefinition(const JsonArray& controls);

  JsonObject addControl(const JsonArray& controls, const char* name, const char* type, int min = 0, int max = UINT8_MAX, bool ro = false, const char* desc = nullptr);
  template <typename T>
  void addControlValue(const JsonObject& control, const T& value) {
    if (control["values"].isNull()) control["values"].to<JsonArray>();  // add array of values
    JsonArray values = control["values"];
    values.add(value);
  }

  // called in compareRecursive->execOnUpdate
  // called from compareRecursive
  virtual void onUpdate(const UpdatedItem& updatedItem, const String& originId) {};
  virtual void onReOrderSwap(uint8_t stateIndex, uint8_t newIndex) {};

 protected:
  EventSocket* _socket;
  void readFromFS() {             // used in ModuleEffects, for live scripts...
    _fsPersistence.readFromFS();  // overwrites the default settings in state
  }

 private:
  FSPersistence<ModuleState> _fsPersistence;
  PsychicHttpServer* _server;
};

#endif
#endif