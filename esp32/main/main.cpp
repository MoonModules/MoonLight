/// The ESP32 entry point: bring up NVS, then hand control to the shared application main.

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include <csignal>   // sig_atomic_t: mm_main's stop flag
#include <cstdio>

extern void mm_main(volatile std::sig_atomic_t& keepRunning, uint16_t httpPort);

static volatile std::sig_atomic_t running = 1;

extern "C" void app_main() {
    // Initialize NVS (required by WiFi and other drivers)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Print heap before network init
    std::printf("Heap free: %lu bytes\n",
                static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));

    // Network init moved to platform layer (NetworkModule calls ethInit/wifiStaInit/wifiApInit)

    mm_main(running, 80);
}
