/**
 *   ESP32 SvelteKit
 *
 *   A simple, secure and extensible framework for IoT projects for ESP32 platforms
 *   with responsive Sveltekit front-end built with TailwindCSS and DaisyUI.
 *   https://github.com/theelims/ESP32-sveltekit
 *
 *   Copyright (C) 2018 - 2023 rjwats
 *   Copyright (C) 2023 - 2025 theelims
 *
 *   All Rights Reserved. This software may be modified and distributed under
 *   the terms of the LGPL v3 license. See the LICENSE file for details.
 **/

#include <Arduino.h>

#if defined(BOARD_HAS_PSRAM) && defined(CONFIG_SPIRAM_MODE_OCT)

// #include <cstddef> // suggested by copilot to surpress operator warning : first parameter of allocation function must be of type 'size_t' - but made no difference

// Threshold (bytes) above which allocations go into PSRAM
constexpr size_t PSRAM_THRESHOLD = 0;  // 87K free, works fine until now
// constexpr size_t PSRAM_THRESHOLD = 512;  //recommended ... ? 32K free, (Small stuff (pointers, FreeRTOS objects, WiFi stack internals) → must stay in internal RAM....)?
// constexpr size_t PSRAM_THRESHOLD = 64;  //65K free, fallback if 0 gives problems?

// Override global new/delete
void* operator new(size_t size) {
  void* ptr = nullptr;
  if (size > PSRAM_THRESHOLD) {
    // Serial.printf("new %d\n", size);
    // Try PSRAM first
    ptr = heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
    if (ptr) return ptr;  // success
  }
  //
  Serial.printf("'new' Fallback to internal RAM %d\n", size);
  ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);

  if (!ptr) {
    Serial.printf("new: CRITICAL - failed to allocate %d bytes\n", size);
    // throw std::bad_alloc(); // Standard requires throwing std::bad_alloc on failure
  }

  return ptr;
}

void operator delete(void* ptr) noexcept {
  // Serial.printf("'delete' using allocMB\n");
  heap_caps_free(ptr);
}

// sized delete overloads (C++14/17 compatibility)
void operator delete(void* ptr, size_t size) noexcept {
  // Serial.printf("'delete' sized %d using allocMB\n", size);
  heap_caps_free(ptr);
}

void* operator new[](size_t size) {
  // Serial.printf("'new[]' using allocMB %d\n", size);
  return operator new(size);  // Reuse logic
}

void operator delete[](void* ptr) noexcept {
  // Serial.printf("'delete[]' using allocMB\n");
  operator delete(ptr);  // Reuse logic
}

// sized delete overloads (C++14/17 compatibility)
void operator delete[](void* ptr, size_t size) noexcept {
  // Serial.printf("'delete[]' sized %d using allocMB\n", size);
  operator delete(ptr);
}
#endif

#include <ESP32SvelteKit.h>
#include <PsychicHttpServer.h>

#define SERIAL_BAUD_RATE 115200

PsychicHttpServer server;

ESP32SvelteKit esp32sveltekit(&server, NROF_END_POINTS);  // 🌙 pio variable

// 🌙
#if FT_ENABLED(FT_MOONBASE)
  #include "MoonBase/Modules/FileManager.h"
  #include "MoonBase/Modules/ModuleIO.h"
  #include "MoonBase/Modules/ModuleTasks.h"

FileManager fileManager = FileManager(&server, &esp32sveltekit);
ModuleTasks moduleTasks = ModuleTasks(&server, &esp32sveltekit);
ModuleIO moduleIO = ModuleIO(&server, &esp32sveltekit);

  // 💫
  #if FT_ENABLED(FT_MOONLIGHT)
    #include "MoonBase/Modules/ModuleDevices.h"  // In MoonLight for the time being, should move to MoonBase using moduleControlCenter ...
    #include "MoonLight/Modules/ModuleChannels.h"
    #include "MoonLight/Modules/ModuleDrivers.h"
    #include "MoonLight/Modules/ModuleEffects.h"
    #include "MoonLight/Modules/ModuleLightsControl.h"
    #include "MoonLight/Modules/ModuleMoonLightInfo.h"
ModuleLightsControl moduleLightsControl = ModuleLightsControl(&server, &esp32sveltekit, &fileManager, &moduleIO);
ModuleDevices moduleDevices = ModuleDevices(&server, &esp32sveltekit, &moduleLightsControl);                           // In MoonLight for the time being, should move to MoonBase using moduleControlCenter ...
ModuleEffects moduleEffects = ModuleEffects(&server, &esp32sveltekit, &fileManager);                                   // fileManager for Live Scripts
ModuleDrivers moduleDrivers = ModuleDrivers(&server, &esp32sveltekit, &fileManager, &moduleLightsControl, &moduleIO);  // fileManager for Live Scripts, Lights control for drivers
    #if FT_ENABLED(FT_LIVESCRIPT)
      #include "MoonLight/Modules/ModuleLiveScripts.h"
ModuleLiveScripts moduleLiveScripts = ModuleLiveScripts(&server, &esp32sveltekit, &fileManager, &moduleEffects, &moduleDrivers);
    #endif
ModuleChannels moduleChannels = ModuleChannels(&server, &esp32sveltekit);
ModuleMoonLightInfo moduleMoonLightInfo = ModuleMoonLightInfo(&server, &esp32sveltekit);

SemaphoreHandle_t swapMutex = xSemaphoreCreateMutex();
volatile bool newFrameReady = false;

TaskHandle_t effectTaskHandle = NULL;
TaskHandle_t driverTaskHandle = NULL;

    #include "esp_task_wdt.h"

void effectTask(void* pvParameters) {
  // 🌙
  esp_task_wdt_add(NULL);

  layerP.setup();  // setup virtual layers (no node setup here as done in addNode)
  static unsigned long last20ms = 0;

  while (true) {
    // Check state under lock
    esp_task_wdt_reset();
    xSemaphoreTake(swapMutex, portMAX_DELAY);

    if (layerP.lights.header.isPositions == 0 && !newFrameReady) {  // within mutex as driver task can change this
      if (layerP.lights.useDoubleBuffer) {
        memcpy(layerP.lights.channelsE, layerP.lights.channelsD, layerP.lights.header.nrOfChannels);  // Copy previous frame (channelsD) to working buffer (channelsE)
        xSemaphoreGive(swapMutex);
      }

      layerP.loop();

      if (millis() - last20ms >= 20) {
        last20ms = millis();
        layerP.loop20ms();
      }

      if (layerP.lights.useDoubleBuffer) {  // Atomic swap channels
        xSemaphoreTake(swapMutex, portMAX_DELAY);
        if (layerP.lights.header.isPositions == 0) {  // Check if not changed while we were unlocked
          uint8_t* temp = layerP.lights.channelsD;
          layerP.lights.channelsD = layerP.lights.channelsE;
          layerP.lights.channelsE = temp;
          newFrameReady = true;
        }
      } else
        newFrameReady = true;
    }

    xSemaphoreGive(swapMutex);
    vTaskDelay(1);
  }
  // Cleanup (never reached in this case, but good practice)
  esp_task_wdt_delete(NULL);
}

void driverTask(void* pvParameters) {
  // 🌙
  esp_task_wdt_add(NULL);

  // layerP.setup() done in effectTask

  while (true) {
    bool mutexGiven = false;
    esp_task_wdt_reset();
    // Check and transition state under lock
    xSemaphoreTake(swapMutex, portMAX_DELAY);
    if (layerP.lights.header.isPositions == 3) {
      EXT_LOGD(ML_TAG, "positions done (3 -> 0)");
      layerP.lights.header.isPositions = 0;
    }

    if (layerP.lights.header.isPositions == 0) {
      if (newFrameReady) {
        newFrameReady = false;
        if (layerP.lights.useDoubleBuffer) {
          xSemaphoreGive(swapMutex);  // Double buffer: release lock, then send
          mutexGiven = true;
        }

        esp32sveltekit.lps++;
        layerP.loopDrivers();
      }
    }

    if (!mutexGiven) xSemaphoreGive(swapMutex);  // not double buffer or if conditions not met
    vTaskDelay(1);
  }
  // Cleanup (never reached in this case, but good practice)
  esp_task_wdt_delete(NULL);
}
  #endif  // MoonLight
#endif    // MoonBase

// 🌙 Custom log output function - 🚧
#ifdef USE_ESP_IDF_LOG
static int custom_vprintf(const char* fmt, va_list args) {
  // Example 1: Write to a custom UART or buffer
  char buffer[256];
  int len = vsnprintf(buffer, sizeof(buffer), fmt, args);

  // Serial.printf("🌙"); //to test it works - 🚧 to send logging to UI

  // Send to custom output (e.g., external UART, network, file, etc.)
  // custom_uart_write(buffer, len);

  // For this example, we'll also write to stdout
  return vprintf(fmt, args);
}
#endif

#if USE_M5UNIFIED
  #include <M5Unified.h>
#endif

std::vector<Module*> modules;
#include "MoonBase/SharedEventEndpoint.h"
#include "MoonBase/SharedHttpEndpoint.h"
#include "MoonBase/SharedWebSocketServer.h"
// #include "MoonBase/SharedFSPersistence.h"

// ADDED: Shared routers (one instance each)
SharedHttpEndpoint* sharedHttpEndpoint = nullptr;
SharedWebSocketServer* sharedWebSocketServer = nullptr;
SharedEventEndpoint* sharedEventEndpoint = nullptr;
// SharedFSPersistence* sharedFsPersistence = nullptr;

void setup() {
#ifdef USE_ESP_IDF_LOG  // 🌙
  esp_log_set_vprintf(custom_vprintf);
  esp_log_level_set("*", LOG_LOCAL_LEVEL);  // use the platformio setting here
#endif

  // start serial and filesystem
  Serial.begin(SERIAL_BAUD_RATE);

  // delay(5000);  // 🌙 to capture all the serial output

  Serial.printf("C++ Standard: %ld\n", __cplusplus);  // 202002L  // 🌙 safeMode

  if (esp_reset_reason() != ESP_RST_UNKNOWN && esp_reset_reason() != ESP_RST_POWERON && esp_reset_reason() != ESP_RST_SW && esp_reset_reason() != ESP_RST_USB) {  // see verbosePrintResetReason
    // ESP_RST_USB is after usb flashing! since esp-idf5
    safeModeMB = true;
  }

  //   // check sizes ...
  //   sizeof(esp32sveltekit);                // 4152
  //   sizeof(WiFiSettingsService);           // 456
  //   sizeof(SystemStatus);                  // 16
  //   sizeof(UploadFirmwareService);         // 32
  //   sizeof(HttpEndpoint<ModuleState>);     // 152
  //   sizeof(EventEndpoint<ModuleState>);    // 112
  //   sizeof(SharedEventEndpoint);           // 8
  //   sizeof(WebSocketServer<ModuleState>);  // 488
  //   sizeof(SharedWebSocketServer);         // 352
  //   sizeof(FSPersistence<ModuleState>);    // 128
  //   sizeof(PsychicHttpServer*);            // 8
  //   sizeof(HttpEndpoint<APSettings>);      // 152
  //   sizeof(SharedHttpEndpoint);            // 16
  //   sizeof(FSPersistence<APSettings>);     // 128
  //   sizeof(APSettingsService);             // 600;
  //   sizeof(PsychicWebSocketHandler);       // 336
  //   sizeof(fileManager);                   // 864
  //   sizeof(Module);                        // 1144 -> 472
  //   sizeof(moduleDevices);                 // 1320
  //   sizeof(moduleIO);                      // 1144
  // #if FT_ENABLED(FT_MOONLIGHT)
  //   sizeof(moduleEffects);        // 1208
  //   sizeof(moduleDrivers);        // 1216
  //   sizeof(moduleLightsControl);  // 1176
  //   #if FT_ENABLED(FT_LIVESCRIPT)
  //   sizeof(moduleLiveScripts);  // 1176
  //   #endif
  //   sizeof(moduleChannels);        // 1144
  //   sizeof(moduleMoonLightInfo);   // 1144
  //   sizeof(layerP.lights);         // 56
  //   sizeof(layerP.lights.header);  // 40
  // #endif

  // start ESP32-SvelteKit
  esp32sveltekit.begin();

  // Create shared routers (one-time)
  sharedHttpEndpoint = new SharedHttpEndpoint(&server, esp32sveltekit.getSecurityManager());
  sharedWebSocketServer = new SharedWebSocketServer(&server, esp32sveltekit.getSecurityManager());
  sharedEventEndpoint = new SharedEventEndpoint(esp32sveltekit.getSocket());
  // sharedFsPersistence = new SharedFSPersistence(esp32sveltekit.getFS());
  if (!sharedHttpEndpoint || !sharedWebSocketServer || !sharedEventEndpoint) {
    EXT_LOGE(ML_TAG, "dev: Failed to allocate shared routers");
    return;
  }

  modules.reserve(12);  // Adjust based on actual module count
  modules.push_back(&moduleTasks);
  modules.push_back(&moduleIO);
// modules.push_back(&fileManager);

// MoonLight
#if FT_ENABLED(FT_MOONLIGHT)
  modules.push_back(&moduleEffects);
  modules.push_back(&moduleDevices);  // In MoonLight for the time being, should move to MoonBase using moduleControlCenter ...
  modules.push_back(&moduleDrivers);
  modules.push_back(&moduleLightsControl);
  modules.push_back(&moduleChannels);
  modules.push_back(&moduleMoonLightInfo);
  #if FT_ENABLED(FT_LIVESCRIPT)
  modules.push_back(&moduleLiveScripts);
  #endif
#endif

  // Register all modules with shared routers
  for (Module* module : modules) {
    sharedHttpEndpoint->registerModule(module);
    sharedWebSocketServer->registerModule(module);
    sharedEventEndpoint->registerModule(module);
    // sharedFsPersistence->registerModule(module);
  }

  // Begin shared routers (one-time setup)
  sharedHttpEndpoint->begin();
  sharedWebSocketServer->begin();
  sharedEventEndpoint->begin();
// sharedFsPersistence->begin();

// MoonBase
#if FT_ENABLED(FT_MOONBASE)
  fileManager.begin();
  for (Module* module : modules) module->begin();

  // 🌙
  #if FT_ENABLED(FT_MOONLIGHT)
  xTaskCreatePinnedToCore(effectTask,          // task function
                          "AppEffects",        // name
                          EFFECTS_STACK_SIZE,  // stack size
                          NULL,                // parameter
                          3,                   // priority
                          &effectTaskHandle,   // task handle
                          0                    // protocol core. high speed effect processing
  );

  xTaskCreatePinnedToCore(driverTask,          // task function
                          "AppDrivers",        // name
                          DRIVERS_STACK_SIZE,  // stack size
                          NULL,                // parameter
                          3,                   // priority
                          &driverTaskHandle,   // task handle
    #ifdef CONFIG_FREERTOS_UNICORE
                          0  // Single-core: use Core 0 (only option)
    #else
                          1  // Multi-core: application core
    #endif
  );
  #endif  // MoonLight

  // run UI stuff in the sveltekit task
  esp32sveltekit.addLoopFunction([]() {
    for (Module* module : modules) module->loop();

    static unsigned long last20ms = 0;
    if (millis() - last20ms >= 20) {
      last20ms = millis();
  #if FT_ENABLED(FT_MOONLIGHT)
      moduleDevices.loop20ms();  // In MoonLight for the time being, should move to MoonBase using moduleControlCenter ...
  #endif

      // every second
      static unsigned long lastSecond = 0;
      if (millis() - lastSecond >= 1000) {
        lastSecond = millis();

        moduleIO.loop1s();
        moduleTasks.loop1s();

        // logYield();

  #if FT_ENABLED(FT_MOONLIGHT)
        // set shared data (eg used in scrolling text effect)
        sharedData.fps = esp32sveltekit.getAnalyticsService()->lps;
        sharedData.connectionStatus = (uint8_t)esp32sveltekit.getConnectionStatus();
        sharedData.clientListSize = esp32sveltekit.getServer()->getClientList().size();
        sharedData.connectedClients = esp32sveltekit.getSocket()->getConnectedClients();
        sharedData.activeClients = esp32sveltekit.getSocket()->getActiveClients();

    #if FT_ENABLED(FT_LIVESCRIPT)
        moduleLiveScripts.loop1s();
    #endif
  #endif

        // every 10 seconds
        static unsigned long last10Second = 0;
        if (millis() - last10Second >= 10000) {
          last10Second = millis();

  #if FT_ENABLED(FT_MOONLIGHT)
          moduleDevices.loop10s();  // In MoonLight for the time being, should move to MoonBase using moduleControlCenter ...
  #endif
        }
      }
    }
  });

#endif  // FT_MOONBASE

#if USE_M5UNIFIED
  auto cfg = M5.config();
  M5.begin(cfg);
  // M5.Display.fillScreen(BLACK);
  #if USE_M5UNIFIEDDisplay
  M5.Display.drawPng(moonmanpng, moonmanpng_len, 0, 0, 0, 0, 0, 0, 100 / 320, 100 / 320);

    // M5.Display.fillRect(50, 50, 100, 100, RED);
    // M5.Display.setTextColor(WHITE);
    // M5.Display.setTextSize(2);
    // M5.Display.drawString("MoonLight", 0, 0);
  #endif
#endif
}

void loop() {
#if USE_M5UNIFIEDDDisplay
  M5.update();
  delay(100);
#else
  // Delete Arduino loop task, as it is not needed
  vTaskDelete(NULL);
#endif
}