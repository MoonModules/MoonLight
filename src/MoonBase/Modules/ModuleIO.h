/**
    @title     MoonBase
    @file      ModuleIO.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/inputoutput/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleIO_h
#define ModuleIO_h

#if FT_MOONBASE == 1

  #include "MoonBase/Module.h"

enum IO_PinUsage {
  pin_Unused,  // 0
  pin_LED_01,
  pin_LED_02,
  pin_LED_03,
  pin_LED_04,
  pin_LED_05,
  pin_LED_06,
  pin_LED_07,
  pin_LED_08,
  pin_LED_09,
  pin_LED_10,
  pin_LED_11,
  pin_LED_12,
  pin_LED_13,
  pin_LED_14,
  pin_LED_15,
  pin_LED_16,
  pin_LED_17,
  pin_LED_18,
  pin_LED_19,  // LED pins
  pin_LED_20,  // LED pins
  pin_LED_CW,
  pin_LED_WW,
  pin_LED_R,
  pin_LED_G,
  pin_LED_B,
  pin_I2S_SD,
  pin_I2S_WS,
  pin_I2S_SCK,
  pin_I2S_MCLK,
  pin_I2C_SDA,
  pin_I2C_SCL,
  pin_Button_01,
  pin_Button_02,
  pin_Button_03,
  pin_Voltage,
  pin_Current,
  pin_Infrared,
  pin_Relay,
  pin_Relay_Brightness,
  pin_DMX,
  pin_OnBoardLed,
  pin_OnBoardKey,
  pin_Battery,
  pin_Temperature,
  pin_Exposed,
  pin_SDIO_PIN_CMD,
  pin_SDIO_PIN_CLK,
  pin_SDIO_PIN_D0,
  pin_SDIO_PIN_D2,
  pin_SDIO_PIN_D3,
  pin_SDIO_PIN_D1,
  pin_Serial_TX,
  pin_Serial_RX,
  pin_Reserved,
  pin_Ethernet,
  pin_Button_OnOff,
  pin_SPI_SCK,
  pin_SPI_MISO,
  pin_SPI_MOSI,
  pin_PHY_CS,
  pin_PHY_IRQ,
  pin_count
};

enum IO_Boards {
  board_none,  //
  board_QuinLEDDigUnoV3,
  board_QuinLEDDigQuadV3,
  board_QuinLEDDigOctoV2,
  board_QuinLEDPenta,
  board_QuinLEDPentaPlus,
  board_SergUniShieldV5,
  board_SergMiniShield,
  board_SE16V1,
  board_WladiD0,
  board_WladiP4Nano,
  board_YvesV48,
  board_TroyP4Nano,
  board_AtomS3,
  board_LuxceoMood1XiaoMod,
  board_Cube202010,
  board_count
};

class ModuleIO : public Module {
 public:
  ESP32SvelteKit* _sveltekit;

  ModuleIO(PsychicHttpServer* server, ESP32SvelteKit* sveltekit) : Module("inputoutput", server, sveltekit) {
    EXT_LOGV(MB_TAG, "constructor");

    // #if CONFIG_IDF_TARGET_ESP32
    //     pinMode(19, OUTPUT); digitalWrite(19, HIGH); // for serg shield boards: to be done: move to new pin manager module, switch off for S3!!!! tbd: add pin manager
    // #endif

    _sveltekit = sveltekit;

    addUpdateHandler([&](const String& originId) { readPins(); }, false);
  }

  void setupDefinition(const JsonArray& controls) override {
    EXT_LOGV(MB_TAG, "");
    JsonObject control;  // state.data has one or more controls
    JsonArray rows;      // if a control is an array, this is the rows of the array

    control = addControl(controls, "boardPreset", "select");
    control["default"] = 0;
    addControlValue(control, BUILD_TARGET);  // 0 none
    addControlValue(control, "QuinLED Dig Uno v3");
    addControlValue(control, "QuinLED Dig Quad v3");
    addControlValue(control, "QuinLED Dig Octa v2");
    addControlValue(control, "QuinLED Penta");
    addControlValue(control, "QuinLED Penta Plus");
    addControlValue(control, "Serg Universal Shield v5");
    addControlValue(control, "Serg Mini Shield");
    addControlValue(control, "Mathieu SE16 v1");
    addControlValue(control, "Wladi D0");
    addControlValue(control, "Wladi P4 Nano");
    addControlValue(control, "Yves V48");
    addControlValue(control, "Troy P4 Nano");
    addControlValue(control, "Atom S3R");
    addControlValue(control, "Luxceo Mood1 Xiao Mod");
    addControlValue(control, "Cube202010");

    control = addControl(controls, "modded", "checkbox");
    control["default"] = false;

    control = addControl(controls, "maxPower", "number", 0, 500, false, "Watt");
    control["default"] = 10;

    control = addControl(controls, "jumper1", "checkbox");
    control["default"] = false;

    control = addControl(controls, "pins", "rows");
    control["filter"] = "!Unused";
    control["crud"] = "ru";

    rows = control["n"].to<JsonArray>();
    {
      control = addControl(rows, "GPIO", "number", 0, SOC_GPIO_PIN_COUNT - 1, true);  // ro

      control = addControl(rows, "usage", "select");
      control["default"] = 0;
      addControlValue(control, "Unused");  // 0
      addControlValue(control, "LED 01");
      addControlValue(control, "LED 02");
      addControlValue(control, "LED 03");
      addControlValue(control, "LED 04");
      addControlValue(control, "LED 05");
      addControlValue(control, "LED 06");
      addControlValue(control, "LED 07");
      addControlValue(control, "LED 08");
      addControlValue(control, "LED 09");
      addControlValue(control, "LED 10");
      addControlValue(control, "LED 11");
      addControlValue(control, "LED 12");
      addControlValue(control, "LED 13");
      addControlValue(control, "LED 14");
      addControlValue(control, "LED 15");
      addControlValue(control, "LED 16");
      addControlValue(control, "LED 17");
      addControlValue(control, "LED 18");
      addControlValue(control, "LED 19");
      addControlValue(control, "LED 20");
      addControlValue(control, "LED CW");
      addControlValue(control, "LED WW");
      addControlValue(control, "LED R");
      addControlValue(control, "LED G");
      addControlValue(control, "LED B");
      addControlValue(control, "I2S SD");
      addControlValue(control, "I2S WS");
      addControlValue(control, "I2S SCK");
      addControlValue(control, "I2S MCLK");
      addControlValue(control, "I2C SDA");
      addControlValue(control, "I2C SCL");
      addControlValue(control, "Button 01");
      addControlValue(control, "Button 02");
      addControlValue(control, "Button 03");
      addControlValue(control, "Voltage");
      addControlValue(control, "Current");
      addControlValue(control, "Infrared");
      addControlValue(control, "Relay");
      addControlValue(control, "Relay Brightness");
      addControlValue(control, "DMX in");
      addControlValue(control, "On Board LED");
      addControlValue(control, "On Board Key");
      addControlValue(control, "Battery");
      addControlValue(control, "Temperature");
      addControlValue(control, "Exposed");
      addControlValue(control, "SDIO_PIN_CMD");
      addControlValue(control, "SDIO_PIN_CLK");
      addControlValue(control, "SDIO_PIN_D0");
      addControlValue(control, "SDIO_PIN_D2");
      addControlValue(control, "SDIO_PIN_D3");
      addControlValue(control, "SDIO_PIN_D1");
      addControlValue(control, "Serial TX");
      addControlValue(control, "Serial RX");
      addControlValue(control, "Reserved");
      addControlValue(control, "Ethernet");
      addControlValue(control, "Button On/Off");
      addControlValue(control, "SPI_SCK");
      addControlValue(control, "SPI_MISO");
      addControlValue(control, "SPI_MOSI");
      addControlValue(control, "PHY_CS");
      addControlValue(control, "PHY_IRQ");

      addControl(rows, "summary", "text", 0, 32, true);  // ro
      // addControl(rows, "Valid", "checkbox", false, true, true);
      // addControl(rows, "Output", "checkbox", false, true, true);
      // addControl(rows, "RTC", "checkbox", false, true, true);

      addControl(rows, "Level", "text", 0, 32, true);     // ro
      addControl(rows, "DriveCap", "text", 0, 32, true);  // ro
    }
  }

  void setBoardPresetDefaults(uint8_t boardID) {
    JsonDocument doc;
    JsonObject object = doc.to<JsonObject>();
    object["modded"] = false;

    JsonArray pins = object["pins"].to<JsonArray>();

    // reset all pins
    for (int gpio_num = 0; gpio_num < SOC_GPIO_PIN_COUNT; gpio_num++) {
      JsonObject pin = pins.add<JsonObject>();
      pin["GPIO"] = gpio_num;
      pin["usage"] = 0;

      // Check if GPIO is valid
      bool is_valid = GPIO_IS_VALID_GPIO(gpio_num);
      bool is_output_valid = GPIO_IS_VALID_OUTPUT_GPIO(gpio_num);
      bool is_rtc_gpio = rtc_gpio_is_valid_gpio((gpio_num_t)gpio_num);

      // Get current level (works for both input and output pins)
      int level = -1;
      if (is_valid) {
        level = gpio_get_level((gpio_num_t)gpio_num);
      }

      // For RTC GPIOs, can also use RTC-specific read
      int rtc_level = -1;
  #ifndef CONFIG_IDF_TARGET_ESP32C3
      if (is_rtc_gpio) {
        rtc_level = rtc_gpio_get_level((gpio_num_t)gpio_num);  // to do find c3 alternative
      }
  #endif

      // Get drive capability (if output capable)
      gpio_drive_cap_t drive_cap = GPIO_DRIVE_CAP_DEFAULT;
      esp_err_t drive_result = ESP_FAIL;
      if (is_output_valid) {
        drive_result = gpio_get_drive_capability((gpio_num_t)gpio_num, &drive_cap);
      }

      Char<32> summary;
      summary.format("%s%s%s", is_valid ? "✅" : "", is_output_valid ? " 💡" : "", is_rtc_gpio ? " ⏰" : "");

      pin["GPIO"] = gpio_num;
      pin["summary"] = summary.c_str();
      // pin["Valid"] = is_valid;
      // pin["Output"] = is_output_valid;
      // pin["RTC"] = is_rtc_gpio;
      pin["Level"] = (level >= 0) ? (level ? "HIGH" : "LOW") : "N/A";
      pin["DriveCap"] = (drive_result == ESP_OK) ? drive_cap_to_string(drive_cap) : "N/A";
    }

    if (boardID == board_SE16V1) {
      object["maxPower"] = 500;
      uint8_t ledPins[16] = {47, 48, 21, 38, 14, 39, 13, 40, 12, 41, 11, 42, 10, 2, 3, 1};  // LED_PINS
      for (int i = 0; i < sizeof(ledPins); i++) pins[ledPins[i]]["usage"] = pin_LED_01 + i;
      pins[0]["usage"] = pin_Button_01;
      pins[45]["usage"] = pin_Button_02;
      pins[46]["usage"] = pin_Button_OnOff;
      pins[8]["usage"] = pin_Voltage;
      pins[9]["usage"] = pin_Current;

      if (_state.data["jumper1"]) {
        pins[5]["usage"] = pin_Infrared;
      } else {  // default
        pins[5]["usage"] = pin_SPI_MISO;
        pins[6]["usage"] = pin_SPI_MOSI;
        pins[7]["usage"] = pin_SPI_SCK;
        pins[15]["usage"] = pin_PHY_CS;
        pins[18]["usage"] = pin_PHY_IRQ;
      }

    } else if (boardID == board_QuinLEDDigUnoV3) {
      // Dig-Uno-V3
      // esp32-d0 (4MB)
      // erase flash first
      // Lolin Wifi fix no -> add as board preset
      // you might need to reset your router if you first run WLED on the same MCU
      // remove fuse then flash, Nice !!!
      object["maxPower"] = 50;  // max 75, but 10A fuse
      pins[16]["usage"] = pin_LED_01;
      pins[3]["usage"] = pin_LED_02;
      pins[0]["usage"] = pin_Button_01;
      pins[15]["usage"] = pin_Relay;
      // pins[2]["usage"] = pin_I2S_SD;
      // pins[12]["usage"] = pin_I2S_WS;
      // pins[13]["usage"] = pin_Temperature;
      // pins[15]["usage"] = pin_I2S_SCK;
      // pins[16]["usage"] = pin_LED_03;
      // pins[32]["usage"] = pin_Exposed;
    } else if (boardID == board_QuinLEDDigQuadV3) {
      // Dig-Quad-V3
      object["maxPower"] = 150;
      uint8_t ledPins[4] = {16, 3, 1, 4};  // LED_PINS
      for (int i = 0; i < sizeof(ledPins); i++) pins[ledPins[i]]["usage"] = pin_LED_01 + i;
      pins[0]["usage"] = pin_Button_01;
      pins[15]["usage"] = pin_Relay;

      // pins[2]["usage"] = pin_I2S_SD;
      // pins[12]["usage"] = pin_I2S_WS;
      // pins[13]["usage"] = pin_Temperature;
      // pins[15]["usage"] = pin_I2S_SCK;
      // pins[32]["usage"] = pin_Exposed;
    } else if (boardID == board_QuinLEDDigOctoV2) {
      uint8_t ledPins[8] = {0, 1, 2, 3, 4, 5, 12, 13};  // LED_PINS
      for (int i = 0; i < sizeof(ledPins); i++) pins[ledPins[i]]["usage"] = pin_LED_01 + i;
    } else if (boardID == board_QuinLEDPenta) {
      uint8_t ledPins[5] = {14, 13, 12, 4, 2};  // LED_PINS
      for (int i = 0; i < sizeof(ledPins); i++) pins[ledPins[i]]["usage"] = pin_LED_01 + i;
      pins[34]["usage"] = pin_Button_01;
      pins[35]["usage"] = pin_Button_02;
      pins[39]["usage"] = pin_Button_03;
      pins[1]["usage"] = pin_I2C_SDA;
      pins[3]["usage"] = pin_I2C_SCL;
    } else if (boardID == board_QuinLEDPentaPlus) {
      pins[33]["usage"] = pin_LED_CW;
      pins[32]["usage"] = pin_LED_WW;
      pins[2]["usage"] = pin_LED_R;
      pins[4]["usage"] = pin_LED_G;
      pins[12]["usage"] = pin_LED_B;
      pins[36]["usage"] = pin_Button_01;
      pins[39]["usage"] = pin_Button_02;
      pins[34]["usage"] = pin_Button_03;
      pins[15]["usage"] = pin_I2C_SDA;
      pins[16]["usage"] = pin_I2C_SCL;
      pins[13]["usage"] = pin_Relay_Brightness;
      pins[05]["usage"] = pin_LED_01;
    } else if (boardID == board_SergMiniShield) {
      object["maxPower"] = 50;  // 10A Fuse ...
      pins[1]["usage"] = pin_LED_01;
      pins[3]["usage"] = pin_LED_02;
    } else if (boardID == board_SergUniShieldV5) {
      object["maxPower"] = 50;  // 10A Fuse ...
      pins[1]["usage"] = pin_LED_01;
      pins[3]["usage"] = pin_LED_02;
      pins[19]["usage"] = pin_Relay_Brightness;
    } else if (boardID == board_WladiD0) {
      pins[3]["usage"] = pin_Voltage;
    } else if (boardID == board_WladiP4Nano) {                                           // https://shop.myhome-control.de/ABC-WLED-ESP32-P4-Shield/HW10027
      object["maxPower"] = 10;                                                           // USB compliant
      uint8_t ledPins[16] = {21, 20, 25, 5, 7, 23, 8, 27, 3, 22, 24, 4, 46, 47, 2, 48};  // LED_PINS
      for (int i = 0; i < sizeof(ledPins); i++) pins[ledPins[i]]["usage"] = pin_LED_01 + i;
    } else if (boardID == board_YvesV48) {
      pins[3]["usage"] = pin_LED_01;
    } else if (boardID == board_TroyP4Nano) {
      object["maxPower"] = 10;                                                            // USB compliant
      uint8_t ledPins[16] = {2, 3, 4, 5, 6, 20, 21, 22, 23, 26, 27, 32, 33, 36, 47, 48};  // LED_PINS
      for (int i = 0; i < sizeof(ledPins); i++) pins[ledPins[i]]["usage"] = pin_LED_01 + i;
      pins[7]["usage"] = pin_I2C_SDA;
      pins[8]["usage"] = pin_I2C_SCL;
      pins[9]["usage"] = pin_Reserved;  // I2S Sound Output Pin
      pins[10]["usage"] = pin_I2S_WS;
      pins[11]["usage"] = pin_I2S_SD;
      pins[12]["usage"] = pin_I2S_SCK;
      pins[13]["usage"] = pin_I2S_MCLK;
      pins[14]["usage"] = pin_SDIO_PIN_D0;   // ESP-Hosted WiFi pins
      pins[15]["usage"] = pin_SDIO_PIN_D1;   // ESP-Hosted WiFi pins
      pins[16]["usage"] = pin_SDIO_PIN_D2;   // ESP-Hosted WiFi pins
      pins[17]["usage"] = pin_SDIO_PIN_D3;   // ESP-Hosted WiFi pins
      pins[18]["usage"] = pin_SDIO_PIN_CLK;  // ESP-Hosted WiFi pins
      pins[19]["usage"] = pin_SDIO_PIN_CMD;  // ESP-Hosted WiFi pins
      pins[24]["usage"] = pin_Reserved;      // USB Pins
      pins[25]["usage"] = pin_Reserved;      // USB Pins
      uint8_t ethernetPins[6] = {28, 29, 30, 31, 34, 35};
      for (int i = 0; i < sizeof(ethernetPins); i++) pins[ethernetPins[i]]["usage"] = pin_Ethernet;  // Ethernet Pins
      pins[37]["usage"] = pin_Serial_TX;
      pins[38]["usage"] = pin_Serial_RX;
      // 24-25 is is USB, but so is 26-27 but they're exposed on the header and work OK for pin outout.
      // 6 is C5 wakeup - but works fine for pin outout.
      // 45 is SD power but it's NC without hacking the board.
      // 53 is for PA enable but it's exposed on header and works for WLED pin output. Best to not use it but left available.
      // 54 is "C4 EN pin" so I guess we shouldn't fuck with that.
    } else if (boardID == board_AtomS3) {
      uint8_t ledPins[4] = {5, 6, 7, 8};  // LED_PINS
      for (int i = 0; i < sizeof(ledPins); i++) pins[ledPins[i]]["usage"] = pin_LED_01 + i;
    } else if (boardID == board_Cube202010) {
      object["maxPower"] = 50;
      uint8_t ledPins[10] = {22, 21, 14, 18, 5, 4, 2, 15, 13, 12};  // LED_PINS
                                                                    // char pins[80] = "2,3,4,16,17,18,19,21,22,23,25,26,27,32,33";  //(D0), more pins possible. to do: complete list.
      for (int i = 0; i < sizeof(ledPins); i++) pins[ledPins[i]]["usage"] = pin_LED_01 + i;
    } else {                    // default
      object["maxPower"] = 10;  // USB compliant
      pins[16]["usage"] = pin_LED_01;
      // trying to add more pins, but these pins not liked by esp32-d0-16MB ... 🚧
      // pins[4]["usage"] = pin_LED_02;
      // pins[5]["usage"] = pin_LED_03;
      // pins[6]["usage"] = pin_LED_04;
      // pins[7]["usage"] = pin_LED_05;
      // pins[8]["usage"] = pin_LED_06;
    }
    // String xxx;
    // serializeJson(_state.data, xxx);
    // EXT_LOGD(MB_TAG, "%s", xxx.c_str());
    // EXT_LOGD(MB_TAG, "%s", xxx.c_str());

    EXT_LOGD(ML_TAG, "boardID %d", boardID);
    // serializeJson(object, Serial);Serial.println();

    update(object, ModuleState::update, _moduleName + "server");
  }

  uint8_t newBoardID = UINT8_MAX;

  void onUpdate(const UpdatedItem& updatedItem) override {
    if (updatedItem.name == "boardPreset" && !_state.updateOriginId.contains("server")) {  // not done by this module: done by UI
      // if booting and modded is false or ! booting
      if ((updatedItem.oldValue == "" && _state.data["modded"] == false) || updatedItem.oldValue != "") {  // only update unmodded
        EXT_LOGD(MB_TAG, "%s %s[%d]%s[%d].%s = %s -> %s", _state.updateOriginId.c_str(), updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
        newBoardID = updatedItem.value;  // run in sveltekit task
      }
    } else if (updatedItem.name == "modded" && !_state.updateOriginId.contains("server")) {  // not done by this module: done by UI
      if (updatedItem.value == false) {
        EXT_LOGD(MB_TAG, "%s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
        newBoardID = _state.data["boardPreset"];  // run in sveltekit task
      }
    } else if (updatedItem.name == "usage" && !_state.updateOriginId.contains("server")) {  // not done by this module: done by UI
      // EXT_LOGD(MB_TAG, "%s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
      // set pins to default if modded is turned off
      JsonDocument doc;
      JsonObject object = doc.to<JsonObject>();
      object["modded"] = true;
      update(object, ModuleState::update, _moduleName + "server");
    }
  }

  // Function to convert drive capability to string
  const char* drive_cap_to_string(gpio_drive_cap_t cap) {
    switch (cap) {
    case GPIO_DRIVE_CAP_0:
      return "WEAK";
    case GPIO_DRIVE_CAP_1:
      return "STRONGER";
    case GPIO_DRIVE_CAP_2:
      return "MEDIUM";
    case GPIO_DRIVE_CAP_3:
      return "STRONGEST";
    default:
      return "UNKNOWN";
    }
  }

  void loop() override {
    // run in sveltekit task
    Module::loop();

    if (newBoardID != UINT8_MAX) {
      setBoardPresetDefaults(newBoardID);  // run from sveltekit task
      newBoardID = UINT8_MAX;
    }
  }

  void readPins() {
  #if FT_ENABLED(FT_ETHERNET)
    EXT_LOGD(MB_TAG, "Try to configure ethernet");
    EthernetSettingsService* ess = _sveltekit->getEthernetSettingsService();
    #ifdef CONFIG_IDF_TARGET_ESP32S3
    ess->v_ETH_SPI_SCK = UINT8_MAX;
    ess->v_ETH_SPI_MISO = UINT8_MAX;
    ess->v_ETH_SPI_MOSI = UINT8_MAX;
    ess->v_ETH_PHY_CS = UINT8_MAX;
    ess->v_ETH_PHY_IRQ = UINT8_MAX;
    // if ethernet pins change
    // find the pins needed
    for (JsonObject pinObject : _state.data["pins"].as<JsonArray>()) {
      uint8_t usage = pinObject["usage"];
      uint8_t gpio = pinObject["GPIO"];
      if (usage == pin_SPI_SCK) ess->v_ETH_SPI_SCK = gpio;
      if (usage == pin_SPI_MISO) ess->v_ETH_SPI_MISO = gpio;
      if (usage == pin_SPI_MOSI) ess->v_ETH_SPI_MOSI = gpio;
      if (usage == pin_PHY_CS) ess->v_ETH_PHY_CS = gpio;
      if (usage == pin_PHY_IRQ) ess->v_ETH_PHY_IRQ = gpio;
    }

    // allocate the pins found
    if (ess->v_ETH_SPI_SCK != UINT8_MAX && ess->v_ETH_SPI_MISO != UINT8_MAX && ess->v_ETH_SPI_MOSI != UINT8_MAX && ess->v_ETH_PHY_CS != UINT8_MAX && ess->v_ETH_PHY_IRQ != UINT8_MAX) {
      // ess->v_ETH_PHY_TYPE = ETH_PHY_W5500;
      // ess->v_ETH_PHY_ADDR = 1;
      ess->v_ETH_PHY_RST = -1;  // not wired
      ess->initEthernet();      // restart ethernet
    }
    #endif
  #endif
  }
};

#endif
#endif
