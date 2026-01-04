/**
    @title     MoonLight
    @file      VirtualLayer.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
           https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

  #include <Arduino.h>
  #include <FastLED.h>

  #include <vector>

  #include "PhysicalLayer.h"

enum MapTypeEnum {
  m_zeroLights,
  m_oneLight,
  m_moreLights,
  m_count  // keep as last entry
};

struct PhysMap {
  union {
  #ifdef BOARD_HAS_PSRAM
    struct {
      uint8_t rgb[3];
      uint8_t mapType;
    };
    struct {
      uint32_t indexP : 24;
      uint32_t mapType_unused1 : 8;
    };
    struct {
      uint32_t indexesIndex : 24;
      uint32_t mapType_unused2 : 8;
    };
    uint32_t raw;
  #else
    // 2 bytes struct
    struct {                // condensed rgb
      uint16_t rgb : 14;    // 14 bits (554 RGB)
      uint16_t mapType : 2;  // 2 bits (4)
    };  // 16 bits
    uint16_t indexP : 14;        // 16384 one physical light (type==1) index to ledsP array
    uint16_t indexesIndex : 14;  // 16384 multiple physical lights (type==2) index in std::vector<std::vector<nrOfLights_t>> mappingTableIndexes;
  #endif
  };

  PhysMap() {
    // EXT_LOGV(ML_TAG, "Constructor");
    mapType = m_zeroLights;  // the default until indexP is added
  #ifdef BOARD_HAS_PSRAM
    memset(rgb, 0, 3);
  #else
    rgb = 0;
  #endif
  }
};

  #define _0D 0
  #define _1D 1
  #define _2D 2
  #define _3D 3
  #define _NoD 4

class VirtualLayer {
 public:
  nrOfLights_t nrOfLights = 256;
  Coord3D size = {16, 16, 1};                                    // not 0,0,0 to prevent div0 eg in Octopus2D
  Coord3D start = {0, 0, 0}, middle = size / 2, end = size - 1;  //{UINT16_MAX,UINT16_MAX,UINT16_MAX}; //default

  // they will be reused to avoid fragmentation
  PhysMap* mappingTable = nullptr;
  nrOfLights_t mappingTableSize = 0;
  std::vector<std::vector<nrOfLights_t>, VectorRAMAllocator<std::vector<nrOfLights_t> > > mappingTableIndexes;
  nrOfLights_t mappingTableIndexesSizeUsed = 0;

  PhysicalLayer* layerP;  // physical LEDs the virtual LEDs are mapped to
  std::vector<Node*, VectorRAMAllocator<Node*> > nodes;

  uint8_t fadeMin;

  uint8_t effectDimension = _3D;  // assuming 3D for the moment
  uint8_t layerDimension = UINT8_MAX;

  Coord3D prevSize;  // to calculate size change

  VirtualLayer();

  ~VirtualLayer();

  void setup();
  void loop();
  void loop20ms();

  void addIndexP(PhysMap& physMap, nrOfLights_t indexP);

  // position is by reference as within XYZ, the position can be modified, for efficiency reasons, don't declare an extra variable for that in XYZ
  nrOfLights_t XYZ(Coord3D& position);

  nrOfLights_t XYZUnModified(const Coord3D& position) const { return position.x + position.y * size.x + position.z * size.x * size.y; }

  void setRGB(const nrOfLights_t indexV, CRGB color) {
    if (layerP->lights.header.offsetWhite != UINT8_MAX && layerP->lights.header.channelsPerLight == 4) {  // W set
      // using the simple algorithm of taking the minimum of RGB as white channel, this is good enough and fastest algorithm - we need speed 🔥
      uint8_t rgbw[4];
      rgbw[3] = MIN(MIN(color.r, color.g), color.b);
      rgbw[0] = color.red - rgbw[3];  // subtract from other channels
      rgbw[1] = color.green - rgbw[3];
      rgbw[2] = color.blue - rgbw[3];
      setLight(indexV, rgbw, layerP->lights.header.offsetRGB, sizeof(rgbw));
    } else
      setLight(indexV, color.raw, layerP->lights.header.offsetRGB, sizeof(color));
  }
  void setRGB(Coord3D pos, CRGB color) { setRGB(XYZ(pos), color); }

  void setWhite(const nrOfLights_t indexV, const uint8_t value) {
    if (layerP->lights.header.offsetWhite != UINT8_MAX) {
      if (layerP->lights.header.channelsPerLight == 4)
        setLight(indexV, &value, 3, sizeof(value));  // for rgbw lights
      else
        setLight(indexV, &value, layerP->lights.header.offsetWhite, sizeof(value));  // for moving heads
    }
  }
  void setWhite(Coord3D pos, const uint8_t value) { setWhite(XYZ(pos), value); }

  void setBrightness(const nrOfLights_t indexV, uint8_t value) {
    value = (value * layerP->lights.header.brightness) / 255;
    if (layerP->lights.header.offsetBrightness != UINT8_MAX) setLight(indexV, &value, layerP->lights.header.offsetBrightness, sizeof(value));
  }
  void setBrightness(Coord3D pos, const uint8_t value) { setBrightness(XYZ(pos), value); }

  void setPan(const nrOfLights_t indexV, const uint8_t value) {
    if (layerP->lights.header.offsetPan != UINT8_MAX) setLight(indexV, &value, layerP->lights.header.offsetPan, sizeof(value));
  }
  void setPan(Coord3D pos, const uint8_t value) { setPan(XYZ(pos), value); }

  void setTilt(const nrOfLights_t indexV, const uint8_t value) {
    if (layerP->lights.header.offsetTilt != UINT8_MAX) setLight(indexV, &value, layerP->lights.header.offsetTilt, sizeof(value));
  }
  void setTilt(Coord3D pos, const uint8_t value) { setTilt(XYZ(pos), value); }

  void setZoom(const nrOfLights_t indexV, const uint8_t value) {
    if (layerP->lights.header.offsetZoom != UINT8_MAX) setLight(indexV, &value, layerP->lights.header.offsetZoom, sizeof(value));
  }
  void setZoom(Coord3D pos, const uint8_t value) { setZoom(XYZ(pos), value); }

  void setRotate(const nrOfLights_t indexV, const uint8_t value) {
    if (layerP->lights.header.offsetRotate != UINT8_MAX) setLight(indexV, &value, layerP->lights.header.offsetRotate, sizeof(value));
  }
  void setRotate(Coord3D pos, const uint8_t value) { setRotate(XYZ(pos), value); }

  void setGobo(const nrOfLights_t indexV, const uint8_t value) {
    if (layerP->lights.header.offsetGobo != UINT8_MAX) setLight(indexV, &value, layerP->lights.header.offsetGobo, sizeof(value));
  }
  void setGobo(Coord3D pos, const uint8_t value) { setGobo(XYZ(pos), value); }

  void setRGB1(const nrOfLights_t indexV, CRGB color) {
    if (layerP->lights.header.offsetRGB1 != UINT8_MAX) setLight(indexV, color.raw, layerP->lights.header.offsetRGB1, sizeof(color));
  }
  void setRGB1(Coord3D pos, CRGB color) { setRGB1(XYZ(pos), color); }

  void setRGB2(const nrOfLights_t indexV, CRGB color) {
    if (layerP->lights.header.offsetRGB2 != UINT8_MAX) setLight(indexV, color.raw, layerP->lights.header.offsetRGB2, sizeof(color));
  }
  void setRGB2(Coord3D pos, CRGB color) { setRGB2(XYZ(pos), color); }

  void setRGB3(const nrOfLights_t indexV, CRGB color) {
    if (layerP->lights.header.offsetRGB3 != UINT8_MAX) setLight(indexV, color.raw, layerP->lights.header.offsetRGB3, sizeof(color));
  }
  void setRGB3(Coord3D pos, CRGB color) { setRGB3(XYZ(pos), color); }

  void setBrightness2(const nrOfLights_t indexV, uint8_t value) {
    value = (value * layerP->lights.header.brightness) / 255;
    if (layerP->lights.header.offsetBrightness2 != UINT8_MAX) setLight(indexV, &value, layerP->lights.header.offsetBrightness2, sizeof(value));
  }
  void setBrightness2(Coord3D pos, const uint8_t value) { setBrightness2(XYZ(pos), value); }

  void setLight(const nrOfLights_t indexV, const uint8_t* channels, uint8_t offset, uint8_t length);

  CRGB getRGB(const nrOfLights_t indexV) { return getLight<CRGB>(indexV, layerP->lights.header.offsetRGB); }
  CRGB getRGB(Coord3D pos) { return getRGB(XYZ(pos)); }

  void addRGB(const Coord3D& position, const CRGB& color) { setRGB(position, getRGB(position) + color); }

  void blendColor(const nrOfLights_t indexV, const CRGB& color, uint8_t blendAmount) { setRGB(indexV, blend(color, getRGB(indexV), blendAmount)); }
  void blendColor(Coord3D position, const CRGB& color, const uint8_t blendAmount) { blendColor(XYZ(position), color, blendAmount); }

  uint8_t getWhite(const nrOfLights_t indexV) { return getLight<uint8_t>(indexV, layerP->lights.header.offsetWhite); }
  uint8_t getWhite(Coord3D pos) { return getWhite(XYZ(pos)); }

  CRGB getRGB1(const nrOfLights_t indexV) { return getLight<CRGB>(indexV, layerP->lights.header.offsetRGB1); }
  CRGB getRGB1(Coord3D pos) { return getRGB1(XYZ(pos)); }

  CRGB getRGB2(const nrOfLights_t indexV) { return getLight<CRGB>(indexV, layerP->lights.header.offsetRGB2); }
  CRGB getRGB2(Coord3D pos) { return getRGB2(XYZ(pos)); }

  CRGB getRGB3(const nrOfLights_t indexV) { return getLight<CRGB>(indexV, layerP->lights.header.offsetRGB3); }
  CRGB getRGB3(Coord3D pos) { return getRGB3(XYZ(pos)); }

  template <typename T>
  T getLight(const nrOfLights_t indexV, uint8_t offset) const;

  // to be called in loop, if more then one effect
  //  void setLightsToBlend(); //uses LEDs

  void fadeToBlackBy(const uint8_t fadeBy = 255);
  void fadeToBlackMin();

  void fill_solid(const CRGB& color);
  void fill_rainbow(const uint8_t initialhue, const uint8_t deltahue);

  void onLayoutPre();
  void onLayoutPost();

  // addLight is called by onLayout for each light in the layout
  void addLight(Coord3D position);

  // checks if a virtual light is mapped to a physical light (use with XY() or XYZ() to get the indexV)
  bool isMapped(int indexV) const { return indexV < mappingTableSize && (mappingTable[indexV].mapType == m_oneLight || mappingTable[indexV].mapType == m_moreLights); }

  void blur1d(fract8 blur_amount, uint16_t x = 0) {
    // todo: check updated in wled-MM
    const uint8_t keep = 255 - blur_amount;
    const uint8_t seep = blur_amount >> 1;
    CRGB carryover = CRGB::Black;
    for (uint16_t i = 0; i < size.y; ++i) {
      CRGB cur = getRGB(Coord3D(x, i));
      CRGB part = cur;
      part.nscale8(seep);
      cur.nscale8(keep);
      cur += carryover;
      if (i) addRGB(Coord3D(x, i - 1), part);
      setRGB(Coord3D(x, i), cur);
      carryover = part;
    }
  }

  void blur2d(fract8 blur_amount) {
    blurRows(size.x, size.y, blur_amount);
    blurColumns(size.x, size.y, blur_amount);
  }

  void blurRows(uint16_t width, uint16_t height, fract8 blur_amount) {
    /*    for (uint16_t row = 0; row < height; row++) {
            CRGB* rowbase = leds + (row * width);
            blur1d( rowbase, width, blur_amount);
        }
    */
    // blur rows same as columns, for irregular matrix
    uint8_t keep = 255 - blur_amount;
    uint8_t seep = blur_amount >> 1;
    for (uint16_t row = 0; row < height; row++) {
      CRGB carryover = CRGB::Black;
      for (uint16_t i = 0; i < width; i++) {
        CRGB cur = getRGB(Coord3D(i, row));
        CRGB part = cur;
        part.nscale8(seep);
        cur.nscale8(keep);
        cur += carryover;
        if (i) addRGB(Coord3D(i - 1, row), part);
        setRGB(Coord3D(i, row), cur);
        carryover = part;
      }
    }
  }

  // blurColumns: perform a blur1d on each column of a rectangular matrix
  void blurColumns(uint16_t width, uint16_t height, fract8 blur_amount) {
    // blur columns
    uint8_t keep = 255 - blur_amount;
    uint8_t seep = blur_amount >> 1;
    for (uint16_t col = 0; col < width; ++col) {
      CRGB carryover = CRGB::Black;
      for (uint16_t i = 0; i < height; ++i) {
        CRGB cur = getRGB(Coord3D(col, i));
        CRGB part = cur;
        part.nscale8(seep);
        cur.nscale8(keep);
        cur += carryover;
        if (i) addRGB(Coord3D(col, i - 1), part);
        setRGB(Coord3D(col, i), cur);
        carryover = part;
      }
    }
  }

  void drawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, CRGB color, bool soft = false, uint8_t depth = UINT8_MAX);

  void drawLine3D(const Coord3D& a, Coord3D b, CRGB color, bool soft = false, uint8_t depth = UINT8_MAX) { drawLine3D(a.x, a.y, a.z, b.x, b.y, b.z, color, soft, depth); }
  // to do: merge with drawLine to support 2D and 3D
  void drawLine3D(uint8_t x1, uint8_t y1, uint8_t z1, uint8_t x2, uint8_t y2, uint8_t z2, CRGB color, bool soft = false, uint8_t depth = UINT8_MAX);

  void drawCircle(int cx, int cy, uint8_t radius, CRGB col, bool soft);

  // shift is used by drawText indicating which letter it is drawing
  void drawCharacter(unsigned char chr, int x = 0, int y = 0, uint8_t font = 0, CRGB col = CRGB::Red, uint16_t shiftPixel = 0, uint16_t shiftChr = 0);

  void drawText(const char* text, int x = 0, int y = 0, uint8_t font = 0, CRGB col = CRGB::Red, uint16_t shiftPixel = 0);
};

#endif