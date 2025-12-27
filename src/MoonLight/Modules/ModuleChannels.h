/**
    @title     MoonBase
    @file      ModuleChannels.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/channels/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleChannels_h
#define ModuleChannels_h

#if FT_MOONLIGHT == 1

  #include "MoonBase/Module.h"
  #include "MoonLight/Layers/VirtualLayer.h"

class ModuleChannels : public Module {
 public:
  ModuleChannels(PsychicHttpServer* server, ESP32SvelteKit* sveltekit) : Module("channels", server, sveltekit) { EXT_LOGV(ML_TAG, "constructor"); }

  void setupDefinition(const JsonArray& controls) override {
    EXT_LOGV(ML_TAG, "");

    JsonObject control;  // state.data has one or more properties

    control = addControl(controls, "view", "select");
    control["default"] = 0;
    addControlValue(control, "Physical layer");
    uint8_t i = 1; //start with 1
    for (VirtualLayer* layer : layerP.layers) {
      Char<32> layerName;
      layerName.format("Layer %d", i);
      addControlValue(control, layerName.c_str());
      i++;
    }

    control = addControl(controls, "group", "checkbox");
    control["default"] = true;

    // control = addControl(controls, "universe", "number");
    // control["default"] = 0;

    control = addControl(controls, "channel", "pad");
    control["width"] = 12;
    control["hoverToServer"] = true;
    control["size"] = 10;
    control["default"]["select"] = 0;
    control["default"]["action"] = "";
    control["default"]["list"].to<JsonArray>();
    control["default"]["count"] = 512;
  }

  void onUpdate(const UpdatedItem& updatedItem) override {
    uint8_t view = _state.data["view"];
    bool group = _state.data["group"];

    if (updatedItem.name == "view" || updatedItem.name == "group") {
      uint16_t count = view == 0 ? layerP.lights.header.nrOfLights : layerP.layers[view - 1]->nrOfLights;
      if (!group) count *= layerP.lights.header.channelsPerLight;
      if (count > 512) count = 512;
      if (count != _state.data["channel"]["count"]) {
        EXT_LOGD(ML_TAG, "set count %d", count);

        JsonDocument doc;
        JsonObject newState = doc.to<JsonObject>();
        newState["channel"]["count"] = count;

        update(newState, ModuleState::update, _moduleName + "server");
      }
    } else if (updatedItem.name == "channel") {
      // EXT_LOGD(ML_TAG, "%s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
      // copy the file to the hidden folder...
      if (updatedItem.oldValue != "" && !updatedItem.value["action"].isNull() && updatedItem.value["action"] != "") {
        if (!layerP.lights.channelsE) return; // to avoid crash during init
        EXT_LOGD(ML_TAG, "handle %s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
        uint16_t select = updatedItem.value["select"];
        uint8_t value = updatedItem.value["action"] == "mouseenter" ? 255 : 0;
        if (view == 0) {  // physical layer
          if (group)
            for (uint8_t i = 0; i < layerP.lights.header.channelsPerLight; i++) layerP.lights.channelsE[select * layerP.lights.header.channelsPerLight + i] = value;
          else
            layerP.lights.channelsE[select] = value;
        } else {
          if (group)
            for (uint8_t i = 0; i < layerP.lights.header.channelsPerLight; i++) layerP.layers[view - 1]->setLight(select, &value, i, 1);
          else
            layerP.layers[view - 1]->setLight(select / layerP.lights.header.channelsPerLight, &value, select % layerP.lights.header.channelsPerLight, 1);
        }
      }
    } else
      EXT_LOGV(ML_TAG, "no handle for %s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
  }
};

#endif
#endif