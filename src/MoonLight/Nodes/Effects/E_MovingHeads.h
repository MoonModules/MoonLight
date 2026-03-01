/**
    @title     MoonLight
    @file      E_MovingHeads.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

class Troy1ColorEffect : public Node {
 public:
  static const char* name() { return "Troy1 Color"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "🚨♫"; }

  bool audioReactive = true;

  void setup() override { addControl(audioReactive, "audioReactive", "checkbox"); }

  void loop() override {
    for (int x = 0; x < layer->size.x; x++) {  // loop over lights defined in layout
      if (audioReactive) {
        layer->setRGB(x, CRGB(sharedData.bands[NUM_GEQ_CHANNELS - 1], sharedData.bands[7], sharedData.bands[0]));
        layer->setBrightness(x, (sharedData.bands[0] > 200) ? 0 : layerP.lights.header.brightness);
      } else {
        layer->setRGB(x, ColorFromPalette(layerP.palette, beatsin8(10)));
        // layer->setBrightness(x, layerP.lights.header.brightness); // done automatically
      }
    }
  }
};

class Troy1MoveEffect : public Node {
 public:
  static const char* name() { return "Troy1 Move"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "🗼♫"; }

  // set default values here
  uint8_t bpm = 30;
  uint8_t pan = 175;
  uint8_t tilt = 90;
  uint8_t zoom = 20;
  uint8_t range = 20;
  uint8_t colorwheel = 0;
  uint8_t colorwheelbrightness = 255;  // 0-255, 0 = off, 255 = full brightness
  time_t cooldown = millis();

  bool autoMove = true;
  bool audioReactive = true;
  bool invert = true;
  // int closestColorIndex = -1;

  // std::vector<CRGB> colorwheelpalette = {
  //     CRGB(255, 255, 255),  // White
  //     CRGB(255, 0, 0),      // Red
  //     CRGB(0, 255, 0),      // Green
  //     CRGB(0, 0, 255),      // Blue
  //     CRGB(255, 255, 0),    // Yellow
  //     CRGB(0, 255, 255),    // Cyan
  //     CRGB(255, 165, 0),    // Orange
  //     CRGB(128, 0, 128)     // Purple
  // };

  void setup() override {
    addControl(bpm, "bpm", "slider");
    addControl(pan, "pan", "slider");
    addControl(tilt, "tilt", "slider");
    addControl(zoom, "zoom", "slider");
    addControl(colorwheel, "colorwheel", "slider", 0, 7);                // 0-7 for 8 colors in the colorwheel
    addControl(colorwheelbrightness, "colorwheelbrightness", "slider");  // 0-7 for 8 colors in the colorwheel
    addControl(autoMove, "autoMove", "checkbox");
    addControl(range, "range", "slider", 0, 127);
    addControl(audioReactive, "audioReactive", "checkbox");
    addControl(invert, "invert", "checkbox");
  }

  // Function to compute Euclidean distance between two colors
  // double colorDistance(const CRGB& c1, const CRGB& c2) { return std::sqrt(std::pow(c1.r - c2.r, 2) + std::pow(c1.g - c2.g, 2) + std::pow(c1.b - c2.b, 2)); }

  // Function to find the index of the closest color
  // int findClosestColorWheelIndex(const CRGB& inputColor, const std::vector<CRGB>& palette) {
  //   int closestIndex = 0;
  //   double minDistance = colorDistance(inputColor, palette[0]);

  //   for (size_t i = 1; i < palette.size(); ++i) {
  //     double distance = colorDistance(inputColor, palette[i]);
  //     if (distance < minDistance) {
  //       minDistance = distance;
  //       closestIndex = static_cast<int>(i);
  //     }
  //   }

  //   return closestIndex;
  // }

  void loop() override {
    for (int x = 0; x < layer->size.x; x++) {  // loop over lights defined in layout
      if (audioReactive) {
        if (sharedData.bands[2] > 200 && cooldown + 3000 < millis()) {  // cooldown for 3 seconds
          cooldown = millis();
          colorwheel = random8(8) * 5;  // random colorwheel index and convert to 0-35 range
        }
        layer->setGobo(x, colorwheel);
        layer->setBrightness2(x, (sharedData.bands[0] > 200) ? sharedData.bands[0] : 0);  // Use the first band for brightness
        layer->setZoom(x, (sharedData.bands[0] > 200) ? 0 : 128);
        uint8_t mypan = beatsin8(bpm, pan - range, pan + range, 0, sharedData.bands[0] / 2);
        uint8_t mytilt = beatsin8(bpm, tilt - range, tilt + range, 0, sharedData.bands[0] / 2);
        if (invert && x % 2 == 0) {
          mypan = 255 - mypan;    // invert pan
          mytilt = 255 - mytilt;  // invert tilt
        }
        layer->setPan(x, mypan);
        layer->setTilt(x, mytilt);
      } else {
        layer->setGobo(x, colorwheel);
        layer->setBrightness2(x, colorwheelbrightness);                                                                   // layerP.lights.header.brightness);
        layer->setPan(x, autoMove ? beatsin8(bpm, pan - range, pan + range, 0, (invert && x % 2 == 0) ? 128 : 0) : pan);  // if automove, pan the light over a range
        layer->setTilt(x, autoMove ? beatsin8(bpm, tilt - range, tilt + range, 0, (invert && x % 2 == 0) ? 128 : 0) : tilt);
        // layer->setBrightness(x, layerP.lights.header.brightness); // done automatically
      }
    }
  }
};

class Troy2ColorEffect : public Node {
 public:
  static const char* name() { return "Troy2 Color"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "🚨♫"; }

  uint8_t cutin = 200;

  bool audioReactive = true;

  void setup() override {
    addControl(cutin, "cutin", "slider");
    addControl(audioReactive, "audioReactive", "checkbox");
  }

  void loop() override {
    for (int x = 0; x < layer->size.x; x++) {  // loop over lights defined in layout
      if (audioReactive) {
        layer->setRGB(x, CRGB(sharedData.bands[NUM_GEQ_CHANNELS - 1] > cutin ? sharedData.bands[NUM_GEQ_CHANNELS - 1] : 0, sharedData.bands[7] > cutin ? sharedData.bands[7] : 0, sharedData.bands[0]));
        layer->setRGB1(x, CRGB(sharedData.bands[NUM_GEQ_CHANNELS - 1], sharedData.bands[7] > cutin ? sharedData.bands[7] : 0, sharedData.bands[0] > cutin ? sharedData.bands[0] : 0));
        layer->setRGB2(x, CRGB(sharedData.bands[NUM_GEQ_CHANNELS - 1] > cutin ? sharedData.bands[NUM_GEQ_CHANNELS - 1] : 0, sharedData.bands[7], sharedData.bands[0] > cutin ? sharedData.bands[0] : 0));
        layer->setRGB3(x, CRGB(sharedData.bands[NUM_GEQ_CHANNELS - 1] > cutin ? ::map(sharedData.bands[NUM_GEQ_CHANNELS - 1], cutin - 1, 255, 0, 255) : 0, sharedData.bands[7] > cutin ? ::map(sharedData.bands[7], cutin - 1, 255, 0, 255) : 0, sharedData.bands[0] > cutin ? ::map(sharedData.bands[0], cutin - 1, 255, 0, 255) : 0));
        // layer->setZoom(x, (sharedData.bands[0]>cutin)?255:0);
        if (sharedData.bands[0] + sharedData.bands[7] + sharedData.bands[NUM_GEQ_CHANNELS - 1] > 1) {
          layer->setBrightness(x, 255);
        } else {
          layer->setBrightness(x, 0);
        }
      } else {
        layer->setRGB(x, ColorFromPalette(layerP.palette, beatsin8(10)));
        // layer->setBrightness(x, layerP.lights.header.brightness); // done automatically
      }
    }
  }
};

class Troy2MoveEffect : public Node {
 public:
  static const char* name() { return "Troy2 Move"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "🗼♫"; }

  uint8_t bpm = 30;
  uint8_t pan = 175;
  uint8_t tilt = 90;
  uint8_t zoom = 20;
  uint8_t range = 20;
  uint8_t cutin = 200;
  time_t cooldown = millis();

  bool autoMove = true;
  bool audioReactive = true;
  bool invert = true;

  void setup() override {
    addControl(bpm, "bpm", "slider");
    addControl(pan, "pan", "slider");
    addControl(tilt, "tilt", "slider");
    addControl(zoom, "zoom", "slider");
    addControl(cutin, "cutin", "slider");
    addControl(autoMove, "autoMove", "checkbox");
    addControl(range, "range", "slider");
    addControl(audioReactive, "audioReactive", "checkbox");
    addControl(invert, "invert", "checkbox");
  }

  // Function to compute Euclidean distance between two colors
  // double colorDistance(const CRGB& c1, const CRGB& c2) { return std::sqrt(std::pow(c1.r - c2.r, 2) + std::pow(c1.g - c2.g, 2) + std::pow(c1.b - c2.b, 2)); }

  // // Function to find the index of the closest color
  // int findClosestColorWheelIndex(const CRGB& inputColor, const std::vector<CRGB>& palette) {
  //   int closestIndex = 0;
  //   double minDistance = colorDistance(inputColor, palette[0]);

  //   for (size_t i = 1; i < palette.size(); ++i) {
  //     double distance = colorDistance(inputColor, palette[i]);
  //     if (distance < minDistance) {
  //       minDistance = distance;
  //       closestIndex = static_cast<int>(i);
  //     }
  //   }
  //   return closestIndex;
  // }

  void loop() override {
    bool coolDownSet = false;
    for (int x = 0; x < layer->size.x; x++) {  // loop over lights defined in layout
      if (audioReactive) {
        if (sharedData.bands[0] > cutin) {
          layer->setZoom(x, 255);
          coolDownSet = true;
        } else if (cooldown + 5000 < millis()) {
          layer->setZoom(x, 0);
          coolDownSet = true;
        }
        // layer->setZoom(x, (sharedData.bands[0]>cutin)?255:0);
        uint8_t mypan = beatsin8(bpm, pan - range, pan + range, 0, sharedData.bands[0] / 2);
        uint8_t mytilt = beatsin8(bpm, tilt - range, tilt + range, 0, sharedData.bands[0] / 2);
        if (invert && x % 2 == 0) {
          mypan = 255 - mypan;    // invert pan
          mytilt = 255 - mytilt;  // invert tilt
        }
        if (sharedData.bands[0] + sharedData.bands[7] + sharedData.bands[NUM_GEQ_CHANNELS - 1] > 1) {
          layer->setPan(x, mypan);
          layer->setTilt(x, mytilt);
        } else {
        }
      } else {
        layer->setPan(x, autoMove ? beatsin8(bpm, pan - range, pan + range, 0, (invert && x % 2 == 0) ? 128 : 0) : pan);  // if automove, pan the light over a range
        layer->setTilt(x, autoMove ? beatsin8(bpm, tilt - range, tilt + range, 0, (invert && x % 2 == 0) ? 128 : 0) : tilt);
        // layer->setBrightness(x, layerP.lights.header.brightness); // done automatically
      }
    }
    if (coolDownSet) cooldown = millis();
  }
};

class FreqColorsEffect : public Node {
 public:
  static const char* name() { return "Freq Colors"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "🚨♫"; }

  uint8_t bpm = 30;
  bool audioReactive = true;

  void setup() override {
    addControl(bpm, "bpm", "slider");
    addControl(audioReactive, "audioReactive", "checkbox");
  }

  void loop() override {
    layer->fadeToBlackBy(50);

    for (int x = 0; x < layer->size.x; x++) {  // loop over lights defined in layout

      if (audioReactive) {
        uint8_t nrOfLights = layer->size.x * 3;
        uint8_t delta = 256 / nrOfLights;

        // set the 3 LED groups for each moving head light
        layer->setRGB({x, 0, 0}, ColorFromPalette(layerP.palette, (x)*delta, sharedData.bands[(x * 3) % 16]));
        layer->setRGB1({x, 0, 0}, ColorFromPalette(layerP.palette, (x + 3) * delta, sharedData.bands[(x * 3 + 1) % 16]));
        layer->setRGB2({x, 0, 0}, ColorFromPalette(layerP.palette, (x + 6) * delta, sharedData.bands[(x * 3 + 2) % 16]));
      } else {
        if (x == beatsin8(bpm, 0, layer->size.x - 1)) {                               // sinelon over moving heads
          layer->setRGB({x, 0, 0}, ColorFromPalette(layerP.palette, beatsin8(10)));   // colorwheel 10 times per minute
          layer->setRGB1({x, 0, 0}, ColorFromPalette(layerP.palette, beatsin8(10)));  // colorwheel 10 times per minute
          layer->setRGB2({x, 0, 0}, ColorFromPalette(layerP.palette, beatsin8(10)));  // colorwheel 10 times per minute
        }
      }

      // layer->setBrightness({x,0,0}, layerP.lights.header.brightness); // done automatically
    }
  }
};

class WowiMoveEffect : public Node {
 public:
  static const char* name() { return "Wowi Move"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "🗼♫"; }

  uint8_t bpm = 30;
  uint8_t pan = 175;
  uint8_t tilt = 90;
  uint8_t zoom = 20;
  uint8_t range = 20;
  ;
  bool autoMove = true;
  bool invert = true;

  void setup() override {
    addControl(bpm, "bpm", "slider");
    addControl(pan, "pan", "slider");
    addControl(tilt, "tilt", "slider");
    addControl(zoom, "zoom", "slider");
    addControl(autoMove, "autoMove", "checkbox");
    addControl(range, "range", "slider", 0, 127);
    addControl(invert, "invert", "checkbox");
  }

  void loop() override {
    for (int x = 0; x < layer->size.x; x++) {                                                                                   // loop over lights defined in layout
      layer->setPan({x, 0, 0}, autoMove ? beatsin8(bpm, pan - range, pan + range, 0, (invert && x % 2 == 0) ? 128 : 0) : pan);  // if automove, pan the light over a range
      layer->setTilt({x, 0, 0}, autoMove ? beatsin8(bpm, tilt - range, tilt + range, 0, (invert && x % 2 == 0) ? 128 : 0) : tilt);
      layer->setZoom({x, 0, 0}, zoom);
      // layer->setBrightness({x,0,0}, layerP.lights.header.brightness); // done automatically
    }
  }
};

class AmbientMoveEffect : public Node {
 public:
  static const char* name() { return "Ambient Move"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "🗼♫"; }

  uint8_t increaser = 255;
  uint8_t decreaser = 8;
  uint8_t tiltMin = 128;
  uint8_t tiltMax = 255;
  uint8_t panMin = 45;
  uint8_t panMax = 135;
  uint8_t panBPM = 5;
  bool invert = true;

  void setup() override {
    addControl(increaser, "increaser", "slider");
    addControl(decreaser, "decreaser", "slider");
    addControl(tiltMin, "tiltMin", "slider");
    addControl(tiltMax, "tiltMax", "slider");
    addControl(panMin, "panMin", "slider");
    addControl(panMax, "panMax", "slider");
    addControl(panBPM, "panBPM", "slider");
    addControl(invert, "invert", "checkbox");

    memset(bandSpeed, 0, sizeof(bandSpeed));
  }

  uint16_t bandSpeed[NUM_GEQ_CHANNELS];

  void loop() override {
    for (int x = 0; x < layer->size.x; x++) {  // x-axis (column)

      uint8_t band = ::map(x, 0, layer->size.x - 1, 2, NUM_GEQ_CHANNELS - 3);  // the frequency band applicable for the column, skip the lowest and the highest
      uint8_t volume = sharedData.bands[band];                                 // the volume for the frequency band

      bandSpeed[band] = constrain(bandSpeed[band] + (volume * increaser) - decreaser * 10, 0,
                                  UINT16_MAX);  // each band has a speed, increased by the volume and also decreased by decreaser. The decreaser should cause a delay

      uint8_t tilt = ::map(bandSpeed[band], 0, UINT16_MAX, tiltMin, tiltMax);  // the higher the band speed, the higher the tilt

      uint8_t pan = ::map(beatsin8((bandSpeed[band] > UINT16_MAX / 4) ? panBPM : 0, 0, 255, 0, (invert && x % 2 == 0) ? 128 : 0), 0, 255, panMin, panMax);  // expect a bit of volume before panning
      uint8_t tilt2 = ::map(beatsin8((bandSpeed[band] > UINT16_MAX / 4) ? panBPM : 0, 0, 255, 0, 64), 0, 255, tiltMin, tiltMax);                            // this is beatcos8, so pan and tilt draw a circle

      layer->setTilt(x, (tilt + tilt2) / 2);
      layer->setPan(x, pan);
    }
  }
};  // AmbientMoveEffect

#endif