/**
    @title     MoonLight Layer Manager Unit Tests
    @file      test_layer_manager.cpp
    @repo      https://github.com/MoonModules/MoonLight
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007

    Native unit tests for LayerManager.
    Tests call the real LayerManager class methods (selectLayer, prepareForPresetLoad,
    onNodeRemoved) via minimal stubs for the ESP32/FreeRTOS dependencies.

    Run with: pio test -e native
**/

#include "doctest.h"

#define FT_MOONLIGHT 1  // activate LayerManager code

// ---------------------------------------------------------------------------
// Minimal stubs — native replacements for ESP32/FreeRTOS types
// ---------------------------------------------------------------------------

#include <ArduinoJson.h>
#include <functional>
#include <vector>
#include <cstring>

// Logging macros — no-ops in native tests
#ifndef EXT_LOGD
  #define EXT_LOGD(tag, ...)
  #define EXT_LOGW(tag, ...)
  #define EXT_LOGE(tag, ...)
  #define ML_TAG "ML"
#endif

// Coord3D — pure C++ header, available natively
#include "MoonBase/utilities/Coord3D.h"

// VectorRAMAllocator stub — standard allocator on host
template <typename T>
using VectorRAMAllocator = std::allocator<T>;

// Node stub
struct Node {
  bool on = true;
  void requestMappings() {}
};

// UpdatedItem stub (used by handleUpdate, not tested here)
struct UpdatedItem {
  struct Parent { const char* operator[](int) const { return ""; } } parent;
  const char* name = "";
  JsonVariantConst value;
};

// VirtualLayer stub — only the fields LayerManager actually touches
struct VirtualLayer {
  Coord3D startPct{0, 0, 0};
  Coord3D endPct{100, 100, 100};
  uint8_t brightness = 255;
  std::vector<Node*, VectorRAMAllocator<Node*>> nodes;
  void* layerP = nullptr;
  void setup() {}
};

// PhysicalLayer stub
struct PhysicalLayer {
  std::vector<VirtualLayer*> layers;
  int activeLayerCount = 0;
  bool requestMapVirtual = false;

  PhysicalLayer() {
    layers.resize(8, nullptr);
    layers[0] = new VirtualLayer();
    activeLayerCount = 1;
  }

  VirtualLayer* ensureLayer(uint8_t i) {
    if (i >= layers.size()) return nullptr;
    if (!layers[i]) {
      layers[i] = new VirtualLayer();
      activeLayerCount++;
    }
    return layers[i];
  }

  void reset() {
    for (auto& l : layers) { delete l; l = nullptr; }
    layers[0] = new VirtualLayer();
    activeLayerCount = 1;
    requestMapVirtual = false;
  }
} layerP;  // global singleton (matches PhysicalLayer.cpp)

// NodeManager stub (only addNode needed for restoreNonSelectedLayers)
struct NodeManager {
  Node* addNode(int /*index*/, const char* /*name*/, JsonArray /*controls*/) { return nullptr; }
};

// ModuleState stub — just wraps a JsonDocument
struct ModuleState {
  StaticJsonDocument<4096> _doc;
  JsonObject data;
  std::function<void(JsonObject)> readHook;
  ModuleState() : data(_doc.to<JsonObject>()) {}
};

// ---------------------------------------------------------------------------
// Now include the real LayerManager (FT_MOONLIGHT=1, ARDUINO not defined →
// #ifdef ARDUINO blocks are skipped, stubs above supply the missing types)
// ---------------------------------------------------------------------------
#include "MoonLight/Layers/LayerManager.h"

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

/// Add a named node object to a JsonArray (mimics what ModuleEffects stores).
static void addNode(JsonArray nodes, const char* name) {
  JsonObject n = nodes.add<JsonObject>();
  n["name"] = name;
  n["on"]   = true;
}

/// Returns true when data["nodes"] contains a node with the given name.
static bool nodesHas(JsonObject data, const char* name) {
  JsonArray nodes = data["nodes"].as<JsonArray>();
  if (nodes.isNull()) return false;
  for (JsonObject n : nodes) {
    if (strcmp(n["name"].as<const char*>(), name) == 0) return true;
  }
  return false;
}

/// Fixture: fresh LayerManager wired to a ModuleState and the layerP stub.
struct Fixture {
  ModuleState state;
  std::vector<Node*, VectorRAMAllocator<Node*>>* nodesVec = nullptr;
  bool uiUpdate = false;
  LayerManager lm;

  Fixture() {
    layerP.reset();
    nodesVec = &(layerP.layers[0]->nodes);
    lm.init(state, nodesVec, uiUpdate);
    // Start with an empty nodes array in state
    state.data["nodes"].to<JsonArray>();
  }
};

// ---------------------------------------------------------------------------
// selectLayer — JSON state swap
// ---------------------------------------------------------------------------

TEST_CASE("selectLayer saves current layer and loads next layer") {
  Fixture f;

  // Layer 0: add Gradient
  addNode(f.state.data["nodes"].as<JsonArray>(), "Gradient");

  // Switch to layer 1 — saves layer 0's nodes, loads layer 1 (empty)
  f.lm.selectLayer(1);

  // nodes_0 must have been saved
  REQUIRE_FALSE(f.state.data["nodes_0"].isNull());
  CHECK_EQ(f.state.data["nodes_0"].as<JsonArray>().size(), 1u);
  CHECK_EQ(strcmp(f.state.data["nodes_0"][0]["name"].as<const char*>(), "Gradient"), 0);

  // data["nodes"] must now be empty (layer 1 has no saved state)
  CHECK_EQ(f.state.data["nodes"].as<JsonArray>().size(), 0u);
  CHECK_EQ(f.lm.getSelectedLayer(), 1u);
}

TEST_CASE("selectLayer round-trip: switch away and back restores nodes") {
  Fixture f;

  // Layer 0: Gradient
  addNode(f.state.data["nodes"].as<JsonArray>(), "Gradient");

  // Switch to layer 1, add Fire
  f.lm.selectLayer(1);
  addNode(f.state.data["nodes"].as<JsonArray>(), "Fire");

  // Switch back to layer 0
  f.lm.selectLayer(0);

  CHECK(nodesHas(f.state.data, "Gradient"));
  CHECK_FALSE(nodesHas(f.state.data, "Fire"));
  CHECK_EQ(f.lm.getSelectedLayer(), 0u);
}

TEST_CASE("selectLayer(i, swapState=false) does not touch nodes JSON") {
  Fixture f;

  addNode(f.state.data["nodes"].as<JsonArray>(), "Gradient");
  // Save a reference snapshot — after selectLayer(false) the JSON must be unchanged
  size_t before = f.state.data["nodes"].as<JsonArray>().size();

  f.lm.selectLayer(1, /*swapState=*/false);

  CHECK_EQ(f.state.data["nodes"].as<JsonArray>().size(), before);
  CHECK_EQ(f.lm.getSelectedLayer(), 1u);
}

// ---------------------------------------------------------------------------
// prepareForPresetLoad — clear non-selected layers
// ---------------------------------------------------------------------------

TEST_CASE("prepareForPresetLoad clears non-zero layer state keys") {
  Fixture f;

  // Set up: layers 1 and 2 exist with persisted state
  layerP.ensureLayer(1);
  layerP.ensureLayer(2);
  addNode(f.state.data["nodes_1"].to<JsonArray>(), "Fire");
  f.state.data["start_1"]["x"]  = 0;
  f.state.data["end_1"]["x"]    = 50;
  f.state.data["brightness_1"]  = 180;
  addNode(f.state.data["nodes_2"].to<JsonArray>(), "Rainbow");
  f.state.data["brightness_2"]  = 200;

  f.lm.prepareForPresetLoad();

  // All non-zero layer keys must be gone
  CHECK(f.state.data["nodes_1"].isNull());
  CHECK(f.state.data["start_1"].isNull());
  CHECK(f.state.data["end_1"].isNull());
  CHECK(f.state.data["brightness_1"].isNull());
  CHECK(f.state.data["nodes_2"].isNull());
  CHECK(f.state.data["brightness_2"].isNull());
}

TEST_CASE("prepareForPresetLoad resets layer 0 bounds to defaults") {
  Fixture f;

  // Simulate stale bounds from a previous preset
  layerP.layers[0]->startPct = {10, 20, 0};
  layerP.layers[0]->endPct   = {80, 90, 100};
  layerP.layers[0]->brightness = 100;
  f.state.data["start"]["x"] = 10;
  f.state.data["end"]["x"]   = 80;

  f.lm.prepareForPresetLoad();

  CHECK_EQ(layerP.layers[0]->startPct.x, 0);
  CHECK_EQ(layerP.layers[0]->endPct.x, 100);
  CHECK_EQ(layerP.layers[0]->brightness, 255);
  // Flat "start"/"end" keys must be removed so old presets don't bleed through
  CHECK(f.state.data["start"].isNull());
  CHECK(f.state.data["end"].isNull());
}

// ---------------------------------------------------------------------------
// prepareForPresetLoad — regression: selectedLayer > 0
//
// Bug: when selectedLayer was > 0 at preset-switch time, data["nodes"] still
// held the stale layer-N nodes after selectLayer(0, false).  compareRecursive
// then diffed the new preset against those stale nodes and could skip node
// recreation if they matched the just-destroyed nodes.
// Fix: call layerStateLoad(data, 0) to overwrite data["nodes"] with layer 0.
// ---------------------------------------------------------------------------

TEST_CASE("prepareForPresetLoad regression: selectedLayer>0 reloads layer-0 nodes") {
  Fixture f;

  // Simulate: user is on layer 1 (Fire running), layer 0 has Gradient saved
  addNode(f.state.data["nodes_0"].to<JsonArray>(), "Gradient");
  f.lm.selectLayer(1, /*swapState=*/false);  // move to layer 1 without JSON swap

  // Manually put Fire into data["nodes"] (as if it had been running on layer 1)
  addNode(f.state.data["nodes"].as<JsonArray>(), "Fire");
  f.state.data["layer"] = 1;

  f.lm.prepareForPresetLoad();  // selectedLayer was 1

  // data["nodes"] must now reflect layer 0 (Gradient), not the stale layer-1 Fire
  CHECK(nodesHas(f.state.data, "Gradient"));
  CHECK_FALSE(nodesHas(f.state.data, "Fire"));
  CHECK_EQ(f.lm.getSelectedLayer(), 0u);
}

TEST_CASE("prepareForPresetLoad regression: no nodes_0 saved → nodes is empty") {
  Fixture f;

  // User was on layer 1, nodes_0 was never saved (e.g. first boot)
  f.lm.selectLayer(1, false);
  addNode(f.state.data["nodes"].as<JsonArray>(), "StaleEffect");

  f.lm.prepareForPresetLoad();

  JsonArray nodes = f.state.data["nodes"].as<JsonArray>();
  REQUIRE_FALSE(nodes.isNull());
  CHECK_EQ(nodes.size(), 0u);  // must clear stale nodes, not keep them
}

// ---------------------------------------------------------------------------
// onNodeRemoved — destroy empty non-zero layer, reload layer 0
// ---------------------------------------------------------------------------

TEST_CASE("onNodeRemoved: non-zero empty layer is destroyed and state keys cleared") {
  Fixture f;

  // Switch to layer 1
  addNode(f.state.data["nodes"].as<JsonArray>(), "Gradient");
  f.lm.selectLayer(1);

  // Save layer 0 state and populate layer 1 state keys
  addNode(f.state.data["nodes_1"].to<JsonArray>(), "Fire");
  f.state.data["start_1"]["x"]  = 5;
  f.state.data["end_1"]["x"]    = 95;
  f.state.data["brightness_1"]  = 200;
  // Layer 1's node vector is empty (node was just removed — that's the trigger)
  // layerP.layers[1] exists but nodes.empty() == true

  f.lm.onNodeRemoved();

  // Layer 1 must be destroyed and its JSON keys cleared
  CHECK(layerP.layers[1] == nullptr);
  CHECK(f.state.data["nodes_1"].isNull());
  CHECK(f.state.data["start_1"].isNull());
  CHECK(f.state.data["end_1"].isNull());
  CHECK(f.state.data["brightness_1"].isNull());

  // Selected layer must fall back to 0
  CHECK_EQ(f.lm.getSelectedLayer(), 0u);
}

TEST_CASE("onNodeRemoved: data[nodes] restored to layer 0 after layer 1 destroyed") {
  Fixture f;

  // Layer 0 has Gradient saved
  addNode(f.state.data["nodes_0"].to<JsonArray>(), "Gradient");

  // Switch to layer 1, then remove its only node
  addNode(f.state.data["nodes"].as<JsonArray>(), "Gradient");  // needed for save
  f.lm.selectLayer(1);
  // data["nodes"] is now empty (layer 1 had nothing saved yet)
  // Simulate: layer 1 node was just removed → nodes.empty() == true

  f.lm.onNodeRemoved();

  // data["nodes"] must reflect layer 0
  CHECK(nodesHas(f.state.data, "Gradient"));
  CHECK_EQ(f.lm.getSelectedLayer(), 0u);
}

TEST_CASE("onNodeRemoved: no-op when selectedLayer is 0") {
  Fixture f;

  // selectedLayer is 0 — onNodeRemoved must not touch anything
  addNode(f.state.data["nodes"].as<JsonArray>(), "Gradient");
  f.lm.onNodeRemoved();

  // Nothing changed
  CHECK(nodesHas(f.state.data, "Gradient"));
  CHECK_EQ(f.lm.getSelectedLayer(), 0u);
  CHECK(layerP.layers[0] != nullptr);
}

TEST_CASE("onNodeRemoved: no-op when selected layer still has nodes") {
  Fixture f;

  f.lm.selectLayer(1);
  // Add a real Node so nodes.empty() == false
  Node n;
  layerP.layers[1]->nodes.push_back(&n);

  f.lm.onNodeRemoved();

  // Layer 1 must still exist (not empty)
  CHECK(layerP.layers[1] != nullptr);
  CHECK_EQ(f.lm.getSelectedLayer(), 1u);
}
