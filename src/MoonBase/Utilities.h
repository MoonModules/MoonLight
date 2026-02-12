/**
    @title     MoonBase
    @file      Utilities.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#include <ESPFS.h>

#include "ArduinoJson.h"
#include "Char.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)  // e.g. for pio.ini settings (see ML_CHIPSET)

// add task and stacksize to logging
// if USE_ESP_IDF_LOG: USE_ESP_IDF_LOG is not showing __FILE__), __LINE__, __FUNCTION__ so add that
#ifdef USE_ESP_IDF_LOG
  #if CORE_DEBUG_LEVEL >= 1
    #define EXT_LOGE(tag, fmt, ...) ESP_LOGE(tag, "%s (%d) [%s:%d] %s: " fmt, pcTaskGetName(xTaskGetCurrentTaskHandle()), uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()), pathToFileName(__FILE__), __LINE__, __FUNCTION__, ##__VA_ARGS__)
  #else
    #define EXT_LOGE(tag, fmt, ...)
  #endif
  #if CORE_DEBUG_LEVEL >= 2
    #define EXT_LOGW(tag, fmt, ...) ESP_LOGW(tag, "%s (%d) [%s:%d] %s: " fmt, pcTaskGetName(xTaskGetCurrentTaskHandle()), uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()), pathToFileName(__FILE__), __LINE__, __FUNCTION__, ##__VA_ARGS__)
  #else
    #define EXT_LOGW(tag, fmt, ...)
  #endif
  #if CORE_DEBUG_LEVEL >= 3
    #define EXT_LOGI(tag, fmt, ...) ESP_LOGI(tag, "%s (%d) [%s:%d] %s: " fmt, pcTaskGetName(xTaskGetCurrentTaskHandle()), uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()), pathToFileName(__FILE__), __LINE__, __FUNCTION__, ##__VA_ARGS__)
  #else
    #define EXT_LOGI(tag, fmt, ...)
  #endif
  #if CORE_DEBUG_LEVEL >= 4
    #define EXT_LOGD(tag, fmt, ...) ESP_LOGD(tag, "%s (%d) [%s:%d] %s: " fmt, pcTaskGetName(xTaskGetCurrentTaskHandle()), uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()), pathToFileName(__FILE__), __LINE__, __FUNCTION__, ##__VA_ARGS__)
  #else
    #define EXT_LOGD(tag, fmt, ...)
  #endif
  #if CORE_DEBUG_LEVEL >= 5
    #define EXT_LOGV(tag, fmt, ...) ESP_LOGV(tag, "%s (%d) [%s:%d] %s: " fmt, pcTaskGetName(xTaskGetCurrentTaskHandle()), uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()), pathToFileName(__FILE__), __LINE__, __FUNCTION__, ##__VA_ARGS__)
  #else
    #define EXT_LOGV(tag, fmt, ...)
  #endif
#else
  #if CORE_DEBUG_LEVEL >= 1
    #define EXT_LOGE(tag, fmt, ...) ESP_LOGE(tag, "%s (%d) " fmt, pcTaskGetName(xTaskGetCurrentTaskHandle()), uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()), ##__VA_ARGS__)
  #else
    #define EXT_LOGE(tag, fmt, ...)
  #endif
  #if CORE_DEBUG_LEVEL >= 2
    #define EXT_LOGW(tag, fmt, ...) ESP_LOGW(tag, "%s (%d) " fmt, pcTaskGetName(xTaskGetCurrentTaskHandle()), uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()), ##__VA_ARGS__)
  #else
    #define EXT_LOGW(tag, fmt, ...)
  #endif
  #if CORE_DEBUG_LEVEL >= 3
    #define EXT_LOGI(tag, fmt, ...) ESP_LOGI(tag, "%s (%d) " fmt, pcTaskGetName(xTaskGetCurrentTaskHandle()), uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()), ##__VA_ARGS__)
  #else
    #define EXT_LOGI(tag, fmt, ...)
  #endif
  #if CORE_DEBUG_LEVEL >= 4
    #define EXT_LOGD(tag, fmt, ...) ESP_LOGD(tag, "%s (%d) " fmt, pcTaskGetName(xTaskGetCurrentTaskHandle()), uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()), ##__VA_ARGS__)
  #else
    #define EXT_LOGD(tag, fmt, ...)
  #endif
  #if CORE_DEBUG_LEVEL >= 5
    #define EXT_LOGV(tag, fmt, ...) ESP_LOGV(tag, "%s (%d) " fmt, pcTaskGetName(xTaskGetCurrentTaskHandle()), uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()), ##__VA_ARGS__)
  #else
    #define EXT_LOGV(tag, fmt, ...)
  #endif
#endif

#define MB_TAG "🌙"
#define ML_TAG "💫"

#ifndef MIN
  #define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
  #define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif


struct Coord3D {
  int x;
  int y;
  int z;

  // Coord3D() : x(0), y(0), z(0) {} // Default constructor

  Coord3D() {
    this->x = 0;
    this->y = 0;
    this->z = 0;
  }

  Coord3D(int x, int y = 0, int z = 0) {
    this->x = x;
    this->y = y;
    this->z = z;
  }

  // comparisons
  bool operator!=(const Coord3D& rhs) {
    return x != rhs.x || y != rhs.y || z != rhs.z;
    // return !(*this==rhs);
  }
  bool operator==(const Coord3D rhs) const { return x == rhs.x && y == rhs.y && z == rhs.z; }
  bool operator<(const int rhs) const { return x < rhs && y < rhs && z < rhs; }

  // Minus / delta (abs)
  Coord3D operator-(const Coord3D rhs) const { return Coord3D(x - rhs.x, y - rhs.y, z - rhs.z); }
  Coord3D operator-(const int rhs) const { return Coord3D(x - rhs, y - rhs, z - rhs); }
  Coord3D operator+(const Coord3D rhs) const { return Coord3D(x + rhs.x, y + rhs.y, z + rhs.z); }
  Coord3D operator*(const Coord3D rhs) const { return Coord3D(x * rhs.x, y * rhs.y, z * rhs.z); }
  Coord3D operator/(const Coord3D rhs) const { return Coord3D(x / rhs.x, y / rhs.y, z / rhs.z); }
  Coord3D operator/(const int rhs) const { return Coord3D(x / rhs, y / rhs, z / rhs); }
  Coord3D operator%(const Coord3D rhs) const { return Coord3D(x % rhs.x, y % rhs.y, z % rhs.z); }

  // assignments
  Coord3D operator=(const Coord3D rhs) {
    x = rhs.x;
    y = rhs.y;
    z = rhs.z;
    return *this;
  }
  Coord3D operator+=(const Coord3D rhs) {
    x += rhs.x;
    y += rhs.y;
    z += rhs.z;
    return *this;
  }
  Coord3D operator/=(const Coord3D rhs) {
    if (rhs.x) x /= rhs.x;
    if (rhs.y) y /= rhs.y;
    if (rhs.z) z /= rhs.z;
    return *this;
  }

  Coord3D maximum(const Coord3D rhs) const { return Coord3D(MAX(x, rhs.x), MAX(y, rhs.y), MAX(z, rhs.z)); }

  unsigned distanceSquared(const Coord3D rhs) const {
    Coord3D delta = (*this - rhs);
    return (delta.x) * (delta.x) + (delta.y) * (delta.y) + (delta.z) * (delta.z);
  }

  bool isOutofBounds(const Coord3D rhs) const { return x < 0 || y < 0 || z < 0 || x >= rhs.x || y >= rhs.y || z >= rhs.z; }
};

// https://arduinojson.org/news/2021/05/04/version-6-18-0/
namespace ArduinoJson {
template <>
struct Converter<Coord3D> {
  static bool toJson(const Coord3D& src, JsonVariant dst) {
    dst["x"] = src.x;
    dst["y"] = src.y;
    dst["z"] = src.z;
    return true;
  }

  static Coord3D fromJson(JsonVariantConst src) { return Coord3D{src["x"], src["y"], src["z"]}; }

  static bool checkJson(JsonVariantConst src) { return src["x"].is<uint16_t>() && src["y"].is<uint8_t>() && src["z"].is<uint8_t>(); }
};
}  // namespace ArduinoJson

// ArduinoJson functions
bool arrayContainsValue(JsonArray array, int value);
int getNextItemInArray(JsonArray array, size_t currentValue, bool backwards = false);

float distance(float x1, float y1, float z1, float x2, float y2, float z2);

// file functions

void extractPath(const char* filepath, char* path);

void walkThroughFiles(File folder, std::function<void(File, File)> fun);

bool copyFile(const char* srcPath, const char* dstPath);

// for game of live
uint16_t crc16(const unsigned char* data_p, size_t length);
uint16_t gcd(uint16_t a, uint16_t b);
uint16_t lcm(uint16_t a, uint16_t b);
bool getBitValue(const uint8_t* byteArray, size_t n);
void setBitValue(uint8_t* byteArray, size_t n, bool value);

bool isInPSRAM(void* ptr);

extern int totalAllocatedMB;

// allocate, try PSRAM, else default, use calloc: zero-initialized (all bytes = 0)
template <typename T>
T* allocMB(size_t n, const char* name = nullptr) {
  T* res = (T*)heap_caps_calloc_prefer(n, sizeof(T), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);  // calloc is malloc + memset(0);
  if (res) {
    totalAllocatedMB += heap_caps_get_allocated_size(res);
    // EXT_LOGD(MB_TAG, "Allocated %s: %d x %d bytes in %s s:%d (tot:%d)", name?name:"x", n, sizeof(T), isInPSRAM(res)?"PSRAM":"RAM", heap_caps_get_allocated_size(res), totalAllocatedMB);
  } else
    EXT_LOGE(MB_TAG, "heap_caps_malloc for %s of %d x %d not succeeded", name?name:"x", n, sizeof(T));
  return res;
}

template <typename T>
T* reallocMB(T* p, size_t n, const char* name = nullptr) {
  T* res = (T*)heap_caps_realloc_prefer(p, n * sizeof(T), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);  // calloc is malloc + memset(0);
  if (res) {
    // EXT_LOGD(MB_TAG, "Re-Allocated %s: %d x %d bytes in %s s:%d", name?name:"x", n, sizeof(T), isInPSRAM(res)?"PSRAM":"RAM", heap_caps_get_allocated_size(res));
  } else
    EXT_LOGE(MB_TAG, "heap_caps_malloc for %s of %d x %d not succeeded", name?name:"x", n, sizeof(T));
  return res;
}

template <typename T>
void reallocMB2(T* &p, size_t &pSize, size_t n, const char* name = nullptr) {
  T* res = (T*)heap_caps_realloc_prefer(p, n * sizeof(T), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);  // calloc is malloc + memset(0);
  if (res) {
    // EXT_LOGD(MB_TAG, "Re-Allocated %s: %d x %d bytes in %s s:%d", name?name:"x", n, sizeof(T), isInPSRAM(res)?"PSRAM":"RAM", heap_caps_get_allocated_size(res));
    p = res;
    pSize = n;
  } else {
    EXT_LOGE(MB_TAG, "heap_caps_malloc for %s of %d x %d not succeeded, keeping old %d", name?name:"x", n, sizeof(T), pSize);
  }
}

// free memory
template <typename T>
void freeMB(T*& p, const char* name = nullptr) {
  if (p) {
    totalAllocatedMB -= heap_caps_get_allocated_size(p);
    // EXT_LOGD(MB_TAG, "free %s: x x %d bytes in %s, s:%d (tot:%d)", name?name:"x", sizeof(T), isInPSRAM(p)?"PSRAM":"RAM", heap_caps_get_allocated_size(p), totalAllocatedMB);
    heap_caps_free(p);
    p = nullptr;
  } else
    EXT_LOGW(MB_TAG, "Nothing to free for %s: pointer is null", name?name:"x");
}

// allocate vector
template <typename T>
struct VectorRAMAllocator {
  using value_type = T;

  T* allocate(size_t n) { return allocMB<T>(n, "vector"); }
  void deallocate(T* p, size_t n) { freeMB(p, "vector"); }
  T* reallocate(T* p, size_t n) { return reallocMB<T>(p, n, "vector"); }
};

// https://arduinojson.org/v7/api/jsondocument/
struct JsonRAMAllocator : ArduinoJson::Allocator {
  //(uint8_t*): simulate 1 byte
  void* allocate(size_t n) override { return allocMB<uint8_t>(n, "json"); }
  void deallocate(void* p) override { freeMB(p, "json"); }
  void* reallocate(void* p, size_t n) override { return reallocMB<uint8_t>((uint8_t*)p, n, "json"); }
  static Allocator* instance() {
    static JsonRAMAllocator allocator;
    return &allocator;
  }
};

// allocate object
template <typename T, typename... Args>
T* allocMBObject(Args&&... args) {
  void* mem = allocMB<T>(1, "object");
  if (mem) {
    return new (mem) T(std::forward<Args>(args)...);
  } else {
    return nullptr;
  }
}

// free object
template <typename T>
void freeMBObject(T*& obj) {
  freeMB(obj, "object");
}

// to use in effect and on display
#if USE_M5UNIFIED
extern unsigned char moonmanpng[];
extern unsigned int moonmanpng_len;
#endif

inline uint32_t fastDiv255(uint32_t x) {  // 3–4 cycles
  return (x * 0x8081u) >> 23;
}

// Task yields
inline uint16_t yieldCallCount = 0;
inline uint16_t yieldCounter = 0;

inline void addYield(uint8_t frequency) {
  if (++yieldCallCount % frequency == 0) {
    yieldCounter++;
    vTaskDelay(1);  // taskYIELD() is not good enough as it does not give back control to idle tasks
  }
}

inline void logYield() {
  EXT_LOGD(ML_TAG, "yieldCounter %d (%d)", yieldCallCount, yieldCounter);
  yieldCounter = 0;
}
