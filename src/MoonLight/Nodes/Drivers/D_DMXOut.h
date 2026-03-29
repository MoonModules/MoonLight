/**
    @title     MoonLight
    @file      D_DMXOut.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

#include "driver/uart.h"
#include "soc/soc_caps.h"

// DMX512 output driver — sends channel data from the physical layer over RS-485.
// Uses UART_NUM_2 on chips that have it (ESP32, S3, P4); falls back to UART_NUM_1
// on C3/H2.  Avoids UART_NUM_1 where possible because ModuleIO's generic RS-485
// bootstrap also uses UART_NUM_1.
// Requires at minimum pin_RS485_TX assigned in the board preset;
// pin_RS485_DE is optional (enables automatic RS-485 direction control).
//
// See also: https://github.com/MoonModules/MoonLight/issues/157

class DMXOutDriver : public DriverNode {
 private:
  uint16_t startChannel = 1;   // DMX start address (1-512)
  Char<32> status = "No pins";

  uint8_t pinTX = UINT8_MAX;
  uint8_t pinDE = UINT8_MAX;

// DMXOut uses UART_NUM_2 on 3-UART chips so it doesn't collide with DMXIn (UART_NUM_1) or
// ModuleIO's RS-485 bootstrap (also UART_NUM_1).  On 2-UART chips there is no UART_NUM_2, so
// both DMX nodes fall back to UART_NUM_1 — startDMX() detects the conflict at runtime.
// SOC_UART_NUM counts only the standard HP UARTs; see D_DMXIn.h for the full explanation.
#if SOC_UART_NUM > 2
  static constexpr uart_port_t uartNum = UART_NUM_2;
#else
  static constexpr uart_port_t uartNum = UART_NUM_1;  // ESP32-C3/H2: no UART_NUM_2 available
#endif
  bool dmxActive = false;
  update_handler_id_t ioUpdateHandler;

 public:
  static const char* name() { return "DMX Out"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }
  static const char* category() { return "Driver"; }

  void setup() override {
    DriverNode::setup();
    addControl(startChannel, "startChannel", "number", 1, 512);
    addControl(status, "status", "text", 0, 32, true);

    ioUpdateHandler = moduleIO->addUpdateHandler([this](const String& originId) { readPins(); });
    readPins();
  }

  void onUpdate(const JsonObject& control) override {
    DriverNode::onUpdate(control);
  }

  void readPins() {
    if (safeModeMB) return;

    bool changed = moduleIO->updatePin(pinTX, pin_RS485_TX);
    changed = moduleIO->updatePin(pinDE, pin_RS485_DE) || changed;

    if (changed) {
      stopDMX();
      if (pinTX != UINT8_MAX) {
        startDMX();
      } else {
        updateControl("status", "No pins");
      }
    }
  }

  void startDMX() {
    uart_config_t uartConfig = {
      .baud_rate = 250000,          // DMX512 baud rate
      .data_bits = UART_DATA_8_BITS,
      .parity    = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_2, // DMX512 requires 2 stop bits
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
    };

#if SOC_UART_NUM <= 2
    // On 2-UART chips DMXIn and DMXOut share UART_NUM_1.  Skip the pre-delete so that
    // uart_driver_install() returns ESP_ERR_INVALID_STATE when DMXIn already owns the port.
    esp_err_t err = uart_driver_install(uartNum, 256, 513 + 16, 0, NULL, 0);
    if (err == ESP_ERR_INVALID_STATE) {
      EXT_LOGW(ML_TAG, "DMX Out: UART_NUM_1 already in use (DMX In active?) — not starting");
      updateControl("status", "UART conflict");
      return;
    }
#else
    uart_driver_delete(uartNum);  // clean up any prior installation
    esp_err_t err = uart_driver_install(uartNum, 256, 513 + 16, 0, NULL, 0);
#endif
    if (err != ESP_OK) {
      EXT_LOGE(ML_TAG, "DMX Out: UART install failed: %s", esp_err_to_name(err));
      updateControl("status", "UART error");
      return;
    }
    uart_param_config(uartNum, &uartConfig);
    uart_set_pin(uartNum, pinTX, UART_PIN_NO_CHANGE,
                 pinDE != UINT8_MAX ? pinDE : UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (pinDE != UINT8_MAX)
      uart_set_mode(uartNum, UART_MODE_RS485_HALF_DUPLEX);

    dmxActive = true;
    updateControl("status", "Active");
    EXT_LOGI(ML_TAG, "DMX Out started TX:%d DE:%d", pinTX, pinDE);
  }

  void stopDMX() {
    if (dmxActive) {
      uart_wait_tx_done(uartNum, pdMS_TO_TICKS(100));
      uart_driver_delete(uartNum);
      dmxActive = false;
      updateControl("status", "Stopped");
    }
  }

  void loop() override {
    DriverNode::loop();  // applies brightness LUT
    if (!dmxActive) return;

    LightsHeader* header = &layerP.lights.header;
    if (header->nrOfChannels == 0) return;

    uint16_t offset = startChannel - 1;  // convert 1-based address to 0-based offset

    // Build the DMX channel payload into a 511-byte stack frame (DMX512 minus start code).
    // Zero-init covers both the startChannel offset padding and any trailing unused slots.
    // rgbwBufferMapping reorders R/G/B to the fixture's channel order (e.g. GRB, WRGB) and
    // extracts a white channel from RGB for RGBW/RGBWYP presets, applying the brightness LUT.
    // This mirrors the ArtNet output driver's per-light remapping.
    uint8_t dmxFrame[511] = {};
    uint16_t frameSize = 0;
    for (uint16_t indexP = 0; indexP < header->nrOfLights; indexP++) {
      uint16_t dstBase = offset + indexP * header->channelsPerLight;
      if (dstBase + header->channelsPerLight > 511) break;
      uint8_t* src = &layerP.lights.channelsD[indexP * header->channelsPerLight];
      uint8_t* dst = &dmxFrame[dstBase];
      memcpy(dst, src, header->channelsPerLight);
      rgbwBufferMapping(dst + header->offsetRGBW, src + header->offsetRGBW);
      if (header->offsetRGBW1 != UINT8_MAX) {
        rgbwBufferMapping(dst + header->offsetRGBW1, src + header->offsetRGBW1);
        if (header->offsetRGBW2 != UINT8_MAX) {
          rgbwBufferMapping(dst + header->offsetRGBW2, src + header->offsetRGBW2);
          if (header->offsetRGBW3 != UINT8_MAX)
            rgbwBufferMapping(dst + header->offsetRGBW3, src + header->offsetRGBW3);
        }
      }
      frameSize = dstBase + header->channelsPerLight;
    }

    const uint8_t startCode = 0;
    uart_wait_tx_done(uartNum, pdMS_TO_TICKS(50));
    uart_write_bytes(uartNum, &startCode, 1);
    uart_write_bytes_with_break(uartNum, dmxFrame, frameSize, 24);
  }

  ~DMXOutDriver() override {
    stopDMX();
    moduleIO->removeUpdateHandler(ioUpdateHandler);
  }
};

#endif
