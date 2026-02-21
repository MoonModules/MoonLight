/**
    @title     MoonLight
    @file      L__Sandbox.h (Layout sandbox)
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

// example template, do not remove!
// add this class in /src/MoonLight/ModuleDrivers::addNodes()
// add documentation in /docs/moonlight/layouts.md
class ExampleLayout : public Node {
 public:
  static const char* name() { return "Example"; }
  static uint8_t dim() { return _3D; }          // dimensions supported
  static const char* tags() { return "🚥🆕"; }  // use emojis see https://moonmoduÍles.org/MoonLight/moonlight/overview/#emoji-coding, 🚥 for layout

  uint8_t width = 12;
  uint8_t height = 12;
  uint8_t depth = 12;

  void setup() override {
    // controls will show in the UI
    // for different type of controls see other Nodes
    addControl(width, "width", "slider");
    addControl(height, "height", "slider");
    addControl(depth, "depth", "slider");
  }

  bool hasOnLayout() const override { return true; }  // so the mapping system knows this node has onLayout, eg each time a modifier changes
  void onLayout() override {
    Coord3D pos;
    for (pos.z = 0; pos.z < layer->size.z; pos.z++) {
      for (pos.y = 0; pos.y < layer->size.y; pos.y++) {
        for (pos.x = 0; pos.x < layer->size.x; pos.x++) {
          addLight(pos);
        }
      }
      nextPin(); // if more pins are defined, the next lights will be assigned to the next pin
    }
  }
};

#endif