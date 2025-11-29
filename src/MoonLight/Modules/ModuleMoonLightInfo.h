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
    JsonObject property;  // state.data has one or more properties
    JsonArray details;    // if a property is an array, this is the details of the array
    // JsonArray values; // if a property is a select, this is the values of the select

    property = root.add<JsonObject>();
    property["name"] = "nrOfLights";
    property["type"] = "number";
    property["max"] = 65536;
    property["ro"] = true;
    property = root.add<JsonObject>();
    property["name"] = "channelsPerLight";
    property["type"] = "number";
    property["max"] = 65536;
    property["ro"] = true;
    property = root.add<JsonObject>();
    property["name"] = "maxChannels";
    property["type"] = "number";
    property["max"] = 65538;
    property["ro"] = true;
    property = root.add<JsonObject>();
    property["name"] = "size";
    property["type"] = "coord3D";
    property["ro"] = true;
    property = root.add<JsonObject>();
    property["name"] = "nodes#";
    property["type"] = "number";
    property["max"] = 65536;
    property["ro"] = true;

    property = root.add<JsonObject>();
    property["name"] = "layers";
    property["type"] = "rows";
    details = property["n"].to<JsonArray>();
    {
      property = details.add<JsonObject>();
      property["name"] = "nrOfLights";
      property["type"] = "number";
      property["max"] = 65536;
      property["ro"] = true;
      property = details.add<JsonObject>();
      property["name"] = "size";
      property["type"] = "coord3D";
      property["ro"] = true;
      property = details.add<JsonObject>();
      property["name"] = "nrOfZeroLights";
      property["type"] = "number";
      property["max"] = 65536;
      property["ro"] = true;
      property = details.add<JsonObject>();
      property["name"] = "nrOfOneLight";
      property["type"] = "number";
      property["max"] = 65536;
      property["ro"] = true;
      property = details.add<JsonObject>();
      property["name"] = "mappingTableIndexes#";
      property["type"] = "number";
      property["max"] = 65536;
      property["ro"] = true;
      property = details.add<JsonObject>();
      property["name"] = "nrOfMoreLights";
      property["type"] = "number";
      property["max"] = 65536;
      property["ro"] = true;
      property = details.add<JsonObject>();
      property["name"] = "nodes#";
      property["type"] = "number";
      property["max"] = 65536;
      property["ro"] = true;
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
        // data["layers"][index]["mappingTable#"] = layer->nrOfLights;
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