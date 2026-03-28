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

#if SOC_UART_NUM > 2
  static constexpr uart_port_t uartNum = UART_NUM_2;
#else
  static constexpr uart_port_t uartNum = UART_NUM_1;  // ESP32-C3/H2 only has UART0/1
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

    uart_driver_delete(uartNum);  // clean up any prior installation
    esp_err_t err = uart_driver_install(uartNum, 256, 513 + 16, 0, NULL, 0);
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
    uint16_t nrChannels = header->nrOfChannels;
    if (offset + nrChannels > 512) nrChannels = 512 - offset;

    // Wait for previous frame to finish, then build the DMX frame in three
    // sequential UART writes (queued into the TX FIFO as a contiguous stream):
    //   1. start code (1 byte, always 0x00 = dimmer data)
    //   2. zero padding for startChannel offset (0 bytes when startChannel == 1)
    //   3. channel data directly from channelsD, with trailing BREAK
    uart_wait_tx_done(uartNum, pdMS_TO_TICKS(50));
    const uint8_t startCode = 0;
    uart_write_bytes(uartNum, &startCode, 1);
    for (uint16_t i = 0; i < offset; i++)
      uart_write_bytes(uartNum, &startCode, 1);  // reuse zero byte for padding
    uart_write_bytes_with_break(uartNum, layerP.lights.channelsD, nrChannels, 24);
  }

  ~DMXOutDriver() override {
    stopDMX();
    moduleIO->removeUpdateHandler(ioUpdateHandler);
  }
};

#endif
