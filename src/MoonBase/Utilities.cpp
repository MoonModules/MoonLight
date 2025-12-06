/**
    @title     MoonBase
    @file      Utilities.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#include "Utilities.h"

// ArduinoJson functions

bool arrayContainsValue(JsonArray array, int value) {
  for (JsonVariant v : array) {
    if (v == value) {
      return true;
    }
  }
  return false;
}
int getNextItemInArray(JsonArray array, size_t currentValue, bool backwards) {
  size_t n = array.size();
  if (!n) return -1;

  size_t i = 0;
  while (i < n && array[i] != currentValue) i++;

  size_t next = (i + (backwards ? -1 : 1) + n) % n;
  return array[next];
}

float distance(float x1, float y1, float z1, float x2, float y2, float z2) { return sqrtf((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2) + (z1 - z2) * (z1 - z2)); }

void extractPath(const char* filepath, char* path) {
  const char* lastSlash = strrchr(filepath, '/');
  if (lastSlash != NULL) {
    size_t pathLength = lastSlash - filepath;
    strncpy(path, filepath, pathLength);
    path[pathLength] = '\0';
  } else {
    // No directory separator found, the entire filepath is the filename
    strcpy(path, "");
  }
}

void walkThroughFiles(File folder, std::function<void(File, File)> fun) {
  folder.rewindDirectory();
  while (true) {
    File file = folder.openNextFile();
    if (!file) break;

    fun(folder, file);

    if (file.isDirectory()) {
      walkThroughFiles(file, fun);
    }
    file.close();
  }
}

bool copyFile(const char* srcPath, const char* dstPath) {
  File src = ESPFS.open(srcPath, "r");
  if (!src) {
    Serial.printf("Failed to open source file: %s\n", srcPath);
    return false;
  }
  // Check if the destination folders already exist
  String dstDir = String(dstPath).substring(0, String(dstPath).lastIndexOf('/'));
  if (!ESPFS.exists(dstDir.c_str())) {
    if (!ESPFS.mkdir(dstDir.c_str())) {
      Serial.printf("Failed to create destination directory: %s\n", dstDir.c_str());
      src.close();
      return false;
    }
  }

  File dst = ESPFS.open(dstPath, "w");
  if (!dst) {
    Serial.printf("Failed to open destination file: %s\n", dstPath);
    src.close();
    return false;
  }

  uint8_t buf[512];
  size_t n;
  while ((n = src.read(buf, sizeof(buf))) > 0) {
    if (dst.write(buf, n) != n) {
      Serial.println("Write failed!");
      src.close();
      dst.close();
      return false;
    }
  }

  src.close();
  dst.close();
  return true;
}

bool isInPSRAM(void* ptr) {
  if (!psramFound() || !ptr) return false;
  uintptr_t addr = (uintptr_t)ptr;
#if defined(CONFIG_IDF_TARGET_ESP32)
  return (addr >= 0x3F800000 && addr < 0x40000000);
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  return (addr >= 0x3F500000 && addr < 0x3FF80000);
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  return (addr >= 0x3C000000 && addr < 0x3E000000);
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  return false;  // ESP32-C3 does not support external PSRAM
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  return false;  // ESP32-C6 does not support external PSRAM
#elif defined(CONFIG_IDF_TARGET_ESP32H2)
  return false;  // ESP32-H2 does not support external PSRAM
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
  // ESP32-P4 PSRAM mapping (when available)
  return (addr >= 0x80000000 && addr < 0x88000000);
#elif defined(CONFIG_IDF_TARGET_ESP32C4)
  return false;  // ESP32-C4 does not support external PSRAM
#endif
  EXT_LOGE(MB_TAG, "isInPSRAM not implemented for this target");
  return false;
}

uint16_t crc16(const unsigned char* data_p, size_t length) {
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

uint16_t gcd(uint16_t a, uint16_t b) {
  while (b != 0) {
    uint16_t t = b;
    b = a % b;
    a = t;
  }
  return a;
}

uint16_t lcm(uint16_t a, uint16_t b) { return a / gcd(a, b) * b; }

bool getBitValue(const uint8_t* byteArray, size_t n) {
  size_t byteIndex = n / 8;
  size_t bitIndex = n % 8;
  uint8_t byte = byteArray[byteIndex];
  return (byte >> bitIndex) & 1;
}
void setBitValue(uint8_t* byteArray, size_t n, bool value) {
  size_t byteIndex = n / 8;
  size_t bitIndex = n % 8;
  if (value)
    byteArray[byteIndex] |= (1 << bitIndex);
  else
    byteArray[byteIndex] &= ~(1 << bitIndex);
}

#if USE_M5UNIFIED
  #include "moonmanpng.h"
#endif

int totalAllocatedMB = 0;
