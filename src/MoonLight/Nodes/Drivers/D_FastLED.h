/**
    @title     MoonLight
    @file      FastLED.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

class FastLEDDriver : public DriverNode {
 public:
  static const char* name() { return "FastLED Driver"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }

  char version[20] = TOSTRING(FASTLED_VERSION);  // "." TOSTRING(FASTLED_VERSION_MINOR) "." TOSTRING(FASTLED_VERSION_PATCH);
  Char<32> status = "ok";
  #if CONFIG_IDF_TARGET_ESP32S3
  uint8_t affinity = 2;  // I2S
  #elif CONFIG_IDF_TARGET_ESP32P4
  uint8_t affinity = 4;  // parlio
  #else
  uint8_t affinity = 0;  // auto
  #endif
  uint8_t temperature = 0;
  bool correction = 0;
  bool dither = false;

  void setup() override {
    DriverNode::setup();  // !!

    addControl(affinity, "affinity", "select");
    addControlValue("Auto");
    addControlValue("RMT");
    addControlValue("I2S");
    addControlValue("SPI");
  #if CONFIG_IDF_TARGET_ESP32P4
    addControlValue("Parlio");
  #endif

    addControl(temperature, "temperature", "select");
    addControlValue("Uncorrected");
    addControlValue("Candle");
    addControlValue("Tungsten100W");
    addControlValue("Halogen");

    addControl(correction, "correction", "select");
    addControlValue("Uncorrected");
    addControlValue("Typical LED");
    addControlValue("Typical SMD5050");

    addControl(dither, "dither", "checkbox");

    addControl(version, "version", "text", 0, 20, true);
    addControl(status, "status", "text", 0, 32, true);
  }

  fl::EOrder rgbOrder = GRB;
  fl::ChannelOptions options;

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) override {
    DriverNode::onUpdate(oldValue, control);  // !!

    LightsHeader* header = &layerP.lights.header;

    EXT_LOGD(ML_TAG, "%s: %s ", control["name"].as<const char*>(), control["value"].as<String>().c_str());

    if (control["name"] == "lightPreset") {
      switch (layerP.lights.header.lightPreset) {
      case lightPreset_RGB:
        rgbOrder = RGB;
        break;
      case lightPreset_RBG:
        rgbOrder = RBG;
        break;
      case lightPreset_GRB:
        rgbOrder = GRB;
        break;
      case lightPreset_GBR:
        rgbOrder = GBR;
        break;
      case lightPreset_BRG:
        rgbOrder = BRG;
        break;
      case lightPreset_BGR:
        rgbOrder = BGR;
        break;
      case lightPreset_RGBW:
        rgbOrder = RGB;
        options.mRgbw.rgbw_mode = kRGBWExactColors;
        options.mRgbw.w_placement = W3;
        break;
      case lightPreset_GRBW:
        rgbOrder = GRB;
        options.mRgbw.rgbw_mode = kRGBWExactColors;
        options.mRgbw.w_placement = W3;
        break;
      case lightPreset_WRGB:
        rgbOrder = RGB;
        options.mRgbw.rgbw_mode = kRGBWExactColors;
        options.mRgbw.w_placement = W0;
        break;
      case lightPreset_GRB6:
        rgbOrder = GRB;
        break;
      case lightPreset_RGB2040:
        // RGB2040 uses standard RGB offsets but has special channel remapping
        // for dual-channel-group architecture (handled in VirtualLayer)
        rgbOrder = RGB;
        break;
      case lightPreset_RGBWYP:
        rgbOrder = RGB;
        options.mRgbw.rgbw_mode = kRGBWExactColors;
        options.mRgbw.w_placement = W3;
        break;
      case lightPreset_MHBeeEyes150W15:
        rgbOrder = RGB;
        options.mRgbw.rgbw_mode = kRGBWExactColors;
        options.mRgbw.w_placement = W3;
        break;
      case lightPreset_MHBeTopper19x15W32:
        rgbOrder = RGB;
        options.mRgbw.rgbw_mode = kRGBWExactColors;
        options.mRgbw.w_placement = W3;
        break;
      case lightPreset_MH19x15W24:
        rgbOrder = RGB;
        options.mRgbw.rgbw_mode = kRGBWExactColors;
        options.mRgbw.w_placement = W3;
        break;
      default:
        rgbOrder = GRB;
        break;
      }
    }

    else if (control["name"] == "affinity") {
      switch (control["value"].as<uint8_t>()) {
      case 0:  // auto
        options.mAffinity = "";
        break;
      case 1:
        options.mAffinity = "RMT";
        break;
      case 2:
        options.mAffinity = "I2S";
        break;
      case 3:
        options.mAffinity = "SPI";
        break;
      case 4:
        options.mAffinity = "PARLIO";
        break;
      }
      FastLED.setExclusiveDriver(options.mAffinity.c_str());
    }

    else if (control["name"] == "temperature") {
      switch (control["value"].as<uint8_t>()) {
      case 0:
        options.mTemperature = UncorrectedTemperature;
        break;
      case 1:
        options.mTemperature = Candle;
        break;
      case 2:
        options.mTemperature = Tungsten100W;
        break;
      case 3:
        options.mTemperature = Halogen;
        break;
      }
    }

    else if (control["name"] == "correction") {
      switch (control["value"].as<uint8_t>()) {
      case 0:
        options.mCorrection = UncorrectedColor;
        break;
      case 1:
        options.mCorrection = TypicalLEDStrip;
        break;
      case 2:
        options.mCorrection = TypicalSMD5050;
        break;
      }
    }

    else if (control["name"] == "dither") {
      options.mDitherMode = control["value"].as<bool>() ? BINARY_DITHER : DISABLE_DITHER;
    }
  }

  uint16_t savedMaxPower = UINT16_MAX;
  void loop() override {
    if (FastLED.count()) {
      if (FastLED.getBrightness() != layerP.lights.header.brightness) {
        EXT_LOGD(ML_TAG, "setBrightness %d", layerP.lights.header.brightness);
        FastLED.setBrightness(layerP.lights.header.brightness);
      }

      if (layerP.maxPower != savedMaxPower) {
        EXT_LOGD(ML_TAG, "setMaxPower %d watt", layerP.maxPower);
        FastLED.setMaxPowerInMilliWatts(1000 * layerP.maxPower);  // 5v, 2000mA, to protect usb while developing
        savedMaxPower = layerP.maxPower;
      }

      // FastLED Led Controllers
      CRGB correction = CRGB(layerP.lights.header.red, layerP.lights.header.green, layerP.lights.header.blue);
      CLEDController* pCur = CLEDController::head();
      while (pCur) {
        if (pCur->getCorrection() != correction) {
          EXT_LOGD(ML_TAG, "setColorCorrection r:%d, g:%d, b:%d (#:%d)", layerP.lights.header.red, layerP.lights.header.green, layerP.lights.header.blue, pCur->size());
          pCur->setCorrection(correction);
        }
        // pCur->size();
        pCur = pCur->next();
      }

      FastLED.show();
    }
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    if (layerP.pass == 1 && !layerP.monitorPass) {
      uint8_t nrOfPins = MIN(MIN(layerP.nrOfLedPins, layerP.nrOfAssignedPins), 4);  // FastLED RMT supports max 4 pins!

      if (nrOfPins == 0) return;

      if (safeModeMB) {
        EXT_LOGW(ML_TAG, "Safe mode enabled, not adding FastLED driver");
        return;
      }

      EXT_LOGD(ML_TAG, "nrOfLedPins #:%d", nrOfPins);
      uint8_t pins[MAX_PINS] = {};

      Char<32> statusString = "#";  // truncate if larger
      statusString += nrOfPins;
      statusString += ": ";
      for (int i = 0; i < nrOfPins; i++) {
        uint8_t assignedPin = layerP.ledPinsAssigned[i];
        if (assignedPin < layerP.nrOfLedPins)
          pins[i] = layerP.ledPins[assignedPin];
        else
          pins[i] = layerP.ledPins[i];
        EXT_LOGD(ML_TAG, "onLayout pin#%d of %d: assigned:%d %d->%d #%d", i, nrOfPins, assignedPin, layerP.ledPins[i], pins[i], layerP.ledsPerPin[i]);
        Char<12> tmp;
        tmp.format(" %d#%d", pins[i], layerP.ledsPerPin[i]);
        statusString += tmp;
      }
      EXT_LOGD(ML_TAG, "status: %s", statusString.c_str());

      updateControl("status", statusString.c_str());
      moduleNodes->requestUIUpdate = true;

      // https://github.com/FastLED/FastLED/blob/master/src/fl/channels/README.md
      fl::ChipsetTimingConfig timing = fl::makeTimingConfig<fl::TIMING_WS2812_800KHZ>();
      CRGB* leds = (CRGB*)layerP.lights.channelsD;
      uint16_t startLed = 0;

      for (uint8_t pinIndex = 0; pinIndex < nrOfPins; pinIndex++) {
        EXT_LOGD(ML_TAG, "ledPin p:%d #:%d", pins[pinIndex], layerP.ledsPerPin[pinIndex]);

        fl::ChannelConfig config(pins[pinIndex], timing, fl::span<CRGB>(&leds[startLed], layerP.ledsPerPin[pinIndex]), rgbOrder, options);
        fl::ChannelPtr channel = fl::Channel::create(config);

        FastLED.add(channel);

        // FastLED.addLeds<WS2812, 16, GRB>(leds, layerP.ledsPerPin[pinIndex]);  // this works!!! (but this is static)

        startLed += layerP.ledsPerPin[pinIndex];

        CLEDController* pCur = CLEDController::head();
        EXT_LOGD(ML_TAG, "controller p:%p", pCur);
        while (pCur) {
          EXT_LOGD(ML_TAG, "controller %d %d", pCur->size(), pCur->getEnabled());
          pCur = pCur->next();
        }

      }  // for pinIndex < nrOfPins

      // Check which drivers are available
      auto drivers = FastLED.getDriverInfos();
      for (size_t i = 0; i < FastLED.getDriverCount(); i++) {
        auto& info = drivers[i];
        EXT_LOGD(ML_TAG, "Driver: %s, Priority: %d, Enabled: %s", info.name.c_str(), info.priority, info.enabled ? "yes" : "no");
      }

      CLEDController* pCur = CLEDController::head();
      EXT_LOGD(ML_TAG, "controller p:%p", pCur);
      while (pCur) {
        EXT_LOGD(ML_TAG, "controller %d %d", pCur->size(), pCur->getEnabled());
        pCur = pCur->next();
      }
    }

    FastLED.setMaxPowerInMilliWatts(1000 * layerP.maxPower);  // 5v, 2000mA, to protect usb while developing
  }

  ~FastLEDDriver() override {
    // TODO 
    // for (auto channel: channels)
    {
      // FastLED.remove(channel);
    }
  }
};

#endif