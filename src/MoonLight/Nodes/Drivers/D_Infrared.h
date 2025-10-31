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
  #ifdef BUILD_TARGET_ESP32_S3_STEPHANELEC_16P

    #include "driver/rmt_rx.h"

    #define IR_DRIVER_TAG "⛑️"

    /**
     * @brief NEC timing spec
     */
    #define NEC_LEADING_CODE_DURATION_0 9000
    #define NEC_LEADING_CODE_DURATION_1 4500
    #define NEC_PAYLOAD_ZERO_DURATION_0 560
    #define NEC_PAYLOAD_ZERO_DURATION_1 560
    #define NEC_PAYLOAD_ONE_DURATION_0 560
    #define NEC_PAYLOAD_ONE_DURATION_1 1690
    #define NEC_REPEAT_CODE_DURATION_0 9000
    #define NEC_REPEAT_CODE_DURATION_1 2250
    #define EXAMPLE_IR_NEC_DECODE_MARGIN 200  // Tolerance for parsing RMT symbols into bit stream

class IRDriver : public Node {
 public:
  static const char* name() { return "IR Driver"; }
  static uint8_t dim() { return _NoD; }      // Dimensions not relevant for drivers?
  static const char* tags() { return "☸️"; }  // use emojis see https://moonmodules.org/MoonLight/moonlight/overview/#emoji-coding, ☸️ for drivers

  uint8_t pin = 5;
  uint16_t s_nec_code_address;
  uint16_t s_nec_code_command;
  bool new_pin_in_waiting = false;
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
      .signal_range_min_ns = 1250,      // the shortest duration for NEC signal is 560us, 1250ns < 560us, valid signal won't be treated as noise
      .signal_range_max_ns = 12000000,  // the longest duration for NEC signal is 9000us, 12000000ns > 9000us, the receive won't stop early
  };

  void setup() override {
    // controls will show in the UI
    // for different type of controls see other Nodes
    addControl(pin, "pin", "slider", 1, SOC_GPIO_PIN_COUNT);
  }

  static bool ir_rmt_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t* edata, void* user_data) {
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t receive_queue = (QueueHandle_t)user_data;
    // send the received RMT symbols to the parser task
    xQueueSendFromISR(receive_queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
  }

  static inline bool nec_check_in_range(uint32_t signal_duration, uint32_t spec_duration) {
    return (signal_duration < (spec_duration + EXAMPLE_IR_NEC_DECODE_MARGIN)) && (signal_duration > (spec_duration - EXAMPLE_IR_NEC_DECODE_MARGIN));
  }

  static bool nec_parse_logic0(rmt_symbol_word_t* rmt_nec_symbols) {
    return nec_check_in_range(rmt_nec_symbols->duration0, NEC_PAYLOAD_ZERO_DURATION_0) && nec_check_in_range(rmt_nec_symbols->duration1, NEC_PAYLOAD_ZERO_DURATION_1);
  }

  static bool nec_parse_logic1(rmt_symbol_word_t* rmt_nec_symbols) {
    return nec_check_in_range(rmt_nec_symbols->duration0, NEC_PAYLOAD_ONE_DURATION_0) && nec_check_in_range(rmt_nec_symbols->duration1, NEC_PAYLOAD_ONE_DURATION_1);
  }

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
        return false;
      }
      cur++;
    }
    // save address and command
    s_nec_code_address = address;
    s_nec_code_command = command;
    return true;
  }

  static bool nec_parse_frame_repeat(rmt_symbol_word_t* rmt_nec_symbols) {
    return nec_check_in_range(rmt_nec_symbols->duration0, NEC_REPEAT_CODE_DURATION_0) && nec_check_in_range(rmt_nec_symbols->duration1, NEC_REPEAT_CODE_DURATION_1);
  }

  void parse_nec_frame(rmt_symbol_word_t* rmt_nec_symbols, size_t symbol_num) {
    if (false) {
      MB_LOGD(IR_DRIVER_TAG, "NEC frame start---\r\n");
      for (size_t i = 0; i < symbol_num; i++) {
        MB_LOGI(IR_DRIVER_TAG, "{%d:%d},{%d:%d}\r\n", rmt_nec_symbols[i].level0, rmt_nec_symbols[i].duration0, rmt_nec_symbols[i].level1, rmt_nec_symbols[i].duration1);
      }
      MB_LOGI(IR_DRIVER_TAG, "---NEC frame end: ");
    }
    // decode RMT symbols
    switch (symbol_num) {
    case 34:  // NEC normal frame
      if (nec_parse_frame(rmt_nec_symbols)) {
        MB_LOGI(IR_DRIVER_TAG, "Address=%04X, Command=%04X", s_nec_code_address, s_nec_code_command);
        uint32_t combined_code = (((uint32_t)s_nec_code_address) << 16) | s_nec_code_command;

        // Here we should implement code address & command to action mapping
        controlModule->update(
            [&](ModuleState& state) {
              bool changed = false;
              // possible actions:
              //  lightsOn
              //  brightness
              //  red
              //  green
              //  blue
              //  palette
              //  preset
              //  presetLoop
              //  firstPreset
              //  lastPreset
              //  monitorOn

              UpdatedItem updatedItem;
              if (combined_code == 0xFF00A35C) {  // Brightness increase
                updatedItem.name = "brightness";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = min(state.data[updatedItem.name].as<uint8_t>() + 10, 255);
                changed = true;
              } else if (combined_code == 0xFF00A25D) {  // Brightness decrease
                updatedItem.name = "brightness";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = max(state.data[updatedItem.name].as<uint8_t>() - 10, 0);
                changed = true;
              } else if (combined_code == 0xFF00BF40) {  // Lights on/off
                updatedItem.name = "lightsOn";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = state.data[updatedItem.name].as<bool>()? false:true;
                changed = true;
              } else if (combined_code == 0xFF00BE41) {  // next button - go to next preset
                updatedItem.name = "preset";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = state.data[updatedItem.name].as<uint8_t>() == 63? 0:state.data[updatedItem.name].as<uint8_t>() + 1;
                changed = true;
              } else if (combined_code == 0xFF00F30C) {  // DIY1 button - enable preset #1
                updatedItem.name = "preset";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = 1;
                changed = true;
              } else if (combined_code == 0xFF00F20D) {  // DIY2 button - enable preset #2
                updatedItem.name = "preset";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = 2;
                changed = true;
              } else if (combined_code == 0xFF00F10E) {  // DIY3 button - enable preset #3
                updatedItem.name = "preset";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = 3;
                changed = true;
              } else if (combined_code == 0xFF00F708) {  // DIY4 button - enable preset #4
                updatedItem.name = "preset";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = 4;
                changed = true;
              } else if (combined_code == 0xFF00F609) {  // DIY5 button - enable preset #5
                updatedItem.name = "preset";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = 5;
                changed = true;
              } else if (combined_code == 0xFF00F50A) {  // DIY6 button - enable preset #6
                updatedItem.name = "preset";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = 6;
                changed = true;
              } else if (combined_code == 0xFF00EB14) {  // increase red
                updatedItem.name = "red";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = min(state.data[updatedItem.name].as<uint8_t>() + 10, 255);
                changed = true;
              } else if (combined_code == 0xFF00EF10) {  // decrease red
                updatedItem.name = "red";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = max(state.data[updatedItem.name].as<uint8_t>() - 10, 0);
                changed = true;
              } else if (combined_code == 0xFF00EA15) {  // increase green
                updatedItem.name = "green";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = min(state.data[updatedItem.name].as<uint8_t>() + 10, 255);
                changed = true;
              } else if (combined_code == 0xFF00EE11) {  // decrease green
                updatedItem.name = "green";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = max(state.data[updatedItem.name].as<uint8_t>() - 10, 0);
                changed = true;
              } else if (combined_code == 0xFF00E916) {  // increase blue
                updatedItem.name = "blue";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = min(state.data[updatedItem.name].as<uint8_t>() + 10, 255);
                changed = true;
              } else if (combined_code == 0xFF00ED12) {  // decrease blue
                updatedItem.name = "blue";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = max(state.data[updatedItem.name].as<uint8_t>() - 10, 0);
                changed = true;
              } else if ((s_nec_code_address == 0xFF00) && (s_nec_code_command == 0xA25D)) {  // palette increase
                updatedItem.name = "palette";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = max(state.data[updatedItem.name].as<uint8_t>() + 1, 8);  // to do: replace 8 with max palette count
                changed = true;
              } else if ((s_nec_code_address == 0xFF00) && (s_nec_code_command == 0xA25D)) {  // palette decrease
                updatedItem.name = "palette";
                updatedItem.oldValue = state.data[updatedItem.name].as<String>();
                state.data[updatedItem.name] = max(state.data[updatedItem.name].as<uint8_t>() - 1, 0);
                changed = true;
              }

              if (changed) {
                updatedItem.value = state.data[updatedItem.name];
                state.onUpdate(updatedItem);
              }

              return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;  // notify StatefulService
            },
            IR_DRIVER_TAG);
      }
      break;
    case 2:  // NEC repeat frame
      if (nec_parse_frame_repeat(rmt_nec_symbols)) {
        MB_LOGI(IR_DRIVER_TAG, "Address=%04X, Command=%04X, repeat", s_nec_code_address, s_nec_code_command);
      }
      break;
    default:
      MB_LOGI(IR_DRIVER_TAG, "Unknown NEC frame");
      break;
    }
  }

  void onUpdate(String& oldValue, JsonObject control) {
    if (control["name"] == "pin") {
      new_pin_in_waiting = true;
    }
  }

  // use for continuous actions, e.g. reading data from sensors or sending data to lights (e.g. LED drivers or Art-Net)
  void loop() override {
    if (receive_queue) {
      if (xQueueReceive(receive_queue, &rx_data, 0) == pdPASS) {
        // parse the receive symbols and print the result
        parse_nec_frame(rx_data.received_symbols, rx_data.num_symbols);
        // start receive again
        ESP_ERROR_CHECK(rmt_receive(rx_channel, raw_symbols, sizeof(raw_symbols), &receive_config));
      }
    }

    if (new_pin_in_waiting) {
      MB_LOGI(IR_DRIVER_TAG, "Changing to pin #%d", pin);

      if (rx_channel) {
        MB_LOGI(IR_DRIVER_TAG, "Removing callback");
        ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &cbs_empty, receive_queue));
        MB_LOGI(IR_DRIVER_TAG, "Stopping RMT reception");
        ESP_ERROR_CHECK(rmt_disable(rx_channel));
        MB_LOGI(IR_DRIVER_TAG, "Deleting old RX channel");
        ESP_ERROR_CHECK(rmt_del_channel(rx_channel));
        rx_channel = NULL;
      }

      if (receive_queue) {
        vQueueDelete(receive_queue);
        receive_queue = NULL;
      }

      rx_channel_cfg.gpio_num = (gpio_num_t)pin;
      MB_LOGI(IR_DRIVER_TAG, "create RMT RX channel");
      ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_channel_cfg, &rx_channel));

      MB_LOGI(IR_DRIVER_TAG, "Enable RMT RX channel");
      ESP_ERROR_CHECK(rmt_enable(rx_channel));

      MB_LOGI(IR_DRIVER_TAG, "Register RX done callback");
      receive_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
      assert(receive_queue);
      ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &cbs, receive_queue));

      MB_LOGI(IR_DRIVER_TAG, "Arm receive");
      ESP_ERROR_CHECK(rmt_receive(rx_channel, raw_symbols, sizeof(raw_symbols), &receive_config));

      new_pin_in_waiting = false;
    }
  };
};

  #endif
#endif