/**
    @title     MoonLight
    @file      D_FastLEDDriver.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

// using the new FastLED channel api
// https://github.com/FastLED/FastLED/blob/master/src/fl/channels/README.md

class FastLEDDriver : public DriverNode {
 public:
  static const char* name() { return "FastLED Driver"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }

  Char<32> version = FASTLED_BUILD;
  Char<32> status = "NoInit";
  Char<32> engine = "Auto";
  uint8_t affinity = 0;  // auto
  uint8_t temperature = 0;
  uint8_t correction = 0;
  bool dither = false;

  void setup() override {
    DriverNode::setup();  // !!

    addControl(affinity, "affinity", "select");
    addControlValue("Auto");
    addControlValue("RMT");
    addControlValue("I2S");  // #ifndef CONFIG_IDF_TARGET_ESP32 ... not now as it changes the order numbering
    addControlValue("SPI");
  #if CONFIG_IDF_TARGET_ESP32P4
    addControlValue("Parlio");
  #endif
    addControl(engine, "engine", "text", 0, 32, true);  // the resolved engine based on affinity

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

    ioUpdateHandler = moduleIO->addUpdateHandler([this](const String& originId) {
      uint8_t nrOfPins = MIN(layerP.nrOfLedPins, layerP.nrOfAssignedPins);

      EXT_LOGD(ML_TAG, "recreate channels and configs %s %d", originId.c_str(), nrOfPins);
      // do something similar as in destructor: delete existing channels
      // do something similar as in onLayout: create new channels

      // should we check here for maxPower changes?
    });
    controlUpdateHandler = moduleControl->addUpdateHandler([this](const String& originId) {
      // brightness changes here?
    });

    // Register event listeners via FastLED
    auto& events = FastLED.channelEvents();

    // Called when channel is created
    events.onChannelCreated.add([](const fl::Channel& ch) { EXT_LOGD(ML_TAG, "Channel created: %s", ch.name().c_str()); });

    // Called when channel data is enqueued to engine
    events.onChannelEnqueued.add([this](const fl::Channel& ch, const fl::string& engine) {
      if (engine != this->engine.c_str()) {
        EXT_LOGD(ML_TAG, "Resolved engine %s %s → %s", ch.name().c_str(), this->engine.c_str(), engine.c_str());
        updateControl("engine", engine.c_str());
        moduleNodes->requestUIUpdate = true;
      }
    });
  }

  fl::EOrder rgbOrder = GRB;
  fl::ChannelOptions options = fl::ChannelOptions();

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) override {
    DriverNode::onUpdate(oldValue, control);  // !!

    // EXT_LOGD(ML_TAG, "%s: %s ", control["name"].as<const char*>(), control["value"].as<String>().c_str());

    if (control["name"] == "lightPreset") {
      options.mRgbw = RgbwInvalid::value();  // Reset RGBW options so RGB-only presets don't inherit stale W config

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
      // FastLED.setExclusiveDriver(options.mAffinity.c_str());
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
    // DriverNode::loop(); // no need to call this as FastLED is not using ledsDriver LUT tables ...

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
      CRGB colorCorrection = CRGB(layerP.lights.header.red, layerP.lights.header.green, layerP.lights.header.blue);
      CLEDController* pCur = CLEDController::head();
      while (pCur) {
        if (pCur->getCorrection() != colorCorrection) {
          EXT_LOGD(ML_TAG, "setColorCorrection r:%d, g:%d, b:%d (#:%d)", layerP.lights.header.red, layerP.lights.header.green, layerP.lights.header.blue, pCur->size());
          pCur->setCorrection(colorCorrection);
        }
        // pCur->size();
        pCur = pCur->next();
      }

      FastLED.show();
    }
  }

  fl::vector<fl::ChannelPtr> channels;

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    if (layerP.pass == 1 && !layerP.monitorPass) {
      uint8_t nrOfPins = MIN(layerP.nrOfLedPins, layerP.nrOfAssignedPins);

      if (affinity == 1 && nrOfPins > 4) nrOfPins = 4;  // FastLED RMT supports max 4 pins!, what about SPI?

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

      version.format("4.0 pre release! %s", FASTLED_BUILD);  // version.format("%s %s", TOSTRING(FASTLED_VERSION), FASTLED_BUILD);
      updateControl("version", version);
      updateControl("status", statusString.c_str());
      moduleNodes->requestUIUpdate = true;

      fl::ChipsetTimingConfig timing = fl::makeTimingConfig<fl::TIMING_WS2812_800KHZ>();
      CRGB* leds = (CRGB*)layerP.lights.channelsD;
      uint16_t startLed = 0;

      FastLED.clear(ClearFlags::CHANNELS);
      // FastLED.reset(ResetFlags::CHANNELS);

      for (uint8_t pinIndex = 0; pinIndex < nrOfPins; pinIndex++) {
        EXT_LOGD(ML_TAG, "ledPin p:%d #:%d rgb:%d aff:%s", pins[pinIndex], layerP.ledsPerPin[pinIndex], rgbOrder, options.mAffinity.c_str());

        fl::ChannelConfig config(pins[pinIndex], timing, fl::span<CRGB>(&leds[startLed], layerP.ledsPerPin[pinIndex]), rgbOrder, options);
        fl::ChannelPtr channel = fl::Channel::create(config);

        FastLED.add(channel);
        channels.push_back(channel);

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

    // FastLED.setMaxPowerInMilliWatts(1000 * layerP.maxPower);  // 5v, 2000mA, to protect usb while developing
  }

  ~FastLEDDriver() override {
    // global: ensure only one FastLEDDriver instance exists. If multiple driver nodes are possible, this destructor will tear down channels for all of them. If singleton is guaranteed by design, consider documenting that assumption at the class level.
    auto& events = FastLED.channelEvents();
    events.onChannelCreated.clear();
    events.onChannelEnqueued.clear();
    FastLED.clear(ClearFlags::CHANNELS);
    // FastLED.reset(ResetFlags::CHANNELS);

    moduleIO->removeUpdateHandler(ioUpdateHandler);
    moduleControl->removeUpdateHandler(controlUpdateHandler);
  }

 private:
  update_handler_id_t ioUpdateHandler;
  update_handler_id_t controlUpdateHandler;
};

#endif