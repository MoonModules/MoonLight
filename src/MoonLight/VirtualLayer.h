/**
    @title     MoonLight
    @file      VirtualLayer.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/general/utilities/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact moonmodules@icloud.com
**/

#pragma once

#if FT_MOONLIGHT

#include <Arduino.h>
#include <vector>
#include <FastLED.h>

#include "PhysicalLayer.h"

enum mapType {
    m_color,
    m_oneLight,
    m_moreLights,
    m_count //keep as last entry
  };
  
struct PhysMap {
    union {
      struct {                 //condensed rgb
        uint16_t rgb14: 14;    //14 bits (554 RGB)
        uint8_t mapType:2;        //2 bits (4)
      }; //16 bits
      uint16_t indexP: 14;   //16384 one physical light (type==1) index to ledsP array
      uint16_t indexes:14;  //16384 multiple physical lights (type==2) index in std::vector<std::vector<uint16_t>> mappingTableIndexes;
    }; // 2 bytes  
    
    PhysMap() {
      // ESP_LOGD(TAG, "Constructor");
      mapType = m_color; // the default until indexP is added
      rgb14 = 0;
    }
  };

class VirtualLayer {

  public:

  uint16_t nrOfLights = 256;
  Coord3D size = {16,16,1}; //not 0,0,0 to prevent div0 eg in Octopus2D
  Coord3D middle = {8,8,1}; //not 0,0,0 to prevent div0 eg in Octopus2D

  //they will be reused to avoid fragmentation
  std::vector<PhysMap> mappingTable;
  std::vector<std::vector<uint16_t>> mappingTableIndexes;
  uint16_t mappingTableSizeUsed = 0; 
  uint16_t mappingTableIndexesSizeUsed = 0; 

  PhysicalLayer *layerP; //physical leds the virtual leds are mapped to
  std::vector<Node *> nodes;

  uint8_t fadeMin;

  uint8_t requestMapPhysical = false; //collect requests to map as it is requested by setup and updateControl and only need to be done once
  uint8_t requestMapVirtual = false; //collect requests to map as it is requested by setup and updateControl and only need to be done once

  VirtualLayer() {
    ESP_LOGD(TAG, "constructor");
  }
  
  ~VirtualLayer();

  void setup();
  void loop();

  void resetMapping();
  void addIndexP(PhysMap &physMap, uint16_t indexP);

  uint16_t XYZ(Coord3D &position);
  
  uint16_t XYZUnprojected(const Coord3D &position) const {
    return position.x + position.y*size.x + position.z * size.x * size.y;
  }

  void fadeToBlackBy(const uint8_t fadeBy);
  void fadeToBlackMin();

  template <typename T>
  void setLight(const uint16_t indexV, const T& value) {  // Serial.printf(" %d: %d,%d,%d", indexV, color.r, color.g, color.b);
    if (indexV < mappingTableSizeUsed) {
      // ESP_LOGD(TAG, "setLightColor %d %d %d %d", indexV, color.r, color.g, color.b, mappingTableSizeUsed);
      switch (mappingTable[indexV].mapType) {
        case m_color:{
          //only room for storing colors
          if (std::is_same<T, CRGB>::value) {
            const byte* valueAsBytes = reinterpret_cast<const byte*>(&value);
            mappingTable[indexV].rgb14 = ((min(valueAsBytes[0] + 3, 255) >> 3) << 9) + 
                                        ((min(valueAsBytes[1] + 3, 255) >> 3) << 4) + 
                                        (min(valueAsBytes[2] + 7, 255) >> 4);
          }
          break;
        }
        case m_oneLight: {
          uint16_t indexP = mappingTable[indexV].indexP;
          //temp fix for cube202020 (some curtains are bgr)
          // if (indexP > 2800) {
          //   fix->ledsP[indexP].r = color.b;
          //   fix->ledsP[indexP].g = color.g;
          //   fix->ledsP[indexP].b = color.r;
          // }
          // else
          memcpy(&layerP->lights.channels[indexP*sizeof(T)], &value, sizeof(T));
    
          // &layerP->lights.channels[indexP*sizeof(T)] = valueAsBytes;
          break; }
        case m_moreLights:
          if (mappingTable[indexV].indexes < mappingTableIndexes.size())
            for (uint16_t indexP: mappingTableIndexes[mappingTable[indexV].indexes]) {
              // if (indexP > 2800) {
              //   fix->ledsP[indexP].r = color.b;
              //   fix->ledsP[indexP].g = color.g;
              //   fix->ledsP[indexP].b = color.r;
              // } else
              memcpy(&layerP->lights.channels[indexP*sizeof(T)], &value, sizeof(T));
            }
          else
            ESP_LOGW(TAG, "dev setLightColor i:%d m:%d s:%d", indexV, mappingTable[indexV].indexes, mappingTableIndexes.size());
          break;
        default: ;
      }
    }
    else if (indexV * sizeof(T) < MAX_CHANNELS) {//no mapping
      memcpy(&layerP->lights.channels[indexV*sizeof(T)], &value, sizeof(T));
    }
      // layerP->lights.dmxChannels[indexV] = (byte*)&color;
    // some operations will go out of bounds e.g. VUMeter, uncomment below lines if you wanna test on a specific effect
    // else //if (indexV != UINT16_MAX) //assuming UINT16_MAX is set explicitly (e.g. in XYZ)
    //   ESP_LOGW(TAG, " dev setLight %d >= %d", indexV, MAX_LEDS);
  }
  template <typename T>
  void setLight(const Coord3D &position, const T& value) {setLight(XYZUnprojected(position), value);}

  template <typename T>
  T getLight(const uint16_t indexV) const {
    if (indexV < mappingTableSizeUsed) {
      switch (mappingTable[indexV].mapType) {
        case m_oneLight: {
          T* b = (T*)&layerP->lights.channels[mappingTable[indexV].indexP * sizeof(T)];
          return *b; 
          break; }
        case m_moreLights: {
          T* b = (T*)&layerP->lights.channels[mappingTableIndexes[mappingTable[indexV].indexes][0] * sizeof(T)]; //any will do as they are all the same
          return *b;
          break; }
        default: // m_color:
          return T();
          // if (std::is_same<T, CRGB>::value) {
          //   const byte* valueAsBytes = reinterpret_cast<const byte*>(&value);
          //   mappingTable[indexV].rgb14 = ((min(valueAsBytes[0] + 3, 255) >> 3) << 9) + 
          //                             ((min(valueAsBytes[1] + 3, 255) >> 3) << 4) + 
          //                               (min(valueAsBytes[2] + 7, 255) >> 4);
          // }
          // return CRGB((mappingTable[indexV].rgb14 >> 9) << 3, 
          //             (mappingTable[indexV].rgb14 >> 4) << 3, 
          //              mappingTable[indexV].rgb14       << 4);
          break;
      }
    }
    else if (indexV * sizeof(T) < MAX_CHANNELS) { //no mapping
      T* b = (T*)&layerP->lights.channels[indexV * sizeof(T)];
      return *b;
    } else {
      // some operations will go out of bounds e.g. VUMeter, uncomment below lines if you wanna test on a specific effect
      // ESP_LOGD(TAG, " dev gPC %d >= %d", indexV, MAX_LEDS);
      return T();
    }
  }
  template <typename T>
  T getLight(const Coord3D &position) const {return getLight<T>(XYZUnprojected(position));}

  //to be called in loop, if more then one effect
  // void setLightsToBlend(); //uses leds

  void fill_solid(const CRGB& color);
  void fill_rainbow(const uint8_t initialhue, const uint8_t deltahue);

  void mapLayout();
  void addLayoutPre();
  void addLight(Coord3D position);
  void addLayoutPost();

  void drawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, CRGB color, bool soft = false, uint8_t depth = UINT8_MAX) {

    // WLEDMM shorten line according to depth
    if (depth < UINT8_MAX) {
      if (depth == 0) return;         // nothing to paint
      if (depth<2) {x1 = x0; y1=y0; } // single pixel
      else {                          // shorten line
        x0 *=2; y0 *=2; // we do everything "*2" for better rounding
        int dx1 = ((int(2*x1) - int(x0)) * int(depth)) / 255;  // X distance, scaled down by depth 
        int dy1 = ((int(2*y1) - int(y0)) * int(depth)) / 255;  // Y distance, scaled down by depth
        x1 = (x0 + dx1 +1) / 2;
        y1 = (y0 + dy1 +1) / 2;
        x0 /=2; y0 /=2;
      }
    }

    const int16_t dx = abs(x1-x0), sx = x0<x1 ? 1 : -1;
    const int16_t dy = abs(y1-y0), sy = y0<y1 ? 1 : -1;

    // single pixel (line length == 0)
    if (dx+dy == 0) {
      setLight({x0, y0, 0}, color);
      return;
    }

    if (soft) {
      // Xiaolin Wu’s algorithm
      const bool steep = dy > dx;
      if (steep) {
        // we need to go along longest dimension
        std::swap(x0,y0);
        std::swap(x1,y1);
      }
      if (x0 > x1) {
        // we need to go in increasing fashion
        std::swap(x0,x1);
        std::swap(y0,y1);
      }
      float gradient = x1-x0 == 0 ? 1.0f : float(y1-y0) / float(x1-x0);
      float intersectY = y0;
      for (uint8_t x = x0; x <= x1; x++) {
        unsigned keep = float(0xFFFF) * (intersectY-int(intersectY)); // how much color to keep
        unsigned seep = 0xFFFF - keep; // how much background to keep
        uint8_t y = uint8_t(intersectY);
        if (steep) std::swap(x,y);  // temporarily swap if steep
        // pixel coverage is determined by fractional part of y co-ordinate
        // WLEDMM added out-of-bounds check: "unsigned(x) < cols" catches negative numbers _and_ too large values
        setLight({x, y, 0}, blend(color, getLight<CRGB>({x, y, 0}), keep));
        uint8_t xx = x+uint8_t(steep);
        uint8_t yy = y+uint8_t(!steep);
        setLight({xx, yy, 0}, blend(color, getLight<CRGB>({xx, yy, 0}), seep));
      
        intersectY += gradient;
        if (steep) std::swap(x,y);  // restore if steep
      }
    } else {
      // Bresenham's algorithm
      int err = (dx>dy ? dx : -dy)/2;   // error direction
      for (;;) {
        // if (x0 >= cols || y0 >= rows) break; // WLEDMM we hit the edge - should never happen
        setLight({x0, y0, 0}, color);
        if (x0==x1 && y0==y1) break;
        int e2 = err;
        if (e2 >-dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
      }
    }
  }

  void drawLine3D(Coord3D a, Coord3D b, CRGB color, bool soft = false, uint8_t depth = UINT8_MAX) {
    drawLine3D(a.x, a.y, a.z, b.x, b.y, b.z, color, soft, depth);
  }
  //to do: merge with drawLine to support 2D and 3D
  void drawLine3D(uint8_t x1, uint8_t y1, uint8_t z1, uint8_t x2, uint8_t y2, uint8_t z2, CRGB color, bool soft = false, uint8_t depth = UINT8_MAX)
  {
        // WLEDMM shorten line according to depth
    if (depth < UINT8_MAX) {
      if (depth == 0) return;         // nothing to paint
      if (depth<2) {x2 = x1; y2=y1; z2=z1;} // single pixel
      else {                          // shorten line
        x1 *=2; y1 *=2; z1 *=2; // we do everything "*2" for better rounding
        int dx1 = ((int(2*x2) - int(x1)) * int(depth)) / 255;  // X distance, scaled down by depth 
        int dy1 = ((int(2*y2) - int(y1)) * int(depth)) / 255;  // Y distance, scaled down by depth
        int dz1 = ((int(2*z2) - int(z1)) * int(depth)) / 255;  // Y distance, scaled down by depth
        x1 = (x1 + dx1 +1) / 2;
        y1 = (y1 + dy1 +1) / 2;
        z1 = (z1 + dz1 +1) / 2;
        x1 /=2; y1 /=2; z1 /=2;
      }
    }

    //to do implement soft

    //Bresenham
    setLight({x1, y1, z1}, color);
    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int dz = abs(z2 - z1);
    int xs;
    int ys;
    int zs;
    if (x2 > x1)
      xs = 1;
    else
      xs = -1;
    if (y2 > y1)
      ys = 1;
    else
      ys = -1;
    if (z2 > z1)
      zs = 1;
    else
      zs = -1;
  
    // Driving axis is X-axis"
    if (dx >= dy && dx >= dz) {
      int p1 = 2 * dy - dx;
      int p2 = 2 * dz - dx;
      while (x1 != x2) {
        x1 += xs;
        if (p1 >= 0) {
          y1 += ys;
          p1 -= 2 * dx;
        }
        if (p2 >= 0) {
          z1 += zs;
          p2 -= 2 * dx;
        }
        p1 += 2 * dy;
        p2 += 2 * dz;
        setLight({x1, y1, z1}, color);
      }
  
      // Driving axis is Y-axis"
    }
    else if (dy >= dx && dy >= dz) {
      int p1 = 2 * dx - dy;
      int p2 = 2 * dz - dy;
      while (y1 != y2) {
        y1 += ys;
        if (p1 >= 0) {
          x1 += xs;
          p1 -= 2 * dy;
        }
        if (p2 >= 0) {
          z1 += zs;
          p2 -= 2 * dy;
        }
        p1 += 2 * dx;
        p2 += 2 * dz;
        setLight({x1, y1, z1}, color);
      }
  
      // Driving axis is Z-axis"
    }
    else {
      int p1 = 2 * dy - dz;
      int p2 = 2 * dx - dz;
      while (z1 != z2) {
        z1 += zs;
        if (p1 >= 0) {
          y1 += ys;
          p1 -= 2 * dz;
        }
        if (p2 >= 0) {
          x1 += xs;
          p2 -= 2 * dz;
        }
        p1 += 2 * dy;
        p2 += 2 * dx;
        setLight({x1, y1, z1}, color);
      }
    }
  }

  Node *findLiveScriptNode(const char *animation);

};

#endif