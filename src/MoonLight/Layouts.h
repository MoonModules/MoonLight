/**
    @title     MoonLight
    @file      Layouts.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/general/utilities/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact moonmodules@icloud.com
**/

#if FT_MOONLIGHT

//alphabetically from here

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
    Node::setup();
    
    addControl(width, "width", "range", 1, 20);
    addControl(height, "height", "range", 1, 20);
    addControl(depth, "depth", "range", 1, 20);
    // addControl(snake, "snake", "checkbox");
    addControl(pin, "pin", "pin", 1, 48);
  }

  void addLayout() override {

    //front: z = 0
    for (uint8_t x = 0; x<width; x++) for (uint8_t y = 0; y<height; y++) addLight(intToCoord3D(x+1, y+1, 0));
    addPin(pin);

    //back: z = depth+1
    for (int x = 0; x<width; x++) for (int y = 0; y<height; y++) addLight(intToCoord3D(x+1, y+1, depth+1));
    addPin(pin-1); //update with real values (move to controls)

    //above: y = 0
    for (int x = 0; x<width; x++) for (int z = 0; z<depth; z++) addLight(intToCoord3D(x+1, 0, z+1));
    addPin(pin-2); //update with real values (move to controls)

    //below: y = height+1
    for (int x = 0; x<width; x++) for (int z = 0; z<depth; z++) addLight(intToCoord3D(x+1, height+1, z+1));
    addPin(pin-3); //update with real values (move to controls)

    //left: x = 0
    for (int z = 0; z<depth; z++) for (int y = 0; y<height; y++) addLight(intToCoord3D(0, y+1, z+1));
    addPin(pin-4); //update with real values (move to controls)

    //right: x = width+1
    for (int z = 0; z<depth; z++) for (int y = 0; y<height; y++) addLight(intToCoord3D(width+1, y+1, z+1));
    addPin(pin-5); //update with real values (move to controls)
  }
};

class PanelLayout: public Node {
  public:

  static const char * name() {return "Panel 🚥";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  uint8_t width = 16;
  uint8_t height = 16;
  uint8_t depth = 1;
  bool snake = true;
  uint8_t pin = 16;

  void setup() override {
    hasLayout = true;
    Node::setup();
    
    addControl(width, "width", "range", 1, 32);
    addControl(height, "height", "range", 1, 32);
    addControl(depth, "depth", "range", 1, 32);
    addControl(snake, "snake", "checkbox");
    addControl(pin, "pin", "pin", 1, 48);
  }

  void addLayout() override {
    
    for (uint8_t x = 0; x<width; x++) {
      for (uint8_t y = 0; y<height; y++) {
        for (uint8_t z = 0; z<depth; z++) {
          addLight(intToCoord3D(x, (x%2 || !snake)?y:height-1-y, z));
        }
      }
    }
    addPin(pin);
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
    Node::setup();
    
    
    addControl(start_x, "starting X", "range", 0, 255);
    addControl(width, "width", "range", 1, 255);
    addControl(yposition, "Y position", "number", 0, 255); 
    addControl(reversed_order, "reversed order", "checkbox");
    addControl(pin, "pin", "pin", 1, 48);
  }

  void addLayout() override {
    if (reversed_order){
      for (int x = start_x+width-1; x>=start_x; x--) {
        addLight(intToCoord3D(x, yposition, 0));
      }
    }
    else {
      for (uint8_t x = start_x; x<start_x+width; x++) {
        addLight(intToCoord3D(x, yposition, 0));
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
    Node::setup();
    
    addControl(start_y, "starting Y", "range", 0, 255);
    addControl(height, "height", "range", 1, 255);
    addControl(xposition, "X position", "number", 0, 255); 
    addControl(reversed_order, "reversed order", "checkbox");
    addControl(pin, "pin", "pin", 1, 48);
  }

  void addLayout() override {
    if (reversed_order){
      for (int y = start_y+height-1; y>=start_y; y--) {
        addLight(intToCoord3D(xposition, y, 0));
      }
    }
    else {
      for (uint8_t y = start_y; y<start_y+height; y++) {
        addLight(intToCoord3D(xposition, y, 0));
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
    Node::setup();

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

#endif //FT_MOONLIGHT