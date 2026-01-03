/**
    @title     MoonLight
    @file      L_MoonLight.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

class HumanSizedCubeLayout : public Node {
 public:
  static const char* name() { return "Human Sized Cube"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🚥"; }

  uint8_t width = 10;
  uint8_t height = 10;
  uint8_t depth = 10;
  // bool snake;

  void setup() override {
    addControl(width, "width", "slider", 1, 20);
    addControl(height, "height", "slider", 1, 20);
    addControl(depth, "depth", "slider", 1, 20);
    // addControl(snake, "snake", "checkbox");
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    // front: z = 0
    for (int x = 0; x < width; x++)
      for (int y = 0; y < height; y++) addLight(Coord3D(x + 1, y + 1, 0));
    nextPin();  // each curtain it's own pin

    // back: z = depth+1
    for (int x = 0; x < width; x++)
      for (int y = 0; y < height; y++) addLight(Coord3D(x + 1, y + 1, depth + 1));
    nextPin();  // each curtain it's own pin

    // above: y = 0
    for (int x = 0; x < width; x++)
      for (int z = 0; z < depth; z++) addLight(Coord3D(x + 1, 0, z + 1));
    nextPin();  // each curtain it's own pin

    // //below: y = height+1
    // for (int x = 0; x<width; x++) for (int z = 0; z<depth; z++) addLight(Coord3D(x+1, height+1, z+1));
    // nextPin();  // each curtain it's own pin

    // left: x = 0
    for (int z = 0; z < depth; z++)
      for (int y = 0; y < height; y++) addLight(Coord3D(0, y + 1, z + 1));
    nextPin();  // each curtain it's own pin

    // right: x = width+1
    for (int z = 0; z < depth; z++)
      for (int y = 0; y < height; y++) addLight(Coord3D(width + 1, y + 1, z + 1));
    nextPin();  // each curtain it's own pin
  }
};

struct Wiring {
  uint16_t size[3] = {16, 16, 16};       // panels of 16x16
  uint8_t wiringOrder = 1;               // first Y, then X, then Z
  bool inc[3] = {true, true};            // left, top, front
  bool snake[3] = {false, true, false};  // snake on Y
  uint8_t* axes;

  void iterate(int axis, int snaker, std::function<void(uint16_t)> fun) {
    bool iInc = inc[axes[axis]];
    if (snake[axis] && snaker % 2 == 1) iInc = !iInc;  // reverse order for snake
    for (int j = iInc ? 0 : size[axes[axis]] - 1; iInc ? j < size[axes[axis]] : j >= 0; j += iInc ? 1 : -1) {
      fun(j);
    }
  }
};

// PanelLayout is a simplified version of PanelsLayout (only one panel)
class PanelLayout : public Node {
 public:
  static const char* name() { return "Panel"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🚥"; }

  Wiring panel = {{16, 16, 1}, 1, {true, true, true}, {false, true, false}};
  ;  // 16x16 panel, increasing over the axis, snake on the Y-axis

  void setup() override {
    addControl(panel.size[0], "panelWidth", "number", 1, 65536);
    addControl(panel.size[1], "panelHeight", "number", 1, 65536);
    addControl(panel.wiringOrder, "wiringOrder", "select");
    addControlValue("XY");
    addControlValue("YX");
    addControl(panel.inc[0], "X++", "checkbox");
    addControl(panel.inc[1], "Y++", "checkbox");
    addControl(panel.snake[1], "snake", "checkbox");
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    uint8_t axisOrders[2][2] = {
        {1, 0},  // Y(1) outer loop, X(0) inner loop
        {0, 1},  // X(0) outer loop, Y(1) inner loop
    };

    panel.axes = axisOrders[panel.wiringOrder];  // choose one of the orders
    panel.iterate(0, 0, [&](uint16_t i) {
      panel.iterate(1, i, [&](uint16_t j) {
        int coords[2];
        coords[panel.axes[0]] = i;
        coords[panel.axes[1]] = j;
        addLight(Coord3D(coords[0], coords[1]));
      });
    });
    nextPin();  // all lights to one pin
  }
};

class PanelsLayout : public Node {
 public:
  static const char* name() { return "Panels"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🚥"; }

  Wiring panels = {{2, 2, 1}, 1, {true, true, true}, {false, false, false}};  // 2x2 panels, increasing over the axis,
  Wiring panel = {{16, 16, 1}, 1, {true, true, true}, {false, true, false}};  // 16x16 panel, increasing over the axis, snake on the Y-axis

  uint8_t panelsPerPin = 1;

  void setup() override {
    addControl(panels.size[0], "horizontalPanels", "number", 1, 32);
    addControl(panels.size[1], "verticalPanels", "number", 1, 32);
    addControl(panels.wiringOrder, "wiringOrderP", "select");
    addControlValue("XY");
    addControlValue("YX");
    addControl(panels.inc[0], "X++P", "checkbox");
    addControl(panels.inc[1], "Y++P", "checkbox");
    addControl(panels.snake[1], "snakeP", "checkbox");

    addControl(panel.size[0], "panelWidth", "number", 1, 65536);
    addControl(panel.size[1], "panelHeight", "number", 1, 65536);
    addControl(panel.wiringOrder, "wiringOrder", "select");
    addControlValue("XY");
    addControlValue("YX");
    addControl(panel.inc[0], "X++", "checkbox");
    addControl(panel.inc[1], "Y++", "checkbox");
    addControl(panel.snake[1], "snake", "checkbox");
    addControl(panelsPerPin, "panelsPerPin", "number", 1);  // minimal 1
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    uint8_t axisOrders[2][2] = {
        {1, 0},  // Y(1) outer loop, X(0) inner loop
        {0, 1},  // X(0) outer loop, Y(1) inner loop
    };

    panels.axes = axisOrders[panels.wiringOrder];  // choose one of the orders
    uint8_t nrOfPanels = 0;
    panels.iterate(0, 0, [&](uint16_t a) {
      panels.iterate(1, a, [&](uint16_t b) {
        int coordsP[2];
        coordsP[panels.axes[0]] = a;
        coordsP[panels.axes[1]] = b;

        panel.axes = axisOrders[panel.wiringOrder];  // choose one of the orders
        panel.iterate(0, 0, [&](uint16_t i) {
          panel.iterate(1, i, [&](uint16_t j) {
            int coords[2];
            coords[panel.axes[0]] = i;
            coords[panel.axes[1]] = j;
            addLight(Coord3D(coordsP[0] * panel.size[0] + coords[0], coordsP[1] * panel.size[1] + coords[1]));
          });
        });

        nrOfPanels++;

        if (nrOfPanels % panelsPerPin == 0) nextPin();  // each panelsPerPin it's own pin
      });
    });
    if (nrOfPanels % panelsPerPin != 0) nextPin();  // If there is a final partial group, still assign it to a pin
  }
};

class CubeLayout : public Node {
 public:
  static const char* name() { return "Cube"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🚥"; }

  Wiring panels = {{10, 10, 10}, 3, {true, true, true}, {false, true, false}};
  ;  // 16x16 panel, increasing over the axis, snake on the Y-axis

  void setup() override {
    addControl(panels.size[0], "width", "number", 1, 128);
    addControl(panels.size[1], "height", "number", 1, 128);
    addControl(panels.size[2], "depth", "number", 1, 128);
    addControl(panels.wiringOrder, "wiringOrder", "select");
    addControlValue("XYZ");
    addControlValue("YXZ");
    addControlValue("XZY");
    addControlValue("YZX");
    addControlValue("ZXY");
    addControlValue("ZYX");
    addControl(panels.inc[0], "X++", "checkbox");
    addControl(panels.inc[1], "Y++", "checkbox");
    addControl(panels.inc[2], "Z++", "checkbox");
    addControl(panels.snake[0], "snakeX", "checkbox");
    addControl(panels.snake[1], "snakeY", "checkbox");
    addControl(panels.snake[2], "snakeZ", "checkbox");
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    uint8_t axisOrders[6][3] = {
        {2, 1, 0},  // Z (outer), Y (middle), X (inner) -- X fastest
        {2, 0, 1},  // Z, X, Y
        {1, 2, 0},  // Y, Z, X
        {1, 0, 2},  // Y, X, Z
        {0, 2, 1},  // X, Z, Y
        {0, 1, 2}   // X, Y, Z
    };

    panels.axes = axisOrders[panels.wiringOrder];  // choose one of the orders
    panels.iterate(0, 0, [&](uint16_t i) {
      panels.iterate(1, i, [&](uint16_t j) {
        panels.iterate(2, j, [&](uint16_t k) {
          int coords[3];
          coords[panels.axes[0]] = i;
          coords[panels.axes[1]] = j;
          coords[panels.axes[2]] = k;
          addLight(Coord3D(coords[0], coords[1], coords[2]));
        });
      });

      nextPin();  // each plane it's own pin
    });
  }
};

class TorontoBarCubesLayout : public Node {
 public:
  static const char* name() { return "Toronto Bar Cubes"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🚥"; }

  // Coord3D size;
  uint8_t nrOfLightsPerCube = 71;

  void setup() override {
    // addControl(size.x, "width", "number", 1, 128);
    // addControl(size.y, "height", "number", 1, 128);
    // addControl(size.z, "depth", "number", 1, 128);
    addControl(nrOfLightsPerCube, "nrOfLightsPerCube", "slider", 1, 128);
  }

  void addCube(Coord3D pos) {
    for (int i = 0; i < nrOfLightsPerCube; i++) addLight(Coord3D(pos.x, pos.y, pos.z));  // all cube lights on the same position for the time being
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {

    //modify to whatever cube order and nr of cubes in a 3D grid space
    Coord3D cubes[] = {
        //
        Coord3D(0, 0, 0),
        Coord3D(1, 0, 0),
        Coord3D(2, 0, 0),
        Coord3D(3, 0, 0)  //
        ,
        Coord3D(0, 1, 0),
        Coord3D(1, 1, 0),
        Coord3D(2, 1, 0),
        Coord3D(3, 1, 0)  //
        ,
        Coord3D(0, 2, 0),
        Coord3D(1, 2, 0),
        Coord3D(2, 2, 0),
        Coord3D(3, 2, 0)  //
        ,
        Coord3D(0, 0, 0),
        Coord3D(1, 0, 1),
        Coord3D(2, 0, 1),
        Coord3D(3, 0, 1)  //
        ,
        Coord3D(0, 1, 1),
        Coord3D(1, 1, 1),
        Coord3D(2, 1, 1),
        Coord3D(3, 1, 1)  //
        ,
        Coord3D(0, 2, 1),
        Coord3D(1, 2, 1),
        Coord3D(2, 2, 1),
        Coord3D(3, 2, 1)  //
    };

    for (Coord3D cube : cubes) addCube(cube);

    nextPin(); // keep this for now ...
  }
};

class SingleLineLayout : public Node {
 public:
  static const char* name() { return "Single Line"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "🚥"; }

  uint8_t start_x = 0;
  uint16_t width = 30;
  uint16_t yposition = 0;
  bool reversed_order = false;
  uint8_t ledPinDIO = 0;  // default

  void setup() override {
    addControl(start_x, "starting X", "slider", 0, 255);
    addControl(width, "width", "number", 1, 1000);
    addControl(yposition, "Y position", "number", 0, 255);
    addControl(reversed_order, "reversed order", "checkbox");
    addControl(ledPinDIO, "LED pin DIO", "select");
    addControlValue("Default");
    Char<8> text;
    for (int i = 0; i < MAXLEDPINS; i++) {
      text.format("LED %02d", i + 1);
      addControlValue(text.c_str());
    }
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    if (reversed_order) {
      for (int x = start_x + width - 1; x >= start_x; x--) {
        addLight(Coord3D(x, yposition, 0));
      }
    } else {
      for (int x = start_x; x < start_x + width; x++) {
        addLight(Coord3D(x, yposition, 0));
      }
    }
    nextPin(ledPinDIO == 0 ? UINT8_MAX : ledPinDIO - 1);  // all lights to one pin, default: use default
  }
};

class SingleRowLayout : public Node {
 public:
  static const char* name() { return "Single Row"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "🚥"; }

  uint8_t start_y = 0;
  uint16_t height = 30;
  uint16_t xposition = 0;
  bool reversed_order = false;
  uint8_t ledPinDIO = 0;  // default

  void setup() override {
    addControl(start_y, "starting Y", "slider", 0, 255);
    addControl(height, "height", "number", 1, 1000);
    addControl(xposition, "X position", "number", 0, 255);
    addControl(reversed_order, "reversed order", "checkbox");
    addControl(ledPinDIO, "LED pin DIO", "select");
    addControlValue("Default");
    Char<8> text;
    for (int i = 0; i < MAXLEDPINS; i++) {
      text.format("LED %02d", i + 1);
      addControlValue(text.c_str());
    }
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    if (reversed_order) {
      for (int y = start_y + height - 1; y >= start_y; y--) {
        addLight(Coord3D(xposition, y, 0));
      }
    } else {
      for (int y = start_y; y < start_y + height; y++) {
        addLight(Coord3D(xposition, y, 0));
      }
    }
    nextPin(ledPinDIO == 0 ? UINT8_MAX : ledPinDIO - 1);  // all lights to one pin, default: use default
  }
};

class RingLayout : public Node {
 public:
  static const char* name() { return "Ring"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🚥"; }

  Coord3D topLeft;
  uint8_t nrOfLEDs = 24;
  uint16_t angleFirst = 0;  // top

  void setup() override {
    addControl(topLeft, "topLeft", "coord3D", 0, 255);
    addControl(nrOfLEDs, "nrOfLEDs", "slider", 1, 255);
    addControl(angleFirst, "angleFirst", "slider", 0, 359);
  }

  bool hasOnLayout() const override { return true; }

  void onLayout() override {
    // Calculate ring dimensions based on 1cm spacing between LEDs
    const float radius = nrOfLEDs / (2.0 * PI);  // or nrOfLEDs / 6.28318
    const float width = 2.2 * radius;
    const float height = 2.2 * radius;

    const float centerX = width / 2.0;
    const float centerY = height / 2.0;

    for (int i = 0; i < nrOfLEDs; i++) {
      float x = centerX;
      float y = centerY;

      if (nrOfLEDs != 1) {
        float angle = PI + (TWO_PI * i) / nrOfLEDs + TWO_PI * angleFirst / 359;

        // EXT_LOGD(ML_TAG, "%d = %d - %d (%d)", angle, (255 * i) / (nrOfLEDs - 1), map(angleFirst, 0, 359, 0, 255), angleFirst);

        x = centerX - sin(angle) * radius;
        y = centerY + cos(angle) * radius;
      }

      addLight({(uint8_t)x + topLeft.x, (uint8_t)y + topLeft.y, topLeft.z});
    }

    nextPin();  // all lights to one pin
  }
};

class Rings16Layout : public Node {
 public:
  static const char* name() { return "16 Rings"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🚥"; }

  // uint8_t nrOfLEDs = 24;
  // uint16_t angleFirst = 0;  // top

  void setup() override {
    // addControl(nrOfLEDs, "nrOfLEDs", "slider", 1, 255);
    // addControl(angleFirst, "angleFirst", "slider", 0, 359);
  }

  bool hasOnLayout() const override { return true; }

  void onLayout() override {
    RingLayout ringLayout;

    uint8_t rows[] = {0, 5, 12, 18, 23, 29, 35, 41, 48};
    uint8_t cols[] = {5, 17, 30, 36, 42, 54, 65};
    ringLayout.topLeft = {cols[5], rows[3], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[6], rows[4], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[5], rows[1], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[5], rows[5], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[4], rows[2], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[5], rows[7], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[3], rows[0], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[4], rows[6], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[2], rows[2], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[3], rows[8], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[1], rows[1], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[2], rows[6], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[1], rows[3], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[1], rows[7], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[0], rows[4], 0};
    ringLayout.onLayout();
    ringLayout.topLeft = {cols[1], rows[5], 0};
    ringLayout.onLayout();
  }
};

class RingsLayout : public Node {
 public:
  static const char* name() { return "Rings"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🚥"; }

  uint8_t width = 16;
  uint8_t height = 16;

  void setup() override {}

  void add(int leds, int radius) {
    for (int i = 0; i < leds; i++) {
      uint8_t x = width / 2.0;
      uint8_t y = height / 2.0;
      if (leds != 1) {
        x = width / 2.0 + ((sin8(255 * i / (leds - 1)) - 127) / 127.0) * radius / 10.0;
        y = height / 2.0 + ((cos8(255 * i / (leds - 1)) - 127) / 127.0) * radius / 10.0;
      }
      addLight({x, y, 0});
    }
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    add(1, 0);
    add(8, 13);
    add(12, 23);
    add(16, 33);
    add(24, 43);
    add(32, 53);
    add(40, 63);
    add(48, 73);
    add(60, 83);
    nextPin();  // all lights to one pin
  }
};

class WheelLayout : public Node {
 public:
  static const char* name() { return "Wheel"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🚥"; }

  uint8_t nrOfSpokes = 12;
  uint8_t ledsPerSpoke = 16;

  void setup() override {
    addControl(nrOfSpokes, "nrOfSpokes", "slider", 1, 48);
    addControl(ledsPerSpoke, "ledsPerSpoke", "slider", 1, 255);
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    // uint16_t size = ledsPerSpoke * 2;
    Coord3D middle;
    middle.x = ledsPerSpoke;
    middle.y = ledsPerSpoke;
    middle.z = 0;  // size >> 1; this is for later 🔥
    for (int i = 0; i < nrOfSpokes; i++) {
      float radians = i * 360 / nrOfSpokes * DEG_TO_RAD;
      for (int j = 0; j < ledsPerSpoke; j++) {
        float radius = j + 1;
        float x = radius * sinf(radians);
        float y = radius * cosf(radians);
        addLight(Coord3D(x + middle.x, y + middle.y, middle.z));
      }
    }
    nextPin();  // all lights to one pin
  }
};

class SpiralLayout : public Node {
 public:
  static const char* name() { return "Spiral"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🚥"; }

  uint16_t ledCount = 640;
  uint16_t bottomRadius = 10;
  uint16_t height = 25;

  void setup() override {
    addControl(ledCount, "ledCount", "number", 1, 2048);
    addControl(bottomRadius, "bottomRadius", "number", 1, 100);
    addControl(height, "height", "number", 1, 200);
  }

  Coord3D middle;

  bool hasOnLayout() const override { return true; }

  void onLayout() override {
    // Calculate middle point (bottom center of spiral)
    middle.x = bottomRadius;
    middle.y = 0;
    middle.z = bottomRadius;

    // Calculate strip length: 1cm between LEDs
    float stripLength = (ledCount - 1) * 1.0f;  // in cm

    // Calculate circumference at bottom
    float bottomCircumference = 2.0f * PI * bottomRadius;

    // Estimate LEDs per round at bottom (adjusts as radius changes)
    float ledsPerRound = bottomCircumference / 1.0f;  // 1cm spacing

    float currentLength = 0;

    for (int i = 0; i < ledCount; i++) {
      // Calculate progress through the spiral (0 to 1)
      float progress = (float)i / (ledCount - 1);

      // Calculate current radius (linear taper from bottom to 0)
      float currentRadius = bottomRadius * (1.0f - progress);

      // Calculate current height
      float currentHeight = height * progress;

      // Calculate angle - accumulate based on arc length
      // For each LED, advance by 1cm along the current circumference
      float currentCircumference = 2.0f * PI * currentRadius;
      float angleIncrement = (currentCircumference > 0) ? (1.0f / currentRadius) : 0;
      currentLength += (i > 0) ? angleIncrement : 0;

      // Alternative: use estimated leds per round
      float radians = i * 2.0f * PI / ledsPerRound;

      // Calculate position
      float x = currentRadius * sinf(radians);
      float y = currentHeight;
      float z = currentRadius * cosf(radians);

      // Add light at calculated position
      addLight(Coord3D(x + middle.x, y + middle.y, z + middle.z));
    }
    nextPin();  // all lights to one pin
  }
};

#endif  // FT_MOONLIGHT