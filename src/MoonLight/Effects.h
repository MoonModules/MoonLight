/**
    @title     MoonLight
    @file      Effects.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/general/utilities/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact moonmodules@icloud.com
**/

#if FT_MOONLIGHT

class SolidEffect: public Node {
  public:

  static const char * name() {return "Solid 🔥💡";}
  static uint8_t dim() {return _1D;}
  static const char * tags() {return "";}

  uint8_t red = 182;
  uint8_t green = 15;
  uint8_t blue = 98;
  uint8_t brightness = 255;

  void setup() override {
    addControl(red, "red", "range");
    addControl(green, "green", "range");
    addControl(blue, "blue", "range");
    addControl(brightness, "brightness", "range");

  }

  void loop() override {
      layerV->fill_solid(CRGB(red * brightness/255, green * brightness/255, blue * brightness/255));
  }
};

class FixedRectangleEffect: public Node {
  public:

  static const char * name() {return "Fixed Rectangle 🔥💡";}
  static uint8_t dim() {return _2D;}
  static const char * tags() {return "";}

  uint8_t red = 255;
  uint8_t green = 255;
  uint8_t blue = 255;
  uint8_t width = 1;
  uint8_t x = 0;
  uint8_t height = 1;
  uint8_t y = 1;

  void setup() override {
    addControl(red, "red", "range", 50);
    addControl(green, "green", "range", 50);
    addControl(blue, "blue", "range", 50);
    addControl(x, "X position", "range", 0);
    addControl(y, "Y position", "range", 0);
    addControl(width, "Rectangle width", "range", 1);
    addControl(height, "Rectangle height", "range", 1);

  }

  void loop() override {
    Coord3D pos = {0,0,0};
    for (pos.x = 0; pos.x < layerV->size.x; pos.x++) {
      for (pos.y = 0; pos.y < layerV->size.y; pos.y++) {
        if (((pos.x >= x) && (pos.x < x + width)) && ((pos.y >= y) && (pos.y < y + height))) {
          layerV->setRGB(pos, CRGB(red, green, blue));
        } else {
          layerV->setRGB(pos, CRGB(0, 0, 0));
        }
      }
    }
  }
};

//alphabetically from here, Custom Nodes at the end

//BouncingBalls inspired by WLED
#define maxNumBalls 16
//each needs 12 bytes
struct Ball {
  unsigned long lastBounceTime;
  float impactVelocity;
  float height;
};

class BouncingBallsEffect: public Node {
  public:

  static const char * name() {return "BouncingBalls 🔥🎨💡";}
  static uint8_t dim() {return _1D;}
  static const char * tags() {return "";}

  uint8_t grav = 128;
  uint8_t numBalls = 8;

  void setup() override {
    addControl(grav, "grav", "range");
    addControl(numBalls, "numBalls", "range", 1, maxNumBalls);
  }

  //binding of loop persistent values (pointers)
  Ball balls[16][maxNumBalls];

  void loop() override {
    layerV->fadeToBlackBy(100);

    // non-chosen color is a random color
    const float gravity = -9.81f; // standard value of gravity
    // const bool hasCol2 = SEGCOLOR(2);
    const unsigned long time = millis();

    //not necessary as effectControls is cleared at setup(LedsLayer &leds)
    // if (call == 0) {
    //   for (size_t i = 0; i < maxNumBalls; i++) balls[i].lastBounceTime = time;
    // }

    for (uint8_t y =0; y < MIN(layerV->size.y,16); y++) { //Min for the time being
      for (size_t i = 0; i < MIN(numBalls, maxNumBalls); i++) {
        float timeSinceLastBounce = (time - balls[y][i].lastBounceTime)/((255-grav)/64 + 1);
        float timeSec = timeSinceLastBounce/1000.0f;
        balls[y][i].height = (0.5f * gravity * timeSec + balls[y][i].impactVelocity) * timeSec; // avoid use pow(x, 2) - its extremely slow !

        if (balls[y][i].height <= 0.0f) {
          balls[y][i].height = 0.0f;
          //damping for better effect using multiple balls
          float dampening = 0.9f - float(i)/float(numBalls * numBalls); // avoid use pow(x, 2) - its extremely slow !
          balls[y][i].impactVelocity = dampening * balls[y][i].impactVelocity;
          balls[y][i].lastBounceTime = time;

          if (balls[y][i].impactVelocity < 0.015f) {
            float impactVelocityStart = sqrtf(-2.0f * gravity) * random8(5,11)/10.0f; // randomize impact velocity
            balls[y][i].impactVelocity = impactVelocityStart;
          }
        } else if (balls[y][i].height > 1.0f) {
          continue; // do not draw OOB ball
        }

        // uint32_t color = SEGCOLOR(0);
        // if (layerV->palette) {
        //   color = layerV->color_wheel(i*(256/MAX(numBalls, 8)));
        // } 
        // else if (hasCol2) {
        //   color = SEGCOLOR(i % NUM_COLORS);
        // }

        uint8_t pos = roundf(balls[y][i].height * (layerV->size.x - 1));

        CRGB color = ColorFromPalette(layerV->layerP->palette, i*(256/max(numBalls, (uint8_t)8))); //error: no matching function for call to 'max(uint8_t&, int)'

        layerV->setRGB({pos, y, 0}, color);
        // if (layerV->size.x<32) layerV->setPixelColor(indexToVStrip(pos, stripNr), color); // encode virtual strip into index
        // else           layerV->setPixelColor(balls[i].height + (stripNr+1)*10.0f, color);
      } //balls      layerV->fill_solid(CRGB::White);
    }
  }
};

//DistortionWaves inspired by WLED, ldirko and blazoncek, https://editor.soulmatelights.com/gallery/1089-distorsion-waves
static uint8_t gamma8(uint8_t b) { //we do nothing with gamma for now
  return b;
}

class DistortionWavesEffect: public Node {
public:
  static const char * name() {return "DistortionWaves 🔥💡";}
  static uint8_t dim() {return _2D;}
  static const char * tags() {return "";}

  uint8_t speed = 4;
  uint8_t scale = 4; 

  void setup() override {
    addControl(speed, "speed", "range", 0, 8);
    addControl(scale, "scale", "range", 0, 8);
  }

  void loop() override {
    //Binding of controls. Keep before binding of vars and keep in same order as in setup()

    uint8_t  w = 2;

    uint16_t a  = millis()/32;
    uint16_t a2 = a/2;
    uint16_t a3 = a/3;

    uint16_t cx =  beatsin8(10-speed,0,layerV->size.x-1)*scale;
    uint16_t cy =  beatsin8(12-speed,0,layerV->size.y-1)*scale;
    uint16_t cx1 = beatsin8(13-speed,0,layerV->size.x-1)*scale;
    uint16_t cy1 = beatsin8(15-speed,0,layerV->size.y-1)*scale;
    uint16_t cx2 = beatsin8(17-speed,0,layerV->size.x-1)*scale;
    uint16_t cy2 = beatsin8(14-speed,0,layerV->size.y-1)*scale;
    
    uint16_t xoffs = 0;
    Coord3D pos = {0,0,0};
    for (pos.x = 0; pos.x < layerV->size.x; pos.x++) {
      xoffs += scale;
      uint16_t yoffs = 0;

      for (pos.y = 0; pos.y < layerV->size.y; pos.y++) {
        yoffs += scale;

        byte rdistort = cos8((cos8(((pos.x<<3)+a )&255)+cos8(((pos.y<<3)-a2)&255)+a3   )&255)>>1; 
        byte gdistort = cos8((cos8(((pos.x<<3)-a2)&255)+cos8(((pos.y<<3)+a3)&255)+a+32 )&255)>>1; 
        byte bdistort = cos8((cos8(((pos.x<<3)+a3)&255)+cos8(((pos.y<<3)-a) &255)+a2+64)&255)>>1; 

        byte valueR = rdistort+ w*  (a- ( ((xoffs - cx)  * (xoffs - cx)  + (yoffs - cy)  * (yoffs - cy))>>7  ));
        byte valueG = gdistort+ w*  (a2-( ((xoffs - cx1) * (xoffs - cx1) + (yoffs - cy1) * (yoffs - cy1))>>7 ));
        byte valueB = bdistort+ w*  (a3-( ((xoffs - cx2) * (xoffs - cx2) + (yoffs - cy2) * (yoffs - cy2))>>7 ));

        valueR = gamma8(cos8(valueR));
        valueG = gamma8(cos8(valueG));
        valueB = gamma8(cos8(valueB));

        layerV->setRGB(pos, CRGB(valueR, valueG, valueB));
      }
    }
  }
}; // DistortionWaves

class FreqMatrixEffect: public Node {
public:

  static const char * name() {return "FreqMatrix 🔥♪💡";}
  static uint8_t dim() {return _1D;}
  static const char * tags() {return "";}

  uint8_t speed = 255;
  uint8_t fx = 128;
  uint8_t lowBin = 18;
  uint8_t highBin = 48;
  uint8_t sensitivity10 = 30;

  void setup() override {
    addControl(speed, "speed", "range");
    addControl(fx, "soundFX", "range");
    addControl(lowBin, "lowBin", "range");
    addControl(highBin, "highBin", "range");
    addControl(sensitivity10, "sensitivity", "range", 10, 100);
  }

  uint8_t aux0;

  void loop() override {
    uint8_t secondHand = (speed < 255) ? (micros()/(256-speed)/500 % 16) : 0;
    if((speed > 254) || (aux0 != secondHand)) {   // WLEDMM allow run run at full speed
      aux0 = secondHand;

      // Pixel brightness (value) based on volume * sensitivity * intensity
      // uint_fast8_t sensitivity10 = ::map(sensitivity, 0, 31, 10, 100); // reduced resolution slider // WLEDMM sensitivity * 10, to avoid losing precision
      int pixVal = audio.volume * (float)fx * (float)sensitivity10 / 2560.0f; // WLEDMM 2560 due to sensitivity * 10
      if (pixVal > 255) pixVal = 255;  // make a brightness from the last avg

      CRGB color = CRGB::Black;

      if (audio.majorPeak > MAX_FREQUENCY) audio.majorPeak = 1;
      // MajorPeak holds the freq. value which is most abundant in the last sample.
      // With our sampling rate of 10240Hz we have a usable freq range from roughtly 80Hz to 10240/2 Hz
      // we will treat everything with less than 65Hz as 0

      if ((audio.majorPeak > 80.0f) && (audio.volume > 0.25f)) { // WLEDMM
        // Pixel color (hue) based on major frequency
        int upperLimit = 80 + 42 * highBin;
        int lowerLimit = 80 + 3 * lowBin;
        //uint8_t i =  lowerLimit!=upperLimit ? ::map(audio.majorPeak, lowerLimit, upperLimit, 0, 255) : audio.majorPeak;  // (original formula) may under/overflow - so we enforce uint8_t
        int freqMapped =  lowerLimit!=upperLimit ? ::map(audio.majorPeak, lowerLimit, upperLimit, 0, 255) : audio.majorPeak;  // WLEDMM preserve overflows
        uint8_t i = abs(freqMapped) & 0xFF;  // WLEDMM we embrace overflow ;-) by "modulo 256"

        color = CHSV(i, 240, (uint8_t)pixVal); // implicit conversion to RGB supplied by FastLED
      }

      // shift the pixels one pixel up
      layerV->setRGB(0, color);
      for (int x = layerV->size.x - 1; x >= 0; x--) { //int as we count down!!!
        if (x!=0) color = layerV->getRGB(x-1);
        for (uint8_t y = 0; y < layerV->size.y; y++)
          for (uint8_t z = 0; z < layerV->size.z; z++)
            layerV->setRGB(intToCoord3D(x,y,z), color);
      }
    }
  }
};

class GEQEffect: public Node {
public:

  static const char * name() {return "GEQ 🔥🎨♫💡";}
  static uint8_t dim() {return _1D;}
  static const char * tags() {return "";}

  uint8_t fadeOut = 255;
  uint8_t ripple = 128;
  bool colorBars = false;
  bool smoothBars = true;

  uint16_t *previousBarHeight; //array
  unsigned long step;

  void setup() override {
    Node::setup();

    addControl(fadeOut, "fadeOut", "range");
    addControl(ripple, "ripple", "range");
    addControl(colorBars, "colorBars", "checkbox");
    addControl(smoothBars, "smoothBars", "checkbox");

    previousBarHeight = (uint16_t*)malloc(layerV->size.x * sizeof(uint16_t));
    if (!previousBarHeight) {
      ESP_LOGE(TAG, "malloc failed for previousBarHeight");
      return;
    }
    memset(previousBarHeight, 0, layerV->size.x * sizeof(uint16_t)); // initialize to 0
    step = millis();
  }

  ~GEQEffect() {
    ESP_LOGI(TAG, "free previousBarHeight");
    if (previousBarHeight) {
      free(previousBarHeight);
      previousBarHeight = nullptr;
    }
  }

  void loop() override {
    const int NUM_BANDS = NUM_GEQ_CHANNELS ; // ::map(layerV->custom1, 0, 255, 1, 16);

    #ifdef SR_DEBUG
    uint8_t samplePeak = *(uint8_t*)um_data->u_data[3];
    #endif

    bool rippleTime = false;
    if (millis() - step >= (256U - ripple)) {
      step = millis();
      rippleTime = true;
    }

    int fadeoutDelay = (256 - fadeOut) / 64; //256..1 -> 4..0
    size_t beat = ::map(beat16( fadeOut), 0, UINT16_MAX, 0, fadeoutDelay-1 ); // instead of call%fadeOutDelay

    if ((fadeoutDelay <= 1 ) || (beat == 0)) layerV->fadeToBlackBy(fadeOut);

    uint16_t lastBandHeight = 0;  // WLEDMM: for smoothing out bars

    //evenly distribute see also Funky Plank/By ewowi/From AXI
    float bandwidth = (float)layerV->size.x / NUM_BANDS;
    float remaining = bandwidth;
    uint8_t band = 0;
    Coord3D pos = {0,0,0};
    for (pos.x=0; pos.x < layerV->size.x; pos.x++) {
      //WLEDMM if not enough remaining
      if (remaining < 1) {band++; remaining+= bandwidth;} //increase remaining but keep the current remaining
      remaining--; //consume remaining

      // ppf("x %d b %d n %d w %f %f\n", x, band, NUM_BANDS, bandwidth, remaining);
      uint8_t frBand = ((NUM_BANDS < NUM_GEQ_CHANNELS) && (NUM_BANDS > 1)) ? ::map(band, 0, NUM_BANDS - 1, 0, NUM_GEQ_CHANNELS-1):band; // always use full range. comment out this line to get the previous behaviour.
      // frBand = constrain(frBand, 0, 15); //WLEDMM can never be out of bounds (I think...)
      uint16_t colorIndex = frBand * 17; //WLEDMM 0.255
      uint16_t bandHeight = audio.bands[frBand];  // WLEDMM we use the original ffResult, to preserve accuracy

      // WLEDMM begin - smooth out bars
      if ((pos.x > 0) && (pos.x < (layerV->size.x-1)) && (smoothBars)) {
        // get height of next (right side) bar
        uint8_t nextband = (remaining < 1)? band +1: band;
        nextband = constrain(nextband, 0, NUM_GEQ_CHANNELS-1);  // just to be sure
        frBand = ((NUM_BANDS < NUM_GEQ_CHANNELS) && (NUM_BANDS > 1)) ? ::map(nextband, 0, NUM_BANDS - 1, 0, NUM_GEQ_CHANNELS-1):nextband; // always use full range. comment out this line to get the previous behaviour.
        uint16_t nextBandHeight = audio.bands[frBand];
        // smooth Band height
        bandHeight = (7*bandHeight + 3*lastBandHeight + 3*nextBandHeight) / 12;   // yeees, its 12 not 13 (10% amplification)
        bandHeight = constrain(bandHeight, 0, 255);   // remove potential over/underflows
        colorIndex = ::map(pos.x, 0, layerV->size.x-1, 0, 255); //WLEDMM
      }
      lastBandHeight = bandHeight; // remember BandHeight (left side) for next iteration
      uint16_t barHeight = ::map(bandHeight, 0, 255, 0, layerV->size.y); // Now we map bandHeight to barHeight. do not subtract -1 from layerV->size.y here
      // WLEDMM end

      if (barHeight > layerV->size.y) barHeight = layerV->size.y;                      // WLEDMM ::map() can "overshoot" due to rounding errors
      if (barHeight > previousBarHeight[pos.x]) previousBarHeight[pos.x] = barHeight; //drive the peak up

      CRGB ledColor = CRGB::Black;

      for (pos.y=0; pos.y < barHeight; pos.y++) {
        if (colorBars) //color_vertical / color bars toggle
          colorIndex = ::map(pos.y, 0, layerV->size.y-1, 0, 255);

        ledColor = ColorFromPalette(layerV->layerP->palette, (uint8_t)colorIndex);

        layerV->setRGB(intToCoord3D(pos.x, layerV->size.y - 1 - pos.y, 0), ledColor);
      }

      if ((ripple > 0) && (previousBarHeight[pos.x] > 0) && (previousBarHeight[pos.x] < layerV->size.y))  // WLEDMM avoid "overshooting" into other segments
        layerV->setRGB(intToCoord3D(pos.x, layerV->size.y - previousBarHeight[pos.x], 0), (CRGB)CHSV( millis()/50, 255, 255)); // take millis()/50 color for the time being

      if (rippleTime && previousBarHeight[pos.x]>0) previousBarHeight[pos.x]--;    //delay/ripple effect

    }
  }
};

class LinesEffect: public Node {
public:

  static const char * name() {return "Lines 🔥💫";}
  static uint8_t dim() {return _1D;}
  static const char * tags() {return "";}

  uint8_t bpm = 120;

  void setup() override {
    addControl(bpm, "bpm", "range");
  }

  void loop() override {
    int   frameNr;

    layerV->fadeToBlackBy(255);

    Coord3D pos = {0,0,0};
    pos.x = ::map(beat16( bpm), 0, UINT16_MAX, 0, layerV->size.x ); //instead of call%width

    for (pos.y = 0; pos.y < layerV->size.y; pos.y++) {
      int colorNr = (frameNr / layerV->size.y) % 3;
      layerV->setRGB(pos, colorNr == 0?CRGB::Red:colorNr == 1?CRGB::Green:CRGB::Blue);
    }

    pos = {0,0,0};
    pos.y = ::map(beat16( bpm), 0, UINT16_MAX, 0, layerV->size.y ); //instead of call%height
    for (pos.x = 0; pos.x <  layerV->size.x; pos.x++) {
      int colorNr = (frameNr / layerV->size.x) % 3;
      layerV->setRGB(pos, colorNr == 0?CRGB::Red:colorNr == 1?CRGB::Green:CRGB::Blue);
    }
    (frameNr)++;
  }
};

class LissajousEffect: public Node {
public:

  static const char * name() {return "Lissajous 🔥🎨💡";}
  static uint8_t dim() {return _1D;}
  static const char * tags() {return "";}

  uint8_t xFrequency = 64;
  uint8_t fadeRate = 128;
  uint8_t speed = 128;

  void setup() override {
    addControl(xFrequency, "xFrequency", "range");
    addControl(fadeRate, "fadeRate", "range");
    addControl(speed, "speed", "range");
  }

  void loop() override {
    layerV->fadeToBlackBy(fadeRate);
    uint_fast16_t phase = millis() * speed / 256;  // allow user to control rotation speed, speed between 0 and 255!
    Coord3D locn = {0,0,0};
    for (int i=0; i < 256; i ++) {
        //WLEDMM: stick to the original calculations of xlocn and ylocn
        locn.x = sin8(phase/2 + (i*xFrequency)/64);
        locn.y = cos8(phase/2 + i*2);
        locn.x = (layerV->size.x < 2) ? 1 : (::map(2*locn.x, 0,511, 0,2*(layerV->size.x-1)) +1) /2;    // softhack007: "*2 +1" for proper rounding
        locn.y = (layerV->size.y < 2) ? 1 : (::map(2*locn.y, 0,511, 0,2*(layerV->size.y-1)) +1) /2;    // "layerV->size.y > 2" is needed to avoid div/0 in ::map()
        layerV->setRGB(locn, ColorFromPalette(layerV->layerP->palette, millis()/100+i, 255));
    }
  }
};

class RainbowEffect: public Node {
public:

  static const char * name() {return "Rainbow 🔥";}
  static uint8_t dim() {return _1D;}
  static const char * tags() {return "";}

  void loop() override {
    static uint8_t hue = 0;
    layerV->fill_rainbow(hue++, 7);
  }
};

// Author: @TroyHacks, from WLED MoonModules
// @license GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
class PaintBrushEffect: public Node {
public:

  static const char * name() {return "Paintbrush 🔥🎨♫🧊💡";}
  static uint8_t dim() {return _1D;}
  static const char * tags() {return "";}

  uint8_t oscillatorOffset = 6 *  160/255;
  uint8_t numLines = 255;
  uint8_t fadeRate = 40;
  uint8_t minLength = 0;
  bool color_chaos = false;
  bool soft = true;
  bool phase_chaos = false;
  
  void setup() override {
    addControl(oscillatorOffset, "oscillatorOffset", "range", 0, 16);
    addControl(numLines, "numLines", "range", 2, 255);
    addControl(fadeRate, "fadeRate", "range", 0, 128);
    addControl(minLength, "minLength", "range");
    addControl(color_chaos, "color_chaos", "checkbox");
    addControl(soft, "soft", "checkbox");
    addControl(phase_chaos, "phase_chaos", "checkbox");
  }

  uint16_t aux0Hue;
  uint16_t aux1Chaos;

  void loop() override {
    const uint16_t cols = layerV->size.x;
    const uint16_t rows = layerV->size.y;
    const uint16_t depth = layerV->size.z;

    // uint8_t numLines = map8(nrOfLines,1,64);
    
    (aux0Hue)++;  // hue
    layerV->fadeToBlackBy(fadeRate);

    aux1Chaos = phase_chaos?random8():0;

    for (size_t i = 0; i < numLines; i++) {
      uint8_t bin = ::map(i,0,numLines,0,15);
      
      uint8_t x1 = beatsin8(oscillatorOffset*1 + audio.bands[0]/16, 0, (cols-1), audio.bands[bin], aux1Chaos);
      uint8_t x2 = beatsin8(oscillatorOffset*2 + audio.bands[0]/16, 0, (cols-1), audio.bands[bin], aux1Chaos);
      uint8_t y1 = beatsin8(oscillatorOffset*3 + audio.bands[0]/16, 0, (rows-1), audio.bands[bin], aux1Chaos);
      uint8_t y2 = beatsin8(oscillatorOffset*4 + audio.bands[0]/16, 0, (rows-1), audio.bands[bin], aux1Chaos);
      uint8_t z1;
      uint8_t z2;
      int length;
      if (depth > 1) {
        z1 = beatsin8(oscillatorOffset*5 + audio.bands[0]/16, 0, (depth-1), audio.bands[bin], aux1Chaos);
        z2 = beatsin8(oscillatorOffset*6 + audio.bands[0]/16, 0, (depth-1), audio.bands[bin], aux1Chaos);

        length = sqrt((x2-x1) * (x2-x1) + (y2-y1) * (y2-y1) + (z2-z1) * (z2-z1));
      } else 
        length = sqrt((x2-x1) * (x2-x1) + (y2-y1) * (y2-y1));

      length = map8(audio.bands[bin],0,length);

      if (length > max(1, (int)minLength)) {
        CRGB color;
        if (color_chaos)
          color = ColorFromPalette(layerV->layerP->palette, i * 255 / numLines + ((aux0Hue)&0xFF), 255);
        else
          color = ColorFromPalette(layerV->layerP->palette, ::map(i, 0, numLines, 0, 255), 255);
        if (depth > 1)
          layerV->drawLine3D(x1, y1, z1, x2, y2, z2, color, soft, length); // no soft implemented in 3D yet
        else
          layerV->drawLine(x1, y1, x2, y2, color, soft, length);
      }
    }
  }
};

class RandomEffect: public Node {
  public:

  static const char * name() {return "Random 🔥";}
  static uint8_t dim() {return _1D;}
  static const char * tags() {return "";}

  void loop() override {
      layerV->fadeToBlackBy(70);
      layerV->setRGB(random16(layerV->nrOfLights), CRGB(255, random8(), 0));
  }
};

class RipplesEffect: public Node {
  public:

  static const char * name() {return "Ripples 🔥🧊💫";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}
  
  uint8_t speed = 50;
  uint8_t interval = 128;

  void setup() override {
    addControl(speed, "speed", "range");
    addControl(interval, "interval", "range");
  }

  void loop() override {

    float ripple_interval = 1.3f * ((255.0f - interval)/128.0f) * sqrtf(layerV->size.y);
    float time_interval = millis()/(100.0 - speed)/((256.0f-128.0f)/20.0f);

    layerV->fadeToBlackBy(255);

    Coord3D pos = {0,0,0};
    for (pos.z=0; pos.z<layerV->size.z; pos.z++) {
      for (pos.x=0; pos.x<layerV->size.x; pos.x++) {

        float d = distance(layerV->size.x/2.0f, layerV->size.z/2.0f, 0.0f, (float)pos.x, (float)pos.z, 0.0f) / 9.899495f * layerV->size.y;
        pos.y = floor(layerV->size.y/2.0f * (1 + sinf(d/ripple_interval + time_interval))); //between 0 and layerV->size.y

        layerV->setRGB(pos, (CRGB)CHSV( millis()/50 + random8(64), 200, 255));
      }
    }
  }
};

class RGBWParEffect: public Node {
  public:

  static const char * name() {return "RGBWPar 🔥";}

  uint8_t bpm = 30;
  uint8_t red = 30;
  uint8_t green = 30;
  uint8_t blue = 30;
  uint8_t white = 0;

  void setup() override {
    addControl(bpm, "bpm", "range");
    addControl(red, "red", "range"); //control["color"] = "Red"; ...
    addControl(green, "green", "range");
    addControl(blue, "blue", "range");
    addControl(white, "white", "range");
  }

  void loop() override {
    layerV->fadeToBlackBy(255); //reset all channels

    uint8_t pos = millis()*bpm/6000 % layerV->size.x; //beatsin16( bpm, 0, layerV->size.x-1);

    layerV->setRGB({pos,0,0}, CRGB(red, green, blue));
    layerV->setWhite({pos,0,0}, white);
  }
};
  
//AI generated
class SinusEffect: public Node {
  public:

  static const char * name() {return "Sinus 🔥";}
  static uint8_t dim() {return _1D;}
  static const char * tags() {return "";}

  uint8_t speed = 5;

  void setup() override {
    addControl(speed, "speed", "range");
  }

  void loop() override {
    layerV->fadeToBlackBy(70);

    uint8_t hueOffset =  millis() / 10;
    static uint16_t phase = 0; // Tracks the phase of the sine wave
    uint8_t brightness = 255;
    
    for (uint16_t i = 0; i < layerV->nrOfLights; i++) {
        // Calculate the sine wave value for the current LED
        uint8_t wave = sin8((i * 255 / layerV->nrOfLights) + phase);
        // Map the sine wave value to a color hue
        uint8_t hue = wave + hueOffset;
        // Set the LED color using the calculated hue
        layerV->setRGB(i, (CRGB)CHSV(hue, 255, brightness));
    }

    // Increment the phase to animate the wave
    phase += speed;
  }
};

class SphereMoveEffect: public Node {
  public:

  static const char * name() {return "SphereMove 🔥🧊💫";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  uint8_t speed = 50;
  
  void setup() override {
    addControl(speed, "speed", "range", 0, 99);
  }

  void loop() override {

    layerV->fadeToBlackBy(255);

    float time_interval = millis()/(100 - speed)/((256.0f-128.0f)/20.0f);

    Coord3D origin;
    origin.x = layerV->size.x / 2.0 * ( 1.0 + sinf(time_interval));
    origin.y = layerV->size.y / 2.0 * ( 1.0 + cosf(time_interval));
    origin.z = layerV->size.z / 2.0 * ( 1.0 + cosf(time_interval));

    float diameter = 2.0f+sinf(time_interval/3.0f);

    Coord3D pos;
    for (pos.x=0; pos.x<layerV->size.x; pos.x++) {
        for (pos.y=0; pos.y<layerV->size.y; pos.y++) {
            for (pos.z=0; pos.z<layerV->size.z; pos.z++) {
                float d = distance(pos.x, pos.y, pos.z, origin.x, origin.y, origin.z);

                if (d>diameter && d<diameter + 1.0) {
                  layerV->setRGB(pos, (CRGB)CHSV( millis()/50 + random8(64), 200, 255));
                }
            }
        }
    }
  }
}; // SphereMove3DEffect

class WaveEffect: public Node {
public:

  static const char * name() {return "Wave 🔥💫";}
  static uint8_t dim() {return _1D;}
  static const char * tags() {return "";}

  uint8_t bpm = 60;
  uint8_t fade = 20;
  uint8_t type = 0;

  void setup() override {
    addControl(bpm, "bpm", "range");
    addControl(fade, "fade", "range");
    JsonObject property = addControl(type, "type", "select"); 
    JsonArray values = property["values"].to<JsonArray>();
    values.add("Saw");
    values.add("Triangle");
    values.add("Sinus");
    values.add("Square");
  }

  void loop() override {
    layerV->fadeToBlackBy(fade); //should only fade rgb ...

    CRGB color = CHSV( millis()/50, 255, 255);

    uint8_t prevPos = layerV->size.x/2; //somewhere in the middle

    for (uint8_t y = 0; y<layerV->size.y; y++) {
      uint8_t pos = 0;

      //delay over y-axis..timebase ...
      switch (type) {
        case 0: pos = ::map(beat8(bpm, y*100), 0, UINT8_MAX, 0, layerV->size.x-1 ); break;
        case 1: pos = ::map(triangle8(bpm, y*100), 0, UINT8_MAX, 0, layerV->size.x-1 ); break;
        case 2: pos = beatsin8( bpm, 0, layerV->size.x-1, y * 100 ); break;
        case 3: pos = beat8(bpm, y*100) > 128? 0 : layerV->size.x-1; break;
        // case 3: pos = ::map(_time(bpm), 0, UINT16_MAX, 0, layerV->size.x-1 ); break;
        default: pos = 0;
      }

      //connect saw and square
      if (abs(prevPos - pos) > layerV->size.x / 2) {
        for (uint8_t x=0; x<layerV->size.x; x++)
          layerV->setRGB({x, y, 0}, color);
      }

      layerV->setRGB({pos, y, 0}, color); //= CRGB(255, random8(), 0);
      prevPos = pos;
    }
  }
};

class MHTroy15Effect: public Node {
  public:

  static const char * name() {return "MHTroy15 🔥♫💫🐺";}

  //set default values here
  uint8_t bpm = 30;
  uint8_t pan = 175;
  uint8_t tilt = 90;
  uint8_t zoom = 20;
  uint8_t range = 20;
  uint8_t colorwheel = 0;
  uint8_t colorwheelbrightness = 255; // 0-255, 0 = off, 255 = full brightness
  time_t cooldown = millis();
  
  bool autoMove = true;
  bool audioReactive = true;
  bool invert = true;
  int closestColorIndex = -1;

  std::vector<CRGB> colorwheelpalette = {
      CRGB(255, 255, 255), // White
      CRGB(255, 0, 0),     // Red
      CRGB(0, 255, 0),     // Green
      CRGB(0, 0, 255),     // Blue
      CRGB(255, 255, 0),   // Yellow
      CRGB(0, 255, 255),   // Cyan
      CRGB(255, 165, 0),   // Orange
      CRGB(128, 0, 128)    // Purple
  };

  void setup() override {
    addControl(bpm, "bpm", "range");
    addControl(pan, "pan", "range");
    addControl(tilt, "tilt", "range");
    addControl(zoom, "zoom", "range");
    addControl(colorwheel, "colorwheel", "range", 0, 7); // 0-7 for 8 colors in the colorwheel
    addControl(colorwheelbrightness, "colorwheelbrightness", "range"); // 0-7 for 8 colors in the colorwheel
    addControl(autoMove, "autoMove", "checkbox");
    addControl(range, "range", "range");
    addControl(audioReactive, "audioReactive", "checkbox");
    addControl(invert, "invert", "checkbox");
  }

  // Function to compute Euclidean distance between two colors
  double colorDistance(const CRGB& c1, const CRGB& c2) {
      return std::sqrt(std::pow(c1.r - c2.r, 2) + std::pow(c1.g - c2.g, 2) + std::pow(c1.b - c2.b, 2));
  }

  // Function to find the index of the closest color
  int findClosestColorWheelIndex(const CRGB& inputColor, const std::vector<CRGB>& palette) {
      int closestIndex = 0;
      double minDistance = colorDistance(inputColor, palette[0]);

      for (size_t i = 1; i < palette.size(); ++i) {
          double distance = colorDistance(inputColor, palette[i]);
          if (distance < minDistance) {
              minDistance = distance;
              closestIndex = static_cast<int>(i);
          }
      }

      return closestIndex;
  }

  void loop() override {

    for (uint8_t x=0; x<layerV->size.x; x++) { // loop over lights defined in layout
      if (audioReactive) {
        layerV->setRGB(x, CRGB(audio.bands[15],audio.bands[7],audio.bands[0]));
        if (audio.bands[2] > 200 && cooldown + 3000 < millis()) { //cooldown for 3 seconds
          cooldown = millis();
          colorwheel = random8(8)*5; //random colorwheel index and convert to 0-35 range
        } 
        layerV->setGobo(x, colorwheel);
        layerV->setBrightness2(x, (audio.bands[0]>200)?audio.bands[0]:0); // Use the first band for brightness
        layerV->setZoom(x, (audio.bands[0]>200)?0:128);
        uint8_t mypan = beatsin8(bpm, pan-range, pan+range, 0, audio.bands[0]/2);
        uint8_t mytilt = beatsin8(bpm, tilt-range, tilt+range, 0, audio.bands[0]/2);
        if (invert && x%2==0) {
          mypan = 255 - mypan; // invert pan
          mytilt = 255 - mytilt; // invert tilt
        }
        layerV->setPan(x, mypan); 
        layerV->setTilt(x, mytilt);
        layerV->setBrightness(x, (audio.bands[0]>200)?0:layerV->layerP->lights.header.brightness);
      } else {
        layerV->setRGB(x, CHSV( beatsin8(10), 255, 255));
        layerV->setGobo(x,colorwheel);
        layerV->setBrightness2(x, colorwheelbrightness); // layerV->layerP->lights.header.brightness);
        layerV->setPan(x, autoMove?beatsin8(bpm, pan-range, pan + range, 0,  (invert && x%2==0)?128:0): pan); //if automove, pan the light over a range
        layerV->setTilt(x, autoMove?beatsin8(bpm, tilt - range, tilt + range, 0,  (invert && x%2==0)?128:0): tilt);
        // layerV->setBrightness(x, layerV->layerP->lights.header.brightness); // done automatically
      }
    }
  }
};

class MHTroy32Effect: public Node {
  public:

  static const char * name() {return "MHTroy32 🔥♫💫🐺";}

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
    addControl(bpm, "bpm", "range");
    addControl(pan, "pan", "range");
    addControl(tilt, "tilt", "range");
    addControl(zoom, "zoom", "range");
    addControl(cutin, "cutin", "range");
    addControl(autoMove, "autoMove", "checkbox");
    addControl(range, "range", "range");
    addControl(audioReactive, "audioReactive", "checkbox");
    addControl(invert, "invert", "checkbox");
  }

  // Function to compute Euclidean distance between two colors
  double colorDistance(const CRGB& c1, const CRGB& c2) {
      return std::sqrt(std::pow(c1.r - c2.r, 2) + std::pow(c1.g - c2.g, 2) + std::pow(c1.b - c2.b, 2));
  }

  // Function to find the index of the closest color
  int findClosestColorWheelIndex(const CRGB& inputColor, const std::vector<CRGB>& palette) {
      int closestIndex = 0;
      double minDistance = colorDistance(inputColor, palette[0]);
      for (size_t i = 1; i < palette.size(); ++i) {
          double distance = colorDistance(inputColor, palette[i]);
          if (distance < minDistance) {
              minDistance = distance;
              closestIndex = static_cast<int>(i);
          }
      }
      return closestIndex;
  }

  void loop() override {

    for (uint8_t x=0; x<layerV->size.x; x++) { // loop over lights defined in layout
      if (audioReactive) {
        layerV->setRGB(x, CRGB(audio.bands[15]>cutin?audio.bands[15]:0,audio.bands[7]>cutin?audio.bands[7]:0,audio.bands[0]));
        layerV->setRGB1(x, CRGB(audio.bands[15],audio.bands[7]>cutin?audio.bands[7]:0,audio.bands[0]>cutin?audio.bands[0]:0));
        layerV->setRGB2(x, CRGB(audio.bands[15]>cutin?audio.bands[15]:0,audio.bands[7],audio.bands[0]>cutin?audio.bands[0]:0));
        layerV->setRGB3(x, CRGB(audio.bands[15]>cutin?::map(audio.bands[15],cutin-1,255,0,255):0,audio.bands[7]>cutin?::map(audio.bands[7],cutin-1,255,0,255):0,audio.bands[0]>cutin?::map(audio.bands[0],cutin-1,255,0,255):0));
        if (audio.bands[0] > cutin) {
          layerV->setZoom(x, 255);
          cooldown = millis();
        } else if (cooldown + 5000 < millis()) {
          layerV->setZoom(x, 0);
          cooldown = millis();
        }
        // layerV->setZoom(x, (audio.bands[0]>cutin)?255:0);
        uint8_t mypan = beatsin8(bpm, pan-range, pan+range, 0, audio.bands[0]/2);
        uint8_t mytilt = beatsin8(bpm, tilt-range, tilt+range, 0, audio.bands[0]/2);
        if (invert && x%2==0) {
          mypan = 255 - mypan; // invert pan
          mytilt = 255 - mytilt; // invert tilt
        }
        if (audio.bands[0]+audio.bands[7]+audio.bands[15] > 1) {
          layerV->setPan(x, mypan); 
          layerV->setTilt(x, mytilt);
          layerV->setBrightness(x, 255);
        } else {
          layerV->setBrightness(x, 0);
        }
      } else {
        layerV->setRGB(x, CHSV( beatsin8(10), 255, 255));
        layerV->setPan(x, autoMove?beatsin8(bpm, pan-range, pan + range, 0,  (invert && x%2==0)?128:0): pan); //if automove, pan the light over a range
        layerV->setTilt(x, autoMove?beatsin8(bpm, tilt - range, tilt + range, 0,  (invert && x%2==0)?128:0): tilt);
        // layerV->setBrightness(x, layerV->layerP->lights.header.brightness); // done automatically
      }
    }
  }
};

class MHWowiEffect: public Node {
  public:

  static const char * name() {return "MHWowi 🔥♫💫";}

  uint8_t bpm = 30;
  uint8_t pan = 175;
  uint8_t tilt = 90;
  uint8_t zoom = 20;
  uint8_t range = 20;;
  bool autoMove = true;
  bool audioReactive = true;
  bool invert = true;

  void setup() override {
    addControl(bpm, "bpm", "range");
    addControl(pan, "pan", "range");
    addControl(tilt, "tilt", "range");
    addControl(zoom, "zoom", "range");
    addControl(autoMove, "autoMove", "checkbox");
    addControl(range, "range", "range");
    addControl(audioReactive, "audioReactive", "checkbox");
    addControl(invert, "invert", "checkbox");
  }

  void loop() override {

    layerV->fadeToBlackBy(50);

    for (uint8_t x=0; x<layerV->size.x; x++) { // loop over lights defined in layout

      if (audioReactive) {
        uint8_t nrOfLights = layerV->size.x * 3;
        uint8_t  delta = 256 / nrOfLights;

        //set the 3 led groups for each moving head light
        layerV->setRGB({x,0,0}, CHSV( (x) * delta, 255, audio.bands[(x * 3) % 16]));
        layerV->setRGB1({x,0,0}, CHSV( (x + 3) * delta, 255, audio.bands[(x * 3 + 1) % 16]));
        layerV->setRGB2({x,0,0}, CHSV( (x + 6) * delta, 255, audio.bands[(x * 3 + 2) % 16]));
      } else {
        if (x == beatsin8(bpm, 0, layerV->size.x - 1)) { //sinelon over moving heads
          layerV->setRGB({x,0,0}, CHSV( beatsin8(10), 255, 255)); //colorwheel 10 times per minute
          layerV->setRGB1({x,0,0}, CHSV( beatsin8(10), 255, 255)); //colorwheel 10 times per minute
          layerV->setRGB2({x,0,0}, CHSV( beatsin8(10), 255, 255)); //colorwheel 10 times per minute
        }
      }

      layerV->setPan({x,0,0}, autoMove?beatsin8(bpm, pan-range, pan + range, 0,  (invert && x%2==0)?128:0): pan); //if automove, pan the light over a range
      layerV->setTilt({x,0,0}, autoMove?beatsin8(bpm, tilt - range, tilt + range, 0,  (invert && x%2==0)?128:0): tilt);
      layerV->setZoom({x,0,0}, zoom);
      // layerV->setBrightness({x,0,0}, layerV->layerP->lights.header.brightness); // done automatically
    }
  }
};

#endif