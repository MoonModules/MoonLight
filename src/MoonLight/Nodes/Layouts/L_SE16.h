/**
    @title     MoonLight
    @file      L_SE16.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

// SE16 board
class SE16Layout : public Node {
 public:
  static const char* name() { return "SE16"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🚥"; }

  bool mirroredPins = false;
  bool pinsAreColumns = false;
  uint16_t ledsPerPin = 10;

  void setup() override {
    addControl(mirroredPins, "mirroredPins", "checkbox");
    addControl(pinsAreColumns, "pinsAreColumns", "checkbox");
    addControl(ledsPerPin, "ledsPerPin", "number", 1, 2047);
  }

  void addStrip(uint16_t xposition, uint16_t start_y, uint16_t stop_y) {
    bool increasing = start_y < stop_y;
    for (int y = start_y; increasing ? (y <= stop_y) : (y >= stop_y); y += increasing ? 1 : -1) {
      if (pinsAreColumns)
        addLight(Coord3D(xposition, y, 0));  // limpkin
      else
        addLight(Coord3D(y, xposition, 0));  // ewowi
    }

    nextPin();  // each strip it's own pin
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    if (mirroredPins) {  // limpkin
      addStrip(7, ledsPerPin, 2 * ledsPerPin - 1);
      addStrip(7, ledsPerPin - 1, 0);
      addStrip(6, ledsPerPin, 2 * ledsPerPin - 1);
      addStrip(6, ledsPerPin - 1, 0);
      addStrip(5, ledsPerPin, 2 * ledsPerPin - 1);
      addStrip(5, ledsPerPin - 1, 0);
      addStrip(4, ledsPerPin, 2 * ledsPerPin - 1);
      addStrip(4, ledsPerPin - 1, 0);
      addStrip(3, ledsPerPin, 2 * ledsPerPin - 1);
      addStrip(3, ledsPerPin - 1, 0);
      addStrip(2, ledsPerPin, 2 * ledsPerPin - 1);
      addStrip(2, ledsPerPin - 1, 0);
      addStrip(1, ledsPerPin, 2 * ledsPerPin - 1);
      addStrip(1, ledsPerPin - 1, 0);
      addStrip(0, ledsPerPin, 2 * ledsPerPin - 1);
      addStrip(0, ledsPerPin - 1, 0);
    } else {  // ewowi
      addStrip(14, 0, ledsPerPin - 1);
      addStrip(15, 0, ledsPerPin - 1);
      addStrip(12, 0, ledsPerPin - 1);
      addStrip(13, 0, ledsPerPin - 1);
      addStrip(10, 0, ledsPerPin - 1);
      addStrip(11, 0, ledsPerPin - 1);
      addStrip(8, 0, ledsPerPin - 1);
      addStrip(9, 0, ledsPerPin - 1);
      addStrip(6, 0, ledsPerPin - 1);
      addStrip(7, 0, ledsPerPin - 1);
      addStrip(4, 0, ledsPerPin - 1);
      addStrip(5, 0, ledsPerPin - 1);
      addStrip(2, 0, ledsPerPin - 1);
      addStrip(3, 0, ledsPerPin - 1);
      addStrip(0, 0, ledsPerPin - 1);
      addStrip(1, 0, ledsPerPin - 1);
    }
  }
};

// LightCrafter16 board
class LightCrafter16Layout : public Node {
 public:
  static const char* name() { return "LightCrafter16"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🚥"; }

  bool pinsAreColumns = false;
  uint16_t ledsPerPin = 10;

  void setup() override {
    addControl(pinsAreColumns, "pinsAreColumns", "checkbox");
    addControl(ledsPerPin, "ledsPerPin", "number", 1, 2047);
  }

  void addStrip(uint16_t xposition, uint16_t start_y, uint16_t stop_y) {
    bool increasing = start_y < stop_y;
    for (int y = start_y; increasing ? (y <= stop_y) : (y >= stop_y); y += increasing ? 1 : -1) {
      if (pinsAreColumns)
        addLight(Coord3D(xposition, y, 0));  // limpkin
      else
        addLight(Coord3D(y, xposition, 0));  // ewowi
    }

    nextPin();  // each strip it's own pin
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    addStrip(0, ledsPerPin - 1, 0);
    addStrip(1, ledsPerPin - 1, 0);
    addStrip(2, ledsPerPin - 1, 0);
    addStrip(3, ledsPerPin - 1, 0);
    addStrip(4, ledsPerPin - 1, 0);
    addStrip(5, ledsPerPin - 1, 0);
    addStrip(6, ledsPerPin - 1, 0);
    addStrip(7, ledsPerPin - 1, 0);
    addStrip(7, ledsPerPin, 2 * ledsPerPin - 1);
    addStrip(6, ledsPerPin, 2 * ledsPerPin - 1);
    addStrip(5, ledsPerPin, 2 * ledsPerPin - 1);
    addStrip(4, ledsPerPin, 2 * ledsPerPin - 1);
    addStrip(3, ledsPerPin, 2 * ledsPerPin - 1);
    addStrip(2, ledsPerPin, 2 * ledsPerPin - 1);
    addStrip(1, ledsPerPin, 2 * ledsPerPin - 1);
    addStrip(0, ledsPerPin, 2 * ledsPerPin - 1);
  }
};

#endif  // FT_MOONLIGHT