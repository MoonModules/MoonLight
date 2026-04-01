/**
    @title     MoonLight
    @file      LayerManager.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.

    Manages multi-layer selection, persistence, and lifecycle for virtual layers.
    Used by ModuleEffects to keep layer logic separate from effect-specific code.
**/

#pragma once

#if FT_MOONLIGHT

  #include <ArduinoJson.h>
  #include "MoonBase/utilities/Char.h"

  #ifdef ARDUINO
    #include "MoonBase/Module.h"
    #include "MoonBase/NodeManager.h"
    #include "PhysicalLayer.h"
    extern TaskHandle_t effectTaskHandle;
  #endif

  /// Copy data["nodes"] → data["nodes_<layer>"]. Called when leaving a layer.
  inline void layerStateSave(JsonObject data, uint8_t layer) {
    Char<16> key;
    key.format("nodes_%d", layer);
    data[key.c_str()].to<JsonArray>().set(data["nodes"].as<JsonArray>());
  }

  /// Copy data["nodes_<layer>"] → data["nodes"], or clear data["nodes"] if absent/empty.
  /// Called when entering a layer, or when restoring layer 0 after a preset load.
  inline void layerStateLoad(JsonObject data, uint8_t layer) {
    Char<16> key;
    key.format("nodes_%d", layer);
    if (!data[key.c_str()].isNull() && data[key.c_str()].as<JsonArray>().size() > 0) {
      data["nodes"].to<JsonArray>().set(data[key.c_str()].as<JsonArray>());
    } else {
      data["nodes"].to<JsonArray>();
    }
  }

  /// Remove the four per-layer state keys (nodes_N, start_N, end_N, brightness_N).
  /// Called when a layer is destroyed so compareRecursive treats the keys as absent.
  inline void layerStateClearKeys(JsonObject data, uint8_t layer) {
    Char<16> key;
    key.format("nodes_%d",      layer); data.remove(key.c_str());
    key.format("start_%d",      layer); data.remove(key.c_str());
    key.format("end_%d",        layer); data.remove(key.c_str());
    key.format("brightness_%d", layer); data.remove(key.c_str());
  }

class LayerManager {
  uint8_t selectedLayer = 0;
  bool needsRestore = false;

  // Stored references — set once via init(), valid for the lifetime of the owning module.
  ModuleState* state = nullptr;
  std::vector<Node*, VectorRAMAllocator<Node*>>** nodesPtr = nullptr;
  bool* requestUIUpdatePtr = nullptr;

 public:
  uint8_t getSelectedLayer() const { return selectedLayer; }

  /// Call once from Module::begin() before any other LayerManager methods.
  void init(ModuleState& moduleState, std::vector<Node*, VectorRAMAllocator<Node*>>*& nodes, bool& requestUIUpdate) {
    state = &moduleState;
    nodesPtr = &nodes;
    requestUIUpdatePtr = &requestUIUpdate;
  }

  /// Switch the active layer, swapping per-layer JSON state (nodes, start/end/brightness).
  void selectLayer(uint8_t index, bool swapState = true) {
    if (index >= layerP.layers.size()) return;

    if (swapState && !state->data["nodes"].isNull()) {
      layerStateSave(state->data, selectedLayer);  // save current layer's node state

      // save current layer's per-layer properties
      Char<16> key;
      VirtualLayer* curLayer = layerP.layers[selectedLayer];
      if (curLayer) {
        key.format("start_%d", selectedLayer);
        state->data[key.c_str()]["x"] = curLayer->startPct.x;
        state->data[key.c_str()]["y"] = curLayer->startPct.y;
        state->data[key.c_str()]["z"] = curLayer->startPct.z;
        key.format("end_%d", selectedLayer);
        state->data[key.c_str()]["x"] = curLayer->endPct.x;
        state->data[key.c_str()]["y"] = curLayer->endPct.y;
        state->data[key.c_str()]["z"] = curLayer->endPct.z;
        key.format("brightness_%d", selectedLayer);
        state->data[key.c_str()] = curLayer->brightness;
      }

      layerStateLoad(state->data, index);  // load new layer's node state
    }

    selectedLayer = index;
    VirtualLayer* layer = layerP.ensureLayer(selectedLayer);
    if (!layer) return;  // allocation failed
    *nodesPtr = &(layer->nodes);
  }

  /// Call before a full state reload from the filesystem (preset switch).
  /// Destroys all non-selected VirtualLayers and clears their state keys so that compareRecursive
  /// sees a clean slate and restoreNonSelectedLayers can rebuild from the new preset.
  void prepareForPresetLoad() {
    // Suspend the effectTask while deleting VirtualLayer objects to prevent a
    // use-after-free: effectTask iterates layerP.layers without a lock, so it must
    // not run while we delete and null out entries.  vTaskSuspend/Resume costs zero
    // extra bytes (no new FreeRTOS object) and effectTask holds no mutex during loop().
    #ifdef ARDUINO
    vTaskSuspend(effectTaskHandle);
    #endif
    for (uint8_t i = 1; i < layerP.layers.size(); i++) {
      if (!layerP.layers[i]) continue;

      // destroy the VirtualLayer (destructor clears LEDs and deletes all nodes)
      delete layerP.layers[i];
      layerP.layers[i] = nullptr;
      if (layerP.activeLayerCount > 1) layerP.activeLayerCount--;

      // clear per-layer JSON state so compareRecursive treats these keys as absent
      layerStateClearKeys(state->data, i);
    }
    #ifdef ARDUINO
    vTaskResume(effectTaskHandle);
    #endif

    // if the selected layer was > 0 (unlikely but safe), fall back to layer 0
    if (selectedLayer > 0) {
      selectLayer(0, false);
      state->data["layer"] = 0;
      // reload layer 0's nodes into state->data["nodes"]: selectLayer(false) skips the state swap,
      // so without this compareRecursive would diff the new preset against the stale layer-N nodes.
      // If they matched, it would skip node recreation even though the nodes were already destroyed.
      layerStateLoad(state->data, 0);
    }

    // reset layer 0's bounds to defaults in both the VirtualLayer and state, so that old-style
    // presets that omit start/end/brightness get the defaults instead of stale values from the
    // previous preset (compareRecursive skips absent keys, so we must pre-clear them)
    if (layerP.layers[0]) {
      layerP.layers[0]->startPct = {0, 0, 0};
      layerP.layers[0]->endPct = {100, 100, 100};
      layerP.layers[0]->brightness = 255;
    }
    state->data.remove("start");
    state->data.remove("end");
    state->data.remove("brightness");

    // schedule restore so non-selected layers from the new preset are rebuilt after readFromFS
    needsRestore = true;
  }

  /// Schedule restoration of non-selected layers. Call from begin() after NodeManager::begin().
  void scheduleRestore() { needsRestore = true; }

  /// Call from loop20ms(). If a restore is pending (after FS state has been loaded), instantiate
  /// nodes for non-selected layers and apply their persisted bounds.
  void checkRestore(NodeManager& nm) {
    if (!needsRestore) return;
    needsRestore = false;
    restoreNonSelectedLayers(nm);
  }

  /// Called after a node is removed. If the current layer is empty, destroy it and switch to layer 0.
  void onNodeRemoved() {
    if (selectedLayer > 0 && layerP.layers[selectedLayer] && layerP.layers[selectedLayer]->nodes.empty()) {
      uint8_t destroyedLayer = selectedLayer;
      EXT_LOGD(ML_TAG, "Destroying empty VirtualLayer %d", destroyedLayer);
      #ifdef ARDUINO
      vTaskSuspend(effectTaskHandle);
      #endif
      delete layerP.layers[destroyedLayer];
      layerP.layers[destroyedLayer] = nullptr;
      layerP.activeLayerCount--;
      #ifdef ARDUINO
      vTaskResume(effectTaskHandle);
      #endif

      // clean up JSON state for the destroyed layer
      layerStateClearKeys(state->data, destroyedLayer);

      // switch UI back to layer 0: reload layer 0's nodes and properties from state
      selectLayer(0, false);
      state->data["layer"] = 0;

      // Reload layer 0's node array into state->data["nodes"] (selectLayer(false) skipped state swap)
      layerStateLoad(state->data, 0);

      // Reload layer 0's per-layer properties from state
      Char<16> key;
      VirtualLayer* layer0 = layerP.layers[0];
      if (layer0) {
        key.format("start_%d", 0);
        if (!state->data[key.c_str()].isNull()) {
          layer0->startPct = {state->data[key.c_str()]["x"].as<int>(), state->data[key.c_str()]["y"].as<int>(), state->data[key.c_str()]["z"].as<int>()};
        }
        key.format("end_%d", 0);
        if (!state->data[key.c_str()].isNull()) {
          layer0->endPct = {state->data[key.c_str()]["x"] | 100, state->data[key.c_str()]["y"] | 100, state->data[key.c_str()]["z"] | 100};
        }
        key.format("brightness_%d", 0);
        if (!state->data[key.c_str()].isNull()) {
          layer0->brightness = state->data[key.c_str()] | 255;
        }
      }

      layerP.requestMapVirtual = true;
      *requestUIUpdatePtr = true;
    }
  }

  /// Handle layer-related onUpdate events. Returns true if the event was consumed.
  bool handleUpdate(const UpdatedItem& updatedItem) {
    if (updatedItem.parent[0] != "") return false;

    if (updatedItem.name == "layer") {
      selectLayer(updatedItem.value.as<uint8_t>());
      *requestUIUpdatePtr = true;
      return true;
    }
    if (updatedItem.name == "brightness") {
      VirtualLayer* layer = layerP.ensureLayer(selectedLayer);
      if (layer) layer->brightness = updatedItem.value.as<uint8_t>();
      return true;
    }
    if (updatedItem.name == "start") {
      VirtualLayer* layer = layerP.ensureLayer(selectedLayer);
      if (!layer) return true;
      layer->startPct = {updatedItem.value["x"].as<int>(), updatedItem.value["y"].as<int>(), updatedItem.value["z"].as<int>()};
      layerP.requestMapVirtual = true;
      return true;
    }
    if (updatedItem.name == "end") {
      VirtualLayer* layer = layerP.ensureLayer(selectedLayer);
      if (!layer) return true;
      layer->endPct = {updatedItem.value["x"] | 100, updatedItem.value["y"] | 100, updatedItem.value["z"] | 100};
      layerP.requestMapVirtual = true;
      return true;
    }
    return false;
  }

  /// Install a readHook on the module state that injects per-layer bounds into the JSON sent to the UI.
  void installReadHook() {
    state->readHook = [this](JsonObject data) {
      VirtualLayer* layer = layerP.ensureLayer(selectedLayer);
      if (!layer) return;
      data["start"]["x"] = layer->startPct.x;
      data["start"]["y"] = layer->startPct.y;
      data["start"]["z"] = layer->startPct.z;
      data["end"]["x"] = layer->endPct.x;
      data["end"]["y"] = layer->endPct.y;
      data["end"]["z"] = layer->endPct.z;
      data["brightness"] = layer->brightness;
    };
  }

  /// Add layer selection dropdown and per-layer bound controls to setupDefinition.
  #ifdef ARDUINO // Because addLayerControls takes a Module& parameter, and Module is only defined when #include "MoonBase/Module.h" is processed — which is guarded by #ifdef ARDUINO. Without the method-level guard, native compilation fails with "unknown type name 'Module'". It's a consequence of the include guard, not a deliberate design choice. It's not testable anyway (it's pure UI setup), so the guard is harmless. If you'd prefer to avoid it, the alternative is to add a Module stub to the test stubs — but Module is complex enough that guarding the one method is simpler.
  static void addLayerControls(Module& module, const JsonArray& controls) {
    JsonObject control = module.addControl(controls, "layer", "select");
    control["default"] = 0;
    // Find highest active index so that a "add new" slot always appears beyond it,
    // even when middle layers have been deleted and activeLayerCount has holes.
    uint8_t highestActive = 0;
    for (uint8_t i = 0; i < layerP.layers.size(); i++) {
      if (layerP.layers[i]) highestActive = i;
    }
    for (uint8_t i = 0; i <= highestActive + 1 && i < layerP.layers.size(); i++) {
      Char<12> layerName;
      layerName.format("Layer %d", i + 1);
      module.addControlValue(control, layerName.c_str());
    }

    control = module.addControl(controls, "start", "coord3D", 0, 100, false, "%");
    control["default"]["x"] = 0; control["default"]["y"] = 0; control["default"]["z"] = 0;
    control = module.addControl(controls, "end", "coord3D", 0, 100, false, "%");
    control["default"]["x"] = 100; control["default"]["y"] = 100; control["default"]["z"] = 100;
    control = module.addControl(controls, "brightness", "slider", 0, 255);
    control["default"] = 255;
  }
  #endif  // ARDUINO

 private:
  /// Instantiate nodes for non-selected layers and restore their per-layer bounds from JSON state.
  void restoreNonSelectedLayers(NodeManager& nm) {
    uint8_t savedSelectedLayer = selectedLayer;
    Char<16> key;
    bool restoredAny = false;

    // Migrate old-format state: old firmware stored 'end' as pixel coordinates (e.g. {16,16,1}
    // for a 16×16 panel) rather than percentages. New firmware uses per-layer keys (end_0, …).
    // If end_0 is absent, reset layer 0 bounds to defaults now — after all deferred compareRecursive
    // updates have settled — so the stale pixel values cannot win by arriving after scheduleRestore().
    key.format("end_%d", savedSelectedLayer);
    if (state->data[key.c_str()].isNull() && layerP.layers[savedSelectedLayer]) {
      layerP.layers[savedSelectedLayer]->startPct  = {0, 0, 0};
      layerP.layers[savedSelectedLayer]->endPct    = {100, 100, 100};
      layerP.layers[savedSelectedLayer]->brightness = 255;
      state->data.remove("start");
      state->data.remove("end");
      state->data.remove("brightness");
      EXT_LOGD(ML_TAG, "Migrated old-format state: reset layer %d bounds to defaults", savedSelectedLayer);
    }

    // Suspend effectTask while adding nodes to non-selected layers: ensureLayer() makes the
    // layer non-null in layerP.layers immediately, so Core 0 would see it and iterate
    // layer->nodes via VirtualLayer::loop() concurrently with our push_back calls here.
    // A push_back that triggers reallocation while Core 0 holds an iterator is a use-after-free.
    // effectTask holds no mutex during loop(), so suspend/resume is safe (same pattern as
    // prepareForPresetLoad and onNodeRemoved).
    #ifdef ARDUINO
    vTaskSuspend(effectTaskHandle);
    #endif
    for (uint8_t i = 0; i < layerP.layers.size(); i++) {
      if (i == savedSelectedLayer) continue;

      key.format("nodes_%d", i);
      JsonArray layerNodes = state->data[key.c_str()];
      if (layerNodes.isNull() || layerNodes.size() == 0) continue;

      VirtualLayer* layer = layerP.ensureLayer(i);
      if (!layer) { EXT_LOGW(ML_TAG, "ensureLayer(%d) failed, skipping", i); continue; }
      selectedLayer = i;
      *nodesPtr = &(layer->nodes);

      for (uint8_t j = 0; j < layerNodes.size(); j++) {
        JsonObject nodeState = layerNodes[j];
        if (nodeState["name"].isNull()) continue;
        char name[32];
        strlcpy(name, nodeState["name"].as<const char*>(), 32);
        Node* node = nm.addNode(j, name, nodeState["controls"]);
        if (node) {
          node->on = nodeState["on"];
          node->requestMappings();
        }
      }

      // restore per-layer bounds
      key.format("start_%d", i);
      if (!state->data[key.c_str()].isNull()) {
        layer->startPct = {state->data[key.c_str()]["x"].as<int>(), state->data[key.c_str()]["y"].as<int>(), state->data[key.c_str()]["z"].as<int>()};
      }
      key.format("end_%d", i);
      if (!state->data[key.c_str()].isNull()) {
        layer->endPct = {state->data[key.c_str()]["x"] | 100, state->data[key.c_str()]["y"] | 100, state->data[key.c_str()]["z"] | 100};
      }
      key.format("brightness_%d", i);
      if (!state->data[key.c_str()].isNull()) {
        layer->brightness = state->data[key.c_str()] | 255;
      }

      EXT_LOGD(ML_TAG, "Restored layer %d: %d nodes, start:%d,%d,%d end:%d,%d,%d brightness:%d",
               i, layer->nodes.size(), layer->startPct.x, layer->startPct.y, layer->startPct.z,
               layer->endPct.x, layer->endPct.y, layer->endPct.z, layer->brightness);
      restoredAny = true;
    }
    #ifdef ARDUINO
    vTaskResume(effectTaskHandle);
    #endif

    selectedLayer = savedSelectedLayer;
    VirtualLayer* layer = layerP.layers[selectedLayer];
    if (layer) *nodesPtr = &(layer->nodes);


    if (restoredAny) layerP.requestMapVirtual = true;
  }
};

#endif  // FT_MOONLIGHT
