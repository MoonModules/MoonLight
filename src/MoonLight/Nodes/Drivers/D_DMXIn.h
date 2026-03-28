/**
    @title     MoonLight
    @file      D_DMXIn.h
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

extern SemaphoreHandle_t swapMutex;

// DMX512 input driver — receives DMX data via RS-485 and writes it to the
// channel buffer or forwards it to LightsControl.
// Uses UART_NUM_2 on chips that have it; falls back to UART_NUM_1 on C3/H2.
// Accepts pin_DMX or pin_RS485_RX from the board preset.
//
// See also: https://github.com/MoonModules/MoonLight/issues/157

class DMXInDriver : public Node {
 private:
  uint16_t startChannel = 1;   // DMX start address (1-512)
  uint8_t  mode = 0;           // 0 = Channels, 1 = LightsControl
  Char<32> status = "No pins";

  uint8_t pinDMX     = UINT8_MAX;
  uint8_t pinRS485RX = UINT8_MAX;
  uint8_t pinRX      = UINT8_MAX;  // effective RX pin (pinDMX or pinRS485RX)

#if SOC_UART_NUM > 2
  static constexpr uart_port_t uartNum = UART_NUM_2;
#else
  static constexpr uart_port_t uartNum = UART_NUM_1;  // ESP32-C3/H2 only has UART0/1
#endif
  bool dmxActive = false;
  QueueHandle_t uartQueue = nullptr;

  uint8_t  rxBuffer[513] = {};   // accumulates bytes between BREAKs
  uint16_t rxPos = 0;
  uint8_t  dmxData[513] = {};    // latest complete DMX frame
  uint16_t dmxLength = 0;
  bool     newFrame = false;

  update_handler_id_t ioUpdateHandler;

 public:
  static const char* name() { return "DMX In"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }
  static const char* category() { return "Driver"; }

  void setup() override {
    addControl(startChannel, "startChannel", "number", 1, 512);
    addControl(mode, "mode", "select");
    addControlValue("Channels");
    addControlValue("LightsControl");
    addControl(status, "status", "text", 0, 32, true);

    ioUpdateHandler = moduleIO->addUpdateHandler([this](const String& originId) { readPins(); });
    readPins();
  }

  void readPins() {
    if (safeModeMB) return;

    moduleIO->updatePin(pinDMX, pin_DMX);
    moduleIO->updatePin(pinRS485RX, pin_RS485_RX);

    uint8_t effectiveRX = (pinDMX != UINT8_MAX) ? pinDMX : pinRS485RX;

    if (effectiveRX != pinRX) {
      pinRX = effectiveRX;
      stopDMX();
      if (pinRX != UINT8_MAX) {
        startDMX();
      } else {
        updateControl("status", "No pins");
      }
    }
  }

  void startDMX() {
    uart_config_t uartConfig = {
      .baud_rate = 250000,
      .data_bits = UART_DATA_8_BITS,
      .parity    = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_2,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_delete(uartNum);
    esp_err_t err = uart_driver_install(uartNum, 1024, 0, 20, &uartQueue, 0);
    if (err != ESP_OK) {
      EXT_LOGE(ML_TAG, "DMX In: UART install failed: %s", esp_err_to_name(err));
      updateControl("status", "UART error");
      return;
    }
    uart_param_config(uartNum, &uartConfig);
    uart_set_pin(uartNum, UART_PIN_NO_CHANGE, pinRX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    dmxActive = true;
    rxPos = 0;
    newFrame = false;
    updateControl("status", "Listening");
    EXT_LOGI(ML_TAG, "DMX In started RX:%d", pinRX);
  }

  void stopDMX() {
    if (dmxActive) {
      uart_driver_delete(uartNum);
      uartQueue = nullptr;
      dmxActive = false;
      updateControl("status", "Stopped");
    }
  }

  void loop() override {
    if (!dmxActive || !uartQueue) return;

    // Drain the UART event queue — a BREAK marks the boundary between DMX frames.
    uart_event_t event;
    while (xQueueReceive(uartQueue, &event, 0)) {
      switch (event.type) {
        case UART_DATA:
          if (rxPos < 513) {
            int toRead = min((int)event.size, 513 - (int)rxPos);
            int len = uart_read_bytes(uartNum, &rxBuffer[rxPos], toRead, 0);
            if (len > 0) rxPos += len;
          }
          break;

        case UART_BREAK: {
          // Grab any remaining bytes that arrived just before the BREAK
          size_t buffered = 0;
          uart_get_buffered_data_len(uartNum, &buffered);
          if (buffered > 0 && rxPos + buffered <= 513) {
            int len = uart_read_bytes(uartNum, &rxBuffer[rxPos], buffered, 0);
            if (len > 0) rxPos += len;
          }
          // If we accumulated a valid frame (start code 0x00 = dimmer data), save it
          if (rxPos > 1 && rxBuffer[0] == 0x00) {
            memcpy(dmxData, rxBuffer, rxPos);
            dmxLength = rxPos;
            newFrame = true;
          }
          rxPos = 0;
          uart_flush_input(uartNum);
          break;
        }

        default:
          break;
      }
    }

    if (!newFrame || dmxLength <= 1) return;
    newFrame = false;

    if (mode == 0) {
      processChannels();
    } else {
      processLightsControl();
    }
  }

  // Write received DMX channel data into the physical-layer channel buffer
  void processChannels() {
    LightsHeader* header = &layerP.lights.header;
    uint16_t offset = startChannel - 1;

    if (offset >= dmxLength - 1) return;
    uint16_t available = dmxLength - 1 - offset;
    uint16_t nrChannels = min(available, (uint16_t)header->nrOfChannels);
    if (nrChannels == 0) return;

    xSemaphoreTake(swapMutex, portMAX_DELAY);
    memcpy(layerP.lights.channelsD, &dmxData[1 + offset], nrChannels);
    xSemaphoreGive(swapMutex);
  }

  // Map received DMX channels to LightsControl properties (all 0-255):
  //   CH1  = brightness      CH2  = red
  //   CH3  = green           CH4  = blue
  //   CH5  = palette         CH6  = bpm
  //   CH7  = intensity       CH8  = preset (select)
  //   CH9  = presetLoop      CH10 = firstPreset (1-64, scaled)
  //   CH11 = lastPreset  (1-64, scaled)
  void processLightsControl() {
    if (!moduleControl) return;

    uint16_t offset = startChannel - 1;
    if (offset >= dmxLength - 1) return;

    uint8_t* ch = &dmxData[1 + offset];
    uint16_t available = dmxLength - 1 - offset;

    JsonDocument doc;
    JsonObject newState = doc.to<JsonObject>();

    newState["brightness"] = ch[0];
    newState["lightsOn"] = (bool)(ch[0] > 0);
    if (available >= 2)  newState["red"]       = ch[1];
    if (available >= 3)  newState["green"]     = ch[2];
    if (available >= 4)  newState["blue"]      = ch[3];
    if (available >= 5)  newState["palette"]   = ch[4];
    if (available >= 6)  newState["bpm"]       = ch[5];
    if (available >= 7)  newState["intensity"] = ch[6];
    if (available >= 8) {
      // preset: send as action/select object so LightsControl applies it
      JsonObject preset = newState["preset"].to<JsonObject>();
      preset["action"] = "select";
      preset["select"] = ch[7];
    }
    if (available >= 9)  newState["presetLoop"]   = ch[8];
    if (available >= 10) newState["firstPreset"]  = (uint8_t)(ch[9]  * 64 / 256 + 1); // scale 0-255 → 1-64
    if (available >= 11) newState["lastPreset"]   = (uint8_t)(ch[10] * 64 / 256 + 1); // scale 0-255 → 1-64

    moduleControl->update(newState, ModuleState::update, "DMXIn");
  }

  ~DMXInDriver() override {
    stopDMX();
    moduleIO->removeUpdateHandler(ioUpdateHandler);
  }
};

#endif  // FT_MOONLIGHT
