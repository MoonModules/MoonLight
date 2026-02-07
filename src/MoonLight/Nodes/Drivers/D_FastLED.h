/**
    @title     MoonLight
    @file      FastLED.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

// #include "fl/channels/channel.h"
// #include "fl/channels/config.h"
// #if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
//   #include "platforms/esp/32/drivers/spi/channel_engine_spi.h"
// #else
//   #include "platforms/esp/32/drivers/parlio/channel_engine_parlio.h"
// #endif

class FastLEDDriver : public Node {
 public:
  static const char* name() { return "FastLED Driver"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }

  char version[20] = TOSTRING(FASTLED_VERSION);  // "." TOSTRING(FASTLED_VERSION_MINOR) "." TOSTRING(FASTLED_VERSION_PATCH);
  Char<32> status = "ok";

  void setup() override {
    addControl(version, "version", "text", 0, 20, true);
    addControl(status, "status", "text", 0, 32, true);
  }

  uint16_t savedMaxPower = UINT16_MAX;
  void loop() override {
    if (FastLED.count()) {
      if (FastLED.getBrightness() != layerP.lights.header.brightness) {
        EXT_LOGD(ML_TAG, "setBrightness %d", layerP.lights.header.brightness);
        FastLED.setBrightness(layerP.lights.header.brightness);
      }

      if (layerP.maxPower != savedMaxPower) {
        EXT_LOGD(ML_TAG, "setMaxPower %d watt", layerP.maxPower);
        FastLED.setMaxPowerInMilliWatts(1000 * layerP.maxPower);  // 5v, 2000mA, to protect usb while developing
        savedMaxPower = layerP.maxPower;
      }

      // FastLED Led Controllers
      CRGB correction = CRGB(layerP.lights.header.red, layerP.lights.header.green, layerP.lights.header.blue);
      CLEDController* pCur = CLEDController::head();
      while (pCur) {
        if (pCur->getCorrection() != correction) {
          EXT_LOGD(ML_TAG, "setColorCorrection r:%d, g:%d, b:%d (#:%d)", layerP.lights.header.red, layerP.lights.header.green, layerP.lights.header.blue, pCur->size());
          pCur->setCorrection(correction);
        }
        // pCur->size();
        pCur = pCur->next();
      }

      FastLED.show();
    }
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    if (layerP.pass == 1 && !layerP.monitorPass) {
      uint8_t nrOfPins = MIN(MIN(layerP.nrOfLedPins, layerP.nrOfAssignedPins), 4);  // FastLED RMT supports max 4 pins!

      if (nrOfPins == 0) return;

      if (safeModeMB) {
        EXT_LOGW(ML_TAG, "Safe mode enabled, not adding FastLED driver");
        return;
      }

      EXT_LOGD(ML_TAG, "nrOfLedPins #:%d", nrOfPins);
      uint8_t pins[MAX_PINS] = {};

      Char<32> statusString = "#";  // truncate if larger
      statusString += nrOfPins;
      statusString += ": ";
      for (int i = 0; i < nrOfPins; i++) {
        uint8_t assignedPin = layerP.ledPinsAssigned[i];
        if (assignedPin < layerP.nrOfLedPins)
          pins[i] = layerP.ledPins[assignedPin];
        else
          pins[i] = layerP.ledPins[i];
        EXT_LOGD(ML_TAG, "onLayout pin#%d of %d: assigned:%d %d->%d #%d", i, nrOfPins, assignedPin, layerP.ledPins[i], pins[i], layerP.ledsPerPin[i]);
        Char<12> tmp;
        tmp.format(" %d#%d", pins[i], layerP.ledsPerPin[i]);
        statusString += tmp;
      }
      EXT_LOGD(ML_TAG, "status: %s", statusString.c_str());

      updateControl("status", statusString.c_str());
      moduleNodes->requestUIUpdate = true;

      // https://github.com/FastLED/FastLED/blob/master/src/fl/channels/README.md
      fl::ChipsetTimingConfig timing = fl::makeTimingConfig<fl::TIMING_WS2812_800KHZ>();
      CRGB* leds = (CRGB*)layerP.lights.channelsD;
      uint16_t startLed = 0;
  #if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
      FastLED.setExclusiveDriver("I2S");
  #else
      FastLED.setExclusiveDriver("PARLIO");
  #endif

      for (uint8_t pinIndex = 0; pinIndex < nrOfPins; pinIndex++) {
        EXT_LOGD(ML_TAG, "ledPin p:%d #:%d", pins[pinIndex], layerP.ledsPerPin[pinIndex]);

        fl::ChannelOptions options;
  #if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
        options.mAffinity = "I2S";
  #else
        options.mAffinity = "PARLIO";
  #endif

        fl::ChannelConfig config(pins[pinIndex], timing, fl::span<CRGB>(&leds[startLed], layerP.ledsPerPin[pinIndex]), GRB, options);
        fl::ChannelPtr channel = fl::Channel::create(config);

        FastLED.add(channel);

        // FastLED.addLeds<WS2812, 16, GRB>(leds, layerP.ledsPerPin[pinIndex]); this works!!! (but this is static)

        startLed += layerP.ledsPerPin[pinIndex];

        CLEDController* pCur = CLEDController::head();
        EXT_LOGD(ML_TAG, "controller p:%p", pCur);
        while (pCur) {
          EXT_LOGD(ML_TAG, "controller %d", pCur->size());
          pCur = pCur->next();
        }

      }  // for pinIndex < nrOfPins

      // Check which drivers are available
      for (size_t i = 0; i < FastLED.getDriverCount(); i++) {
        auto drivers = FastLED.getDriverInfos();
        auto& info = drivers[i];
        EXT_LOGD(ML_TAG, "Driver: %s, Priority: %d, Enabled: %s", info.name.c_str(), info.priority, info.enabled ? "yes" : "no");
      }

      CLEDController* pCur = CLEDController::head();
      EXT_LOGD(ML_TAG, "controller p:%p", pCur);
      while (pCur) {
        EXT_LOGD(ML_TAG, "controller %d", pCur->size());
        pCur = pCur->next();
      }
    }

    FastLED.setMaxPowerInMilliWatts(1000 * layerP.maxPower);  // 5v, 2000mA, to protect usb while developing
  }
};

#endif