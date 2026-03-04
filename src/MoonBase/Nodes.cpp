/**
    @title     MoonLight
    @file      Nodes.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT
  #include "Nodes.h"

SharedData sharedData;

JsonObject Node::findOrCreateControl(const char* name, bool& newControl) {
  JsonObject control = JsonObject();
  for (JsonObject control1 : controls) {
    if (control1["name"] == name) {
      control = control1;
      break;
    }
  }
  if (control.isNull()) {
    control = controls.add<JsonObject>();
    control["name"] = name;
    newControl = true;
  }
  return control;
}

JsonObject Node::setupControl(const char* name, const char* type, int min, int max, bool ro, const char* desc, uint8_t sizeCode, size_t sizeofVar, bool newControl, JsonObject control) {
  control["type"] = type;
  control["valid"] = true;
  // optional properties
  if (ro)
    control["ro"] = true;
  else if (!control["ro"].isNull())
    control.remove("ro");
  if (min != 0)
    control["min"] = min;
  else if (!control["min"].isNull())
    control.remove("min");
  if (max != UINT8_MAX)
    control["max"] = max;
  else if (!control["max"].isNull())
    control.remove("max");
  if (desc)
    control["desc"] = desc;
  else if (!control["desc"].isNull())
    control.remove("desc");

  // set size based on control type and sizeCode
  if (control["type"] == "slider" || control["type"] == "select" || control["type"] == "pin" || control["type"] == "number") {
    if (sizeCode == 8 || sizeCode == 108 || sizeCode == 16 || sizeCode == 32 || sizeCode == 33 || sizeCode == 34)
      control["size"] = sizeCode;
    else
      EXT_LOGE(ML_TAG, "size %d mismatch for %s", sizeCode, name);
  } else if (control["type"] == "selectFile" || control["type"] == "text") {
    control["size"] = sizeofVar;
  } else if (control["type"] == "checkbox") {
    if (sizeCode != sizeof(bool))
      EXT_LOGE(ML_TAG, "type for %s is not bool", name);
    else
      control["size"] = sizeof(bool);
  } else if (control["type"] == "coord3D") {
    if (sizeCode != sizeof(Coord3D))
      EXT_LOGE(ML_TAG, "type for %s is not Coord3D", name);
    else
      control["size"] = sizeof(Coord3D);
  } else
    EXT_LOGE(ML_TAG, "type of %s not compatible: %s (%d)", control["name"].as<const char*>(), control["type"].as<const char*>(), control["size"].as<uint8_t>());

  if (newControl) {
    Char<20> oldValue = "";
    onUpdate(oldValue, control);  // custom onUpdate for the node
  }

  return control;
}

void Node::updateControl(const JsonObject& control) {
  // EXT_LOGD(ML_TAG, "onUpdate %s", control["name"].as<const char*>());
  // if (oldValue == "") return;                                                              // newControl, value already set
  if (!control["name"].isNull() && !control["type"].isNull() && !control["p"].isNull()) {  // name and type can be null if control is removed in compareRecursive
    int pointer = control["p"];
    // EXT_LOGD(ML_TAG, "%s = %s t:%s p:%p", control["name"].as<const char*>(), control["value"].as<String>().c_str(), control["type"].as<const char*>(), (void*)pointer);

    if (pointer) {
      if (control["type"] == "slider" || control["type"] == "select" || control["type"] == "pin" || control["type"] == "number") {
        if (control["size"] == 8) {
          uint8_t* valuePointer = (uint8_t*)pointer;
          *valuePointer = control["value"];
          // EXT_LOGV(ML_TAG, "%s = %d", control["name"].as<const char*>(), *valuePointer);
        } else if (control["size"] == 108) {
          int8_t* valuePointer = (int8_t*)pointer;
          *valuePointer = control["value"];
          // EXT_LOGV(ML_TAG, "%s = %d", control["name"].as<const char*>(), *valuePointer);
        } else if (control["size"] == 16) {
          uint16_t* valuePointer = (uint16_t*)pointer;
          *valuePointer = control["value"];
          // EXT_LOGV(ML_TAG, "%s = %d", control["name"].as<const char*>(), *valuePointer);
        } else if (control["size"] == 32) {
          uint32_t* valuePointer = (uint32_t*)pointer;
          *valuePointer = control["value"];
          // EXT_LOGV(ML_TAG, "%s = %d", control["name"].as<const char*>(), *valuePointer);
        } else if (control["size"] == 33) {
          int* valuePointer = (int*)pointer;
          *valuePointer = control["value"];
          // EXT_LOGV(ML_TAG, "%s = %d", control["name"].as<const char*>(), *valuePointer);
        } else if (control["size"] == 34) {
          float* valuePointer = (float*)pointer;
          *valuePointer = control["value"];
          // EXT_LOGV(ML_TAG, "%s = %d", control["name"].as<const char*>(), *valuePointer);
        } else {
          EXT_LOGW(ML_TAG, "size not supported or not set for %s: %d", control["name"].as<const char*>(), control["size"].as<int>());
        }
      } else if (control["type"] == "selectFile" || control["type"] == "text") {
        char* valuePointer = (char*)pointer;
        size_t bufSize = control["size"].isNull() ? 32 : control["size"].as<size_t>();
        if (!control["max"].isNull()) {
          // `max` is content length; reserve one byte for '\0'
          bufSize = MIN(bufSize, control["max"].as<size_t>() + 1);
        }
        const char* src = control["value"].as<const char*>();
        if (bufSize > 0 && src) {
          strlcpy(valuePointer, src, bufSize);
          // valuePointer[copyLen] = '\0';  // strlcpy does this
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

#endif  // FT_MOONLIGHT