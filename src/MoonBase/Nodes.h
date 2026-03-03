/**
    @title     MoonLight
    @file      Nodes.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

  #include "MoonBase/Modules/ModuleIO.h"      // Includes also Module.h but also enum IO_Pins
  #include "MoonLight/Layers/VirtualLayer.h"  //VirtualLayer.h will include PhysicalLayer.h

/// Builds display name with dimension emoji and tags. Implementation in Nodes.cpp.
String buildNameAndTags(const char* name, uint8_t dim, const char* tags);

/// Returns the display name of a node type with dimension emoji and tags appended.
/// Used in the UI dropdown to show e.g. "Glow 📏 ⚙️".
template <typename T>
String getNameAndTags() {
  return buildNameAndTags(T::name(), T::dim(), T::tags());
}

/// Base class for all node types (effects, layouts, modifiers, drivers).
/// Nodes define their own UI controls via addControl() and receive lifecycle callbacks:
/// constructor() → setup() → loop()/loop20ms() → ~destructor().
/// Subclasses override onLayout() for layout nodes, modifySize()/modifyPosition()/modifyXYZ() for modifiers.
class Node {
 public:
  static const char* name() { return "noname"; }
  static const char* tags() { return ""; }
  static uint8_t dim() { return _NoD; };

  VirtualLayer* layer = nullptr;  // the virtual layer this effect is using
  JsonArray controls;
  Module* moduleControl = nullptr;                // to access global lights control functions if needed
  Module* moduleIO = nullptr;                     // to access io pins if needed
  Module* moduleNodes = nullptr;                  // to request UI update if needed
  const SemaphoreHandle_t* layerMutex = nullptr;  // pointer to layerMutex (set in constructor)

  virtual bool isLiveScriptNode() const { return false; }
  virtual bool hasOnLayout() const { return false; }  // run map on monitor (pass1) and modifier new Node, on/off, control changed or layout setup, on/off or control changed (pass1 and 2)
  virtual bool hasModifier() const { return false; }  // modifier new Node, on/off, control changed: run layout.requestMapLayout. onLayoutPre: modifySize, addLight: modifyPosition XYZ: modifyXYZ

  bool on = false;  // onUpdate will set it on

  // C++ constructors are not inherited, so declare it as normal functions
  virtual void constructor(VirtualLayer* layer, const JsonArray& controls, const SemaphoreHandle_t* layerMutex) {
    this->layer = layer;
    this->controls = controls;
    this->layerMutex = layerMutex;
  }

  // destructor
  virtual ~Node() {}  // delete any allocated memory

  // effect and layout
  virtual void setup() {};

  // effect, layout and modifier

  /// Finds an existing control by name, or creates a new one. Sets newControl flag. Implementation in Nodes.cpp.
  JsonObject findOrCreateControl(const char* name, bool& newControl);

  /// Sets control properties, size, and calls onUpdate for new controls. Implementation in Nodes.cpp.
  JsonObject setupControl(const char* name, const char* type, int min, int max, bool ro, const char* desc,
                          uint8_t sizeCode, size_t sizeofVar, bool newControl, JsonObject control);

  /// Registers a UI control for this node, binding it to the given variable.
  /// The template sets value/default/pointer; all other logic is in findOrCreateControl() and setupControl().
  template <class ControlType>
  JsonObject addControl(const ControlType& variable, const char* name, const char* type, int min = 0, int max = UINT8_MAX, bool ro = false, const char* desc = nullptr) {
    bool newControl = false;
    JsonObject control = findOrCreateControl(name, newControl);

    if (newControl)
      control["value"] = variable;  // set default value for new controls
    control["default"] = variable;
    control["p"] = (uint32_t)&variable;

    // encode ControlType as a size code for the non-template helper
    uint8_t sizeCode = 0;
    if (std::is_same<ControlType, uint8_t>::value)        sizeCode = 8;
    else if (std::is_same<ControlType, int8_t>::value)    sizeCode = 108;
    else if (std::is_same<ControlType, uint16_t>::value)  sizeCode = 16;
    else if (std::is_same<ControlType, uint32_t>::value)  sizeCode = 32;
    else if (std::is_same<ControlType, int>::value)       sizeCode = 33;
    else if (std::is_same<ControlType, float>::value)     sizeCode = 34;
    else if (std::is_same<ControlType, bool>::value)      sizeCode = sizeof(bool);
    else if (std::is_same<ControlType, Coord3D>::value)   sizeCode = sizeof(Coord3D);

    return setupControl(name, type, min, max, ro, desc, sizeCode, sizeof(variable), newControl, control);
  }

  template <typename T>
  void addControlValue(const T& value) {
    if (controls.size() == 0) return;                                   // guard against empty controls
    JsonObject control = controls[controls.size() - 1];                 // last control
    if (control["values"].isNull()) control["values"].to<JsonArray>();  // add array of values
    JsonArray values = control["values"];
    values.add(value);
  }

  // called in addControl (oldValue = "") and in NodeManager onUpdate nodes[i].control[j]
  void updateControl(const JsonObject& control);  // see Nodes.cpp for implementation

  template <typename T>
  void updateControl(const char* name, const T value) {
    for (JsonObject control : controls) {
      if (control["name"] == name) {
        control["value"] = value;
        updateControl(control);
        break;
      }
    }
  }

  virtual void onUpdate(const Char<20>& oldValue, const JsonObject& control) {}

  void requestMappings() {
    if (hasModifier() || hasOnLayout()) {
      // EXT_LOGD(ML_TAG, "hasOnLayout or Modifier -> requestMapVirtual");
      layerP.requestMapVirtual = true;
    }
    if (hasOnLayout()) {
      // EXT_LOGD(ML_TAG, "hasOnLayout -> requestMapPhysical");
      layerP.requestMapPhysical = true;
    }
  }

  // effect, layout and modifier (?)
  virtual void loop() {}
  virtual void loop20ms() {}
  virtual void onSizeChanged(const Coord3D& oldSize) {}  // virtual/effect nodes: virtual size, physical/driver nodes: physical size

  // layout
  virtual void onLayout() {}  // the definition of the layout, called by mapLayout()

  // convenience functions to add a light
  void addLight(Coord3D position) { layerP.addLight(position); }

  // convenience function for next pin
  void nextPin(uint8_t ledPinDIO = UINT8_MAX) { layerP.nextPin(ledPinDIO); }

  // modifier
  virtual void modifySize() {}
  virtual void modifyPosition(Coord3D& position) {}  // not const as position is changed
  virtual void modifyXYZ(Coord3D& position) {}
};

  #include "DriverNode.h"

/// Generates a triangle wave (0→255→0) at the given BPM, similar to beat8 but symmetric.
inline uint8_t triangle8(uint8_t bpm, uint32_t timebase = 0) {
  uint8_t beat = beat8(bpm, timebase);
  if (beat < 128)
    return beat * 2;  // rising edge
  else
    return (255 - ((beat - 128) * 2));  // falling edge
}

/// Data shared between nodes (audio sync, status info, gravity, etc.).
/// Single static instance accessible by all effect/driver nodes.
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
  size_t activeClients;
  size_t clientListSize;

  Coord3D gravity;

  // FastLED Audio
  bool vocalsActive = false;
  float vocalConfidence = 0.0f;
  float bassLevel = 0.0f;
  float midLevel = 0.0f;
  float trebleLevel = 0.0f;
  bool beat = false;
  uint8_t percussionType = UINT8_MAX;

} sharedData;

  /**
   * Nodes Guidelines:
   *
   * 1) Don't use String type class variables but char[x] as can crash when node is destructed (node in PSRAM, string in heap)
   * 2) no static variables in node classes as we can run multiple instances of the same node which should not share data -> class variables.
   */

  // Drivers first as used by others
  #include "MoonLight/Nodes/Drivers/D_ArtnetIn.h"
  #include "MoonLight/Nodes/Drivers/D_ArtnetOut.h"
  #include "MoonLight/Nodes/Drivers/D_AudioSync.h"
  #include "MoonLight/Nodes/Drivers/D_FastLEDAudio.h"
  #include "MoonLight/Nodes/Drivers/D_FastLEDDriver.h"
  #include "MoonLight/Nodes/Drivers/D_Hub75.h"
  #include "MoonLight/Nodes/Drivers/D_IMU.h"
  #include "MoonLight/Nodes/Drivers/D_Infrared.h"
  #include "MoonLight/Nodes/Drivers/D_ParallelLEDDriver.h"
  #include "MoonLight/Nodes/Drivers/D__Sandbox.h"
  #include "MoonLight/Nodes/Effects/E_FastLED.h"
  #include "MoonLight/Nodes/Effects/E_MoonLight.h"
  #include "MoonLight/Nodes/Effects/E_MoonModules.h"
  #include "MoonLight/Nodes/Effects/E_MovingHeads.h"
  #include "MoonLight/Nodes/Effects/E_SoulmateLights.h"
  #include "MoonLight/Nodes/Effects/E_WLED.h"
  #include "MoonLight/Nodes/Effects/E__Sandbox.h"
  #include "MoonLight/Nodes/Layouts/L_MoonLight.h"
  #include "MoonLight/Nodes/Layouts/L_SE16.h"
  #include "MoonLight/Nodes/Layouts/L__Sandbox.h"
  #include "MoonLight/Nodes/Modifiers/M_MoonLight.h"
  #include "MoonLight/Nodes/Modifiers/M__Sandbox.h"

#endif  // FT_MOONLIGHT