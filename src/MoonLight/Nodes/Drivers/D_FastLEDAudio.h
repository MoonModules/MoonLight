/**
    @title     MoonLight
    @file      D_FastLEDAudio.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

  #include "fl/audio.h"
  #include "fl/audio/audio_processor.h"
  #include "fl/audio_input.h"
  #include "fl/time_alpha.h"

// https://github.com/FastLED/FastLED/blob/master/src/fl/audio/README.md

class FastLEDAudioDriver : public Node {
 private:
  // Member variables for audio configuration
  fl::AudioConfigI2S* i2sConfig = nullptr;
  fl::AudioConfig* config = nullptr;
  fl::shared_ptr<fl::IAudioInput> audioInput;

 public:
  static const char* name() { return "FastLED Audio"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }

  fl::AudioProcessor audioProcessor;

  update_handler_id_t ioUpdateHandler;

  bool signalConditioning = true;
  bool autoGain = false;
  bool noiseFloorTracking = false;
  uint8_t channel = fl::Left;

  void setup() override {
    addControl(signalConditioning, "signalConditioning", "checkbox");
    addControl(autoGain, "autoGain", "checkbox");
    addControl(noiseFloorTracking, "noiseFloorTracking", "checkbox");
    addControl(channel, "channel", "select");
    addControlValue("Left");
    addControlValue("Right");
    addControlValue("Both");

    ioUpdateHandler = moduleIO->addUpdateHandler([this](const String& originId) { readPins(); }, true);
    readPins();  // Node added at runtime so initial IO update not received so run explicitly

    audioProcessor.onBeat([]() {
      sharedData.beat = true;
      // EXT_LOGD(ML_TAG, "onBeat");
    });

    audioProcessor.onVocalStart([]() {
      sharedData.vocalsActive = true;
      // EXT_LOGD(ML_TAG, "onVocalStart");
    });

    audioProcessor.onVocalEnd([]() {
      sharedData.vocalsActive = false;
      // EXT_LOGD(ML_TAG, "onVocalEnd");
    });

    audioProcessor.onVocalConfidence([](float confidence) {
      sharedData.vocalConfidence = sharedData.vocalsActive ? confidence : 0.0;
      // EXT_LOGD(ML_TAG, "onVocalConfidence %d", confidence);
    });

    audioProcessor.onBass([](float level) {
      if (level > 0.01f) {
        sharedData.bassLevel = level;
        // EXT_LOGD(ML_TAG, "onBass: %f", level);
      }
    });

    audioProcessor.onMid([](float level) {
      if (level > 0.01f) {
        sharedData.midLevel = level;
        // EXT_LOGD(ML_TAG, "onMid: %f", level);
      }
    });

    audioProcessor.onTreble([](float level) {
      if (level > 0.01f) {
        sharedData.trebleLevel = level;
        // EXT_LOGD(ML_TAG, "onTreble: %f", level);
      }
    });
    audioProcessor.onPercussion([](fl::PercussionType type) {
      // EXT_LOGD(ML_TAG, "onPercussion: %d", type);
      sharedData.percussionType = (uint8_t)type;
    });
  }

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) override {
    if (control["name"] == "signalConditioning") {
      audioProcessor.setSignalConditioningEnabled(signalConditioning);
    }
    if (control["name"] == "autoGain") {
      audioProcessor.setAutoGainEnabled(autoGain);
    }
    if (control["name"] == "noiseFloorTracking") {
      audioProcessor.setNoiseFloorTrackingEnabled(noiseFloorTracking);
    }
    if (control["name"] == "channel" && oldValue != "") {  // not on boot as readPins will do it then
      // recreate with the new channel
      stopAudio();
      startAudio();
    }
  }

  uint8_t pinI2SSD = UINT8_MAX;
  uint8_t pinI2SWS = UINT8_MAX;
  uint8_t pinI2SSCK = UINT8_MAX;

  bool updatePin(uint8_t& pin, const uint8_t pinUsage) {
    bool i2sPinsChanged = false;
    moduleIO->read(
        [&](ModuleState& state) {
          for (JsonObject pinObject : state.data["pins"].as<JsonArray>()) {
            if (pinObject["usage"] == pinUsage && pin != pinObject["GPIO"]) {
              pin = pinObject["GPIO"];
              i2sPinsChanged = true;
            }
          }
        },
        name());
    return i2sPinsChanged;  // an empty pin means it is not allocated anymore
  }

  void readPins() {
    if (safeModeMB) {
      EXT_LOGW(ML_TAG, "Safe mode enabled, not adding pins");
      return;
    }

    bool changed = updatePin(pinI2SWS, pin_I2S_WS);
    changed = updatePin(pinI2SSD, pin_I2S_SD) || changed;
    changed = updatePin(pinI2SSCK, pin_I2S_SCK) || changed;

    if (changed) {
      EXT_LOGI(ML_TAG, "(re)creating audioInput %d %d %d", pinI2SWS, pinI2SSD, pinI2SSCK);
      stopAudio();
      if (pinI2SWS != UINT8_MAX && pinI2SSD != UINT8_MAX && pinI2SSCK != UINT8_MAX) startAudio();
    }
  }

  void loop() override {
    if (!audioInput) return;

    sharedData.beat = false;
    sharedData.percussionType = UINT8_MAX;

    while (fl::AudioSample sample = audioInput->read()) {
      audioProcessor.update(sample);
    }
  }

  void startAudio() {
    // Create configuration objects
    i2sConfig = new fl::AudioConfigI2S(pinI2SWS, pinI2SSD, pinI2SSCK, 0, channel == 1 ? fl::Right : channel == 2 ? fl::Both : fl::Left, 44100, 16, fl::Philips);

    config = new fl::AudioConfig(*i2sConfig);

    fl::string errorMsg;
    audioInput = fl::IAudioInput::create(*config, &errorMsg);
    if (!audioInput) {
      EXT_LOGE(ML_TAG, "Failed to create audio input: %s", errorMsg.c_str());
      return;
    }
    audioInput->start();
  }

  void stopAudio() {
    if (audioInput) {
      audioInput->stop();
      audioInput.reset();  // Explicitly release shared_ptr, even makes it a nullptr...
    }

    // Clean up raw pointers
    if (config) {
      delete config;
      config = nullptr;
    }

    if (i2sConfig) {
      delete i2sConfig;
      i2sConfig = nullptr;
    }
  }

  ~FastLEDAudioDriver() override {
    stopAudio();
    moduleIO->removeUpdateHandler(ioUpdateHandler);
  }
};

#endif