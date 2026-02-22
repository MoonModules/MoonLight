/**
    @title     MoonLight
    @file      E_FastLED.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

class RainbowEffect : public Node {
 public:
  static const char* name() { return "Rainbow"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "⚡️"; }

  uint8_t speed = 8;  // default 8*32 = 256 / 256 = 1 = hue++
  uint8_t deltaHue = 7;
  uint8_t chanceOfGlitter = 80;

  void setup() {
    addControl(speed, "speed", "slider", 0, 32);
    addControl(deltaHue, "deltaHue", "slider", 0, 32);
    addControl(chanceOfGlitter, "chanceOfGlitter", "slider", 0, 100);
  }

  uint16_t hue = 0;

  void loop() override {
    layer->fill_rainbow((hue += speed * 32) >> 8, deltaHue);  // hue back to uint8_t
    if (chanceOfGlitter && random8() < chanceOfGlitter) {
      layer->setRGB(random16(layer->nrOfLights), CRGB::White);
    }
  }
};

class FLAudioEffect : public Node {
 public:
  static const char* name() { return "FLAudio"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "⚡️🎵"; }

  void setup() {}

  uint16_t hue = 0;
  uint8_t beatLevel = 0;
  float maxBass = 0;
  float maxTreble = 0;
  float maxVocal = 0;

  void loop() override {
    layer->fadeToBlackBy(70);

    // maxLevels
    if (sharedData.bassLevel > maxBass) maxBass = sharedData.bassLevel;
    if (sharedData.trebleLevel > maxTreble) maxTreble = sharedData.trebleLevel;
    if (sharedData.vocalConfidence > maxVocal) maxVocal = sharedData.vocalConfidence;

    if (sharedData.beat) beatLevel = 255;

    // EXT_LOGD(ML_TAG, "%f-%d %f-%d %d-%d %f-%d", sharedData.bassLevel, bassLevel, sharedData.trebleLevel, trebleLevel, sharedData.beat, beatLevel, sharedData.vocalsActive ? sharedData.vocalConfidence : 0, vocalsLevel);

    layer->drawLine(0, layer->size.y - 1, 0, layer->size.y - 1 - layer->size.y * sharedData.bassLevel / maxBass, CRGB::Blue);
    layer->drawLine(1, layer->size.y - 1, 1, layer->size.y - 1 - layer->size.y * sharedData.trebleLevel / maxTreble, CRGB::Orange);
    layer->drawLine(2, layer->size.y - 1, 2, layer->size.y - 1 - layer->size.y * sharedData.vocalConfidence / maxVocal, CRGB::Green);
    layer->drawLine(3, layer->size.y - 1, 3, layer->size.y - 1 - layer->size.y * beatLevel / 255, CRGB::Red);

    //correct if lower output
    if (maxBass > 0) maxBass -= 0.01;
    if (maxTreble > 0) maxTreble -= 0.01;
    if (maxVocal > 0) maxVocal -= 0.01;

    if (beatLevel) beatLevel--;
  }
};

#endif