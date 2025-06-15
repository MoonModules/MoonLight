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

  static const char * name() {return "Human Sized Cube 🚥 🧊💫";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  uint8_t width;
  uint8_t height;
  uint8_t depth;
  // bool snake;
  uint8_t pin;

  void setup() override {
    hasLayout = true;
    Node::setup();
    
    addControl(&width, "width", "range", 10, 1, 20);
    addControl(&height, "height", "range", 10, 1, 20);
    addControl(&depth, "depth", "range", 10, 1, 20);
    // addControl(&snake, "snake", "checkbox", true);
    addControl(&pin, "pin", "number", 16, 1, 48);
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

class SingleLineLayout: public Node {
  public:

  static const char * name() {return "Single Line 🚥";}

  uint8_t yposition;
  uint8_t width;
  uint8_t pin;

  void setup() override {
    hasLayout = true;
    Node::setup();
    
    addControl(&width, "width", "range", 4, 1, 32); 
    addControl(&yposition, "Y position", "number", 0, 0, 16); 
    addControl(&pin, "pin", "number", 16, 1, 48);

    layerV->layerP->lights.header.channelsPerLight = 24;
    layerV->layerP->lights.header.offsetPan = 0;
    layerV->layerP->lights.header.offsetTilt = 1;
    layerV->layerP->lights.header.offsetBrightness = 3;
    layerV->layerP->lights.header.offsetRGB = 4;
    layerV->layerP->lights.header.offsetZoom = 17;
  }

  void addLayout() override {
    for (uint8_t x = 0; x<width; x++) {
      addLight({x, yposition, 0});
    }
    addPin(pin); //needed to slow down the dmx stream ... wip
  }

};

class SingleRowLayout: public Node {
  public:

  static const char * name() {return "Single Row 🚥";}

  uint8_t xposition;
  uint8_t height;
  uint8_t pin;

  void setup() override {
    hasLayout = true;
    Node::setup();
    
    addControl(&height, "height", "range", 4, 1, 32); 
    addControl(&xposition, "X position", "number", 0, 0, 16); 
    addControl(&pin, "pin", "number", 16, 1, 48);

    layerV->layerP->lights.header.channelsPerLight = 24;
    layerV->layerP->lights.header.offsetPan = 0;
    layerV->layerP->lights.header.offsetTilt = 1;
    layerV->layerP->lights.header.offsetBrightness = 3;
    layerV->layerP->lights.header.offsetRGB = 4;
    layerV->layerP->lights.header.offsetZoom = 17;
  }

  void addLayout() override {
    for (uint8_t y = 0; y<height; y++) {
      addLight({xposition, y, 0});
    }
    addPin(pin); //needed to slow down the dmx stream ... wip
  }

};

class PanelLayout: public Node {
  public:

  static const char * name() {return "Panel 🚥";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  uint8_t width;
  uint8_t height;
  uint8_t depth;
  bool snake;
  uint8_t pin;

  void setup() override {
    hasLayout = true;
    Node::setup();
    
    addControl(&width, "width", "range", 16, 1, 32);
    addControl(&height, "height", "range", 16, 1, 32);
    addControl(&depth, "depth", "range", 1, 1, 32);
    addControl(&snake, "snake", "checkbox", true);
    addControl(&pin, "pin", "number", 16, 1, 48);
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

class RingsLayout: public Node {
  public:

  static const char * name() {return "Rings 🚥";}
  static uint8_t dim() {return _2D;}
  static const char * tags() {return "";}

  uint8_t pin;

  uint8_t width = 16;
  uint8_t height = 16;
  
  void setup() override {
    hasLayout = true;
    Node::setup();

    addControl(&pin, "pin", "number", 2, 1, 48);
  }

  void add(int leds, int radius) {
    for (int i = 0; i<leds; i++) {
      uint8_t x = width / 2.0 + ((sin8(255 * i / leds) - 127) / 127.0) * radius / 10.0;
      uint8_t y = height / 2.0 + ((cos8(255 * i / leds) - 127) / 127.0) * radius / 10.0;
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