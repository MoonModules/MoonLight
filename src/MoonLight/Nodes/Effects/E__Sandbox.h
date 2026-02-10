/**
    @title     MoonLight
    @file      E__Sandbox.h (Effect sandbox)
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

// example template, do not remove!
// add this class in /src/MoonLight/ModuleEffects::addNodes()
// add documentation in /docs/moonlight/effects.md
class ExampleEffect : public Node {
 public:
  static const char* name() { return "Example"; }
  static uint8_t dim() { return _3D; }            // Dimensions supported _3D prefered, _2D or _1D can be used for first phase
  static const char* tags() { return "🔥⏳"; }  // use emojis see https://moonmodules.org/MoonLight/moonlight/overview/#emoji-coding, 🔥 for effect

  uint8_t bpm = 60;  // 1 beat per second
  uint8_t intensity = 128;

  void setup() override {
    // controls will show in the UI
    // for different type of controls see other Nodes
    addControl(bpm, "bpm", "slider");
    addControl(intensity, "intensity", "slider");
  }

  void onSizeChanged(const Coord3D& prevSize) override {}  // e.g. realloc variables

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) {
    // add your custom onUpdate code here
    if (control["name"] == "bpm") {
      if (control["value"] == 0) {
      }
    }
  }

  // each frame of effect
  // use of bpm functions like beatsin8 is recommended so independent from LEDs framerate (33000/nrOfLights)
  void loop() override {
    layer->fadeToBlackBy(70);
    sharedData;  // read shared data if needed, add sharedData if needed
    Coord3D pos;
    for (pos.z = 0; pos.z < layer->size.z; pos.z++) {
      for (pos.y = 0; pos.y < layer->size.y; pos.y++) {
        for (pos.x = 0; pos.x < layer->size.x; pos.x++) {
          pos.x = beatsin8(bpm, 0, layer->size.x - 1);
          pos.y = beatsin8(intensity, 0, layer->size.y - 1);
          pos.z = beatsin8(intensity, 0, layer->size.z - 1);
          layer->setRGB(pos, ColorFromPalette(layerP.palette, beatsin8(12, 0, 255)));
          // NOTE: effects must ensure pos is within layer->size bounds (setRGB omits bounds checks for performance)
        }
      }
    }
  }

  ~ExampleEffect() override {};  // e,g, to free allocated memory
};

#endif

// WLED conversion changes:
//
// controls: static const char _data_FX_MODE_PACMAN[] PROGMEM = "PacMan@Speed,# of PowerDots,Blink distance,Blur,# of Ghosts,Dots,Smear,Compact;;!;1;m12=0,sx=192,ix=64,c1=64,c2=0,c3=12,o1=1,o2=0"; -> addControl()
// control mapping: unsigned numGhosts = map(nrOfghosts, 0, 31, 2, 8); -> change the range in addControl instead of mapping
// SEGLEN: layer->nrOfLights
// RED: CRGB::Red etc.
// SEGMENT.intensity: use addControl class variables
// SEGENV.aux0: -> class variable (change/extend name to make it semantic)
// SEGENV.call== 0 -> move to setup()
// SEGMENT. -> Layer->
// FRAMETIME -> 1000 / 40 (for the time being)
// SEGCOLOR(1) -> CRGB::Black (for the time being)