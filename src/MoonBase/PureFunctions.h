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

inline uint32_t fastDiv255(uint32_t x) {  // 3-4 cycles
  return (x * 0x8081u) >> 23;
}

inline float distance(float x1, float y1, float z1, float x2, float y2, float z2) {
  return sqrtf((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2) + (z1 - z2) * (z1 - z2));
}

inline void extractPath(const char* filepath, char* path) {
  const char* lastSlash = strrchr(filepath, '/');
  if (lastSlash != NULL) {
    size_t pathLength = lastSlash - filepath + 1;
    strlcpy(path, filepath, pathLength);
    // path[pathLength] = '\0'; // strlcpy does this
  } else {
    // No directory separator found, the entire filepath is the filename
    strcpy(path, "");
  }
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
