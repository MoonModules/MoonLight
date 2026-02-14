/**
    @title     MoonLight
    @file      ModuleDrivers.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/drivers/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleDrivers_h
#define ModuleDrivers_h

#if FT_MOONLIGHT

  #include "FastLED.h"
  #include "MoonBase/NodeManager.h"
  #include "MoonLight/Modules/ModuleLightsControl.h"

// #include "Nodes.h" //Nodes.h will include VirtualLayer.h which will include PhysicalLayer.h

class ModuleDrivers : public NodeManager {
 public:
  ModuleLightsControl* _moduleLightsControl;
  ModuleIO* _moduleIO;
  ModuleDrivers(PsychicHttpServer* server, ESP32SvelteKit* sveltekit, FileManager* fileManager, ModuleLightsControl* moduleLightsControl, ModuleIO* moduleIO) : NodeManager("drivers", server, sveltekit, fileManager) {
    _moduleLightsControl = moduleLightsControl;
    _moduleIO = moduleIO;
    EXT_LOGV(ML_TAG, "constructor");
  }

  void readPins() {
    if (safeModeMB) {
      EXT_LOGW(ML_TAG, "Safe mode enabled, not adding pins");
      return;
    }

    _moduleIO->read(
        [&](ModuleState& state) {
          // find the pins in board definitions

          memset(layerP.ledPins, UINT8_MAX, sizeof(layerP.ledPins));

          layerP.maxPower = state.data["maxPower"];
          EXT_LOGD(ML_TAG, "maxPower %d", layerP.maxPower);

          // assign pins (valid only)
          for (JsonObject pinObject : state.data["pins"].as<JsonArray>()) {
            uint8_t usage = pinObject["usage"];
            uint8_t index = pinObject["index"];
            uint8_t gpio = pinObject["GPIO"];
            if (usage == pin_LED && index >= 1 && index <= 20 && GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
              layerP.ledPins[index - 1] = gpio;
            }
          }

          // Remove all UINT8_MAX values by compacting the array
          layerP.nrOfLedPins = 0;
          for (int readPos = 0; readPos < sizeof(layerP.ledPins); readPos++) {
            if (layerP.ledPins[readPos] != UINT8_MAX) {  // only pins which have a nrOfLedPins // && layerP.ledsPerPin[layerP.nrOfLedPins] != UINT16_MAX && layerP.ledsPerPin[layerP.nrOfLedPins] != 0
              layerP.ledPins[layerP.nrOfLedPins++] = layerP.ledPins[readPos];
            }
          }

          // log pins
          for (int i = 0; i < layerP.nrOfLedPins; i++) {
            if (layerP.ledsPerPin[i] > 0 && layerP.ledsPerPin[i] != UINT16_MAX) EXT_LOGD(ML_TAG, "ledPins[%d-%d] = %d (#%d)", i, layerP.nrOfLedPins, layerP.ledPins[i], layerP.ledsPerPin[i]);
          }

          layerP.requestMapPhysical = true;
          layerP.requestMapVirtual = true;
        },
        _moduleName);
  }

  void begin() override {
    defaultNodeName = "";  // getNameAndTags<PanelLayout>();
    nodes = &layerP.nodes;
    NodeManager::begin();

    _moduleIO->addUpdateHandler([this](const String& originId) { readPins(); }, false);
  }

  void addNodes(const JsonObject& control) override {
    // Layouts, Most used first
    addControlValue(control, getNameAndTags<PanelLayout>());
    addControlValue(control, getNameAndTags<PanelsLayout>());
    addControlValue(control, getNameAndTags<CubeLayout>());
    addControlValue(control, getNameAndTags<HumanSizedCubeLayout>());
    addControlValue(control, getNameAndTags<TorontoBarGourdsLayout>());
    addControlValue(control, getNameAndTags<RingLayout>());
    addControlValue(control, getNameAndTags<Rings16Layout>());
    addControlValue(control, getNameAndTags<Rings241Layout>());
    addControlValue(control, getNameAndTags<CarLightsLayout>());
    addControlValue(control, getNameAndTags<WheelLayout>());
    addControlValue(control, getNameAndTags<SpiralLayout>());
    addControlValue(control, getNameAndTags<SingleRowLayout>());
    addControlValue(control, getNameAndTags<SingleColumnLayout>());

    // Drivers, Most used first
    addControlValue(control, getNameAndTags<ParallelLEDDriver>());
    addControlValue(control, getNameAndTags<FastLEDDriver>());
    addControlValue(control, getNameAndTags<ArtNetInDriver>());
    addControlValue(control, getNameAndTags<ArtNetOutDriver>());
    addControlValue(control, getNameAndTags<AudioSyncDriver>());
    addControlValue(control, getNameAndTags<IRDriver>());
    addControlValue(control, getNameAndTags<IMUDriver>());
    addControlValue(control, getNameAndTags<HUB75Driver>());

    // board preset specific
    _moduleIO->read(
        [&](ModuleState& state) {
          uint8_t boardPreset = state.data["boardPreset"];
          if (boardPreset == board_SE16V1) addControlValue(control, getNameAndTags<SE16Layout>());
          if (boardPreset == board_LightCrafter16) addControlValue(control, getNameAndTags<LightCrafter16Layout>());
        },
        _moduleName);
  }

  Node* addNode(const uint8_t index, char* name, const JsonArray& controls) const override {
    Node* node = nullptr;

    // Layouts, most used first
    if (!node) node = checkAndAlloc<PanelLayout>(name);
    if (!node) node = checkAndAlloc<PanelsLayout>(name);
    if (!node) node = checkAndAlloc<CubeLayout>(name);
    if (!node) node = checkAndAlloc<RingLayout>(name);
    if (!node) node = checkAndAlloc<Rings16Layout>(name);
    if (!node) node = checkAndAlloc<Rings241Layout>(name);
    if (!node) node = checkAndAlloc<CarLightsLayout>(name);
    if (!node) node = checkAndAlloc<WheelLayout>(name);
    if (!node) node = checkAndAlloc<SpiralLayout>(name);
    if (!node) node = checkAndAlloc<HumanSizedCubeLayout>(name);
    if (!node) node = checkAndAlloc<TorontoBarGourdsLayout>(name);
    if (!node) node = checkAndAlloc<SingleRowLayout>(name);
    if (!node) node = checkAndAlloc<SingleColumnLayout>(name);

    // Drivers most used first
    if (!node) node = checkAndAlloc<ParallelLEDDriver>(name);
    if (!node) node = checkAndAlloc<FastLEDDriver>(name);
    if (!node) node = checkAndAlloc<ArtNetInDriver>(name);
    if (!node) node = checkAndAlloc<ArtNetOutDriver>(name);
    if (!node) node = checkAndAlloc<AudioSyncDriver>(name);
    if (!node) node = checkAndAlloc<IRDriver>(name);
    if (!node) node = checkAndAlloc<IMUDriver>(name);
    if (!node) node = checkAndAlloc<HUB75Driver>(name);

    // board preset specific
    _moduleIO->read(
        [&](ModuleState& state) {
          uint8_t boardPreset = state.data["boardPreset"];
          if (!node && boardPreset == board_SE16V1) node = checkAndAlloc<SE16Layout>(name);
          if (!node && boardPreset == board_LightCrafter16) node = checkAndAlloc<LightCrafter16Layout>(name);
        },
        _moduleName);

  #if FT_LIVESCRIPT
    if (!node) {
      LiveScriptNode* liveScriptNode = allocMBObject<LiveScriptNode>();
      liveScriptNode->animation = name;  // set the (file)name of the script
      node = liveScriptNode;
    }
  #endif

    if (node) {
      EXT_LOGI(ML_TAG, "Add %s (p:%p pr:%d)", name, node, isInPSRAM(node));

      node->constructor(layerP.layers[0], controls, &layerP.driversMutex);  // pass the layer to the node (C++ constructors are not inherited, so declare it as normal functions)
      node->moduleControl = _moduleLightsControl;                           // to access global lights control functions if needed
      node->moduleIO = _moduleIO;                                           // to get pin allocations
      node->moduleNodes = (Module*)this;                                    // to request UI update
      node->setup();                                                        // run the setup of the effect
      node->onSizeChanged(Coord3D());     // to init memory allocations
      // layers[0]->nodes.reserve(index+1);

      // from here it runs concurrently in the drivers task
      if (index >= layerP.nodes.size())
        layerP.nodes.push_back(node);
      else
        layerP.nodes[index] = node;  // add the node to the layer
    }

    EXT_LOGV(ML_TAG, "%s (s:%d p:%p)", name, layerP.nodes.size(), node);

    return node;
  }

  bool initPins = false;

  void loop() override {
    NodeManager::loop();

    if (!initPins) {
      readPins();  // initially
      initPins = true;
    }
  }

};  // class ModuleDrivers

#endif
#endif