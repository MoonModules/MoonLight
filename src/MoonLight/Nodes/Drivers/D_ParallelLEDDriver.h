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

  void loop() override {
  #if HP_ALL_DRIVERS
    if (!initDone) return;

    if (layer->layerP->lights.header.isPositions == 0) {
      DriverNode::loop();

      uint8_t nrOfPins = min(layerP.nrOfLedPins, layerP.nrOfAssignedPins);

    #ifndef CONFIG_IDF_TARGET_ESP32P4
      if (ledsDriver.total_leds > 0) ledsDriver.showPixels(WAIT);
    #else
      show_parlio(layer->layerP->ledPins, layer->layerP->lights.header.nrOfLights, layer->layerP->lights.channels, ledsDriver._brightness, layer->layerP->lights.header.channelsPerLight == 4, nrOfPins, layer->layerP->ledsPerPin[0],  // different ledsPerPin not supported yet
                  layer->layerP->lights.header.offsetRed, layer->layerP->lights.header.offsetGreen, layer->layerP->lights.header.offsetBlue);
    #endif
    }
  #else  // ESP32_LEDSDRIVER
    if (!ledsDriver.initLedsDone) return;

    if (layer->layerP->lights.header.isPositions == 0) {
      DriverNode::loop();

      ledsDriver.show();
    }
  #endif
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
  #if HP_ALL_DRIVERS
    if (layer->layerP->pass == 1 && !layer->layerP->monitorPass) {
      uint8_t nrOfPins = min(layerP.nrOfLedPins, layerP.nrOfAssignedPins);

      if (!lightPresetSaved || nrOfPins == 0) {  //|| initDone can be done multiple times now...
        EXT_LOGD(ML_TAG, "return: lightpresetsaved:%d initDone:%d #:%d", lightPresetSaved, initDone, nrOfPins);
        return;
      }

      EXT_LOGD(ML_TAG, "nrOfLedPins %d %d %d", nrOfPins, layerP.nrOfLedPins, layerP.nrOfAssignedPins);
      if (safeModeMB) {
        EXT_LOGW(ML_TAG, "Safe mode enabled, not adding Parallel LED Driver");
        return;
      }

    #ifndef CONFIG_IDF_TARGET_ESP32P4  // Non P4: Yves driver
      uint8_t pins[MAX_PINS];
      Char<32> statusString = "#";
      statusString += nrOfPins;
      statusString += ": ";
      for (int i = 0; i < nrOfPins; i++) {
        uint8_t assignedPin = layerP.ledPinsAssigned[i];
        if (assignedPin < layerP.nrOfLedPins)
          pins[i] = layerP.ledPins[assignedPin];
        else
          pins[i] = layerP.ledPins[i];
        EXT_LOGD(ML_TAG, "onLayout pin#%d of %d: %d -> %d #%d", i, nrOfPins, layer->layerP->ledPins[i], pins[i], layer->layerP->ledsPerPin[i]);
        Char<12> tmp;
        tmp.format(" %d#%d", pins[i], layer->layerP->ledsPerPin[i]);
        statusString += tmp;
      }
      EXT_LOGD(ML_TAG, "status: %s", statusString.c_str());

      updateControl("status", statusString.c_str());
      moduleNodes->requestUIUpdate = true;

      if (!initDone) {
        __NB_DMA_BUFFER = dmaBuffer;  // __NB_DMA_BUFFER is a variable

        uint8_t savedBrightness = ledsDriver._brightness;  //(initLed sets it to 255 and thats not what we want)

        EXT_LOGD(ML_TAG, "init Parallel LED Driver %d %d %d %d", layer->layerP->lights.header.channelsPerLight, layer->layerP->lights.header.offsetRed, layer->layerP->lights.header.offsetGreen, layer->layerP->lights.header.offsetBlue);
        ledsDriver.initled(layer->layerP->lights.channels, pins, layer->layerP->ledsPerPin, nrOfPins, layer->layerP->lights.header.channelsPerLight, layer->layerP->lights.header.offsetRed, layer->layerP->lights.header.offsetGreen, layer->layerP->lights.header.offsetBlue);

        ledsDriver.setBrightness(savedBrightness);  //(initLed sets it to 255 and thats not what we want)

      #if ML_LIVE_MAPPING
        ledsDriver.setMapLed(&mapLed);
      #endif

        initDone = true;  // so loop is called and initled not called again if channelsPerLight or pins saved
      } else {
        // don't call initled again as that will crash because if channelsPerLight (nb_components) change, the dma buffers are not big enough

        EXT_LOGD(ML_TAG, "update Parallel LED Driver %d %d %d %d", layer->layerP->lights.header.channelsPerLight, layer->layerP->lights.header.offsetRed, layer->layerP->lights.header.offsetGreen, layer->layerP->lights.header.offsetBlue);
        ledsDriver.updateDriver(pins, layer->layerP->ledsPerPin, nrOfPins, dmaBuffer, layer->layerP->lights.header.channelsPerLight, layer->layerP->lights.header.offsetRed, layer->layerP->lights.header.offsetGreen, layer->layerP->lights.header.offsetBlue);
      }

    #else  // P4: Parlio Troy Driver
      initDone = true;  // so loop is called and initled not called again if channelsPerLight or pins saved
    #endif
    }
  #else  // ESP32_LEDSDRIVER
    if (!lightPresetSaved || ledsDriver.initLedsDone || layer->layerP->sortedPins.size() == 0) return;

    if (layer->layerP->pass == 1) {
      EXT_LOGD(ML_TAG, "sortedPins #:%d", layer->layerP->sortedPins.size());
      if (safeModeMB) {
        EXT_LOGW(ML_TAG, "Safe mode enabled, not adding Parallel LED Driver");
        return;
      }

    #if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32)
      int numPins = 0;
      PinConfig pinConfig[MAX_PINS];

      for (const SortedPin& sortedPin : layer->layerP->sortedPins) {
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
        ledsDriver.initLeds(layer->layerP->lights.channels, pinConfig, numPins, layer->layerP->lights.header.channelsPerLight, layer->layerP->lights.header.offsetRed, layer->layerP->lights.header.offsetGreen, layer->layerP->lights.header.offsetBlue, layer->layerP->lights.header.offsetWhite);  // 102 is GRB

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