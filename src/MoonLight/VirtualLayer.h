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
        byte mapType:2;        //2 bits (4)
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

    void setLightColor(const Coord3D &position, const CRGB& color) {setLightColor(XYZUnprojected(position), color);}
    void setLightColor(const uint16_t indexV, const CRGB& color); //uses leds
    CRGB getLightColor(const uint16_t indexV) const;
    void fadeToBlackBy(const uint8_t fadeBy);
    void fadeToBlackMin();

    template <typename T>
    void setLight(const Coord3D &position, const T& value) {setLight(XYZUnprojected(position), value);}
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
    T getLight(const uint16_t indexV) const {
      if (indexV < mappingTableSizeUsed) {
        switch (mappingTable[indexV].mapType) {
          case m_oneLight:
            return layerP->lights.channels[mappingTable[indexV].indexP * sizeof(T)]; 
            break;
          case m_moreLights:
            return layerP->lights.channels[mappingTableIndexes[mappingTable[indexV].indexes][0] * sizeof(T)]; //any will do as they are all the same
            break;
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
      else if (indexV * sizeof(T) < MAX_CHANNELS) //no mapping
        return layerP->lights.channels[indexV * sizeof(T)];
      else {
        // some operations will go out of bounds e.g. VUMeter, uncomment below lines if you wanna test on a specific effect
        // ESP_LOGD(TAG, " dev gPC %d >= %d", indexV, MAX_LEDS);
        return T();
      }
    }

    //to be called in loop, if more then one effect
    // void setLightsToBlend(); //uses leds

    void fill_solid(const CRGB& color);
    void fill_rainbow(const uint8_t initialhue, const uint8_t deltahue);

    void addLayoutPre();

    void addLight(Coord3D position);

    void addLayoutPost();

};

#endif