/**
    @title     MoonLight
    @file      E_MoonLight.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

class SolidEffect : public Node {
 public:
  static const char* name() { return "Solid"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🔥"; }

  uint8_t red = 182;
  uint8_t green = 15;
  uint8_t blue = 98;
  uint8_t white = 0;
  uint8_t brightness = 255;

  void setup() override {
    addControl(red, "red", "slider");
    addControl(green, "green", "slider");
    addControl(blue, "blue", "slider");
    addControl(white, "white", "slider");
    addControl(brightness, "brightness", "slider");
  }

  void loop() override {
    layer->fill_solid(CRGB(red * brightness / 255, green * brightness / 255, blue * brightness / 255));
    if (layerP.lights.header.offsetWhite != UINT8_MAX && white > 0)
      for (int index = 0; index < layer->nrOfLights; index++) layer->setWhite(index, white * brightness / 255);
  }
};

// by limpkin
class FixedRectangleEffect : public Node {
 public:
  static const char* name() { return "Fixed Rectangle"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥"; }

  uint8_t red = 182;
  uint8_t green = 15;
  uint8_t blue = 98;
  uint8_t white = 0;
  uint16_t width = 1;
  uint16_t x = 0;
  uint16_t height = 1;
  uint16_t y = 0;
  uint16_t depth = 1;
  uint16_t z = 0;
  bool alternateWhite = false;  // to be used for frontlight

  void setup() override {
    addControl(red, "red", "slider");
    addControl(green, "green", "slider");
    addControl(blue, "blue", "slider");
    addControl(white, "white", "slider");
    addControl(x, "X position", "slider", 0);
    addControl(y, "Y position", "slider", 0);
    addControl(z, "Z position", "slider", 0);
    addControl(width, "Rectangle width", "slider", 1);
    addControl(height, "Rectangle height", "slider", 1);
    addControl(depth, "Rectangle depth", "slider", 1);
    addControl(alternateWhite, "alternateWhite", "checkbox");
  }

  bool alternate = true;

  void loop() override {
    alternate = false;
    layer->fadeToBlackBy(10);  // cleanup old leds if changing
    Coord3D pos = {0, 0, 0};
    for (pos.z = z; pos.z < MIN(z + depth, layer->size.z); pos.z++) {
      for (pos.y = y; pos.y < MIN(y + height, layer->size.y); pos.y++) {
        for (pos.x = x; pos.x < MIN(x + width, layer->size.x); pos.x++) {
          if (alternateWhite && alternate)
            layer->setRGB(pos, CRGB::White);
          else
            layer->setRGB(pos, CRGB(red, green, blue));
          if (white > 0) layer->setWhite(pos, white);
          if (height < width) alternate = !alternate;
        }
        if (height > width) alternate = !alternate;
      }
    }
  }
};

  // BouncingBalls inspired by WLED
  #define maxNumBalls 16
// each needs 12 bytes
struct Ball {
  unsigned long lastBounceTime;
  float impactVelocity;
  float height;
};

class LinesEffect : public Node {
 public:
  static const char* name() { return "Lines"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥"; }

  uint8_t bpm = 120;

  void setup() override { addControl(bpm, "bpm", "slider"); }

  void loop() override {
    int frameNr = 0;

    layer->fadeToBlackBy(255);

    Coord3D pos = {0, 0, 0};
    pos.x = ::map(beat16(bpm), 0, UINT16_MAX, 0, layer->size.x);  // instead of call%width

    for (pos.y = 0; pos.y < layer->size.y; pos.y++) {
      int colorNr = (frameNr / layer->size.y) % 3;
      layer->setRGB(pos, colorNr == 0 ? CRGB::Red : colorNr == 1 ? CRGB::Green : CRGB::Blue);
    }

    pos = {0, 0, 0};
    pos.y = ::map(beat16(bpm), 0, UINT16_MAX, 0, layer->size.y);  // instead of call%height
    for (pos.x = 0; pos.x < layer->size.x; pos.x++) {
      int colorNr = (frameNr / layer->size.x) % 3;
      layer->setRGB(pos, colorNr == 0 ? CRGB::Red : colorNr == 1 ? CRGB::Green : CRGB::Blue);
    }
    (frameNr)++;
  }
};

class RandomEffect : public Node {
 public:
  static const char* name() { return "Random"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🔥🎨"; }

  uint8_t fade = 70;
  void setup() { addControl(fade, "fade", "slider"); }
  void loop() override {
    layer->fadeToBlackBy(fade);
    layer->setRGB(random16(layer->nrOfLights), ColorFromPalette(layerP.palette, random8()));
  }
};

class RipplesEffect : public Node {
 public:
  static const char* name() { return "Ripples"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🔥"; }

  uint8_t speed = 50;
  uint8_t interval = 128;

  void setup() override {
    addControl(speed, "speed", "slider");
    addControl(interval, "interval", "slider");
  }

  void loop() override {
    float ripple_interval = 1.3f * ((255.0f - interval) / 128.0f) * sqrtf(layer->size.y);
    float time_interval = millis() / (100.0 - speed) / ((256.0f - 128.0f) / 20.0f);

    layer->fadeToBlackBy(255);

    Coord3D pos = {0, 0, 0};
    for (pos.z = 0; pos.z < layer->size.z; pos.z++) {
      for (pos.x = 0; pos.x < layer->size.x; pos.x++) {
        float d = distance(layer->size.x / 2.0f, layer->size.z / 2.0f, 0.0f, (float)pos.x, (float)pos.z, 0.0f) / 9.899495f * layer->size.y;
        pos.y = floor(layer->size.y / 2.0f * (1 + sinf(d / ripple_interval + time_interval)));  // between 0 and layer->size.y

        layer->setRGB(pos, (CRGB)CHSV(millis() / 50 + random8(64), 200, 255));
      }
    }
  }
};

  #if USE_M5UNIFIED
    #include <M5Unified.h>
  #endif

class ScrollingTextEffect : public Node {
 public:
  static const char* name() { return "Scrolling Text"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥"; }

  char textIn[32];
  Char<32> text;  // = Char<32>("MoonLight");
  uint8_t speed = 0;
  uint8_t font = 0;
  uint8_t preset = 0;
  Coord3D ctest;

  void setup() override {
    addControl(font, "font", "select");
    addControlValue("4x6");
    addControlValue("5x8");
    addControlValue("5x12");
    addControlValue("6x8");
    addControlValue("7x9");

    addControl(preset, "preset", "select");
    addControlValue("Auto");
    addControlValue("Text");
    addControlValue("IP");
    addControlValue("FPS");
    addControlValue("Time");
    addControlValue("Up");
    addControlValue("Status 🛜");
    addControlValue("Clients 🛜");
    addControlValue("Connected 🛜");
    addControlValue("Free memory");

    addControl(textIn, "text", "text", 1, sizeof(textIn));  // size needed to protect char array!

    addControl(speed, "speed", "slider");
  }

  void loop() override {
    layer->fadeToBlackBy();

    uint8_t choice;
    if (preset > 0)  // not auto
      choice = preset;
    else {
      if (strlen(textIn) == 0)
        choice = (millis() / 1000 % 8) + 2;
      else
        choice = (millis() / 1000 % 9) + 1;
    }

    IPAddress activeIP = WiFi.isConnected() ? WiFi.localIP() : ETH.localIP();
    switch (choice) {
    case 1:
      text.format("%s", textIn);
      break;
    case 2:
      text.format(".%d", activeIP[3]);
      break;
    case 3:
      text.format("%ds", sharedData.fps);
      break;
    case 4:
      text.formatTime(time(nullptr), "%H%M");
      break;
    case 5:
  #define MILLIS_PER_MINUTE (60 * 1000)
  #define MILLIS_PER_HOUR (MILLIS_PER_MINUTE * 60)
  #define MILLIS_PER_DAY (MILLIS_PER_HOUR * 24)
      if (millis() < MILLIS_PER_MINUTE)                                                         // within 1 minute
        text.format("%ds", millis() / 1000);                                                    // seconds
      else if (millis() < MILLIS_PER_MINUTE * 10)                                               // within 10 min
        text.format("%dm%d", millis() / MILLIS_PER_MINUTE, (millis() / 1000) % 60);             // minutes and seconds
      else if (millis() < MILLIS_PER_HOUR)                                                      // within 1 hour
        text.format("%dm", millis() / MILLIS_PER_MINUTE);                                       // minutes
      else if (millis() < MILLIS_PER_HOUR * 10)                                                 // within 10 hours
        text.format("%dh%d", millis() / MILLIS_PER_HOUR, (millis() / MILLIS_PER_MINUTE) % 60);  // hours and minutes
      else if (millis() < MILLIS_PER_DAY)                                                       // within 1 day
        text.format("%dh", millis() / MILLIS_PER_HOUR);                                         // hours
      else if (millis() < MILLIS_PER_DAY * 10)                                                  // within 10 days
        text.format("%dd%d", millis() / (MILLIS_PER_DAY), (millis() / MILLIS_PER_HOUR) % 24);   // days and hours
      else                                                                                      // more than 10 days
        text.format("%dh", millis() / (MILLIS_PER_DAY));                                        // days
      break;
    case 6:
      text.format("%s", sharedData.connectionStatus == 0 ? "Off" : sharedData.connectionStatus == 1 ? "AP-" : sharedData.connectionStatus == 2 ? "AP+" : sharedData.connectionStatus == 3 ? "Sta-" : sharedData.connectionStatus == 4 ? "Sta+" : "mqqt");
      break;
    case 7:
      text.format("%dC", sharedData.clientListSize);
      break;
    case 8:
      text.format("%dCC", sharedData.connectedClients);
      break;
    case 9:
      text.format("%dKB", ESP.getFreeHeap() / 1024);
      break;
    }
    layer->setRGB(Coord3D(choice - 1), CRGB::Blue);

    // EVERY_N_SECONDS(1)
    //   Serial.printf(" %d:%s", choice-1, text.c_str());

    // if (text && strnlen(text.c_str(), 2) > 0) {
    layer->drawText(text.c_str(), 0, 1, font, CRGB::Red, -(millis() / 25 * speed / 256));  // instead of call
    // }

  #if USE_M5UNIFIEDDisplay
    M5.Display.fillRect(0, 0, 100, 15, BLACK);
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(2);
    M5.Display.drawString(text.c_str(), 0, 0);
  #endif
  }
};  // ScrollingText

// AI generated
class SinusEffect : public Node {
 public:
  static const char* name() { return "Sinus"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🔥"; }

  uint8_t speed = 5;

  void setup() override { addControl(speed, "speed", "slider"); }

  void loop() override {
    layer->fadeToBlackBy(70);

    uint8_t hueOffset = millis() / 10;
    static uint16_t phase = 0;  // Tracks the phase of the sine wave
    uint8_t brightness = 255;

    for (uint16_t i = 0; i < layer->nrOfLights; i++) {
      // Calculate the sine wave value for the current LED
      uint8_t wave = sin8((i * 255 / layer->nrOfLights) + phase);
      // Map the sine wave value to a color hue
      uint8_t hue = wave + hueOffset;
      // Set the LED color using the calculated hue
      layer->setRGB(i, (CRGB)CHSV(hue, 255, brightness));
    }

    // Increment the phase to animate the wave
    phase += speed;
  }
};

class SphereMoveEffect : public Node {
 public:
  static const char* name() { return "Sphere Move"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🔥"; }

  uint8_t speed = 50;

  void setup() override { addControl(speed, "speed", "slider", 0, 99); }

  void loop() override {
    layer->fadeToBlackBy(255);

    float time_interval = millis() / (100 - speed) / ((256.0f - 128.0f) / 20.0f);

    Coord3D origin;
    origin.x = layer->size.x / 2.0 * (1.0 + sinf(time_interval));
    origin.y = layer->size.y / 2.0 * (1.0 + cosf(time_interval));
    origin.z = layer->size.z / 2.0 * (1.0 + cosf(time_interval));

    float diameter = 2.0f + sinf(time_interval / 3.0f);

    Coord3D pos;
    for (pos.x = 0; pos.x < layer->size.x; pos.x++) {
      for (pos.y = 0; pos.y < layer->size.y; pos.y++) {
        for (pos.z = 0; pos.z < layer->size.z; pos.z++) {
          float d = distance(pos.x, pos.y, pos.z, origin.x, origin.y, origin.z);

          if (d > diameter && d < diameter + 1.0) {
            layer->setRGB(pos, (CRGB)CHSV(millis() / 50 + random8(64), 200, 255));
          }
        }
      }
    }
  }
};  // SphereMoveEffect

// by @Brandon502
class StarFieldEffect : public Node {  // Inspired by Daniel Shiffman's Coding Train https://www.youtube.com/watch?v=17WoOqgXsRM
 public:
  static const char* name() { return "StarField"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥🎨"; }

  struct Star {
    int x, y, z;
    uint8_t colorIndex;
  };

  static float fmap(const float x, const float in_min, const float in_max, const float out_min, const float out_max) { return (out_max - out_min) * (x - in_min) / (in_max - in_min) + out_min; }

  uint8_t speed = 20;
  uint8_t numStars = 16;
  uint8_t blur = 128;
  bool usePalette = false;

  void setup() override {
    addControl(speed, "speed", "slider", 0, 30);
    addControl(numStars, "numStars", "slider", 1, 255);
    addControl(blur, "blur", "slider", 0, 255);
    addControl(usePalette, "usePalette", "checkbox");

    // set up all stars
    for (int i = 0; i < 255; i++) {
      stars[i].x = random(-layer->size.x, layer->size.x);
      stars[i].y = random(-layer->size.y, layer->size.y);
      stars[i].z = random(layer->size.x);
      stars[i].colorIndex = random8();
    }
  }

  unsigned long step;
  Star stars[255];

  void loop() override {
    if (!speed || millis() - step < 1000 / speed) return;  // Not enough time passed

    layer->fadeToBlackBy(blur);

    for (int i = 0; i < numStars; i++) {
      // update star
      //  EXT_LOGD(ML_TAG, "Star %d Pos: %d, %d, %d -> ", i, stars[i].x, stars[i].y, stars[i].z);
      float sx = layer->size.x / 2.0 + fmap(float(stars[i].x) / stars[i].z, 0, 1, 0, layer->size.x / 2.0);
      float sy = layer->size.y / 2.0 + fmap(float(stars[i].y) / stars[i].z, 0, 1, 0, layer->size.y / 2.0);

      // EXT_LOGD(ML_TAG, " %f, %f\n", sx, sy);

      Coord3D pos = Coord3D(sx, sy);
      if (!pos.isOutofBounds(layer->size)) {
        if (usePalette)
          layer->setRGB(Coord3D(sx, sy), ColorFromPalette(layerP.palette, stars[i].colorIndex, ::map(stars[i].z, 0, layer->size.x, 255, 150)));
        else {
          uint8_t color = ::map(stars[i].colorIndex, 0, 255, 120, 255);
          int brightness = ::map(stars[i].z, 0, layer->size.x, 7, 10);
          color *= brightness / 10.0;
          layer->setRGB(Coord3D(sx, sy), CRGB(color, color, color));
        }
      }
      stars[i].z -= 1;
      if (stars[i].z <= 0 || pos.isOutofBounds(layer->size)) {
        stars[i].x = random(-layer->size.x, layer->size.x);
        stars[i].y = random(-layer->size.y, layer->size.y);
        stars[i].z = layer->size.x;
        stars[i].colorIndex = random8();
      }
    }

    step = millis();
  }
};  // StarFieldEffect

// BY MONSOONO / @Flavourdynamics
class PraxisEffect : public Node {
 public:
  static const char* name() { return "Praxis"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥🎨"; }

  uint8_t macroMutatorFreq = 3;
  uint8_t macroMutatorMin = 250;
  uint8_t macroMutatorMax = 255;
  uint8_t microMutatorFreq = 4;
  uint8_t microMutatorMin = 200;
  uint8_t microMutatorMax = 255;

  void setup() override {
    addControl(macroMutatorFreq, "macroMutatorFreq", "slider", 0, 15);
    addControl(macroMutatorMin, "macroMutatorMin", "slider", 0, 255);
    addControl(macroMutatorMax, "macroMutatorMax", "slider", 0, 255);
    addControl(microMutatorFreq, "microMutatorFreq", "slider", 0, 15);
    addControl(microMutatorMin, "microMutatorMin", "slider", 0, 255);
    addControl(microMutatorMax, "microMutatorMax", "slider", 0, 255);
    // ui->initSlider(parentVar, "hueSpeed", layer->effectData.write<uint8_t>(20), 1, 100); // (14), 1, 255)
    // ui->initSlider(parentVar, "saturation", layer->effectData.write<uint8_t>(255), 0, 255);
  }

  void loop() override {
    // uint8_t huespeed = layer->effectData.read<uint8_t>();
    // uint8_t saturation = layer->effectData.read<uint8_t>(); I will revisit this when I have a display

    uint16_t macro_mutator = beatsin16(macroMutatorFreq, macroMutatorMin << 8, macroMutatorMax << 8);  // beatsin16(14, 65350, 65530);
    uint16_t micro_mutator = beatsin16(microMutatorFreq, microMutatorMin, microMutatorMax);            // beatsin16(2, 550, 900);
    // uint16_t macro_mutator = beatsin8(macroMutatorFreq, macroMutatorMin, macroMutatorMax); // beatsin16(14, 65350, 65530);
    // uint16_t micro_mutator = beatsin8(microMutatorFreq, microMutatorMin, microMutatorMax); // beatsin16(2, 550, 900);

    Coord3D pos = {0, 0, 0};
    uint8_t huebase = millis() / 40;  // 1 + ~huespeed

    for (pos.x = 0; pos.x < layer->size.x; pos.x++) {
      for (pos.y = 0; pos.y < layer->size.y; pos.y++) {
        // uint8_t hue = huebase + (-(pos.x+pos.y)*macro_mutator*10) + ((pos.x+pos.x*pos.y*(macro_mutator*256))/(micro_mutator+1));
        uint8_t hue = huebase + ((pos.x + pos.y * macro_mutator * pos.x) / (micro_mutator + 1));
        // uint8_t hue = huebase + ((pos.x+pos.y)*(250-macro_mutator)/5) + ((pos.x+pos.y*macro_mutator*pos.x)/(micro_mutator+1)); Original
        CRGB colour = ColorFromPalette(layerP.palette, hue, 255);
        layer->setRGB(pos, colour);  // blend(layer->getRGB(pos), colour, 155);
      }
    }
  }
};  // Praxis

class WaveEffect : public Node {
 public:
  static const char* name() { return "Wave"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥"; }

  uint8_t bpm = 60;
  uint8_t fade = 20;
  uint8_t type = 0;

  void setup() override {
    addControl(bpm, "bpm", "slider");
    addControl(fade, "fade", "slider");
    addControl(type, "type", "select");
    addControlValue("Saw");
    addControlValue("Triangle");
    addControlValue("Sinus");
    addControlValue("Square");
    addControlValue("Sin3");  // with @pathightree
    addControlValue("Noise");
  }

  void loop() override {
    layer->fadeToBlackBy(fade);  // should only fade rgb ...

    CRGB color = CHSV(millis() / 50, 255, 255);

    int prevPos = layer->size.x / 2;  // somewhere in the middle

    for (int y = 0; y < layer->size.y; y++) {
      int pos = 0;

      uint8_t b8 = beat8(bpm, y * 100);
      uint8_t bs8 = beatsin8(bpm, 0, 255, y * 100);
      // delay over y-axis..timebase ...
      switch (type) {
      case 0:
        pos = b8 * layer->size.x / 256;
        break;
      case 1:
        pos = triangle8(bpm, y * 100) * layer->size.x / 256;
        break;
      case 2:
        pos = bs8 * layer->size.x / 256;
        break;
      case 3:
        pos = b8 > 128 ? 0 : layer->size.x - 1;
        break;
      case 4:
        pos = (bs8 + beatsin8(bpm * 0.65, 0, 255, y * 200) + beatsin8(bpm * 1.43, 0, 255, y * 300)) * layer->size.x / 256 / 3;
        break;
      case 5:
        pos = inoise8(millis() * bpm / 256 + y * 1000) * layer->size.x / 256;
        break;  // bpm not really bpm, more speed
      default:
        pos = 0;
      }

      // connect saw and square
      if ((type == 0 || type == 3) && abs(prevPos - pos) > layer->size.x / 2) {
        for (int x = 0; x < layer->size.x; x++) layer->setRGB(Coord3D(x, y), color);
      }

      layer->setRGB(Coord3D(pos, y), color);  //= CRGB(255, random8(), 0);
      prevPos = pos;
    }
  }
};

class FreqSawsEffect : public Node {
 public:
  static const char* name() { return "Frequency Saws"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥♫🎨🪚"; }

  uint8_t fade = 4;
  uint8_t increaser = 211;
  uint8_t decreaser = 18;
  uint8_t bpmMax = 198;
  bool invert = false;
  bool keepOn = false;
  uint8_t method = 2;

  void setup() override {
    addControl(fade, "fade", "slider");
    addControl(increaser, "increaser", "slider");
    addControl(decreaser, "decreaser", "slider");
    addControl(bpmMax, "bpmMax", "slider");
    addControl(invert, "invert", "checkbox");
    addControl(keepOn, "keepOn", "checkbox");
    addControl(method, "method", "select");
    addControlValue("Chaos");
    addControlValue("Chaos fix");
    addControlValue("BandPhases");

    memset(bandSpeed, 0, sizeof(bandSpeed));
    memset(bandPhase, 0, sizeof(bandPhase));
    memset(lastBpm, 0, sizeof(lastBpm));
    memset(phaseOffset, 0, sizeof(phaseOffset));

    lastTime = millis();
  }

  uint16_t bandSpeed[NUM_GEQ_CHANNELS];
  uint16_t bandPhase[NUM_GEQ_CHANNELS];   // Track phase position for each band
  uint8_t lastBpm[NUM_GEQ_CHANNELS];      // For beat8 continuity tracking
  uint8_t phaseOffset[NUM_GEQ_CHANNELS];  // Phase offset for beat8 sync
  unsigned long lastTime;                 // For time-based phase calculation

  void loop() override {
    layer->fadeToBlackBy(fade);
    // Update timing for frame-rate independent phase
    unsigned long currentTime = millis();
    uint32_t deltaMs = currentTime - lastTime;
    lastTime = currentTime;

    for (int x = 0; x < layer->size.x; x++) {                          // x-axis (column)
      uint8_t band = ::map(x, 0, layer->size.x, 0, NUM_GEQ_CHANNELS);  // the frequency band applicable for the column, skip the lowest and the highest
      uint8_t volume = sharedData.bands[band];                         // the volume for the frequency band
      // Target speed based on current volume
      uint16_t targetSpeed = (volume * increaser * 257);

      if (volume > 0) {
        bandSpeed[band] = max(bandSpeed[band], targetSpeed);
      } else {
        // Calculate decay amount based on time to reach zero
        if (decreaser > 0 && bandSpeed[band] > 0) {
          uint32_t decayAmount = MAX((bandSpeed[band] * deltaMs) / (decreaser * 10UL), 1);  // *10 to scale decreaser range 0-255 to 0-2550ms
          if (decayAmount >= bandSpeed[band]) {
            bandSpeed[band] = 0;
          } else {
            bandSpeed[band] -= decayAmount;
          }
        }
      }

      if (bandSpeed[band] > 1 || keepOn) {                               // for some reason bandSpeed[band] doesn't reach 0 ... WIP
        uint8_t bpm = ::map(bandSpeed[band], 0, UINT16_MAX, 0, bpmMax);  // the higher the band speed, the higher the beats per minute.
        uint8_t y;
        if (method == 0) {                                      // chaos
          y = ::map(beat8(bpm), 0, 255, 0, layer->size.y - 1);  // saw wave, running over the y-axis, speed is determined by bpm
        } else if (method == 1) {                               // chaos corrected
          // Maintain phase continuity when BPM changes
          if (bpm != lastBpm[band]) {
            uint8_t currentPos = beat8(lastBpm[band]) + phaseOffset[band];
            uint8_t newPos = beat8(bpm);
            phaseOffset[band] = currentPos - newPos;
            lastBpm[band] = bpm;
          }
          y = ::map(beat8(bpm) + phaseOffset[band], 0, 255, 0, layer->size.y - 1);  // saw wave, running over the y-axis, speed is determined by bpm
        } else if (method == 2) {                                                   // bandphases
          // Time-based phase increment - 1 complete cycle per second at 60 BPM
          uint32_t phaseIncrement = (bpm * deltaMs * 65536UL) / (60UL * 1000UL);
          phaseIncrement /= 2;  // Make it 8x faster - adjust this multiplier as needed
          bandPhase[band] += phaseIncrement;
          y = ::map(bandPhase[band] >> 8, 0, 255, 0, layer->size.y - 1);  // saw wave, running over the y-axis, speed is determined by bpm
        }
        // y-axis shows a saw wave which runs faster if the bandSpeed for the x-column is higher, if rings are used for the y-axis, it will show as turning wheels
        layer->setRGB(Coord3D(x, (invert && x % 2 == 0) ? layer->size.y - 1 - y : y), ColorFromPalette(layerP.palette, ::map(x, 0, layer->size.x - 1, 0, 255)));
      }
    }
  }
};  // FreqSawsEffect

// by WildCats08 / @Brandon502
class RubiksCubeEffect : public Node {
 public:
  static const char* name() { return "Rubik's Cube"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🔥💫"; }

  struct Cube {
    uint8_t SIZE;
    static const uint8_t MAX_SIZE = 8;
    using Face = std::array<std::array<uint8_t, MAX_SIZE>, MAX_SIZE>;
    Face front;
    Face back;
    Face left;
    Face right;
    Face top;
    Face bottom;

    Cube() { init(SIZE); }

    void init(uint8_t cubeSize) {
      SIZE = cubeSize;
      for (int i = 0; i < MAX_SIZE; i++)
        for (int j = 0; j < MAX_SIZE; j++) {
          front[i][j] = 0;
          back[i][j] = 1;
          left[i][j] = 2;
          right[i][j] = 3;
          top[i][j] = 4;
          bottom[i][j] = 5;
        }
    }

    void rotateFace(Face& face, bool clockwise) {
      Face temp = face;
      if (clockwise)
        for (int i = 0; i < SIZE; i++)
          for (int j = 0; j < SIZE; j++) {
            face[j][SIZE - 1 - i] = temp[i][j];
          }
      else
        for (int i = 0; i < SIZE; i++)
          for (int j = 0; j < SIZE; j++) {
            face[SIZE - 1 - j][i] = temp[i][j];
          }
    }

    void rotateRow(int startRow, int stopRow, bool clockwise) {
      std::array<uint8_t, MAX_SIZE> temp;
      for (int row = startRow; row <= stopRow; row++) {
        if (clockwise)
          for (int i = 0; i < SIZE; i++) {
            temp[i] = left[row][i];
            left[row][i] = front[row][i];
            front[row][i] = right[row][i];
            right[row][i] = back[row][i];
            back[row][i] = temp[i];
          }
        else
          for (int i = 0; i < SIZE; i++) {
            temp[i] = left[row][i];
            left[row][i] = back[row][i];
            back[row][i] = right[row][i];
            right[row][i] = front[row][i];
            front[row][i] = temp[i];
          }
      }
    }

    void rotateColumn(int startCol, int stopCol, bool clockwise) {
      std::array<uint8_t, MAX_SIZE> temp;
      for (int col = startCol; col <= stopCol; col++) {
        if (clockwise)
          for (int i = 0; i < SIZE; i++) {
            temp[i] = top[i][col];
            top[i][col] = front[i][col];
            front[i][col] = bottom[i][col];
            bottom[i][col] = back[SIZE - 1 - i][SIZE - 1 - col];
            back[SIZE - 1 - i][SIZE - 1 - col] = temp[i];
          }
        else
          for (int i = 0; i < SIZE; i++) {
            temp[i] = top[i][col];
            top[i][col] = back[SIZE - 1 - i][SIZE - 1 - col];
            back[SIZE - 1 - i][SIZE - 1 - col] = bottom[i][col];
            bottom[i][col] = front[i][col];
            front[i][col] = temp[i];
          }
      }
    }

    void rotateFaceLayer(bool clockwise, int startLayer, int endLayer) {
      for (int layer = startLayer; layer <= endLayer; layer++) {
        std::array<uint8_t, MAX_SIZE> temp;
        for (int i = 0; i < SIZE; i++) temp[i] = clockwise ? top[SIZE - 1 - layer][i] : bottom[layer][i];
        for (int i = 0; i < SIZE; i++) {
          if (clockwise) {
            top[SIZE - 1 - layer][i] = left[SIZE - 1 - i][SIZE - 1 - layer];
            left[SIZE - 1 - i][SIZE - 1 - layer] = bottom[layer][SIZE - 1 - i];
            bottom[layer][SIZE - 1 - i] = right[i][layer];
            right[i][layer] = temp[i];
          } else {
            bottom[layer][SIZE - 1 - i] = left[SIZE - 1 - i][SIZE - 1 - layer];
            left[SIZE - 1 - i][SIZE - 1 - layer] = top[SIZE - 1 - layer][i];
            top[SIZE - 1 - layer][i] = right[i][layer];
            right[i][layer] = temp[SIZE - 1 - i];
          }
        }
      }
    }

    void rotateFront(bool clockwise, uint8_t width) {
      rotateFaceLayer(clockwise, 0, width - 1);
      rotateFace(front, clockwise);
      if (width >= SIZE) rotateFace(back, !clockwise);
    }
    void rotateBack(bool clockwise, uint8_t width) {
      rotateFaceLayer(!clockwise, SIZE - width, SIZE - 1);
      rotateFace(back, clockwise);
      if (width >= SIZE) rotateFace(front, !clockwise);
    }
    void rotateLeft(bool clockwise, uint8_t width) {
      rotateFace(left, clockwise);
      rotateColumn(0, width - 1, !clockwise);
      if (width >= SIZE) rotateFace(right, !clockwise);
    }
    void rotateRight(bool clockwise, uint8_t width) {
      rotateFace(right, clockwise);
      rotateColumn(SIZE - width, SIZE - 1, clockwise);
      if (width >= SIZE) rotateFace(left, !clockwise);
    }
    void rotateTop(bool clockwise, uint8_t width) {
      rotateFace(top, clockwise);
      rotateRow(0, width - 1, clockwise);
      if (width >= SIZE) rotateFace(bottom, !clockwise);
    }
    void rotateBottom(bool clockwise, uint8_t width) {
      rotateFace(bottom, clockwise);
      rotateRow(SIZE - width, SIZE - 1, !clockwise);
      if (width >= SIZE) rotateFace(top, !clockwise);
    }

    void drawCube(VirtualLayer* layer) {
      int sizeX = MAX(layer->size.x - 1, 1);
      int sizeY = MAX(layer->size.y - 1, 1);
      int sizeZ = MAX(layer->size.z - 1, 1);

      // 3 Sided Cube Cheat add 1 to led size if "panels" missing. May affect different fixture types
      if (layer->layerDimension == _3D) {
        if (!layer->isMapped(layer->XYZUnModified(Coord3D(0, layer->size.y / 2, layer->size.z / 2))) || !layer->isMapped(layer->XYZUnModified(Coord3D(layer->size.x - 1, layer->size.y / 2, layer->size.z / 2)))) sizeX++;
        if (!layer->isMapped(layer->XYZUnModified(Coord3D(layer->size.x / 2, 0, layer->size.z / 2))) || !layer->isMapped(layer->XYZUnModified(Coord3D(layer->size.x / 2, layer->size.y - 1, layer->size.z / 2)))) sizeY++;
        if (!layer->isMapped(layer->XYZUnModified(Coord3D(layer->size.x / 2, layer->size.y / 2, 0))) || !layer->isMapped(layer->XYZUnModified(Coord3D(layer->size.x / 2, layer->size.y / 2, layer->size.z - 1)))) sizeZ++;
      }

      // Previously SIZE - 1. Cube size expanded by 2, makes edges thicker. Constrains are used to prevent out of bounds
      const float scaleX = (SIZE + 1.0) / sizeX;
      const float scaleY = (SIZE + 1.0) / sizeY;
      const float scaleZ = (SIZE + 1.0) / sizeZ;

      // Calculate once for optimization
      const int halfX = sizeX / 2;
      const int halfY = sizeY / 2;
      const int halfZ = sizeZ / 2;

      const CRGB COLOR_MAP[] = {CRGB::Red, CRGB::DarkOrange, CRGB::Blue, CRGB::Green, CRGB::Yellow, CRGB::White};

      for (int x = 0; x < layer->size.x; x++)
        for (int y = 0; y < layer->size.y; y++)
          for (int z = 0; z < layer->size.z; z++) {
            Coord3D led = Coord3D(x, y, z);
            if (layer->isMapped(layer->XYZUnModified(led)) == 0) continue;  // skip if not a physical LED

            // Normalize the coordinates to the Rubik's cube range. Subtract 1 since cube expanded by 2
            int normalizedX = constrain(round(x * scaleX) - 1, 0, SIZE - 1);
            int normalizedY = constrain(round(y * scaleY) - 1, 0, SIZE - 1);
            int normalizedZ = constrain(round(z * scaleZ) - 1, 0, SIZE - 1);

            // Calculate the distance to the closest face
            int distX = min(x, sizeX - x);
            int distY = min(y, sizeY - y);
            int distZ = min(z, sizeZ - z);
            int dist = min(distX, min(distY, distZ));

            if (dist == distZ && z < halfZ)
              layer->setRGB(led, COLOR_MAP[front[normalizedY][normalizedX]]);
            else if (dist == distX && x < halfX)
              layer->setRGB(led, COLOR_MAP[left[normalizedY][SIZE - 1 - normalizedZ]]);
            else if (dist == distY && y < halfY)
              layer->setRGB(led, COLOR_MAP[top[SIZE - 1 - normalizedZ][normalizedX]]);
            else if (dist == distZ && z >= halfZ)
              layer->setRGB(led, COLOR_MAP[back[normalizedY][SIZE - 1 - normalizedX]]);
            else if (dist == distX && x >= halfX)
              layer->setRGB(led, COLOR_MAP[right[normalizedY][normalizedZ]]);
            else if (dist == distY && y >= halfY)
              layer->setRGB(led, COLOR_MAP[bottom[normalizedZ][normalizedX]]);
          }
    }
  };

  struct Move {
    uint8_t face;       // 0-5 (3 bits)
    uint8_t width;      // 0-7 (3 bits)
    uint8_t direction;  // 0 or 1 (1 bit)
  };

  Move createRandomMoveStruct(uint8_t cubeSize, uint8_t prevFace) {
    Move move;
    do {
      move.face = random(6);
    } while (move.face / 2 == prevFace / 2);
    move.width = random(cubeSize - 2);
    move.direction = random(2);
    return move;
  }

  uint8_t packMove(Move move) {
    uint8_t packed = (move.face & 0b00000111) | ((move.width << 3) & 0b00111000) | ((move.direction << 6) & 0b01000000);
    return packed;
  }

  Move unpackMove(uint8_t packedMove) {
    Move move;
    move.face = packedMove & 0b00000111;
    move.width = (packedMove >> 3) & 0b00000111;
    move.direction = (packedMove >> 6) & 0b00000001;
    return move;
  }

  // UI control variables
  uint8_t turnsPerSecond = 2;
  uint8_t cubeSize = 3;
  bool randomTurning = false;

  void setup() override {
    addControl(turnsPerSecond, "turnsPerSecond", "slider", 0, 20);
    addControl(cubeSize, "cubeSize", "slider", 1, 8);
    addControl(randomTurning, "randomTurning", "checkbox");
  }

  bool doInit = false;
  void onUpdate(const Char<20>& oldValue, const JsonObject& control) override {
    if (control["name"] == "cubeSize" || control["name"] == "randomTurning") {
      doInit = true;
    }
  }

  unsigned long step;
  Cube cube;
  uint8_t moveList[100];
  uint8_t moveIndex;
  uint8_t prevFaceMoved;

  void init() {
    cube.init(cubeSize);
    uint8_t moveCount = cubeSize * 10 + random(20);
    // Randomly turn entire cube
    for (int x = 0; x < 3; x++) {
      if (random(2)) cube.rotateRight(1, cubeSize);
      if (random(2)) cube.rotateTop(1, cubeSize);
      if (random(2)) cube.rotateFront(1, cubeSize);
    }
    // Generate scramble
    for (int i = 0; i < moveCount; i++) {
      Move move = createRandomMoveStruct(cubeSize, prevFaceMoved);
      prevFaceMoved = move.face;
      moveList[i] = packMove(move);

      (cube.*rotateFuncs[move.face])(move.direction, move.width + 1);
    }

    moveIndex = moveCount - 1;

    cube.drawCube(layer);
  }

  typedef void (Cube::*RotateFunc)(bool direction, uint8_t width);
  RotateFunc rotateFuncs[6] = {&Cube::rotateFront, &Cube::rotateBack, &Cube::rotateLeft, &Cube::rotateRight, &Cube::rotateTop, &Cube::rotateBottom};

  void loop() override {
    if (doInit && millis() > step || step - 3100 > millis()) {  // step - 3100 > millis() temp fix for default on boot
      step = millis() + 1000;
      doInit = false;
      init();
    }

    if (!turnsPerSecond || millis() - step < 1000 / turnsPerSecond || millis() < step) return;

    Move move = randomTurning ? createRandomMoveStruct(cubeSize, prevFaceMoved) : unpackMove(moveList[moveIndex]);

    (cube.*rotateFuncs[move.face])(!move.direction, move.width + 1);

    cube.drawCube(layer);

    if (!randomTurning && moveIndex == 0) {
      step = millis() + 3000;
      doInit = true;
      return;
    }
    if (!randomTurning) moveIndex--;
    step = millis();
  }
};

// by WildCats08 / @Brandon502
class ParticlesEffect : public Node {
 public:
  static const char* name() { return "Particles"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🔥💫🧭🎨"; }

  struct Particle {
    float x, y, z;
    float vx, vy, vz;
    CRGB color;

    void update() {
      x += vx;
      y += vy;
      z += vz;
    }
    void revert() {
      x -= vx;
      y -= vy;
      z -= vz;
    }

    Coord3D toCoord3DRounded() { return Coord3D(round(x), round(y), round(z)); }

    void updatePositionandDraw(VirtualLayer* layer, int particleIndex = 0, bool debugPrint = false) {
      if (debugPrint) EXT_LOGD(ML_TAG, "Particle %d: Pos: %f, %f, %f Velocity: %f, %f, %f\n", particleIndex, x, y, z, vx, vy, vz);

      Coord3D prevPos = toCoord3DRounded();
      if (debugPrint) EXT_LOGD(ML_TAG, "     PrevPos: %d, %d, %d\n", prevPos.x, prevPos.y, prevPos.z);

      update();
      Coord3D newPos = toCoord3DRounded();
      if (debugPrint) EXT_LOGD(ML_TAG, "     NewPos: %d, %d, %d\n", newPos.x, newPos.y, newPos.z);

      if (newPos == prevPos) return;  // Skip if no change in position

      layer->setRGB(prevPos, CRGB::Black);  // Clear previous position

      if (layer->isMapped(layer->XYZUnModified(newPos)) && !newPos.isOutofBounds(layer->size) && layer->getRGB(newPos) == CRGB::Black) {
        if (debugPrint) EXT_LOGD(ML_TAG, "     New Pos was mapped and particle placed\n");
        layer->setRGB(newPos, color);  // Set new position
        return;
      }

      // Particle is not mapped, find nearest mapped pixel
      Coord3D nearestMapped = prevPos;                         // Set nearest to previous position
      unsigned nearestDist = newPos.distanceSquared(prevPos);  // Set distance to previous position
      int diff = 0;                                            // If distance the same check how many coordinates are different (larger is better)
      bool changed = false;

      if (debugPrint) EXT_LOGD(ML_TAG, "     %d, %d, %d, Not Mapped! Nearest: %d, %d, %d dist: %d diff: %d\n", newPos.x, newPos.y, newPos.z, nearestMapped.x, nearestMapped.y, nearestMapped.z, nearestDist, diff);

      // Check neighbors for nearest mapped pixel. This should be changed to check neighbors with similar velocity
      for (int i = -1; i <= 1; i++)
        for (int j = -1; j <= 1; j++)
          for (int k = -1; k <= 1; k++) {
            Coord3D testPos = newPos + Coord3D(i, j, k);
            if (testPos == prevPos) continue;                               // Skip current position
            if (!layer->isMapped(layer->XYZUnModified(testPos))) continue;  // Skip if not mapped
            if (testPos.isOutofBounds(layer->size)) continue;               // Skip out of bounds
            if (layer->getRGB(testPos) != CRGB::Black) continue;            // Skip if already colored by another particle
            unsigned dist = testPos.distanceSquared(newPos);
            int differences = (prevPos.x != testPos.x) + (prevPos.y != testPos.y) + (prevPos.z != testPos.z);
            if (debugPrint) EXT_LOGD(ML_TAG, "     TestPos: %d %d %d Dist: %d Diff: %d", testPos.x, testPos.y, testPos.z, dist, differences);
            if (debugPrint) EXT_LOGD(ML_TAG, "     New Velocities: %d, %d, %d\n", (testPos.x - prevPos.x), (testPos.y - prevPos.y), (testPos.z - prevPos.z));
            if (dist < nearestDist || (dist == nearestDist && differences >= diff)) {
              nearestDist = dist;
              nearestMapped = testPos;
              diff = differences;
              changed = true;
            }
          }
      if (changed) {  // Change velocity to move towards nearest mapped pixel. Update position.
        if (newPos.x != nearestMapped.x) vx = constrain(nearestMapped.x - prevPos.x, -1, 1);
        if (newPos.y != nearestMapped.y) vy = constrain(nearestMapped.y - prevPos.y, -1, 1);
        if (newPos.z != nearestMapped.z) vz = constrain(nearestMapped.z - prevPos.z, -1, 1);

        x = nearestMapped.x;
        y = nearestMapped.y;
        z = nearestMapped.z;

        if (debugPrint) EXT_LOGD(ML_TAG, "     New Position: %d, %d, %d New Velocity: %f, %f, %f\n", nearestMapped.x, nearestMapped.y, nearestMapped.z, vx, vy, vz);
      } else {
        // No valid position found, revert to previous position
        // Find which direction is causing OoB / not mapped and set velocity to 0
        Coord3D testing = toCoord3DRounded();
        revert();
        // change X val
        testing.x = newPos.x;
        if (testing.isOutofBounds(layer->size) || !layer->isMapped(layer->XYZUnModified(testing))) vx = 0;
        // change Y val
        testing = toCoord3DRounded();
        testing.y = newPos.y;
        if (testing.isOutofBounds(layer->size) || !layer->isMapped(layer->XYZUnModified(testing))) vy = 0;
        // change Z val
        testing = toCoord3DRounded();
        testing.z = newPos.z;
        if (testing.isOutofBounds(layer->size) || !layer->isMapped(layer->XYZUnModified(testing))) vz = 0;

        if (debugPrint) EXT_LOGD(ML_TAG, "     No valid position found, reverted. Velocity Updated\n");
        if (debugPrint) EXT_LOGD(ML_TAG, "     New Pos: %f, %f, %f Velo: %f, %f, %f\n", x, y, z, vx, vy, vz);
      }

      layer->setRGB(toCoord3DRounded(), color);
    }
  };

  uint8_t speed = 15;
  uint8_t numParticles = 10;
  bool barriers = false;
  #ifdef STARBASE_USERMOD_MPU6050
  bool gyro = true;
  #else
  bool gyro = false;
  #endif
  bool randomGravity = true;
  uint8_t gravityChangeInterval = 5;
  // bool debugPrint    = layer->effectData.read<bool>();
  bool debugPrint = false;

  void setup() override {
    addControl(speed, "speed", "slider", 0, 30);
    addControl(numParticles, "number of Particles", "slider", 1, 255);
    addControl(barriers, "barriers", "checkbox");
  #ifdef STARBASE_USERMOD_MPU6050
    addControl(gyro, "gyro", "checkbox");
  #endif
    addControl(randomGravity, "randomGravity", "checkbox");
    addControl(gravityChangeInterval, "gravityChangeInterval", "slider", 1, 10);
    // addControl(bool, "Debug Print",             layer->effectData.write<bool>(0));
  }

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) override {
    if (control["name"] == "number of Particles" || control["name"] == "barriers") {
      settingUpParticles();
    }
  }

  void settingUpParticles() {
    EXT_LOGD(ML_TAG, "Setting Up Particles\n");
    layer->fill_solid(CRGB::Black);

    if (barriers) {
      // create a 2 pixel thick barrier around middle y value with gaps
      for (int x = 0; x < layer->size.x; x++)
        for (int z = 0; z < layer->size.z; z++) {
          if (!random8(5)) continue;
          layer->setRGB(Coord3D(x, layer->size.y / 2, z), CRGB::White);
          layer->setRGB(Coord3D(x, layer->size.y / 2 - 1, z), CRGB::White);
        }
    }

    for (int index = 0; index < numParticles; index++) {
      Coord3D rPos;
      int attempts = 0;
      do {  // Get random mapped position that isn't colored (infinite loop if small fixture size and high particle count)
        rPos = {random8(layer->size.x), random8(layer->size.y), random8(layer->size.z)};
        attempts++;
      } while ((!layer->isMapped(layer->XYZUnModified(rPos)) || layer->getRGB(rPos) != CRGB::Black) && attempts < 1000);
      // rPos = {1,1,0};
      particles[index].x = rPos.x;
      particles[index].y = rPos.y;
      particles[index].z = rPos.z;

      particles[index].vx = (random8() / 256.0f) * 2.0f - 1.0f;
      particles[index].vy = (random8() / 256.0f) * 2.0f - 1.0f;
      if (layer->layerDimension == _3D)
        particles[index].vz = (random8() / 256.0f) * 2.0f - 1.0f;
      else
        particles[index].vz = 0;

      particles[index].color = ColorFromPalette(layerP.palette, random8());
      Coord3D initPos = particles[index].toCoord3DRounded();
      layer->setRGB(initPos, particles[index].color);
    }
    EXT_LOGD(ML_TAG, "Particles Set Up\n");
    step = millis();
  }

  Particle particles[255];
  unsigned long step;
  unsigned long gravUpdate = 0;
  float gravity[3];

  void loop() override {
    if (!speed || millis() - step < 1000 / speed) return;  // Not enough time passed

    float gravityX, gravityY, gravityZ;  // Gravity if using gyro or random gravity

  #ifdef STARBASE_USERMOD_MPU6050
    if (gyro) {
      gravity[0] = -mpu6050->gravityVector.x;
      gravity[1] = mpu6050->gravityVector.z;  // Swap Y and Z axis
      gravity[2] = -mpu6050->gravityVector.y;

      if (layer->layerDimension == _2D) {  // Swap back Y and Z axis set Z to 0
        gravity[1] = -gravity[2];
        gravity[2] = 0;
      }
    }
  #endif

    if (randomGravity) {
      if (millis() - gravUpdate > gravityChangeInterval * 1000) {
        gravUpdate = millis();
        float scale = 5.0f;
        // Generate Perlin noise values and scale them
        gravity[0] = (inoise8(step, 0, 0) / 128.0f - 1.0f) * scale;
        gravity[1] = (inoise8(0, step, 0) / 128.0f - 1.0f) * scale;
        gravity[2] = (inoise8(0, 0, step) / 128.0f - 1.0f) * scale;

        gravity[0] = constrain(gravity[0], -1.0f, 1.0f);
        gravity[1] = constrain(gravity[1], -1.0f, 1.0f);
        gravity[2] = constrain(gravity[2], -1.0f, 1.0f);

        if (layer->layerDimension == _2D) gravity[2] = 0;
        // EXT_LOGD(ML_TAG, "Random Gravity: %f, %f, %f\n", gravity[0], gravity[1], gravity[2]);
      }
    }

    for (int index = 0; index < numParticles; index++) {
      if (gyro || randomGravity) {  // Lerp gravity towards gyro or random gravity if enabled
        float lerpFactor = .75;
        particles[index].vx += (gravity[0] - particles[index].vx) * lerpFactor;
        particles[index].vy += (gravity[1] - particles[index].vy) * lerpFactor;  // Swap Y and Z axis
        particles[index].vz += (gravity[2] - particles[index].vz) * lerpFactor;
      }
      particles[index].updatePositionandDraw(layer, index, debugPrint);
    }

    step = millis();
  }
};

  #if USE_M5UNIFIED

class MoonManEffect : public Node {
 public:
  static const char* name() { return "Moon Man"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥🎵🎨☾"; }

  // Create an M5Canvas for PNG processing
  M5Canvas* canvas;  //(&M5.Display);

  void setup() override { canvas = new M5Canvas(&M5.Display); }

  void onSizeChanged(const Coord3D& prevSize) override {
    // Create canvas for processing
    canvas->deleteSprite();
    canvas->createSprite(layer->size.x, layer->size.y);
    // Load and display PNG
    displayPNGToPanel();
  }

  bool success = false;

  void displayPNGToPanel() {
    // Method 1: Direct decode to canvas (if PNG fits in memory)
    canvas->fillSprite(TFT_BLACK);

    // Draw PNG to canvas - M5GFX handles scaling automatically
    success = canvas->drawPng(moonmanpng, moonmanpng_len, 0, 0, 0, 0, 0, 0, layer->size.x / 320.0, layer->size.y / 320.0);
    if (success) {
      Serial.println("PNG decoded successfully!");

      // Transfer canvas to LED panel
      transferCanvasToPanel();
    } else {
      Serial.println("PNG decode failed!");
    }
  }

  void transferCanvasToPanel() {
    // Read each pixel from canvas and send to LED panel
    for (int y = 0; y < layer->size.y; y++) {
      for (int x = 0; x < layer->size.x; x++) {
        // Get pixel color from canvas
        uint16_t color = canvas->readPixel(x, y);

        // Convert RGB565 to RGB888 for LED panel
        uint8_t r = ((color >> 11) & 0x1F) << 3;  // 5 bits -> 8 bits
        uint8_t g = ((color >> 5) & 0x3F) << 2;   // 6 bits -> 8 bits
        uint8_t b = (color & 0x1F) << 3;          // 5 bits -> 8 bits

        // Set pixel on LED panel
        layer->setRGB(Coord3D(x, y), CRGB(r, g, b));
      }
    }
  }

  void loop() override {
    if (success)
      // Transfer canvas to LED panel
      transferCanvasToPanel();
  }  // loop

};  // MoonManEffect

  #endif

class SpiralFireEffect : public Node {
 public:
  static const char* name() { return "Spiral Fire"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🔥🎨⏳"; }

  uint8_t speed = 60;
  uint8_t intensity = 180;
  uint8_t rotationSpeed = 30;

  void setup() override {
    addControl(speed, "speed", "slider");
    addControl(intensity, "intensity", "slider");
    addControl(rotationSpeed, "rotation", "slider");
  }

  void loop() override {
    layer->fadeToBlackBy(40);

    Coord3D pos;
    uint16_t time = millis() >> 4;

    // Calculate center of the cone
    float centerX = layer->size.x / 2.0f;
    float centerZ = layer->size.z / 2.0f;

    for (pos.y = 0; pos.y < layer->size.y; pos.y++) {
      for (pos.x = 0; pos.x < layer->size.x; pos.x++) {
        for (pos.z = 0; pos.z < layer->size.z; pos.z++) {
          // Calculate distance from center (radius)
          float dx = pos.x - centerX;
          float dz = pos.z - centerZ;
          float radius = sqrtf(dx * dx + dz * dz);

          // Calculate angle around the cone
          float angle = atan2f(dz, dx);

          // Expected radius at this height (cone tapers to point at top)
          float expectedRadius = (layer->size.x / 2.0f) * (1.0f - (float)pos.y / layer->size.y);

          // Only light LEDs that are close to the cone surface
          if (fabsf(radius - expectedRadius) < 1.5f) {
            // Create rising flame effect with spiral
            uint8_t spiralPhase = (uint8_t)(angle * 40.0f + pos.y * 20 - time * rotationSpeed / 10);
            uint8_t flameHeight = beatsin8(speed, 0, layer->size.y);

            // Brightness based on height and spiral pattern
            if (pos.y <= flameHeight) {
              uint8_t brightness = 255 - (pos.y * 255 / layer->size.y);
              brightness = qadd8(brightness, sin8(spiralPhase) / 2);

              // Color: bottom hot (yellow/white), top cooler (red/orange)
              uint8_t colorIndex = 255 - (pos.y * 180 / layer->size.y) + sin8(spiralPhase) / 3;

              CRGB color = ColorFromPalette(layerP.palette, colorIndex, brightness);

              // Add some intensity variation
              color.nscale8(intensity);

              layer->setRGB(pos, color);
            }
          }
        }
      }
    }
  }
};

// https://github.com/toggledbits/MatrixFireFast/blob/master/MatrixFireFast/MatrixFireFast.ino
class FireEffect : public Node {
 public:
  static const char* name() { return "Fire"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "💫"; }

  const uint32_t colors[11] = {0x000000, 0x100000, 0x300000, 0x600000, 0x800000, 0xA00000, 0xC02000, 0xC04000, 0xC06000, 0xC08000, 0x807080};
  const uint8_t NCOLORS = std::size(colors);

  void glow(int x, int y, int z, uint8_t flareDecay, bool usePalette) {
    int b = z * 10 / flareDecay + 1;
    for (int i = (y - b); i < (y + b); ++i) {
      for (int j = (x - b); j < (x + b); ++j) {
        if (i >= 0 && j >= 0 && i < layer->size.y && j < layer->size.x) {
          int d = (flareDecay * isqrt((x - j) * (x - j) + (y - i) * (y - i)) + 5) / 10;
          uint8_t n = 0;
          if (z > d) n = z - d;
          if (layer->getRGB(Coord3D(j, layer->size.y - 1 - i)) < usePalette ? ColorFromPalette(layerP.palette, n * 23) : colors[n]) {  // can only get brighter
            layer->setRGB(Coord3D(j, layer->size.y - 1 - i), usePalette ? ColorFromPalette(layerP.palette, n * 23) : colors[n]);       // 23*11 -> within palette range
          }
        }
      }
    }
  }

  // utility function?
  uint32_t isqrt(uint32_t n) {
    if (n < 2) return n;
    uint32_t smallCandidate = isqrt(n >> 2) << 1;
    uint32_t largeCandidate = smallCandidate + 1;
    return (largeCandidate * largeCandidate > n) ? smallCandidate : largeCandidate;
  }

  bool usePalette = false;
  uint8_t flareRows = 2;
  uint8_t maxFlare = 8;
  uint8_t flareChance = 50;
  uint8_t flareDecay = 14;

  void setup() {
    addControl(usePalette, "usePalette", "checkbox");
    addControl(flareRows, "flareRows", "slider", 0, 5);       /* number of rows (from bottom) allowed to flare */
    addControl(maxFlare, "maxFlare", "slider", 0, 18);        /* max number of simultaneous flares */
    addControl(flareChance, "flareChance", "slider", 0, 100); /* chance (%) of a new flare (if there's room) */
    addControl(flareDecay, "flareDecay", "slider", 0, 28);    /* decay rate of flare radiation; 14 is good */
  }

  uint8_t nflare;
  uint32_t flare[18];

  void loop() {
    // Effect Variables

    // First, move all existing heat points up the display and fade
    for (int y = layer->size.y - 1; y > 0; --y) {
      for (int x = 0; x < layer->size.x; ++x) {
        CRGB n = CRGB::Black;
        if (layer->getRGB(Coord3D(x, layer->size.y - y)) != CRGB::Black) {
          n = layer->getRGB(Coord3D(x, layer->size.y - y));  // - 0; //-0 to force conversion to CRGB
          if (n.red > 10)
            n.red -= 10;
          else
            n.red = 0;
          if (n.green > 10)
            n.green -= 10;
          else
            n.green = 0;
          if (n.blue > 10)
            n.blue -= 10;
          else
            n.blue = 0;
        }
        layer->setRGB(Coord3D(x, layer->size.y - 1 - y), n);
      }
    }

    // Heat the bottom row
    for (int x = 0; x < layer->size.x; ++x) {
      CRGB i = layer->getRGB(Coord3D(x, layer->size.y - 1));  // - 0; //-0 to force conversion to CRGB
      if (i != CRGB::Black) {
        layer->setRGB(Coord3D(x, layer->size.y - 1), usePalette ? ColorFromPalette(layerP.palette, random8()) : colors[random(NCOLORS - 6, NCOLORS - 2)]);
      }
    }

    // flare
    for (int i = 0; i < nflare; ++i) {
      int x = flare[i] & 0xff;
      int y = (flare[i] >> 8) & 0xff;
      int z = (flare[i] >> 16) & 0xff;

      glow(x, y, z, flareDecay, usePalette);

      if (z > 1) {
        flare[i] = (flare[i] & 0xffff) | ((z - 1) << 16);
      } else {
        // This flare is out
        for (int j = i + 1; j < nflare; ++j) {
          flare[j - 1] = flare[j];
        }
        --(nflare);
      }
    }

    // newflare();
    if (nflare < maxFlare && random(1, 101) <= flareChance) {
      int x = random(0, layer->size.x);
      int y = random(0, flareRows);
      int z = NCOLORS - 1;
      flare[(nflare)++] = (z << 16) | (y << 8) | (x & 0xff);

      glow(x, y, z, flareDecay, usePalette);
    }
  }

};  // Fire Effect

class VUMeterEffect : public Node {
 public:
  static const char* name() { return "VU Meter"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "♫💫📺"; }

  void drawNeedle(float angle, Coord3D topLeft, Coord3D size, CRGB color) {
    int x0 = topLeft.x + size.x / 2;  // Center of the needle
    int y0 = topLeft.y + size.y - 1;  // Bottom of the needle

    layer->drawCircle(topLeft.x + size.x / 2, topLeft.y + size.y / 2, size.x / 2, ColorFromPalette(layerP.palette, 35, 128), false);

    // Calculate needle end position
    int x1 = x0 - round(size.y * 0.7 * cos((angle + 30) * PI / 180));
    int y1 = y0 - round(size.y * 0.7 * sin((angle + 30) * PI / 180));

    // Draw the needle
    layer->drawLine(x0, y0, x1, y1, color, true);
  }

  uint8_t speed = 255;
  uint8_t bands = NUM_GEQ_CHANNELS;

  void setup() override {
    layer->fill_solid(CRGB::Black);
    addControl(speed, "speed", "slider");
    addControl(bands, "bands", "slider", 1, NUM_GEQ_CHANNELS);
  }

  void loop() override {
    layer->fadeToBlackBy(200);

    uint8_t nHorizontal = 4;
    uint8_t nVertical = 2;

    uint8_t band = 0;
    for (int h = 0; h < nHorizontal; h++) {
      for (int v = 0; v < nVertical; v++) {
        drawNeedle((float)sharedData.bands[2 * (band++)] / 2.0, {layer->size.x * h / nHorizontal, layer->size.y * v / nVertical, 0}, {layer->size.x / nHorizontal, layer->size.y / nVertical, 0}, ColorFromPalette(layerP.palette, 255 / (nHorizontal * nVertical) * band));
      }  // sharedData.bands[band++] / 200
    }
    // ppf(" v:%f, f:%f", sharedData.volume, (float) sharedData.bands[5]);
  }
};  // VUMeter

class PixelMapEffect : public Node {
 public:
  static const char* name() { return "PixelMap"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "💫"; }

  Coord3D pos = {0, 0, 0};

  void setup() override { addControl(pos, "pos", "coord3D", 0, MAX(MAX(layer->size.x, layer->size.x), layer->size.x) - 1); }

  void loop() override {
    layer->fill_solid(CRGB::Black);

    layer->setRGB(pos, CHSV(millis() / 50 + random8(64), 255, 255));  // ColorFromPalette(layerP.palette,call, bri);
  }
};  // PixelMap

class MarioTestEffect : public Node {
 public:
  static const char* name() { return "MarioTest"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "💫"; }

  bool background = false;
  Coord3D offset = {0, 0, 0};

  void setup() override {
    addControl(background, "background", "checkbox");
    addControl(offset, "offset", "coord3D", 0, 255);
  }

  const uint8_t mario[16][16] = {{0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0},  //
                                 {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0},  //
                                 {0, 0, 0, 0, 2, 2, 2, 3, 3, 4, 3, 0, 0, 0, 0, 0},  //
                                 {0, 0, 0, 2, 3, 2, 3, 3, 3, 4, 3, 3, 3, 0, 0, 0},  //
                                 {0, 0, 0, 2, 3, 2, 2, 3, 3, 3, 4, 3, 3, 3, 0, 0},  //
                                 {0, 0, 0, 0, 2, 3, 3, 3, 3, 4, 4, 4, 4, 0, 0, 0},  //
                                 {0, 0, 0, 0, 0, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0},  //
                                 {0, 0, 0, 0, 5, 5, 1, 5, 5, 1, 0, 0, 0, 0, 0, 0},  //
                                 {0, 0, 0, 5, 5, 5, 1, 5, 5, 1, 5, 5, 5, 0, 0, 0},  //
                                 {0, 0, 5, 5, 5, 5, 1, 5, 5, 1, 5, 5, 5, 5, 0, 0},  //
                                 {0, 0, 3, 3, 5, 5, 1, 1, 1, 1, 5, 5, 3, 3, 0, 0},  //
                                 {0, 0, 3, 3, 3, 1, 6, 1, 1, 6, 1, 3, 3, 3, 0, 0},  //
                                 {0, 0, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 0, 0},  //
                                 {0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0},  //
                                 {0, 0, 0, 2, 2, 2, 0, 0, 0, 0, 2, 2, 2, 0, 0, 0},  //
                                 {0, 0, 2, 2, 2, 2, 0, 0, 0, 0, 2, 2, 2, 2, 0, 0}};

  const CRGB colors[7] = {CRGB::DimGrey, CRGB::Red, CRGB::Brown, CRGB::Tan, CRGB::Black, CRGB::Blue, CRGB::Yellow};

  void loop() override {
    if (background)
      layer->fill_solid(CRGB::DimGrey);
    else
      layer->fill_solid(CRGB::Black);
    // draw 16x16 mario
    for (int x = 0; x < MIN(16, layer->size.x); x++)
      for (int y = 0; y < MIN(16, layer->size.y); y++) {
        layer->setRGB(Coord3D(x + offset.x, y + offset.y), colors[mario[y][x]]);
      }
  }
};  // MarioTest

// by netmindz

class RingEffect : public Node {
 protected:
  void setRing(int ring, CRGB colour) {       // so britisch ;-)
    layer->setRGB(Coord3D(0, ring), colour);  // 1D effect on y-axis (default)
  }
};

class RingRandomFlowEffect : public RingEffect {
 public:
  static const char* name() { return "RingRandomFlow"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "💫"; }

  // void setup() override {} //so no palette control is created

  uint8_t* hue = nullptr;

  ~RingRandomFlowEffect() { freeMB(hue); }

  void onSizeChanged(const Coord3D& prevSize) override {
    freeMB(hue);
    hue = allocMB<uint8_t>(layer->size.y);
    if (!hue) {
      EXT_LOGE(ML_TAG, "allocate hue failed");
    }
  }

  void loop() override {
    if (hue) {
      hue[0] = random(0, 255);
      for (int r = 0; r < layer->size.y; r++) {
        setRing(r, CHSV(hue[r], 255, 255));
      }
      for (int r = (layer->size.y - 1); r >= 1; r--) {
        hue[r] = hue[(r - 1)];  // set this ruing based on the inner
      }
      // FastLED.delay(SPEED);
    }
  }
};

// by netmindz
class AudioRingsEffect : public RingEffect {
 public:
  static const char* name() { return "AudioRings"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "♫💫"; }

  bool inWards = true;

  void setup() override {
    addControl(inWards, "inWards", "checkbox");
    // addControl(nrOfRings, "rings", "slider", 1, 50);
  }

  void loop() override {
    uint8_t nrOfRings = MAX(layer->size.y, 2);  // height of the layer, minimal 2
    for (int i = 0; i < nrOfRings; i++) {
      uint8_t band = ::map(i, 0, nrOfRings - 1, 0, NUM_GEQ_CHANNELS - 1);

      byte val;
      if (inWards) {
        val = sharedData.bands[band];
      } else {
        val = sharedData.bands[NUM_GEQ_CHANNELS - 1 - band];
      }

      // Visualize leds to the beat
      CRGB color = ColorFromPalette(layerP.palette, val, val);
      //      CRGB color = ColorFromPalette(currentPalette, val, 255, currentBlending);
      //      color.nscale8_video(val);
      setRing(i, color);
      //        setRingFromFtt((i * 2), i);
    }

    if (nrOfRings >= 2) setRingFromFtt(2, nrOfRings - 2);  // set outer rings to bass
    if (nrOfRings >= 1) setRingFromFtt(0, nrOfRings - 1);  // set outer rings to bass
  }
  void setRingFromFtt(int index, int ring) {
    byte val = sharedData.bands[index];
    // Visualize leds to the beat
    CRGB color = ColorFromPalette(layerP.palette, val);
    color.nscale8_video(val);
    setRing(ring, color);
  }
};

#endif