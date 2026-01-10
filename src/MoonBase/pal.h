/**
    @title     MoonBase
    @file      pal.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

/**
 Platform Abstraction Layer (PAL)

 Goal:
 * not depending on Arduino.h
 * use esp-idf functions (instead of arduino.h)
 * in a way to not create a new dependency on esp-idf

 Approach
 * Incremental changes: you can still do it the old way but the new way is available as well

 Benefits
 * Common way of working, within one repository, Possibly between different repo's
 * No mix of arduino and esp-idf code in different places
 * platform Arduino can be removed in the future
 * Automated testing e.g. on Linux
 **/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pal {

/* =================================================
 * Basic types
 * ================================================= */

struct rgb_t {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

/* =================================================
 * Time
 * ================================================= */

uint32_t millis();
uint64_t micros();
void delay_ms(uint32_t ms);

/* =================================================
 * LED output (hot path)
 * ================================================= */

void led_submit(const rgb_t* buffer, size_t led_count);
void led_set_brightness(uint8_t brightness);

/* =================================================
 * Memory
 * ================================================= */

void* malloc(size_t size);
void free(void* ptr);

/* =================================================
 * Logging
 * ================================================= */

enum class LogLevel : uint8_t { Error, Warn, Info, Debug };

void log(LogLevel level, const char* tag, const char* message);

/* =================================================
 * Capability hints (0 = no, 1 = yes)
 * ================================================= */

int cap_led_dma();
int cap_led_parallel();

/* =================================================
 * UDP socket (stateful)
 * ================================================= */

class UdpSocket {
 public:
  virtual ~UdpSocket() = default;

  virtual bool open(uint16_t local_port) = 0;
  virtual int send_to(const char* ip, uint16_t port, const uint8_t* data, size_t len) = 0;
  virtual int recv_from(uint8_t* buffer, size_t max_len,
                        char* src_ip,       // out
                        uint16_t* src_port  // out
                        ) = 0;
  virtual void close() = 0;
};

UdpSocket* udp_socket_create();
void udp_socket_destroy(UdpSocket* socket);

}  // namespace pal
