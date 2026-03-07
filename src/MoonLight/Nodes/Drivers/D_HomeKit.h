/**
    @title     MoonLight
    @file      D_HomeKit.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/homeautomation/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

#include <WiFi.h>

/** @brief Driver node that bridges MoonLight to Apple Home via Homebridge HTTP.
 *
 *  Supported modes (selectable via the GUI dropdown):
 *  - 0 Off             — no integration active
 *  - 1 Homebridge HTTP — logs the existing /api/lightscontrol URL for use with
 *                        the homebridge-http-lightbulb plugin; zero extra flash
 *
 *  Homebridge polls the ESP32 over HTTP so no loop() work is required here.
 */
class HomeKitNode : public Node {
 public:
  /** @brief Node display name shown in the Drivers list. */
  static const char* name() { return "HomeKit Driver"; }
  /** @brief No pixel dimensions — this node does not drive LEDs directly. */
  static uint8_t dim() { return _NoD; }
  /** @brief Driver emoji tag. */
  static const char* tags() { return "☸️"; }

  uint8_t mode = 0;

  /** @brief Register the mode select control. */
  void setup() override {
    addControl(mode, "mode", "select");
    addControlValue("Off");
    addControlValue("Homebridge HTTP");
  }

  /** @brief React to mode changes: log the Homebridge plugin URL or silence the integration. */
  void onUpdate(const JsonObject& control) override {
    if (control["name"] == "mode") {
      uint8_t value = control["value"];
      if (value == 0)
        EXT_LOGI(ML_TAG, "HomeKit: Off");
      else if (value == 1)
        EXT_LOGI(ML_TAG, "HomeKit: configure homebridge-http-lightbulb with URL: http://%s/api/lightscontrol",
                 WiFi.localIP().toString().c_str());
    }
  }
};

#endif
