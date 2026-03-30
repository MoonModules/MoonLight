/**
    @title     MoonLight
    @file      NodeManager.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/develop/nodes/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef NodeManager_h
#define NodeManager_h

#if FT_MOONLIGHT

  #include "MoonBase/Module.h"
  #include "MoonBase/Modules/FileManager.h"
  #include "Nodes.h"  //Nodes.h will include VirtualLayer.h which will include PhysicalLayer.h
  #if FT_LIVESCRIPT
    #include "LiveScriptNode.h"
  #endif

/// Manages a list of Node instances (effects, drivers, layouts, or modifiers) and their lifecycle.
/// Handles node creation/destruction, control updates, and row reordering via the generic module UI.
class NodeManager : public Module {
 public:
  /// Default node name shown in the "add node" dropdown (e.g. "Glow" for effects).
  Char<32> defaultNodeName;

 protected:
  /// File manager for detecting config file changes on the filesystem.
  FileManager* _fileManager;

  /// The live list of Node instances managed by this module. Allocated with PSRAM-aware allocator.
  std::vector<Node*, VectorRAMAllocator<Node*>>* nodes = nullptr;

  NodeManager(const char* moduleName, PsychicHttpServer* server, ESP32SvelteKit* sveltekit, FileManager* fileManager);

  /// Registers file-change handler and calls Module::begin(). Subclasses must call NodeManager::begin().
  void begin() override;

  /// Processes deferred LiveScript compilations. Subclasses must call NodeManager::loop20ms().
  void loop20ms() override;

  /// Override to populate the node name dropdown with addNodeValue() calls.
  virtual void addNodes(const JsonObject& control) {}

  /// Adds a node entry as {name, category} object to the control's values array.
  template <typename T>
  void addNodeValue(const JsonObject& control) {
    if (control["values"].isNull()) control["values"].to<JsonArray>();
    JsonArray values = control["values"];
    JsonObject entry = values.add<JsonObject>();
    entry["name"] = getNameAndTags<T>();
    entry["category"] = T::category();
  }

 public:
  /// Factory method: creates the correct Node subclass for the given name. Returns nullptr if unknown.
  virtual Node* addNode(const uint8_t index, char* name, const JsonArray& controls) const { return nullptr; }

 protected:

  /// Checks if name matches T::name() (case/symbol insensitive) and allocates a T if so.
  template <typename T>
  Node* checkAndAlloc(char* name) const {
    if (equalAZaz09(name, T::name())) {
      strlcpy(name, getNameAndTags<T>().c_str(), 32);  // if the non AZaz09 part of the name changed, reassign the right name
      return allocMBObject<T>();
    } else
      return nullptr;
  }

  /// Defines the data model: nodes array with name, on/off, and controls sub-rows.
  void setupDefinition(const JsonArray& controls) override;

  /// Dispatches control updates to the appropriate handler based on parent/name context.
  void onUpdate(const UpdatedItem& updatedItem) override;

  /// Swaps two nodes in the nodes vector and requests remapping.
  void onReOrderSwap(uint8_t stateIndex, uint8_t newIndex) override;

 private:
  /// Handles nodes[i].name changes: creates/destroys nodes, manages controls, applies migrations.
  // Migration note (20251204): hardcoded renames for legacy driver names ("Physical Driver" → ParallelLEDDriver,
  // "IR Driver" → IRDriver). When adding new migrations, follow the same pattern with contains() + getNameAndTags<T>().
  void handleNodeNameChange(const UpdatedItem& updatedItem, JsonVariant nodeState);

  /// Handles nodes[i].on changes: toggles node on/off state and calls node's onUpdate.
  void handleNodeOnChange(const UpdatedItem& updatedItem, JsonVariant nodeState);

  /// Handles nodes[i].controls[j].value changes: updates the control value and calls node's onUpdate.
  void handleNodeControlValueChange(const UpdatedItem& updatedItem, JsonVariant nodeState);

 protected:
  /// Called after a node is removed from the nodes list. Override to handle cleanup (e.g. destroying empty layers).
  virtual void onNodeRemoved() {}

  /// Called just before a full state reload from the filesystem (e.g. preset switch).
  /// Override to clean up any state that must be cleared before compareRecursive runs.
  virtual void onBeforeStateLoad() {}

 public:
  #if FT_LIVESCRIPT
  /// Finds a LiveScript node by its animation file name. Returns nullptr if not found.
  Node* findLiveScriptNode(const char* animation);
  #endif
};

#endif
#endif
