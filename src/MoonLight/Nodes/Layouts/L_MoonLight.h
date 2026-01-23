/**
    @title     MoonLight
    @file      L_MoonLight.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
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

  template <typename Callback>
  void iterate(int axis, int snaker, Callback&& fun) {
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

class TorontoBarGourdsLayout : public Node {
 public:
  static const char* name() { return "Toronto Bar Gourds"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🚥"; }

  // Coord3D size;
  uint8_t nrOfLightsPerGourd = 61;  // 5*12+1; mode 0 only
  uint8_t granularity = 2;          // One LED One Light

  void setup() override {
    addControl(granularity, "granularity", "select");
    addControlValue("One Gourd One Light");
    addControlValue("One Side One Light");
    addControlValue("One LED One Light");
    addControl(nrOfLightsPerGourd, "nrOfLightsPerGourd", "slider", 1, 128, "One gourd");  // mode 0 only
  }

  // One Gourd One Light
  void addGourd(Coord3D pos) {
    for (int i = 0; i < nrOfLightsPerGourd; i++) addLight(Coord3D(pos.x, pos.y, pos.z));  // all gourd lights on the same position for the time being
  }

  // One Side One Light
  void addGourdSides(Coord3D pos) {
    const uint8_t gourdLength = 3;  // each side can be mapped in a 3 * 3 * 3 grid (27 leds), 5 sides + 1 middle LED
    Coord3D sides[] = {Coord3D(1, 1, 0), Coord3D(2, 1, 1), Coord3D(1, 1, 2), Coord3D(0, 1, 1), Coord3D(1, 2, 1)};

    for (Coord3D side : sides) {                                                                                                                 // 5 sides
      for (int i = 0; i < 12; i++) addLight(Coord3D(pos.x * gourdLength + side.x, pos.y * gourdLength + side.y, pos.z * gourdLength + side.z));  // each side has 12 leds, all mapped to the same virtual pixel
    }

    // + middleLED
    Coord3D side = {1, 1, 1};
    addLight(Coord3D(pos.x * gourdLength + side.x, pos.y * gourdLength + side.y, pos.z * gourdLength + side.z));  // middleLED
  }

  // One LED One Light
  void addGourdPixels(Coord3D pos) {
    const uint8_t gourdLength = 7;
    Coord3D pixels[] = {Coord3D(0, 0), Coord3D(1, 0), Coord3D(2, 0), Coord3D(3, 0), Coord3D(3, 1), Coord3D(3, 2), Coord3D(3, 3), Coord3D(2, 3), Coord3D(1, 3), Coord3D(0, 3), Coord3D(0, 2), Coord3D(0, 1)};  // 12 pixels each

    // back and front: z constant, increasing x and y
    for (Coord3D pixel : pixels) addLight(Coord3D(pos.x * gourdLength + pixel.x, pos.y * gourdLength + pixel.y, pos.z * gourdLength));                    // front
    for (Coord3D pixel : pixels) addLight(Coord3D(pos.x * gourdLength + pixel.x, pos.y * gourdLength + pixel.y, pos.z * gourdLength + gourdLength - 1));  // back

    // left and right: x constant, increasing y and z
    for (Coord3D pixel : pixels) addLight(Coord3D(pos.x * gourdLength, pos.y * gourdLength + pixel.x, pos.z * gourdLength + pixel.y));                    // left
    for (Coord3D pixel : pixels) addLight(Coord3D(pos.x * gourdLength + gourdLength - 1, pos.y * gourdLength + pixel.x, pos.z * gourdLength + pixel.y));  // right

    // bottom : y constant: increasing x and z
    for (Coord3D pixel : pixels) addLight(Coord3D(pos.x * gourdLength + pixel.x, pos.y * gourdLength + gourdLength - 1, pos.z * gourdLength + pixel.y));  // bottom

    // + middleLED
    Coord3D middle = {3, 3, 3};
    addLight(Coord3D(pos.x * gourdLength + middle.x, pos.y * gourdLength + middle.y, pos.z * gourdLength + middle.z));  // middleLED
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    // modify to whatever gourd order and nr of gourds in a 3D grid space
    Coord3D gourds[] = {
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
        Coord3D(0, 0, 1),
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

    uint8_t gourdCounter = 0;

    for (Coord3D gourd : gourds) {
      if (granularity == 0) {  // one gourd one light
        addGourd(gourd);
      } else if (granularity == 1) {  // one side one light
        addGourdSides(gourd);
      } else if (granularity == 2) {  // one LED one light
        addGourdPixels(gourd);
      }

      gourdCounter++;
      if (gourdCounter % 10 == 0) nextPin();  // will not be used by Art-Net but in case of using a LED driver, every 10 gourds (61 LEDs each) will be on a separate pin
    }

    if (gourdCounter % 10 != 0) nextPin();  // add final pin
  }
};

class SingleRowLayout : public Node {
 public:
  static const char* name() { return "Single Row"; }
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

class SingleColumnLayout : public Node {
 public:
  static const char* name() { return "Single Column"; }
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

  Coord3D ringCenter;
  uint8_t nrOfLEDs = 24;
  uint16_t angleFirst = 0;  // top
  uint16_t rotation = 360;
  uint8_t scale = 1;
  bool clockwise = true;
  bool doNextPin = true;  // add the lights to a pin is default. not in UI, but when RingLayout is used in other layouts it can be switched off so these layouts control the pin assignment

  void setup() override {
    addControl(nrOfLEDs, "nrOfLEDs", "slider", 1, 255);
    addControl(angleFirst, "angleFirst", "slider", 0, 359);
    addControl(rotation, "rotation", "slider", 0, 360);
    addControl(clockwise, "clockwise", "boolean");
    addControl(scale, "scale", "slider", 1, 10);
  }

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) {
    // add your custom onUpdate code here
    if (control["name"] == "nrOfLEDs") {
      ringCenter.x = 1.1 * getRadius(nrOfLEDs);
      ringCenter.y = 1.1 * getRadius(nrOfLEDs);
      ringCenter.z = 0;
    }
  }

  bool hasOnLayout() const override { return true; }

  const float getRadius(uint8_t nrOfLEDs) { return nrOfLEDs / (TWO_PI); }

  void onLayout() override {
    // Calculate ring dimensions based on 1cm * scale spacing between LEDs

    for (int i = 0; i < nrOfLEDs; i++) {
      float x = scale * ringCenter.x;
      float y = scale * ringCenter.y;

      // Calculate angle for this LED including the angleFirst offset
      float ledAngle = fmod(angleFirst + ((float)i / nrOfLEDs) * 360.0, 360.0);

      // Calculate the actual position angle for rendering
      float angleRad = PI + (TWO_PI * i) / nrOfLEDs + TWO_PI * angleFirst / 360.0;

      if (nrOfLEDs != 1) {
        x -= scale * sinf(angleRad) * getRadius(nrOfLEDs);
        y += scale * cosf(angleRad) * getRadius(nrOfLEDs);
      }

      // Check if this LED should be included in the partial circle
      bool includeLED = false;
      if (rotation < 1.0 || rotation >= 360.0) {
        // Full circle
        includeLED = true;
      } else {
        // Calculate end angle
        float endAngle;
        if (clockwise) {
          endAngle = fmod(angleFirst + rotation, 360.0);
        } else {
          endAngle = fmod(angleFirst - rotation + 360.0, 360.0);
        }

        // Check if ledAngle is between angleFirst and endAngle
        if (clockwise) {
          if (endAngle >= angleFirst) {
            // No wrap: angleFirst=180, rotation=90, endAngle=270
            includeLED = (ledAngle >= angleFirst && ledAngle <= endAngle);
          } else {
            // Wraps around 0: angleFirst=270, rotation=180, endAngle=90
            includeLED = (ledAngle >= angleFirst || ledAngle <= endAngle);
          }
        } else {
          if (endAngle <= angleFirst) {
            // No wrap: angleFirst=180, rotation=90, endAngle=90
            includeLED = (ledAngle <= angleFirst && ledAngle >= endAngle);
          } else {
            // Wraps around 0: angleFirst=90, rotation=180, endAngle=270
            includeLED = (ledAngle <= angleFirst || ledAngle >= endAngle);
          }
        }
      }

      if (includeLED) {
        addLight({(int)x, (int)y, scale * ringCenter.z});
      }
    }
    if (doNextPin) nextPin();  // all lights to one pin
  }
};

class Rings16Layout : public Node {
 public:
  static const char* name() { return "16 Rings"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🚥"; }

  uint8_t scale = 1;
  
  void setup() override {
    addControl(scale, "scale", "slider", 1, 10);
  }

  bool hasOnLayout() const override { return true; }

  void onLayout() override {
    RingLayout ringLayout;
    ringLayout.scale = scale;

    uint8_t rows[] = {5, 10, 17, 23, 28, 34, 39, 46, 53};
    uint8_t cols[] = {10, 22, 35, 41, 47, 59, 70};
    ringLayout.ringCenter = {cols[5], rows[3], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[6], rows[4], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[5], rows[1], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[5], rows[5], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[4], rows[2], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[5], rows[7], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[3], rows[0], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[4], rows[6], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[2], rows[2], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[3], rows[8], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[1], rows[1], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[2], rows[6], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[1], rows[3], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[1], rows[7], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[0], rows[4], 0};
    ringLayout.onLayout();
    ringLayout.ringCenter = {cols[1], rows[5], 0};
    ringLayout.onLayout();
  }
};

class Rings241Layout : public Node {
 public:
  static const char* name() { return "Rings 241"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🚥"; }

  uint8_t scale = 2;

  void setup() override { addControl(scale, "scale", "slider", 1, 10); }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    RingLayout ringLayout;
    ringLayout.doNextPin = false;
    ringLayout.angleFirst = 0;
    ringLayout.scale = scale;
    ringLayout.rotation = 360;

    uint8_t leftMargin = 1.1 * ringLayout.getRadius(60);

    // inner light left
    ringLayout.ringCenter = {leftMargin, leftMargin, 0};
    ringLayout.nrOfLEDs = 1;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 8;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 12;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 16;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 24;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 32;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 40;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 48;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 60;
    ringLayout.onLayout();

    nextPin();  // all lights to one pin
  }
};

class CarLightsLayout : public Node {
 public:
  static const char* name() { return "Car Lights"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🚥"; }

  // uint8_t nrOfSpokes = 12;
  uint8_t scale = 2;

  void setup() override {
    // addControl(nrOfSpokes, "nrOfSpokes", "slider", 1, 48);
    // addControl(ledsPerSpoke, "ledsPerSpoke", "slider", 1, 255);
    addControl(scale, "scale", "slider", 1, 10);
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    RingLayout ringLayout;
    ringLayout.doNextPin = false;
    ringLayout.angleFirst = 90;
    ringLayout.scale = scale;
    ringLayout.rotation = 360;

    uint8_t leftMargin = 9;

    // inner light left
    ringLayout.ringCenter = {leftMargin + 11, 10, 0};
    ringLayout.nrOfLEDs = 1;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 8;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 12;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 16;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 24;
    ringLayout.onLayout();

    // outer light left
    ringLayout.ringCenter = {leftMargin, 6, 0};
    ringLayout.nrOfLEDs = 1;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 8;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 12;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 16;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 24;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 34;
    ringLayout.onLayout();

    nextPin();

    // inner light right
    ringLayout.ringCenter = {leftMargin + 25, 10, 0};
    ringLayout.nrOfLEDs = 1;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 8;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 12;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 16;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 24;
    ringLayout.onLayout();

    // outer light right
    ringLayout.ringCenter = {leftMargin + 36, 6, 0};
    ringLayout.nrOfLEDs = 1;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 8;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 12;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 16;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 24;
    ringLayout.onLayout();
    ringLayout.nrOfLEDs = 34;
    ringLayout.onLayout();

    nextPin();

    // left strip
    for (int x = (leftMargin + 16) * scale; x >= leftMargin * scale; x--) {
      addLight({x, 15 * scale});
    }
    ringLayout.ringCenter = {leftMargin, 6, 0};
    ringLayout.nrOfLEDs = 52;
    ringLayout.angleFirst = 180;
    ringLayout.rotation = 90;
    ringLayout.onLayout();

    for (int y = 5; y >= 1; y--) {
      addLight({0, y * scale});
    }

    nextPin();

    // right strip
    for (int x = (leftMargin + 19) * scale; x <= (leftMargin + 35) * scale; x++) {
      addLight({x, 15 * scale});
    }

    ringLayout.ringCenter = {leftMargin + 36, 6, 0};
    ringLayout.nrOfLEDs = 52;
    ringLayout.angleFirst = 180;
    ringLayout.rotation = 90;
    ringLayout.clockwise = false;
    ringLayout.onLayout();

    for (int y = 5; y >= 1; y--) {
      addLight({(leftMargin + 44) * scale, y * scale});
    }

    nextPin();
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