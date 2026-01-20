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

#include <SystemStatus.h>

#include <esp32-hal.h>

#if CONFIG_IDF_TARGET_ESP32 // ESP32/PICO-D4
#include "esp32/rom/rtc.h"
#define ESP_TARGET "ESP32";
#elif CONFIG_IDF_TARGET_ESP32S2
#include "esp32/rom/rtc.h"
#define ESP_TARGET "ESP32-S2";
#elif CONFIG_IDF_TARGET_ESP32C3
#include "esp32c3/rom/rtc.h"
#define ESP_TARGET "ESP32-C3";
#elif CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/rtc.h"
#define ESP_TARGET "ESP32-S3";
#elif CONFIG_IDF_TARGET_ESP32C6
#include "esp32c6/rom/rtc.h"
#define ESP_TARGET "ESP32-C6";
#elif CONFIG_IDF_TARGET_ESP32P4 // 🌙
    #include "esp32p4/rom/rtc.h"
    #define ESP_TARGET "ESP32-P4";
    #include "esp_hosted_api_types.h"
    #include <esp_hosted.h>
    // #include "esp_hosted_host_fw_ver.h"
    // #include "esp_hosted_misc.h"
    // #include "esp_hosted_ota.h"
#else
#error Target CONFIG_IDF_TARGET is not supported
#endif

#ifndef ARDUINO_VERSION
#ifndef STRINGIZE
#define STRINGIZE(s) #s
#endif
#define ARDUINO_VERSION_STR(major, minor, patch) "v" STRINGIZE(major) "." STRINGIZE(minor) "." STRINGIZE(patch)
#define ARDUINO_VERSION ARDUINO_VERSION_STR(ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH)
#endif

String verbosePrintResetReason(int reason)
{
    switch (reason)
    {
    case ESP_RST_UNKNOWN:
        return ("Reset reason can not be determined");
        break;
    case ESP_RST_POWERON:
        return ("Reset due to power-on event");
        break;
    case ESP_RST_EXT:
        return ("Reset by external pin (not applicable for ESP32)");
        break;
    case ESP_RST_SW:
        return ("Software reset via esp_restart");
        break;
    case ESP_RST_PANIC:
        return ("Software reset due to exception/panic");
        break;
    case ESP_RST_INT_WDT:
        return ("Reset (software or hardware) due to interrupt watchdog");
        break;
    case ESP_RST_TASK_WDT:
        return ("Reset due to task watchdog");
        break;
    case ESP_RST_WDT:
        return ("Reset due to other watchdogs");
        break;
    case ESP_RST_DEEPSLEEP:
        return ("Reset after exiting deep sleep mode");
        break;
    case ESP_RST_BROWNOUT:
        return ("Brownout reset (software or hardware)");
        break;
    case ESP_RST_SDIO:
        return ("Reset over SDIO");
        break;
// 🌙 looks like this exists in all the boards we use, and ifdef ESP_RST_USB is not returning true for some reason ...
// #ifdef ESP_RST_USB
    case ESP_RST_USB:
        return ("Reset by USB peripheral");
        break;
// #endif
// #ifdef ESP_RST_JSVK_TAG
    case ESP_RST_JTAG:
        return ("Reset by JSVK_TAG");
        break;
// #endif
// #ifdef ESP_RST_EFUSE
    case ESP_RST_EFUSE:
        return ("Reset due to efuse error");
        break;
// #endif
// #ifdef ESP_RST_PWR_GLITCH
    case ESP_RST_PWR_GLITCH:
        return ("Reset due to power glitch detected");
        break;
// #endif
// #ifdef ESP_RST_CPU_LOCKUP
    case ESP_RST_CPU_LOCKUP:
        return ("Reset due to CPU lock up (double exception)");
        break;
// #endif
    default:
        char buffer[50];
        snprintf(buffer, sizeof(buffer), "Unknown reset reason (%d)", reason);
        return String(buffer);
        break;
    }
}

SystemStatus::SystemStatus(PsychicHttpServer *server,
                           SecurityManager *securityManager) : _server(server),
                                                               _securityManager(securityManager)
{
}

void SystemStatus::begin()
{
    _server->on(SYSTEM_STATUS_SERVICE_PATH,
                HTTP_GET,
                _securityManager->wrapRequest(std::bind(&SystemStatus::systemStatus, this, std::placeholders::_1),
                                              AuthenticationPredicates::IS_AUTHENTICATED));

    ESP_LOGV(SVK_TAG, "Registered GET endpoint: %s", SYSTEM_STATUS_SERVICE_PATH);
}

esp_err_t SystemStatus::systemStatus(PsychicRequest *request)
{
    PsychicJsonResponse response = PsychicJsonResponse(request, false);
    JsonObject root = response.getRoot();

    root["esp_platform"] = ESP_TARGET;
    root["firmware_version"] = APP_VERSION;
    root["firmware_date"] = APP_DATE; // 🌙
    root["firmware_target"] = BUILD_TARGET; // 🌙
    root["platform_version"] = PLATFORM_VERSION; // 🌙
    root["max_alloc_heap"] = ESP.getMaxAllocHeap();
    if (psramFound())
    {
        root["free_psram"] = ESP.getFreePsram();
        root["used_psram"] = ESP.getPsramSize() - ESP.getFreePsram();
        root["psram_size"] = ESP.getPsramSize();
        // #if CONFIG_ESP32S3_SPIRAM_SUPPORT  // WLEDMM -S3 has "qspi" or "opi" PSRAM mode
            #if CONFIG_SPIRAM_MODE_OCT
            root["psram_mode"]  = "🚀 OPI";
            #elif CONFIG_SPIRAM_MODE_QUAD
            root["psram_mode"]  = "QSPI";
            #else
            root["psram_mode"]  = "other";
            #endif
        // #endif
    }
    root["cpu_freq_mhz"] = ESP.getCpuFreqMHz();
    root["cpu_type"] = ESP.getChipModel();
    root["cpu_rev"] = ESP.getChipRevision();
    root["cpu_cores"] = ESP.getChipCores();
    root["free_heap"] = ESP.getFreeHeap();
    root["used_heap"] = ESP.getHeapSize() - ESP.getFreeHeap();
    root["total_heap"] = ESP.getHeapSize();
    root["min_free_heap"] = ESP.getMinFreeHeap();
    root["sketch_size"] = ESP.getSketchSize();
    root["free_sketch_space"] = ESP.getFreeSketchSpace();
    root["sdk_version"] = ESP.getSdkVersion();
    root["arduino_version"] = ARDUINO_VERSION;
    root["flash_chip_size"] = ESP.getFlashChipSize();
    root["flash_chip_speed"] = ESP.getFlashChipSpeed();
    switch (ESP.getFlashChipMode()) {  // 🌙
        // missing: Octal modes
        case FM_QIO:  root["flash_chip_mode"] = "QIO"; break;
        case FM_QOUT: root["flash_chip_mode"] = "QOUT";break;
        case FM_DIO:  root["flash_chip_mode"] = "DIO"; break;
        case FM_DOUT: root["flash_chip_mode"] = "DOUT or other";break;
        #if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_ESPTOOLPY_FLASHMODE_OPI
        case FM_FAST_READ: root["flash_chip_mode"] = "🚀OPI";break;
        #else
        case FM_FAST_READ: root["flash_chip_mode"] = "fast_read";break;
        #endif
        case FM_SLOW_READ: root["flash_chip_mode"] = "slow_read";break;
        default: root["flash_chip_mode"] = "other"; break;
    }
    root["fs_total"] = ESPFS.totalBytes();
    root["fs_used"] = ESPFS.usedBytes();
    root["core_temp"] = temperatureRead();
    root["cpu_reset_reason"] = verbosePrintResetReason(esp_reset_reason());
    root["uptime"] = millis() / 1000;
    #ifdef CONFIG_IDF_TARGET_ESP32P4
        esp_hosted_coprocessor_fwver_t c6_fw_version;
        esp_hosted_get_coprocessor_fwversion(&c6_fw_version);
        char coprocessor[10];
        snprintf(coprocessor, sizeof(coprocessor), "v%d.%d.%d", c6_fw_version.major1, c6_fw_version.minor1, c6_fw_version.patch1);
        root["coprocessor"] = coprocessor;
    #endif

    heapHealth(root["heap_info_app"].to<JsonVariant>(), MALLOC_CAP_INTERNAL);
    heapHealth(root["heap_info_dma"].to<JsonVariant>(), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    return response.send();
}

 // 🌙
void SystemStatus::heapHealth(JsonVariant variant, uint32_t caps) {
    multi_heap_info_t mhi;
    heap_caps_get_info(&mhi, caps);

    // Normalize everything to 0–1
    float f1 = mhi.largest_free_block / 8192.0;   // ideal largest block: ≥ 8 KB
    float f2 = mhi.total_free_bytes / 16384.0;  // ideal free heap: ≥ 16 KB
    float f3 = mhi.minimum_free_bytes / 8192.0;   // ideal minimum free: ≥ 8 KB

    // Clamp
    if (f1 > 1) f1 = 1;
    if (f2 > 1) f2 = 1;
    if (f3 > 1) f3 = 1;

    uint8_t health1 = 100 * (f1 + f2 + f3) / 3;

    // largest_free_block → fragmentation
    // total_free_bytes → headroom
    // minimum_free_bytes → how close you came to death

    // Condition	Meaning
    // lfb < 5 KB	Heavy fragmentation → unstable
    // tfb < 10–15 KB	malloc/WiFi/TLS failures possible
    // mfb < 8 KB	You were close to crashing before

    // So the formula gives:
    // GOOD (🟢) → plenty of room & low fragmentation
    // WARNING (🟠) → danger if load spikes
    // BAD (🔴) → fragmentation or low RAM already dangerous

    uint8_t health2 = 100 * mhi.largest_free_block / mhi.total_free_bytes;

    // Interpretation:
    // ≥ 66% → 🟢 good
    // < 33% → 🔴 bad
    // else → 🟠 okay

    char s[128];
    snprintf(s, sizeof(s),
        "%s %zu+%zu=%zu🧱| %d+%d=%d KB |🔹%dKB→%d%%%s (🔻%dKB)",
        health1>66?"🟢":(health1<33?"🔴":"🟠"),
        mhi.allocated_blocks,
        mhi.free_blocks,
        mhi.total_blocks,
        mhi.total_allocated_bytes / 1024,
        mhi.total_free_bytes / 1024,
        (mhi.total_allocated_bytes +mhi.total_free_bytes) / 1024,
        mhi.largest_free_block / 1024,
        health2,
        health2>66?"🟢":(health2<33?"🔴":"🟠"),
        mhi.minimum_free_bytes / 1024
    );
    variant.set(s);
}