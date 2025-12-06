/**
    @title     MoonLight
    @file      D_Infrared.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

  #include "driver/rmt_rx.h"

  #define IR_DRIVER_TAG "⛑️"

  /**
   * @brief NEC timing spec
   */
  #define NEC_LEADING_CODE_DURATION_0 9000
  #define NEC_LEADING_CODE_DURATION_1 4500
  #define NEC_PAYLOAD_ZERO_DURATION_0 562
  #define NEC_PAYLOAD_ZERO_DURATION_1 562
  #define NEC_PAYLOAD_ONE_DURATION_0 562
  #define NEC_PAYLOAD_ONE_DURATION_1 1687
  #define NEC_REPEAT_CODE_DURATION_0 9000
  #define NEC_REPEAT_CODE_DURATION_1 2250
  #define EXAMPLE_IR_NEC_DECODE_MARGIN 300  // Tolerance for parsing RMT symbols into bit stream

class IRDriver : public Node {
 public:
  static const char* name() { return "Infrared Driver"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }  // use emojis see https://moonmodules.org/MoonLight/moonlight/overview/#emoji-coding, ☸️ for drivers

  uint8_t pinInfrared = UINT8_MAX;
  uint8_t irPreset = 1;

  void readPins() {
    moduleIO->read([&](ModuleState& state) {
      pinInfrared = UINT8_MAX;
      for (JsonObject pinObject : state.data["pins"].as<JsonArray>()) {
        uint8_t usage = pinObject["usage"];
        if (usage == pin_Infrared) {
          pinInfrared = pinObject["GPIO"];
          EXT_LOGD(ML_TAG, "pin_Infrared found %d", pinInfrared);
        }
      }
      if (pinInfrared != UINT8_MAX) {
        EXT_LOGI(IR_DRIVER_TAG, "Changing to pin #%d", pinInfrared);

        if (rx_channel) {
          EXT_LOGI(IR_DRIVER_TAG, "Removing callback");
          ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &cbs_empty, receive_queue));
          EXT_LOGI(IR_DRIVER_TAG, "Stopping RMT reception");
          ESP_ERROR_CHECK(rmt_disable(rx_channel));
          EXT_LOGI(IR_DRIVER_TAG, "Deleting old RX channel");
          ESP_ERROR_CHECK(rmt_del_channel(rx_channel));
          rx_channel = NULL;
        }

        if (receive_queue) {
          vQueueDelete(receive_queue);
          receive_queue = NULL;
        }

        rx_channel_cfg.gpio_num = (gpio_num_t)pinInfrared;
        EXT_LOGI(IR_DRIVER_TAG, "create RMT RX channel");
        ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_channel_cfg, &rx_channel));

        EXT_LOGI(IR_DRIVER_TAG, "Enable RMT RX channel");
        ESP_ERROR_CHECK(rmt_enable(rx_channel));

        EXT_LOGI(IR_DRIVER_TAG, "Register RX done callback");
        receive_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
        assert(receive_queue);
        ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &cbs, receive_queue));

        EXT_LOGI(IR_DRIVER_TAG, "Arm receive");
        ESP_ERROR_CHECK(rmt_receive(rx_channel, raw_symbols, sizeof(raw_symbols), &receive_config));
      }
      // for (int i = 0; i < sizeof(pins); i++) EXT_LOGD(ML_TAG, "pin %d = %d", i, pins[i]);
    });
  }

  void setup() override {
    addControl(irPreset, "irPreset", "select");
    addControlValue("Swiss remote");
    addControlValue("Athom");  // see https://www.athom.tech/blank-1/wled-esp32-music-addressable-led-strip-controller
    addControlValue("Luxceo");

    moduleIO->addUpdateHandler([&](const String& originId) { readPins(); }, false);
    readPins();  // initially
  }

  uint32_t codeOn;
  uint32_t codeOff;
  uint32_t codeBrightnessInc;
  uint32_t codeBrightnessDec;
  uint32_t codeRedInc;
  uint32_t codeRedDec;
  uint32_t codeGreenInc;
  uint32_t codeGreenDec;
  uint32_t codeBlueInc;
  uint32_t codeBlueDec;
  uint32_t codePaletteInc;
  uint32_t codePaletteDec;
  uint32_t codePresetInc;
  uint32_t codePresetDec;
  uint32_t codePreset0;
  uint32_t codePreset1;
  uint32_t codePreset2;
  uint32_t codePreset3;
  uint32_t codePreset4;
  uint32_t codePreset5;
  uint32_t codePreset6;
  uint32_t codePresetLoopInc;
  uint32_t codePresetLoopDec;
  uint32_t codeFirstPresetInc;
  uint32_t codeFirstPresetDec;
  uint32_t codeLastPresetInc;
  uint32_t codeLastPresetDec;

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) override {
    if (control["name"] == "irPreset") {
      uint8_t value = control["value"];
      switch (value) {
      case 0:  // Swiss remote
        codeOn = 0xFF00BF40;
        codeOff = 0xFF00BF40;
        codeBrightnessInc = 0xFF00A35C;
        codeBrightnessDec = 0xFF00A25D;
        codeRedInc = 0xFF00EB14;
        codeRedDec = 0xFF00EF10;
        codeGreenInc = 0xFF00EA15;
        codeGreenDec = 0xFF00EE11;
        codeBlueInc = 0xFF00E916;
        codeBlueDec = 0xFF00ED12;
        codePaletteInc = 0xFF00A25D;
        codePaletteDec = 0xFF00A25D;
        codePreset1 = 0xFF00F30C;
        codePreset2 = 0xFF00F20D;
        codePreset3 = 0xFF00F10E;
        codePreset4 = 0xFF00F708;
        codePreset5 = 0xFF00F609;
        codePreset6 = 0xFF00F50A;
        break;
      case 1:  // Dutch remote
        codeOn = 0xEF00FC03;
        codeOff = 0xEF00FD02;
        codeBrightnessInc = 0xEF00F40B;
        codeBrightnessDec = 0xEF00F906;
        codeRedInc = 0xEF00F708;
        codeRedDec = 0xEF00F30C;
        codeGreenInc = 0xEF00F609;
        codeGreenDec = 0xEF00F20D;
        codeBlueInc = 0xEF00F50A;
        codeBlueDec = 0xEF00F10E;
        codePaletteInc = 0xEF00E916;
        codePaletteDec = 0xEF00EA15;
        codePresetInc = 0xEF00FF00;
        codePresetDec = 0xEF00FE01;
        codePresetLoopInc = 0xEF00FB04;
        codePresetLoopDec = 0xEF00FA05;
        codeFirstPresetInc = 0;
        codeFirstPresetDec = 0;
        codeLastPresetInc = 0;
        codeLastPresetDec = 0;
        break;
      case 2:  // Luxceo Mood1
        codeOn = 0x7F80ED12;
        codeOff = 0x7F80E51A;
        codeBrightnessInc = 0x7F80FD02;
        codeBrightnessDec = 0x7F80FA05;
        // codeRedInc = 0xEF00F708;
        // codeRedDec = 0xEF00F30C;
        // codeGreenInc = 0xEF00F609;
        // codeGreenDec = 0xEF00F20D;
        // codeBlueInc = 0xEF00F50A;
        // codeBlueDec = 0xEF00F10E;
        codePaletteInc = 0x7F80F10E;
        codePaletteDec = 0x7F80F20D;
        codePresetInc = 0x7F80FE01;
        codePresetDec = 0x7F80FB04;
        codePresetLoopInc = 0xEF00FB04;
        codePresetLoopDec = 0xEF00FA05;
        // codeFirstPresetInc = 0;
        // codeFirstPresetDec = 0;
        // codeLastPresetInc = 0;
        // codeLastPresetDec = 0;
        break;
      }
    }
  }

  uint16_t s_nec_code_address;
  uint16_t s_nec_code_command;
  rmt_rx_done_event_data_t rx_data;
  rmt_symbol_word_t raw_symbols[64];  // 64 symbols should be sufficient for a standard NEC frame
  QueueHandle_t receive_queue = NULL;
  rmt_channel_handle_t rx_channel = NULL;

  rmt_rx_event_callbacks_t cbs = {
      .on_recv_done = IRDriver::ir_rmt_rx_done_callback,
  };
  rmt_rx_event_callbacks_t cbs_empty = {
      .on_recv_done = NULL,
  };

  rmt_rx_channel_config_t rx_channel_cfg = {
      .gpio_num = GPIO_NUM_5,
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 1000000,  // 1MHz resolution, 1 tick = 1us
      .mem_block_symbols = 64,   // amount of RMT symbols that the channel can store at a time
  };

  // the following timing requirement is based on NEC protocol
  rmt_receive_config_t receive_config = {
      .signal_range_min_ns = 3186,      // the shortest duration for NEC signal is 560us, (set to max value)
      .signal_range_max_ns = 12000000,  // the longest duration for NEC signal is 9000us, 12000000ns > 9000us, the receive won't stop early
  };

  static bool ir_rmt_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t* edata, void* user_data) {
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t receive_queue = (QueueHandle_t)user_data;
    // send the received RMT symbols to the parser task
    xQueueSendFromISR(receive_queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
  }

  static inline bool nec_check_in_range(uint32_t signal_duration, uint32_t spec_duration) { return (signal_duration < (spec_duration + EXAMPLE_IR_NEC_DECODE_MARGIN)) && (signal_duration > (spec_duration - EXAMPLE_IR_NEC_DECODE_MARGIN)); }

  static bool nec_parse_logic0(rmt_symbol_word_t* rmt_nec_symbols) { return nec_check_in_range(rmt_nec_symbols->duration0, NEC_PAYLOAD_ZERO_DURATION_0) && nec_check_in_range(rmt_nec_symbols->duration1, NEC_PAYLOAD_ZERO_DURATION_1); }

  static bool nec_parse_logic1(rmt_symbol_word_t* rmt_nec_symbols) { return nec_check_in_range(rmt_nec_symbols->duration0, NEC_PAYLOAD_ONE_DURATION_0) && nec_check_in_range(rmt_nec_symbols->duration1, NEC_PAYLOAD_ONE_DURATION_1); }

  bool nec_parse_frame(rmt_symbol_word_t* rmt_nec_symbols) {
    rmt_symbol_word_t* cur = rmt_nec_symbols;
    uint16_t address = 0;
    uint16_t command = 0;
    bool valid_leading_code = nec_check_in_range(cur->duration0, NEC_LEADING_CODE_DURATION_0) && nec_check_in_range(cur->duration1, NEC_LEADING_CODE_DURATION_1);
    if (!valid_leading_code) {
      return false;
    }
    cur++;
    for (int i = 0; i < 16; i++) {
      if (nec_parse_logic1(cur)) {
        address |= 1 << i;
      } else if (nec_parse_logic0(cur)) {
        address &= ~(1 << i);
      } else {
        EXT_LOGI(IR_DRIVER_TAG, "addr bit decode error");
        return false;
      }
      cur++;
    }
    for (int i = 0; i < 16; i++) {
      if (nec_parse_logic1(cur)) {
        command |= 1 << i;
      } else if (nec_parse_logic0(cur)) {
        command &= ~(1 << i);
      } else {
        EXT_LOGI(IR_DRIVER_TAG, "command bit decode error");
        return false;
      }
      cur++;
    }
    // save address and command
    s_nec_code_address = address;
    s_nec_code_command = command;
    return true;
  }

  static bool nec_parse_frame_repeat(rmt_symbol_word_t* rmt_nec_symbols) { return nec_check_in_range(rmt_nec_symbols->duration0, NEC_REPEAT_CODE_DURATION_0) && nec_check_in_range(rmt_nec_symbols->duration1, NEC_REPEAT_CODE_DURATION_1); }

  void parse_nec_frame(rmt_symbol_word_t* rmt_nec_symbols, size_t symbol_num) {
    if (false) {
      EXT_LOGD(IR_DRIVER_TAG, "NEC frame start---\r\n");
      for (size_t i = 0; i < symbol_num; i++) {
        EXT_LOGI(IR_DRIVER_TAG, "{%d:%d},{%d:%d}\r\n", rmt_nec_symbols[i].level0, rmt_nec_symbols[i].duration0, rmt_nec_symbols[i].level1, rmt_nec_symbols[i].duration1);
      }
      EXT_LOGI(IR_DRIVER_TAG, "---NEC frame end: ");
    }
    // decode RMT symbols

    bool valid_parsing = false;
    bool nec_repeat = false;
    if (symbol_num == 34)
      valid_parsing = nec_parse_frame(rmt_nec_symbols);
    else if (symbol_num == 2) {
      valid_parsing = nec_parse_frame_repeat(rmt_nec_symbols);
      nec_repeat = true;
    } else {
      // EXT_LOGI(IR_DRIVER_TAG, "Unknown NEC frame");
      return;
    }

    // create combined code
    uint32_t combined_code = (((uint32_t)s_nec_code_address) << 16) | s_nec_code_command;

    // do not allow 0s, which also match non initialize IR codes
    if ((combined_code == 0) || (valid_parsing == false)) {
      return;
    }

    EXT_LOGI(IR_DRIVER_TAG, "AddressCommand=%04X%04X %s", s_nec_code_address, s_nec_code_command, symbol_num == 2 ? "Longpress" : "");

    JsonDocument doc;
    JsonObject newState = doc.to<JsonObject>();

    // remaining actions:
    //  presetLoop
    //  firstPreset
    //  lastPreset
    //  monitorOn

    moduleControl->read([&](ModuleState& state) {
      if (combined_code == codeBrightnessInc) {  // Brightness increase
        newState["brightness"] = min(state.data["brightness"].as<uint8_t>() + 5, 255);
      } else if (combined_code == codeBrightnessDec) {  // Brightness decrease
        newState["brightness"] = max(state.data["brightness"].as<uint8_t>() - 5, 0);
      } else if (combined_code == codeRedInc) {  // increase red
        newState["red"] = min(state.data["red"].as<uint8_t>() + 5, 255);
      } else if (combined_code == codeRedDec) {  // decrease red
        newState["red"] = max(state.data["red"].as<uint8_t>() - 5, 0);
      } else if (combined_code == codeGreenInc) {  // increase green
        newState["green"] = min(state.data["green"].as<uint8_t>() + 5, 255);
      } else if (combined_code == codeGreenDec) {  // decrease green
        newState["green"] = max(state.data["green"].as<uint8_t>() - 5, 0);
      } else if (combined_code == codeBlueInc) {  // increase blue
        newState["blue"] = min(state.data["blue"].as<uint8_t>() + 5, 255);
      } else if (combined_code == codeBlueDec) {  // decrease blue
        newState["blue"] = max(state.data["blue"].as<uint8_t>() - 5, 0);
      } else if (combined_code == codePresetLoopInc) {
        newState["presetLoop"] = min(state.data["presetLoop"].as<uint8_t>() + 5, 255);
      } else if (combined_code == codePresetLoopDec) {
        newState["presetLoop"] = max(state.data["presetLoop"].as<uint8_t>() - 5, 0);
      } else if (combined_code == codeFirstPresetInc) {
        newState["firstPreset"] = min(state.data["firstPreset"].as<uint8_t>() + 5, 255);
      } else if (combined_code == codeFirstPresetDec) {
        newState["firstPreset"] = max(state.data["firstPreset"].as<uint8_t>() - 5, 0);
      } else if (combined_code == codeLastPresetInc) {
        newState["lastPreset"] = min(state.data["lastPreset"].as<uint8_t>() + 5, 255);
      } else if (combined_code == codeLastPresetDec) {
        newState["lastPreset"] = max(state.data["lastPreset"].as<uint8_t>() - 5, 0);
      }

      // find the current preset in the list, select the next

      // do not process longpress
      if (nec_repeat == false) {
        if (combined_code == codeOff || combined_code == codeOn) {  // Lights on/off
          newState["lightsOn"] = state.data["lightsOn"].as<bool>() ? false : true;
        } else if (combined_code == codePaletteInc) {                             // palette increase
          newState["palette"] = min(state.data["palette"].as<uint8_t>() + 1, 8);  // to do: replace 8 with max palette count
        } else if (combined_code == codePaletteDec) {                             // palette decrease
          newState["palette"] = max(state.data["palette"].as<uint8_t>() - 1, 0);
        } else if (combined_code == codePresetDec) {  // next button - go to previous preset
          newState["preset"] = state.data["preset"];
          newState["preset"]["action"] = "click";
          newState["preset"]["select"] = getNextItemInArray(state.data["preset"]["list"], state.data["preset"]["selected"], true);  // backwards
        } else if (combined_code == codePresetInc) {                                                                                // next button - go to next preset
          newState["preset"] = state.data["preset"];
          newState["preset"]["action"] = "click";
          newState["preset"]["select"] = getNextItemInArray(state.data["preset"]["list"], state.data["preset"]["selected"]);
        } else if (combined_code == codePreset0) {  // DIY1 button - enable preset #0
          newState["preset"] = state.data["preset"];
          newState["preset"]["action"] = "click";
          newState["preset"]["select"] = 0;
        } else if (combined_code == codePreset1) {  // DIY1 button - enable preset #1
          newState["preset"] = state.data["preset"];
          newState["preset"]["action"] = "click";
          newState["preset"]["select"] = 1;
        } else if (combined_code == codePreset2) {  // DIY1 button - enable preset #2
          newState["preset"] = state.data["preset"];
          newState["preset"]["action"] = "click";
          newState["preset"]["select"] = 2;
        } else if (combined_code == codePreset3) {  // DIY3 button - enable preset #3
          newState["preset"] = state.data["preset"];
          newState["preset"]["action"] = "click";
          newState["preset"]["select"] = 3;
        } else if (combined_code == codePreset4) {  // DIY4 button - enable preset #4
          newState["preset"] = state.data["preset"];
          newState["preset"]["action"] = "click";
          newState["preset"]["select"] = 4;
        } else if (combined_code == codePreset5) {  // DIY5 button - enable preset #5
          newState["preset"] = state.data["preset"];
          newState["preset"]["action"] = "click";
          newState["preset"]["select"] = 5;
        } else if (combined_code == codePreset6) {  // DIY6 button - enable preset #6
          newState["preset"] = state.data["preset"];
          newState["preset"]["action"] = "click";
          newState["preset"]["select"] = 6;
        }
      }

      // update the state and ModuleState::update processes the changes behind the scenes
      if (newState.size()) {
        // serializeJson(doc, Serial);
        // Serial.println();
        moduleControl->update(newState, ModuleState::update, IR_DRIVER_TAG);  // Do not add server in the originID as that blocks updates, see execOnUpdate
      }
    });
  }

  void loop() override {
    if (receive_queue) {
      if (xQueueReceive(receive_queue, &rx_data, 0) == pdPASS) {
        if (rx_data.num_symbols != 1) EXT_LOGD(IR_DRIVER_TAG, "Received symbols: #%d", rx_data.num_symbols);  // will not be processed (only 34 and 2)
        // parse the receive symbols and print the result
        parse_nec_frame(rx_data.received_symbols, rx_data.num_symbols);
        // start receive again
        ESP_ERROR_CHECK(rmt_receive(rx_channel, raw_symbols, sizeof(raw_symbols), &receive_config));
      }
    }
  };
};

#endif