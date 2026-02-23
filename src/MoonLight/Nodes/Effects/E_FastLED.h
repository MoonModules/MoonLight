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

  uint8_t fade = 70;

  void setup() { addControl(fade, "fade", "slider"); }

  uint8_t beatLevel = 0;
  float maxBass = 255;
  float maxMid = 255;
  float maxTreble = 255;
  float maxVocal = 255;

  void loop() override {
    layer->fadeToBlackBy(fade);

    if (sharedData.beat) beatLevel = 255;

    // EXT_LOGD(ML_TAG, "%f %f %d %f", sharedData.bassLevel, sharedData.trebleLevel, sharedData.beat, beatLevel, sharedData.vocalsActive ? sharedData.vocalConfidence : 0);

    uint8_t columnNr = 0;
    if (maxBass > 0.0f) layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.bassLevel / maxBass, CRGB::Red);
    columnNr++;
    if (maxMid > 0.0f) layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.midLevel / maxMid, CRGB::Orange);
    columnNr++;
    if (maxTreble > 0.0f) layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.trebleLevel / maxTreble, CRGB::Green);
    columnNr++;
    if (maxVocal > 0.0f) layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.vocalConfidence / maxVocal, CRGB::Blue);
    columnNr++;
    
    // beat
    layer->drawLine(columnNr, layer->size.y - 1, columnNr++, layer->size.y - 1 - layer->size.y * beatLevel / 255, CRGB::Purple);
    if (sharedData.beat) layer->setRGB(Coord3D(columnNr, layer->size.y - 1), CRGB::Purple);
    columnNr++;

    // percussion 
    if (sharedData.percussionType != UINT8_MAX) layer->setRGB(Coord3D(columnNr + sharedData.percussionType, layer->size.y - 1), CRGB::Cyan);
    columnNr+=3;

    // beat decay
    if (beatLevel && layer->size.y > 0) beatLevel -= MIN(255 / layer->size.y, beatLevel);
  }
};

#endif