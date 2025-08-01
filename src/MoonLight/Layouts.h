/**
    @title     MoonLight
    @file      Layouts.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

//alphabetically from here, Custom Nodes at the end

class HumanSizedCubeLayout: public Node {
  public:

  static const char * name() {return "Human Sized Cube 🚥🧊💫";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  uint8_t width = 10;
  uint8_t height = 10;
  uint8_t depth = 10;
  // bool snake;
  uint8_t pin = 16;

  void setup() override {
    hasLayout = true;
    
    addControl(width, "width", "range", 1, 20);
    addControl(height, "height", "range", 1, 20);
    addControl(depth, "depth", "range", 1, 20);
    // addControl(snake, "snake", "checkbox");
    addControl(pin, "pin", "pin", 1, 48);
  }

  void addLayout() override {

    //front: z = 0
    for (uint8_t x = 0; x<width; x++) for (uint8_t y = 0; y<height; y++) addLight(Coord3D(x+1, y+1, 0));
    addPin(pin);

    //back: z = depth+1
    for (int x = 0; x<width; x++) for (int y = 0; y<height; y++) addLight(Coord3D(x+1, y+1, depth+1));
    addPin(pin-1); //update with real values (move to controls)

    //above: y = 0
    for (int x = 0; x<width; x++) for (int z = 0; z<depth; z++) addLight(Coord3D(x+1, 0, z+1));
    addPin(pin-2); //update with real values (move to controls)

    //below: y = height+1
    for (int x = 0; x<width; x++) for (int z = 0; z<depth; z++) addLight(Coord3D(x+1, height+1, z+1));
    addPin(pin-3); //update with real values (move to controls)

    //left: x = 0
    for (int z = 0; z<depth; z++) for (int y = 0; y<height; y++) addLight(Coord3D(0, y+1, z+1));
    addPin(pin-4); //update with real values (move to controls)

    //right: x = width+1
    for (int z = 0; z<depth; z++) for (int y = 0; y<height; y++) addLight(Coord3D(width+1, y+1, z+1));
    addPin(pin-5); //update with real values (move to controls)
  }
};

class PanelLayout: public Node {
  public:

  static const char * name() {return "Panel 🚥";}
  static uint8_t dim() {return _2D;}
  static const char * tags() {return "";}

  uint16_t width = 16;
  uint16_t height = 16;
  uint8_t orientation = 1; //first Y increases, then X
  bool incX = true; //Left
  bool incY = true; //Top
  bool snake = true;
  uint8_t pin = 16;

  void setup() override {
    hasLayout = true;
    
    addControl(width, "width", "number", 1, 2047);
    addControl(height, "height", "number", 1, 255);
    JsonObject property;
    JsonArray values;
    property = addControl(orientation, "orientation", "select"); 
    values = property["values"].to<JsonArray>();
    values.add("XY");
    values.add("YX");
    addControl(incX, "X++", "checkbox"); 
    addControl(incY, "Y++", "checkbox"); 
    addControl(snake, "snake", "checkbox");
    addControl(pin, "pin", "pin", 1, 48);
  }

  void addLayout() override {
    const uint8_t axisOrders[6][3] = {
      {1, 0}, // Z (outer), Y (middle), X (inner) -- X fastest
      {0, 1}, // Z, X, Y
    };

    const uint8_t* axes = axisOrders[orientation];
    uint16_t ends[2] = {width, height};
    bool inc[2] = {incX, incY};

    bool iInc = inc[axes[0]];
    for (int i = iInc? 0 : ends[axes[0]]-1 ; iInc? i < ends[axes[0]] : i>=0 ; i+= iInc? 1 : -1) {
      bool jInc = inc[axes[1]];
      if (snake && i%2==1) jInc = !jInc; //reverse order for snake
      for (int j = jInc? 0 : ends[axes[1]]-1 ; jInc? j < ends[axes[1]] : j>=0 ; j+= jInc? 1 : -1) {
            int coords[2];
            coords[axes[0]] = i;
            coords[axes[1]] = j;
            addLight(Coord3D(coords[0], coords[1], 0));
      }
    }

    addPin(pin);
  }

};

class CubeLayout: public Node {
  public:

  static const char * name() {return "Cube 🚥🧊";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  uint16_t width = 10;
  uint16_t height = 10;
  uint16_t depth = 10;
  uint8_t orientation = 3; //first Y increases, then X
  bool incX = true; //Left
  bool incY = true; //Top
  bool incZ = true; //Front
  bool snakeX = false;
  bool snakeY = true;
  bool snakeZ = false;
  uint8_t pin = 16;

  void setup() override {
    hasLayout = true;
    
    addControl(width, "width", "number", 1, 2047);
    addControl(height, "height", "number", 1, 255);
    addControl(depth, "depth", "number", 1, 31);
    JsonObject property;
    JsonArray values;
    property = addControl(orientation, "orientation", "select"); 
    values = property["values"].to<JsonArray>();
    values.add("XYZ");
    values.add("YXZ");
    values.add("XZY");
    values.add("YZX");
    values.add("ZXY");
    values.add("ZYX");
    addControl(incX, "X++", "checkbox"); 
    addControl(incY, "Y++", "checkbox"); 
    addControl(incZ, "Z++", "checkbox"); 
    addControl(snakeX, "snakeX", "checkbox");
    addControl(snakeY, "snakeY", "checkbox");
    addControl(snakeZ, "snakeZ", "checkbox");
    addControl(pin, "pin", "pin", 1, 48);
  }

  void addLayout() override {
    const uint8_t axisOrders[6][3] = {
      {2, 1, 0}, // Z (outer), Y (middle), X (inner) -- X fastest
      {2, 0, 1}, // Z, X, Y
      {1, 2, 0}, // Y, Z, X
      {1, 0, 2}, // Y, X, Z
      {0, 2, 1}, // X, Z, Y
      {0, 1, 2}  // X, Y, Z
    };

    const uint8_t* axes = axisOrders[orientation];
    uint16_t ends[3] = {width, height, depth};
    bool inc[3] = {incX, incY, incZ};
    bool snakes[3] = {snakeX, snakeY, snakeZ};

    bool iInc = inc[axes[0]];
    for (int i = iInc? 0 : ends[axes[0]]-1 ; iInc? i < ends[axes[0]] : i>=0 ; i+= iInc? 1 : -1) {
      bool jInc = inc[axes[1]];
      if (snakes[axes[1]] && i%2==1) jInc = !jInc; //reverse order for snake
      for (int j = jInc? 0 : ends[axes[1]]-1 ; jInc? j < ends[axes[1]] : j>=0 ; j+= jInc? 1 : -1) {
        bool kInc = inc[axes[2]];
        if (snakes[axes[2]] && j%2==1) kInc = !kInc; //reverse order for snake
        for (int k = kInc? 0 : ends[axes[2]]-1 ; kInc? k < ends[axes[2]] : k>=0 ; k+= kInc? 1 : -1) {
            int coords[3];
            coords[axes[0]] = i;
            coords[axes[1]] = j;
            coords[axes[2]] = k;
            addLight(Coord3D(coords[0], coords[1], coords[2]));
        }
      }
    }

    addPin(pin);
  }

};

class PanelsLayout: public Node {
  public:

  static const char * name() {return "Panels 🚥";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  uint8_t  horizontalPanels = 1;
  uint8_t  verticalPanels = 4;
  uint8_t  panelWidth = 32;
  uint8_t  panelHeight = 8;
  bool snake = true;
  uint8_t pin = 16;

  void setup() override {
    hasLayout = true;
    
    addControl(horizontalPanels, "horizontalPanels", "range", 1, 32);
    addControl(verticalPanels, "verticalPanels", "range", 1, 32);
    addControl(panelWidth, "panelWidth", "range", 1, 64);
    addControl(panelHeight, "panelHeight", "range", 1, 64);
    addControl(snake, "snake", "checkbox");
    addControl(pin, "pin", "pin", 1, 48);
  }

  void addLayout() override {

    for (int panelY = verticalPanels - 1; panelY >=0; panelY--) {

      for (int panelX = horizontalPanels-1; panelX >=0; panelX--) {

        for (int x=panelWidth-1; x>=0; x--) {
          for (int y=panelHeight-1; y>=0; y--) {
            int y2 = y; if (snake && x%2 == 0) {y2 = panelHeight - 1 - y;}
            addLight(Coord3D(panelX * panelWidth + x, panelY * panelHeight + y2, 0));
          }
        }

      }

      addPin(pin);

    }
  }
};

class SingleLineLayout: public Node {
  public:

  static const char * name() {return "Single Line 🚥";}
  static uint8_t dim() {return _2D;}
  static const char * tags() {return "";}

  uint8_t start_x = 0;
  uint8_t width = 1;
  uint16_t yposition = 0;
  uint8_t pin = 16;
  bool reversed_order = false;

  void setup() override {
    hasLayout = true;

    addControl(start_x, "starting X", "range", 0, 255);
    addControl(width, "width", "range", 1, 255);
    addControl(yposition, "Y position", "number", 0, 255); 
    addControl(reversed_order, "reversed order", "checkbox");
    addControl(pin, "pin", "pin", 1, 48);
  }

  void addLayout() override {
    if (reversed_order){
      for (int x = start_x+width-1; x>=start_x; x--) {
        addLight(Coord3D(x, yposition, 0));
      }
    }
    else {
      for (uint8_t x = start_x; x<start_x+width; x++) {
        addLight(Coord3D(x, yposition, 0));
      }
    }
    addPin(pin);
  }

};

class SingleRowLayout: public Node {
  public:

  static const char * name() {return "Single Row 🚥";}
  static uint8_t dim() {return _2D;}
  static const char * tags() {return "";}

  uint8_t start_y = 0;
  uint8_t height = 1;
  uint16_t xposition = 0;
  uint8_t pin = 16;
  bool reversed_order = false;


  void setup() override {
    hasLayout = true;
    addControl(start_y, "starting Y", "range", 0, 255);
    addControl(height, "height", "range", 1, 255);
    addControl(xposition, "X position", "number", 0, 255); 
    addControl(reversed_order, "reversed order", "checkbox");
    addControl(pin, "pin", "pin", 1, 48);
  }

  void addLayout() override {
    if (reversed_order){
      for (int y = start_y+height-1; y>=start_y; y--) {
        addLight(Coord3D(xposition, y, 0));
      }
    }
    else {
      for (uint8_t y = start_y; y<start_y+height; y++) {
        addLight(Coord3D(xposition, y, 0));
      }
    }
    addPin(pin);
  }
};

class RingsLayout: public Node {
  public:

  static const char * name() {return "Rings 🚥";}
  static uint8_t dim() {return _2D;}
  static const char * tags() {return "";}

  uint8_t pin = 16;

  uint8_t width = 16;
  uint8_t height = 16;
  
  void setup() override {
    hasLayout = true;

    addControl(pin, "pin", "pin", 1, 48);
  }

  void add(int leds, int radius) {

    for (int i = 0; i<leds; i++) {
      uint8_t x = width / 2.0;
      uint8_t y = height / 2.0;
      if (leds != 1) {
        x = width / 2.0 + ((sin8(255 * i / (leds-1)) - 127) / 127.0) * radius / 10.0;
        y = height / 2.0 + ((cos8(255 * i / (leds-1)) - 127) / 127.0) * radius / 10.0;
      }
      addLight({x, y, 0});
    }
  }

  void addLayout() override{
    
    add(1, 0);
    add(8, 13);
    add(12, 23);
    add(16, 33);
    add(24, 43);
    add(32, 53);
    add(40, 63);
    add(48, 73);
    add(60, 83);

    addPin(pin);
  }
};

class WheelLayout: public Node {
  public:

  static const char * name() {return "Wheel 🚥";}
  static uint8_t dim() {return _2D;}
  static const char * tags() {return "";}

  uint8_t pin = 16;

  uint8_t nrOfSpokes = 12;
  uint8_t ledsPerSpoke = 16;
  
  void setup() override {
    hasLayout = true;

    addControl(nrOfSpokes, "nrOfSpokes", "range", 1, 48);
    addControl(ledsPerSpoke, "ledsPerSpoke", "range", 1, 255);
    addControl(pin, "pin", "pin", 1, 48);
  }

  void addLayout() override {
    // uint16_t size = ledsPerSpoke * 2;
    Coord3D middle;
    middle.x = ledsPerSpoke;
    middle.y = ledsPerSpoke;
    middle.z = 0; // size >> 1; this is for later 🔥
    for (int i=0; i<nrOfSpokes; i++) {
      float radians = i*360/nrOfSpokes * DEG_TO_RAD;
      for (int j=0; j<ledsPerSpoke; j++) {
        float radius = j + 1;
        float x = radius * sinf(radians);
        float y = radius * cosf(radians);
        addLight(Coord3D(x + middle.x, y + middle.y, middle.z));
      }
    }
  }
};

//custom layouts

//SE16 board
class SE16Layout: public Node {
  public:

  static const char * name() {return "SE16 🚥";}
  static uint8_t dim() {return _2D;}
  static const char * tags() {return "";}

  bool mirroredPins = false;
  bool pinsAreColumns = false;
  uint16_t ledsPerPin = 10;

  void setup() override {
    hasLayout = true;
    
    addControl(mirroredPins, "mirroredPins", "checkbox");
    addControl(pinsAreColumns, "pinsAreColumns", "checkbox");
    addControl(ledsPerPin, "ledsPerPin", "number", 1, 2047);
  }

  void addStrip( uint16_t xposition, uint16_t start_y,  uint16_t stop_y, uint8_t pin) {

    bool increasing = start_y < stop_y;
    for (int y = start_y; increasing ? (y <= stop_y) : (y >= stop_y); y += increasing?1:-1) {
      if (pinsAreColumns)
        addLight(Coord3D(xposition, y, 0)); //limpkin
      else
        addLight(Coord3D(y, xposition, 0)); //ewowi
    }

    addPin(pin);
  }

  void addLayout() override {

    //pin layout of the board
    // 47-48
    // 21-38
    // 14-39
    // 13-40
    // 12-41
    // 11-42
    // 10-02
    // 03-01

    if (mirroredPins) { //limpkin
      addStrip(7, ledsPerPin, 2*ledsPerPin-1, 47); addStrip(7, ledsPerPin-1, 0, 48);
      addStrip(6, ledsPerPin, 2*ledsPerPin-1, 21); addStrip(6, ledsPerPin-1, 0, 38);
      addStrip(5, ledsPerPin, 2*ledsPerPin-1, 14); addStrip(5, ledsPerPin-1, 0, 39);
      addStrip(4, ledsPerPin, 2*ledsPerPin-1, 13); addStrip(4, ledsPerPin-1, 0, 40);
      addStrip(3, ledsPerPin, 2*ledsPerPin-1, 12); addStrip(3, ledsPerPin-1, 0, 41);
      addStrip(2, ledsPerPin, 2*ledsPerPin-1, 11); addStrip(2, ledsPerPin-1, 0, 42);
      addStrip(1, ledsPerPin, 2*ledsPerPin-1, 10); addStrip(1, ledsPerPin-1, 0, 2);
      addStrip(0, ledsPerPin, 2*ledsPerPin-1, 3);  addStrip(0, ledsPerPin-1, 0, 1);
    } else { //ewowi
      addStrip(14, 0, ledsPerPin-1, 16); addStrip(15, 0, ledsPerPin-1, 48);
      addStrip(12, 0, ledsPerPin-1, 21); addStrip(13, 0, ledsPerPin-1, 38);
      addStrip(10, 0, ledsPerPin-1, 14); addStrip(11, 0, ledsPerPin-1, 39);
      addStrip(8, 0, ledsPerPin-1, 13); addStrip(9, 0, ledsPerPin-1, 40);
      addStrip(6, 0, ledsPerPin-1, 12); addStrip(7, 0, ledsPerPin-1, 41);
      addStrip(4, 0, ledsPerPin-1, 11); addStrip(5, 0, ledsPerPin-1, 42);
      addStrip(2, 0, ledsPerPin-1, 10); addStrip(3, 0, ledsPerPin-1, 2);
      addStrip(0, 0, ledsPerPin-1, 3);  addStrip(1, 0, ledsPerPin-1, 1);
    }
  }
};

#endif //FT_MOONLIGHT