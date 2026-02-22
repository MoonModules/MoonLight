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

  // Beat detection state
  float currentBPM = 0.0f;
  uint32_t lastBeatTime = 0;
  uint32_t beatCount = 0;
  uint32_t onsetCount = 0;

  void setup() override {
    Node::setup();  // !!

    fl::string errorMsg;
    audioInput = fl::IAudioInput::create(config, &errorMsg);
    audioInput->start();

    audioProcessor.onBeat([&]() {
      beatCount++;
      lastBeatTime = fl::millis();
      sharedData.beat = true;
      EXT_LOGD(ML_TAG, "BEAT #: %d", beatCount);
    });

    audioProcessor.onVocalStart([&]() { sharedData.vocalsActive = true; });

    audioProcessor.onVocalEnd([&]() { sharedData.vocalsActive = false; });

    audioProcessor.onVocalConfidence([](float confidence) {
      static uint32_t lastPrint = 0;
      if (fl::millis() - lastPrint > 200) {
        sharedData.vocalConfidence = confidence;
        EXT_LOGD(ML_TAG, "Vocal confidence: %f", confidence);
        lastPrint = fl::millis();
      }
    });

    audioProcessor.onBass([](float level) {
      if (level > 0.01f) {
        sharedData.bassLevel = level;
        EXT_LOGD(ML_TAG, "Bass: %f", level);
      }
    });

    audioProcessor.onTreble([](float level) {
      if (level > 0.01f) {
        sharedData.trebleLevel = level;
        EXT_LOGD(ML_TAG, "Treble: %f", level);
      }
    });
  }

  void loop20ms() override {
    while (fl::AudioSample sample = audioInput->read()) {
      audioProcessor.update(sample);
    }

    sharedData.beat = false;
  }

  ~FastLEDAudioDriver() override {}
};

#endif