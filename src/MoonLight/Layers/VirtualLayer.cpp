/**
    @title     MoonLight
    @file      VirtualLayer.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

  #include "VirtualLayer.h"

  #include "MoonBase/Nodes.h"
  #include "PhysicalLayer.h"

// convenience functions to call fastled functions out of the Leds namespace (there naming conflict)
void fastled_fadeToBlackBy(CRGB* leds, uint16_t num_leds, uint8_t fadeBy) { fadeToBlackBy(leds, num_leds, fadeBy); }
void fastled_fill_solid(struct CRGB* targetArray, int numToFill, const CRGB& color) { fill_solid(targetArray, numToFill, color); }
void fastled_fill_rainbow(struct CRGB* targetArray, int numToFill, uint8_t initialhue, uint8_t deltahue) { fill_rainbow(targetArray, numToFill, initialhue, deltahue); }

VirtualLayer::~VirtualLayer() {
  EXT_LOGV(ML_TAG, "destructor");
  fadeToBlackBy(255);  // clear the LEDs

  for (Node* node : nodes) {
    // node->destructor();
    delete node;
  }
  nodes.clear();

  // clear array of array of indexes
  for (std::vector<uint16_t>& mappingTableIndex : mappingTableIndexes) {
    mappingTableIndex.clear();
  }
  mappingTableIndexes.clear();
}

void VirtualLayer::setup() {
  mappingTable = (PhysMap*)(layerP->lights.mappings);
  // no node setup here as done in addNode !
}

void VirtualLayer::loop() {
  fadeToBlackMin();

  // set brightness default to global brightness
  if (layerP->lights.header.offsetBrightness != UINT8_MAX) {
    for (int i = 0; i < nrOfLights; i++) {
      setBrightness(i, 255);   // will be corrected with globalbrighness
      setBrightness2(i, 255);  // will be corrected with globalbrighness
    }
  }

  if (prevSize != size) EXT_LOGD(ML_TAG, "onSizeChanged V %d,%d,%d -> %d,%d,%d", prevSize.x, prevSize.y, prevSize.z, size.x, size.y, size.z);
  for (Node* node : nodes) {
    if (prevSize != size) node->onSizeChanged(prevSize);
    if (node->on) node->loop();
  }
  prevSize = size;
};

void VirtualLayer::loop20ms() {
  for (Node* node : nodes) {
    if (node->on) node->loop20ms();
  }
}

void VirtualLayer::addIndexP(PhysMap& physMap, uint16_t indexP) {
  // EXT_LOGV(ML_TAG, "i:%d t:%d s:%d i:%d", indexP, physMap.mapType, mappingTableIndexes.size(), physMap.indexes);
  switch (physMap.mapType) {
  case m_zeroLights:  // zero -> one
    // case m_rgbColor:
    physMap.indexP = indexP;
    physMap.mapType = m_oneLight;
    break;
  case m_oneLight: {  // one -> more
    uint16_t oldIndexP = physMap.indexP;
    // change to m_moreLights and add the old indexP and new indexP to the multiple indexP array
    mappingTableIndexesSizeUsed++;  // add a new slot in the mappingTableIndexes
    if (mappingTableIndexes.size() < mappingTableIndexesSizeUsed)
      mappingTableIndexes.push_back({oldIndexP, indexP});
    else
      mappingTableIndexes[mappingTableIndexesSizeUsed - 1] = {oldIndexP, indexP};

    physMap.indexes = mappingTableIndexesSizeUsed - 1;  // array position
    physMap.mapType = m_moreLights;
    break;
  }
  case m_moreLights:  // more -> more
    // mappingTableIndexes.reserve(physMap.indexes+1);
    mappingTableIndexes[physMap.indexes].push_back(indexP);
    // EXT_LOGV(ML_TAG, " more %d", mappingTableIndexes.size());
    break;
  }
  // EXT_LOGV(ML_TAG, "\n");
}
uint16_t VirtualLayer::XYZ(Coord3D& position) {
  // XYZ modifiers (this is not slowing things down as you might have expected ...)
  for (Node* node : nodes) {      // e.g. random or scrolling or rotate modifier
    if (node->on)                 //  && node->hasModifier()
      node->modifyXYZ(position);  // modifies the position
  }

  return XYZUnModified(position);
}

// void VirtualLayer::setLightsToBlend() {
//   for (const std::vector<uint16_t>& mappingTableIndex: mappingTableIndexes) {
//       for (const uint16_t indexP: mappingTableIndex)
//       layerP->lightsToBlend[indexP] = true;
//     }
//     for (const PhysMap &physMap: mappingTable) {
//       if (physMap.mapType == m_oneLight)
//       layerP->lightsToBlend[physMap.indexP] = true;
//     }
// }

void VirtualLayer::setLight(const uint16_t indexV, const uint8_t* channels, uint8_t offset, uint8_t length) {
  if (indexV < nrOfLights) {
    // EXT_LOGV(ML_TAG, "setLightColor %d %d %d %d", indexV, color.r, color.g, color.b, nrOfLights);
    switch (mappingTable[indexV].mapType) {
    case m_zeroLights: {
      // only room for storing colors
      if (length <= 4) {  // also for RGBW, but store only RGB ... 🚧
        mappingTable[indexV].rgb14 = ((min(channels[0] + 3, 255) >> 3) << 9) + ((min(channels[1] + 3, 255) >> 3) << 4) + (min(channels[2] + 7, 255) >> 4);
      }
      break;
    }
    case m_oneLight: {
      uint16_t indexP = mappingTable[indexV].indexP;
      memcpy(&layerP->lights.channels[indexP * layerP->lights.header.channelsPerLight + offset], channels, length);
      break;
    }
    case m_moreLights:
      if (mappingTable[indexV].indexes < mappingTableIndexes.size())
        for (uint16_t indexP : mappingTableIndexes[mappingTable[indexV].indexes]) {
          memcpy(&layerP->lights.channels[indexP * layerP->lights.header.channelsPerLight + offset], channels, length);
        }
      else
        EXT_LOGW(ML_TAG, "dev setLightColor i:%d m:%d s:%d", indexV, mappingTable[indexV].indexes, mappingTableIndexes.size());
      break;
    default:;
    }
  } else if (indexV * layerP->lights.header.channelsPerLight + offset + length < layerP->lights.maxChannels) {  // no mapping
    memcpy(&layerP->lights.channels[indexV * layerP->lights.header.channelsPerLight + offset], channels, length);
  }
}

template <typename T>
T VirtualLayer::getLight(const uint16_t indexV, uint8_t offset) const {
  if (indexV < nrOfLights) {
    switch (mappingTable[indexV].mapType) {
    case m_oneLight: {
      T* result = (T*)&layerP->lights.channels[mappingTable[indexV].indexP * layerP->lights.header.channelsPerLight + offset];
      return *result;  // return the color as CRGB
      break;
    }
    case m_moreLights: {
      T* result = (T*)&layerP->lights.channels[mappingTableIndexes[mappingTable[indexV].indexes][0] * layerP->lights.header.channelsPerLight + offset];  // any will do as they are all the same
      return *result;                                                                                                                                    // return the color as CRGB
      break;
    }
    default:                                              // m_zeroLights:
      if (layerP->lights.header.channelsPerLight <= 4) {  // also for RGBW but retrieve only RGB ... 🚧
        T result;
        ((uint8_t*)&result)[0] = (mappingTable[indexV].rgb14 >> 9) << 3;
        ((uint8_t*)&result)[1] = (mappingTable[indexV].rgb14 >> 4) << 3;
        ((uint8_t*)&result)[2] = (mappingTable[indexV].rgb14) << 4;
        return result;
      } else
        return T();  // not implemented yet
      break;
    }
  } else if (indexV * layerP->lights.header.channelsPerLight + offset + 3 < layerP->lights.maxChannels) {  // no mapping
    T* result = (T*)&layerP->lights.channels[indexV * layerP->lights.header.channelsPerLight + offset];
    return *result;  // return the color as CRGB
  } else {
    // some operations will go out of bounds e.g. VUMeter, uncomment below lines if you wanna test on a specific effect
    // EXT_LOGV(ML_TAG, " dev gPC %d >= %d", indexV, MAX_CHANNELS);
    return T();
  }
}

// fadeToBlackBy will only do primary RGB colors
void VirtualLayer::fadeToBlackBy(const uint8_t fadeBy) { fadeMin = fadeMin ? MIN(fadeMin, fadeBy) : fadeBy; }

void VirtualLayer::fadeToBlackMin() {
  if (fadeMin > 0) {  // at least one fadeBy

    uint8_t fadeBy = fadeMin;  // + fadeMax / div;
    // if (effectDimension < layerDimension) { //only process the effect lights (so modifiers can do things with the other dimension)
    //   for (int y=0; y < ((effectDimension == _1D)?1:size.y); y++) { //1D effects only on y=0, 2D effects loop over y
    //     for (int x=0; x<size.x; x++) {
    //       CRGB color = getLight({x,y,0});
    //       color.nscale8(255-fadeBy);
    //       setRGB({x,y,0}, color);
    //     }
    //   }
    // } else
    if (layerP->lights.header.channelsPerLight == 3 && layerP->layers.size() == 1) {  // CRGB lights
      fastled_fadeToBlackBy((CRGB*)layerP->lights.channels, layerP->lights.header.nrOfLights, fadeBy);
    } else {  // multichannel lights
      for (uint16_t index = 0; index < nrOfLights; index++) {
        CRGB color = getRGB(index);  // direct access to the channels
        color.nscale8(255 - fadeBy);
        setRGB(index, color);
        if (layerP->lights.header.offsetWhite != UINT8_MAX) {
          uint8_t white = getWhite(index);  // direct access to the channels
          white = (white * fadeBy) >> 8;
          setWhite(index, white);
        }
        if (layerP->lights.header.offsetRGB1 != UINT8_MAX) {
          CRGB color = getRGB1(index);  // direct access to the channels
          color.nscale8(255 - fadeBy);
          setRGB1(index, color);
        }
        if (layerP->lights.header.offsetRGB2 != UINT8_MAX) {
          CRGB color = getRGB2(index);  // direct access to the channels
          color.nscale8(255 - fadeBy);
          setRGB2(index, color);
        }
        if (layerP->lights.header.offsetRGB3 != UINT8_MAX) {
          CRGB color = getRGB3(index);  // direct access to the channels
          color.nscale8(255 - fadeBy);
          setRGB3(index, color);
        }
      }
    }
    // reset fade
    fadeMin = 0;  // no fade
  }
}

void VirtualLayer::fill_solid(const CRGB& color) {
  // if (effectDimension < layerDimension) { //only process the effect lights (so modifiers can do things with the other dimension)
  //   for (int y=0; y < ((effectDimension == _1D)?1:size.y); y++) { //1D effects only on y=0, 2D effects loop over y
  //     for (int x=0; x<size.x; x++) {
  //       setRGB({x,y,0}, color);
  //     }
  //   }
  // } else
  if (layerP->lights.header.channelsPerLight == 3 && layerP->layers.size() == 1) {  // faster, else manual
    fastled_fill_solid((CRGB*)layerP->lights.channels, layerP->lights.header.nrOfLights, color);
  } else {
    for (uint16_t index = 0; index < nrOfLights; index++) setRGB(index, color);
  }
}

void VirtualLayer::fill_rainbow(const uint8_t initialhue, const uint8_t deltahue) {
  // if (effectDimension < layerDimension) { //only process the effect lights (so modifiers can do things with the other dimension)
  //   CHSV hsv;
  //   hsv.hue = initialhue;
  //   hsv.val = 255;
  //   hsv.sat = 240;
  //   for (int y=0; y < ((effectDimension == _1D)?1:size.y); y++) { //1D effects only on y=0, 2D effects loop over y
  //     for (int x=0; x<size.x; x++) {
  //       setRGB({x,y,0}, hsv);
  //       hsv.hue += deltahue;
  //     }
  //   }
  // } else
  if (layerP->lights.header.channelsPerLight == 3 && layerP->layers.size() == 1) {  // faster, else manual
    fastled_fill_rainbow((CRGB*)layerP->lights.channels, layerP->lights.header.nrOfLights, initialhue, deltahue);
  } else {
    CHSV hsv;
    hsv.hue = initialhue;
    hsv.val = 255;
    hsv.sat = 240;

    for (uint16_t index = 0; index < nrOfLights; index++) {
      setRGB(index, hsv);
      hsv.hue += deltahue;
    }
  }
}

void VirtualLayer::onLayoutPre() {
  nrOfLights = 0;
  size = layerP->lights.header.size;  // start with the physical size
  start = {0, 0, 0};
  end = size - 1;
  middle = size / 2;

  // modifiers
  for (Node* node : nodes) {
    if (node->on)  //  && node->hasModifier()
      node->modifySize();
  }

  // 0 to 3D depending on start and end (e.g. to display ScrollingText on one side of a cube)
  layerDimension = 0;
  if (size.x > 1) layerDimension++;
  if (size.y > 1) layerDimension++;
  if (size.z > 1) layerDimension++;

  // resetMapping

  for (std::vector<uint16_t>& mappingTableIndex : mappingTableIndexes) {
    mappingTableIndex.clear();
  }
  mappingTableIndexesSizeUsed = 0;  // do not clear mappingTableIndexes, reuse it

  memset(mappingTable, 0, layerP->lights.maxMappings);  // reset mappingTable
}

void VirtualLayer::addLight(Coord3D position) {
  // modifiers
  for (Node* node : nodes) {
    if (node->on)  //  && node->hasModifier()
      node->modifyPosition(position);
  }

  if (position.x != UINT16_MAX) {  // can be set to UINT16_MAX by modifier todo: check multiple modifiers
    uint16_t indexV = XYZUnModified(position);
    if (mappingTable && indexV < layerP->lights.maxMappings / sizeof(PhysMap)) {
      nrOfLights = indexV + 1;
      addIndexP(mappingTable[indexV], layerP->indexP);
    }
  } else {
    // set unmapped lights to 0, e.g. needed by checkerboard modifier
    memset(&layerP->lights.channels[layerP->indexP * layerP->lights.header.channelsPerLight], 0, layerP->lights.header.channelsPerLight);
  }
}

void VirtualLayer::onLayoutPost() {
  // prepare logging:
  uint16_t nrOfOneLight = 0;
  uint16_t nrOfMoreLights = 0;
  uint16_t nrOfZeroLights = 0;
  for (size_t i = 0; i < nrOfLights; i++) {
    PhysMap& map = mappingTable[i];
    switch (map.mapType) {
    case m_zeroLights:
      nrOfZeroLights++;
      break;
    case m_oneLight:
      // EXT_LOGV(ML_TAG,"%d mapping =1: #ledsP : %d", i, map.indexP);
      nrOfOneLight++;
      break;
    case m_moreLights:
      // Char<32> str;
      for (uint16_t indexP : mappingTableIndexes[map.indexes]) {
        // str += indexP;
        nrOfMoreLights++;
      }
      // EXT_LOGV(ML_TAG, "%d mapping >1: #ledsP : %s", i, str.c_str());
      break;
    }
    // else
    //   EXT_LOGV(ML_TAG, "%d no mapping\n", x);
  }

  EXT_LOGD(MB_TAG, "V:%d x %d x %d = v:%d = 1:0:%d + 1:1:%d + mti:%d (1:m:%d)", size.x, size.y, size.z, nrOfLights, nrOfZeroLights, nrOfOneLight, mappingTableIndexesSizeUsed, nrOfMoreLights);
}

void VirtualLayer::drawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, CRGB color, bool soft, uint8_t depth) {
  // WLEDMM shorten line according to depth
  if (depth < UINT8_MAX) {
    if (depth == 0) return;  // nothing to paint
    if (depth < 2) {
      x1 = x0;
      y1 = y0;
    }  // single pixel
    else {  // shorten line
      x0 *= 2;
      y0 *= 2;                                                 // we do everything "*2" for better rounding
      int dx1 = ((int(2 * x1) - int(x0)) * int(depth)) / 255;  // X distance, scaled down by depth
      int dy1 = ((int(2 * y1) - int(y0)) * int(depth)) / 255;  // Y distance, scaled down by depth
      x1 = (x0 + dx1 + 1) / 2;
      y1 = (y0 + dy1 + 1) / 2;
      x0 /= 2;
      y0 /= 2;
    }
  }

  const int16_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  const int16_t dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1;

  // single pixel (line length == 0)
  if (dx + dy == 0) {
    setRGB({x0, y0, 0}, color);
    return;
  }

  if (soft) {
    // Xiaolin Wu’s algorithm
    const bool steep = dy > dx;
    if (steep) {
      // we need to go along longest dimension
      std::swap(x0, y0);
      std::swap(x1, y1);
    }
    if (x0 > x1) {
      // we need to go in increasing fashion
      std::swap(x0, x1);
      std::swap(y0, y1);
    }
    float gradient = x1 - x0 == 0 ? 1.0f : float(y1 - y0) / float(x1 - x0);
    float intersectY = y0;
    for (uint8_t x = x0; x <= x1; x++) {
      unsigned keep = float(0xFFFF) * (intersectY - int(intersectY));  // how much color to keep
      unsigned seep = 0xFFFF - keep;                                   // how much background to keep
      uint8_t y = uint8_t(intersectY);
      if (steep) std::swap(x, y);  // temporarily swap if steep
      // pixel coverage is determined by fractional part of y co-ordinate
      // WLEDMM added out-of-bounds check: "unsigned(x) < cols" catches negative numbers _and_ too large values
      setRGB({x, y, 0}, blend(color, getRGB({x, y, 0}), keep));
      uint8_t xx = x + uint8_t(steep);
      uint8_t yy = y + uint8_t(!steep);
      setRGB({xx, yy, 0}, blend(color, getRGB({xx, yy, 0}), seep));

      intersectY += gradient;
      if (steep) std::swap(x, y);  // restore if steep
    }
  } else {
    // Bresenham's algorithm
    int err = (dx > dy ? dx : -dy) / 2;  // error direction
    for (;;) {
      // if (x0 >= cols || y0 >= rows) break; // WLEDMM we hit the edge - should never happen
      setRGB({x0, y0, 0}, color);
      if (x0 == x1 && y0 == y1) break;
      int e2 = err;
      if (e2 > -dx) {
        err -= dy;
        x0 += sx;
      }
      if (e2 < dy) {
        err += dx;
        y0 += sy;
      }
    }
  }
}

// to do: merge with drawLine to support 2D and 3D
void VirtualLayer::drawLine3D(uint8_t x1, uint8_t y1, uint8_t z1, uint8_t x2, uint8_t y2, uint8_t z2, CRGB color, bool soft, uint8_t depth) {
  // WLEDMM shorten line according to depth
  if (depth < UINT8_MAX) {
    if (depth == 0) return;  // nothing to paint
    if (depth < 2) {
      x2 = x1;
      y2 = y1;
      z2 = z1;
    }  // single pixel
    else {  // shorten line
      x1 *= 2;
      y1 *= 2;
      z1 *= 2;                                                 // we do everything "*2" for better rounding
      int dx1 = ((int(2 * x2) - int(x1)) * int(depth)) / 255;  // X distance, scaled down by depth
      int dy1 = ((int(2 * y2) - int(y1)) * int(depth)) / 255;  // Y distance, scaled down by depth
      int dz1 = ((int(2 * z2) - int(z1)) * int(depth)) / 255;  // Y distance, scaled down by depth
      x1 = (x1 + dx1 + 1) / 2;
      y1 = (y1 + dy1 + 1) / 2;
      z1 = (z1 + dz1 + 1) / 2;
      x1 /= 2;
      y1 /= 2;
      z1 /= 2;
    }
  }

  // to do implement soft

  // Bresenham
  setRGB({x1, y1, z1}, color);
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
      setRGB({x1, y1, z1}, color);
    }

    // Driving axis is Y-axis"
  } else if (dy >= dx && dy >= dz) {
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
      setRGB({x1, y1, z1}, color);
    }

    // Driving axis is Z-axis"
  } else {
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
      setRGB({x1, y1, z1}, color);
    }
  }
}

void VirtualLayer::drawCircle(int cx, int cy, uint8_t radius, CRGB col, bool soft) {
  if (radius == 0) return;
  if (soft) {
    // Xiaolin Wu’s algorithm
    int rsq = radius * radius;
    int x = 0;
    int y = radius;
    unsigned oldFade = 0;
    while (x < y) {
      float yf = sqrtf(float(rsq - x * x));              // needs to be floating point
      unsigned fade = float(0xFFFF) * (ceilf(yf) - yf);  // how much color to keep
      if (oldFade > fade) y--;
      oldFade = fade;
      setRGB(Coord3D(cx + x, cy + y, 0), blend(col, getRGB(Coord3D(cx + x, cy + y, 0)), fade));
      setRGB(Coord3D(cx - x, cy + y), blend(col, getRGB(Coord3D(cx - x, cy + y)), fade));
      setRGB(Coord3D(cx + x, cy - y), blend(col, getRGB(Coord3D(cx + x, cy - y)), fade));
      setRGB(Coord3D(cx - x, cy - y), blend(col, getRGB(Coord3D(cx - x, cy - y)), fade));
      setRGB(Coord3D(cx + y, cy + x), blend(col, getRGB(Coord3D(cx + y, cy + x)), fade));
      setRGB(Coord3D(cx - y, cy + x), blend(col, getRGB(Coord3D(cx - y, cy + x)), fade));
      setRGB(Coord3D(cx + y, cy - x), blend(col, getRGB(Coord3D(cx + y, cy - x)), fade));
      setRGB(Coord3D(cx - y, cy - x), blend(col, getRGB(Coord3D(cx - y, cy - x)), fade));
      setRGB(Coord3D(cx + x, cy + y - 1), blend(getRGB(Coord3D(cx + x, cy + y - 1)), col, fade));
      setRGB(Coord3D(cx - x, cy + y - 1), blend(getRGB(Coord3D(cx - x, cy + y - 1)), col, fade));
      setRGB(Coord3D(cx + x, cy - y + 1), blend(getRGB(Coord3D(cx + x, cy - y + 1)), col, fade));
      setRGB(Coord3D(cx - x, cy - y + 1), blend(getRGB(Coord3D(cx - x, cy - y + 1)), col, fade));
      setRGB(Coord3D(cx + y - 1, cy + x), blend(getRGB(Coord3D(cx + y - 1, cy + x)), col, fade));
      setRGB(Coord3D(cx - y + 1, cy + x), blend(getRGB(Coord3D(cx - y + 1, cy + x)), col, fade));
      setRGB(Coord3D(cx + y - 1, cy - x), blend(getRGB(Coord3D(cx + y - 1, cy - x)), col, fade));
      setRGB(Coord3D(cx - y + 1, cy - x), blend(getRGB(Coord3D(cx - y + 1, cy - x)), col, fade));
      x++;
    }
  } else {
    // Bresenham’s Algorithm
    int d = 3 - (2 * radius);
    int y = radius, x = 0;
    while (y >= x) {
      setRGB(Coord3D(cx + x, cy + y), col);
      setRGB(Coord3D(cx - x, cy + y), col);
      setRGB(Coord3D(cx + x, cy - y), col);
      setRGB(Coord3D(cx - x, cy - y), col);
      setRGB(Coord3D(cx + y, cy + x), col);
      setRGB(Coord3D(cx - y, cy + x), col);
      setRGB(Coord3D(cx + y, cy - x), col);
      setRGB(Coord3D(cx - y, cy - x), col);
      x++;
      if (d > 0) {
        y--;
        d += 4 * (x - y) + 10;
      } else {
        d += 4 * x + 6;
      }
    }
  }
}

  #include "../misc/font/console_font_4x6.h"
  #include "../misc/font/console_font_5x12.h"
  #include "../misc/font/console_font_5x8.h"
  #include "../misc/font/console_font_6x8.h"
  #include "../misc/font/console_font_7x9.h"

// shift is used by drawText indicating which letter it is drawing
void VirtualLayer::drawCharacter(unsigned char chr, int x, int y, uint8_t font, CRGB col, uint16_t shiftPixel, uint16_t shiftChr) {
  if (chr < 32 || chr > 126) return;  // only ASCII 32-126 supported
  chr -= 32;                          // align with font table entries

  Coord3D fontSize;
  switch (font % 5) {
  case 0:
    fontSize.x = 4;
    fontSize.y = 6;
    break;
  case 1:
    fontSize.x = 5;
    fontSize.y = 8;
    break;
  case 2:
    fontSize.x = 5;
    fontSize.y = 12;
    break;
  case 3:
    fontSize.x = 6;
    fontSize.y = 8;
    break;
  case 4:
    fontSize.x = 7;
    fontSize.y = 9;
    break;
  }

  Coord3D chrPixel;
  for (chrPixel.y = 0; chrPixel.y < fontSize.y; chrPixel.y++) {  // character height
    Coord3D pixel;
    pixel.z = 0;
    pixel.y = y + chrPixel.y;
    if (pixel.y >= 0 && pixel.y < size.y) {
      byte bits = 0;
      switch (font % 5) {
      case 0:
        bits = pgm_read_byte_near(&console_font_4x6[(chr * fontSize.y) + chrPixel.y]);
        break;
      case 1:
        bits = pgm_read_byte_near(&console_font_5x8[(chr * fontSize.y) + chrPixel.y]);
        break;
      case 2:
        bits = pgm_read_byte_near(&console_font_5x12[(chr * fontSize.y) + chrPixel.y]);
        break;
      case 3:
        bits = pgm_read_byte_near(&console_font_6x8[(chr * fontSize.y) + chrPixel.y]);
        break;
      case 4:
        bits = pgm_read_byte_near(&console_font_7x9[(chr * fontSize.y) + chrPixel.y]);
        break;
      }

      for (chrPixel.x = 0; chrPixel.x < fontSize.x; chrPixel.x++) {
        // x adjusted by: chr in text, scroll value, font column
        pixel.x = (x + shiftChr * fontSize.x + shiftPixel + (fontSize.x - 1) - chrPixel.x) % size.x;
        if ((pixel.x >= 0 && pixel.x < size.x) && ((bits >> (chrPixel.x + (8 - fontSize.x))) & 0x01)) {  // bit set & drawing on-screen
          setRGB(pixel, col);
        }
      }
    }
  }
}

void VirtualLayer::drawText(const char* text, int x, int y, uint8_t font, CRGB col, uint16_t shiftPixel) {
  const int numberOfChr = text ? strnlen(text, 256) : 0;  // max 256 charcters
  for (int shiftChr = 0; shiftChr < numberOfChr; shiftChr++) {
    drawCharacter(text[shiftChr], x, y, font, col, shiftPixel, shiftChr);
  }
}

#endif  // FT_MOONLIGHT