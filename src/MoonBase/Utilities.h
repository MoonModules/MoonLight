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
#include "Coord3D.h"
#include "PureFunctions.h"

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

// file functions

void walkThroughFiles(File folder, std::function<void(File, File)> fun);

bool copyFile(const char* srcPath, const char* dstPath);

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
