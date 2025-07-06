/**
    @title     MoonBase
    @file      ModuleStephanElecDriverInfo.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/modules/module/demo/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact moonmodules@icloud.com
**/

#ifndef ModuleStephanElecDriverInfo_h
#define ModuleStephanElecDriverInfo_h

#if FT_MOONBASE == 1

#include "../MoonBase/Module.h"
class ModuleStephanElecDriverInfo : public Module
{
public:

    ModuleStephanElecDriverInfo(PsychicHttpServer *server,
            ESP32SvelteKit *sveltekit,
            FilesService *filesService
        ) : Module("BoardInfo", server, sveltekit, filesService) {
            ESP_LOGD(TAG, "constructor");
    }

    void setupDefinition(JsonArray root) override{
        ESP_LOGD(TAG, "");
        JsonObject property; // state.data has one or more properties
        JsonArray details; // if a property is an array, this is the details of the array
        // JsonArray values; // if a property is a select, this is the values of the select

        property = root.add<JsonObject>(); property["name"] = "nrOfLights"; property["type"] = "number"; property["max"] = 65536;
        property = root.add<JsonObject>(); property["name"] = "channelsPerLight"; property["type"] = "number"; property["max"] = 65536;
        property = root.add<JsonObject>(); property["name"] = "chipset"; property["type"] = "text"; property["max"] = 20;
        property = root.add<JsonObject>(); property["name"] = "size"; property["type"] = "coord3D";
        property = root.add<JsonObject>(); property["name"] = "layers"; property["type"] = "array"; details = property["n"].to<JsonArray>();
        {
            property = details.add<JsonObject>(); property["name"] = "nrOfLights"; property["type"] = "number"; property["max"] = 65536;
            property = details.add<JsonObject>(); property["name"] = "size"; property["type"] = "coord3D";
            property = details.add<JsonObject>(); property["name"] = "mappingTable#"; property["type"] = "number"; property["max"] = 65536;
            property = details.add<JsonObject>(); property["name"] = "color#"; property["type"] = "number"; property["max"] = 65536;
            property = details.add<JsonObject>(); property["name"] = "phys#"; property["type"] = "number"; property["max"] = 65536;
            property = details.add<JsonObject>(); property["name"] = "mappingTableIndexes#"; property["type"] = "number"; property["max"] = 65536;
            property = details.add<JsonObject>(); property["name"] = "physM#"; property["type"] = "number"; property["max"] = 65536;
            property = details.add<JsonObject>(); property["name"] = "nodes#"; property["type"] = "number"; property["max"] = 65536;
        }
    }

    void begin() {
        Module::begin();

        _state.readHook = [&](JsonObject data) {
            ESP_LOGD(TAG, "readHook");
            //this should be updated each time the UI queries for it ... (now only at boot)
            data["nrOfLights"] = layerP.lights.header.nrOfLights;
            data["channelsPerLight"] = layerP.lights.header.channelsPerLight;
            data["chipset"] = TOSTRING(ML_CHIPSET);
            data["size"]["x"] = layerP.lights.header.size.x;
            data["size"]["y"] = layerP.lights.header.size.y;
            data["size"]["z"] = layerP.lights.header.size.z;
            uint8_t index = 0;
            for (VirtualLayer *layerV : layerP.layerV) {
                uint16_t nrOfPhysical = 0;
                uint16_t nrOfPhysicalM = 0;
                uint16_t nrOfColor = 0;
                for (size_t i = 0; i < layerV->mappingTableSizeUsed; i++) {
                    PhysMap &map = layerV->mappingTable[i];
                    switch (map.mapType) {
                    case m_color:
                        nrOfColor++;
                        break;
                    case m_oneLight:
                        nrOfPhysical++;
                        break;
                    case m_moreLights:
                        for (uint16_t indexP: layerV->mappingTableIndexes[map.indexes]) {
                            nrOfPhysicalM++;
                        }
                        break;
                    }
                }

                data["layers"][index]["nrOfLights"] = layerV->nrOfLights;
                data["layers"][index]["size"]["x"] = layerV->size.x;
                data["layers"][index]["size"]["y"] = layerV->size.y;
                data["layers"][index]["size"]["z"] = layerV->size.z;
                data["layers"][index]["mappingTable#"] = layerV->mappingTable.size();
                data["layers"][index]["color#"] = nrOfColor;
                data["layers"][index]["phys#"] = nrOfPhysical;
                data["layers"][index]["mappingTableIndexes#"] = layerV->mappingTableIndexesSizeUsed;
                data["layers"][index]["physM#"] = nrOfPhysicalM;
                data["layers"][index]["nodes#"] = layerV->nodes.size();
                index++;
            }
        };
    }
};

#endif
#endif