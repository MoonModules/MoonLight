/**
    @title     MoonBase
    @file      ModuleMoonLightInfo.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/moonlightinfo/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleMoonLightInfo_h
#define ModuleMoonLightInfo_h

#if FT_MOONLIGHT == 1

  #include "MoonBase/Module.h"
class ModuleMoonLightInfo : public Module {
 public:
  ModuleMoonLightInfo(PsychicHttpServer* server, ESP32SvelteKit* sveltekit) : Module("moonlightinfo", server, sveltekit) { EXT_LOGV(ML_TAG, "constructor"); }

  void setupDefinition(const JsonArray& root) override {
    EXT_LOGV(ML_TAG, "");
    JsonObject control;  // state.data has one or more properties
    JsonArray details;    // if a control is an array, this is the details of the array
    // JsonArray values; // if a control is a select, this is the values of the select

    control = addControl(root, "nrOfLights", "number", 0, 65536, true);
    control = addControl(root, "channelsPerLight", "number", 0, 65536, true);
    control = addControl(root, "maxChannels", "number", 0, 65536, true);
    control = addControl(root, "maxMappings", "number", 0, 65536, true);
    control = addControl(root, "size", "coord3D", 0, 65536, true);
    control = addControl(root, "nodes#", "number", 0, 65536, true);

    control = addControl(root, "layers", "rows");

    details = control["n"].to<JsonArray>();
    {
      control = addControl(details, "nrOfLights", "number", 0, 65536, true);
      control = addControl(details, "size", "coord3D", 0, 65536, true);
      control = addControl(details, "nrOfZeroLights", "number", 0, 65536, true);
      control = addControl(details, "nrOfOneLight", "number", 0, 65536, true);
      control = addControl(details, "mappingTableIndexes#", "number", 0, 65536, true);
      control = addControl(details, "nrOfMoreLights", "number", 0, 65536, true);
      control = addControl(details, "nodes#", "number", 0, 65536, true);
    }
  }

  void begin() override {
    Module::begin();

    _state.readHook = [&](JsonObject data) {
      EXT_LOGV(ML_TAG, "readHook");
      // this should be updated each time the UI queries for it ... (now only at boot)
      data["nrOfLights"] = layerP.lights.header.nrOfLights;
      data["channelsPerLight"] = layerP.lights.header.channelsPerLight;
      data["maxChannels"] = layerP.lights.maxChannels;
      data["maxMappings"] = layerP.lights.maxMappings;
      data["size"]["x"] = layerP.lights.header.size.x;
      data["size"]["y"] = layerP.lights.header.size.y;
      data["size"]["z"] = layerP.lights.header.size.z;
      data["nodes#"] = layerP.nodes.size();
      uint8_t index = 0;
      for (VirtualLayer* layer : layerP.layers) {
        uint16_t nrOfZeroLights = 0;
        uint16_t nrOfOneLight = 0;
        uint16_t nrOfMoreLights = 0;
        for (size_t i = 0; i < layer->nrOfLights; i++) {
          PhysMap& map = layer->mappingTable[i];
          switch (map.mapType) {
          case m_zeroLights:
            nrOfZeroLights++;
            break;
          case m_oneLight:
            nrOfOneLight++;
            break;
          case m_moreLights:
            for (uint16_t indexP : layer->mappingTableIndexes[map.indexes]) {
              nrOfMoreLights++;
            }
            break;
          }
        }

        data["layers"][index]["nrOfLights"] = layer->nrOfLights;
        data["layers"][index]["size"]["x"] = layer->size.x;
        data["layers"][index]["size"]["y"] = layer->size.y;
        data["layers"][index]["size"]["z"] = layer->size.z;
        data["layers"][index]["nrOfZeroLights"] = nrOfZeroLights;
        data["layers"][index]["nrOfOneLight"] = nrOfOneLight;
        data["layers"][index]["mappingTableIndexes#"] = layer->mappingTableIndexesSizeUsed;
        data["layers"][index]["nrOfMoreLights"] = nrOfMoreLights;
        data["layers"][index]["nodes#"] = layer->nodes.size();
        index++;
      }
    };
  }
};

#endif
#endif