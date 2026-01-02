/**
    @title     MoonBase
    @file      pal_espidf.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <string.h>

#include "pal.h"

namespace pal {

/* =================================================
 * Time
 * ================================================= */

uint32_t millis() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

uint64_t micros() { return static_cast<uint64_t>(esp_timer_get_time()); }

void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

/* =================================================
 * LED output (ESP-IDF backend placeholder)
 * ================================================= */

// below functions is poc code. Need to be verified when actually used

static uint8_t s_led_brightness = 255;  // brightness control will be implemented when the actual LED driver is added.

void led_submit(const rgb_t* buffer, size_t led_count) {
  /* To be implemented with:
     - RMT
     - PARLIO
     - DMA / PPA
     - or external driver
  */
  (void)buffer;
  (void)led_count;
}

void led_set_brightness(uint8_t brightness) { s_led_brightness = brightness; }

/* =================================================
 * Memory
 * ================================================= */

void* malloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_8BIT); }

void free(void* ptr) { heap_caps_free(ptr); }

/* =================================================
 * Logging
 * ================================================= */

void log(LogLevel level, const char* tag, const char* message) {
  esp_log_level_t esp_level = ESP_LOG_INFO;

  switch (level) {
  case LogLevel::Error:
    esp_level = ESP_LOG_ERROR;
    break;
  case LogLevel::Warn:
    esp_level = ESP_LOG_WARN;
    break;
  case LogLevel::Info:
    esp_level = ESP_LOG_INFO;
    break;
  case LogLevel::Debug:
    esp_level = ESP_LOG_DEBUG;
    break;
  }

  esp_log_write(esp_level, tag, "%s\n", message);
}

/* =================================================
 * Capabilities
 * ================================================= */

int cap_led_dma() { return 1; /* ESP32 supports DMA */ }

int cap_led_parallel() { return 1; /* ESP32 generally supports parallel output */ }

/* =================================================
 * UDP socket (ESP-IDF / lwIP)
 * ================================================= */

class EspIdfUdpSocket : public UdpSocket {
 public:
  EspIdfUdpSocket() : sock_(-1) {}

  ~EspIdfUdpSocket() override { close(); }

  // below functions is poc code. Need to be verified when actually used

  bool open(uint16_t local_port) override {
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock_ < 0) {
      return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(local_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(sock_);
      sock_ = -1;
      return false;
    }
    return true;
  }

  int send_to(const char* ip, uint16_t port, const uint8_t* data, size_t len) override {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_aton(ip, &dest.sin_addr);

    return sendto(sock_, data, len, 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
  }

  int recv_from(uint8_t* buffer, size_t max_len,
                char* src_ip,       // out
                uint16_t* src_port  // out
  ) {
    sockaddr_in src_addr{};
    socklen_t src_len = sizeof(src_addr);

    int received = recvfrom(sock_, buffer, max_len, 0, reinterpret_cast<sockaddr*>(&src_addr), &src_len);

    if (received >= 0 && src_ip && src_port) {
      inet_ntoa_r(src_addr.sin_addr, src_ip, 16);
      *src_port = ntohs(src_addr.sin_port);
    }

    return received;
  }

  void close() override {
    if (sock_ >= 0) {
      ::close(sock_);
      sock_ = -1;
    }
  }

 private:
  int sock_;
};

/* =================================================
 * Factory
 * ================================================= */

UdpSocket* udp_socket_create() { return new EspIdfUdpSocket(); }

void udp_socket_destroy(UdpSocket* socket) { delete socket; }

}  // namespace pal
