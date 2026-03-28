/**
    @title     MoonBase
    @file      MemAlloc.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#include <cstddef>

#include "ArduinoJson.h"

// Fallback no-op macros when included standalone (real definitions come from PlatformFunctions.h)
#ifndef EXT_LOGE
  #define EXT_LOGE(tag, fmt, ...)
#endif
#ifndef EXT_LOGW
  #define EXT_LOGW(tag, fmt, ...)
#endif
#ifndef EXT_LOGD
  #define EXT_LOGD(tag, fmt, ...)
#endif
#ifndef MB_TAG
  #define MB_TAG "🌙"
#endif

extern int totalAllocatedMB;

bool isInPSRAM(void* ptr);

// allocate, try PSRAM, else default, use calloc: zero-initialized (all bytes = 0)
template <typename T>
T* allocMB(size_t n, const char* name = nullptr) {
  T* res = (T*)heap_caps_calloc_prefer(n, sizeof(T), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);  // calloc is malloc + memset(0);
  if (res) {
    totalAllocatedMB += heap_caps_get_allocated_size(res);
    // EXT_LOGD(MB_TAG, "Allocated %s: %d x %d bytes in %s s:%d (tot:%d)", name?name:"x", n, sizeof(T), isInPSRAM(res)?"PSRAM":"RAM", heap_caps_get_allocated_size(res), totalAllocatedMB);
  } else
    EXT_LOGE(MB_TAG, "heap_caps_malloc for %s of %d x %d not succeeded", name ? name : "x", n, sizeof(T));
  return res;
}

template <typename T>
T* reallocMB(T* p, size_t n, const char* name = nullptr) {
  T* res = (T*)heap_caps_realloc_prefer(p, n * sizeof(T), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);  // calloc is malloc + memset(0);
  if (res) {
    // EXT_LOGD(MB_TAG, "Re-Allocated %s: %d x %d bytes in %s s:%d", name?name:"x", n, sizeof(T), isInPSRAM(res)?"PSRAM":"RAM", heap_caps_get_allocated_size(res));
  } else
    EXT_LOGE(MB_TAG, "heap_caps_malloc for %s of %d x %d not succeeded", name ? name : "x", n, sizeof(T));
  return res;
}

template <typename T>
void reallocMB2(T*& p, size_t& pSize, size_t n, const char* name = nullptr) {
  T* res = (T*)heap_caps_realloc_prefer(p, n * sizeof(T), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);  // calloc is malloc + memset(0);
  if (res) {
    // EXT_LOGD(MB_TAG, "Re-Allocated %s: %d x %d bytes in %s s:%d", name?name:"x", n, sizeof(T), isInPSRAM(res)?"PSRAM":"RAM", heap_caps_get_allocated_size(res));
    p = res;
    pSize = n;
  } else {
    EXT_LOGE(MB_TAG, "heap_caps_malloc for %s of %d x %d not succeeded, keeping old %d", name ? name : "x", n, sizeof(T), pSize);
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
    EXT_LOGW(MB_TAG, "Nothing to free for %s: pointer is null", name ? name : "x");
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
  if (obj) {
    obj->~T();
  }
  freeMB(obj, "object");
}
