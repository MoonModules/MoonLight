/**
    @title     MoonLight Layer Unit Tests
    @file      test_layers.cpp
    @repo      https://github.com/MoonModules/MoonLight
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007

    Native unit tests for the pure-type layer headers:
      - LightsHeader.h  (nrOfLights_t, LightsHeader, Lights)
      - PhysMap.h       (MapTypeEnum, PhysMap)

    These headers have no ESP32/FreeRTOS/FastLED dependencies and compile
    on any standard C++17 host.

    Run with: pio test -e native
**/

#include "doctest.h"

// Pure-type headers — no ESP32 deps
#include "MoonLight/Layers/LightsHeader.h"
#include "MoonLight/Layers/PhysMap.h"

// ============================================================
// nrOfLights_t
// ============================================================

TEST_CASE("nrOfLights_t size") {
  // Without BOARD_HAS_PSRAM (native host = no PSRAM) it must be uint16_t.
  // This validates the type alias chosen for memory-constrained boards.
  CHECK(sizeof(nrOfLights_t) == sizeof(uint16_t));
  CHECK(nrOfLights_t_MAX == UINT16_MAX);
}

// ============================================================
// LightsHeader — struct layout and default values
// ============================================================

TEST_CASE("LightsHeader default values") {
  LightsHeader h;

  SUBCASE("size defaults to 16x16x1") {
    CHECK_EQ(h.size.x, 16);
    CHECK_EQ(h.size.y, 16);
    CHECK_EQ(h.size.z, 1);
  }

  SUBCASE("light count defaults") {
    CHECK_EQ(h.nrOfLights, 256u);
    CHECK_EQ(h.nrOfChannels, 0u);
  }

  SUBCASE("isPositions starts at 0 (normal mode)") {
    CHECK_EQ(h.isPositions, 0);
  }

  SUBCASE("lightPreset defaults to 2 (GRB)") {
    CHECK_EQ(h.lightPreset, 2);
  }

  SUBCASE("colour correction defaults to full white") {
    CHECK_EQ(h.brightness, 255);
    CHECK_EQ(h.red, 255);
    CHECK_EQ(h.green, 255);
    CHECK_EQ(h.blue, 255);
  }

  SUBCASE("RGB channel layout defaults (GRB order)") {
    CHECK_EQ(h.channelsPerLight, 3);
    CHECK_EQ(h.offsetRGBW, 0);
    CHECK_EQ(h.offsetRed, 1);    // G=0, R=1, B=2 → GRB
    CHECK_EQ(h.offsetGreen, 0);
    CHECK_EQ(h.offsetBlue, 2);
  }

  SUBCASE("optional channels absent by default (UINT8_MAX sentinel)") {
    CHECK_EQ(h.offsetWhite, UINT8_MAX);
    CHECK_EQ(h.offsetBrightness, UINT8_MAX);
    CHECK_EQ(h.offsetPan, UINT8_MAX);
    CHECK_EQ(h.offsetTilt, UINT8_MAX);
    CHECK_EQ(h.offsetZoom, UINT8_MAX);
    CHECK_EQ(h.offsetRotate, UINT8_MAX);
    CHECK_EQ(h.offsetGobo, UINT8_MAX);
    CHECK_EQ(h.offsetRGBW1, UINT8_MAX);
    CHECK_EQ(h.offsetRGBW2, UINT8_MAX);
    CHECK_EQ(h.offsetRGBW3, UINT8_MAX);
    CHECK_EQ(h.offsetBrightness2, UINT8_MAX);
  }

  SUBCASE("fill bytes are zero-initialised") {
    CHECK_EQ(h.fill[0], 0);
    CHECK_EQ(h.fill[1], 0);
    CHECK_EQ(h.fill[2], 0);
  }
}

TEST_CASE("LightsHeader struct size matches Monitor.svelte binary protocol") {
  // The frontend reads this struct byte-by-byte. Any change in size breaks the UI.
  // Total data: 12 (Coord3D) + 4 (nrOfLights) + 4 (nrOfChannels) + 22 (uint8_t fields) + 3 (fill) = 45 bytes
  // Compiler pads to 4-byte alignment (due to Coord3D/uint32_t members): sizeof = 48.
  // ModuleLightsControl.h sends exactly headerPrimeNumber (47) bytes, verified by static_assert.
  CHECK_EQ(sizeof(LightsHeader), 48u);
  // Must be larger than headerPrimeNumber=47 so the frontend can identify the packet.
  CHECK_GT(sizeof(LightsHeader), 47u);
}

TEST_CASE("LightsHeader field offsets (binary protocol stability)") {
  // Key offsets that Monitor.svelte depends on.
  CHECK_EQ(offsetof(LightsHeader, size), 0u);
  CHECK_EQ(offsetof(LightsHeader, nrOfLights), 12u);
  CHECK_EQ(offsetof(LightsHeader, nrOfChannels), 16u);
  CHECK_EQ(offsetof(LightsHeader, isPositions), 20u);
  CHECK_EQ(offsetof(LightsHeader, lightPreset), 21u);
  CHECK_EQ(offsetof(LightsHeader, brightness), 22u);
  CHECK_EQ(offsetof(LightsHeader, channelsPerLight), 26u);
  CHECK_EQ(offsetof(LightsHeader, offsetRGBW), 27u);
  CHECK_EQ(offsetof(LightsHeader, offsetRed), 28u);
  CHECK_EQ(offsetof(LightsHeader, offsetGreen), 29u);
  CHECK_EQ(offsetof(LightsHeader, offsetBlue), 30u);
  CHECK_EQ(offsetof(LightsHeader, offsetWhite), 31u);
  CHECK_EQ(offsetof(LightsHeader, fill), 42u);
}

// ============================================================
// LightsHeader::resetOffsets
// ============================================================

TEST_CASE("LightsHeader::resetOffsets restores GRB defaults") {
  LightsHeader h;

  // Modify fields to simulate a configured 4-channel RGBW setup
  h.nrOfChannels = 1024;
  h.channelsPerLight = 4;
  h.offsetRGBW = 0;
  h.offsetRed = 0;
  h.offsetGreen = 1;
  h.offsetBlue = 2;
  h.offsetWhite = 3;
  h.offsetPan = 10;
  h.offsetTilt = 11;
  h.fill[0] = 0xFF;

  h.resetOffsets();

  CHECK_EQ(h.nrOfChannels, 0u);
  CHECK_EQ(h.channelsPerLight, 3);
  CHECK_EQ(h.offsetRGBW, 0);
  CHECK_EQ(h.offsetRed, 1);    // GRB: R=1
  CHECK_EQ(h.offsetGreen, 0);  // GRB: G=0
  CHECK_EQ(h.offsetBlue, 2);   // GRB: B=2
  CHECK_EQ(h.offsetWhite, UINT8_MAX);
  CHECK_EQ(h.offsetBrightness, UINT8_MAX);
  CHECK_EQ(h.offsetPan, UINT8_MAX);
  CHECK_EQ(h.offsetTilt, UINT8_MAX);
  CHECK_EQ(h.offsetZoom, UINT8_MAX);
  CHECK_EQ(h.offsetRotate, UINT8_MAX);
  CHECK_EQ(h.offsetGobo, UINT8_MAX);
  CHECK_EQ(h.offsetRGBW1, UINT8_MAX);
  CHECK_EQ(h.offsetRGBW2, UINT8_MAX);
  CHECK_EQ(h.offsetRGBW3, UINT8_MAX);
  CHECK_EQ(h.offsetBrightness2, UINT8_MAX);
  CHECK_EQ(h.fill[0], 0);
}

TEST_CASE("LightsHeader::resetOffsets does not touch lightPreset") {
  // lightPreset is managed exclusively by Drivers — resetOffsets must not touch it.
  LightsHeader h;
  h.lightPreset = 7;  // e.g. lightPreset_GRBW
  h.resetOffsets();
  CHECK_EQ(h.lightPreset, 7);
}

TEST_CASE("LightsHeader::resetOffsets does not touch size or nrOfLights") {
  LightsHeader h;
  h.size = Coord3D(8, 8, 1);
  h.nrOfLights = 64;
  h.resetOffsets();
  CHECK_EQ(h.size.x, 8);
  CHECK_EQ(h.size.y, 8);
  CHECK_EQ(h.nrOfLights, 64u);
}

// ============================================================
// nrOfChannels formula
// The physical channel count must equal nrOfLights × channelsPerLight.
// For RGB2040, each virtual light spans 2× the channels (interleaved empty slots).
// ============================================================

TEST_CASE("nrOfChannels formula") {
  SUBCASE("standard RGB 3 channels per light") {
    uint32_t nrOfLights = 256;
    uint8_t channelsPerLight = 3;
    CHECK_EQ(nrOfLights * channelsPerLight, 768u);
  }

  SUBCASE("RGBW 4 channels per light") {
    uint32_t nrOfLights = 256;
    uint8_t channelsPerLight = 4;
    CHECK_EQ(nrOfLights * channelsPerLight, 1024u);
  }

  SUBCASE("moving head 32 channels per light") {
    uint32_t nrOfLights = 64;
    uint8_t channelsPerLight = 32;
    CHECK_EQ(nrOfLights * channelsPerLight, 2048u);
  }

  SUBCASE("RGB2040 doubles channel count due to interleaved empty slots") {
    uint32_t nrOfLights = 100;
    uint8_t channelsPerLight = 3;
    uint8_t rgb2040Factor = 2;
    CHECK_EQ(nrOfLights * channelsPerLight * rgb2040Factor, 600u);
  }
}

// ============================================================
// Lights struct
// ============================================================

TEST_CASE("Lights struct defaults") {
  Lights l;
  CHECK(l.channelsE == nullptr);
  CHECK(l.channelsD == nullptr);
  CHECK_EQ(l.maxChannels, 0u);
  CHECK_FALSE(l.useDoubleBuffer);
}

// ============================================================
// MapTypeEnum
// ============================================================

TEST_CASE("MapTypeEnum values are stable") {
  // These values are serialised into PhysMap bit fields. Changing them
  // would silently corrupt all existing mapping tables.
  CHECK_EQ(static_cast<int>(m_zeroLights), 0);
  CHECK_EQ(static_cast<int>(m_oneLight), 1);
  CHECK_EQ(static_cast<int>(m_moreLights), 2);
  CHECK_EQ(static_cast<int>(m_count), 3);
}

// ============================================================
// PhysMap
// ============================================================

TEST_CASE("PhysMap constructor initialises to m_zeroLights") {
  PhysMap p;
  CHECK_EQ(p.mapType, static_cast<unsigned>(m_zeroLights));
}

TEST_CASE("PhysMap size matches PSRAM mode") {
  // Without BOARD_HAS_PSRAM (native host): 2 bytes (saves RAM on ESP32-D0).
  // With    BOARD_HAS_PSRAM: 4 bytes (supports 24-bit indexP / up to 16M lights).
#ifdef BOARD_HAS_PSRAM
  CHECK_EQ(sizeof(PhysMap), 4u);
#else
  CHECK_EQ(sizeof(PhysMap), 2u);
#endif
}

TEST_CASE("PhysMap can hold all three map types") {
  PhysMap p;
  CHECK_EQ(p.mapType, static_cast<unsigned>(m_zeroLights));

  p.mapType = m_oneLight;
  CHECK_EQ(p.mapType, static_cast<unsigned>(m_oneLight));

  p.mapType = m_moreLights;
  CHECK_EQ(p.mapType, static_cast<unsigned>(m_moreLights));
}

TEST_CASE("PhysMap indexP round-trip (without PSRAM: 14-bit field)") {
#ifndef BOARD_HAS_PSRAM
  PhysMap p;
  p.mapType = m_oneLight;
  // 14-bit field: max value is 16383
  p.indexP = 16383;
  CHECK_EQ(p.indexP, static_cast<unsigned>(16383));

  p.indexP = 0;
  CHECK_EQ(p.indexP, static_cast<unsigned>(0));

  p.indexP = 100;
  CHECK_EQ(p.indexP, static_cast<unsigned>(100));
#endif
}

TEST_CASE("PhysMap indexesIndex round-trip (without PSRAM: 14-bit field)") {
#ifndef BOARD_HAS_PSRAM
  PhysMap p;
  p.mapType = m_moreLights;
  p.indexesIndex = 255;
  CHECK_EQ(p.indexesIndex, static_cast<unsigned>(255));
#endif
}

TEST_CASE("PhysMap PSRAM: indexP is 24-bit") {
#ifdef BOARD_HAS_PSRAM
  PhysMap p;
  p.mapType = m_oneLight;
  p.indexP = (1 << 24) - 1;  // max 24-bit value
  CHECK_EQ(p.indexP, static_cast<unsigned>((1 << 24) - 1));
#endif
}

// ============================================================
// XYZUnModified coordinate formula
// Verifies the flat index formula: x + y*sx + z*sx*sy
// This is the inner loop of every effect — correctness is critical.
// ============================================================

// Compute flat index from 3-D coordinates and grid size using the same
// formula as VirtualLayer::XYZUnModified, for use in tests.
static uint32_t xyzFlat(int x, int y, int z, int sx, int sy) {
  return static_cast<uint32_t>(x + y * sx + z * sx * sy);
}

TEST_CASE("XYZUnModified formula — 1D strip") {
  // A 16×1×1 strip: index == x
  for (int x = 0; x < 16; x++) {
    CHECK_EQ(xyzFlat(x, 0, 0, 16, 1), static_cast<uint32_t>(x));
  }
}

TEST_CASE("XYZUnModified formula — 2D matrix") {
  // 8×8×1 matrix: row-major order, index = x + y*8
  CHECK_EQ(xyzFlat(0, 0, 0, 8, 8), 0u);
  CHECK_EQ(xyzFlat(7, 0, 0, 8, 8), 7u);
  CHECK_EQ(xyzFlat(0, 1, 0, 8, 8), 8u);
  CHECK_EQ(xyzFlat(7, 7, 0, 8, 8), 63u);
  // Spot check: pixel (3,2) = 3 + 2*8 = 19
  CHECK_EQ(xyzFlat(3, 2, 0, 8, 8), 19u);
}

TEST_CASE("XYZUnModified formula — 3D cube") {
  // 4×4×4 cube: index = x + y*4 + z*16
  CHECK_EQ(xyzFlat(0, 0, 0, 4, 4), 0u);
  CHECK_EQ(xyzFlat(3, 3, 3, 4, 4), 63u);  // last pixel
  // Pixel (1,2,3): 1 + 2*4 + 3*16 = 1 + 8 + 48 = 57
  CHECK_EQ(xyzFlat(1, 2, 3, 4, 4), 57u);
}

TEST_CASE("XYZUnModified formula — default grid 16×16×1") {
  // Default VirtualLayer size matches default LightsHeader size
  const int sx = 16, sy = 16;
  CHECK_EQ(xyzFlat(0, 0, 0, sx, sy), 0u);
  CHECK_EQ(xyzFlat(15, 15, 0, sx, sy), 255u);  // last pixel
  CHECK_EQ(xyzFlat(0, 1, 0, sx, sy), 16u);
  CHECK_EQ(xyzFlat(5, 3, 0, sx, sy), 53u);  // 5 + 3*16 = 53
}

// ============================================================
// presetCorrection formula (RGB2040 interleaving)
// Virtual index n maps to physical index n + (n/20)*20.
// This inserts 20 empty physical channels between each group of 20 virtual lights.
// ============================================================

// Compute the RGB2040 physical index for a virtual index.
// Same formula as VirtualLayer::presetCorrection.
static uint32_t rgb2040Physical(uint32_t indexV) {
  return indexV + (indexV / 20) * 20;
}

TEST_CASE("presetCorrection — RGB2040 interleaving formula") {
  SUBCASE("first group [0..19] maps 1:1 (no gap before it)") {
    for (uint32_t i = 0; i < 20; i++) {
      CHECK_EQ(rgb2040Physical(i), i);
    }
  }

  SUBCASE("virtual 20 maps to physical 40 (first gap)") {
    CHECK_EQ(rgb2040Physical(20), 40u);
  }

  SUBCASE("second group [20..39] offset by 20") {
    for (uint32_t i = 20; i < 40; i++) {
      CHECK_EQ(rgb2040Physical(i), i + 20);
    }
  }

  SUBCASE("virtual 40 maps to physical 80 (second gap)") {
    CHECK_EQ(rgb2040Physical(40), 80u);
  }

  SUBCASE("third group [40..59] offset by 40") {
    for (uint32_t i = 40; i < 60; i++) {
      CHECK_EQ(rgb2040Physical(i), i + 40);
    }
  }

  SUBCASE("group boundaries are exact") {
    // Each group of 20 virtual lights occupies 40 physical channels
    // (20 active + 20 empty). Virtual index k*20 → physical (k*20)*2 = k*40.
    CHECK_EQ(rgb2040Physical(0), 0u);
    CHECK_EQ(rgb2040Physical(20), 40u);
    CHECK_EQ(rgb2040Physical(40), 80u);
    CHECK_EQ(rgb2040Physical(60), 120u);
    CHECK_EQ(rgb2040Physical(100), 200u);
  }
}

// ============================================================
// isMapped logic
// VirtualLayer::isMapped returns true if a virtual pixel has at least one
// physical light mapped to it.
// ============================================================

// Replicate the isMapped logic using PhysMap directly (same logic as
// VirtualLayer::isMapped — tested here without needing VirtualLayer).
static bool isMapped(const PhysMap* table, size_t tableSize, bool oneToOne, uint32_t indexV) {
  return oneToOne || (indexV < tableSize && (table[indexV].mapType == m_oneLight || table[indexV].mapType == m_moreLights));
}

TEST_CASE("isMapped — oneToOneMapping bypasses the table") {
  // When the entire layout is 1:1, isMapped always returns true.
  PhysMap table[4];  // all default to m_zeroLights
  CHECK(isMapped(table, 4, /*oneToOne=*/true, 0));
  CHECK(isMapped(table, 4, /*oneToOne=*/true, 100));  // even out-of-bounds
}

TEST_CASE("isMapped — m_zeroLights means not mapped") {
  PhysMap table[4];  // all default to m_zeroLights
  CHECK_FALSE(isMapped(table, 4, false, 0));
  CHECK_FALSE(isMapped(table, 4, false, 3));
}

TEST_CASE("isMapped — m_oneLight is mapped") {
  PhysMap table[4];
  table[2].mapType = m_oneLight;
  CHECK_FALSE(isMapped(table, 4, false, 0));
  CHECK(isMapped(table, 4, false, 2));
}

TEST_CASE("isMapped — m_moreLights is mapped") {
  PhysMap table[4];
  table[1].mapType = m_moreLights;
  CHECK(isMapped(table, 4, false, 1));
}

TEST_CASE("isMapped — index out of table range is not mapped") {
  PhysMap table[4];
  for (auto& p : table) p.mapType = m_oneLight;
  // index 4 is beyond the table of size 4
  CHECK_FALSE(isMapped(table, 4, false, 4));
  CHECK_FALSE(isMapped(table, 4, false, 100));
}
