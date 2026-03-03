/**
    @title     MoonBase
    @file      Module.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/develop/modules/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef Module_h
#define Module_h

#if FT_MOONBASE == 1

  #include <ESP32SvelteKit.h>
  #include "utilities/Utilities.h"

/// Tracks which control changed during a state update, including its parent context and old/new values.
// sizeof was 160 chars -> 80 -> 68 -> 88
struct UpdatedItem {
  Char<20> parent[2];  // 24 -> 2*20
  uint8_t index[2];    // 2*1
  Char<20> name;       // 16 -> 16 -> 20
  Char<20> oldValue;   // 32 -> 16 -> 20, smaller then 11 bytes mostly
  JsonVariant value;   // 8->16->4

  /// Initializes parent/index fields to empty/unset defaults.
  UpdatedItem() {
    parent[0] = "";  // will be checked in onUpdate
    parent[1] = "";
    index[0] = UINT8_MAX;
    index[1] = UINT8_MAX;
  }
};

typedef std::function<void(JsonObject data)> ReadHook;

/// Shared ArduinoJson document for all modules (saves RAM vs one doc per module).
extern JsonDocument* gModulesDoc;  // shared document for all modules, to save RAM

/// Holds the JSON state for a single module. Manages setup, comparison, and update dispatch.
class ModuleState {
 public:
  /// The module's live state object, stored inside the shared gModulesDoc.
  JsonObject data = JsonObject();  // isNull()

  /// Allocates a new JsonObject in the shared gModulesDoc (creates the doc on first use).
  ModuleState();

  /// Removes this module's data from the shared gModulesDoc.
  ~ModuleState();

  /// Callback to populate the control definition array; set by Module::begin().
  std::function<void(const JsonArray& controls)> setupDefinition = nullptr;

  /// Initializes state from the definition's defaults if no persisted data exists.
  void setupData();

  /// Recursively compares old and new state, updating stateData in-place and dispatching processUpdatedItem for each change.
  // called from ModuleState::update and ModuleState::setupData()
  bool compareRecursive(const JsonString& parent, const JsonVariant& oldData, const JsonVariant& newData, UpdatedItem& updatedItem, const String& originId, uint8_t depth = UINT8_MAX, uint8_t index = UINT8_MAX);

  /// Detects row-swap reorderings in arrays and dispatches processUpdatedItem with name="swap".
  // called from ModuleState::update
  bool checkReOrderSwap(const JsonString& parent, const JsonVariant& oldData, const JsonVariant& newData, UpdatedItem& updatedItem, const String& originId, uint8_t depth = UINT8_MAX, uint8_t index = UINT8_MAX);

  /// Callback invoked for each changed control; set by Module constructor to route to Module::processUpdatedItem.
  std::function<void(const UpdatedItem&, const String&)> processUpdatedItem = nullptr;

  /// Copies module state into stateJson for sending to the UI (calls readHook first if set).
  static void read(ModuleState& state, JsonObject& stateJson);

  /// Applies newData to the module state, detecting changes and dispatching updates. Returns CHANGED or UNCHANGED.
  static StateUpdateResult update(JsonObject& newData, ModuleState& state, const String& originId);  //, const String& originId

  /// Optional hook called before read() copies state, allowing modules to refresh dynamic data.
  ReadHook readHook = nullptr;  // called when the UI requests the state, can be used to update the state before sending it to the UI
};

/// Base class for all MoonBase modules. Provides lifecycle hooks, state management, and generic UI control registration.
class Module : public StatefulService<ModuleState> {
 public:
  /// Human-readable module name, used for REST/WS endpoints and logging.
  const char* _moduleName = "";

  /// Set to true to push current state to UI on next loop20ms() cycle.
  bool requestUIUpdate = false;

  Module(const char* moduleName, PsychicHttpServer* server, ESP32SvelteKit* sveltekit);

  /// Registers HTTP/WS endpoints and initializes state from definition or persisted file.
  // any Module that overrides begin() must continue to call Module::begin() (e.g., at the start of its own begin()
  virtual void begin();

  /// Called every SvelteKit loop iteration (fastest). Override for high-frequency polling.
  // run in sveltekit task
  virtual void loop() {}

  /// Called every 20ms. Flushes requestUIUpdate to the UI. Subclasses must call Module::loop20ms().
  virtual void loop20ms();

  /// Called every ~1 second. Override for periodic checks.
  virtual void loop1s() {}

  /// Called every ~10 seconds. Override for infrequent maintenance.
  virtual void loop10s() {}

  /// Routes an updated control to onUpdate() or onReOrderSwap(), and marks state for saving if from the UI.
  void processUpdatedItem(const UpdatedItem& updatedItem, const String& originId);

  /// Override to populate the control definition array with addControl() calls.
  virtual void setupDefinition(const JsonArray& controls);

  /// Adds a control definition to the array. Returns the new control object for further customization.
  JsonObject addControl(const JsonArray& controls, const char* name, const char* type, int min = 0, int max = UINT8_MAX, bool ro = false, const char* desc = nullptr);

  /// Appends a value to the control's "values" array (creates the array if needed).
  template <typename T>
  void addControlValue(const JsonObject& control, const T& value) {
    if (control["values"].isNull()) control["values"].to<JsonArray>();  // add array of values
    JsonArray values = control["values"];
    values.add(value);
  }

  /// Called when a non-swap control value changes. Override to react to specific control updates.
  // called in compareRecursive->execOnUpdate
  // called from compareRecursive
  virtual void onUpdate(const UpdatedItem& updatedItem, const String& originId) {};

  /// Called when rows are reordered via drag-and-drop. Override to update internal data structures.
  virtual void onReOrderSwap(uint8_t stateIndex, uint8_t newIndex) {};

  /// Reads the assigned GPIO pin for the given usage from ModuleIO state. Returns true if pin changed.
  bool updatePin(uint8_t& pin, const uint8_t pinUsage, bool checkOut = false);

 protected:

  /// HTTP server for registering REST endpoints. Protected so subclasses (e.g. NodeManager) can register additional routes.
  PsychicHttpServer* _server;
  ESP32SvelteKit* _sveltekit;
};

#endif
#endif
