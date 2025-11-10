/**
    @title     MoonLight
    @file      Nodes.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

  #include <ESPFS.h>

  #include "MoonBase/Module.h"
  #include "MoonLight/Layers/VirtualLayer.h"  //VirtualLayer.h will include PhysicalLayer.h

  #define NODE_METADATA_VIRTUALS()                          \
    const char* getName() const override { return name(); } \
    uint8_t getDim() const override { return dim(); }       \
    const char* getTags() const override { return tags(); }

template <typename T>
String getNameAndTags() {
  String result = T::name();

  uint8_t dim = T::dim();
  if (dim == _0D)
    result += " 💡";
  else if (dim == _1D)
    result += " 📏";
  else if (dim == _2D)
    result += " ⏹️";
  else if (dim == _3D)
    result += " 🧊";

  result += " ";
  result += T::tags();

  return result;
}

class Node {
 public:
  static const char* name() { return "noname"; }
  static const char* tags() { return ""; }
  static uint8_t dim() { return _NoD; };

  VirtualLayer* layer = nullptr;  // the virtual layer this effect is using
  JsonArray controls;
  Module* controlModule = nullptr;  // to access global lights control functions if needed

  virtual bool isLiveScriptNode() const { return false; }
  virtual bool hasOnLayout() const { return false; }  // run map on monitor (pass1) and modifier new Node, on/off, control changed or layout setup, on/off or control changed (pass1 and 2)
  virtual bool hasModifier() const { return false; }  // modifier new Node, on/off, control changed: run layout.requestMapLayout. onLayoutPre: modifySize, addLight: modifyPosition XYZ: modifyXYZ

  bool on = false;  // onUpdate will set it on

  // C++ constructors are not inherited, so declare it as normal functions
  virtual void constructor(VirtualLayer* layer, JsonArray controls) {
    this->layer = layer;
    this->controls = controls;
  }

  // effect and layout
  virtual void setup() {};

  // effect, layout and modifier

  template <class ControlType>
  JsonObject addControl(ControlType& variable, const char* name, const char* type, int min = 0, int max = UINT8_MAX, bool ro = false, const char* desc = nullptr) {
    uint32_t pointer = (uint32_t)&variable;

    bool newControl = false;  // flag to check if control is new or already exists
    // if control already exists only update it's pointer
    JsonObject control;
    for (JsonObject control1 : controls) {
      if (control1["name"] == name) {
        // EXT_LOGD(ML_TAG, "%s t:%s p:%p ps:%d", name, type, pointer, sizeof(ControlType));
        control1["p"] = pointer;
        control = control1;  // set control to the found one
        break;
      }
    }

    if (control.isNull()) {  // if control not found, create a new one
      control = controls.add<JsonObject>();
      control["name"] = name;
      control["type"] = type;
      control["default"] = variable;

      control["p"] = pointer;

      if (ro) control["ro"] = true;                // else if (!control["ro"].isNull()) control.remove("ro");
      if (min != 0) control["min"] = min;          // else if (!control["min"].isNull()) control.remove("min");
      if (max != UINT8_MAX) control["max"] = max;  // else if (!control["max"].isNull()) control.remove("max");
      if (desc) control["desc"] = desc;

      newControl = true;  // set flag to true, as control is new
    }

    // EXT_LOGD(ML_TAG, "%s t:%s p:%p ps:%d", name, type, pointer, sizeof(ControlType));

    if (newControl) control["value"] = variable;  // set default

    // setValue
    if (control["type"] == "slider" || control["type"] == "select" || control["type"] == "pin" || control["type"] == "number") {
      if (std::is_same<ControlType, uint8_t>::value) {
        control["size"] = 8;
      } else if (std::is_same<ControlType, uint16_t>::value) {
        control["size"] = 16;
      } else if (std::is_same<ControlType, int>::value) {
        control["size"] = 32;
      } else if (std::is_same<ControlType, float>::value) {
        control["size"] = 33;  // trick to indicate float (which is 32 bits)
      } else {
        EXT_LOGE(ML_TAG, "size %d mismatch for %s", sizeof(ControlType), name);
      }
    } else if (control["type"] == "selectFile" || control["type"] == "text") {
      // if (sizeof(ControlType) != 4) {
      //   EXT_LOGE(ML_TAG, "sizeof mismatch for %s", name);
      // } else
      control["size"] = sizeof(variable);
    } else if (control["type"] == "checkbox") {
      if (!std::is_same<ControlType, bool>::value) {
        EXT_LOGE(ML_TAG, "type for %s is not bool", name);
      } else
        control["size"] = sizeof(bool);
    } else if (control["type"] == "coord3D") {
      if (!sizeof(!std::is_same<ControlType, Coord3D>::value)) {
        EXT_LOGE(ML_TAG, "type for %s is not Coord3D", name);
      } else
        control["size"] = sizeof(Coord3D);
    } else
      EXT_LOGE(ML_TAG, "type of %s not compatible: %s (%d)", control["name"].as<String>().c_str(), control["type"].as<String>().c_str(), control["size"].as<uint8_t>());

    if (newControl) {
      String oldValue = "";
      updateControl(oldValue, control);
      onUpdate(oldValue, control);  // custom onUpdate for the node
    }

    return control;
  }

  // called in addControl (oldValue = "") and in NodeManager onUpdate nodes[i].control[j]
  virtual void updateControl(String& oldValue, JsonObject control);  // see Nodes.cpp for implementation

  virtual void onUpdate(String& oldValue, JsonObject control) {}

  void requestMappings() {
    if (hasModifier() || hasOnLayout()) {
      // EXT_LOGD(ML_TAG, "hasOnLayout or Modifier -> requestMapVirtual");
      layer->layerP->requestMapVirtual = true;
    }
    if (hasOnLayout()) {
      // EXT_LOGD(ML_TAG, "hasOnLayout -> requestMapPhysical");
      layer->layerP->requestMapPhysical = true;
    }
  }

  // effect, layout and modifier (?)
  virtual void loop() {}
  virtual void onSizeChanged(Coord3D oldSize) {}  // virtual/effect nodes: virtual size, physical/driver nodes: physical size

  // layout
  virtual void onLayout() {}  // the definition of the layout, called by mapLayout()

  // convenience functions to add a light
  void addLight(Coord3D position) { layer->layerP->addLight(position); }

  // convenience functions to add a pin
  void addPin(uint8_t pinNr) { layer->layerP->addPin(pinNr); }
  char* addNextPin(char*& nextPin) {  //&: by reference to change the pointer to the next pin
    while (*nextPin && !isdigit((unsigned char)*nextPin)) nextPin++;
    if (*nextPin) {
      int pin = strtol(nextPin, (char**)&nextPin, 10);
      addPin(pin);
    }  // add next pin
    return nextPin;
  }

  // modifier
  virtual void modifySize() {}
  virtual void modifyPosition(Coord3D& position) {}  // not const as position is changed
  virtual void modifyXYZ(Coord3D& position) {}

  virtual ~Node() {}  // delete any allocated memory
};

  #if FT_LIVESCRIPT
class LiveScriptNode : public Node {
 public:
  static const char* name() { return "LiveScriptNode"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "⚙️"; }

  bool hasSetupFunction = false;
  bool hasLoopFunction = false;
  // bool hasAddControlFunction = false;
  bool hasModifyFunction = false;
  bool hasOnLayoutFunction = false;
  bool isLiveScriptNode() const override { return true; }
  bool hasModifier() const override { return hasModifyFunction; }
  bool hasOnLayout() const override { return hasOnLayoutFunction; }

  const char* animation = nullptr;  // which animation (file) to run

  void setup() override;  // addExternal, compileAndRun

  void loop() override;  // call Node.loop to process requestMapLayout. todo: sync with script...

  // layout
  void onLayout() override;  // call map in LiveScript

  ~LiveScriptNode();

  // LiveScript functions
  void compileAndRun();
  void execute();
  void kill();
  void free();
  void killAndDelete();
  static void getScriptsJson(JsonArray scripts);
};

  #endif

  #if HP_ALL_DRIVERS
    // #define NUM_LEDS_PER_STRIP 256 not for non virtal... (only setting __delay when NO_WAIT)
    #include "I2SClocklessLedDriver.h"
extern I2SClocklessLedDriver ledsDriver;  // defined in Nodes.cpp
  #else                                   // ESP32_LEDSDRIVER
    #include "ESP32-LedsDriver.h"
    #define MAX_PINS 20  // this is also defined in ESP32-LedsDriver.h...
    #if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
static PhysicalDriverESP32S3 ledsDriver;  //    sizeof(driver) = 1080K !
    #elif CONFIG_IDF_TARGET_ESP32
static PhysicalDriverESP32D0 ledsDriver;  //    sizeof(driver) = 1080K !
    #else
static LedsDriver ledsDriver;  //   only the core driver, for setBrightness and setColorCorrection and LUT
    #endif
  #endif

class DriverNode : public Node {
  uint16_t maxPower = 10;
  uint8_t brightnessSaved = UINT8_MAX;

 protected:
  bool lightPresetSaved = false;  // initLeds can only start if this has been saved

  #if HP_ALL_DRIVERS
  CRGB savedColorCorrection;
  bool initDone = false;
  #endif

 protected:
  uint8_t lightPreset = 2;  // GRB

 public:
  void setup() override;

  void loop() override;

  void reOrderAndDimRGBW(uint8_t* packetRGBChannel, uint8_t* lightsRGBChannel);

  // called in addControl (oldValue = "") and in NodeManager onUpdate nodes[i].control[j]
  void onUpdate(String& oldValue, JsonObject control) override;
};

// Helper function to generate a triangle wave similar to beat16
inline uint8_t triangle8(uint8_t bpm, uint32_t timebase = 0) {
  uint8_t beat = beat8(bpm, timebase);
  if (beat < 128)
    return beat * 2;  // rising edge
  else
    return (255 - ((beat - 128) * 2));  // falling edge
}

// data shared between nodes
static struct SharedData {
  // audio sync
  uint8_t bands[16] = {0};  // Our calculated freq. channel result table to be used by effects
  float volume;             // either sampleAvg or sampleAgc depending on soundAgc; smoothed sample
  int16_t volumeRaw;
  float majorPeak;  // FFT: strongest (peak) frequency

  // used in scrollingtext
  uint16_t fps;
  uint8_t connectionStatus;
  size_t connectedClients;
  size_t clientListSize;
} sharedData;

  /**
   * Nodes Guidelines:
   *
   * 1) Don't use String type class variables but char[x] as can crash when node is destructed (node in PSRAM, string in heap)
   * 2) no static variables in node classes as we can run multiple instances of the same node which should not share data -> class variables.
   */

  // Drivers first as used by others
  #include "MoonLight/Nodes/Drivers/D_ArtnetOut.h"
  #include "MoonLight/Nodes/Drivers/D_ArtnetIn.h"
  #include "MoonLight/Nodes/Drivers/D_AudioSync.h"
  #include "MoonLight/Nodes/Drivers/D_DMXIn.h"
  #include "MoonLight/Nodes/Drivers/D_FastLED.h"
  #include "MoonLight/Nodes/Drivers/D_Hub75.h"
  #include "MoonLight/Nodes/Drivers/D_Infrared.h"
  #include "MoonLight/Nodes/Drivers/D_PhysicalDriver.h"
  #include "MoonLight/Nodes/Drivers/D_VirtualDriver.h"
  #include "MoonLight/Nodes/Drivers/D__Sandbox.h"
  #include "MoonLight/Nodes/Effects/E_FastLED.h"
  #include "MoonLight/Nodes/Effects/E_MoonLight.h"
  #include "MoonLight/Nodes/Effects/E_MoonModules.h"
  #include "MoonLight/Nodes/Effects/E_MovingHeads.h"
  #include "MoonLight/Nodes/Effects/E_SoulmateLights.h"
  #include "MoonLight/Nodes/Effects/E_WLED.h"
  #include "MoonLight/Nodes/Effects/E__Sandbox.h"
  #include "MoonLight/Nodes/Layouts/L_MoonLight.h"
  #include "MoonLight/Nodes/Layouts/L__Sandbox.h"
  #include "MoonLight/Nodes/Modifiers/M_MoonLight.h"
  #include "MoonLight/Nodes/Modifiers/M__Sandbox.h"
  #ifdef BUILD_TARGET_ESP32_S3_STEPHANELEC_16P
    #include "MoonLight/Nodes/Layouts/L_SE16.h"
  #endif

#endif  // FT_MOONLIGHT