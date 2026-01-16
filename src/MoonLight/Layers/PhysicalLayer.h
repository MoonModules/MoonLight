/**
    @title     MoonLight
    @file      PhysicalLayer.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

  #include <vector>

  #include "FastLED.h"
  #include "MoonBase/Utilities.h"

// #include "VirtualLayer.h"

  #define MAXLEDPINS 20  // max strips for Parallel LED Driver

class VirtualLayer;  // Forward as PhysicalLayer refers back to VirtualLayer
class Node;          // Forward as PhysicalLayer refers back to Node
class Modifier;      // Forward as PhysicalLayer refers back to Modifier

  #ifdef BOARD_HAS_PSRAM
typedef uint32_t nrOfLights_t;
  #else
typedef uint16_t nrOfLights_t;
  #endif

// make changes to LightsHeader carefully as the alignment of this struct must be preserved as Monitor.svelte is depending on it
struct LightsHeader {
  Coord3D size = Coord3D(16, 16, 1);  // 0 max position of light, counted by onLayoutPre/Post and addLight. 12 bytes not 0,0,0 to prevent div0 eg in Octopus2D
  uint32_t nrOfLights = 256;          // 12 nr of physical lights, counted by addLight
  uint32_t nrOfChannels;              // 16,  so we can deal with exceptional cases e.g. RGB2040 make sure it starts at even position!!! for alignment!!!
  uint8_t lightPreset = 2;            // 20, so we can deal with exceptional cases e.g. RGB2040. default 2 / GRB
  uint8_t channelsPerLight = 3;       // 21 RGB default
  uint8_t brightness;                 // 22 brightness set by light control (sent to LEDs driver normally)
  uint8_t red = 255;                  // 23 brightness set by light control (sent to LEDs driver normally)
  uint8_t green = 255;                // 24 brightness set by light control (sent to LEDs driver normally)
  uint8_t blue = 255;                 // 25 brightness set by light control (sent to LEDs driver normally)
  uint8_t offsetRGB = 0;              // 26 RGB default
  struct {                            // 27 condensed rgb
    uint8_t isPositions : 2 = 0;      // is the lights.positions array filled with positions
    uint8_t offsetRed : 2 = 1;        // GRB is default
    uint8_t offsetGreen : 2 = 0;
    uint8_t offsetBlue : 2 = 2;
  };  // 8 bits
  uint8_t offsetWhite = UINT8_MAX;  // 28
  // PAR lights
  uint8_t offsetBrightness = UINT8_MAX;  // 29 in case the light has a separate brightness channel
  // Moving heads
  uint8_t offsetPan = UINT8_MAX;          // 30
  uint8_t offsetTilt = UINT8_MAX;         // 31
  uint8_t offsetZoom = UINT8_MAX;         // 32
  uint8_t offsetRotate = UINT8_MAX;       // 33
  uint8_t offsetGobo = UINT8_MAX;         // 34
  uint8_t offsetRGB1 = UINT8_MAX;         // 35
  uint8_t offsetRGB2 = UINT8_MAX;         // 36
  uint8_t offsetRGB3 = UINT8_MAX;         // 37
  uint8_t offsetBrightness2 = UINT8_MAX;  // 38
  // =============
  // 39 bytes total
  uint8_t fill[3];  // fill with dummies to have at least headerPrimeNumber bytes total, be aware of padding so do not change order of vars (Coord3D and uint32_t on top, uint8_t after that)

  void resetOffsets() {
    nrOfChannels = 0;
    channelsPerLight = 3;  // RGB default
    // lightPreset = lightPreset_GRB; // don't reset as managed by Drivers
    offsetRGB = 0;
    offsetRed = 1;
    offsetGreen = 0;
    offsetBlue = 2;
    offsetWhite = UINT8_MAX;
    offsetBrightness = UINT8_MAX;
    offsetPan = UINT8_MAX;
    offsetTilt = UINT8_MAX;
    offsetZoom = UINT8_MAX;
    offsetRotate = UINT8_MAX;
    offsetGobo = UINT8_MAX;
    offsetRGB1 = UINT8_MAX;
    offsetRGB2 = UINT8_MAX;
    offsetRGB3 = UINT8_MAX;
    offsetBrightness2 = UINT8_MAX;
    memset(fill, 0, sizeof(fill));  // set to 0
  }
};

struct Lights {
  LightsHeader header;
  uint8_t* channelsE = nullptr;  // channels used by effects and modifiers (double buffering)
  uint8_t* channelsD = nullptr;  // channels used by drivers (double buffering)
  nrOfLights_t maxChannels = 0;
  bool useDoubleBuffer = false;  // Only when PSRAM available

  // std::vector<size_t> universes; //tells at which byte the universe starts
};

// contains the Lights structure/definition and implements layout functions (add*, modify*)
class PhysicalLayer {
 public:
  Lights lights;  // the physical lights

  // std::vector<bool> lightsToBlend; //this is a 1-bit vector !!! overlapping effects will blend
  // uint8_t globalBlend = 128; // to do add as UI control...

  std::vector<VirtualLayer*, VectorRAMAllocator<VirtualLayer*>> layers;  // the virtual layers using this physical layer

  CRGBPalette16 palette = PartyColors_p;

  uint8_t requestMapPhysical = false;  // collect requests to map as it is requested by setup and onUpdate and only need to be done once
  uint8_t requestMapVirtual = false;   // collect requests to map as it is requested by setup and onUpdate and only need to be done once

  std::vector<Node*, VectorRAMAllocator<Node*>> nodes;

  nrOfLights_t indexP = 0;

  Coord3D prevSize;  // to calculate size change

  SemaphoreHandle_t effectsMutex = xSemaphoreCreateMutex();
  SemaphoreHandle_t driversMutex = xSemaphoreCreateMutex();

  PhysicalLayer();
  ~PhysicalLayer();

  void setup();
  void loop();
  void loop20ms();
  void loopDrivers();

  // mapLayout calls onLayoutPre, onLayout for each node and onLayoutPost and expects pass to be set (1 or 2)
  void mapLayout();

  uint8_t pass = 0;  //'class global' so addLight/Pin functions know which pass it is in
  bool monitorPass = false;
  void onLayoutPre();
  void addLight(Coord3D position);
  void nextPin(uint8_t ledPin = UINT8_MAX);  // if more pins are defined, the next lights will be assigned to the next pin
  void onLayoutPost();

  // from board presets
  uint8_t ledPins[MAXLEDPINS];
  uint8_t ledPinsAssigned[MAXLEDPINS];
  uint16_t ledsPerPin[MAXLEDPINS];  // uint16_t is more then enough as more then 65K leds per pin will never happen
  uint8_t nrOfLedPins = 0;
  uint8_t nrOfAssignedPins = 0;
  uint16_t maxPower = 0;

  // an effect is using a virtual layer: tell the effect in which layer to run...

  // to be called in setup, if more then one effect
  // void initLightsToBlend();

  uint8_t gamma8(uint8_t b) {  // we do nothing with gamma for now
    return b;
  }
};

extern PhysicalLayer layerP;  // global declaration of the physical layer

#endif  // FT_MOONLIGHT