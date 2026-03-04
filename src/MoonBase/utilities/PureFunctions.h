/**
    @title     MoonBase
    @file      PureFunctions.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.

    Portable pure functions with zero ESP32/ArduinoJson dependencies.
    Can be included in native unit tests.
**/

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>

// for unit testing
#ifndef ARDUINO
  #include <string>
  using String = std::string;
// #else
//   #include <WString.h>
#endif

inline uint32_t fastDiv255(uint32_t x) {  // 3-4 cycles
  return (x * 0x8081u) >> 23;
}

inline float distance(float x1, float y1, float z1, float x2, float y2, float z2) {
  return sqrtf((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2) + (z1 - z2) * (z1 - z2));
}

inline size_t extractPath(const char* filepath, char* path, size_t pathSize) {
  if (pathSize == 0) return 0;
  const char* lastSlash = strrchr(filepath, '/');
  if (lastSlash != nullptr) {
    size_t pathLen = static_cast<size_t>(lastSlash - filepath);
    size_t toCopy = pathLen < pathSize - 1 ? pathLen : pathSize - 1;
    memcpy(path, filepath, toCopy);
    path[toCopy] = '\0';
    return toCopy;
  }
  path[0] = '\0';
  return 0;
}

// for game of live
inline uint16_t crc16(const unsigned char* data_p, size_t length) {
  uint8_t x;
  uint16_t crc = 0xFFFF;
  if (!length) return 0x1D0F;
  while (length--) {
    x = crc >> 8 ^ *data_p++;
    x ^= x >> 4;
    crc = (crc << 8) ^ ((uint16_t)(x << 12)) ^ ((uint16_t)(x << 5)) ^ ((uint16_t)x);
  }
  return crc;
}

inline uint16_t gcd(uint16_t a, uint16_t b) {
  while (b != 0) {
    uint16_t t = b;
    b = a % b;
    a = t;
  }
  return a;
}

inline uint16_t lcm(uint16_t a, uint16_t b) {
  if (a == 0 || b == 0) return 0;
  return a / gcd(a, b) * b;
}

inline bool getBitValue(const uint8_t* byteArray, size_t n) {
  size_t byteIndex = n / 8;
  size_t bitIndex = n % 8;
  uint8_t byte = byteArray[byteIndex];
  return (byte >> bitIndex) & 1;
}

inline void setBitValue(uint8_t* byteArray, size_t n, bool value) {
  size_t byteIndex = n / 8;
  size_t bitIndex = n % 8;
  if (value)
    byteArray[byteIndex] |= (1 << bitIndex);
  else
    byteArray[byteIndex] &= ~(1 << bitIndex);
}

// convenience function to compare two char strings
inline bool equal(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  return strcmp(a, b) == 0;
}

// compare two char strings, ignoring non-alphanumeric characters
inline bool equalAZaz09(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;

  while (*a || *b) {
    while (*a && !((*a >= '0' && *a <= '9') || (*a >= 'A' && *a <= 'Z') || (*a >= 'a' && *a <= 'z'))) a++;
    while (*b && !((*b >= '0' && *b <= '9') || (*b >= 'A' && *b <= 'Z') || (*b >= 'a' && *b <= 'z'))) b++;
    if (*a != *b) return false;
    if (*a) {
      a++;
      b++;
    }
  }
  return true;
}

inline bool contains(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  return strstr(a, b) != nullptr;
}

// Dimension constants (used by Nodes and VirtualLayer)
#define _0D 0
#define _1D 1
#define _2D 2
#define _3D 3
#define _NoD 4

/// Builds display name with dimension emoji and tags.
inline String buildNameAndTags(const char* name, uint8_t dim, const char* tags) {
  String result = name;

  if (dim == _0D)
    result += " 💡";
  else if (dim == _1D)
    result += " 📏";
  else if (dim == _2D)
    result += " ⏹️";
  else if (dim == _3D)
    result += " 🧊";

  if (strlen(tags)) {
    result += " ";
    result += tags;
  }

  return result;
}
