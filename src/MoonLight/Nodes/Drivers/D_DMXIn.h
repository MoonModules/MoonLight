/**
    @title     MoonLight
    @file      D_Infrared.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

class DMXInDriver : public Node {
 public:
  static const char* name() { return "DMX In"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️🚧"; }  // use emojis see https://moonmodules.org/MoonLight/moonlight/overview/#emoji-coding, ☸️ for drivers

  uint8_t universe = 0;
  uint8_t firstChannel = 0;
  bool hardwareDMX = false;  // otherwise via artnet
  char statusMessage[64] = "Okay";

  void setup() override {
    addControl(universe, "universe", "number");
    addControl(firstChannel, "firstChannel", "number");
    addControl(hardwareDMX, "hardwareDMX", "checkbox");
    addControl(statusMessage, "status", "text", 0, 64, true);  // read only
  }

  bool success = true;
  uint8_t pins[8];

  void onUpdate(String& oldValue, JsonObject control) override {
    if (control["name"] == "hardwareDMX") {
      if (control["value"] == true) {
        pins[0] = UINT8_MAX;  // get dmx pins from the io module, if not exist, make the init fail
        pins[1] = UINT8_MAX;  // get dmx pins from the io module, if not exist, make the init fail
        pins[2] = UINT8_MAX;  // get dmx pins from the io module, if not exist, make the init fail
        //...
        if (pins[0] == UINT8_MAX || pins[1] == UINT8_MAX || pins[2] == UINT8_MAX) {
          success = false;
          strcpy(statusMessage, "Pins not found");
        }
      }
    } else {
      // Art-Net init...
      success = true;
    }
    if (success) strcpy(statusMessage, "Wating on UK 😁");
  }

  // use for continuous actions, e.g. reading data from sensors or sending data to lights (e.g. LED drivers or Art-Net)
  void loop() override {
    // if dmx input received (via hardware or artnet)
    // if universe and first channel match
    if (false) {
      uint8_t channels[12]; //currently 7, will be about 12
      // assign the received channels

      JsonDocument doc;
      JsonObject newState = doc.as<JsonObject>();

      // remaining actions:
      //  presetLoop
      //  firstPreset
      //  lastPreset
      //  monitorOn
      //  BPM
      //  intensity

      controlModule->read([&](ModuleState& state) {
        newState["lightsOn"] = channels[0];
        newState["brightness"] = channels[1];
        newState["red"] = channels[2];
        newState["green"] = channels[3];
        newState["blue"] = channels[4];
        newState["preset"] = map(channels[5], 0, 256, 0, 63);
        newState["palette"] = map(channels[6], 0, 256, 0, 8);
      });

      // update the state and ModuleState::update processes the changes behind the scenes
      controlModule->update(newState, ModuleState::update, "DMXIn");
    }
  };
};

#endif