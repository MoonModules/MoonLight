/**
    @title     MoonLight
    @file      Nodes.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT
  #include "Nodes.h"

SharedData sharedData;

JsonObject Node::findOrCreateControl(const char* name, bool& newControl) { return ::findOrCreateControl(controls, name, newControl); }

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
    onUpdate(control);  // custom onUpdate for the node
  }

  return control;
}

void Node::addControlValue(const char* value) {
  if (controls.size() == 0) return;                                   // guard against empty controls
  JsonObject control = controls[controls.size() - 1];                 // last control
  if (control["values"].isNull()) control["values"].to<JsonArray>();  // add array of values
  JsonArray values = control["values"];
  values.add(value);
  EXT_LOGD(ML_TAG, "%s, %d", value, control["values"].size());
}

void Node::updateControl(const JsonObject& control) { ::updateControl(control); }

#endif  // FT_MOONLIGHT