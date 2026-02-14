/**
    @title     MoonLight
    @file      ModuleEffects.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/effects/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleEffects_h
#define ModuleEffects_h

#if FT_MOONLIGHT

  #include "FastLED.h"
  #include "MoonBase/NodeManager.h"
  #include "MoonLight/Modules/ModuleLightsControl.h"

// #include "MoonBase/Nodes.h" //Nodes.h will include VirtualLayer.h which will include PhysicalLayer.h

class ModuleEffects : public NodeManager {
 public:
  ModuleLightsControl* _moduleLightsControl;

  ModuleEffects(PsychicHttpServer* server, ESP32SvelteKit* sveltekit, FileManager* fileManager, ModuleLightsControl* moduleLightsControl) : NodeManager("effects", server, sveltekit, fileManager) {
    EXT_LOGV(ML_TAG, "constructor");
    _moduleLightsControl = moduleLightsControl;
  }

  void begin() override {
    defaultNodeName = getNameAndTags<RandomEffect>();
    nodes = &(layerP.layers[0]->nodes);  // to do add nodes from all layers...
    NodeManager::begin();

  #if FT_ENABLED(FT_MONITOR)
    _socket->registerEvent("monitor");
    _server->on("/rest/monitorLayout", HTTP_GET, [&](PsychicRequest* request) {
      EXT_LOGV(ML_TAG, "rest monitor triggered");

      // trigger pass 1 mapping of layout
      layerP.pass = 1;  //(requestMapPhysical=1 physical rerun)
      layerP.monitorPass = true;
      layerP.mapLayout();
      layerP.monitorPass = false;

      PsychicJsonResponse response = PsychicJsonResponse(request, false);
      return response.send();
    });
  #endif

    _state.readHook = [&](JsonObject data) {
      data["start"]["x"] = 0;
      data["start"]["y"] = 0;
      data["start"]["z"] = 0;
      data["end"]["x"] = layerP.lights.header.size.x;
      data["end"]["y"] = layerP.lights.header.size.y;
      data["end"]["z"] = layerP.lights.header.size.z;
      data["brightness"] = layerP.lights.header.brightness;
    };
  }

  void setupDefinition(const JsonArray& controls) override {
    EXT_LOGV(ML_TAG, "");
    JsonObject control;  // state.data has one or more properties
    control = addControl(controls, "layer", "select");
    control["default"] = 0;  // the first entry has index 0 and refers to Layer 1 (layer counting starts with 1)
    uint8_t i = 1;           // start with one
    for (VirtualLayer* layer : layerP.layers) {
      Char<12> layerName;
      layerName.format("Layer %d", i);
      addControlValue(control, layerName.c_str());
      i++;
    }

    addControl(controls, "start", "coord3D", 0, UINT16_MAX, true);
    addControl(controls, "end", "coord3D", 0, UINT16_MAX, true);
    addControl(controls, "brightness", "slider", 0, UINT8_MAX, true);

    NodeManager::setupDefinition(controls);
  }

  void addNodes(const JsonObject& control) override {
    // keep the order the same as in https://moonmodules.org/MoonLight/moonlight/effects

    // MoonLight effects, Solid first then alphabetically
    addControlValue(control, getNameAndTags<SolidEffect>());
    addControlValue(control, getNameAndTags<AudioRingsEffect>());
    addControlValue(control, getNameAndTags<LinesEffect>());
    addControlValue(control, getNameAndTags<FireEffect>());
    addControlValue(control, getNameAndTags<FixedRectangleEffect>());
    addControlValue(control, getNameAndTags<ParticlesEffect>());
    addControlValue(control, getNameAndTags<PraxisEffect>());
    addControlValue(control, getNameAndTags<StarSkyEffect>());
  #if USE_M5UNIFIED
    addControlValue(control, getNameAndTags<MoonManEffect>());
  #endif
    addControlValue(control, getNameAndTags<FreqSawsEffect>());
    addControlValue(control, getNameAndTags<MarioTestEffect>());
    addControlValue(control, getNameAndTags<PixelMapEffect>());
    addControlValue(control, getNameAndTags<RandomEffect>());
    addControlValue(control, getNameAndTags<RingRandomFlowEffect>());
    addControlValue(control, getNameAndTags<RipplesEffect>());
    addControlValue(control, getNameAndTags<RubiksCubeEffect>());
    addControlValue(control, getNameAndTags<ScrollingTextEffect>());
    addControlValue(control, getNameAndTags<SinusEffect>());
    addControlValue(control, getNameAndTags<SphereMoveEffect>());
    addControlValue(control, getNameAndTags<SpiralFireEffect>());
    addControlValue(control, getNameAndTags<StarFieldEffect>());
    addControlValue(control, getNameAndTags<VUMeterEffect>());
    addControlValue(control, getNameAndTags<WaveEffect>());

    // MoonModules effects, alphabetically
    addControlValue(control, getNameAndTags<GameOfLifeEffect>());
    addControlValue(control, getNameAndTags<GEQ3DEffect>());
    addControlValue(control, getNameAndTags<PaintBrushEffect>());

    // WLED effects, alphabetically
    addControlValue(control, getNameAndTags<BlackholeEffect>());
    addControlValue(control, getNameAndTags<BouncingBallsEffect>());
    addControlValue(control, getNameAndTags<BlurzEffect>());
    addControlValue(control, getNameAndTags<ColorTwinkleEffect>());
    addControlValue(control, getNameAndTags<DistortionWavesEffect>());
    addControlValue(control, getNameAndTags<DJLightEffect>());
    addControlValue(control, getNameAndTags<DNAEffect>());
    addControlValue(control, getNameAndTags<DripEffect>());
    addControlValue(control, getNameAndTags<FreqMatrixEffect>());
    addControlValue(control, getNameAndTags<FireworksEffect>());
    addControlValue(control, getNameAndTags<FlowEffect>());
    addControlValue(control, getNameAndTags<FrizzlesEffect>());
    addControlValue(control, getNameAndTags<FunkyPlankEffect>());
    addControlValue(control, getNameAndTags<GEQEffect>());
    addControlValue(control, getNameAndTags<HeartBeatEffect>());
    addControlValue(control, getNameAndTags<JuliaEffect>());
    addControlValue(control, getNameAndTags<LissajousEffect>());
    addControlValue(control, getNameAndTags<Noise2DEffect>());
    addControlValue(control, getNameAndTags<NoiseMeterEffect>());
    addControlValue(control, getNameAndTags<OctopusEffect>());
    addControlValue(control, getNameAndTags<PacManEffect>());
    addControlValue(control, getNameAndTags<PlasmaEffect>());
    addControlValue(control, getNameAndTags<PoliceEffect>());
    addControlValue(control, getNameAndTags<PopCornEffect>());
    addControlValue(control, getNameAndTags<RainEffect>());
    addControlValue(control, getNameAndTags<TetrixEffect>());
    addControlValue(control, getNameAndTags<WaverlyEffect>());

    // FastLED effects
    addControlValue(control, getNameAndTags<RainbowEffect>());

    // Moving head effects, alphabetically
    addControlValue(control, getNameAndTags<AmbientMoveEffect>());
    addControlValue(control, getNameAndTags<FreqColorsEffect>());
    addControlValue(control, getNameAndTags<Troy1ColorEffect>());
    addControlValue(control, getNameAndTags<Troy1MoveEffect>());
    addControlValue(control, getNameAndTags<Troy2ColorEffect>());
    addControlValue(control, getNameAndTags<Troy2MoveEffect>());
    addControlValue(control, getNameAndTags<WowiMoveEffect>());

    // Modifiers, most used first
    addControlValue(control, getNameAndTags<MultiplyModifier>());
    addControlValue(control, getNameAndTags<MirrorModifier>());
    addControlValue(control, getNameAndTags<TransposeModifier>());
    addControlValue(control, getNameAndTags<CircleModifier>());
    addControlValue(control, getNameAndTags<RotateModifier>());
    addControlValue(control, getNameAndTags<CheckerboardModifier>());
    addControlValue(control, getNameAndTags<PinwheelModifier>());
    addControlValue(control, getNameAndTags<RippleXZModifier>());

    // find all the .sc files on FS
    File rootFolder = ESPFS.open("/");
    walkThroughFiles(rootFolder, [&](File folder, File file) {
      if (strstr(file.name(), ".sc")) {
        // EXT_LOGV(ML_TAG, "found file %s", file.path());
        addControlValue(control, (char*)file.path());
      }
    });
    rootFolder.close();
  }

  Node* addNode(const uint8_t index, char* name, const JsonArray& controls) const override {
    Node* node = nullptr;

    // MoonLight effects, Solid first then alphabetically
    if (!node) node = checkAndAlloc<SolidEffect>(name);
    if (!node) node = checkAndAlloc<AudioRingsEffect>(name);
    if (!node) node = checkAndAlloc<FireEffect>(name);
    if (!node) node = checkAndAlloc<FixedRectangleEffect>(name);
    if (!node) node = checkAndAlloc<FreqSawsEffect>(name);
    if (!node) node = checkAndAlloc<LinesEffect>(name);
    if (!node) node = checkAndAlloc<MarioTestEffect>(name);
    if (!node) node = checkAndAlloc<StarSkyEffect>(name);
  #if USE_M5UNIFIED
    if (!node) node = checkAndAlloc<MoonManEffect>(name);
  #endif
    if (!node) node = checkAndAlloc<ParticlesEffect>(name);
    if (!node) node = checkAndAlloc<PraxisEffect>(name);
    if (!node) node = checkAndAlloc<PixelMapEffect>(name);
    if (!node) node = checkAndAlloc<RandomEffect>(name);
    if (!node) node = checkAndAlloc<RingRandomFlowEffect>(name);
    if (!node) node = checkAndAlloc<RipplesEffect>(name);
    if (!node) node = checkAndAlloc<RubiksCubeEffect>(name);
    if (!node) node = checkAndAlloc<ScrollingTextEffect>(name);
    if (!node) node = checkAndAlloc<SinusEffect>(name);
    if (!node) node = checkAndAlloc<SphereMoveEffect>(name);
    if (!node) node = checkAndAlloc<StarFieldEffect>(name);
    if (!node) node = checkAndAlloc<WaveEffect>(name);
    if (!node) node = checkAndAlloc<SpiralFireEffect>(name);
    if (!node) node = checkAndAlloc<VUMeterEffect>(name);

    // MoonModules effects, alphabetically
    if (!node) node = checkAndAlloc<GameOfLifeEffect>(name);
    if (!node) node = checkAndAlloc<GEQ3DEffect>(name);
    if (!node) node = checkAndAlloc<PaintBrushEffect>(name);

    // WLED effects, alphabetically
    if (!node) node = checkAndAlloc<BlackholeEffect>(name);
    if (!node) node = checkAndAlloc<BouncingBallsEffect>(name);
    if (!node) node = checkAndAlloc<BlurzEffect>(name);
    if (!node) node = checkAndAlloc<ColorTwinkleEffect>(name);
    if (!node) node = checkAndAlloc<DistortionWavesEffect>(name);
    if (!node) node = checkAndAlloc<DJLightEffect>(name);
    if (!node) node = checkAndAlloc<DNAEffect>(name);
    if (!node) node = checkAndAlloc<DripEffect>(name);
    if (!node) node = checkAndAlloc<FireworksEffect>(name);
    if (!node) node = checkAndAlloc<FlowEffect>(name);
    if (!node) node = checkAndAlloc<FreqMatrixEffect>(name);
    if (!node) node = checkAndAlloc<FrizzlesEffect>(name);
    if (!node) node = checkAndAlloc<FunkyPlankEffect>(name);
    if (!node) node = checkAndAlloc<GEQEffect>(name);
    if (!node) node = checkAndAlloc<HeartBeatEffect>(name);
    if (!node) node = checkAndAlloc<JuliaEffect>(name);
    if (!node) node = checkAndAlloc<LissajousEffect>(name);
    if (!node) node = checkAndAlloc<Noise2DEffect>(name);
    if (!node) node = checkAndAlloc<NoiseMeterEffect>(name);
    if (!node) node = checkAndAlloc<OctopusEffect>(name);
    if (!node) node = checkAndAlloc<PacManEffect>(name);
    if (!node) node = checkAndAlloc<PlasmaEffect>(name);
    if (!node) node = checkAndAlloc<PoliceEffect>(name);
    if (!node) node = checkAndAlloc<PopCornEffect>(name);
    if (!node) node = checkAndAlloc<RainEffect>(name);
    if (!node) node = checkAndAlloc<TetrixEffect>(name);
    if (!node) node = checkAndAlloc<WaverlyEffect>(name);

    // FastLED
    if (!node) node = checkAndAlloc<RainbowEffect>(name);

    // Moving head effects, alphabetically

    if (!node) node = checkAndAlloc<AmbientMoveEffect>(name);
    if (!node) node = checkAndAlloc<FreqColorsEffect>(name);
    if (!node) node = checkAndAlloc<Troy1ColorEffect>(name);
    if (!node) node = checkAndAlloc<Troy1MoveEffect>(name);
    if (!node) node = checkAndAlloc<Troy2ColorEffect>(name);
    if (!node) node = checkAndAlloc<Troy2MoveEffect>(name);
    if (!node) node = checkAndAlloc<WowiMoveEffect>(name);

    // Modifiers, most used first

    if (!node) node = checkAndAlloc<MultiplyModifier>(name);
    if (!node) node = checkAndAlloc<MirrorModifier>(name);
    if (!node) node = checkAndAlloc<TransposeModifier>(name);
    if (!node) node = checkAndAlloc<CircleModifier>(name);
    if (!node) node = checkAndAlloc<RotateModifier>(name);
    if (!node) node = checkAndAlloc<CheckerboardModifier>(name);
    if (!node) node = checkAndAlloc<PinwheelModifier>(name);
    if (!node) node = checkAndAlloc<RippleXZModifier>(name);

  #if FT_LIVESCRIPT
    if (!node) {
      LiveScriptNode* liveScriptNode = allocMBObject<LiveScriptNode>();
      liveScriptNode->animation = name;  // set the (file)name of the script
      node = liveScriptNode;
    }
  #endif

    if (node) {
      EXT_LOGI(ML_TAG, "Add %s (p:%p pr:%d)", name, node, isInPSRAM(node));

      node->constructor(layerP.layers[0], controls, &layerP.effectsMutex);  // pass the layer to the node
      node->moduleControl = _moduleLightsControl;                           // to access global lights control functions if needed
      // node->moduleIO = _moduleIO;                     // to get pin allocations
      node->moduleNodes = (Module*)this;  // to request UI update
      node->setup();                      // run the setup of the effect
      node->onSizeChanged(Coord3D());     // to init memory allocations
      // layers[0]->nodes.reserve(index+1);

      // from here it runs concurrently in the effects task
      if (index >= layerP.layers[0]->nodes.size())
        layerP.layers[0]->nodes.push_back(node);
      else
        layerP.layers[0]->nodes[index] = node;  // add the node to the layer
    }

    return node;
  }

  // run effects
  void loop() override { NodeManager::loop(); }

  void onUpdate(const UpdatedItem& updatedItem, const String& originId) override {
    NodeManager::onUpdate(updatedItem, originId);
    if (originId.toInt()) { // UI triggered
      _moduleLightsControl->read(
          [&](ModuleState& state) {
            JsonDocument doc;
            JsonObject newState = doc.to<JsonObject>();

            EXT_LOGI(ML_TAG, "remove preset");
            newState["preset"] = state.data["preset"];
            newState["preset"]["select"] = 255;
            newState["preset"]["selected"] = 255;
            if (newState.size()) {
              _moduleLightsControl->update(newState, ModuleState::update, originId);  // Do not add server in the originID as that blocks updates, see execOnUpdate
            }
          },
          originId);
    }
  }

};  // class ModuleEffects

#endif
#endif