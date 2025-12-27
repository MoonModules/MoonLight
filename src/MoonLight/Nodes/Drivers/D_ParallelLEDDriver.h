/**
    @title     MoonLight
    @file      D_ParallelLEDDriver.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

  #include <ESP32SvelteKit.h>  // for safeModeMB and restartNeeded

  #ifdef CONFIG_IDF_TARGET_ESP32P4
    #include "parlio.h"
  #endif

class ParallelLEDDriver : public DriverNode {
 public:
  static const char* name() { return "Parallel LED Driver"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }

  #if HP_ALL_DRIVERS
  Char<32> version = HP_ALL_VERSION;
  Char<32> status = "ok";
    #ifndef BOARD_HAS_PSRAM
  uint8_t dmaBuffer = 6;
    #else
  uint8_t dmaBuffer = 75;
    #endif
  #endif

  void setup() override {
    DriverNode::setup();
  #if HP_ALL_DRIVERS
    addControl(dmaBuffer, "dmaBuffer", "slider", 1, 100);
    addControl(version, "version", "text", 0, 32, true);  // read only
    addControl(status, "status", "text", 0, 32, true);    // read only
    updateControl("version", HP_ALL_VERSION);             // update also if node already exists
  #endif
  }

  uint8_t pins[MAX_PINS] = {};

  void loop() override {
  #if HP_ALL_DRIVERS
    if (!initDone) return;

    if (layerP.lights.header.isPositions == 0) {
      DriverNode::loop();  // This populates the LUT tables!

    #ifndef CONFIG_IDF_TARGET_ESP32P4
      if (ledsDriver.total_leds > 0) ledsDriver.showPixels(WAIT);
    #else
      uint8_t nrOfPins = min(layerP.nrOfLedPins, layerP.nrOfAssignedPins);
      // LUTs are accessed directly within show_parlio via extern ledsDriver
      // No brightness parameter needed
      show_parlio(pins, layerP.lights.header.nrOfLights, layerP.lights.channelsD, layerP.lights.header.channelsPerLight == 4, nrOfPins, layerP.ledsPerPin[0], layerP.lights.header.offsetRed, layerP.lights.header.offsetGreen, layerP.lights.header.offsetBlue);
    #endif
    }
  #else  // ESP32_LEDSDRIVER
    if (!ledsDriver.initLedsDone) return;

    if (layerP.lights.header.isPositions == 0) {
      DriverNode::loop();

      ledsDriver.show();
    }
  #endif
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
  #if HP_ALL_DRIVERS
    if (layerP.pass == 1 && !layerP.monitorPass) {
      uint8_t nrOfPins = min(layerP.nrOfLedPins, layerP.nrOfAssignedPins);

      if (!lightPresetSaved || nrOfPins == 0) {  //|| initDone can be done multiple times now...
        EXT_LOGD(ML_TAG, "return: lightpresetsaved:%d initDone:%d #:%d", lightPresetSaved, initDone, nrOfPins);
        return;
      }

      if (safeModeMB) {
        EXT_LOGW(ML_TAG, "Safe mode enabled, not adding Parallel LED Driver");
        return;
      }

      EXT_LOGD(ML_TAG, "nrOfLedPins %d %d %d", nrOfPins, layerP.nrOfLedPins, layerP.nrOfAssignedPins);

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

        // ESP32-P4: Uses parlio driver with direct LUT access (no explicit init needed)
        // Non-P4: Uses Yves driver with DMA buffer allocation and initled()
    #ifndef CONFIG_IDF_TARGET_ESP32P4  // Non P4: Yves driver

      if (!initDone) {
        __NB_DMA_BUFFER = dmaBuffer;  // __NB_DMA_BUFFER is a variable

        uint8_t savedBrightness = ledsDriver._brightness;  //(initLed sets it to 255 and thats not what we want)

        EXT_LOGD(ML_TAG, "init Parallel LED Driver %d %d %d %d", layerP.lights.header.channelsPerLight, layerP.lights.header.offsetRed, layerP.lights.header.offsetGreen, layerP.lights.header.offsetBlue);
        ledsDriver.initled(layerP.lights.channelsD, pins, layerP.ledsPerPin, nrOfPins, layerP.lights.header.channelsPerLight, layerP.lights.header.offsetRed, layerP.lights.header.offsetGreen, layerP.lights.header.offsetBlue);

        ledsDriver.setBrightness(savedBrightness);  //(initLed sets it to 255 and thats not what we want)

      #if ML_LIVE_MAPPING
        ledsDriver.setMapLed(&mapLed);
      #endif

        initDone = true;  // so loop is called and initled not called again if channelsPerLight or pins saved
      } else {
        // don't call initled again as that will crash because if channelsPerLight (nb_components) change, the dma buffers are not big enough

        EXT_LOGD(ML_TAG, "update Parallel LED Driver %d %d %d %d", layerP.lights.header.channelsPerLight, layerP.lights.header.offsetRed, layerP.lights.header.offsetGreen, layerP.lights.header.offsetBlue);
        ledsDriver.updateDriver(pins, layerP.ledsPerPin, nrOfPins, dmaBuffer, layerP.lights.header.channelsPerLight, layerP.lights.header.offsetRed, layerP.lights.header.offsetGreen, layerP.lights.header.offsetBlue);
      }

    #else  // P4: Parlio Troy Driver
      initDone = true;  // so loop is called and initled not called again if channelsPerLight or pins saved
    #endif
    }
  #else  // ESP32_LEDSDRIVER
    if (!lightPresetSaved || ledsDriver.initLedsDone || layerP.sortedPins.size() == 0) return;

    if (layerP.pass == 1) {
      EXT_LOGD(ML_TAG, "sortedPins #:%d", layerP.sortedPins.size());
      if (safeModeMB) {
        EXT_LOGW(ML_TAG, "Safe mode enabled, not adding Parallel LED Driver");
        return;
      }

    #if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32)
      int numPins = 0;
      PinConfig pinConfig[MAX_PINS];

      for (const SortedPin& sortedPin : layerP.sortedPins) {
        EXT_LOGD(ML_TAG, "sortedPin s:%d #:%d p:%d", sortedPin.startLed, sortedPin.nrOfLights, sortedPin.pin);
        if (numPins < MAX_PINS) {
          pinConfig[numPins].gpio = sortedPin.pin;
          pinConfig[numPins].nrOfLeds = sortedPin.nrOfLights;
          numPins++;
        }
      }

      EXT_LOGD(ML_TAG, "pins[");
      for (int i = 0; i < numPins; i++) {
        EXT_LOGD(ML_TAG, ", %d", pinConfig[i].gpio);
      }

      if (numPins > 0) {
        ledsDriver.initLeds(layerP.lights.channelsD, pinConfig, numPins, layerP.lights.header.channelsPerLight, layerP.lights.header.offsetRed, layerP.lights.header.offsetGreen, layerP.lights.header.offsetBlue, layerP.lights.header.offsetWhite);  // 102 is GRB

      #if ML_LIVE_MAPPING
        driver.setMapLed(&mapLed);
      #endif
      }
    #endif
    }
  #endif
  }

  ~ParallelLEDDriver() override {
  #if HP_ALL_DRIVERS
    EXT_LOGD(ML_TAG, "Destroy %d + 1 dma buffers", __NB_DMA_BUFFER);

    ledsDriver.deleteDriver();

      // if recreating the Parallel LED Driver later, still initled cannot be done again ?
  #endif
  }
};

#endif