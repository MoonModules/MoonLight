/**
    @title     MoonBase
    @file      Module.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/develop/modules/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONBASE == 1

  #include "Module.h"

JsonDocument* gModulesDoc = nullptr;

void setDefaults(JsonObject controls, JsonArray definition) {
  for (JsonObject control : definition) {
    // if (control["type"] == "coord3Dxx") {
    //     EXT_LOGD(ML_TAG, "coord3D %d %d %d",  control["default"]["x"].as<int>(),  control["default"]["y"].as<int>(),  control["default"]["z"].as<int>());
    //     JsonObject object = controls[control["name"]].to<JsonObject>();
    //     controls[control["name"]]["x"] = control["default"]["x"];
    //     controls[control["name"]]["y"] = control["default"]["y"];
    //     controls[control["name"]]["z"] = control["default"]["z"];
    // } else
    if (control["type"] != "rows") {
      controls[control["name"]] = control["default"];
    } else {
      // JsonArray array = controls[control["name"]].to<JsonArray>();
      // //loop over detail controls (recursive)
      // JsonObject object = array.add<JsonObject>(); // add one row
      // setDefaults(object, control["n"].as<JsonArray>());
    }
  }
}

void ModuleState::setupData() {
  // only if no file ...
  if (data.size() == 0) {
    EXT_LOGV(MB_TAG, "size %d", data.size());
    JsonDocument definition;
    if (setupDefinition)
      setupDefinition(definition.to<JsonArray>());
    else
      EXT_LOGW(MB_TAG, "no definition");

    // create a new json for the defaults
    JsonDocument doc;
    setDefaults(doc.to<JsonObject>(), definition.as<JsonArray>());

    // assign the new defaults to state and run onUpdate
    data.clear();  //->to<JsonObject>(); //clear data
    UpdatedItem updatedItem;
    compareRecursive("", data, doc.as<JsonObject>(), updatedItem, "module");  // fill data with doc and calls onUpdates
  }

  // to do: check if the file matches the definition
}

void ModuleState::read(ModuleState& state, JsonObject& stateJson) {
  if (state.readHook) state.readHook(state.data);

  stateJson.set(state.data);  // copy
}

bool ModuleState::checkReOrderSwap(const JsonString& parent, const JsonVariant& stateData, const JsonVariant& newData, UpdatedItem& updatedItem, const String& originId, uint8_t depth, uint8_t index) {
  bool changed = false;
  // check if newData is a reordering of state
  // if so reorder state, no comparison with updates needed
  for (JsonPair newControl : newData.as<JsonObject>()) {
    if (newControl.value().is<JsonArray>()) {
      // bool reorderedRows = false;
      JsonArray newArray = newControl.value();
      JsonArray stateArray = stateData[newControl.key()];
      // check each row with each row of stateArray and check if same row found

      // only pure swaps, no add or remove of rows
      if (newArray.size() == stateArray.size()) {
        uint8_t parkedAtIndex;
        uint8_t parkedFromIndex = UINT8_MAX;

        // size_t minSize = MIN(stateArray.size(), newArray.size());
        for (uint8_t stateIndex = 0; stateIndex < stateArray.size(); stateIndex++) {  //} JsonObject stateObject : stateArray) {
          for (uint8_t newIndex = 0; newIndex < stateArray.size(); newIndex++) {      //} JsonObject newObject : newArray) {
            if (stateIndex != newIndex && stateArray[stateIndex] == newArray[newIndex]) {
              // if the old value found somewhere else, it has been moved so we can overwrite the current value
              stateArray[stateIndex].set(newArray[stateIndex]);  // copies the value to the state array
              EXT_LOGD(ML_TAG, "(%d @ %d) %d -> %d", parkedFromIndex, parkedAtIndex, stateIndex, newIndex);
              changed = true;

              // calculate swap indexes
              // eg 0 to 1 take 0 and store in 1. old 1 is parked in 0
              //.  1 to 2 take the 1 parked in 0 store old 2 in 0
              //.  2 to 0 take the 2 parked in 0 . it is already in 0 so we are done!!!

              // temp = newindex; newindex = oldindex; oldindex = temp (storage)!

              // for each map which maps to another row, call onReOrderSwap

              // check if stateindex is stored somewhere else
              // if stateIndex refers to an index which is parked, use the parking spot.

              uint8_t newStateIndex = stateIndex;
              if (stateIndex == parkedFromIndex)  // e.g. parkedFromIndex ==1
                newStateIndex = parkedAtIndex;    // e.g. 1 is stored in 0

              if (newStateIndex != newIndex) {
                UpdatedItem updatedItem;
                updatedItem.name = "swap";
                updatedItem.index[0] = stateIndex;
                updatedItem.index[1] = newIndex;
                processUpdatedItem(updatedItem, originId);
              }

              if (parkedFromIndex == UINT8_MAX) parkedFromIndex = newIndex;  // the index of value in the array stored in the parking spot
              parkedAtIndex = newStateIndex;                                 // the parking spot created
            }
          }
        }
      }  // equal size
    }
  }
  return changed;
}

bool ModuleState::compareRecursive(const JsonString& parent, const JsonVariant& stateData, const JsonVariant& newData, UpdatedItem& updatedItem, const String& originId, uint8_t depth, uint8_t index) {
  bool changed = false;
  for (JsonPair newControl : newData.as<JsonObject>()) {
    if (stateData[newControl.key()].isNull()) {
      stateData[newControl.key()] = nullptr;  // Initialize the key in stateData if it doesn't exist todo: run in loopTask ?
    }
  }

  // loop over all properties in stateData
  bool identifyingFieldFound = false;
  for (JsonPair stateControl : stateData.as<JsonObject>()) {
    JsonString key = stateControl.key();
    JsonVariant stateValue = stateData[key.c_str()];
    JsonVariant newValue = newData[key.c_str()];
    if (!newValue.isNull() && stateValue != newValue) {  // if value changed, don't update if not defined in newValue

      if (depth != UINT8_MAX && depth < 2) {  // depth starts with '-1' (no depth)
        updatedItem.parent[depth] = parent;
        updatedItem.index[depth] = index;
      }
      for (uint8_t i = depth + 1; i < 2; i++) {  // reset deeper levels when coming back from recursion
        updatedItem.parent[i] = "";
        updatedItem.index[i] = UINT8_MAX;
      }

      if (stateValue.is<JsonArray>() || newValue.is<JsonArray>()) {  // if the control is an array
        if (!identifyingFieldFound) {
          if (stateValue.isNull()) {
            stateData[key.c_str()].to<JsonArray>();
            stateValue = stateData[key.c_str()];
          }  // if old value is null, set to empty array
          JsonArray stateArray = stateValue.as<JsonArray>();
          JsonArray newArray = newValue.as<JsonArray>();

          // EXT_LOGD(MB_TAG, "compare %s[%d] %s = %s -> %s", parent.c_str(), index, key.c_str(), stateValue.as<const char*>(), newValue.as<const char*>());

          for (int i = 0; i < MAX(stateArray.size(), newArray.size()); i++) {  // compare each item in the array
            // EXT_LOGD(MB_TAG, "compare %s[%d] %s = %s -> %s", parent.c_str(), index, key.c_str(), stateArray[i].as<const char*>(), newArray[i].as<const char*>());
            if (i >= stateArray.size()) {  // newArray has added a row
              // EXT_LOGD(MB_TAG, "add %s.%s[%d] (%d/%d) d: %d", parent.c_str(), key.c_str(), i, stateArray.size(), newArray.size(), depth);
              stateArray.add<JsonObject>();  // add new row
              changed = compareRecursive(key, stateArray[i], newArray[i], updatedItem, originId, depth + 1, i) || changed;
            } else if (i >= newArray.size()) {  // newArray has deleted a row
              changed = true;                   // compareRecursive(key, stateArray[i], newArray[i], updatedItem, depth+1, i) || changed;

              // EXT_LOGD(MB_TAG, "remove %s.%s[%d] (%d/%d) d: %d", parent.c_str(), key.c_str(), i, stateArray.size(), newArray.size(), depth);

              updatedItem.parent[1] = "";  // reset deeper levels when coming back from recursion (repeat in loop)
              updatedItem.index[1] = UINT8_MAX;
              if ((uint8_t)(depth + 1) < 2) {
                updatedItem.parent[(uint8_t)(depth + 1)] = key;
                updatedItem.index[(uint8_t)(depth + 1)] = i;
              }
              // Node controls need not to be removed if they just have been added by Node::addControl via Nodemanager.h - which sets a valid status to a control
              // Don't remove array items marked as valid (e.g., controls added in setup() that don't exist in persisted state yet)
              bool isValid = !stateArray[i]["valid"].isNull() && stateArray[i]["valid"].as<bool>();
              if (!isValid) {
                for (JsonPair control : stateArray[i].as<JsonObject>()) {
                  updatedItem.name = control.key();
                  updatedItem.oldValue = control.value();
                  updatedItem.value = JsonVariant();    // Assign an empty JsonVariant
                  stateArray[i].remove(control.key());  // remove the control from the state row so onUpdate see it as empty

                  processUpdatedItem(updatedItem, originId);
                }

                stateArray.remove(i);  // remove the state row entirely
              } else {
                EXT_LOGD(MB_TAG, "skip remove %s.%s[%d] d: %d", parent.c_str(), key.c_str(), i, depth);
              }
            } else {  // row already exists
              changed = compareRecursive(key, stateArray[i], newArray[i], updatedItem, originId, depth + 1, i) || changed;
            }
          }  // loop over array
        }  // identifyingFieldFound
      } else {             // if control is key/value
        if (key != "p") {  // do not process pointers
          changed = true;
          updatedItem.name = key;
          updatedItem.oldValue = stateValue;
          stateData[key.c_str()] = newValue;           // update the value in stateData, should not be done in runLoopTask as FS update then misses the change!!
          updatedItem.value = stateData[key.c_str()];  // store the stateData item (convenience)

          processUpdatedItem(updatedItem, originId);

          // If both sides are objects and their identifying property "name" changed,
          // emit only that "name" update for this object and DO NOT recurse into it.
          // This prevents spurious children (e.g., controls.*) updates during a rename.
          if (key == "name" && originId.toInt()) { // only when done from UI, Not when system boots and FS loads
            // EXT_LOGD(MB_TAG, "identifyingFieldFound %s.%s", parent.c_str(), key.c_str());
            identifyingFieldFound = true;
          }
        }
      }
    }  // if value changed
  }  // for (JsonPair stateControl
  return changed;
}

StateUpdateResult ModuleState::update(JsonObject& newData, ModuleState& state, const String& originId) {
  // if (state.data.isNull()) EXT_LOGD(ML_TAG, "state data is null %d %d", newData.size(), newData != state.data); // state.data never null here

  if (newData.size() != 0) {  // in case of empty file

    // check which controls have updated
    if (newData != state.data) {
      UpdatedItem updatedItem;

      // bool isNew = state.data.isNull();  // device is starting , not useful as state.data never null here

      bool changed = state.checkReOrderSwap("", state.data, newData, updatedItem, originId);

      // if (originId != "devicesserver" && originId != "tasksserver") {
      //   String ss;
      //   serializeJson(newData, ss);
      //   EXT_LOGD(ML_TAG, "newData %s from %s", ss.c_str(), originId.c_str());
      // }

      // EXT_LOGD(ML_TAG, "update isNew %d changed %d", isNew, changed);
      // serializeJson(state.data, Serial);Serial.println();
      // serializeJson(newData, Serial);Serial.println();

      if (state.compareRecursive("", state.data, newData, updatedItem, originId)) {
        if (changed) EXT_LOGW(ML_TAG, "checkReOrderSwap changed, compareRecursive also changed? %s", originId.c_str());
        changed = true;
      }

      return (changed) ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;  //! isNew &&
    } else
      return StateUpdateResult::UNCHANGED;
  } else {
    EXT_LOGW(MB_TAG, "empty newData %s", originId.c_str());
    return StateUpdateResult::UNCHANGED;
  }
}

Module::Module(const char* moduleName, PsychicHttpServer* server, ESP32SvelteKit* sveltekit)
    : _socket(sveltekit->getSocket()),
      _fsPersistence(ModuleState::read, ModuleState::update, this, sveltekit->getFS(), (String("/.config/") + moduleName + ".json").c_str(), true)  // 🌙 true: delayedWrites
{
  _moduleName = (moduleName && moduleName[0] != '\0') ? moduleName : "unnamed";

  EXT_LOGV(MB_TAG, "constructor %s", moduleName);
  _server = server;

  _state.processUpdatedItem = [&](const UpdatedItem& updatedItem, const String& originId) {
    processUpdatedItem(updatedItem, originId);  // Ensure updatedItem is of type UpdatedItem&
  };
}

void Module::begin() {
  EXT_LOGV(MB_TAG, "");

  _fsPersistence.readFromFS();  // overwrites the default settings in state

  // no virtual functions in constructor so this is in begin()
  _state.setupDefinition = [&](const JsonArray& controls) {
    this->setupDefinition(controls);  // using here means setupDefinition must be virtual ...
  };
  // _state.setupDefinition = this->setupDefinition;
  _state.setupData();  // if no data readFromFS, using overridden virtual function setupDefinition

  _server->on((String("/rest/") + _moduleName + "Def").c_str(), HTTP_GET, [&](PsychicRequest* request) {
    PsychicJsonResponse response = PsychicJsonResponse(request, false);
    JsonArray controls = response.getRoot().to<JsonArray>();

    setupDefinition(controls);  // virtual function

    // char buffer[2048];
    // serializeJson(controls, buffer, sizeof(buffer));
    // EXT_LOGD(MB_TAG, "server->on %s moduleDef %s", request->url().c_str(), buffer);

    return response.send();
  });
}

void Module::setupDefinition(const JsonArray& controls) {  // virtual so it can be overriden in derived classes
  EXT_LOGW(MB_TAG, "not implemented");
  JsonObject control;  // state.data has one or more properties
  JsonArray rows;      // if a control is an array, this is the rows of the array

  control = addControl(controls, "text", "text");
  control["default"] = "MoonLight";
}

JsonObject Module::addControl(const JsonArray& controls, const char* name, const char* type, int min, int max, bool ro, const char* desc) {
  JsonObject control = controls.add<JsonObject>();
  control["name"] = name;
  control["type"] = type;
  if (min != 0) control["min"] = min;
  if (max != UINT8_MAX) control["max"] = max;
  if (ro) control["ro"] = true;  // else if (!control["ro"].isNull()) control.remove("ro");
  if (desc) control["desc"] = desc;
  return control;
}

#endif