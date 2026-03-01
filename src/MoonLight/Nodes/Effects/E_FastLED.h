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
  bool usePalette = false;

  void setup() {
    addControl(speed, "speed", "slider", 0, 32);
    addControl(deltaHue, "deltaHue", "slider", 0, 32);
    addControl(chanceOfGlitter, "chanceOfGlitter", "slider", 0, 100);
    addControl(usePalette, "usePalette", "checkbox");
  }

  uint16_t hue = 0;

  void loop() override {
    if (usePalette) {
      uint8_t paletteIndex = hue >> 8;
      for (int i = 0; i < layer->nrOfLights; ++i) {
        layer->setRGB(i, ColorFromPalette(layerP.palette, paletteIndex));
        paletteIndex += deltaHue;
      }
    } else {
      layer->fill_rainbow(hue >> 8, deltaHue);  // hue back to uint8_t
    }
    hue += speed * 32;

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

  uint8_t fade = 70;

  void setup() { addControl(fade, "fade", "slider"); }

  uint8_t beatLevel = 0;

  void loop() override {
    layer->fadeToBlackBy(fade);

    if (sharedData.beat) beatLevel = 255;

    // EXT_LOGD(ML_TAG, "%f %f %d %f", sharedData.bassLevel, sharedData.trebleLevel, sharedData.beat, beatLevel, sharedData.vocalsActive ? sharedData.vocalConfidence : 0);

    uint8_t columnNr = 0;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.bassLevel, CRGB::Red);
    columnNr++;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.midLevel, CRGB::Orange);
    columnNr++;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.trebleLevel, CRGB::Green);
    columnNr++;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.vocalConfidence, CRGB::Blue);
    columnNr++;

    // beat
    layer->drawLine(columnNr, layer->size.y - 1, columnNr++, layer->size.y - 1 - layer->size.y * beatLevel / 255, CRGB::Purple);
    if (sharedData.beat) layer->setRGB(Coord3D(columnNr, layer->size.y - 1), CRGB::Purple);
    columnNr++;

    // percussion
    if (sharedData.percussionType != UINT8_MAX) layer->setRGB(Coord3D(columnNr + sharedData.percussionType, layer->size.y - 1), CRGB::Cyan);
    columnNr += 3;

    // beat decay
    if (beatLevel && layer->size.y > 0) beatLevel -= MIN(255 / layer->size.y, beatLevel);
  }
};

#endif