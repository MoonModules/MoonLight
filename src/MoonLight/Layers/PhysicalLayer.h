/**
    @title     MoonLight
    @file      PhysicalLayer.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

  #include <vector>

  #include "FastLED.h"
  #include "MoonBase/utilities/PlatformFunctions.h"
  #include "LightsHeader.h"  // pure types: nrOfLights_t, LightsHeader, Lights — no ESP32 deps

// #include "VirtualLayer.h"

// Forward declarations (PhysicalLayer ↔ VirtualLayer/Node/Modifier are mutually referential)
class VirtualLayer;
class Node;
class Modifier;

// ----------------------------------------------------------------------------
// PhysicalLayer — top-level owner of the Lights data and all VirtualLayers.
// Manages the layout mapping pipeline (pass 1 = physical, pass 2 = virtual),
// the effect/driver loop dispatch, and the per-pin LED strip assignments.
// Global singleton: `layerP` (defined in PhysicalLayer.cpp).
// ----------------------------------------------------------------------------
class PhysicalLayer {
 public:
  // The physical channel array and its metadata.
  Lights lights;

  // All virtual layers that map onto this physical layer.
  std::vector<VirtualLayer*, VectorRAMAllocator<VirtualLayer*>> layers;

  // Shared colour palette, used by effects that don't define their own.
  CRGBPalette16 palette = PartyColors_p;

  // Layout remap request flags, consumed at the start of loopDrivers().
  // IMPORTANT: setting requestMapPhysical (pass 1) automatically triggers
  // requestMapVirtual (pass 2) in loopDrivers() — pass 2 must always follow
  // pass 1 so the virtual mapping table stays in sync with the physical layout.
  // Callers should set requestMapPhysical when the physical light count or
  // positions change, and requestMapVirtual alone when only modifiers change.
  uint8_t requestMapPhysical = false;
  uint8_t requestMapVirtual = false;

  // Driver/layout/modifier nodes attached directly to this physical layer.
  std::vector<Node*, VectorRAMAllocator<Node*>> nodes;

  // Current physical light index, incremented by addLight() during pass 2.
  nrOfLights_t indexP = 0;

  // Previous size, used to detect size changes and trigger onSizeChanged().
  Coord3D prevSize;

  // Mutexes protecting the effects and drivers task respectively.
  SemaphoreHandle_t effectsMutex = xSemaphoreCreateMutex();
  SemaphoreHandle_t driversMutex = xSemaphoreCreateMutex();

  PhysicalLayer();
  ~PhysicalLayer();

  //Deleting copy/assignment prevents accidental shallow copies of ownership-bearing state.
  PhysicalLayer(const PhysicalLayer&) = delete;
  PhysicalLayer& operator=(const PhysicalLayer&) = delete;
  PhysicalLayer(PhysicalLayer&&) = delete;
  PhysicalLayer& operator=(PhysicalLayer&&) = delete;

  // Allocate channel buffers and initialise all virtual layers.
  void setup();

  // Run one effect frame across all virtual layers (called from effectTask, Core 0).
  void loop();

  // Run 20 ms periodic updates across all virtual layers (called from effectTask(), Core 0).
  void loop20ms();

  // Run one driver frame: process pending layout mapping, then loop all driver nodes (Core 1).
  void loopDrivers();

  // Run 20 ms periodic driver updates (called from driverTask(), Core 1).
  void loop20msDrivers();

  // Execute a full layout mapping pass: calls onLayoutPre → onLayout (per node) → onLayoutPost.
  // pass must be set to 1 (physical) or 2 (virtual) before calling.
  void mapLayout();

  // Current layout pass: 1 = physical (count lights, assign pins), 2 = virtual (build mapping table).
  uint8_t pass = 0;

  // When true, pass 1 skips pin-state rebuild: ledsPerPin/ledPinsAssigned are not reset
  // and nextPin() assignments are suppressed. Positions are always stored to channelsE on
  // every pass 1 regardless of this flag; only pin-state mutation is gated.
  bool monitorPass = false;

  // Called before each layout pass to reset counters and prepare buffers.
  void onLayoutPre();

  // Register one physical light at the given position.
  // Pass 1: record the position and update size/nrOfLights.
  // Pass 2: forward to all virtual layers to build their mapping tables.
  void addLight(Coord3D position);

  // Signal that the next lights added belong to the next LED pin.
  // ledPin: explicit pin override (UINT8_MAX = use sequential order).
  void nextPin(uint8_t ledPin = UINT8_MAX);

  // Called after all addLight() calls in a pass; finalises sizes and notifies virtual layers.
  void onLayoutPost();

  // Ensures the VirtualLayer at the given index exists, creating it on demand if needed.
  // Returns nullptr if index is out of bounds.
  VirtualLayer* ensureLayer(uint8_t index);

  // Per-pin LED strip configuration (populated by board presets via ModuleIO).
  uint8_t ledPins[MAXLEDPINS];          // pin numbers in board preset order
  uint8_t ledPinsAssigned[MAXLEDPINS];  // pin numbers after assignment/override
  uint16_t ledsPerPin[MAXLEDPINS];      // number of LEDs assigned to each pin
  uint8_t nrOfLedPins = 0;             // total pins defined by the board preset
  uint8_t nrOfAssignedPins = 0;        // pins actually assigned during layout pass 1
  uint16_t maxPower = 0;               // power budget in mA (0 = unlimited)

};

// Global singleton physical layer, defined in PhysicalLayer.cpp.
extern PhysicalLayer layerP;

#endif  // FT_MOONLIGHT
