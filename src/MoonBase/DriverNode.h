/**
    @title     MoonLight
    @file      DriverNode.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

// This header is included from Nodes.h after the Node base class definition.
// Do not include Nodes.h here to avoid circular includes.

/// Platform-specific LED driver instance, used for brightness LUT, color correction, and LED output.
  #if HP_ALL_DRIVERS
    // #define NUM_LEDS_PER_STRIP 256 not for non virtal... (only setting __delay when NO_WAIT)
    #include "I2SClocklessLedDriver.h"
extern I2SClocklessLedDriver ledsDriver;  // defined in DriverNode.cpp
  #else                                   // ESP32_LEDSDRIVER
    #include "ESP32-LedsDriver.h"
    #define MAX_PINS 20  // this is also defined in ESP32-LedsDriver.h...
     #if  defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)
extern PhysicalDriverESP32S3 ledsDriver;
     #elif  defined(CONFIG_IDF_TARGET_ESP32)
extern PhysicalDriverESP32D0 ledsDriver;
     #else 
extern LedsDriver ledsDriver;
     #endif 
  #endif

/// Enumeration of supported LED/fixture color-channel orderings and multi-channel presets.
/// Each preset defines channelsPerLight and the byte offsets for R, G, B, W, pan, tilt, etc.
enum LightPresetsEnum {
  lightPreset_RGB,
  lightPreset_RBG,
  lightPreset_GRB,  // default WS2812
  lightPreset_GBR,
  lightPreset_BRG,
  lightPreset_BGR,
  lightPreset_RGBW,                // e.g. 4 channel par/dmx light
  lightPreset_GRBW,                // rgbw LED eg. sk6812
  lightPreset_WRGB,                // rgbw ws2814 LEDs
  lightPreset_GRB6,                // some LED curtains
  lightPreset_RGB2040,             // curtain 2040
  lightPreset_RGBWYP,              // 6 channel par/dmx light with UV etc
  lightPreset_MHBeeEyes150W15,     // 15 channels moving head, see https://moonmodules.org/MoonLight/moonlight/drivers/#art-net
  lightPreset_MHBeTopper19x15W32,  // 32 channels moving head
  lightPreset_MH19x15W24,          // 24 channels moving heads
  lightPreset_count
};

/// Base class for LED/fixture driver nodes. Handles light preset selection,
/// brightness/power management via FastLED LUT, and color correction.
/// Concrete drivers (FastLED, Parallel, ArtNet, Hub75) inherit from this.
class DriverNode : public Node {
  uint8_t brightnessSaved = UINT8_MAX;  ///< Cached brightness to detect changes
  uint16_t maxPowerSaved = UINT16_MAX;  ///< Cached max power to detect changes

 protected:
  bool lightPresetSaved = false;  ///< initLeds can only start after lightPreset has been saved

  #if HP_ALL_DRIVERS
  CRGB savedColorCorrection;  ///< Cached color correction for change detection (HP_ALL_DRIVERS)
  bool initDone = false;      ///< Whether the HP driver has been initialized
  #endif

 public:
  /// Populates the lightPreset dropdown control with all supported channel orderings.
  void setup() override;

  /// Applies brightness (with power limiting) and color correction to the LED driver each frame.
  void loop() override;

  /// Reorders RGB(W) channels, applies gamma LUT, and extracts white channel for RGBW fixtures.
  void rgbwBufferMapping(uint8_t* packetRGBChannel, uint8_t* lightsRGBChannel);

  /// Handles lightPreset changes: sets channel offsets and notifies the driver.
  void onUpdate(const Char<20>& oldValue, const JsonObject& control) override;
};

#endif  // FT_MOONLIGHT
