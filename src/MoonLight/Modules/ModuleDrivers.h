/**
    @title     MoonLight
    @file      ModuleDrivers.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/drivers/
    @Copyright © 2026 GitHub MoonLight Commit Authors
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
  ModuleDrivers(PsychicHttpServer* server, ESP32SvelteKit* sveltekit, FileManager* fileManager, ModuleLightsControl* moduleLightsControl, ModuleIO* moduleIO) : NodeManager("drivers", server, sveltekit, fileManager) {
    _moduleLightsControl = moduleLightsControl;
    _moduleIO = moduleIO;

    _moduleIO->addUpdateHandler([this](const String& originId) { readPins(); }, false);
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

          // UART0 TX/RX pins used as LED pin: disable all UART0 output
          // to prevent the I2S LED driver and UART driver from fighting over the pin,
          // which causes serial noise and eventual watchdog crash.
          // TX/RX are board-specific (ESP32: 1/3, S3: 43/44, C3: 21/20, P4: 37/38).
          // Note: using UART0 pins as LED pins is discouraged — you lose all serial debugging.
          {
            bool needsUartSuppression = false;
            for (int i = 0; i < layerP.nrOfLedPins; i++) {
              if (layerP.ledPins[i] == TX || layerP.ledPins[i] == RX) {
                needsUartSuppression = true;
                break;
              }
            }
            if (needsUartSuppression && !_uartSuppressed) {
              EXT_LOGW(ML_TAG, "UART0 TX/RX (GPIO %d/%d) used as LED pin — disabling UART0 to prevent crash (serial debug lost!)", TX, RX);
              _origVprintf = esp_log_set_vprintf([](const char*, va_list) -> int { return 0; });
              Serial.end();
              _uartSuppressed = true;
            } else if (!needsUartSuppression && _uartSuppressed) {
              Serial.begin(SERIAL_BAUD_RATE);
              esp_log_set_vprintf(_origVprintf);
              _uartSuppressed = false;
              EXT_LOGI(ML_TAG, "UART0 TX/RX (GPIO %d/%d) no longer LED pins — UART0 and logging restored", TX, RX);
            }
          }

          // log pins
          for (int i = 0; i < layerP.nrOfLedPins; i++) {
            if (layerP.ledsPerPin[i] > 0 && layerP.ledsPerPin[i] != UINT16_MAX) EXT_LOGD(ML_TAG, "ledPins[%d-%d] = %d (#%d)", i, layerP.nrOfLedPins, layerP.ledPins[i], layerP.ledsPerPin[i]);
          }

          layerP.requestMapPhysical = true;
        },
        _moduleName);
  }  // readPins

  void begin() override {
    defaultNodeName = "";  // getNameAndTags<PanelLayout>();
    nodes = &layerP.nodes;
    NodeManager::begin();
  }

  void addNodes(const JsonObject& control) override {
    // Layouts, Most used first
    addNodeValue<PanelLayout>(control);
    addNodeValue<PanelsLayout>(control);
    addNodeValue<CubeLayout>(control);
    addNodeValue<HumanSizedCubeLayout>(control);
    addNodeValue<TorontoBarGourdsLayout>(control);
    addNodeValue<RingLayout>(control);
    addNodeValue<Rings16Layout>(control);
    addNodeValue<Rings241Layout>(control);
    addNodeValue<CarLightsLayout>(control);
    addNodeValue<WheelLayout>(control);
    addNodeValue<SpiralLayout>(control);
    addNodeValue<SingleRowLayout>(control);
    addNodeValue<SingleColumnLayout>(control);
    addNodeValue<TubesLayout>(control);

    // Drivers, Most used first
    addNodeValue<ParallelLEDDriver>(control);
    addNodeValue<FastLEDDriver>(control);
    addNodeValue<FastLEDAudioDriver>(control);
    addNodeValue<NetworkInDriver>(control);
    addNodeValue<NetworkOutDriver>(control);
    addNodeValue<DMXInDriver>(control);
    addNodeValue<DMXOutDriver>(control);
    addNodeValue<WLEDAudioDriver>(control);
    addNodeValue<IRDriver>(control);
    addNodeValue<IMUDriver>(control);
    // addNodeValue<HUB75Driver>(control);

    // board preset specific
    _moduleIO->read(
        [&](ModuleState& state) {
          const char* boardPreset = state.data["boardPreset"] | "";
          if (strcmp(boardPreset, BoardName::SE16V1) == 0) addNodeValue<SE16Layout>(control);
          if (strcmp(boardPreset, BoardName::LightCrafter16) == 0) addNodeValue<LightCrafter16Layout>(control);
        },
        _moduleName);

  #if FT_LIVESCRIPT
    // find layout/driver live scripts (.sc files with L_ or D_ prefix) on FS
    File rootFolder = ESPFS.open("/");
    walkThroughFiles(rootFolder, [&](File folder, File file) {
      const char* fname = file.name();
      size_t len = strlen(fname);
      bool isSc = (len >= 3) && strcmp(fname + (len - 3), ".sc") == 0;
      if (isSc && (strncmp(fname, "L_", 2) == 0 || strncmp(fname, "D_", 2) == 0)) {
        if (control["values"].isNull()) control["values"].to<JsonArray>();
        JsonObject entry = control["values"].as<JsonArray>().add<JsonObject>();
        entry["name"] = (const char*)file.path();
        entry["category"] = "LiveScript";
      }
    });
    rootFolder.close();
  #endif
  }

  Node* addNode(const uint8_t index, char* name, const JsonArray& controls) const override {
    Node* node = nullptr;

    // Layouts, most used first
    // cppcheck-suppress knownConditionTrueFalse -- intentional: chain tries each type in order; first check is always true after node=nullptr
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
    if (!node) node = checkAndAlloc<TubesLayout>(name);

    // Drivers most used first
    if (!node) node = checkAndAlloc<ParallelLEDDriver>(name);
    if (!node) node = checkAndAlloc<FastLEDDriver>(name);
    if (!node) node = checkAndAlloc<FastLEDAudioDriver>(name);
    if (!node) node = checkAndAlloc<NetworkInDriver>(name);
    if (!node) node = checkAndAlloc<NetworkOutDriver>(name);
    // Migration: Art-Net In / Art-Net Out were renamed to Network In / Network Out.
    // equalAZaz09 strips punctuation, so "ArtNetIn" != "NetworkIn" — handle explicitly.
    if (!node && equalAZaz09(name, "ArtNetIn"))  { strlcpy(name, NetworkInDriver::name(),  32); node = allocMBObject<NetworkInDriver>();  }
    if (!node && equalAZaz09(name, "ArtNetOut")) { strlcpy(name, NetworkOutDriver::name(), 32); node = allocMBObject<NetworkOutDriver>(); }
    if (!node) node = checkAndAlloc<DMXInDriver>(name);
    if (!node) node = checkAndAlloc<DMXOutDriver>(name);
    if (!node) node = checkAndAlloc<WLEDAudioDriver>(name);
    // Migration: Audio Sync was renamed to WLED Audio.
    if (!node && equalAZaz09(name, "AudioSync")) { strlcpy(name, WLEDAudioDriver::name(), 32); node = allocMBObject<WLEDAudioDriver>(); }
    if (!node) node = checkAndAlloc<IRDriver>(name);
    if (!node) node = checkAndAlloc<IMUDriver>(name);
    // if (!node) node = checkAndAlloc<HUB75Driver>(name);

    // board preset specific
    _moduleIO->read(
        [&](ModuleState& state) {
          const char* boardPreset = state.data["boardPreset"] | "";
          if (!node && strcmp(boardPreset, BoardName::SE16V1) == 0) node = checkAndAlloc<SE16Layout>(name);
          if (!node && strcmp(boardPreset, BoardName::LightCrafter16) == 0) node = checkAndAlloc<LightCrafter16Layout>(name);
        },
        _moduleName);

  #if FT_LIVESCRIPT
    if (!node && !safeModeMB) {
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
      node->moduleNodes = (Module*)this;                                    // cppcheck-suppress dangerousTypeCast -- upcast; to request UI update
      node->setup();                                                        // run the setup of the effect
      node->onSizeChanged(Coord3D());                                     // to init memory allocations
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

 private:
  ModuleLightsControl* _moduleLightsControl;
  ModuleIO* _moduleIO;
  bool _uartSuppressed = false;
  vprintf_like_t _origVprintf = nullptr;

};  // class ModuleDrivers

#endif
#endif