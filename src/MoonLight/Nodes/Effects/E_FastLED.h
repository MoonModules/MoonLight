/**
    @title     MoonLight
    @file      E_FastLED.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
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

    if (sharedData.fl_beat) beatLevel = 255;

    // EXT_LOGD(ML_TAG, "%f %f %d %f", sharedData.fl_bassLevel, sharedData.fl_trebleLevel, sharedData.fl_beat, beatLevel, sharedData.fl_vocalsActive ? sharedData.fl_vocalConfidence : 0);

    uint8_t columnNr = 0;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.fl_bassLevel, CRGB::Red);
    columnNr++;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.fl_midLevel, CRGB::Orange);
    columnNr++;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.fl_trebleLevel, CRGB::Green);
    columnNr++;

    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.volume, CRGB::Yellow);
    columnNr++;
    // Normalize BPM to 0-1 range (assuming typical range 60-200 BPM)
    float normalizedBpm = constrain(sharedData.fl_bpm, 60.0f, 200.0f);
    normalizedBpm = (normalizedBpm - 60.0f) / 140.0f;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * normalizedBpm, CRGB::Yellow);
    columnNr++;

    // vocal
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.fl_vocalConfidence, CRGB::Purple);
    columnNr++;

    // beat
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.fl_beatConfidence, CRGB::Blue);
    columnNr++;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * beatLevel / 255.0f, CRGB::Blue);
    columnNr++;
    if (sharedData.fl_beat) layer->setRGB(Coord3D(columnNr, layer->size.y - 1), CRGB::Blue);
    columnNr++;

    // percussion
    if (sharedData.fl_hihat) layer->setRGB(Coord3D(columnNr, layer->size.y - 1), CRGB::Cyan);
    columnNr++;
    if (sharedData.fl_kick) layer->setRGB(Coord3D(columnNr, layer->size.y - 1), CRGB::Cyan);
    columnNr++;
    if (sharedData.fl_snare) layer->setRGB(Coord3D(columnNr, layer->size.y - 1), CRGB::Cyan);
    columnNr++;
    if (sharedData.fl_tom) layer->setRGB(Coord3D(columnNr, layer->size.y - 1), CRGB::Cyan);

    // beat decay
    if (beatLevel && layer->size.y > 0) beatLevel -= MIN(255 / layer->size.y, beatLevel);
  }
};

#endif