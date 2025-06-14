/**
    @title     MoonLight
    @file      LedsDriver.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/general/utilities/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact moonmodules@icloud.com
**/

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

#include "LedsDriver.h"

#include "PhysicalLayer.h"

#ifdef HP_PHYSICAL_DRIVER
  #define NUMSTRIPS 16 //can this be changed e.g. when we have 20 pins?
  #define NUM_LEDS_PER_STRIP 256 //could this be removed from driver lib as makes not so much sense
  #if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
    #include "I2SClockLessLedDriveresp32s3.h"
    static I2SClocklessLedDriveresp32S3 driver;
  #else
    #include "I2SClocklessLedDriver.h"
    static I2SClocklessLedDriver driver;
  #endif

  void LedsDriver::init(Lights &lights, const std::vector<SortedPin> &sortedPins) {
    int pins[16]; //max 16 pins
    int lengths[16];
    int nb_pins=0;

    for (const SortedPin &sortedPin : sortedPins) {
      ESP_LOGD(TAG, "sortpins s:%d #:%d p:%d", sortedPin.startLed, sortedPin.nrOfLights, sortedPin.pin);
      if (nb_pins < NUMSTRIPS) {
        pins[nb_pins] = sortedPin.pin;
        lengths[nb_pins] = sortedPin.nrOfLights;
        nb_pins++;
      }
    }

    //fill the rest of the pins with the pins already used
    //preferably NUMSTRIPS is a variable...
    for (uint8_t i = nb_pins; i < NUMSTRIPS; i++) {
      pins[i] = pins[i%nb_pins];
      lengths[i] = 0;
    }
    ESP_LOGD(TAG, "pins[");
    for (int i=0; i<nb_pins; i++) {
      ESP_LOGD(TAG, ", %d", pins[i]);
    }
    ESP_LOGD(TAG, "]\n");

    if (nb_pins > 0) {
      #if CONFIG_IDF_TARGET_ESP32S3 | CONFIG_IDF_TARGET_ESP32S2
        driver.initled((uint8_t*) lights.leds, pins, nb_pins, lengths[0]); //s3 doesn't support lengths so we pick the first
        //void initled( uint8_t * leds, int * pins, int numstrip, int NUM_LED_PER_STRIP)
      #else
        driver.initled((uint8_t*) lights.leds, pins, lengths, nb_pins, (colorarrangment)colorOrder);
        #if ML_LIVE_MAPPING
          driver.setMapLed(&mapLed);
        #endif
        //void initled(uint8_t *leds, int *Pinsq, int *sizes, int num_strips, colorarrangment cArr)
      #endif
    //   Variable("Fixture", "brightness").triggerEvent(onChange, UINT8_MAX, true); //set brightness (init is true so bri value not send via udp)
      setBrightness(setMaxPowerBrightnessFactor / 256); //not brighter then the set limit (WIP)
    }
  }

  void LedsDriver::setBrightness(uint8_t brightness) {
    driver.setBrightness(brightness * setMaxPowerBrightnessFactor / 256);
  }

  void LedsDriver::setColorCorrection(uint8_t red, uint8_t green, uint8_t blue) {
      driver.setGamma(red/255.0, green/255.0, blue/255.0);
  }

  void LedsDriver::show() {
    // if statement needed as we need to wait until the driver is initialised
    #if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
      if (driver.ledsbuff != nullptr)
        driver.show();
    #else
      if (driver.total_leds > 0)
        driver.showPixels(WAIT);
    #endif
  }

  void LedsDriver::preKill() {}
  void LedsDriver::postKill() {}
  
#elif HP_VIRTUAL_DRIVER //see https://github.com/ewowi/I2SClocklessVirtualLedDriver read me
  
  // #define NUM_LEDS_PER_STRIP 256 // for I2S_MAPPING_MODE_OPTION_MAPPING_IN_MEMORY ...

  #undef CORE_DEBUG_LEVEL
  #define CORE_DEBUG_LEVEL 0 //surpress ESP_LOGE compile error, increase to 6 when debugging
  //catch errors from library, enable when debugging
  // #define ICVD_LOGD(tag, format, ...) ppf(format, ##__VA_ARGS__)
  // #define ICVD_LOGE(tag, format, ...) ppf(format, ##__VA_ARGS__)
  // #define ICVD_LOGV(tag, format, ...) ppf(format, ##__VA_ARGS__)
  // #define ICVD_LOGI(tag, format, ...) ppf(format, ##__VA_ARGS__)

  #define USE_FASTLED //so CRGB is supported e.g. in initLed
  // #define __BRIGHTNESS_BIT 5 //underscore ! default 8, set off for the moment as ui brightness stopped working, will look at it later. 
                                //the max brightness will be 2^5=32 If you remember when I have discussed about the fact that the showPixels is not always occupied with gives time for other processes to run. Well the less time we 'spent' in buffer calcualtion the better.for instance if you do not use gamma calculation and you can cope with a brightness that is a power of 2:
  // #include "esp_heap_caps.h"
  #if STARBASE_USERMOD_LIVE & HP_LIVE_MAPPING
    #define I2S_MAPPING_MODE (I2S_MAPPING_MODE_OPTION_MAPPING_SOFTWARE) //works no flickering anymore (due to __NB_DMA_BUFFER)!
    // #define I2S_MAPPING_MODE (I2S_MAPPING_MODE_OPTION_MAPPING_IN_MEMORY) //not working: IllegalInstruction Backtrace: 0x5515d133:0x3ffb1fc0 |<-CORRUPTED
    // #define _DMA_EXTENSTION 64 //not needed (yet)
  #else
    #define I2S_MAPPING_MODE (I2S_MAPPING_MODE_OPTION_NONE) //works but mapping using MoonLight mappingTable needed
  #endif

  #undef TAG
  #define TAG "🐸" // for S3 (todo also for non s3...)

  #include "I2SClocklessVirtualLedDriver.h"
  static I2SClocklessVirtualLedDriver driver;

  #define maxNrOfPins 8 // max 8 shift registers is 8 * 2048 is 16384 leds
  void LedsDriver::init(Lights &lights, const std::vector<SortedPin> &sortedPins) {
    int pins[maxNrOfPins]; //each pin going to shift register
    int lengths[maxNrOfPins];
    int nb_pins=0;

    for (const SortedPin &sortedPin : sortedPins) {
      ESP_LOGD(TAG, "sortpins s:%d #:%d p:%d\n", sortedPin.startLed, sortedPin.nrOfLights, sortedPin.pin);
      if (nb_pins < maxNrOfPins) {
        pins[nb_pins] = sortedPin.pin;
        lengths[nb_pins] = sortedPin.nrOfLights;
        nb_pins++;
      }
    }

    // //fill the rest of the pins with the pins already used
    // for (uint8_t i = nb_pins; i < maxNrOfPins; i++) {
    //   pins[i] = pins[i%nb_pins];
    //   lengths[i] = 0;
    // }
    ESP_LOGD(TAG, "pins[");
    for (int i=0; i<nb_pins; i++) {
      ESP_LOGD(TAG, ", %d", pins[i]);
    }
    ESP_LOGD(TAG, "]\n");

    for (int i=0; i< MAX_LEDS; i++) lights.leds[i] = CRGB::Black; //avoid very bright pixels during reboot (WIP)

    // pinsM->allocatePin(clockPin, "Leds", "Clock");
    // pinsM->allocatePin(latchPin, "Leds", "Latch");
    
    #if CONFIG_IDF_TARGET_ESP32S3
      if (driver.driverInit) {
        NUM_LEDS_PER_STRIP = lengths[0]/8; //each shift register feeds 8 panels
        NBIS2SERIALPINS = sortedPins.size();
        driver._clockspeed = clockFreq==10?clock_1000KHZ:clockFreq==11?clock_1111KHZ:clockFreq==12?clock_1123KHZ:clock_800KHZ;
        driver.setPins(pins, clockPin, latchPin);
      } else
        driver.initled(lights.leds, pins, clockPin, latchPin, lengths[0]/8, sortedPins.size(), clockFreq==10?clock_1000KHZ:clockFreq==11?clock_1111KHZ:clockFreq==12?clock_1123KHZ:clock_800KHZ);
    #else
      if (driver.driverInit) {
        NUM_LEDS_PER_STRIP = lengths[0]/8; //each shift register feeds 8 panels
        NBIS2SERIALPINS = sortedPins.size();
        driver.setPins(pins, clockPin, latchPin);
      } else
        driver.initled(lights.leds, pins, clockPin, latchPin, lengths[0]/8, sortedPins.size());
    #endif

    // driver.setColorOrderPerStrip(0, (colorarrangment)colorOrder); //to be implemented...

    // driver.enableShowPixelsOnCore(1);
    #if ML_LIVE_MAPPING
      driver.setMapLed(&mapLed);
    #endif

    // if (driver.driverInit) driver.showPixels(WAIT);  //avoid very bright pixels during reboot (WIP)
    driver.setBrightness(10); //avoid very bright pixels during reboot (WIP)

    // Variable("Fixture", "brightness").triggerEvent(onChange, UINT8_MAX, true); //set brightness (init is true so bri value not send via udp)
  }

  void LedsDriver::setBrightness(uint8_t brightness) {
    driver.setBrightness(brightness);
  }

  void LedsDriver::setColorCorrection(uint8_t red, uint8_t green, uint8_t blue) {
      driver.setGamma(red/255.0, green/255.0, blue/255.0);
  }

  void LedsDriver::show(){
    // if statement needed as we need to wait until the driver is initialised
    if (driver.driverInit)
      driver.showPixels(WAIT);
  }

  void LedsDriver::preKill()
  {
    ESP_LOGD(TAG, "");
    // LEDS specific
    //tbd: move this to LedModFixture...
    #if HP_PHYSICAL_DRIVER || HP_VIRTUAL_DRIVER
      driver.__enableDriver = false;
      while (driver.isDisplaying) {};
      //delay(20);
    #endif
  }
  void LedsDriver::postKill()
  {
    ESP_LOGD(TAG, "");
    // LEDS specific
    #if HP_PHYSICAL_DRIVER || HP_VIRTUAL_DRIVER
      // delay(10);
      driver.__enableDriver = true;
    #endif
  }

#elif MM_HUB75_DRIVER
  #include "WhateverHubDriver.h"
  static WhateverHubDriver driver;

  void LedsDriver::init(Lights &lights, const std::vector<SortedPin> &sortedPins) {
  }
  void LedsDriver::setBrightness(uint8_t brightness) {
  }
  void LedsDriver::show(){
  }
  void LedsDriver::preKill() {}
  void LedsDriver::postKill() {}

#else //FastLED driver

  void LedsDriver::init(Lights &lights, const std::vector<SortedPin> &sortedPins) {
    for (const SortedPin &sortedPin : sortedPins) {
      ESP_LOGD(TAG, "sortpins s:%d #:%d p:%d", sortedPin.startLed, sortedPin.nrOfLights, sortedPin.pin);

      uint16_t startLed = sortedPin.startLed;
      uint16_t nrOfLights = sortedPin.nrOfLights;
      uint16_t pin = sortedPin.pin;

      switch (sortedPin.pin) {
      #if CONFIG_IDF_TARGET_ESP32
        case 0: FastLED.addLeds<ML_CHIPSET, 0 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 1: FastLED.addLeds<ML_CHIPSET, 1 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 2: FastLED.addLeds<ML_CHIPSET, 2 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 3: FastLED.addLeds<ML_CHIPSET, 3 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 4: FastLED.addLeds<ML_CHIPSET, 4 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 5: FastLED.addLeds<ML_CHIPSET, 5 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 6: FastLED.addLeds<ML_CHIPSET, 6 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 7: FastLED.addLeds<ML_CHIPSET, 7 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 8: FastLED.addLeds<ML_CHIPSET, 8 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 9: FastLED.addLeds<ML_CHIPSET, 9 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 10: FastLED.addLeds<ML_CHIPSET, 10 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 11: FastLED.addLeds<ML_CHIPSET, 11 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 12: FastLED.addLeds<ML_CHIPSET, 12 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 13: FastLED.addLeds<ML_CHIPSET, 13 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 14: FastLED.addLeds<ML_CHIPSET, 14 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 15: FastLED.addLeds<ML_CHIPSET, 15 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
    #if !defined(BOARD_HAS_PSRAM) && !defined(ARDUINO_ESP32_PICO)
        // 16+17 = reserved for PSRAM, or reserved for FLASH on pico-D4
        case 16: FastLED.addLeds<ML_CHIPSET, 16 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 17: FastLED.addLeds<ML_CHIPSET, 17 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
    #endif
        case 18: FastLED.addLeds<ML_CHIPSET, 18 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 19: FastLED.addLeds<ML_CHIPSET, 19 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 20: FastLED.addLeds<ML_CHIPSET, 20 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 21: FastLED.addLeds<ML_CHIPSET, 21 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 22: FastLED.addLeds<ML_CHIPSET, 22 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 23: FastLED.addLeds<ML_CHIPSET, 23 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 24: FastLED.addLeds<ML_CHIPSET, 24 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 25: FastLED.addLeds<ML_CHIPSET, 25 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 26: FastLED.addLeds<ML_CHIPSET, 26 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 27: FastLED.addLeds<ML_CHIPSET, 27 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 28: FastLED.addLeds<ML_CHIPSET, 28 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 29: FastLED.addLeds<ML_CHIPSET, 29 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 30: FastLED.addLeds<ML_CHIPSET, 30 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 31: FastLED.addLeds<ML_CHIPSET, 31 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 32: FastLED.addLeds<ML_CHIPSET, 32 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 33: FastLED.addLeds<ML_CHIPSET, 33 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // 34-39 input-only
        // case 34: FastLED.addLeds<ML_CHIPSET, 34 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 35: FastLED.addLeds<ML_CHIPSET, 35 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 36: FastLED.addLeds<ML_CHIPSET, 36 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 37: FastLED.addLeds<ML_CHIPSET, 37 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 38: FastLED.addLeds<ML_CHIPSET, 38 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 39: FastLED.addLeds<ML_CHIPSET, 39 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
      #endif //CONFIG_IDF_TARGET_ESP32

      #if CONFIG_IDF_TARGET_ESP32S2
        case 0: FastLED.addLeds<ML_CHIPSET, 0 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 1: FastLED.addLeds<ML_CHIPSET, 1 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 2: FastLED.addLeds<ML_CHIPSET, 2 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 3: FastLED.addLeds<ML_CHIPSET, 3 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 4: FastLED.addLeds<ML_CHIPSET, 4 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 5: FastLED.addLeds<ML_CHIPSET, 5 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 6: FastLED.addLeds<ML_CHIPSET, 6 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 7: FastLED.addLeds<ML_CHIPSET, 7 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 8: FastLED.addLeds<ML_CHIPSET, 8 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 9: FastLED.addLeds<ML_CHIPSET, 9 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 10: FastLED.addLeds<ML_CHIPSET, 10 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 11: FastLED.addLeds<ML_CHIPSET, 11 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 12: FastLED.addLeds<ML_CHIPSET, 12 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 13: FastLED.addLeds<ML_CHIPSET, 13 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 14: FastLED.addLeds<ML_CHIPSET, 14 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 15: FastLED.addLeds<ML_CHIPSET, 15 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 16: FastLED.addLeds<ML_CHIPSET, 16 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 17: FastLED.addLeds<ML_CHIPSET, 17 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 18: FastLED.addLeds<ML_CHIPSET, 18 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
    #if !ARDUINO_USB_CDC_ON_BOOT
        // 19 + 20 = USB HWCDC. reserved for USB port when ARDUINO_USB_CDC_ON_BOOT=1
        case 19: FastLED.addLeds<ML_CHIPSET, 19 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 20: FastLED.addLeds<ML_CHIPSET, 20 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
    #endif
        case 21: FastLED.addLeds<ML_CHIPSET, 21 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // 22 to 32: not connected, or reserved for SPI FLASH
        // case 22: FastLED.addLeds<ML_CHIPSET, 22 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 23: FastLED.addLeds<ML_CHIPSET, 23 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 24: FastLED.addLeds<ML_CHIPSET, 24 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 25: FastLED.addLeds<ML_CHIPSET, 25 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
    #if !defined(BOARD_HAS_PSRAM)
        // 26-32 = reserved for PSRAM
        case 26: FastLED.addLeds<ML_CHIPSET, 26 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 27: FastLED.addLeds<ML_CHIPSET, 27 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 28: FastLED.addLeds<ML_CHIPSET, 28 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 29: FastLED.addLeds<ML_CHIPSET, 29 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 30: FastLED.addLeds<ML_CHIPSET, 30 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 31: FastLED.addLeds<ML_CHIPSET, 31 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 32: FastLED.addLeds<ML_CHIPSET, 32 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
    #endif
        case 33: FastLED.addLeds<ML_CHIPSET, 33 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 34: FastLED.addLeds<ML_CHIPSET, 34 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 35: FastLED.addLeds<ML_CHIPSET, 35 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 36: FastLED.addLeds<ML_CHIPSET, 36 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 37: FastLED.addLeds<ML_CHIPSET, 37 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 38: FastLED.addLeds<ML_CHIPSET, 38 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 39: FastLED.addLeds<ML_CHIPSET, 39 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 40: FastLED.addLeds<ML_CHIPSET, 40 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 41: FastLED.addLeds<ML_CHIPSET, 41 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 42: FastLED.addLeds<ML_CHIPSET, 42 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 43: FastLED.addLeds<ML_CHIPSET, 43 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 44: FastLED.addLeds<ML_CHIPSET, 44 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 45: FastLED.addLeds<ML_CHIPSET, 45 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // 46 input-only
        // case 46: FastLED.addLeds<ML_CHIPSET, 46 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
      #endif //CONFIG_IDF_TARGET_ESP32S2

      #if CONFIG_IDF_TARGET_ESP32C3
        case 0: FastLED.addLeds<ML_CHIPSET, 0 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 1: FastLED.addLeds<ML_CHIPSET, 1 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 2: FastLED.addLeds<ML_CHIPSET, 2 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 3: FastLED.addLeds<ML_CHIPSET, 3 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 4: FastLED.addLeds<ML_CHIPSET, 4 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 5: FastLED.addLeds<ML_CHIPSET, 5 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 6: FastLED.addLeds<ML_CHIPSET, 6 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 7: FastLED.addLeds<ML_CHIPSET, 7 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 8: FastLED.addLeds<ML_CHIPSET, 8 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 9: FastLED.addLeds<ML_CHIPSET, 9 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 10: FastLED.addLeds<ML_CHIPSET, 10 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // 11-17 reserved for SPI FLASH
        //case 11: FastLED.addLeds<ML_CHIPSET, 11 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        //case 12: FastLED.addLeds<ML_CHIPSET, 12 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        //case 13: FastLED.addLeds<ML_CHIPSET, 13 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        //case 14: FastLED.addLeds<ML_CHIPSET, 14 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        //case 15: FastLED.addLeds<ML_CHIPSET, 15 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        //case 16: FastLED.addLeds<ML_CHIPSET, 16 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        //case 17: FastLED.addLeds<ML_CHIPSET, 17 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
    #if !ARDUINO_USB_CDC_ON_BOOT
        // 18 + 19 = USB HWCDC. reserved for USB port when ARDUINO_USB_CDC_ON_BOOT=1
        case 18: FastLED.addLeds<ML_CHIPSET, 18 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 19: FastLED.addLeds<ML_CHIPSET, 19 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
    #endif
        // 20+21 = Serial RX+TX --> don't use for LEDS when serial-to-USB is needed
        case 20: FastLED.addLeds<ML_CHIPSET, 20 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 21: FastLED.addLeds<ML_CHIPSET, 21 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
      #endif //CONFIG_IDF_TARGET_ESP32S2

      #if CONFIG_IDF_TARGET_ESP32S3
        case 0: FastLED.addLeds<ML_CHIPSET, 0 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 1: FastLED.addLeds<ML_CHIPSET, 1 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 2: FastLED.addLeds<ML_CHIPSET, 2 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 3: FastLED.addLeds<ML_CHIPSET, 3 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 4: FastLED.addLeds<ML_CHIPSET, 4 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 5: FastLED.addLeds<ML_CHIPSET, 5 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 6: FastLED.addLeds<ML_CHIPSET, 6 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 7: FastLED.addLeds<ML_CHIPSET, 7 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 8: FastLED.addLeds<ML_CHIPSET, 8 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 9: FastLED.addLeds<ML_CHIPSET, 9 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 10: FastLED.addLeds<ML_CHIPSET, 10 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 11: FastLED.addLeds<ML_CHIPSET, 11 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 12: FastLED.addLeds<ML_CHIPSET, 12 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 13: FastLED.addLeds<ML_CHIPSET, 13 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 14: FastLED.addLeds<ML_CHIPSET, 14 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 15: FastLED.addLeds<ML_CHIPSET, 15 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 16: FastLED.addLeds<ML_CHIPSET, 16 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 17: FastLED.addLeds<ML_CHIPSET, 17 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 18: FastLED.addLeds<ML_CHIPSET, 18 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
      #if !ARDUINO_USB_CDC_ON_BOOT
        // 19 + 20 = USB-JTAG. Not recommended for other uses.
        case 19: FastLED.addLeds<ML_CHIPSET, 19 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 20: FastLED.addLeds<ML_CHIPSET, 20 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
      #endif
        case 21: FastLED.addLeds<ML_CHIPSET, 21 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // // 22 to 32: not connected, or SPI FLASH
        // case 22: FastLED.addLeds<ML_CHIPSET, 22 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 23: FastLED.addLeds<ML_CHIPSET, 23 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 24: FastLED.addLeds<ML_CHIPSET, 24 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 25: FastLED.addLeds<ML_CHIPSET, 25 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 26: FastLED.addLeds<ML_CHIPSET, 26 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 27: FastLED.addLeds<ML_CHIPSET, 27 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 28: FastLED.addLeds<ML_CHIPSET, 28 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 29: FastLED.addLeds<ML_CHIPSET, 29 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 30: FastLED.addLeds<ML_CHIPSET, 30 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 31: FastLED.addLeds<ML_CHIPSET, 31 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // case 32: FastLED.addLeds<ML_CHIPSET, 32 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
      #if !defined(BOARD_HAS_PSRAM)
        // 33 to 37: reserved if using _octal_ SPI Flash or _octal_ PSRAM
        case 33: FastLED.addLeds<ML_CHIPSET, 33 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 34: FastLED.addLeds<ML_CHIPSET, 34 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 35: FastLED.addLeds<ML_CHIPSET, 35 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 36: FastLED.addLeds<ML_CHIPSET, 36 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 37: FastLED.addLeds<ML_CHIPSET, 37 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
      #endif
        case 38: FastLED.addLeds<ML_CHIPSET, 38 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 39: FastLED.addLeds<ML_CHIPSET, 39 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 40: FastLED.addLeds<ML_CHIPSET, 40 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 41: FastLED.addLeds<ML_CHIPSET, 41 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 42: FastLED.addLeds<ML_CHIPSET, 42 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        // 43+44 = Serial RX+TX --> don't use for LEDS when serial-to-USB is needed
        case 43: FastLED.addLeds<ML_CHIPSET, 43 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 44: FastLED.addLeds<ML_CHIPSET, 44 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 45: FastLED.addLeds<ML_CHIPSET, 45 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 46: FastLED.addLeds<ML_CHIPSET, 46 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 47: FastLED.addLeds<ML_CHIPSET, 47 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
        case 48: FastLED.addLeds<ML_CHIPSET, 48 COLOR_ORDER_ARG>(lights.leds, startLed, nrOfLights).setCorrection(TypicalLEDStrip) RGBW_CALL; break;
      #endif //CONFIG_IDF_TARGET_ESP32S3

      default: ESP_LOGW(TAG, "FastLEDPin assignment: pin not supported %d", sortedPin.pin);
      } //switch pinNr

      FastLED.setMaxPowerInMilliWatts(10000); // 5v, 2000mA, to protect usb while developing

    } //sortedPins
  }

  void LedsDriver::setBrightness(uint8_t brightness) {
    FastLED.setBrightness(brightness);
  }

  void LedsDriver::setColorCorrection(uint8_t red, uint8_t green, uint8_t blue) {
    //FastLED Led Controllers
    CLEDController *pCur = CLEDController::head();
    while( pCur) {
        // ++x;
        pCur->setCorrection(CRGB(red, green, blue));
        // pCur->size();
        pCur = pCur->next();
    }
}

  void LedsDriver::show() {
    if (FastLED.count()) {
        FastLED.show();
    }
  }
  void LedsDriver::preKill() {}
  void LedsDriver::postKill() {}

#endif
#endif //FT_MOONLIGHT
