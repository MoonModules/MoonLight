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

  #define I2S_CLK_PIN 6  // Serial Clock (SCK) (BLUE)
  #define I2S_WS_PIN 4   // Word Select (WS) (GREEN)
  #define I2S_SD_PIN 5   // Serial Data (SD) (YELLOW)
  #define I2S_CHANNEL fl::Left

static fl::AudioConfigI2S i2sConfig(I2S_WS_PIN, I2S_SD_PIN, I2S_CLK_PIN, 0, I2S_CHANNEL, 44100, 16, fl::Philips);
static fl::AudioConfig config(i2sConfig);
static fl::shared_ptr<fl::IAudioInput> audioInput;

class FastLEDAudioDriver : public Node {
 public:
  static const char* name() { return "FastLED Audio"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }

  fl::AudioProcessor audioProcessor;

  void setup() override {
    Node::setup();  // !!

    fl::string errorMsg;
    audioInput = fl::IAudioInput::create(config, &errorMsg);
    if (!audioInput) {
      EXT_LOGE(ML_TAG, "Failed to create audio input: %s", errorMsg.c_str());
      return;
    }
    audioInput->start();

    audioProcessor.onBeat([]() {
      sharedData.beat = true;
      EXT_LOGD(ML_TAG, "onBeat");
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

    audioProcessor.onTreble([](float level) {
      if (level > 0.01f) {
        sharedData.trebleLevel = level;
        // EXT_LOGD(ML_TAG, "onTreble: %f", level);
      }
    });
  }

  void loop20ms() override {
    if (!audioInput) return;

    sharedData.beat = false;

    while (fl::AudioSample sample = audioInput->read()) {
      audioProcessor.update(sample);
    }
  }

  ~FastLEDAudioDriver() override {
    if (audioInput) {
      audioInput->stop();
    }
  }
};

#endif