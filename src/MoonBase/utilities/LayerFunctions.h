/**
    @title     MoonLight
    @file      LayerFunctions.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.

    Pure functions for layer bounds computation and coordinate mapping.
    No ESP32/Arduino dependencies — safe for native unit tests.
**/

#pragma once

#include "Coord3D.h"

/// Convert a percentage (0–100) to a pixel coordinate within a dimension size.
inline int pctToPixel(int pct, int dimSize) { return dimSize * pct / 100; }

/// Compute the physical start/end bounds from percentage-based bounds and physical fixture size.
/// Ensures at least 1 pixel in each dimension (avoids zero-size layers).
inline void computeLayerBounds(Coord3D physSize, Coord3D startPct, Coord3D endPct, Coord3D& startPhy, Coord3D& endPhy) {
  startPhy = {pctToPixel(startPct.x, physSize.x), pctToPixel(startPct.y, physSize.y), pctToPixel(startPct.z, physSize.z)};
  endPhy = {pctToPixel(endPct.x, physSize.x), pctToPixel(endPct.y, physSize.y), pctToPixel(endPct.z, physSize.z)};
  // ensure at least 1 pixel in each dimension to avoid zero-size layers
  endPhy = endPhy.maximum(Coord3D(startPhy.x + 1, startPhy.y + 1, startPhy.z + 1));
}

/// Check whether a position is outside the physical bounds [startPhy, endPhy).
inline bool isOutsideLayerBounds(Coord3D position, Coord3D startPhy, Coord3D endPhy) {
  return position.x < startPhy.x || position.x >= endPhy.x ||
         position.y < startPhy.y || position.y >= endPhy.y ||
         position.z < startPhy.z || position.z >= endPhy.z;
}

/// Map a 3-D virtual coordinate to a flat index: x + y * sizeX + z * sizeX * sizeY.
inline int coordToIndex(Coord3D position, Coord3D size) { return position.x + position.y * size.x + position.z * size.x * size.y; }

/// Scale a uint8_t value by two 0–255 factors (e.g. global brightness and layer brightness).
/// Result is in range [0, 255]. Uses rounding to minimise quantisation error.
inline uint8_t scale8x8(uint8_t value, uint8_t factor1, uint8_t factor2) {
  return ((uint32_t)value * factor1 * factor2 + 32512) / 65025;
}
