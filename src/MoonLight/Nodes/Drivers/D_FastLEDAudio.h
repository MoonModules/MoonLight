/**
    @title     MoonLight
    @file      D_FastLEDAudio.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

  #include "fl/audio/audio.h"
  #include "fl/audio/input.h"
  #include "fl/audio/audio_processor.h"
  #include "fl/audio/detector/equalizer.h"
// #include "fl/time_alpha.h"

// https://github.com/FastLED/FastLED/blob/master/src/fl/audio/README.md

class FastLEDAudioDriver : public Node {
 private:
  // Member variables for audio configuration
  fl::audio::ConfigI2S* i2sConfig = nullptr;
  fl::audio::ConfigPdm* pdmConfig = nullptr;
  fl::audio::Config* config = nullptr;
  fl::shared_ptr<fl::audio::IInput> audioInput;

 public:
  static const char* name() { return "FastLED Audio"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }
  static const char* category() { return "Driver"; }

  fl::audio::Processor audioProcessor;

  bool signalConditioning = false; // if true nothng is displayed ...
  // bool autoGain = false;
  bool noiseFloorTracking = false;
  uint8_t channel = (uint8_t)fl::audio::AudioChannel::Left;
  uint8_t gain = 128;
  bool drainBuffer = false; // if false 60 fps. otherwise 40 fps
  Char<32> status = "No pins";

  void setup() override {
    addControl(signalConditioning, "signalConditioning", "checkbox");
    addControl(gain, "gain", "slider");
    // addControl(autoGain, "autoGain", "checkbox"); // option was removed
    addControl(noiseFloorTracking, "noiseFloorTracking", "checkbox");
    addControl(channel, "channel", "select");
    addControlValue("Left");
    addControlValue("Right");
    addControlValue("Both");
    addControl(drainBuffer, "drainBuffer", "checkbox");
    addControl(status, "status", "text", 0, 32, true);

    ioUpdateHandler = moduleIO->addUpdateHandler([this](const String& originId) { readPins(); });
    readPins();  // Node added at runtime so initial IO update not received so run explicitly

    // audioProcessor.onBeat([]() {
    //   sharedData.beat = true;
    //   // EXT_LOGD(ML_TAG, "onBeat");
    // });

    // audioProcessor.onVocalStart([]() {
    //   sharedData.vocalsActive = true;
    //   // EXT_LOGD(ML_TAG, "onVocalStart");
    // });

    // audioProcessor.onVocalEnd([]() {
    //   sharedData.vocalsActive = false;
    //   // EXT_LOGD(ML_TAG, "onVocalEnd");
    // });

    // audioProcessor.onVocalConfidence([](float confidence) {
    //   sharedData.vocalConfidence = sharedData.vocalsActive ? confidence : 0.0;
    //   // EXT_LOGD(ML_TAG, "onVocalConfidence %d", confidence);
    // });

    // audioProcessor.onBass([](float level) {
    //   if (level > 0.01f) {
    //     sharedData.bassLevel = level;
    //     // EXT_LOGD(ML_TAG, "onBass: %f", level);
    //   }
    // });

    // audioProcessor.onMid([](float level) {
    //   if (level > 0.01f) {
    //     sharedData.midLevel = level;
    //     // EXT_LOGD(ML_TAG, "onMid: %f", level);
    //   }
    // });

    // audioProcessor.onTreble([](float level) {
    //   if (level > 0.01f) {
    //     sharedData.trebleLevel = level;
    //     // EXT_LOGD(ML_TAG, "onTreble: %f", level);
    //   }
    // });

    // audioProcessor.onPercussion([](fl::PercussionType type) {
    //   // EXT_LOGD(ML_TAG, "onPercussion: %d", type);
    //   sharedData.percussionType = (uint8_t)type;
    // });

    // // Each drum hit triggers a different color
    // audioProcessor.onKick([]() {
    //   // gFlashColor = CRGB::Red;
    // });

    // audioProcessor.onSnare([]() {
    //   // gFlashColor = CRGB::Yellow;
    // });

    // audioProcessor.onHiHat([]() {
    //   // gFlashColor = CRGB::Cyan;
    // });

    // audioProcessor.onTom([]() {
    //   // gFlashColor = CRGB::Purple;
    // });

    // Callback: get everything in one struct
    // audioProcessor.onEqualizer([](const fl::Equalizer& eq) {
    //   eq.bass, eq.mid, eq.treble, eq.volume, eq.zcf — all 0.0-1.0
    //   eq.bins — span<const float, 16>, each 0.0-1.0
    //   for (int i = 0; i < 16; ++i) {
    //     sharedData.bands[i] = static_cast<uint8_t>(eq.bins[i] * 255);
    //   }
    //   const float norm = (eq.volumeNormFactor > 0.000001f) ? eq.volumeNormFactor : 1.0f;
    //   sharedData.volume = eq.volume / norm;
    //   sharedData.volumeRaw = static_cast<int16_t>(sharedData.volume * 32767.0f);
    //   // sharedData.majorPeak = eq.dominantMagnitude;
    // });
  }

  void onUpdate(const JsonObject& control) override {
    if (control["name"] == "signalConditioning") {
      audioProcessor.setSignalConditioningEnabled(signalConditioning);
    }
    // if (control["name"] == "autoGain") {
    //   audioProcessor.setAutoGainEnabled(autoGain);
    // }
    if (control["name"] == "gain") {
      audioProcessor.setGain(gain / 128.0f);
    }
    if (control["name"] == "noiseFloorTracking") {
      audioProcessor.setNoiseFloorTrackingEnabled(noiseFloorTracking);
    }
    if (control["name"] == "channel" && audioInput) {  // skip init-time recreate; readPins handles initial startup
      // recreate with the new channel
      stopService();
      startService();
    }
  }

  uint8_t pinI2SSD = UINT8_MAX;
  uint8_t pinI2SWS = UINT8_MAX;
  uint8_t pinI2SSCK = UINT8_MAX;

  void readPins() {
    if (safeModeMB) {
      EXT_LOGW(ML_TAG, "Safe mode enabled, not adding pins");
      return;
    }

    bool changed = moduleIO->updatePin(pinI2SWS, pin_I2S_WS);
    changed = moduleIO->updatePin(pinI2SSD, pin_I2S_SD) || changed;
    changed = moduleIO->updatePin(pinI2SSCK, pin_I2S_SCK) || changed;

    if (changed) {
      stopService();
      if (pinI2SWS != UINT8_MAX && pinI2SSD != UINT8_MAX) {
        EXT_LOGI(ML_TAG, "(re)creating audioInput WS:%d SD:%d SCK:%d (%s)", pinI2SWS, pinI2SSD, pinI2SSCK, pinI2SSCK != UINT8_MAX ? "I2S" : "PDM");
        startService();
      } else {
        updateControl("status", "No pins");
      }
    }
  }

  void loop() override {
    if (!audioInput) return;

    sharedData.fl_beat = false;
    sharedData.fl_kick = false;
    sharedData.fl_tom = false;
    sharedData.fl_hihat = false;
    sharedData.fl_snare = false;
    // sharedData.percussionType = UINT8_MAX;

    // To verify ...
    // The FastLED audio input uses a background task/interrupt to fill a sample buffer.
    // Calling audioInput->read() only once per iteration drains a single sample while leaving the remaining buffered samples unprocessed.
    // With 44.1 kHz input and typical loop cadence (~20 ms), roughly 800+ samples accumulate and are discarded each frame, causing severe data loss and degraded EQ/beat/BPM detection.

    if (drainBuffer) {
      while (fl::audio::Sample sample = audioInput->read()) {
        audioProcessor.update(sample);
      }

    } else {
      fl::audio::Sample sample = audioInput->read();
      if (sample.isValid()) {
        audioProcessor.update(sample);
      }
    }

    for (int i = 0; i < 16; ++i) {
      sharedData.bands[i] = static_cast<uint8_t>(audioProcessor.getEqBin(i) * 255);
    }
    const float norm = (audioProcessor.getEqVolumeNormFactor() > 0.000001f) ? audioProcessor.getEqVolumeNormFactor() : 1.0f;
    sharedData.volume = audioProcessor.getEqVolume() / norm;
    sharedData.volumeRaw = static_cast<int16_t>(sharedData.volume * 32767.0f);

    sharedData.fl_bassLevel = audioProcessor.getEqBass();
    sharedData.fl_midLevel = audioProcessor.getEqMid();
    sharedData.fl_trebleLevel = audioProcessor.getEqTreble();

    sharedData.fl_vocalConfidence = audioProcessor.getVocalConfidence();
    sharedData.fl_beatConfidence = audioProcessor.getBeatConfidence();

    sharedData.fl_hihat = audioProcessor.isHiHat();
    sharedData.fl_kick = audioProcessor.isKick();
    sharedData.fl_snare = audioProcessor.isSnare();
    sharedData.fl_tom = audioProcessor.isTom();

    sharedData.fl_beat = sharedData.fl_beatConfidence > 0.5f;  // audioProcessor.isBeat(); // not implemented yet ...

    sharedData.fl_bpm = audioProcessor.getBPM();
  }

  void startService() {
    if (pinI2SSCK != UINT8_MAX) {
      // Standard I2S microphone (3 pins: WS, SD, SCK)
      i2sConfig = new fl::audio::ConfigI2S(pinI2SWS, pinI2SSD, pinI2SSCK, 0, channel == 1 ? fl::audio::AudioChannel::Right : channel == 2 ? fl::audio::AudioChannel::Both : fl::audio::AudioChannel::Left, 44100, 16, fl::audio::I2SCommFormat::Philips);
      config = new fl::audio::Config(*i2sConfig);
    } else {
      // PDM microphone (2 pins: SD=data, WS=clock) — e.g. QuinLED Dig-Next-2
      pdmConfig = new fl::audio::ConfigPdm(pinI2SSD, pinI2SWS, 0);
      config = new fl::audio::Config(*pdmConfig);
    }

    fl::string errorMsg;
    audioInput = fl::audio::IInput::create(*config, &errorMsg);
    if (!audioInput) {
      EXT_LOGE(ML_TAG, "Failed to create audio input: %s", errorMsg.c_str());
      updateControl("status", errorMsg.c_str());
      return;
    }
    audioInput->start();
    fl::string startError;
    if (audioInput->error(&startError)) {
      EXT_LOGE(ML_TAG, "Audio input error after start: %s", startError.c_str());
      updateControl("status", startError.c_str());
    } else {
      updateControl("status", pinI2SSCK != UINT8_MAX ? "I2S active" : "PDM active");
    }
  }

  void stopService() {
    if (audioInput) {
      audioInput->stop();
      audioInput.reset();
      updateControl("status", "Stopped");
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

    if (pdmConfig) {
      delete pdmConfig;
      pdmConfig = nullptr;
    }
  }

  ~FastLEDAudioDriver() override {
    stopService();
    moduleIO->removeUpdateHandler(ioUpdateHandler);
  }

 private:
  update_handler_id_t ioUpdateHandler;
};

#endif