/**
    @title     MoonLight
    @file      D__Sandbox.h (Driver sandbox)
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
// add documentation in /docs/moonlight/drivers.md
class ExampleDriver : public Node {
 public:
  static const char* name() { return "Example"; }
  static uint8_t dim() { return _NoD; }        // Dimensions not relevant for drivers?
  static const char* tags() { return "☸️⏳"; }  // use emojis see https://moonmodules.org/MoonLight/moonlight/overview/#emoji-coding, ☸️ for drivers

  uint8_t pin = 16;

  void setup() override {
    // controls will show in the UI
    // for different type of controls see other Nodes
    addControl(pin, "pin", "slider", 1, SOC_GPIO_PIN_COUNT - 1);
  }

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) {
    // add your custom onUpdate code here
    if (control["name"] == "pin") {
      if (control["value"] == 0) {
      }
    }
  }

  bool hasOnLayout() const override { return true; }  // so the mapping system knows this node has onLayout, eg each time a modifier changes
  void onLayout() override {};                        // onLayout for drivers is used to init or update the driver based on the layouts, a driver will use the layout nodes which are defined before the driver node, so order matters

  // use for continuous actions, e.g. reading data from sensors or sending data to lights (e.g. LED drivers or Art-Net)
  void loop() override {
    pinMode(pin, INPUT);
    int value = digitalRead(pin);
    sharedData;  // write value to shared data if needed, add sharedData if needed, use in other nodes (e.g. effects)
  };

  ~ExampleDriver() override {};  // e.g. to free allocated memory
};

#endif