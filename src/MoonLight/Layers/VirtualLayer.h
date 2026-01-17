/**
    @title     MoonLight
    @file      VirtualLayer.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
           https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

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
    struct {                 // condensed rgb
      uint16_t rgb : 14;     // 14 bits (554 RGB)
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
  size_t mappingTableSize = 0;
  std::vector<std::vector<nrOfLights_t>, VectorRAMAllocator<std::vector<nrOfLights_t> > > mappingTableIndexes;
  nrOfLights_t mappingTableIndexesSizeUsed = 0;

  PhysicalLayer* layerP;  // physical LEDs the virtual LEDs are mapped to
  std::vector<Node*, VectorRAMAllocator<Node*> > nodes;

  uint8_t fadeMin;

  uint8_t effectDimension = _3D;  // assuming 3D for the moment
  uint8_t layerDimension = UINT8_MAX;

  Coord3D prevSize;  // to calculate size change

  bool oneToOneMapping = false;

  VirtualLayer();

  ~VirtualLayer();

  void setup();
  void loop();
  void loop20ms();

  void addIndexP(PhysMap& physMap, nrOfLights_t indexP);

  // position is by reference as within XYZ, the position can be modified, for efficiency reasons, don't declare an extra variable for that in XYZ
  nrOfLights_t XYZ(Coord3D& position);

  nrOfLights_t XYZUnModified(const Coord3D& position) const { return position.x + position.y * size.x + position.z * size.x * size.y; }

  template <typename Callback>
  void forEachLightIndex(const nrOfLights_t indexV, Callback&& callback, bool onlyOne = false) {
    if (indexV < mappingTableSize) {
      switch (mappingTable[indexV].mapType) {
      case m_oneLight: {
        nrOfLights_t indexP = mappingTable[indexV].indexP;
        // if (layerP->lights.header.lightPreset == lightPreset_RGB2040) {
        //   indexP += (indexP / 20) * 20;
        // }
        callback(indexP);
        break;
      }
      case m_moreLights:
        if (mappingTable[indexV].indexesIndex < mappingTableIndexes.size()) {
          for (nrOfLights_t indexP : mappingTableIndexes[mappingTable[indexV].indexesIndex]) {
            // if (layerP->lights.header.lightPreset == lightPreset_RGB2040) {
            //   indexP += (indexP / 20) * 20;
            // }
            callback(indexP);
            if (onlyOne) return;
          }
        }
        break;
      }
    } else {                                                                                      // no mappnig
      if (indexV * (layerP->lights.header.channelsPerLight + 1) <= layerP->lights.maxChannels) {  // make sure the light is in the channels array
        callback(indexV);
      }
    }
  }

  // set the value for a channel in each corresponding physical light
  void setLight(const nrOfLights_t indexV, uint8_t offset, uint8_t value) {
    forEachLightIndex(indexV, [&](nrOfLights_t indexP) { layerP->lights.channelsE[indexP * layerP->lights.header.channelsPerLight + offset] = value; });
  }

  void setRGB(const nrOfLights_t indexV, CRGB color) {
    if (mappingTable[indexV].mapType == m_zeroLights) {
  #ifdef BOARD_HAS_PSRAM
      memcpy(&mappingTable[indexV].rgb, &color, 3);
  #else
      mappingTable[indexV].rgb = ((min(color[0] + 3, 255) >> 3) << 9) + ((min(color[1] + 3, 255) >> 3) << 4) + (min(color[2] + 7, 255) >> 4);
  #endif
    } else
      forEachLightIndex(indexV, [&](nrOfLights_t indexP) { memcpy(&layerP->lights.channelsE[indexP * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGB], color.raw, sizeof(color)); });
  }
  void setRGB(Coord3D pos, CRGB color) { setRGB(XYZ(pos), color); }

  void setBrightness(const nrOfLights_t indexV, uint8_t value) {
    if (layerP->lights.header.offsetBrightness != UINT8_MAX) {
      setLight(indexV, layerP->lights.header.offsetBrightness, (value * layerP->lights.header.brightness) / 255);
    }
  }
  void setBrightness(Coord3D pos, const uint8_t value) { setBrightness(XYZ(pos), value); }

  void setPan(const nrOfLights_t indexV, const uint8_t value) {
    if (layerP->lights.header.offsetPan != UINT8_MAX) setLight(indexV, layerP->lights.header.offsetPan, value);
  }
  void setPan(Coord3D pos, const uint8_t value) { setPan(XYZ(pos), value); }

  void setTilt(const nrOfLights_t indexV, const uint8_t value) {
    if (layerP->lights.header.offsetTilt != UINT8_MAX) setLight(indexV, layerP->lights.header.offsetTilt, value);
  }
  void setTilt(Coord3D pos, const uint8_t value) { setTilt(XYZ(pos), value); }

  void setZoom(const nrOfLights_t indexV, const uint8_t value) {
    if (layerP->lights.header.offsetZoom != UINT8_MAX) setLight(indexV, layerP->lights.header.offsetZoom, value);
  }
  void setZoom(Coord3D pos, const uint8_t value) { setZoom(XYZ(pos), value); }

  void setRotate(const nrOfLights_t indexV, const uint8_t value) {
    if (layerP->lights.header.offsetRotate != UINT8_MAX) setLight(indexV, layerP->lights.header.offsetRotate, value);
  }
  void setRotate(Coord3D pos, const uint8_t value) { setRotate(XYZ(pos), value); }

  void setGobo(const nrOfLights_t indexV, const uint8_t value) {
    if (layerP->lights.header.offsetGobo != UINT8_MAX) setLight(indexV, layerP->lights.header.offsetGobo, value);
  }
  void setGobo(Coord3D pos, const uint8_t value) { setGobo(XYZ(pos), value); }

  void setRGB1(const nrOfLights_t indexV, CRGB color) {
    if (layerP->lights.header.offsetRGB1 != UINT8_MAX) forEachLightIndex(indexV, [&](nrOfLights_t indexP) { memcpy(&layerP->lights.channelsE[indexP * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGB1], color.raw, sizeof(color)); });
  }
  void setRGB1(Coord3D pos, CRGB color) { setRGB1(XYZ(pos), color); }

  void setRGB2(const nrOfLights_t indexV, CRGB color) {
    if (layerP->lights.header.offsetRGB2 != UINT8_MAX) forEachLightIndex(indexV, [&](nrOfLights_t indexP) { memcpy(&layerP->lights.channelsE[indexP * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGB2], color.raw, sizeof(color)); });
  }
  void setRGB2(Coord3D pos, CRGB color) { setRGB2(XYZ(pos), color); }

  void setRGB3(const nrOfLights_t indexV, CRGB color) {
    if (layerP->lights.header.offsetRGB3 != UINT8_MAX) forEachLightIndex(indexV, [&](nrOfLights_t indexP) { memcpy(&layerP->lights.channelsE[indexP * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGB3], color.raw, sizeof(color)); });
  }
  void setRGB3(Coord3D pos, CRGB color) { setRGB3(XYZ(pos), color); }

  void setBrightness2(const nrOfLights_t indexV, uint8_t value) {
    value = (value * layerP->lights.header.brightness) / 255;
    if (layerP->lights.header.offsetBrightness2 != UINT8_MAX) setLight(indexV, layerP->lights.header.offsetBrightness2, value);
  }
  void setBrightness2(Coord3D pos, const uint8_t value) { setBrightness2(XYZ(pos), value); }

  // get the value for a channel in one! corresponding physical light (the others are the same)
  uint8_t getLight(const nrOfLights_t indexV, uint8_t offset) {
    uint8_t value = 0; // assume 0, not UINT8_MAX as that sets the channel to max
    forEachLightIndex(indexV, [&](nrOfLights_t indexP) { value = layerP->lights.channelsE[indexP * layerP->lights.header.channelsPerLight + offset]; }, true);
    return value;
  }

  CRGB getRGB(const nrOfLights_t indexV) {
    if (mappingTable[indexV].mapType == m_zeroLights) {
  #ifdef BOARD_HAS_PSRAM
      return *(CRGB*)&mappingTable[indexV].rgb;
  #else
      CRGB result;
      ((uint8_t*)&result)[0] = (mappingTable[indexV].rgb >> 9) << 3;           // R: bits [13:9]
      ((uint8_t*)&result)[1] = ((mappingTable[indexV].rgb >> 4) & 0x1F) << 3;  // G: bits [8:4]
      ((uint8_t*)&result)[2] = (mappingTable[indexV].rgb & 0x0F) << 4;         // B: bits [3:0]
      return result;
  #endif
    } else {
      CRGB color;
      forEachLightIndex(indexV, [&](nrOfLights_t indexP) { memcpy(&color, &layerP->lights.channelsE[indexP * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGB], sizeof(color)); }, true);
      return color;
    }
  }
  CRGB getRGB(Coord3D pos) { return getRGB(XYZ(pos)); }

  void addRGB(const Coord3D& position, const CRGB& color) { setRGB(position, getRGB(position) + color); }

  void blendColor(const nrOfLights_t indexV, const CRGB& color, uint8_t blendAmount) { setRGB(indexV, blend(color, getRGB(indexV), blendAmount)); }
  void blendColor(Coord3D position, const CRGB& color, const uint8_t blendAmount) { blendColor(XYZ(position), color, blendAmount); }

  CRGB getRGB1(const nrOfLights_t indexV) {
    if (layerP->lights.header.offsetRGB1 == UINT8_MAX) return CRGB::Black;
    CRGB color = CRGB::Black;
    forEachLightIndex(indexV, [&](nrOfLights_t indexP) { memcpy(&color, &layerP->lights.channelsE[indexP * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGB1], sizeof(color)); }, true);
    return color;
  }
  CRGB getRGB1(Coord3D pos) { return getRGB1(XYZ(pos)); }

  CRGB getRGB2(const nrOfLights_t indexV) {
    if (layerP->lights.header.offsetRGB2 == UINT8_MAX) return CRGB::Black;
    CRGB color = CRGB::Black;
    forEachLightIndex(indexV, [&](nrOfLights_t indexP) { memcpy(&color, &layerP->lights.channelsE[indexP * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGB2], sizeof(color)); }, true);
    return color;
  }
  CRGB getRGB2(Coord3D pos) { return getRGB2(XYZ(pos)); }

  CRGB getRGB3(const nrOfLights_t indexV) {
    if (layerP->lights.header.offsetRGB3 == UINT8_MAX) return CRGB::Black;
    CRGB color = CRGB::Black;
    forEachLightIndex(indexV, [&](nrOfLights_t indexP) { memcpy(&color, &layerP->lights.channelsE[indexP * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGB3], sizeof(color)); }, true);
    return color;
  }
  CRGB getRGB3(Coord3D pos) { return getRGB3(XYZ(pos)); }

  // to be called in loop, if more then one effect
  //  void setLightsToBlend(); //uses LEDs

  void fadeToBlackBy(const uint8_t fadeBy = 255);
  void fadeToBlackMin();

  void fill_solid(const CRGB& color);
  void fill_rainbow(const uint8_t initialhue, const uint8_t deltahue);

  void onLayoutPre();
  void createMappingTableAndAddOneToOne();
  void onLayoutPost();

  // addLight is called by onLayout for each light in the layout
  void addLight(Coord3D position);

  // checks if a virtual light is mapped to a physical light (use with XY() or XYZ() to get the indexV)
  bool isMapped(nrOfLights_t indexV) const { return oneToOneMapping || indexV < mappingTableSize && (mappingTable[indexV].mapType == m_oneLight || mappingTable[indexV].mapType == m_moreLights); }

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