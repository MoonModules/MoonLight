/**
    @title     MoonLight
    @file      Nodes.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/general/utilities/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact moonmodules@icloud.com
**/

#pragma once

#if FT_MOONLIGHT

#define _1D 1
#define _2D 2
#define _3D 3

#include "VirtualLayer.h" //VirtualLayer.h will include PhysicalLayer.h

#include <ESPFS.h>

class Node {
public:
  static const char * name() {return "noname";}
  static const char * tags() {return "";}
  static uint8_t dim() {return _1D;};

  VirtualLayer *layerV = nullptr; //the virtual layer this effect is using
  JsonArray controls;

  bool hasLayout = false; //run map on monitor (pass1) and modifier new Node, on/off, control changed or layout setup, on/off or control changed (pass1 and 2) 
  bool hasModifier = false; //modifier new Node, on/off, control changed: run layout.requestMapLayout. addLayoutPre: modifyLayout, addLight: modifyLight (todo XYZ: modifyXYZ)

  virtual bool isLiveScriptNode() const { return false; }

  bool on = false; //onUpdate will set it on

  //C++ constructor and destructor are not inherited, so declare it as normal functions
  virtual void constructor(VirtualLayer *layerV, JsonArray controls) {
    this->layerV = layerV;
    this->controls = controls;
  }

  //effect and layout
  virtual void setup() {
    if (hasLayout) {
      layerV->layerP->lights.header.resetOffsets(); //reset offsets to default
      layerV->requestMapPhysical = true;
      layerV->requestMapVirtual = true;
    }
  };

  //effect, layout and modifier
  template <class ControlType, class PointerType>
  JsonObject addControl(PointerType pointer2, const char *name, const char *type, ControlType defaul, int min = 0, int max = 255) {
    uint32_t pointer = (uint32_t)pointer2;

    // if control already exists only update it's pointer
    for (JsonObject control : controls) {
      if (control["name"] == name) {
        ESP_LOGD(TAG, "update control %s %s %d", name, type, defaul);
        control["p"] = pointer;
        return control;
      }
    }
    JsonObject control = controls.add<JsonObject>(); 
    control["name"] = name;
    control["type"] = type;
    control["default"] = defaul;
    control["p"] = pointer;
    if (min != INT_MIN) control["min"] = min;
    if (max != INT_MAX) control["max"] = max;

    if (pointer) {
      //setValue
      if (control["type"] == "range" || control["type"] == "select" || control["type"] == "pin") {
        uint8_t *valuePointer = (uint8_t *)pointer;
        *valuePointer = defaul;
        control["value"] = *valuePointer;
      }
      else if (control["type"] == "selectFile") {
        char *valuePointer = (char *)pointer;
        *valuePointer = defaul;
        control["value"] = valuePointer;
      }
      else if (control["type"] == "number") {
        uint16_t *valuePointer = (uint16_t *)pointer;
        *valuePointer = defaul;
        control["value"] = *valuePointer;
      }
      else if (control["type"] == "checkbox") {
        bool *valuePointer = (bool *)pointer;
        *valuePointer = defaul;
        control["value"] = *valuePointer;
      }
      // else if (control["type"] == "coord3D") {
      //   Coord3D *valuePointer = (Coord3D *)pointer;
        // control["value"] = *valuePointer;
      // }
      else
        ESP_LOGE(TAG, "type not supported yet %s", control["type"].as<String>().c_str());
    }

    return control;
  };

  virtual void updateControl(JsonObject control); // see Nodes.cpp for implementation

  //effect, layout and modifier (?)
  virtual void loop() {}

  //layout
  virtual void addLayout() {} //the definition of the layout, called by mapLayout()

  void addLight(Coord3D position) {
    layerV->layerP->addLight(position);
  }

  void addPin(uint8_t pinNr) {
    layerV->layerP->addPin(pinNr);
  }

  //modifier
  virtual void modifyLayout() {}
  virtual void modifyLight(Coord3D &position) {} //not const as position is changed
  virtual void modifyXYZ(Coord3D &position) {}

  virtual void destructor() {
    //delete any allocated memory
    if (hasLayout)
      layerV->resetMapping();
  }

};
  

#if FT_LIVESCRIPT
class LiveScriptNode: public Node {
  public:

  static const char * name() {return "LiveScriptNode ⚙️";}
  static uint8_t dim() {return _2D;}
  static const char * tags() {return "";}

  bool hasSetup = false;
  bool hasLoop = false;
  bool hasAddControl = false;
  bool isLiveScriptNode() const override { return true; }

  const char *animation; //which animation (file) to run

  void setup() override; //addExternal, compileAndRun
  
  void loop() override; //call Node.loop to process requestMapLayout. todo: sync with script...

  //layout
  void addLayout() override; // call map in LiveScript
  
  void destructor() override;

  // LiveScript functions
  void compileAndRun();
  void execute();
  void kill();
  void free();
  void killAndDelete();
  void getScriptsJson(JsonArray scripts);
  
};

#endif

#include "Mods.h"

#include "Layouts.h"

#include "Effects.h"
// #include "EffectsFastLED.h"

#include "Modifiers.h"

#endif //FT_MOONLIGHT