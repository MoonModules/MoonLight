/**
    @title     MoonLight
    @file      DriverNode.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

  // #include "DriverNode.h" // Nodes.h will include it!
  #include "Nodes.h"

  #if HP_ALL_DRIVERS
I2SClocklessLedDriver ledsDriver;
  #else  // ESP32_LEDSDRIVER
    #if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)
PhysicalDriverESP32S3 ledsDriver;
    #elif defined(CONFIG_IDF_TARGET_ESP32)
PhysicalDriverESP32D0 ledsDriver;
    #else
LedsDriver ledsDriver;
    #endif
  #endif

void DriverNode::setup() {
  addControl(layerP.lights.header.lightPreset, "lightPreset", "select");
  addControlValue("RGB");
  addControlValue("RBG");
  addControlValue("GRB");  // default WS2812
  addControlValue("GBR");
  addControlValue("BRG");
  addControlValue("BGR");
  addControlValue("RGBW");                   // e.g. 4 channel par/dmx light
  addControlValue("GRBW");                   // rgbw LED eg. sk6812
  addControlValue("WRGB");                   // rgbw ws2814 LEDs
  addControlValue("Curtain GRB6");           // some LED curtains
  addControlValue("Curtain RGB2040");        // curtain RGB2040
  addControlValue("Lightbar RGBWYP");        // 6 channel par/dmx light with UV etc
  addControlValue("RGBCCT");                 // 5 channel RGB + cold white + warm white // 🌙
  addControlValue("MH BeeEyes 150W-15");     // 15 channels moving head, see https://moonmodules.org/MoonLight/moonlight/drivers/#art-net
  addControlValue("MH BeTopper 19x15W-32");  // 32 channels moving head
  addControlValue("MH 19x15W-24");           // 24 channels moving heads
}

void DriverNode::loop() {
  LightsHeader* header = &layerP.lights.header;

  // use ledsDriver LUT for super efficient leds dimming 🔥 (used by rgbwBufferMapping)

  uint8_t brightness = (header->offsetBrightness == UINT8_MAX) ? header->brightness : 255;  // set brightness to 255 if offsetBrightness is set (fixture will do its own brightness)

  if (brightness != brightnessSaved || layerP.maxPower != maxPowerSaved) {
    // Use FastLED for setMaxPowerInMilliWatts stuff, don't use if more then 8096 LEDs, decent power is assumed then! Also in case of Art-Net to HUB75 panels this calculation is not using the right mW per LED
    bool canUseFastLedPowerCalc = (header->channelsPerLight == 3) && (layerP.lights.header.nrOfLights <= 8096);
    uint8_t correctedBrightness = canUseFastLedPowerCalc ? calculate_max_brightness_for_power_mW(reinterpret_cast<CRGB*>(layerP.lights.channelsD), layerP.lights.header.nrOfLights, brightness, layerP.maxPower * 1000) : brightness;

    // EXT_LOGD(ML_TAG, "setBrightness b:%d + p:%d -> cb:%d", brightness, layerP.maxPower, correctedBrightness);
    ledsDriver.setBrightness(correctedBrightness);
    brightnessSaved = brightness;
    maxPowerSaved = layerP.maxPower;
  }

  #if HP_ALL_DRIVERS
  if (savedColorCorrection.red != layerP.lights.header.red || savedColorCorrection.green != layerP.lights.header.green || savedColorCorrection.blue != layerP.lights.header.blue) {
    ledsDriver.setGamma(layerP.lights.header.red / 255.0, layerP.lights.header.green / 255.0, layerP.lights.header.blue / 255.0, 1.0);
    // EXT_LOGD(ML_TAG, "setColorCorrection r:%d, g:%d, b:%d (%d %d %d)", layerP.lights.header.red, layerP.lights.header.green, layerP.lights.header.blue,
    // savedColorCorrection.red, savedColorCorrection.green, savedColorCorrection.blue);
    savedColorCorrection.red = layerP.lights.header.red;
    savedColorCorrection.green = layerP.lights.header.green;
    savedColorCorrection.blue = layerP.lights.header.blue;
  }
  #else  // ESP32_LEDSDRIVER
  CRGB correction;
  uint8_t white;
  ledsDriver.getColorCorrection(correction.red, correction.green, correction.blue, white);
  if (correction.red != header->red || correction.green != header->green || correction.blue != header->blue) {
    EXT_LOGD(ML_TAG, "setColorCorrection r:%d, g:%d, b:%d (%d %d %d)", header->red, header->green, header->blue, correction.red, correction.green, correction.blue);
    ledsDriver.setColorCorrection(header->red, header->green, header->blue);
  }
  #endif
}

void DriverNode::onUpdate(const JsonObject& control) {
  LightsHeader* header = &layerP.lights.header;

  // EXT_LOGD(ML_TAG, "%s: %s ", control["name"].as<const char*>(), control["value"].as<String>().c_str());

  if (control["name"] == "lightPreset") {
    uint8_t oldChannelsPerLight = header->channelsPerLight;

    header->resetOffsets();

    switch (header->lightPreset) {
    case lightPreset_RGB:
      header->channelsPerLight = 3;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      break;
    case lightPreset_RBG:
      header->channelsPerLight = 3;
      header->offsetRed = 0;
      header->offsetGreen = 2;
      header->offsetBlue = 1;
      break;
    case lightPreset_GRB:
      header->channelsPerLight = 3;
      header->offsetRed = 1;
      header->offsetGreen = 0;
      header->offsetBlue = 2;
      break;
    case lightPreset_GBR:
      header->channelsPerLight = 3;
      header->offsetRed = 2;
      header->offsetGreen = 0;
      header->offsetBlue = 1;
      break;
    case lightPreset_BRG:
      header->channelsPerLight = 3;
      header->offsetRed = 1;
      header->offsetGreen = 2;
      header->offsetBlue = 0;
      break;
    case lightPreset_BGR:
      header->channelsPerLight = 3;
      header->offsetRed = 2;
      header->offsetGreen = 1;
      header->offsetBlue = 0;
      break;
    case lightPreset_RGBW:
      header->channelsPerLight = 4;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      header->offsetWhite = 3;
      break;
    case lightPreset_GRBW:
      header->channelsPerLight = 4;
      header->offsetRed = 1;
      header->offsetGreen = 0;
      header->offsetBlue = 2;
      header->offsetWhite = 3;
      break;
    case lightPreset_WRGB:
      header->channelsPerLight = 4;
      header->offsetRed = 1;
      header->offsetGreen = 2;
      header->offsetBlue = 3;
      header->offsetWhite = 0;
      break;
    case lightPreset_GRB6:
      header->channelsPerLight = 6;
      header->offsetRed = 1;
      header->offsetGreen = 0;
      header->offsetBlue = 2;
      break;
    case lightPreset_RGB2040:
      // RGB2040 uses standard RGB offsets but has special channel remapping
      // for dual-channel-group architecture (handled in VirtualLayer)
      header->channelsPerLight = 3;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      break;
    case lightPreset_RGBWYP:
      header->channelsPerLight = 6;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      header->offsetWhite = 3;
      break;
    case lightPreset_RGBCCT:  // 🌙
      header->channelsPerLight = 5;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      header->offsetWhite = 3;   // cold white
      header->offsetWhite2 = 4;  // warm white
      break;
    case lightPreset_MHBeeEyes150W15:
      header->channelsPerLight = 15;  // set channels per light to 15 (RGB + Pan + Tilt + Zoom + Brightness)
      header->offsetRGBW = 10;        // set offset for RGB lights in DMX map
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      header->offsetPan = 0;
      header->offsetTilt = 1;
      header->offsetZoom = 7;
      header->offsetBrightness = 8;   // set offset for brightness
      header->offsetGobo = 5;         // set offset for color wheel in DMX map
      header->offsetBrightness2 = 3;  // set offset for color wheel brightness in DMX map    } //BGR
      break;
    case lightPreset_MHBeTopper19x15W32:
      header->channelsPerLight = 32;
      header->offsetRGBW = 9;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      header->offsetRGBW1 = 13;
      header->offsetRGBW2 = 17;
      header->offsetRGBW3 = 24;
      header->offsetPan = 0;
      header->offsetTilt = 2;
      header->offsetZoom = 5;
      header->offsetBrightness = 6;
      break;
    case lightPreset_MH19x15W24:
      header->channelsPerLight = 24;
      header->offsetRGBW = 4;
      header->offsetRed = 0;
      header->offsetGreen = 1;
      header->offsetBlue = 2;
      header->offsetWhite = 3;
      header->offsetPan = 0;
      header->offsetTilt = 1;
      header->offsetBrightness = 3;
      header->offsetRGBW1 = 8;
      header->offsetRGBW2 = 12;
      header->offsetZoom = 17;
      break;
    default:
      EXT_LOGW(ML_TAG, "Invalid lightPreset value: %d", header->lightPreset);
      // Fall back to GRB (most common default)
      header->lightPreset = lightPreset_GRB;
      header->channelsPerLight = 3;
      header->offsetRed = 1;
      header->offsetGreen = 0;
      header->offsetBlue = 2;
      break;
    }

    EXT_LOGD(ML_TAG, "setLightPreset %d (cPL:%d, o:%d,%d,%d,%d)", header->lightPreset, header->channelsPerLight, header->offsetRed, header->offsetGreen, header->offsetBlue, header->offsetWhite);

    // FASTLED_ASSERT(true, "oki");

  #if HP_ALL_DRIVERS
    #ifndef CONFIG_IDF_TARGET_ESP32P4
    if (initDone) {
      // ledsDriver.setOffsets(header->offsetRed, header->offsetGreen, header->offsetBlue, header->offsetWhite);

      // if (oldChannelsPerLight != header->channelsPerLight)
      //   restartNeeded = true; //in case
    }
    #endif

  #else  // ESP32_LEDSDRIVER
    if (ledsDriver.initLedsDone) {
      ledsDriver.setOffsets(header->offsetRed, header->offsetGreen, header->offsetBlue, header->offsetWhite);

      if (oldChannelsPerLight != header->channelsPerLight) restartNeeded = true;  // in case
    }
  #endif

    lightPresetSaved = true;
  }
}

void DriverNode::rgbwBufferMapping(uint8_t* packetRGBChannel, uint8_t* lightsRGBChannel) {
  // use ledsDriver.__rbg_map[0]; for super fast brightness and gamma correction! see secondPixel in ESP32-LedDriver!
  // apply the LUT to the RGB channels !

  uint8_t red = lightsRGBChannel[0];
  uint8_t green = lightsRGBChannel[1];
  uint8_t blue = lightsRGBChannel[2];
  // extract White from RGB
  if (layerP.lights.header.offsetWhite != UINT8_MAX) {
    // if white is filled, use that and do not extract rgbw
    uint8_t white = lightsRGBChannel[3];
    if (!white) {
      white = MIN(MIN(red, green), blue);
      red -= white;
      green -= white;
      blue -= white;
    }
    packetRGBChannel[layerP.lights.header.offsetWhite] = ledsDriver.__white_map[white];
  }

  packetRGBChannel[layerP.lights.header.offsetRed] = ledsDriver.__red_map[red];
  packetRGBChannel[layerP.lights.header.offsetGreen] = ledsDriver.__green_map[green];
  packetRGBChannel[layerP.lights.header.offsetBlue] = ledsDriver.__blue_map[blue];
}

#endif  // FT_MOONLIGHT
