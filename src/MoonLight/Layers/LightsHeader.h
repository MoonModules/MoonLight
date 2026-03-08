/**
    @title     MoonLight
    @file      LightsHeader.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.

    Pure types for the physical light channel system.
    This header has NO ESP32, FreeRTOS, or FastLED dependencies and can be
    included in native (host) unit tests directly.
**/

#pragma once

#include <cstdint>
#include <cstring>

#include "MoonBase/utilities/Coord3D.h"

// Maximum number of LED strips for the Parallel LED Driver.
#define MAXLEDPINS 20

// ----------------------------------------------------------------------------
// nrOfLights_t — index type for physical lights.
// uint32_t on boards with PSRAM, uint16_t otherwise.
// ----------------------------------------------------------------------------
#ifdef BOARD_HAS_PSRAM
typedef uint32_t nrOfLights_t;
  #define nrOfLights_t_MAX UINT32_MAX
#else
typedef uint16_t nrOfLights_t;
  #define nrOfLights_t_MAX UINT16_MAX
#endif

// ----------------------------------------------------------------------------
// LightsHeader — fixed-size metadata block at the front of the channel array.
// Layout must remain stable: Monitor.svelte reads it directly over WebSocket.
// Do not reorder or insert fields without updating the frontend.
// Data: 12 (Coord3D) + 4 + 4 (uint32_t) + 22 (uint8_t fields) + 3 (fill) = 45 bytes.
// sizeof(LightsHeader) == 48 (compiler pads to 4-byte alignment).
// ModuleLightsControl sends exactly 47 bytes (headerPrimeNumber) over the socket.
// ----------------------------------------------------------------------------
struct LightsHeader {
  Coord3D size = Coord3D(16, 16, 1);  // 0  max position of lights (updated by onLayout passes). Default non-zero to avoid divide-by-zero in effects.
  uint32_t nrOfLights = 256;          // 12 number of physical lights, counted by addLight
  uint32_t nrOfChannels = 0;          // 16 total channel count (nrOfLights × channelsPerLight). Must stay at an even offset for alignment.
  uint8_t isPositions = 0;            // 20 0=normal, 1=layout in progress, 2=positions stored, 3=no lights
  uint8_t lightPreset = 2;            // 21 channel ordering preset (GRB default = 2; see LightPresetsEnum)
  uint8_t brightness = 255;           // 22 global brightness, applied by driver
  uint8_t red = 255;                  // 23 colour correction red channel
  uint8_t green = 255;                // 24 colour correction green channel
  uint8_t blue = 255;                 // 25 colour correction blue channel
  uint8_t channelsPerLight = 3;       // 26 channels per light (RGB=3, RGBW=4, moving heads up to 32)
  uint8_t offsetRGBW = 0;            // 27 byte offset of the primary RGBW block within a light's channels
  uint8_t offsetRed = 1;             // 28 red offset within offsetRGBW block (GRB default: R=1)
  uint8_t offsetGreen = 0;           // 29 green offset within offsetRGBW block (GRB default: G=0)
  uint8_t offsetBlue = 2;            // 30 blue offset within offsetRGBW block (GRB default: B=2)
  uint8_t offsetWhite = UINT8_MAX;   // 31 white channel offset; UINT8_MAX = no white channel
  // PAR lights
  uint8_t offsetBrightness = UINT8_MAX;  // 32 separate brightness channel; UINT8_MAX = not present
  // Moving heads
  uint8_t offsetPan = UINT8_MAX;          // 33
  uint8_t offsetTilt = UINT8_MAX;         // 34
  uint8_t offsetZoom = UINT8_MAX;         // 35
  uint8_t offsetRotate = UINT8_MAX;       // 36
  uint8_t offsetGobo = UINT8_MAX;         // 37
  uint8_t offsetRGBW1 = UINT8_MAX;        // 38 secondary RGBW block
  uint8_t offsetRGBW2 = UINT8_MAX;        // 39
  uint8_t offsetRGBW3 = UINT8_MAX;        // 40
  uint8_t offsetBrightness2 = UINT8_MAX;  // 41
  // =============
  // 42 bytes used + 3 fill bytes = 45 total.
  // Fields above must stay in this order (Coord3D + uint32_t first, uint8_t after)
  // to preserve natural alignment and binary compatibility with Monitor.svelte.
  uint8_t fill[3] = {};

  // Reset all channel offsets to RGB/GRB defaults.
  // Called before each layout pass so the driver can re-configure the preset.
  void resetOffsets() {
    nrOfChannels = 0;
    channelsPerLight = 3;  // RGB default
    // lightPreset is intentionally NOT reset here — managed by Drivers
    offsetRGBW = 0;
    offsetRed = 1;
    offsetGreen = 0;
    offsetBlue = 2;
    offsetWhite = UINT8_MAX;
    offsetBrightness = UINT8_MAX;
    offsetPan = UINT8_MAX;
    offsetTilt = UINT8_MAX;
    offsetZoom = UINT8_MAX;
    offsetRotate = UINT8_MAX;
    offsetGobo = UINT8_MAX;
    offsetRGBW1 = UINT8_MAX;
    offsetRGBW2 = UINT8_MAX;
    offsetRGBW3 = UINT8_MAX;
    offsetBrightness2 = UINT8_MAX;
    memset(fill, 0, sizeof(fill));
  }
};

// ----------------------------------------------------------------------------
// Lights — owns the raw channel byte arrays used for double-buffered rendering.
// channelsE is written by effect nodes; channelsD is read by driver nodes.
// On boards without PSRAM, both pointers alias the same allocation.
// ----------------------------------------------------------------------------
struct Lights {
  LightsHeader header;
  uint8_t* channelsE = nullptr;  // effects write here (double-buffer front)
  uint8_t* channelsD = nullptr;  // drivers read from here (double-buffer back)
  nrOfLights_t maxChannels = 0;
  bool useDoubleBuffer = false;  // true only when PSRAM is available
};
