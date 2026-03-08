/**
    @title     MoonLight
    @file      PhysMap.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.

    Pure types for the virtual→physical light mapping table.
    This header has NO ESP32, FreeRTOS, or FastLED dependencies and can be
    included in native (host) unit tests directly.
**/

#pragma once

#include <cstdint>
#include <cstring>

#include "LightsHeader.h"  // for nrOfLights_t

// ----------------------------------------------------------------------------
// MapTypeEnum — describes how a virtual light index maps to physical lights.
// Stored in PhysMap::mapType (2 bits on non-PSRAM, 8 bits on PSRAM boards).
// ----------------------------------------------------------------------------
enum MapTypeEnum {
  m_zeroLights,  // virtual pixel has no corresponding physical light
  m_oneLight,    // virtual pixel maps to exactly one physical light (via indexP)
  m_moreLights,  // virtual pixel maps to multiple physical lights (via indexesIndex)
  m_count        // sentinel — keep last
};

// ----------------------------------------------------------------------------
// PhysMap — one entry in the virtual→physical mapping table.
// Compact union: 4 bytes on PSRAM boards, 2 bytes otherwise.
// mapType selects which union member is active.
// ----------------------------------------------------------------------------
struct PhysMap {
  union {
#ifdef BOARD_HAS_PSRAM
    struct {
      uint8_t rgb[3];   // cached RGB value when mapType == m_zeroLights
      uint8_t mapType;
    };
    struct {
      uint32_t indexP : 24;          // physical light index when mapType == m_oneLight
      uint32_t mapType_unused1 : 8;
    };
    struct {
      uint32_t indexesIndex : 24;    // index into mappingTableIndexes when mapType == m_moreLights
      uint32_t mapType_unused2 : 8;
    };
    uint32_t raw;
#else
    // 2-byte struct for boards without PSRAM
    struct {
      uint16_t rgb : 14;     // condensed 554 RGB value when mapType == m_zeroLights
      uint16_t mapType : 2;
    };
    uint16_t indexP : 14;        // physical light index when mapType == m_oneLight (max 16384)
    uint16_t indexesIndex : 14;  // index into mappingTableIndexes when mapType == m_moreLights
#endif
  };

  // Initialises to m_zeroLights with a zeroed RGB cache.
  PhysMap() {
    mapType = m_zeroLights;
#ifdef BOARD_HAS_PSRAM
    memset(rgb, 0, 3);
#else
    rgb = 0;
#endif
  }
};
