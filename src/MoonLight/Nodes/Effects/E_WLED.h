/**
    @title     MoonLight
    @file      E_WLED.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

class BouncingBallsEffect : public Node {
 public:
  static const char* name() { return "Bouncing Balls"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥🎨🐙"; }

  uint8_t grav = 128;
  uint8_t numBalls = 8;

  void setup() override {
    addControl(grav, "grav", "slider");
    addControl(numBalls, "numBalls", "slider", 1, maxNumBalls);
  }

  Ball (*balls)[maxNumBalls] = nullptr;  //[maxColumns][maxNumBalls];
  uint16_t ballsSize = 0;

  ~BouncingBallsEffect() override { freeMB(balls); }

  void onSizeChanged(const Coord3D& prevSize) override {
    Ball(*newAlloc)[maxNumBalls] = reallocMB<Ball[maxNumBalls]>(balls, layer->size.x);

    if (newAlloc) {
      balls = newAlloc;
      ballsSize = layer->size.x;
    } else {
      EXT_LOGE(ML_TAG, "(re)allocate balls failed");
    }
  }

  void loop() override {
    if (!balls) return;

    layer->fadeToBlackBy(100);

    // non-chosen color is a random color
    const float gravity = -9.81f;  // standard value of gravity
    // const bool hasCol2 = SEGCOLOR(2);
    const unsigned long time = millis();

    // not necessary as effectControls is cleared at setup()
    //  if (call == 0) {
    //    for (size_t i = 0; i < maxNumBalls; i++) balls[i].lastBounceTime = time;
    //  }

    for (int x = 0; x < MIN(layer->size.x, ballsSize); x++) {
      for (size_t i = 0; i < MIN(numBalls, maxNumBalls); i++) {
        float timeSinceLastBounce = (time - balls[x][i].lastBounceTime) / ((255 - grav) / 64 + 1);
        float timeSec = timeSinceLastBounce / 1000.0f;
        balls[x][i].height = (0.5f * gravity * timeSec + balls[x][i].impactVelocity) * timeSec;  // avoid use pow(x, 2) - its extremely slow !

        if (balls[x][i].height <= 0.0f) {
          balls[x][i].height = 0.0f;
          // damping for better effect using multiple balls
          float dampening = 0.9f - float(i) / float(numBalls * numBalls);  // avoid use pow(x, 2) - its extremely slow !
          balls[x][i].impactVelocity = dampening * balls[x][i].impactVelocity;
          balls[x][i].lastBounceTime = time;

          if (balls[x][i].impactVelocity < 0.015f) {
            float impactVelocityStart = sqrtf(-2.0f * gravity) * random8(5, 11) / 10.0f;  // randomize impact velocity
            balls[x][i].impactVelocity = impactVelocityStart;
          }
        } else if (balls[x][i].height > 1.0f) {
          continue;  // do not draw OOB ball
        }

        // uint32_t color = SEGCOLOR(0);
        // if (layerP.palette) {
        //   color = layer->color_wheel(i*(256/MAX(numBalls, 8)));
        // }
        // else if (hasCol2) {
        //   color = SEGCOLOR(i % NUM_COLORS);
        // }

        uint8_t pos = layer->size.y - 1 - roundf(balls[x][i].height * (layer->size.y - 1));  // balls go up

        CRGB color = ColorFromPalette(layerP.palette, i * (256 / max(numBalls, (uint8_t)8)));  // error: no matching function for call to 'max(uint8_t&, int)'

        layer->setRGB(Coord3D(x, pos), color);
      }  // balls      layer->fill_solid(CRGB::White);
    }
  }
};

  // original version from SR 0.13, with some enhancements by @softhack007
  // Blurz. By Andrew Tuline.
  // Hint: Looks best with segment brightness set to max (use global brightness to reduce brightness)
  // even with 1D effect we have to take logic for 2D segments for allocation as fill_solid() fills whole segment
  #define MAX_FREQ_LOG10 4.04238f  // log10(MAX_FREQUENCY)
class BlurzEffect : public Node {
 public:
  static const char* name() { return "Blurz"; }
  static uint8_t dim() { return _3D; }  // test...
  static const char* tags() { return "🔥🎵🎨☾"; }

  // static const char _data_FX_MODE_BLURZ[] PROGMEM = "Blurz Plus ☾@Fade rate,Blur,,,,FreqMap ☾,GEQ Scanner ☾,;!,Color mix;!;01f;sx=48,ix=127,m12=7,si=0"; // Pinwheel, Beatsin

  uint16_t aux0 = 0;
  uint16_t aux1 = 65535;  // last pixel postion. 65535 = none
  CRGB step = {0, 0, 0};  // last pixel color
  uint32_t call = 0;      // last pixel color

  uint8_t fadeRate = 48;
  uint8_t blur = 127;
  bool freqMap;
  bool geqScanner;

  void setup() override {
    addControl(fadeRate, "fadeRate", "slider", 1, 255);
    addControl(blur, "blur", "slider", 1, 255);
    addControl(freqMap, "freqMap", "checkbox");
    addControl(geqScanner, "geqScanner", "checkbox");

    layer->fadeToBlackBy(255);
  }

  void loop() override {
    random16_add_entropy(esp_random() & 0xFFFF);  // improves randonmess

    int fadeoutDelay = (256 - fadeRate) / 24;
    if ((fadeoutDelay <= 1) || ((call % fadeoutDelay) == 0))
      layer->fadeToBlackBy(fadeRate);
    else {
      layer->blur2d(8 + blur / 8 + fadeoutDelay * 4);
    }
    if ((aux1 < layer->size.x * layer->size.y * layer->size.z) && (sharedData.volume > 1.0f)) layer->setRGB(aux1, step);  // "repaint" last pixel after blur

    unsigned freqBand = aux0 % 16;
    uint16_t segLoc = random16(layer->size.x * layer->size.y * layer->size.z);

    if (freqMap) {  // FreqMap mode : blob location by major frequency
      int freqLocn;
      unsigned maxLen = (geqScanner) ? MAX(1, layer->size.x * layer->size.y * layer->size.z - 16) : layer->size.x * layer->size.y * layer->size.z;  // usable segment length - leave 16 pixels when embedding "GEQ scan"
      freqLocn = roundf((log10f((float)sharedData.majorPeak) - 1.78f) * float(maxLen) / (MAX_FREQ_LOG10 - 1.78f));                                  // log10 frequency range is from 1.78 to 3.71. Let's scale to layer->size.x * layer->size.y * layer->size.z. // WLEDMM proper rounding
      if (freqLocn < 1) freqLocn = 0;                                                                                                               // avoid underflow
      segLoc = (geqScanner) ? freqLocn + freqBand : freqLocn;
    } else if (geqScanner) {  // GEQ Scanner mode: blob location is defined by frequency band + random offset
      float bandWidth = float(layer->size.x * layer->size.y * layer->size.z) / 16.0f;
      int bandStart = roundf(bandWidth * freqBand);
      segLoc = bandStart + random16(max(1, int(bandWidth)));
    }
    segLoc = MAX(uint16_t(0), MIN(uint16_t(layer->size.x * layer->size.y * layer->size.z - 1), segLoc));  // fix overflows

    if (layer->size.x * layer->size.y * layer->size.z < 2) segLoc = 0;                                                       // WLEDMM just to be sure
    unsigned pixColor = (2 * sharedData.bands[freqBand] * 240) / max(1, layer->size.x * layer->size.y * layer->size.z - 1);  // WLEDMM avoid uint8 overflow, and preserve pixel parameters for redraw
    unsigned pixIntensity = MIN((unsigned)(2.0f * sharedData.bands[freqBand]), 255U);

    if (sharedData.volume > 1.0f) {
      layer->setRGB(segLoc, ColorFromPalette(layerP.palette, pixColor));
      step = layer->getRGB(segLoc);  // remember last color
      aux1 = segLoc;                 // remember last position

      layer->blur2d(blur);
      aux0++;
      aux0 %= 16;                                                         // make sure it doesn't cross 16
      layer->addRGB(segLoc, ColorFromPalette(layerP.palette, pixColor));  // repaint center pixel after blur
    } else
      layer->blur2d(blur);  // silence - just blur it again
    call++;
  }  // loop
};  // BlurzEffect

// DistortionWaves inspired by WLED, ldirko and blazoncek, https://editor.soulmatelights.com/gallery/1089-distorsion-waves
class DistortionWavesEffect : public Node {
 public:
  static const char* name() { return "Distortion Waves"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥🐙"; }

  uint8_t speed = 4;
  uint8_t scale = 4;

  void setup() override {
    addControl(speed, "speed", "slider", 0, 8);
    addControl(scale, "scale", "slider", 0, 8);
  }

  void loop() override {
    uint8_t w = 2;

    uint16_t a = millis() / 32;
    uint16_t a2 = a / 2;
    uint16_t a3 = a / 3;

    uint16_t cx = beatsin8(10 - speed, 0, layer->size.x - 1) * scale;
    uint16_t cy = beatsin8(12 - speed, 0, layer->size.y - 1) * scale;
    uint16_t cx1 = beatsin8(13 - speed, 0, layer->size.x - 1) * scale;
    uint16_t cy1 = beatsin8(15 - speed, 0, layer->size.y - 1) * scale;
    uint16_t cx2 = beatsin8(17 - speed, 0, layer->size.x - 1) * scale;
    uint16_t cy2 = beatsin8(14 - speed, 0, layer->size.y - 1) * scale;

    uint16_t xoffs = 0;
    Coord3D pos = {0, 0, 0};
    for (pos.x = 0; pos.x < layer->size.x; pos.x++) {
      xoffs += scale;
      uint16_t yoffs = 0;

      for (pos.y = 0; pos.y < layer->size.y; pos.y++) {
        yoffs += scale;

        uint8_t rdistort = cos8((cos8(((pos.x << 3) + a) & 255) + cos8(((pos.y << 3) - a2) & 255) + a3) & 255) >> 1;
        uint8_t gdistort = cos8((cos8(((pos.x << 3) - a2) & 255) + cos8(((pos.y << 3) + a3) & 255) + a + 32) & 255) >> 1;
        uint8_t bdistort = cos8((cos8(((pos.x << 3) + a3) & 255) + cos8(((pos.y << 3) - a) & 255) + a2 + 64) & 255) >> 1;

        uint8_t valueR = rdistort + w * (a - (((xoffs - cx) * (xoffs - cx) + (yoffs - cy) * (yoffs - cy)) >> 7));
        uint8_t valueG = gdistort + w * (a2 - (((xoffs - cx1) * (xoffs - cx1) + (yoffs - cy1) * (yoffs - cy1)) >> 7));
        uint8_t valueB = bdistort + w * (a3 - (((xoffs - cx2) * (xoffs - cx2) + (yoffs - cy2) * (yoffs - cy2)) >> 7));

        valueR = layerP.gamma8(cos8(valueR));
        valueG = layerP.gamma8(cos8(valueG));
        valueB = layerP.gamma8(cos8(valueB));

        layer->setRGB(pos, CRGB(valueR, valueG, valueB));
      }
    }
  }
};  // DistortionWaves

  #define MAX_FREQUENCY 11025  // sample frequency / 2 (as per Nyquist criterion)

class FreqMatrixEffect : public Node {
 public:
  static const char* name() { return "Freq Matrix"; }
  static uint8_t dim() { return _1D; }  // 2D/3D-ish?
  static const char* tags() { return "🔥♪🐙"; }

  uint8_t speed = 255;
  uint8_t fx = 128;
  uint8_t lowBin = 0;   // 18;
  uint8_t highBin = 0;  // 48;
  uint8_t sensitivity10 = 30;
  bool audioSpeed = false;  // 💫

  void setup() override {
    addControl(speed, "speed", "slider");
    addControl(fx, "soundFX", "slider");
    addControl(lowBin, "lowBin", "slider");
    addControl(highBin, "highBin", "slider");
    addControl(sensitivity10, "sensitivity", "slider", 10, 100);
    addControl(audioSpeed, "audioSpeed", "checkbox");
  }

  uint8_t aux0;
  unsigned long bassMemory = 0;

  void loop() override {
    uint8_t secondHand;
    if (audioSpeed) {
      if (sharedData.bands[0] > 100) {
        if (bassMemory < 5000) bassMemory++;
      } else if (bassMemory)
        bassMemory--;  // 5000 frames cooldown

      secondHand = (micros() / (256 - speed * bassMemory / 5000) / 500 % 16);
    } else
      secondHand = (speed < 255) ? (micros() / (256 - speed) / 500 % 16) : 0;

    if ((speed > 254) || (aux0 != secondHand)) {  // WLEDMM allow run run at full speed
      aux0 = secondHand;

      // Pixel brightness (value) based on volume * sensitivity * intensity
      // uint_fast8_t sensitivity10 = ::map(sensitivity, 0, 31, 10, 100); // reduced resolution slider // WLEDMM sensitivity * 10, to avoid losing precision
      int pixVal = sharedData.volume * (float)fx * (float)sensitivity10 / 2560.0f;  // WLEDMM 2560 due to sensitivity * 10
      if (pixVal > 255) pixVal = 255;                                               // make a brightness from the last avg

      CRGB color = CRGB::Black;

      if (sharedData.majorPeak > MAX_FREQUENCY) sharedData.majorPeak = 1;
      // MajorPeak holds the freq. value which is most abundant in the last sample.
      // With our sampling rate of 10240Hz we have a usable freq range from roughtly 80Hz to 10240/2 Hz
      // we will treat everything with less than 65Hz as 0

      if ((sharedData.majorPeak > 80.0f) && (sharedData.volume > 0.25f)) {  // WLEDMM
        // Pixel color (hue) based on major frequency
        int upperLimit = 80 + 42 * highBin;
        int lowerLimit = 80 + 3 * lowBin;
        // uint8_t i =  lowerLimit!=upperLimit ? ::map(sharedData.majorPeak, lowerLimit, upperLimit, 0, 255) : sharedData.majorPeak;  // (original formula) may under/overflow - so we enforce uint8_t
        int freqMapped = lowerLimit != upperLimit ? ::map(sharedData.majorPeak, lowerLimit, upperLimit, 0, 255) : sharedData.majorPeak;  // WLEDMM preserve overflows
        uint8_t i = abs(freqMapped) & 0xFF;                                                                                              // WLEDMM we embrace overflow ;-) by "modulo 256"

        color = CHSV(i, 240, (uint8_t)pixVal);  // implicit conversion to RGB supplied by FastLED
      }

      // shift the pixels one pixel up
      layer->setRGB(0, color);
      for (int x = layer->size.x - 1; x >= 0; x--) {  // int as we count down!!!
        if (x != 0) color = layer->getRGB(x - 1);
        for (int y = 0; y < layer->size.y; y++)
          for (int z = 0; z < layer->size.z; z++) layer->setRGB(Coord3D(x, y, z), color);
      }
    }
  }
};

class GEQEffect : public Node {
 public:
  static const char* name() { return "GEQ"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥🎨♫🐙"; }

  uint8_t fadeOut = 255;
  uint8_t ripple = 128;
  bool colorBars = false;
  bool smoothBars = true;

  unsigned long step;

  void setup() override {
    Node::setup();

    addControl(fadeOut, "fadeOut", "slider");
    addControl(ripple, "ripple", "slider");
    addControl(colorBars, "colorBars", "checkbox");
    addControl(smoothBars, "smoothBars", "checkbox");

    step = millis();
  }

  uint16_t* previousBarHeight = nullptr;
  uint8_t previousBarHeightSize = 0;

  ~GEQEffect() { freeMB(previousBarHeight); }

  void onSizeChanged(const Coord3D& prevSize) override {
    uint16_t* newAlloc = reallocMB<uint16_t>(previousBarHeight, layer->size.x);
    if (newAlloc) {
      previousBarHeight = newAlloc;
      previousBarHeightSize = layer->size.x;
    } else {
      EXT_LOGE(ML_TAG, "(re)allocate previousBarHeight failed");
    }
  }

  void loop() override {
    const int NUM_BANDS = NUM_GEQ_CHANNELS;  // ::map(layer->custom1, 0, 255, 1, 16);

  #ifdef SR_DEBUG
    uint8_t samplePeak = *(uint8_t*)um_data->u_data[3];
  #endif

    bool rippleTime = false;
    if (millis() - step >= (256U - ripple)) {
      step = millis();
      rippleTime = true;
    }

    int fadeoutDelay = (256 - fadeOut) / 64;                                   // 256..1 -> 4..0
    size_t beat = ::map(beat16(fadeOut), 0, UINT16_MAX, 0, fadeoutDelay - 1);  // instead of call%fadeOutDelay

    if ((fadeoutDelay <= 1) || (beat == 0)) layer->fadeToBlackBy(fadeOut);

    uint16_t lastBandHeight = 0;  // WLEDMM: for smoothing out bars

    // evenly distribute see also Funky Plank/By ewowi/From AXI
    float bandwidth = (float)layer->size.x / NUM_BANDS;
    float remaining = bandwidth;
    uint8_t band = 0;
    Coord3D pos = {0, 0, 0};
    for (pos.x = 0; pos.x < layer->size.x; pos.x++) {
      // WLEDMM if not enough remaining
      if (remaining < 1) {
        band++;
        remaining += bandwidth;
      }  // increase remaining but keep the current remaining
      remaining--;  // consume remaining

      // EXT_LOGD(ML_TAG, "x %d b %d n %d w %f %f\n", x, band, NUM_BANDS, bandwidth, remaining);
      uint8_t frBand = ((NUM_BANDS < NUM_GEQ_CHANNELS) && (NUM_BANDS > 1)) ? ::map(band, 0, NUM_BANDS, 0, NUM_GEQ_CHANNELS) : band;  // always use full range. comment out this line to get the previous behaviour.
      // frBand = constrain(frBand, 0, NUM_GEQ_CHANNELS-1); //WLEDMM can never be out of bounds (I think...)
      uint16_t colorIndex = frBand * 17;               // WLEDMM 0.255
      uint16_t bandHeight = sharedData.bands[frBand];  // WLEDMM we use the original ffResult, to preserve accuracy

      // WLEDMM begin - smooth out bars
      if ((pos.x > 0) && (pos.x < (layer->size.x - 1)) && (smoothBars)) {
        // get height of next (right side) bar
        uint8_t nextband = (remaining < 1) ? band + 1 : band;
        nextband = constrain(nextband, 0, NUM_GEQ_CHANNELS - 1);                                                                       // just to be sure
        frBand = ((NUM_BANDS < NUM_GEQ_CHANNELS) && (NUM_BANDS > 1)) ? ::map(nextband, 0, NUM_BANDS, 0, NUM_GEQ_CHANNELS) : nextband;  // always use full range. comment out this line to get the previous behaviour.
        uint16_t nextBandHeight = sharedData.bands[frBand];
        // smooth Band height
        bandHeight = (7 * bandHeight + 3 * lastBandHeight + 3 * nextBandHeight) / 12;  // yeees, its 12 not 13 (10% amplification)
        bandHeight = constrain(bandHeight, 0, 255);                                    // remove potential over/underflows
        colorIndex = ::map(pos.x, 0, layer->size.x, 0, 256);                           // WLEDMM
      }
      lastBandHeight = bandHeight;  // remember BandHeight (left side) for next iteration
      uint16_t barHeight = ::map(bandHeight, 0, 255, 0,
                                 layer->size.y);  // Now we map bandHeight to barHeight. do not subtract -1 from layer->size.y here (should be 256 (ewowiHack) but then the top row is not displayed...)
      // WLEDMM end

      if (barHeight > layer->size.y) barHeight = layer->size.y;  // WLEDMM ::map() can "overshoot" due to rounding errors

      CRGB ledColor = CRGB::Black;

      for (pos.y = 0; pos.y < barHeight; pos.y++) {
        if (colorBars)  // color_vertical / color bars toggle
          colorIndex = ::map(pos.y, 0, layer->size.y, 0, 256);

        ledColor = ColorFromPalette(layerP.palette, (uint8_t)colorIndex);

        layer->setRGB(Coord3D(pos.x, layer->size.y - 1 - pos.y), ledColor);
      }

      if (previousBarHeight && pos.x < previousBarHeightSize) {
        if (barHeight > previousBarHeight[pos.x]) previousBarHeight[pos.x] = barHeight;                                  // drive the peak up
        if ((ripple > 0) && (previousBarHeight[pos.x] > 0) && (previousBarHeight[pos.x] < layer->size.y))                // WLEDMM avoid "overshooting" into other segments
          layer->setRGB(Coord3D(pos.x, layer->size.y - previousBarHeight[pos.x]), (CRGB)CHSV(millis() / 50, 255, 255));  // take millis()/50 color for the time being

        if (rippleTime && previousBarHeight[pos.x] > 0) previousBarHeight[pos.x]--;  // delay/ripple effect
      }
    }
  }
};

class LissajousEffect : public Node {
 public:
  static const char* name() { return "Lissajous"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥🎨🐙"; }

  uint8_t xFrequency = 64;
  uint8_t fadeRate = 128;
  uint8_t speed = 128;

  void setup() override {
    addControl(xFrequency, "xFrequency", "slider");
    addControl(fadeRate, "fadeRate", "slider");
    addControl(speed, "speed", "slider");
  }

  void loop() override {
    layer->fadeToBlackBy(fadeRate);
    uint_fast16_t phase = millis() * speed / 256;  // allow user to control rotation speed, speed between 0 and 255!
    Coord3D locn = {0, 0, 0};
    for (int i = 0; i < 256; i++) {
      // WLEDMM: stick to the original calculations of xlocn and ylocn
      locn.x = sin8(phase / 2 + (i * xFrequency) / 64);
      locn.y = cos8(phase / 2 + i * 2);
      locn.x = (layer->size.x < 2) ? 1 : (::map(2 * locn.x, 0, 511, 0, 2 * (layer->size.x - 1)) + 1) / 2;  // softhack007: "*2 +1" for proper rounding
      locn.y = (layer->size.y < 2) ? 1 : (::map(2 * locn.y, 0, 511, 0, 2 * (layer->size.y - 1)) + 1) / 2;  // "layer->size.y > 2" is needed to avoid div/0 in ::map()
      layer->setRGB(locn, ColorFromPalette(layerP.palette, millis() / 100 + i, 255));
    }
  }
};

class Noise2DEffect : public Node {
 public:
  static const char* name() { return "Noise2D"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥🎨🐙"; }

  uint8_t speed = 8;
  uint8_t scale = 64;

  void setup() override {
    addControl(speed, "speed", "slider", 0, 15);
    addControl(scale, "scale", "slider", 2, 255);
  }

  void loop() override {
    for (int y = 0; y < layer->size.y; y++) {
      for (int x = 0; x < layer->size.x; x++) {
        uint8_t pixelHue8 = inoise8(x * scale, y * scale, millis() / (16 - speed));
        layer->setRGB(Coord3D(x, y), ColorFromPalette(layerP.palette, pixelHue8));
      }
    }
  }
};  // Noise2D

class NoiseMeterEffect : public Node {
 public:
  static const char* name() { return "Noise Meter"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "♪🐙🎨"; }

  uint8_t fadeRate = 248;
  uint8_t width = 128;

  void setup() override {
    addControl(fadeRate, "fadeRate", "slider", 200, 254);
    addControl(width, "width", "slider", 0, 255);
  }

  uint8_t aux0;
  uint8_t aux1;

  void loop() override {
    layer->fadeToBlackBy(fadeRate);

    float tmpSound2 = sharedData.volumeRaw * 2.0 * (float)width / 255.0;
    int maxLen = ::map(tmpSound2, 0, 255, 0, layer->size.x);  // map to pixels availeable in current segment              // Still a bit too sensitive.
    // if (maxLen <0) maxLen = 0;
    // if (maxLen >layer->size.x) maxLen = layer->size.x;

    for (int i = 0; i < maxLen; i++) {                                                                                     // The louder the sound, the wider the soundbar. By Andrew Tuline.
      uint8_t index = inoise8(i * sharedData.volume + aux0, aux1 + i * sharedData.volume);                                 // Get a value from the noise function. I'm using both x and y axis.
      for (int y = 0; y < layer->size.y; y++)                                                                              // propagate to other dimensions
        for (int z = 0; z < layer->size.z; z++) layer->setRGB(Coord3D(i, y, z), ColorFromPalette(layerP.palette, index));  //, 255, PALETTE_SOLID_WRAP));
    }

    aux0 += beatsin8(5, 0, 10);
    aux1 += beatsin8(4, 0, 10);
  }
};  // NoiseMeter

/*
/  Pac-Man by Bob Loeffler with help from @dedehai and @blazoncek
*   speed slider is for speed.
*   intensity slider is for selecting the number of power dots.
*   custom1 slider is for selecting the LED where the ghosts will start blinking blue.
*   custom2 slider is for blurring the LEDs in the segment.
*   custom3 slider is for selecting the # of ghosts (between 2 and 8).
*   check1 is for displaying White Dots that PacMan eats.  Enabled will show white dots.  Disabled will not show any white dots (all leds will be black).
*   check2 is for Smear mode (enabled will smear/persist the LED colors, disabled will not).
*   check3 is for the Compact Dots mode of displaying white dots.  Enabled will show white dots in every LED.  Disabled will show black LEDs between the white dots.
*   aux1 is the main counter for timing.
*/
typedef struct PacManChars {
  signed pos;
  signed topPos;  // LED position of farthest PacMan has moved
  uint32_t color;
  bool direction;  // true = moving away from first LED
  bool blue;       // used for ghosts only
  bool eaten;      // used for power dots only
} pacmancharacters_t;

class PacManEffect : public Node {
 public:
  static const char* name() { return "PacMan"; }
  static uint8_t dim() { return _1D; }  // it is a 1D effect, todo: 2D
  static const char* tags() { return "🔥🐙"; }

  // static const char _data_FX_MODE_PACMAN[] PROGMEM = "PacMan@Speed,# of PowerDots,Blink distance,Blur,# of Ghosts,Dots,Smear,Compact;;!;1;m12=0,sx=192,ix=64,c1=64,c2=0,c3=12,o1=1,o2=0";
  uint8_t speed = 192;
  uint8_t numPowerDotsControl = 64;
  uint8_t blinkDistance = 64;
  uint8_t blur = 0;
  uint8_t numGhosts = 4;
  bool dots = true;
  bool smearMode = false;
  bool compact = false;

  void setup() override {
    addControl(speed, "speed", "slider");
    addControl(numPowerDotsControl, "#powerdots", "slider");
    addControl(blinkDistance, "blinkDistance", "slider", 20, 255);
    addControl(blur, "blur", "slider");
    addControl(numGhosts, "#ghosts", "slider", 2, 8);
    addControl(dots, "dots", "checkbox");
    addControl(smearMode, "smear", "checkbox");
    addControl(compact, "compact", "checkbox");
  }

  pacmancharacters_t* character = nullptr;
  uint8_t nrOfCharacters = 0;

  ~PacManEffect() { freeMB(character); }

  void onSizeChanged(const Coord3D& prevSize) override { initializePacMan(); }

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) {
    if (control["name"] == "#powerdots" || control["name"] == "#ghosts") {
      initializePacMan();
    }
  }

  uint8_t aux1TimingCounter;
  uint8_t numPowerDots;

  const unsigned ORANGEYELLOW = 0xFFCC00;
  const unsigned PURPLEISH = 0xB000B0;
  const unsigned ORANGEISH = 0xFF8800;
  const unsigned WHITEISH = 0x999999;
  const unsigned PACMAN = 0;  // PacMan is character[0]
  const uint32_t ghostColors[4] = {CRGB::Red, PURPLEISH, CRGB::Cyan, ORANGEISH};

  void initializePacMan() {
    numPowerDots = MIN(layer->nrOfLights / 10U, numPowerDotsControl);  // cap the max so packed state fits in 8 bits: ML: keep the nr visible in the UI

    EXT_LOGD(ML_TAG, "#l:%d #pd:%d #g:%d #pd:%d", layer->nrOfLights, numPowerDotsControl, numGhosts, numPowerDots);

    pacmancharacters_t* newAlloc = reallocMB<pacmancharacters_t>(character, numGhosts + numPowerDots + 1);  // +1 is the PacMan character
    if (newAlloc) {
      character = newAlloc;
      nrOfCharacters = numGhosts + numPowerDots + 1;
    } else {
      EXT_LOGE(ML_TAG, "(re)allocate character failed");  // keep old (if existed)
    }

    if (nrOfCharacters > 0) {
      character[PACMAN].color = CRGB::Yellow;
      character[PACMAN].pos = 0;
      character[PACMAN].topPos = 0;
      character[PACMAN].direction = true;
      character[PACMAN].blue = false;
    }

    // Initialize ghosts with alternating colors
    for (int i = 1; i <= MIN(numGhosts, nrOfCharacters); i++) {
      character[i].color = ghostColors[(i - 1) % 4];
      character[i].pos = -2 * (i + 1);
      character[i].direction = true;
      character[i].blue = false;
    }

    // Initialize power dots
    for (int i = 0; i < numPowerDots; i++) {
      if (i + numGhosts + 1 < nrOfCharacters) {
        character[i + numGhosts + 1].color = ORANGEYELLOW;
        character[i + numGhosts + 1].eaten = false;
      }
    }
    if (numGhosts + 1 < nrOfCharacters) {
      character[numGhosts + 1].pos = layer->nrOfLights - 1;  // Last power dot at end
    }
  }

  unsigned long step = 0;

  void loop() override {
    if (layer->nrOfLights > 16 + (2 * numGhosts) && character) {
      // Calculate when blue ghosts start blinking.
      // On first call (or after settings change), `topPos` is not known yet, so fall back to the full segment length in that case.
      int maxBlinkPos = character[PACMAN].topPos;  //(SEGENV.call == 0) ? (int)layer->nrOfLights - 1 :
      if (maxBlinkPos < 20) maxBlinkPos = 20;
      int startBlinkingGhostsLED = (layer->nrOfLights < 64) ? (int)layer->nrOfLights / 3 : map(blinkDistance, 20, 255, 20, maxBlinkPos);

      if (millis() > step) {
        step = millis();
        aux1TimingCounter++;
      }

      // Clear background if not in smear mode
      if (!smearMode) layer->fadeToBlackBy(255);

      // Draw white dots in front of PacMan if option selected
      if (dots) {
        int step = compact ? 1 : 2;  // Compact or spaced dots
        for (int i = layer->nrOfLights - 1; i > character[PACMAN].topPos; i -= step) {
          layer->setRGB(i, WHITEISH);
        }
      }

      // Update power dot positions dynamically
      uint32_t everyXLeds = (((uint32_t)layer->nrOfLights - 10U) << 8) / numPowerDots;  // Fixed-point spacing for power dots: use 32-bit math to avoid overflow on long segments.
      for (int i = 1; i < numPowerDots; i++) {
        character[i + numGhosts + 1].pos = 10 + ((i * everyXLeds) >> 8);
      }

      // Blink power dots every 10 ticks
      if (aux1TimingCounter % 10 == 0) {
        uint32_t dotColor = (character[numGhosts + 1].color == ORANGEYELLOW) ? CRGB::Black : ORANGEYELLOW;
        for (int i = 0; i < numPowerDots; i++) {
          character[i + numGhosts + 1].color = dotColor;
        }
      }

      // Blink blue ghosts when nearing start
      if (aux1TimingCounter % 15 == 0 && character[1].blue && character[PACMAN].pos <= startBlinkingGhostsLED) {
        uint32_t ghostColor = (character[1].color == CRGB::Blue) ? WHITEISH : CRGB::Blue;
        for (int i = 1; i <= numGhosts; i++) {
          character[i].color = ghostColor;
        }
      }

      // Draw uneaten power dots
      for (int i = 0; i < numPowerDots; i++) {
        if (!character[i + numGhosts + 1].eaten && (unsigned)character[i + numGhosts + 1].pos < layer->nrOfLights) {
          layer->setRGB(character[i + numGhosts + 1].pos, character[i + numGhosts + 1].color);
        }
      }

      // Check if PacMan ate a power dot
      for (int j = 0; j < numPowerDots; j++) {
        auto& dot = character[j + numGhosts + 1];
        if (character[PACMAN].pos == dot.pos && !dot.eaten) {
          // Reverse all characters - PacMan now chases ghosts
          for (int i = 0; i <= numGhosts; i++) {
            character[i].direction = false;
          }
          // Turn ghosts blue
          for (int i = 1; i <= numGhosts; i++) {
            character[i].color = CRGB::Blue;
            character[i].blue = true;
          }
          dot.eaten = true;
          break;  // only one power dot per frame
        }
      }

      // Reset when PacMan reaches start with blue ghosts
      if (character[1].blue && character[PACMAN].pos <= 0) {
        // Reverse direction back
        for (int i = 0; i <= numGhosts; i++) {
          character[i].direction = true;
        }
        // Reset ghost colors
        for (int i = 1; i <= numGhosts; i++) {
          character[i].color = ghostColors[(i - 1) % 4];
          character[i].blue = false;
        }
        // Reset power dots if last one was eaten
        if (character[numGhosts + 1].eaten) {
          for (int i = 0; i < numPowerDots; i++) {
            character[i + numGhosts + 1].eaten = false;
          }
          character[PACMAN].topPos = 0;  // set the top position of PacMan to LED 0 (beginning of the segment)
        }
      }

      // Update and draw characters based on speed setting
      bool updatePositions = (aux1TimingCounter % map(speed, 0, 255, 15, 1) == 0);

      // update positions of characters if it's time to do so
      if (updatePositions) {
        character[PACMAN].pos += character[PACMAN].direction ? 1 : -1;
        for (int i = 1; i <= numGhosts; i++) {
          character[i].pos += character[i].direction ? 1 : -1;
        }
      }

      // Draw PacMan
      if ((unsigned)character[PACMAN].pos < layer->nrOfLights) {
        layer->setRGB(character[PACMAN].pos, character[PACMAN].color);
      }

      // Draw ghosts
      for (int i = 1; i <= numGhosts; i++) {
        if ((unsigned)character[i].pos < layer->nrOfLights) {
          layer->setRGB(character[i].pos, character[i].color);
        }
      }

      // Track farthest position of PacMan
      if (character[PACMAN].topPos < character[PACMAN].pos) {
        character[PACMAN].topPos = character[PACMAN].pos;
      }

      layer->blur2d(blur >> 1);
    }
  }
};

/*
/  Ants (created by making modifications to the Rolling Balls code) - Bob Loeffler 2025
*   First slider is for the ants' speed.
*   Second slider is for the # of ants.
*   Third slider is for the Ants' size.
*   Fourth slider (custom2) is for blurring the LEDs in the segment.
*   Checkbox1 is for Gathering food (enabled if you want the ants to gather food, disabled if they are just walking).
*     We will switch directions when they get to the beginning or end of the segment when gathering food.
*     When gathering food, the Pass By option will automatically be enabled so they can drop off their food easier (and look for more food).
*   Checkbox2 is for Overlay mode (enabled is Overlay, disabled is no overlay)
*   Checkbox3 is for whether the ants will bump into each other (disabled) or just pass by each other (enabled)
*/
// Ant structure representing each ant's state
struct Ant {
  unsigned long lastBumpUpdate;  // the last time the ant bumped into another ant
  bool hasFood;
  float velocity;
  float position;  // (0.0 to 1.0 range)
};

constexpr unsigned MAX_ANTS = 32;
constexpr float MIN_COLLISION_TIME_MS = 2.0f;
constexpr float VELOCITY_MIN = 2.0f;
constexpr float VELOCITY_MAX = 10.0f;

// Helper function to get food pixel color based on ant and background colors
static uint32_t getFoodColor(CRGB antColor, CRGB backgroundColor) {
  if (antColor == CRGB::White) return (backgroundColor == CRGB::Yellow) ? CRGB::Gray : CRGB::Yellow;
  return (backgroundColor == CRGB::White) ? CRGB::Yellow : CRGB::White;
}

// Helper function to handle ant boundary wrapping or bouncing
static void handleBoundary(Ant& ant, float& position, bool gatherFood, bool atStart, unsigned long currentTime) {
  if (gatherFood) {
    // Bounce mode: reverse direction and update food status
    position = atStart ? 0.0f : 1.0f;
    ant.velocity = -ant.velocity;
    ant.lastBumpUpdate = currentTime;
    ant.position = position;
    ant.hasFood = atStart;  // Has food when leaving start, drops it at end
  } else {
    // Wrap mode: teleport to opposite end
    position = atStart ? 1.0f : 0.0f;
    ant.lastBumpUpdate = currentTime;
    ant.position = position;
  }
}

class AntEffect : public Node {
 public:
  static const char* name() { return "Ants"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "🔥🎨"; }

  uint8_t antSpeed = 192;
  uint8_t nrOfAnts = MAX_ANTS / 2;
  uint8_t antSizeControl = 8;
  uint8_t blur = 0;
  bool gatherFood = true;
  bool passByControl = true;
  // static const char _data_FX_MODE_ANTS[] PROGMEM = "Ants@Ant speed,# of ants,Ant size,Blur,,Gathering food,Overlay,Pass by;!,!,!;!;1;sx=192,ix=255,c1=32,c2=0,o1=1,o3=1";

  void setup() override {
    addControl(antSpeed, "antSpeed", "slider");
    addControl(nrOfAnts, "nrOfAnts", "slider", 1, MAX_ANTS);
    addControl(antSizeControl, "antSize", "slider", 1, 20);
    addControl(blur, "blur", "slider");
    addControl(gatherFood, "gatherFood", "checkbox");
    addControl(passByControl, "passBy", "checkbox");

    initAnts();
  }

  Ant ants[MAX_ANTS];

  // Helper function to calculate ant color
  CRGB getAntColor(int antIndex, int numAnts, bool usePalette) {
    // if (usePalette)
    return ColorFromPalette(layerP.palette, antIndex * 255 / numAnts);
    // Alternate between two colors for default palette
    // return (antIndex % 3 == 1) ? SEGCOLOR(0) : SEGCOLOR(2);
  }

  // Helper function to render a single ant pixel with food handling
  void renderAntPixel(int pixelIndex, int pixelOffset, int antSize, const Ant& ant, CRGB antColor, CRGB backgroundColor, bool gatherFood) {
    bool isMovingBackward = (ant.velocity < 0);
    bool isFoodPixel = gatherFood && ant.hasFood && ((isMovingBackward && pixelOffset == 0) || (!isMovingBackward && pixelOffset == antSize - 1));
    if (isFoodPixel) {
      layer->setRGB(pixelIndex, getFoodColor(antColor, backgroundColor));
    } else {
      layer->setRGB(pixelIndex, antColor);
    }
  }

  // Initialize ants on first call
  void initAnts() {
    int confusedAntIndex = random(0, numAnts);  // the first random ant to go backwards

    for (int i = 0; i < MAX_ANTS; i++) {
      ants[i].lastBumpUpdate = millis();

      // Random velocity
      float velocity = VELOCITY_MIN + (VELOCITY_MAX - VELOCITY_MIN) * random16(1000, 5000) / 5000.0f;
      // One random ant moves in opposite direction
      ants[i].velocity = (i == confusedAntIndex) ? -velocity : velocity;
      // Random starting position (0.0 to 1.0)
      ants[i].position = random16(0, 10000) / 10000.0f;
      // Ants don't have food yet
      ants[i].hasFood = false;
    }
  }

  unsigned numAnts;

  void loop() override {
    // Extract configuration from segment settings
    numAnts = MIN(1 + (layer->nrOfLights * nrOfAnts >> 12), MAX_ANTS);
    bool passBy = passByControl || gatherFood;  // global no‑collision when gathering food is enabled
    unsigned antSize = antSizeControl + (gatherFood ? 1 : 0);

    // Calculate time conversion factor based on speed slider
    float timeConversionFactor = float(scale8(8, 255 - antSpeed) + 1) * 20000.0f;

    layer->fadeToBlackBy(255);

    // Update and render each ant
    for (int i = 0; i < numAnts; i++) {
      float timeSinceLastUpdate = float(millis() - ants[i].lastBumpUpdate) / timeConversionFactor;
      float newPosition = ants[i].position + ants[i].velocity * timeSinceLastUpdate;

      // Reset ants that wandered too far off-track (e.g., after intensity change)
      if (newPosition < -0.5f || newPosition > 1.5f) {
        newPosition = ants[i].position = random16(0, 10000) / 10000.0f;
        ants[i].lastBumpUpdate = millis();
      }

      // Handle boundary conditions (bounce or wrap)
      if (newPosition <= 0.0f && ants[i].velocity < 0.0f) {
        handleBoundary(ants[i], newPosition, gatherFood, true, millis());
      } else if (newPosition >= 1.0f && ants[i].velocity > 0.0f) {
        handleBoundary(ants[i], newPosition, gatherFood, false, millis());
      }

      // Handle collisions between ants (if not passing by)
      if (!passBy) {
        for (int j = i + 1; j < numAnts; j++) {
          if (fabsf(ants[j].velocity - ants[i].velocity) < 0.001f) continue;  // Moving in same direction at same speed; avoids tiny denominators

          // Calculate collision time using physics
          float timeOffset = float(ants[j].lastBumpUpdate - ants[i].lastBumpUpdate);
          float collisionTime = (timeConversionFactor * (ants[i].position - ants[j].position) + ants[i].velocity * timeOffset) / (ants[j].velocity - ants[i].velocity);

          // Check if collision occurred in valid time window
          float timeSinceJ = float(millis() - ants[j].lastBumpUpdate);
          if (collisionTime > MIN_COLLISION_TIME_MS && collisionTime < timeSinceJ) {
            // Update positions to collision point
            float adjustedTime = (collisionTime + float(ants[j].lastBumpUpdate - ants[i].lastBumpUpdate)) / timeConversionFactor;
            ants[i].position += ants[i].velocity * adjustedTime;
            ants[j].position = ants[i].position;

            // Update collision time
            unsigned long collisionMoment = static_cast<unsigned long>(collisionTime + 0.5f) + ants[j].lastBumpUpdate;
            ants[i].lastBumpUpdate = collisionMoment;
            ants[j].lastBumpUpdate = collisionMoment;

            // Reverse the ant with greater speed magnitude
            if (fabsf(ants[i].velocity) > fabsf(ants[j].velocity)) {
              ants[i].velocity = -ants[i].velocity;
            } else {
              ants[j].velocity = -ants[j].velocity;
            }

            // Recalculate position after collision
            newPosition = ants[i].position + ants[i].velocity * (millis() - ants[i].lastBumpUpdate) / timeConversionFactor;
          }
        }
      }

      // Clamp position to valid range
      newPosition = constrain(newPosition, 0.0f, 1.0f);
      unsigned pixelPosition = roundf(newPosition * (layer->nrOfLights - 1));

      // Determine ant color
      CRGB antColor = getAntColor(i, numAnts, true);  // always use palette for now

      // Render ant pixels
      for (int pixelOffset = 0; pixelOffset < antSize; pixelOffset++) {
        unsigned currentPixel = pixelPosition + pixelOffset;
        if (currentPixel >= layer->nrOfLights) break;
        renderAntPixel(currentPixel, pixelOffset, antSize, ants[i], antColor, CRGB::Black, gatherFood);
      }

      // Update ant state
      ants[i].lastBumpUpdate = millis();
      ants[i].position = newPosition;
    }

    layer->blur1d(blur >> 1);
  }
};

/*
 * Tetris or Stacking (falling bricks) Effect
 * by Blaz Kristan (AKA blazoncek) (https://github.com/blazoncek, https://blaz.at/home)
 */
// 20 bytes
typedef struct Tetris {
  float pos;
  float speed;
  uint8_t col;     // color index
  uint16_t brick;  // brick size in pixels
  uint16_t stack;  // stack size in pixels
  uint32_t step;   // 2D-fication of SEGENV.step (state)
} tetris;

class TetrixEffect : public Node {
 public:
  static const char* name() { return "Tetrix"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥🐙🎨"; }  // use emojis see https://moonmodules.org/MoonLight/moonlight/overview/#emoji-coding, 🔥 for effect, 🎨 if palette used (recommended)

  uint8_t speedControl = 0;  // 1 beat per second
  uint8_t width = 0;
  bool oneColor = false;
  // static const char _data_FX_MODE_TETRIX[] PROGMEM = "Tetrix@!,Width,,,,One color;!,!;!;1.5d;sx=0,ix=0,pal=11,m12=1";  // WLEDMM 1.5d

  void setup() override {
    // controls will show in the UI
    // for different type of controls see other Nodes
    addControl(speedControl, "speed", "slider");
    addControl(width, "width", "slider");
    addControl(oneColor, "oneColor", "checkbox");
  }

  Tetris* drops = nullptr;
  uint16_t nrOfDrops = 0;

  void onSizeChanged(const Coord3D& prevSize) override {
    Tetris* newAlloc = reallocMB<Tetris>(drops, layer->size.x);
    if (newAlloc) {
      drops = newAlloc;
      nrOfDrops = layer->size.x;
    } else {
      EXT_LOGE(ML_TAG, "(re)allocate drops failed");  // keep old (if existed)
    }
    for (int i = 0; i < nrOfDrops; i++) {
      drops[i].stack = 0;               // reset brick stack size
      drops[i].step = millis() + 2000;  // start by fading out strip
      if (oneColor) drops[i].col = 0;   // use only one color from palette
    }
  }

  ~TetrixEffect() override { freeMB(drops); };

  void loop() override {
    if (!drops) return;

    // if (SEGENV.call == 0) layer->fill(SEGCOLOR(1));  // will fill entire segment (1D or 2D), then use drops[x].step = 0 below

    const uint8_t FRAMETIME = 1000 / 40;

    for (int x = 0; x < nrOfDrops; x++) {
      if (drops[x].step == 0) {  // init brick
        // speed calculation: a single brick should reach bottom of strip in X seconds
        // if the speed is set to 1 this should take 5s and at 255 it should take 0.25s
        // as this is dependant on layer->size.y it should be taken into account and the fact that effect runs every FRAMETIME s
        int speed = speedControl ? speedControl : random8(1, 255);
        speed = map(speed, 1, 255, 40000, 250);                                                    // time in ms taken for full (layer->size.y) drop
        drops[x].speed = float(layer->size.y * FRAMETIME) / float(speed);                          // set speed
        drops[x].pos = layer->size.y;                                                              // start at end of segment (no need to subtract 1)
        if (!oneColor) drops[x].col = random8(0, 15) << 4;                                         // limit color choices so there is enough HUE gap
        drops[x].step = 1;                                                                         // drop state (0 init, 1 forming, 2 falling)
        drops[x].brick = (width ? (width >> 5) + 1 : random8(1, 5)) * (1 + (layer->size.y >> 6));  // size of brick
      }

      if (drops[x].step == 1) {  // forming
        if (random8() >> 6) {    // random drop
          drops[x].step = 2;     // fall
        }
      }

      if (drops[x].step == 2) {               // falling
        if (drops[x].pos > drops[x].stack) {  // fall until top of stack
          drops[x].pos -= drops[x].speed;     // may add gravity as: speed += gravity
          if (drops[x].pos < drops[x].stack) drops[x].pos = drops[x].stack;
          for (int i = drops[x].pos; i < layer->size.y; i++) {
            CRGB col = i < drops[x].pos + drops[x].brick ? ColorFromPalette(layerP.palette, drops[x].col) : CRGB::Black;
            layer->setRGB(Coord3D(x, layer->size.y - 1 - i), col);
          }
        } else {                                                                 // we hit bottom
          drops[x].step = 0;                                                     // proceed with next brick, go back to init
          drops[x].stack += drops[x].brick;                                      // increase the stack size
          if (drops[x].stack >= layer->size.y) drops[x].step = millis() + 2000;  // fade out stack
        }
      }

      if (drops[x].step > 2) {  // fade strip
        drops[x].brick = 0;     // reset brick size (no more growing)
        if (drops[x].step > millis()) {
          // allow fading of virtual strip
          for (int i = 0; i < layer->size.y; i++) layer->blendColor(Coord3D(x, i), CRGB::Black, 25);  // 10% blend
        } else {
          drops[x].stack = 0;               // reset brick stack size
          drops[x].step = 0;                // proceed with next brick
          if (oneColor) drops[x].col += 8;  // gradually increase palette index
        }
      }
    }
  }
};

// each needs 19 bytes
// Spark type is used for popcorn, fireworks, and drip
struct Spark {
  float pos, posX;
  float vel, velX;
  uint16_t col;
  uint8_t colIndex;
};

  #define maxNumPopcorn 21  // max 21 on 16 segment ESP8266
  #define NUM_COLORS 3      /* number of colors per segment */

class PopCornEffect : public Node {
 public:
  static const char* name() { return "PopCorn"; }
  static uint8_t dim() { return _1D; }  // 2D-ish? check latest in WLED...
  static const char* tags() { return "♪🎨🐙"; }

  uint8_t speed = 128;
  uint8_t numPopcorn = maxNumPopcorn / 2;
  bool useaudio = false;

  void setup() override {
    addControl(speed, "speed", "slider");
    addControl(numPopcorn, "corns", "slider", 1, maxNumPopcorn);
    addControl(useaudio, "useaudio", "checkbox");
  }

  Spark popcorn[maxNumPopcorn];  // array

  void loop() override {
    // binding of loop persistent values (pointers)

    layer->fill_solid(CRGB::Black);

    float gravity = -0.0001f - (speed / 200000.0f);  // m/s/s
    gravity *= layer->size.x;

    if (numPopcorn == 0) numPopcorn = 1;

    for (int i = 0; i < numPopcorn; i++) {
      if (popcorn[i].pos >= 0.0f) {  // if kernel is active, update its position
        popcorn[i].pos += popcorn[i].vel;
        popcorn[i].vel += gravity;
      } else {                   // if kernel is inactive, randomly pop it
        bool doPopCorn = false;  // WLEDMM allows to inhibit new pops
        // WLEDMM begin
        if (useaudio) {
          if ((sharedData.volume > 1.0f)  // no pops in silence
                                          // &&((audioSync->sync.samplePeak > 0) || (audioSync->sync.volumeRaw > 128))  // try to pop at onsets (our peek detector still sucks)
              && (random8() < 4))         // stay somewhat random
            doPopCorn = true;
        } else {
          if (random8() < 2) doPopCorn = true;  // default POP!!!
        }

        if (doPopCorn) {  // POP!!!
          popcorn[i].pos = 0.01f;

          uint16_t peakHeight = 128 + random8(128);  // 0-255
          peakHeight = (peakHeight * (layer->size.x - 1)) >> 8;
          popcorn[i].vel = sqrtf(-2.0f * gravity * peakHeight);

          // if (layer->palette)
          // {
          popcorn[i].colIndex = random8();
          // } else {
          //   uint8_t  col = random8(0, NUM_COLORS);
          //   if (!SEGCOLOR(2) || !SEGCOLOR(col)) col = 0;
          //   popcorn[i].colIndex = col;
          // }
        }
      }
      if (popcorn[i].pos >= 0.0f) {  // draw now active popcorn (either active before or just popped)
        // uint32_t col = layer->color_wheel(popcorn[i].colIndex);
        // if (!layer->palette && popcorn[i].colIndex < NUM_COLORS) col = SEGCOLOR(popcorn[i].colIndex);
        uint16_t ledIndex = popcorn[i].pos;
        CRGB col = ColorFromPalette(layerP.palette, popcorn[i].colIndex * (256 / maxNumPopcorn));
        if (ledIndex < layer->size.x) {
          layer->setRGB(ledIndex, col);
          for (int y = 0; y < layer->size.y; y++)
            for (int z = 0; z < layer->size.z; z++) layer->setRGB(Coord3D(ledIndex, y, z), col);
        }
      }
    }
  }
};  // PopCorn

class WaverlyEffect : public Node {
 public:
  static const char* name() { return "Waverly"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥♪🎨🐙"; }

  uint8_t fadeRate = 128;
  uint8_t amplification = 30;
  bool noClouds = false;

  void setup() override {
    addControl(fadeRate, "fadeRate", "slider");
    addControl(amplification, "amplification", "slider");
    addControl(noClouds, "noClouds", "checkbox");

    // addControl(bool, "soundPressure", layer->effectData.write<bool>(false));
    // addControl(bool, "AGCDebug", layer->effectData.write<bool>(false));
  }

  void loop() override {
    // bool soundPressure = layer->effectData.read<bool>();
    // bool agcDebug = layer->effectData.read<bool>();

    layer->fadeToBlackBy(fadeRate);
    // netmindz: cannot find these in audio sync
    //  if (agcDebug && soundPressure) soundPressure = false;                 // only one of the two at any time
    //  if ((soundPressure) && (audioSync->sync.volumeSmth > 0.5f)) audioSync->sync.volumeSmth = audioSync->sync.soundPressure;    // show sound pressure instead of volume
    //  if (agcDebug) audioSync->sync.volumeSmth = 255.0 - audioSync->sync.agcSensitivity;                    // show AGC level instead of volume

    long t = millis() / 2;
    Coord3D pos = {0, 0, 0};  // initialize z otherwise wrong results
    for (pos.x = 0; pos.x < layer->size.x; pos.x++) {
      uint16_t thisVal = sharedData.volume * amplification * inoise8(pos.x * 45, t, t) / 4096;  // WLEDMM back to SR code
      uint16_t thisMax = MIN(map(thisVal, 0, 512, 0, layer->size.y), (long)layer->size.y);

      for (pos.y = 0; pos.y < thisMax; pos.y++) {
        CRGB color = ColorFromPalette(layerP.palette, ::map(pos.y, 0, thisMax, 250, 0));
        if (!noClouds) layer->addRGB(pos, color);
        layer->addRGB(Coord3D((layer->size.x - 1) - pos.x, (layer->size.y - 1) - pos.y), color);
      }
    }
    layer->blur2d(16);
  }
};  // Waverly

class BlackholeEffect : public Node {
 public:
  static const char* name() { return "Blackhole"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥🎨⏳🐙"; }

  uint8_t fadeRate = 128;    // speed
  uint8_t outerYfreq = 128;  // intensity
  uint8_t outerXfreq = 128;  // custom1
  uint8_t innerXfreq = 128;  // custom2
  uint8_t innerYfreq = 128;  // custom3
  uint8_t blur = 16;         // check3

  void setup() override {
    addControl(fadeRate, "fadeRate", "slider");
    addControl(outerYfreq, "outerYfreq", "slider");
    addControl(outerXfreq, "outerXfreq", "slider");
    addControl(innerXfreq, "innerXfreq", "slider");
    addControl(innerYfreq, "innerYfreq", "slider");
    addControl(blur, "blur", "slider");
  }

  void loop() override {
    const int cols = layer->size.x;
    const int rows = layer->size.y;
    int x, y;

    layer->fadeToBlackBy(16 + (fadeRate >> 3));  // create fading trails
    unsigned long t = millis() / 128;            // timebase
    // outer stars
    for (size_t i = 0; i < 8; i++) {
      x = beatsin8(outerXfreq >> 3, 0, cols - 1, 0, ((i % 2) ? 128 : 0) + t * i);
      y = beatsin8(outerYfreq >> 3, 0, rows - 1, 0, ((i % 2) ? 192 : 64) + t * i);
      layer->addRGB(Coord3D(x, y), ColorFromPalette(layerP.palette, i * 32));
    }
    // inner stars
    for (size_t i = 0; i < 4; i++) {
      x = beatsin8(innerXfreq >> 3, cols / 4, cols - 1 - cols / 4, 0, ((i % 2) ? 128 : 0) + t * i);
      y = beatsin8(innerYfreq >> 3, rows / 4, rows - 1 - rows / 4, 0, ((i % 2) ? 192 : 64) + t * i);
      layer->addRGB(Coord3D(x, y), ColorFromPalette(layerP.palette, 255 - i * 64));
    }
    // central white dot
    layer->setRGB(Coord3D(cols / 2, rows / 2), CRGB::White);
    // blur everything a bit
    if (blur) layer->blur2d(blur);
  }
};

// dna originally by by ldirko at https://pastebin.com/pCkkkzcs. Updated by Preyy. WLED conversion by Andrew Tuline.
class DNAEffect : public Node {
 public:
  static const char* name() { return "DNA"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🐙💫"; }

  uint8_t speed = 16;
  uint8_t blur = 128;
  uint8_t phases = 64;

  void setup() override {
    addControl(speed, "speed", "slider", 0, 32);
    addControl(blur, "blur", "slider");
    addControl(phases, "phases", "slider");
  }

  void loop() override {
    const int cols = layer->size.x;
    const int rows = layer->size.y;

    layer->fadeToBlackBy(64);

    // WLEDMM optimized to prevent holes at height > 32
    int lastY1 = -1;
    int lastY2 = -1;
    for (int i = 0; i < cols; i++) {
      // 256 is a complete phase
      // half a phase is dna is 128
      uint8_t phase;  // = cols * i / 8;
      // 32: 4 * i
      // 16: 8 * i
      phase = i * phases / cols;

      // phase = i * 2 / (cols+1) * phases;

      int posY1 = beatsin8(speed, 0, rows - 1, 0, phase);
      int posY2 = beatsin8(speed, 0, rows - 1, 0, phase + 128);
      if ((i == 0) || ((abs(lastY1 - posY1) < 2) && (abs(lastY2 - posY2) < 2))) {  // use original code when no holes
        layer->setRGB(Coord3D(i, posY1), ColorFromPalette(layerP.palette, i * 5 + millis() / 17, beatsin8(5, 55, 255, 0, i * 10)));
        layer->setRGB(Coord3D(i, posY2), ColorFromPalette(layerP.palette, i * 5 + 128 + millis() / 17, beatsin8(5, 55, 255, 0, i * 10 + 128)));
      } else {  // draw line to prevent holes
        layer->drawLine(i - 1, lastY1, i, posY1, ColorFromPalette(layerP.palette, i * 5 + millis() / 17, beatsin8(5, 55, 255, 0, i * 10)));
        layer->drawLine(i - 1, lastY2, i, posY2, ColorFromPalette(layerP.palette, i * 5 + 128 + millis() / 17, beatsin8(5, 55, 255, 0, i * 10 + 128)));
      }
      lastY1 = posY1;
      lastY2 = posY2;
    }
    layer->blur2d(blur);
  }
};  // DNA

// Octopus inspired by WLED, Stepko and Sutaburosu and blazoncek
// Idea from https://www.youtube.com/watch?v=HsA-6KIbgto&ab_channel=GreatScott%21 (https://editor.soulmatelights.com/gallery/671-octopus)
class OctopusEffect : public Node {
 public:
  static const char* name() { return "Octopus"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🐙"; }

  struct Map_t {
    uint8_t angle;
    uint8_t radius;
  };

  uint8_t speed = 16;
  Coord3D offset = {50, 50, 50};
  uint8_t legs = 4;
  bool radialWave = false;

  void setup() override {
    addControl(speed, "speed", "slider", 1, 32);
    addControl(offset, "offset", "coord3D", 0, 100, false);  // 0..100%
    addControl(legs, "legs", "slider", 1, 8);

    addControl(radialWave, "radialWave", "checkbox");
  }

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) {
    // add your custom onUpdate code here
    if (control["name"] == "offset") {
      if (rMap) setRMap();
    }
  }

  Coord3D prevLedSize;
  Map_t* rMap = nullptr;
  uint16_t rMapSize = 0;
  uint32_t step;

  ~OctopusEffect() { freeMB(rMap); }

  void setRMap() {
    const uint8_t C_X = layer->size.x / 2 + (offset.x - 50) * layer->size.x / 100;
    const uint8_t C_Y = layer->size.y / 2 + (offset.y - 50) * layer->size.y / 100;
    Coord3D pos = {0, 0, 0};
    const uint8_t mapp = 180 / max(layer->size.x, layer->size.y);
    for (pos.x = 0; pos.x < layer->size.x; pos.x++) {
      for (pos.y = 0; pos.y < layer->size.y; pos.y++) {
        uint16_t indexV = layer->XYZUnModified(pos);
        if (indexV < layer->size.x * layer->size.y) {                        // excluding UINT16_MAX from XY if out of bounds due to projection
          rMap[indexV].angle = 40.7436f * atan2f(pos.y - C_Y, pos.x - C_X);  // avoid 128*atan2()/PI
          rMap[indexV].radius = hypotf(pos.x - C_X, pos.y - C_Y) * mapp;     // thanks Sutaburosu
        }
      }
    }
  }

  void onSizeChanged(const Coord3D& prevSize) override {
    Map_t* newAlloc = reallocMB<Map_t>(rMap, layer->size.x * layer->size.y);
    if (newAlloc) {
      rMap = newAlloc;
      rMapSize = layer->size.x * layer->size.y;
      setRMap();
    } else {
      EXT_LOGE(ML_TAG, "(re)allocate rMap failed");
    }
  }

  void loop() override {
    if (rMap) {  // check if rMap allocation successful

      step = millis() * speed / 25;  // sys.now/25 = 40 per second. speed / 32: 1-4 range ? (1-8 ??)
      if (radialWave)
        step = 3 * step / 4;  // 7/6 = 1.16 for RadialWave mode
      else
        step = step / 2;  // 1/2 for Octopus mode

      Coord3D pos = {0, 0, 0};
      for (pos.x = 0; pos.x < layer->size.x; pos.x++) {
        for (pos.y = 0; pos.y < layer->size.y; pos.y++) {
          uint16_t indexV = layer->XYZUnModified(pos);
          if (indexV < rMapSize) {  // excluding UINT16_MAX from XY if out of bounds due to projection
            byte angle = rMap[indexV].angle;
            byte radius = rMap[indexV].radius;
            uint16_t intensity;
            if (radialWave)
              intensity = sin8(step + sin8(step - radius) + angle * legs);  // RadialWave
            else
              intensity = sin8(sin8((angle * 4 - radius) / 4 + step / 2) + radius - step + angle * legs);  // octopus
            intensity = intensity * intensity / 255;                                                       // add a bit of non-linearity for cleaner display
            layer->setRGB(pos, ColorFromPalette(layerP.palette, step / 2 - radius, intensity));
          }
        }
      }
    }  // if (layer->effectData.success())
  }

};  // Octopus

// Frizzles inspired by WLED, Stepko, Andrew Tuline, https://editor.soulmatelights.com/gallery/640-color-frizzles
class FrizzlesEffect : public Node {
 public:
  static const char* name() { return "Frizzles"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🐙"; }

  uint8_t bpm = 60;
  uint8_t intensity = 128;
  uint8_t blur = 128;

  void setup() override {
    addControl(bpm, "BPM", "slider");
    addControl(intensity, "intensity", "slider");
    addControl(blur, "blur", "slider");
  }

  void loop() override {
    layer->fadeToBlackBy(16);

    for (int i = 8; i > 0; i--) {
      Coord3D pos = {0, 0, 0};
      pos.x = beatsin8(bpm / 8 + i, 0, layer->size.x - 1);
      pos.y = beatsin8(intensity / 8 - i, 0, layer->size.y - 1);
      CRGB color = ColorFromPalette(layerP.palette, beatsin8(12, 0, 255));
      layer->setRGB(pos, color);
    }
    layer->blur2d(blur);
  }
};  // Frizzles

/*
 * Exploding fireworks effect
 * adapted from: http://www.anirama.com/1000leds/1d-fireworks/
 * adapted for 2D WLED by blazoncek (Blaz Kristan (AKA blazoncek))
 * simplified for StarLight by ewowi in Nov 24
 */
class FireworksEffect : public Node {
 public:
  static const char* name() { return "Fireworks"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🐙"; }

  uint8_t gravityControl = 128;
  uint8_t firingSide = 128;
  uint8_t numSparks = 128;

  void setup() override {
    layer->fadeToBlackBy(16);
    addControl(gravityControl, "gravity", "slider");
    addControl(firingSide, "firingSide", "slider");
    addControl(numSparks, "numSparks", "slider");
    // PROGMEM = "Fireworks 1D@Gravity,Firing side;!,!;!;12;pal=11,ix=128";
  }

  float dying_gravity;
  uint16_t aux0Flare;
  Spark sparks[255];
  Spark* flare = sparks;  // first spark is flare data

  void loop() override {
    const uint16_t cols = layer->size.x;
    const uint16_t rows = layer->size.y;

    // allocate segment data

    // if (dataSize != aux1) { //reset to flare if sparks were reallocated (it may be good idea to reset segment if bounds change)
    //   dying_gravity = 0.0f;
    //   (aux0Flare) = 0;
    //   aux1 = dataSize;
    // }

    layer->fadeToBlackBy(252);  // fade_out(252);

    float gravity = -0.0004f - (gravityControl / 800000.0f);  // m/s/s
    gravity *= rows;

    if (aux0Flare < 2) {     // FLARE
      if (aux0Flare == 0) {  // init flare
        flare->pos = 0;
        flare->posX = random16(2, cols - 3);
        uint16_t peakHeight = 75 + random8(180);  // 0-255
        peakHeight = (peakHeight * (rows - 1)) >> 8;
        flare->vel = sqrtf(-2.0f * gravity * peakHeight);
        flare->velX = (random8(9) - 4) / 32.f;
        flare->col = 255;  // brightness
        aux0Flare = 1;
      }

      // launch
      if (flare->vel > 12 * gravity) {
        // flare
        layer->setRGB(Coord3D(int(flare->posX), rows - uint16_t(flare->pos) - 1), CRGB(flare->col, flare->col, flare->col));
        flare->pos += flare->vel;
        flare->posX += flare->velX;
        flare->pos = constrain(flare->pos, 0, rows - 1);
        flare->posX = constrain(flare->posX, 0, cols - 1);
        flare->vel += gravity;
        flare->col -= 2;
      } else {
        aux0Flare = 2;  // ready to explode
      }
    } else if (aux0Flare < 4) {
      /*
       * Explode!
       *
       * Explosion happens where the flare ended.
       * Size is proportional to the height.
       */
      uint8_t nSparks = flare->pos + random8(4);
      // nSparks = std::max(nSparks, 4U);  // This is not a standard constrain; numSparks is not guaranteed to be at least 4
      nSparks = MIN(nSparks, numSparks);

      // initialize sparks
      if (aux0Flare == 2) {
        for (int i = 1; i < nSparks; i++) {
          sparks[i].pos = flare->pos;
          sparks[i].posX = flare->posX;
          sparks[i].vel = (float(random16(20001)) / 10000.0f) - 0.9f;  // from -0.9 to 1.1
          // sparks[i].vel *= rows<32 ? 0.5f : 1; // reduce velocity for smaller strips
          sparks[i].velX = (float(random16(20001)) / 10000.0f) - 0.9f;  // from -0.9 to 1.1
          // sparks[i].velX = (float(random16(10001)) / 10000.0f) - 0.5f; // from -0.5 to 0.5
          sparks[i].col = 345;  // abs(sparks[i].vel * 750.0); // set colors before scaling velocity to keep them bright
          // sparks[i].col = constrain(sparks[i].col, 0, 345);
          sparks[i].colIndex = random8();
          sparks[i].vel *= flare->pos / rows;    // proportional to height
          sparks[i].velX *= flare->posX / cols;  // proportional to width
          sparks[i].vel *= -gravity * 50;
        }
        // sparks[1].col = 345; // this will be our known spark
        dying_gravity = gravity / 2;
        aux0Flare = 3;
      }

      if (sparks[1].col > 4) {  //&& sparks[1].pos > 0) { // as long as our known spark is lit, work with all the sparks
        for (int i = 1; i < nSparks; i++) {
          sparks[i].pos += sparks[i].vel;
          sparks[i].posX += sparks[i].velX;
          sparks[i].vel += dying_gravity;
          sparks[i].velX += dying_gravity;
          if (sparks[i].col > 3) sparks[i].col -= 4;

          if (sparks[i].pos > 0 && sparks[i].pos < rows) {
            if (!(sparks[i].posX >= 0 && sparks[i].posX < cols)) continue;
            uint16_t prog = sparks[i].col;
            CRGB spColor = ColorFromPalette(layerP.palette, sparks[i].colIndex);
            CRGB c = CRGB::Black;  // HeatColor(sparks[i].col);
            if (prog > 300) {      // fade from white to spark color
              c = CRGB(blend(spColor, CRGB::White, (prog - 300) * 5));
            } else if (prog > 45) {  // fade from spark color to black
              c = CRGB(blend(CRGB::Black, spColor, prog - 45));
              uint8_t cooling = (300 - prog) >> 5;
              c.g = qsub8(c.g, cooling);
              c.b = qsub8(c.b, cooling * 2);
            }
            layer->setRGB(Coord3D(sparks[i].posX, rows - sparks[i].pos - 1), c);
          }
        }
        layer->blur2d(16);
        dying_gravity *= .8f;  // as sparks burn out they fall slower
      } else {
        aux0Flare = 6 + random8(10);  // wait for this many frames
      }
    } else {
      aux0Flare--;
      if (aux0Flare < 4) {
        aux0Flare = 0;  // back to flare
      }
    }
  }

};  // FireworksEffect

class FunkyPlankEffect : public Node {
 public:
  static const char* name() { return "Funky Plank"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "♫🐙💫"; }

  uint8_t speed = 255;
  uint8_t bands = NUM_GEQ_CHANNELS;

  void setup() override {
    layer->fill_solid(CRGB::Black);
    addControl(speed, "speed", "slider");
    addControl(bands, "bands", "slider", 1, NUM_GEQ_CHANNELS);
  }

  uint8_t aux0;

  void loop() override {
    uint8_t secondHand = (speed < 255) ? (micros() / (256 - speed) / 500 % 16) : 0;
    if ((speed > 254) || (aux0 != secondHand)) {  // WLEDMM allow run run at full speed
      aux0 = secondHand;

      // evenly distribute see also GEQ/By ewowi/From AXI
      float bandwidth = (float)layer->size.x / bands;
      float remaining = bandwidth;
      uint8_t band = 0;
      for (int posx = 0; posx < layer->size.x; posx++) {
        if (remaining < 1) {
          band++;
          remaining += bandwidth;
        }  // increase remaining but keep the current remaining
        remaining--;  // consume remaining

        int hue = sharedData.bands[map(band, 0, bands - 1, 0, NUM_GEQ_CHANNELS - 1)];
        int v = ::map(hue, 0, 255, 10, 255);
        layer->setRGB(Coord3D(posx, 0), CHSV(hue, 255, v));
      }

      // drip down:
      for (int i = (layer->size.y - 1); i > 0; i--) {
        for (int j = (layer->size.x - 1); j >= 0; j--) {
          layer->setRGB(Coord3D(j, i), layer->getRGB(Coord3D(j, i - 1)));
        }
      }
    }
  }
};  // FunkyPlank

// Best of both worlds from Palette and Spot effects. By Aircoookie
class FlowEffect : public Node {
 public:
  static const char* name() { return "Flow"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "🐙"; }  // 🐙 means wled origin

  uint8_t speed = 128;
  uint8_t zonesControl = 128;

  void setup() override {
    addControl(speed, "speed", "slider");
    addControl(zonesControl, "zones", "slider");
  }

  void loop() override {
    uint16_t counter = 0;
    if (speed != 0) {
      counter = millis() * ((speed >> 2) + 1);
      counter = counter >> 8;
    }

    uint16_t maxZones = layer->size.y / 6;  // only looks good if each zone has at least 6 LEDs
    uint16_t zones = (zonesControl * maxZones) >> 8;
    if (zones & 0x01) zones++;  // zones must be even
    if (zones < 2) zones = 2;
    uint16_t zoneLen = layer->size.y / zones;
    uint16_t offset = (layer->size.y - zones * zoneLen) >> 1;

    layer->fill_solid(ColorFromPalette(layerP.palette, -counter));

    for (int z = 0; z < zones; z++) {
      uint16_t pos = offset + z * zoneLen;
      for (int i = 0; i < zoneLen; i++) {
        uint8_t colorIndex = (i * 255 / zoneLen) - counter;
        uint16_t led = (z & 0x01) ? i : (zoneLen - 1) - i;
        layer->setRGB(Coord3D(0, pos + led), ColorFromPalette(layerP.palette, colorIndex));
      }
    }
  }
};

static void mode_fireworks(VirtualLayer* layer, uint16_t x, uint16_t aux0, uint16_t aux1, uint8_t speed, uint8_t intensity, bool useAudio = false) {
  // fade_out(0);
  layer->fadeToBlackBy(10);
  // if (call == 0) {
  //   aux0 = UINT16_MAX;
  //   aux1 = UINT16_MAX;
  // }
  bool valid1 = (aux0 < layer->size.y);
  bool valid2 = (aux1 < layer->size.y);
  CRGB sv1 = 0, sv2 = 0;
  if (valid1) sv1 = layer->getRGB(Coord3D(x, layer->size.y - 1 - aux0));
  if (valid2) sv2 = layer->getRGB(Coord3D(x, layer->size.y - 1 - aux1));

  // WLEDSR
  uint8_t blurAmount = 255 - speed;
  uint8_t my_intensity = 129 - intensity;
  bool addPixels = true;  // false -> inhibit new pixels in silence
  int soundColor = -1;    // -1 = random color; 0..255 = use as palette index

  // if (useAudio) {
  //   if (FFT_MajorPeak < 100)    { blurAmount = 254;} // big blobs
  //   else {
  //     if (FFT_MajorPeak > 3200) { blurAmount = 1;}   // small blobs
  //     else {                                         // blur + color depends on major frequency
  //       float musicIndex = logf(FFT_MajorPeak);            // log scaling of peak freq
  //       blurAmount = mapff(musicIndex, 4.60, 8.08, 253, 1);// map to blur range (low freq = more blur)
  //       blurAmount = constrain(blurAmount, 1, 253);        // remove possible "overshot" results
  //       soundColor = mapff(musicIndex, 4.6, 8.08, 0, 255); // pick color from frequency
  //   } }
  //   if (sampleAgc <= 1.0) {      // silence -> no new pixels, just blur
  //     valid1 = valid2 = false;   // do not copy last pixels
  //     addPixels = false;
  //     blurAmount = 128;
  //   }
  //   my_intensity = 129 - (speed >> 1); // dirty hack: use "speed" slider value intensity (no idea how to _disable_ the first slider, but show the second one)
  //   if (samplePeak == 1) my_intensity -= my_intensity / 4;    // inclease intensity at peaks
  //   if (samplePeak > 1) my_intensity = my_intensity / 2;      // double intensity at main peaks
  // }
  // // WLEDSR end

  layer->blur1d(blurAmount, x);  // only blur x column. todo: check blurColumn ...
  if (valid1) layer->setRGB(Coord3D(x, layer->size.y - 1 - aux0), sv1);
  if (valid2) layer->setRGB(Coord3D(x, layer->size.y - 1 - aux1), sv2);

  if (addPixels) {  // WLEDSR
    for (uint16_t i = 0; i < max(1, layer->size.y / 20); i++) {
      if (random8(my_intensity) == 0) {
        uint16_t index = random(layer->size.y);
        if (soundColor < 0)
          layer->setRGB(Coord3D(x, layer->size.y - 1 - index), ColorFromPalette(layerP.palette, random8()));
        else
          layer->setRGB(Coord3D(x, layer->size.y - 1 - index), ColorFromPalette(layerP.palette, soundColor + random8(24)));  // WLEDSR
        aux1 = aux0;
        aux0 = index;
      }
    }
  }
  // return FRAMETIME;
}

class RainEffect : public Node {
 public:
  static const char* name() { return "Rain"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🐙"; }

  uint8_t speed = 128;
  uint8_t intensity = 64;

  void setup() override {
    addControl(speed, "speed", "slider", 1, 255);
    addControl(intensity, "intensity", "slider", 1, 128);
  }

  Spark* drops = nullptr;
  uint16_t nrOfDrops = 0;

  ~RainEffect() override { freeMB(drops); }

  void onSizeChanged(const Coord3D& prevSize) override {
    Spark* newAlloc = reallocMB<Spark>(drops, layer->size.x);

    if (newAlloc) {
      drops = newAlloc;
      nrOfDrops = layer->size.x;
      for (int x = 0; x < layer->size.x; x++) {
        drops[x].pos = 0;
        drops[x].col = 0;
        drops[x].vel = 0;
      }
    } else {
      EXT_LOGE(ML_TAG, "(re)allocate drops failed");
    }
  }

  void loop() override {
    // if(call == 0) {
    // layer->fill(CRGB::Black);
    // }
    for (int x = 0; x < nrOfDrops; x++) {
      drops[x].vel += 1000 / 40;                                           // FRAMETIME;
      if (drops[x].vel > (5U + (50U * (255U - speed)) / layer->size.y)) {  // SPEED_FORMULA_L) {
        drops[x].vel = 1;
        // if (strip.isMatrix) {
        //   //uint32_t ctemp[layer->size.y];
        //   //for (int i = 0; i<layer->size.y; i++) ctemp[i] = layer->getRGB(i, layer->size.x-1);
        //   layer->move(6, 1, true);  // move all pixels down
        //   //for (int i = 0; i<layer->size.y; i++) layer->setRGB(Coord3D(i, 0), ctemp[i]); // wrap around
        //   drops[x].pos = (drops[x].pos % layer->size.y) + (drops[x].pos / layer->size.y + 1) * layer->size.y;
        //   drops[x].col = (drops[x].col % layer->size.y) + (drops[x].col / layer->size.y + 1) * layer->size.y;
        // } else
        {
          // shift all leds down
          CRGB ctemp = layer->getRGB(Coord3D(x, 0));
          for (int i = 0; i < layer->size.y - 1; i++) {
            layer->setRGB(Coord3D(x, layer->size.y - 1 - i), layer->getRGB(Coord3D(x, layer->size.y - 1 - (i + 1))));
          }
          layer->setRGB(Coord3D(x, 0), ctemp);  // wrap around
          drops[x].pos++;                       // increase spark index
          drops[x].col++;
        }
        if (drops[x].pos <= 0) drops[x].pos = UINT16_MAX;     // reset previous spark position
        if (drops[x].col <= 0) drops[x].pos = UINT16_MAX;     // reset previous spark position
        if (drops[x].pos >= layer->size.y) drops[x].pos = 0;  // ignore
        if (drops[x].col >= layer->size.y) drops[x].col = 0;
      }
      mode_fireworks(layer, x, drops[x].pos, drops[x].col, speed, intensity);
    }
  }
};  // RainEffect

  #define maxNumDrops 6
class DripEffect : public Node {
 public:
  static const char* name() { return "Drip"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🐙💫"; }

  uint8_t gravityControl = 128;
  uint8_t drips = 4;
  uint8_t swell = 4;
  uint8_t bounce = 64;

  void setup() override {
    addControl(gravityControl, "gravity", "slider", 1, 255);
    addControl(drips, "drips", "slider", 1, 6);
    addControl(swell, "swell", "slider", 1, 6);
    addControl(bounce, "bounce", "slider");
  }

  Spark (*drops)[maxNumDrops] = nullptr;  //[maxColumns][maxNumBalls];
  uint16_t nrOfDrops = 0;

  ~DripEffect() override { freeMB(drops); }

  void onSizeChanged(const Coord3D& prevSize) override {
    Spark(*newAlloc)[maxNumDrops] = reallocMB<Spark[maxNumDrops]>(drops, layer->size.x);

    if (newAlloc) {
      drops = newAlloc;
      nrOfDrops = layer->size.x;
      for (int x = 0; x < layer->size.x; x++) {
        for (int j = 0; j < maxNumDrops; j++) {
          drops[x][j].colIndex = init;  // Set to init so loop() will initialize properly
        }
      }
    } else {
      EXT_LOGE(ML_TAG, "(re)allocate drops failed");
    }
  }

  enum DropState { init = 0, forming = 1, falling = 2, bouncing = 5 };

  void loop() override {
    layer->fadeToBlackBy(90);

    float gravity = -(gravityControl / 800000.0f);

    gravity *= max(1, layer->size.y - 1);
    int sourcedrop = 12;

    for (int x = 0; x < nrOfDrops; x++) {
      for (int j = 0; j < drips; j++) {
        if (drops[x][j].colIndex == init) {                      // init
          drops[x][j].pos = layer->size.y - 1;                   // start at end
          drops[x][j].vel = 0;                                   // speed
          drops[x][j].col = sourcedrop;                          // brightness
          drops[x][j].colIndex = forming;                        // drop state
          CRGB c = ColorFromPalette(layerP.palette, random8());  // random color by MoonModules, hacked into velX of type float
          memcpy(&drops[x][j].velX, &c, sizeof(CRGB));
        }
        CRGB dropColor;
        memcpy(&dropColor, &drops[x][j].velX, sizeof(CRGB));  // hacked back

        layer->setRGB(Coord3D(x, 0), blend(CRGB::Black, dropColor, sourcedrop));  // water source
        if (drops[x][j].colIndex == forming) {
          if (drops[x][j].col > 255) drops[x][j].col = 255;
          layer->setRGB(Coord3D(x, layer->size.y - 1 - drops[x][j].pos), blend(CRGB::Black, dropColor, drops[x][j].col));

          drops[x][j].col += swell;  // swelling

          if (random16() <= drops[x][j].col * swell * swell / 10) {  // random drop
            drops[x][j].colIndex = falling;
            drops[x][j].col = 255;
          }
        }
        if (drops[x][j].colIndex > forming) {  // falling
          if (drops[x][j].pos > 0) {           // fall until end of layer
            drops[x][j].pos += drops[x][j].vel;
            if (drops[x][j].pos < 0) drops[x][j].pos = 0;
            drops[x][j].vel += gravity;  // gravity is negative

            layer->setRGB(Coord3D(x, layer->size.y - 1 - drops[x][j].pos), dropColor);  // needed for bouncing

            for (int i = 1; i < 7 - drops[x][j].colIndex; i++) {                                                       // some minor math so we don't expand bouncing droplets
              uint16_t pos = constrain(drops[x][j].pos + i, 0, layer->size.y - 1);                                     // this is BAD, returns a pos >= layer->size.y occasionally
              layer->setRGB(Coord3D(x, layer->size.y - 1 - pos), blend(CRGB::Black, dropColor, drops[x][j].col / i));  // spread pixel with fade while falling
            }

            if (drops[x][j].colIndex > falling) {  // during bounce, some water is on the floor
              layer->setRGB(Coord3D(x, layer->size.y - 1), blend(dropColor, CRGB::Black, drops[x][j].col));
            }
          } else {                                 // we hit bottom, pos <= 0
            if (drops[x][j].colIndex > falling) {  // bouncing, already hit once, so back to forming
              drops[x][j].colIndex = init;
            } else {
              if (drops[x][j].colIndex == falling) {                // init bounce
                drops[x][j].vel = -drops[x][j].vel * bounce / 255;  // reverse velocity with damping
                drops[x][j].pos += drops[x][j].vel;
              }
              drops[x][j].col = sourcedrop * 2;
              drops[x][j].colIndex = bouncing;
            }
          }
        }
      }
    }
  }
};  // DripEffect

class HeartBeatEffect : public Node {
 public:
  static const char* name() { return "HeartBeat"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "🐙💫♥"; }

  uint8_t speed = 15;
  uint8_t intensity = 128;

  void setup() override {
    addControl(speed, "speed", "slider", 0, 31);
    addControl(intensity, "intensity", "slider");
  }

  bool isSecond;
  uint16_t bri_lower;
  unsigned long step;

  void loop() override {
    uint8_t bpm = 40 + (speed);
    uint32_t msPerBeat = (60000L / bpm);
    uint32_t secondBeat = (msPerBeat / 3);
    unsigned long beatTimer = millis() - step;

    bri_lower = bri_lower * 2042 / (2048 + intensity);

    if ((beatTimer > secondBeat) && !isSecond) {  // time for the second beat?
      bri_lower = UINT16_MAX;                     // 3/4 bri
      isSecond = true;
    }

    if (beatTimer > msPerBeat) {  // time to reset the beat timer?
      bri_lower = UINT16_MAX;     // full bri
      isSecond = false;
      step = millis();
    }

    for (int i = 0; i < layer->size.y; i++) {
      layer->setRGB(Coord3D(0, i), ColorFromPalette(layerP.palette, ::map(i, 0, layer->size.y, 0, 255), 255 - (bri_lower >> 8)));
    }
  }
};  // HeartBeatEffect

class DJLightEffect : public Node {
 public:
  static const char* name() { return "DJLight"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "♫🐙"; }

  uint8_t speed = 255;
  bool candyFactory = true;
  uint8_t fade = 4;

  void setup() override {
    layer->fill_solid(CRGB::Black);
    addControl(speed, "speed", "slider");
    addControl(candyFactory, "candyFactory", "checkbox");
    addControl(fade, "fade", "slider", 0, 10);
  }

  uint8_t aux0;

  void loop() override {
    const int mid = layer->size.y / 2;

    uint8_t secondHand = (speed < 255) ? (micros() / (256 - speed) / 500 % 16) : 0;
    if ((speed > 254) || (aux0 != secondHand)) {  // WLEDMM allow run run at full speed
      aux0 = secondHand;

      CRGB color = CRGB(0, 0, 0);
      // color = CRGB(sharedData.bands[NUM_GEQ_CHANNELS-1]/2, sharedData.bands[5]/2, sharedData.bands[0]/2);   // formula from 0.13.x (10Khz): R = 3880-5120, G=240-340, B=60-100
      if (!candyFactory) {
        color = CRGB(sharedData.bands[12] / 2, sharedData.bands[3] / 2, sharedData.bands[1] / 2);  // formula for 0.14.x  (22Khz): R = 3015-3704, G=216-301, B=86-129
      } else {
        // candy factory: an attempt to get more colors
        color = CRGB(sharedData.bands[11] / 2 + sharedData.bands[12] / 4 + sharedData.bands[14] / 4,  // red  : 2412-3704 + 4479-7106
                     sharedData.bands[4] / 2 + sharedData.bands[3] / 4,                               // green: 216-430
                     sharedData.bands[0] / 4 + sharedData.bands[1] / 4 + sharedData.bands[2] / 4);    // blue:  46-216
        if ((color.getLuma() < 96) && (sharedData.volume >= 1.5f)) {                                  // enhance "almost dark" pixels with yellow, based on not-yet-used channels
          unsigned yello_g = (sharedData.bands[5] + sharedData.bands[6] + sharedData.bands[7]) / 3;
          unsigned yello_r = (sharedData.bands[7] + sharedData.bands[8] + sharedData.bands[9] + sharedData.bands[10]) / 4;
          color.green += (uint8_t)yello_g / 2;
          color.red += (uint8_t)yello_r / 2;
        }
      }

      if (sharedData.volume < 1.0f) color = CRGB(0, 0, 0);  // silence = black

      // make colors less "pastel", by turning up color saturation in HSV space
      if (color.getLuma() > 32) {  // don't change "dark" pixels
        CHSV hsvColor = rgb2hsv_approximate(color);
        hsvColor.v = constrain(hsvColor.v, 48, 204);  // 48 < brightness < 204
        if (candyFactory)
          hsvColor.s = max(hsvColor.s, (uint8_t)204);  // candy factory mode: strongly turn up color saturation (> 192)
        else
          hsvColor.s = max(hsvColor.s, (uint8_t)108);  // normal mode: turn up color saturation to avoid pastels
        color = hsvColor;
      }
      // if (color.getLuma() > 12) color.maximizeBrightness();          // for testing

      // layer->setRGB(mid, color.fadeToBlackBy(map(sharedData.bands[4], 0, 255, 255, 4)));     // 0.13.x  fade -> 180hz-260hz
      uint8_t fadeVal = ::map(sharedData.bands[3], 0, 255, 255, 4);  // 0.14.x  fade -> 216hz-301hz
      if (candyFactory) fadeVal = constrain(fadeVal, 0, 176);        // "candy factory" mode - avoid complete fade-out
      layer->setRGB(Coord3D(0, mid), color.fadeToBlackBy(fadeVal));

      for (int i = layer->size.y - 1; i > mid; i--) layer->setRGB(Coord3D(0, i), layer->getRGB(Coord3D(0, i - 1)));  // move to the left
      for (int i = 0; i < mid; i++) layer->setRGB(Coord3D(0, i), layer->getRGB(Coord3D(0, i + 1)));                  // move to the right

      layer->fadeToBlackBy(fade);
    }
  }
};  // DJLight

// /*
//  * Spotlights moving back and forth that cast dancing shadows.
//  * Shine this through tree branches/leaves or other close-up objects that cast
//  * interesting shadows onto a ceiling or tarp.
//  *
//  * By Steve Pomeroy @xxv
//  */
// uint16_t mode_dancing_shadows(void)
// {
//   if (layer->nrOfLights == 1) return mode_static();
//   uint8_t numSpotlights = ::map(intensity, 0, 255, 2, SPOT_MAX_COUNT);  // 49 on 32 segment ESP32, 17 on 16 segment ESP8266
//   bool initialize = aux0 != numSpotlights;
//   aux0 = numSpotlights;

//   uint16_t dataSize = sizeof(spotlight) * numSpotlights;
//   if (!SEGENV.allocateData(dataSize)) return mode_static(); //allocation failed
//   Spotlight* spotlights = reinterpret_cast<Spotlight*>(SEGENV.data);
//   if (call == 0) SEGENV.setUpLeds();   // WLEDMM use lossless getRGB()

//   layer->fill(CRGB::Black);

//   unsigned long time = millis();
//   bool respawn = false;

//   for (size_t i = 0; i < numSpotlights; i++) {
//     if (!initialize) {
//       // advance the position of the spotlight
//       int16_t delta = (float)(time - spotlights[i].lastUpdateTime) *
//                   (spotlights[i].speed * ((1.0f + speed)/100.0f));

//       if (abs(delta) >= 1) {
//         spotlights[i].position += delta;
//         spotlights[i].lastUpdateTime = time;
//       }

//       respawn = (spotlights[i].speed > 0.0 && spotlights[i].position > (layer->nrOfLights + 2))
//              || (spotlights[i].speed < 0.0 && spotlights[i].position < -(spotlights[i].width + 2));
//     }

//     if (initialize || respawn) {
//       spotlights[i].colorIdx = random8();
//       spotlights[i].width = random8(1, 10);

//       spotlights[i].speed = 1.0f/random8(4, 50);

//       if (initialize) {
//         spotlights[i].position = random16(layer->nrOfLights);
//         spotlights[i].speed *= random8(2) > 0 ? 1.0 : -1.0;
//       } else {
//         if (random8(2)) {
//           spotlights[i].position = layer->nrOfLights + spotlights[i].width;
//           spotlights[i].speed *= -1.0f;
//         }else {
//           spotlights[i].position = -spotlights[i].width;
//         }
//       }

//       spotlights[i].lastUpdateTime = time;
//       spotlights[i].type = random8(SPOT_TYPES_COUNT);
//     }

//     uint32_t color = layer-> ColorFromPalette(layer->layerP->palette, spotlights[i].colorIdx);
//     int start = spotlights[i].position;

//     if (spotlights[i].width <= 1) {
//       if (start >= 0 && start < layer->nrOfLights) {
//         layer->blendColor(start, color, 128);
//       }
//     } else {
//       switch (spotlights[i].type) {
//         case SPOT_TYPE_SOLID:
//           for (size_t j = 0; j < spotlights[i].width; j++) {
//             if ((start + j) >= 0 && (start + j) < layer->nrOfLights) {
//               layer->blendColor(start + j, color, 128);
//             }
//           }
//         break;

//         case SPOT_TYPE_GRADIENT:
//           for (size_t j = 0; j < spotlights[i].width; j++) {
//             if ((start + j) >= 0 && (start + j) < layer->nrOfLights) {
//               layer->blendColor(start + j, color, cubicwave8(map(j, 0, spotlights[i].width - 1, 0, 255)));
//             }
//           }
//         break;

//         case SPOT_TYPE_2X_GRADIENT:
//           for (size_t j = 0; j < spotlights[i].width; j++) {
//             if ((start + j) >= 0 && (start + j) < layer->nrOfLights) {
//               layer->blendColor(start + j, color, cubicwave8(2 * ::map(j, 0, spotlights[i].width - 1, 0, 255)));
//             }
//           }
//         break;

//         case SPOT_TYPE_2X_DOT:
//           for (size_t j = 0; j < spotlights[i].width; j += 2) {
//             if ((start + j) >= 0 && (start + j) < layer->nrOfLights) {
//               layer->blendColor(start + j, color, 128);
//             }
//           }
//         break;

//         case SPOT_TYPE_3X_DOT:
//           for (size_t j = 0; j < spotlights[i].width; j += 3) {
//             if ((start + j) >= 0 && (start + j) < layer->nrOfLights) {
//               layer->blendColor(start + j, color, 128);
//             }
//           }
//         break;

//         case SPOT_TYPE_4X_DOT:
//           for (size_t j = 0; j < spotlights[i].width; j += 4) {
//             if ((start + j) >= 0 && (start + j) < layer->nrOfLights) {
//               layer->blendColor(start + j, color, 128);
//             }
//           }
//         break;
//       }
//     }
//   }

//   return FRAMETIME;
// }
// static const char _data_FX_MODE_DANCING_SHADOWS[] PROGMEM = "Dancing Shadows@!,# of shadows;!;!";

// //  TwinkleFOX by Mark Kriegsman: https://gist.github.com/kriegsman/756ea6dcae8e30845b5a
// //
// //  TwinkleFOX: Twinkling 'holiday' lights that fade in and out.
// //  Colors are chosen from a palette. Read more about this effect using the link above!

// // If COOL_LIKE_INCANDESCENT is set to 1, colors will
// // fade out slighted 'reddened', similar to how
// // incandescent bulbs change color as they get dim down.
// #define COOL_LIKE_INCANDESCENT 1

// CRGB twinklefox_one_twinkle(uint32_t ms, uint8_t salt, bool cat)
// {
//   // Overall twinkle speed (changed)
//   uint16_t ticks = ms / aux0;
//   uint8_t fastcycle8 = ticks;
//   uint16_t slowcycle16 = (ticks >> 8) + salt;
//   slowcycle16 += sin8_t(slowcycle16);
//   slowcycle16 = (slowcycle16 * 2053) + 1384;
//   uint8_t slowcycle8 = (slowcycle16 & 0xFF) + (slowcycle16 >> 8);

//   // Overall twinkle density.
//   // 0 (NONE lit) to 8 (ALL lit at once).
//   // Default is 5.
//   uint8_t twinkleDensity = (intensity >> 5) +1;

//   uint8_t bright = 0;
//   if (((slowcycle8 & 0x0E)/2) < twinkleDensity) {
//     uint8_t ph = fastcycle8;
//     // This is like 'triwave8', which produces a
//     // symmetrical up-and-down triangle sawtooth waveform, except that this
//     // function produces a triangle wave with a faster attack and a slower decay
//     if (cat) //twinklecat, variant where the leds instantly turn on
//     {
//       bright = 255 - ph;
//     } else { //vanilla twinklefox
//       if (ph < 86) {
//       bright = ph * 3;
//       } else {
//         ph -= 86;
//         bright = 255 - (ph + (ph/2));
//       }
//     }
//   }

//   uint8_t hue = slowcycle8 - salt;
//   CRGB c;
//   if (bright > 0) {
//     c = ColorFromPalette(SEGPALETTE, hue, bright, NOBLEND);
//     if(COOL_LIKE_INCANDESCENT == 1) {
//       // This code takes a pixel, and if its in the 'fading down'
//       // part of the cycle, it adjusts the color a little bit like the
//       // way that incandescent bulbs fade toward 'red' as they dim.
//       if (fastcycle8 >= 128)
//       {
//         uint8_t cooling = (fastcycle8 - 128) >> 4;
//         c.g = qsub8(c.g, cooling);
//         c.b = qsub8(c.b, cooling * 2);
//       }
//     }
//   } else {
//     c = CRGB::Black;
//   }
//   return c;
// }

// //  This function loops over each pixel, calculates the
// //  adjusted 'clock' that this pixel should use, and calls
// //  "CalculateOneTwinkle" on each pixel.  It then displays
// //  either the twinkle color of the background color,
// //  whichever is brighter.
// uint16_t twinklefox_base(bool cat)
// {
//   // "PRNG16" is the pseudorandom number generator
//   // It MUST be reset to the same starting value each time
//   // this function is called, so that the sequence of 'random'
//   // numbers that it generates is (paradoxically) stable.
//   uint16_t PRNG16 = 11337;

//   // Calculate speed
//   if (speed > 100) aux0 = 3 + ((255 - speed) >> 3);
//   else aux0 = 22 + ((100 - speed) >> 1);

//   // Set up the background color, "bg".
//   CRGB bg = CRGB(SEGCOLOR(1));
//   uint8_t bglight = bg.getAverageLight();
//   if (bglight > 64) {
//     bg.nscale8_video(16); // very bright, so scale to 1/16th
//   } else if (bglight > 16) {
//     bg.nscale8_video(64); // not that bright, so scale to 1/4th
//   } else {
//     bg.nscale8_video(86); // dim, scale to 1/3rd.
//   }

//   uint8_t backgroundBrightness = bg.getAverageLight();

//   for (int i = 0; i < layer->nrOfLights; i++) {

//     PRNG16 = (uint16_t)(PRNG16 * 2053) + 1384; // next 'random' number
//     uint16_t myclockoffset16= PRNG16; // use that number as clock offset
//     PRNG16 = (uint16_t)(PRNG16 * 2053) + 1384; // next 'random' number
//     // use that number as clock speed adjustment factor (in 8ths, from 8/8ths to 23/8ths)
//     uint8_t myspeedmultiplierQ5_3 =  ((((PRNG16 & 0xFF)>>4) + (PRNG16 & 0x0F)) & 0x0F) + 0x08;
//     uint32_t myclock30 = (uint32_t)((millis() * myspeedmultiplierQ5_3) >> 3) + myclockoffset16;
//     uint8_t  myunique8 = PRNG16 >> 8; // get 'salt' value for this pixel

//     // We now have the adjusted 'clock' for this pixel, now we call
//     // the function that computes what color the pixel should be based
//     // on the "brightness = f( time )" idea.
//     CRGB c = twinklefox_one_twinkle(myclock30, myunique8, cat);

//     uint8_t cbright = c.getAverageLight();
//     int16_t deltabright = cbright - backgroundBrightness;
//     if (deltabright >= 32 || (!bg)) {
//       // If the new pixel is significantly brighter than the background color,
//       // use the new color.
//       layer->setRGB(i, c.red, c.green, c.blue);
//     } else if (deltabright > 0) {
//       // If the new pixel is just slightly brighter than the background color,
//       // mix a blend of the new color and the background color
//       layer->setRGB(i, color_blend(RGBW32(bg.r,bg.g,bg.b,0), RGBW32(c.r,c.g,c.b,0), deltabright * 8));
//     } else {
//       // if the new pixel is not at all brighter than the background color,
//       // just use the background color.
//       layer->setRGB(i, bg.r, bg.g, bg.b);
//     }
//   }
//   return FRAMETIME;
// }

// uint16_t mode_twinklefox()
// {
//   return twinklefox_base(false);
// }
// static const char _data_FX_MODE_TWINKLEFOX[] PROGMEM = "Twinklefox@!,Twinkle rate;!,!;!";

// uint16_t mode_twinklecat()
// {
//   return twinklefox_base(true);
// }
// static const char _data_FX_MODE_TWINKLECAT[] PROGMEM = "Twinklecat@!,Twinkle rate;!,!;!";

#endif