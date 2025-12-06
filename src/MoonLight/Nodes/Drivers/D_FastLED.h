/**
    @title     MoonLight
    @file      FastLED.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

  // Expand to , COLOR_ORDER if defined
  #if defined(ML_COLOR_ORDER)
    #define COLOR_ORDER_ARG , ML_COLOR_ORDER
  #else
    #define COLOR_ORDER_ARG
  #endif

  // Expand to .setRgbw(RgbwDefault()) if ML_RGBW is 1
  #if ML_RGBW
    #define RGBW_CALL .setRgbw(RgbwDefault())
  #else
    #define RGBW_CALL
  #endif

class FastLEDDriver : public Node {
 public:
  static const char* name() { return "FastLED Driver"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }

  char chipSet[20] = TOSTRING(ML_CHIPSET);
  char version[20] = TOSTRING(FASTLED_VERSION);  // "." TOSTRING(FASTLED_VERSION_MINOR) "." TOSTRING(FASTLED_VERSION_PATCH);
  char colorOrder[20] = TOSTRING(ML_COLOR_ORDER);
  #if FASTLED_USES_ESP32S3_I2S
  bool usesI2S = true;
  #else
  bool usesI2S = false;
  #endif
  #if FASTLED_RMT5
  bool usesRMT5 = true;
  #else
  bool usesRMT5 = false;
  #endif

  void setup() override {
    addControl(version, "version", "text", 0, 20, true);
    addControl(chipSet, "chipSet", "text", 0, 20, true);
    addControl(colorOrder, "colorOrder", "text", 0, 20, true);
    addControl(usesI2S, "usesI2S", "checkbox", 0, 20, true);
    addControl(usesRMT5, "usesRMT5", "checkbox", 0, 20, true);
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
        // ++x;
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
      // if (safeModeMB) {
      //     EXT_LOGW(ML_TAG, "Safe mode enabled, not adding FastLED driver");
      //     return;
      // }

      uint8_t nrOfPins = min(layerP.nrOfLedPins, layerP.nrOfAssignedPins);

      if (nrOfPins == 0) return;

      EXT_LOGD(ML_TAG, "nrOfLedPins #:%d", nrOfPins);

      uint16_t startLed = 0;
      for (uint8_t pinIndex = 0; pinIndex < nrOfPins && pinIndex < 4; pinIndex++) {  // FastLED RMT supports max 4 pins!
        EXT_LOGD(ML_TAG, "ledPin s:%d #:%d p:%d", pinIndex, layerP.ledsPerPin[pinIndex]);

        uint16_t nrOfLights = layerP.ledsPerPin[pinIndex];

        CRGB* leds = (CRGB*)layerP.lights.channels;

        switch (layerP.ledPins[pinIndex]) {
  #if CONFIG_IDF_TARGET_ESP32
        case 0:
          FastLED.addLeds<ML_CHIPSET, 0 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 1:
          FastLED.addLeds<ML_CHIPSET, 1 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 2:
          FastLED.addLeds<ML_CHIPSET, 2 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 3:
          FastLED.addLeds<ML_CHIPSET, 3 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 4:
          FastLED.addLeds<ML_CHIPSET, 4 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 5:
          FastLED.addLeds<ML_CHIPSET, 5 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        // case 6: FastLED.addLeds<ML_CHIPSET, 6 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 7: FastLED.addLeds<ML_CHIPSET, 7 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 8: FastLED.addLeds<ML_CHIPSET, 8 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 9: FastLED.addLeds<ML_CHIPSET, 9 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 10: FastLED.addLeds<ML_CHIPSET, 10 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 11:
          FastLED.addLeds<ML_CHIPSET, 11 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 12:
          FastLED.addLeds<ML_CHIPSET, 12 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 13:
          FastLED.addLeds<ML_CHIPSET, 13 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 14:
          FastLED.addLeds<ML_CHIPSET, 14 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 15:
          FastLED.addLeds<ML_CHIPSET, 15 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
    #if !defined(BOARD_HAS_PSRAM) && !defined(ARDUINO_ESP32_PICO)
        // 16+17 = reserved for PSRAM, or reserved for FLASH on pico-D4
        case 16:
          FastLED.addLeds<ML_CHIPSET, 16 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 17:
          FastLED.addLeds<ML_CHIPSET, 17 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
    #endif
        case 18:
          FastLED.addLeds<ML_CHIPSET, 18 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 19:
          FastLED.addLeds<ML_CHIPSET, 19 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        // case 20: FastLED.addLeds<ML_CHIPSET, 20 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 21:
          FastLED.addLeds<ML_CHIPSET, 21 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 22:
          FastLED.addLeds<ML_CHIPSET, 22 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 23:
          FastLED.addLeds<ML_CHIPSET, 23 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        // case 24: FastLED.addLeds<ML_CHIPSET, 24 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 25:
          FastLED.addLeds<ML_CHIPSET, 25 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 26:
          FastLED.addLeds<ML_CHIPSET, 26 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 27:
          FastLED.addLeds<ML_CHIPSET, 27 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        // case 28: FastLED.addLeds<ML_CHIPSET, 28 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 29: FastLED.addLeds<ML_CHIPSET, 29 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 30: FastLED.addLeds<ML_CHIPSET, 30 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 31: FastLED.addLeds<ML_CHIPSET, 31 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 32:
          FastLED.addLeds<ML_CHIPSET, 32 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 33:
          FastLED.addLeds<ML_CHIPSET, 33 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
            // 34-39 input-only
            // case 34: FastLED.addLeds<ML_CHIPSET, 34 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 35: FastLED.addLeds<ML_CHIPSET, 35 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 36: FastLED.addLeds<ML_CHIPSET, 36 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 37: FastLED.addLeds<ML_CHIPSET, 37 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 38: FastLED.addLeds<ML_CHIPSET, 38 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 39: FastLED.addLeds<ML_CHIPSET, 39 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
  #endif  // CONFIG_IDF_TARGET_ESP32

  #if CONFIG_IDF_TARGET_ESP32S2
        case 0:
          FastLED.addLeds<ML_CHIPSET, 0 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 1:
          FastLED.addLeds<ML_CHIPSET, 1 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 2:
          FastLED.addLeds<ML_CHIPSET, 2 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 3:
          FastLED.addLeds<ML_CHIPSET, 3 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 4:
          FastLED.addLeds<ML_CHIPSET, 4 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 5:
          FastLED.addLeds<ML_CHIPSET, 5 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 6:
          FastLED.addLeds<ML_CHIPSET, 6 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 7:
          FastLED.addLeds<ML_CHIPSET, 7 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 8:
          FastLED.addLeds<ML_CHIPSET, 8 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 9:
          FastLED.addLeds<ML_CHIPSET, 9 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 10:
          FastLED.addLeds<ML_CHIPSET, 10 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 11:
          FastLED.addLeds<ML_CHIPSET, 11 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 12:
          FastLED.addLeds<ML_CHIPSET, 12 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 13:
          FastLED.addLeds<ML_CHIPSET, 13 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 14:
          FastLED.addLeds<ML_CHIPSET, 14 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 15:
          FastLED.addLeds<ML_CHIPSET, 15 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 16:
          FastLED.addLeds<ML_CHIPSET, 16 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 17:
          FastLED.addLeds<ML_CHIPSET, 17 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 18:
          FastLED.addLeds<ML_CHIPSET, 18 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
    #if !ARDUINO_USB_CDC_ON_BOOT
        // 19 + 20 = USB HWCDC. reserved for USB port when ARDUINO_USB_CDC_ON_BOOT=1
        case 19:
          FastLED.addLeds<ML_CHIPSET, 19 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 20:
          FastLED.addLeds<ML_CHIPSET, 20 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
    #endif
        case 21:
          FastLED.addLeds<ML_CHIPSET, 21 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
            // 22 to 32: not connected, or reserved for SPI FLASH
            // case 22: FastLED.addLeds<ML_CHIPSET, 22 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 23: FastLED.addLeds<ML_CHIPSET, 23 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 24: FastLED.addLeds<ML_CHIPSET, 24 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 25: FastLED.addLeds<ML_CHIPSET, 25 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
    #if !defined(BOARD_HAS_PSRAM)
        // 26-32 = reserved for PSRAM
        case 26:
          FastLED.addLeds<ML_CHIPSET, 26 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
              // case 27: FastLED.addLeds<ML_CHIPSET, 27 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
              // case 28: FastLED.addLeds<ML_CHIPSET, 28 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
              // case 29: FastLED.addLeds<ML_CHIPSET, 29 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
              // case 30: FastLED.addLeds<ML_CHIPSET, 30 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
              // case 31: FastLED.addLeds<ML_CHIPSET, 31 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
              // case 32: FastLED.addLeds<ML_CHIPSET, 32 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
    #endif
        case 33:
          FastLED.addLeds<ML_CHIPSET, 33 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 34:
          FastLED.addLeds<ML_CHIPSET, 34 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 35:
          FastLED.addLeds<ML_CHIPSET, 35 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 36:
          FastLED.addLeds<ML_CHIPSET, 36 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 37:
          FastLED.addLeds<ML_CHIPSET, 37 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 38:
          FastLED.addLeds<ML_CHIPSET, 38 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 39:
          FastLED.addLeds<ML_CHIPSET, 39 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 40:
          FastLED.addLeds<ML_CHIPSET, 40 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 41:
          FastLED.addLeds<ML_CHIPSET, 41 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 42:
          FastLED.addLeds<ML_CHIPSET, 42 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 43:
          FastLED.addLeds<ML_CHIPSET, 43 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 44:
          FastLED.addLeds<ML_CHIPSET, 44 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 45:
          FastLED.addLeds<ML_CHIPSET, 45 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
            // 46 input-only
            // case 46: FastLED.addLeds<ML_CHIPSET, 46 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
  #endif  // CONFIG_IDF_TARGET_ESP32S2

  #if CONFIG_IDF_TARGET_ESP32C3
        case 0:
          FastLED.addLeds<ML_CHIPSET, 0 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 1:
          FastLED.addLeds<ML_CHIPSET, 1 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 2:
          FastLED.addLeds<ML_CHIPSET, 2 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 3:
          FastLED.addLeds<ML_CHIPSET, 3 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 4:
          FastLED.addLeds<ML_CHIPSET, 4 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 5:
          FastLED.addLeds<ML_CHIPSET, 5 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 6:
          FastLED.addLeds<ML_CHIPSET, 6 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 7:
          FastLED.addLeds<ML_CHIPSET, 7 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 8:
          FastLED.addLeds<ML_CHIPSET, 8 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 9:
          FastLED.addLeds<ML_CHIPSET, 9 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 10:
          FastLED.addLeds<ML_CHIPSET, 10 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
            // 11-17 reserved for SPI FLASH
            // case 11: FastLED.addLeds<ML_CHIPSET, 11 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 12: FastLED.addLeds<ML_CHIPSET, 12 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 13: FastLED.addLeds<ML_CHIPSET, 13 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 14: FastLED.addLeds<ML_CHIPSET, 14 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 15: FastLED.addLeds<ML_CHIPSET, 15 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 16: FastLED.addLeds<ML_CHIPSET, 16 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 17: FastLED.addLeds<ML_CHIPSET, 17 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
    #if !ARDUINO_USB_CDC_ON_BOOT
        // 18 + 19 = USB HWCDC. reserved for USB port when ARDUINO_USB_CDC_ON_BOOT=1
        case 18:
          FastLED.addLeds<ML_CHIPSET, 18 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 19:
          FastLED.addLeds<ML_CHIPSET, 19 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
    #endif
        // 20+21 = Serial RX+TX --> don't use for LEDS when serial-to-USB is needed
        case 20:
          FastLED.addLeds<ML_CHIPSET, 20 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 21:
          FastLED.addLeds<ML_CHIPSET, 21 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
  #endif  // CONFIG_IDF_TARGET_ESP32S2

  #ifdef CONFIG_IDF_TARGET_ESP32S3
        case 0:
          FastLED.addLeds<ML_CHIPSET, 0 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 1:
          FastLED.addLeds<ML_CHIPSET, 1 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 2:
          FastLED.addLeds<ML_CHIPSET, 2 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 3:
          FastLED.addLeds<ML_CHIPSET, 3 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 4:
          FastLED.addLeds<ML_CHIPSET, 4 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 5:
          FastLED.addLeds<ML_CHIPSET, 5 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 6:
          FastLED.addLeds<ML_CHIPSET, 6 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 7:
          FastLED.addLeds<ML_CHIPSET, 7 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 8:
          FastLED.addLeds<ML_CHIPSET, 8 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 9:
          FastLED.addLeds<ML_CHIPSET, 9 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 10:
          FastLED.addLeds<ML_CHIPSET, 10 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 11:
          FastLED.addLeds<ML_CHIPSET, 11 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 12:
          FastLED.addLeds<ML_CHIPSET, 12 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 13:
          FastLED.addLeds<ML_CHIPSET, 13 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 14:
          FastLED.addLeds<ML_CHIPSET, 14 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 15:
          FastLED.addLeds<ML_CHIPSET, 15 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 16:
          FastLED.addLeds<ML_CHIPSET, 16 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 17:
          FastLED.addLeds<ML_CHIPSET, 17 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 18:
          FastLED.addLeds<ML_CHIPSET, 18 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
    #if !ARDUINO_USB_CDC_ON_BOOT
        // 19 + 20 = USB-JTAG. Not recommended for other uses.
        case 19:
          FastLED.addLeds<ML_CHIPSET, 19 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 20:
          FastLED.addLeds<ML_CHIPSET, 20 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
    #endif
        case 21:
          FastLED.addLeds<ML_CHIPSET, 21 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
            // // 22 to 32: not connected, or SPI FLASH
            // case 22: FastLED.addLeds<ML_CHIPSET, 22 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 23: FastLED.addLeds<ML_CHIPSET, 23 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 24: FastLED.addLeds<ML_CHIPSET, 24 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 25: FastLED.addLeds<ML_CHIPSET, 25 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 26: FastLED.addLeds<ML_CHIPSET, 26 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 27: FastLED.addLeds<ML_CHIPSET, 27 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 28: FastLED.addLeds<ML_CHIPSET, 28 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 29: FastLED.addLeds<ML_CHIPSET, 29 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 30: FastLED.addLeds<ML_CHIPSET, 30 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 31: FastLED.addLeds<ML_CHIPSET, 31 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
            // case 32: FastLED.addLeds<ML_CHIPSET, 32 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
    #if !defined(BOARD_HAS_PSRAM)
        // 33 to 37: reserved if using _octal_ SPI Flash or _octal_ PSRAM
        case 33:
          FastLED.addLeds<ML_CHIPSET, 33 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 34:
          FastLED.addLeds<ML_CHIPSET, 34 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 35:
          FastLED.addLeds<ML_CHIPSET, 35 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 36:
          FastLED.addLeds<ML_CHIPSET, 36 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 37:
          FastLED.addLeds<ML_CHIPSET, 37 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
    #endif
        case 38:
          FastLED.addLeds<ML_CHIPSET, 38 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 39:
          FastLED.addLeds<ML_CHIPSET, 39 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 40:
          FastLED.addLeds<ML_CHIPSET, 40 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 41:
          FastLED.addLeds<ML_CHIPSET, 41 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 42:
          FastLED.addLeds<ML_CHIPSET, 42 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        // 43+44 = Serial RX+TX --> don't use for LEDS when serial-to-USB is needed
        case 43:
          FastLED.addLeds<ML_CHIPSET, 43 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 44:
          FastLED.addLeds<ML_CHIPSET, 44 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 45:
          FastLED.addLeds<ML_CHIPSET, 45 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 46:
          FastLED.addLeds<ML_CHIPSET, 46 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 47:
          FastLED.addLeds<ML_CHIPSET, 47 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
        case 48:
          FastLED.addLeds<ML_CHIPSET, 48 COLOR_ORDER_ARG>(leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL;
          break;
  #endif  // CONFIG_IDF_TARGET_ESP32S3

        default:
          EXT_LOGW(ML_TAG, "FastLEDPin assignment: pin not supported %d", layerP.ledPins[pinIndex]);
        }  // switch pinNr

        startLed += layerP.ledsPerPin[pinIndex];

      }  // sortedPins
    }

    FastLED.setMaxPowerInMilliWatts(1000 * layerP.maxPower);  // 5v, 2000mA, to protect usb while developing
  }
};

#endif