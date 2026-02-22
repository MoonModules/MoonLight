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
  uint8_t beatBrightness = 0;

  void loop() override {
    if (sharedData.vocalsActive) {
      layer->fill_rainbow((hue += 8 * 32) >> 8, 7);  // hue back to uint8_t
    } else if (beatBrightness > 0) {
      if (sharedData.beat) beatBrightness = 255;
      CHSV color = CHSV(hue, 255, beatBrightness);
      layer->fill_solid(color);

      // Decay the brightness
      if (beatBrightness > 10) {
        beatBrightness = beatBrightness * 0.85f;  // Exponential decay
        hue += 32;                                // Shift color on each beat

      } else {
        beatBrightness = 0;
      }
    } else {  // random pixels
      layer->fadeToBlackBy(70);
      layer->setRGB(random16(layer->nrOfLights), ColorFromPalette(layerP.palette, random8()));
    }
  }
};

#endif