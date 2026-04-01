/**
    @title     MoonLight
    @file      VirtualLayer.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

  #include "VirtualLayer.h"

  #include "MoonBase/Nodes.h"
  #include "MoonBase/utilities/LayerFunctions.h"
  #include "PhysicalLayer.h"

// convenience functions to call fastled functions out of the Leds namespace (there naming conflict)
void fastled_fadeToBlackBy(CRGB* leds, uint16_t num_leds, uint8_t fadeBy) { fadeToBlackBy(leds, num_leds, fadeBy); }  // supports max UINT16_MAX leds !
void fastled_fill_solid(CRGB* targetArray, int numToFill, const CRGB& color) { fill_solid(targetArray, numToFill, color); }

VirtualLayer::VirtualLayer() { EXT_LOGV(ML_TAG, "constructor"); }

VirtualLayer::~VirtualLayer() {
  EXT_LOGV(ML_TAG, "destructor");

  for (Node* node : nodes) {
    // node->destructor();
    delete node;
  }
  nodes.clear();

  // clear array of array of indexes
  for (std::vector<nrOfLights_t>& mappingTableIndex : mappingTableIndexes) {
    mappingTableIndex.clear();
  }
  mappingTableIndexes.clear();
  // clear mapping table
  freeMB(mappingTable);
  freeMB(virtualChannels);
}

void VirtualLayer::setup() {
  // no node setup here as done in addNode !
}

void VirtualLayer::loop() {
  if (nodes.empty()) return;  // skip empty layers (no effects assigned)

  // Consume fadeBy: scale virtualChannels before running effects this frame
  if (fadeBy > 0 && virtualChannels) {
    uint8_t cpl = layerP->lights.header.channelsPerLight;
    if (cpl == 3 && nrOfLights < UINT16_MAX) {
      fastled_fadeToBlackBy(reinterpret_cast<CRGB*>(virtualChannels), (uint16_t)nrOfLights, fadeBy);
    } else {
      uint8_t scale = 255 - fadeBy;
      for (nrOfLights_t i = 0; i < nrOfLights; i++) {
        uint8_t* vch = &virtualChannels[i * cpl];
        reinterpret_cast<CRGB*>(&vch[layerP->lights.header.offsetRGBW])->nscale8(scale);
        if (layerP->lights.header.offsetWhite  != UINT8_MAX) vch[layerP->lights.header.offsetRGBW + 3] = scale8(vch[layerP->lights.header.offsetRGBW + 3], scale);
        if (layerP->lights.header.offsetWhite2 != UINT8_MAX) vch[layerP->lights.header.offsetRGBW + 4] = scale8(vch[layerP->lights.header.offsetRGBW + 4], scale);
        if (layerP->lights.header.offsetRGBW1  != UINT8_MAX) { reinterpret_cast<CRGB*>(&vch[layerP->lights.header.offsetRGBW1])->nscale8(scale); vch[layerP->lights.header.offsetRGBW1 + 3] = scale8(vch[layerP->lights.header.offsetRGBW1 + 3], scale); }
        if (layerP->lights.header.offsetRGBW2  != UINT8_MAX) { reinterpret_cast<CRGB*>(&vch[layerP->lights.header.offsetRGBW2])->nscale8(scale); vch[layerP->lights.header.offsetRGBW2 + 3] = scale8(vch[layerP->lights.header.offsetRGBW2 + 3], scale); }
        if (layerP->lights.header.offsetRGBW3  != UINT8_MAX) { reinterpret_cast<CRGB*>(&vch[layerP->lights.header.offsetRGBW3])->nscale8(scale); vch[layerP->lights.header.offsetRGBW3 + 3] = scale8(vch[layerP->lights.header.offsetRGBW3 + 3], scale); }
      }
    }
  }
  fadeBy = 0;

  // Reset dimmer channels to default before effects run (or to 0 when brightness==0 to avoid
  // stale values reaching compositeTo() when effects are skipped by the early-return below).
  if (layerP->lights.header.offsetBrightness != UINT8_MAX) {
    for (int i = 0; i < nrOfLights; i++) {
      setBrightness(i, 255);   // will be corrected with globalbrighness
      setBrightness2(i, 255);  // will be corrected with globalbrighness
    }
  }

  if (brightness == 0) return;  // compositeTo() will output black — skip running effects

  // for virtual nodes
  if (prevSize != size) EXT_LOGD(ML_TAG, "onSizeChanged V %d,%d,%d -> %d,%d,%d", prevSize.x, prevSize.y, prevSize.z, size.x, size.y, size.z);
  for (Node* node : nodes) {
    if (prevSize != size) {
      xSemaphoreTake(*node->layerMutex, portMAX_DELAY);
      node->onSizeChanged(prevSize);
      xSemaphoreGive(*node->layerMutex);
    }
    if (node->on) {
      xSemaphoreTake(*node->layerMutex, portMAX_DELAY);
      node->loop();
      xSemaphoreGive(*node->layerMutex);
      addYield(10);
    }
  }
  prevSize = size;
  // brightness is applied in compositeTo(), not here
};

void VirtualLayer::loop20ms() {
  for (Node* node : nodes) {
    if (node->on) node->loop20ms();
  }
}

void VirtualLayer::presetCorrection(nrOfLights_t& indexP) const {
  // RGB2040 has physical layout with alternating 20-light segments:
  // virtual [0..19] -> physical [0..19], virtual [20..39] -> physical [40..59], etc.
  if (layerP->lights.header.lightPreset == lightPreset_RGB2040) indexP += (indexP / 20) * 20;
}

void VirtualLayer::addIndexP(PhysMap& physMap, nrOfLights_t indexP) {
  // EXT_LOGV(ML_TAG, "i:%d t:%d s:%d i:%d", indexP, physMap.mapType, mappingTableIndexes.size(), physMap.indexes);
  switch (physMap.mapType) {
  case m_zeroLights:  // zero -> one
    // case m_rgbColor:
    physMap.indexP = indexP;
    physMap.mapType = m_oneLight;
    break;
  case m_oneLight: {  // one -> more
    nrOfLights_t oldIndexP = physMap.indexP;
    // change to m_moreLights and add the old indexP and new indexP to the multiple indexP array
    // EXT_LOGD(ML_TAG, "%d %d %d", indexP, mappingTableIndexes.size(), mappingTableIndexesSizeUsed);
    mappingTableIndexesSizeUsed++;  // add a new slot in the mappingTableIndexes
    if (mappingTableIndexes.size() < mappingTableIndexesSizeUsed)
      mappingTableIndexes.push_back({oldIndexP, indexP});
    else
      mappingTableIndexes[mappingTableIndexesSizeUsed - 1] = {oldIndexP, indexP};

    physMap.indexesIndex = mappingTableIndexesSizeUsed - 1;  // array position
    physMap.mapType = m_moreLights;
    allOneLight = false;  // this layer now has at least one fan-out entry
    break;
  }
  case m_moreLights:  // more -> more
    // mappingTableIndexes.reserve(physMap.indexesIndex+1);
    mappingTableIndexes[physMap.indexesIndex].push_back(indexP);
    // EXT_LOGV(ML_TAG, " more %d", mappingTableIndexes.size());
    break;
  }
  // EXT_LOGV(ML_TAG, "");
}
nrOfLights_t VirtualLayer::XYZ(Coord3D& position) {
  // XYZ modifiers (this is not slowing things down as you might have expected ...)
  for (Node* node : nodes) {      // e.g. random or scrolling or rotate modifier
    if (node->on)                 //  && node->hasModifier()
      node->modifyXYZ(position);  // modifies the position
  }

  return XYZUnModified(position);
}



void VirtualLayer::fill_solid(const CRGB& color) {
  if (virtualChannels && layerP->lights.header.channelsPerLight == 3) {
    fastled_fill_solid(reinterpret_cast<CRGB*>(virtualChannels), nrOfLights, color);
  } else {
    for (nrOfLights_t index = 0; index < nrOfLights; index++) setRGB(index, color);
  }
}


void VirtualLayer::createMappingTableAndAddOneToOne() {
  if (mappingTableSize != size.x * size.y * size.z) {
    EXT_LOGD(ML_TAG, "Allocating mappingTable: nrOfLights=%d, sizeof(PhysMap)=%d, total bytes=%d", size.x * size.y * size.z, sizeof(PhysMap), size.x * size.y * size.z * sizeof(PhysMap));
    reallocMB2<PhysMap>(mappingTable, mappingTableSize, size.x * size.y * size.z, "mappingTable");
  }

  if (mappingTable && mappingTableSize) memset(mappingTable, 0, mappingTableSize * sizeof(PhysMap));  // on layout, set mappingTable to default PhysMap

  // EXT_LOGD(ML_TAG, "Filling mappingTable < %d", layerP->indexP);

  for (nrOfLights_t indexV = 0; indexV < MIN(layerP->indexP, mappingTableSize); indexV++) {
    addIndexP(mappingTable[indexV], indexV);
  }
}

void VirtualLayer::onLayoutPre() {
  if (nodes.empty()) return;  // skip layout for empty layers (no effects assigned)

  // resetMapping

  nrOfLights = 0;

  // apply percentage bounds to compute the virtual layer's window into the physical fixture
  computeLayerBounds(layerP->lights.header.size, startPct, endPct, startPhy, endPhy);

  size = endPhy - startPhy;
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

  mappingTableIndexesSizeUsed = 0;  // do not clear mappingTableIndexes, reuse it
  for (std::vector<nrOfLights_t>& mappingTableIndex : mappingTableIndexes) {
    mappingTableIndex.clear();
  }

  oneToOneMapping = true;  // addLight will set it to false as soon as irregularity is discovered
  allOneLight = true;      // addIndexP will set it to false as soon as any m_moreLights entry appears
}

bool VirtualLayer::addLight(Coord3D position) {
  if (nodes.empty()) return false;  // skip layout for empty layers

  // filter: positions outside the percentage-based bounds are blacked out (unmapped)
  bool outsideBounds = isOutsideLayerBounds(position, startPhy, endPhy);

  if (outsideBounds) {
    // treat as unmapped — same as modifier setting position to UINT16_MAX
    if (oneToOneMapping) {
      oneToOneMapping = false;
      createMappingTableAndAddOneToOne();
    }
    return false;  // this layer did not cover the pixel
  }

  // remap position to virtual coordinates (0-based within the layer's window)
  position = position - startPhy;

  // modifiers
  for (Node* node : nodes) {
    if (node->on)  //  && node->hasModifier()
      node->modifyPosition(position);
  }

  if (position.x != UINT16_MAX) {  // can be set to UINT16_MAX by modifier todo: check multiple modifiers
    nrOfLights_t indexV = XYZUnModified(position);

    if (oneToOneMapping && layerP->indexP != indexV) {
      oneToOneMapping = false;
      createMappingTableAndAddOneToOne();
    }

    nrOfLights = MAX(nrOfLights, indexV + 1);

    if (!oneToOneMapping) {
      if (indexV < mappingTableSize) {
        addIndexP(mappingTable[indexV], layerP->indexP);
      }
    }
  } else {
    if (oneToOneMapping) {
      // we found an irregularity, create the mapping table
      oneToOneMapping = false;
      createMappingTableAndAddOneToOne();
    }
    // modifier rejected this pixel (position.x == UINT16_MAX): leave channelsD untouched.
    // compositeLayers() zeroes channelsD before every composite step, so no explicit clear needed here.
  }
  return true;  // this layer covered the pixel
}

void VirtualLayer::onLayoutPost() {
  if (nodes.empty()) return;  // skip layout for empty layers

  // prepare logging:
  nrOfLights_t nrOfOneLight = 0;
  nrOfLights_t nrOfMoreLights = 0;
  nrOfLights_t nrOfZeroLights = 0;
  if (oneToOneMapping) {
    nrOfOneLight = nrOfLights;
    // free the mappingTables instead of preserve to allow mapping free memory
    if (mappingTableIndexes.size()) {
      EXT_LOGI(ML_TAG, "Clear mappingTableIndexes size %d / %d", mappingTableIndexes.size(), mappingTableIndexesSizeUsed);
      mappingTableIndexes.clear();
      mappingTableIndexesSizeUsed = 0;
    }
    if (mappingTable) {
      EXT_LOGI(ML_TAG, "Clear mappingTable size %d", mappingTableSize);
      freeMB(mappingTable);
      mappingTableSize = 0;
    }
  } else {
    EXT_LOGI(ML_TAG, "!oneToOne mapping !");
    for (size_t indexV = 0; indexV < MIN(nrOfLights, mappingTableSize); indexV++) {
      PhysMap& map = mappingTable[indexV];
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
        for (nrOfLights_t indexP : mappingTableIndexes[map.indexesIndex]) {
          // str += indexP;
          nrOfMoreLights++;
        }
        // EXT_LOGV(ML_TAG, "%d mapping >1: #ledsP : %s", i, str.c_str());
        break;
      }
      // else
      //   EXT_LOGV(ML_TAG, "%d no mapping", x);
    }
  }

  EXT_LOGI(ML_TAG, "V:%d x %d x %d = v:%d = 1:0:%d + 1:1:%d + mti:%d (1:m:%d)", size.x, size.y, size.z, nrOfLights, nrOfZeroLights, nrOfOneLight, mappingTableIndexesSizeUsed, nrOfMoreLights);

  // Allocate (or reuse) the per-layer virtual pixel buffer now that nrOfLights is final.
  size_t needed = (size_t)nrOfLights * layerP->lights.header.channelsPerLight;
  if (needed > virtualChannelsByteSize) {
    bool firstAlloc = (virtualChannelsByteSize == 0);
    freeMB(virtualChannels);
    virtualChannels = allocMB<uint8_t>(needed);
    virtualChannelsByteSize = virtualChannels ? needed : 0;
    EXT_LOGD(ML_TAG, "virtualChannels: %d bytes in %s", (int)needed, isInPSRAM(virtualChannels) ? "PSRAM" : "RAM");
    if (firstAlloc) { transitionBrightness = 0; startTransition(255, 500); }  // fade in when layer first comes to life
  }
  if (virtualChannels) memset(virtualChannels, 0, virtualChannelsByteSize);
}

void VirtualLayer::compositeTo(uint8_t* dest, const LightsHeader& header) {
  if (!virtualChannels || nodes.empty()) return;
  uint8_t cpl = header.channelsPerLight;
  uint8_t b = scale8(brightness, transitionBrightness);

  // Fast path for pure RGB (cpl==3): no white/control channels to check.
  // When oneToOneMapping is also true this is the tightest possible loop —
  // avoids forEachLightIndex dispatch and all UINT8_MAX-guarded branches.
  if (cpl == 3) {
    CRGB* src = reinterpret_cast<CRGB*>(virtualChannels);
    CRGB* dst = reinterpret_cast<CRGB*>(dest);
    if (oneToOneMapping) {
      if (b == 255) {
        for (nrOfLights_t i = 0; i < nrOfLights; i++) dst[i] += src[i];
      } else {
        for (nrOfLights_t i = 0; i < nrOfLights; i++) { CRGB c = src[i]; c.nscale8_video(b); dst[i] += c; }
      }
    } else if (allOneLight) {
      // Serpentine / shifted panel fast path: all mapped entries are m_oneLight —
      // direct table access, no switch dispatch, no forEachLightIndex overhead.
      for (nrOfLights_t indexV = 0; indexV < nrOfLights; indexV++) {
        if (mappingTable[indexV].mapType != m_oneLight) continue;  // skip unmapped pixels
        CRGB color = src[indexV];
        if (b < 255) color.nscale8_video(b);
        nrOfLights_t indexP = mappingTable[indexV].indexP;
        presetCorrection(indexP);
        dst[indexP] += color;
      }
    } else {
      // General 1:N path (e.g. modifiers that map 1D rings to 2D grid positions).
      for (nrOfLights_t indexV = 0; indexV < nrOfLights; indexV++) {
        CRGB color = src[indexV];
        if (b < 255) color.nscale8_video(b);
        forEachLightIndex(indexV, [&](nrOfLights_t indexP) { dst[indexP] += color; });
      }
    }
    return;
  }

  // General path for multi-channel lights (cpl > 3: RGBW, moving heads, etc.)
  for (nrOfLights_t indexV = 0; indexV < nrOfLights; indexV++) {
    uint8_t* vch = &virtualChannels[indexV * cpl];

    // Primary RGB — apply effective brightness, additive composite
    CRGB color = *reinterpret_cast<CRGB*>(&vch[header.offsetRGBW]);
    if (b < 255) color.nscale8_video(b);

    forEachLightIndex(indexV, [&](nrOfLights_t indexP) {
      uint8_t* dst = &dest[indexP * cpl];

      // Color channels: additive compositing (saturates at 255)
      *reinterpret_cast<CRGB*>(&dst[header.offsetRGBW]) += color;

      if (header.offsetWhite  != UINT8_MAX) {
        uint8_t w = vch[header.offsetRGBW + 3];  // canonical white slot: setWhite() writes here
        if (b < 255) w = scale8(w, b);
        dst[header.offsetWhite] = qadd8(dst[header.offsetWhite], w);  // driver wire-order destination
        if (header.offsetWhite2 != UINT8_MAX) {
          uint8_t w = vch[header.offsetRGBW + 4];  // canonical second-white slot
          if (b < 255) w = scale8(w, b);
          dst[header.offsetWhite2] = qadd8(dst[header.offsetWhite2], w);
        }
      }
      if (header.offsetRGBW1 != UINT8_MAX) {
        CRGB c = *reinterpret_cast<CRGB*>(&vch[header.offsetRGBW1]);
        if (b < 255) c.nscale8_video(b);
        *reinterpret_cast<CRGB*>(&dst[header.offsetRGBW1]) += c;
        uint8_t w = vch[header.offsetRGBW1 + 3];
        if (b < 255) w = scale8(w, b);
        dst[header.offsetRGBW1 + 3] = qadd8(dst[header.offsetRGBW1 + 3], w);
        if (header.offsetRGBW2 != UINT8_MAX) {
          CRGB c = *reinterpret_cast<CRGB*>(&vch[header.offsetRGBW2]);
          if (b < 255) c.nscale8_video(b);
          *reinterpret_cast<CRGB*>(&dst[header.offsetRGBW2]) += c;
          uint8_t w = vch[header.offsetRGBW2 + 3];
          if (b < 255) w = scale8(w, b);
          dst[header.offsetRGBW2 + 3] = qadd8(dst[header.offsetRGBW2 + 3], w);
          if (header.offsetRGBW3 != UINT8_MAX) {
            CRGB c = *reinterpret_cast<CRGB*>(&vch[header.offsetRGBW3]);
            if (b < 255) c.nscale8_video(b);
            *reinterpret_cast<CRGB*>(&dst[header.offsetRGBW3]) += c;
            uint8_t w = vch[header.offsetRGBW3 + 3];
            if (b < 255) w = scale8(w, b);
            dst[header.offsetRGBW3 + 3] = qadd8(dst[header.offsetRGBW3 + 3], w);
          }
        }
      }

      // Control channels (brightness, pan, tilt, zoom, rotate, gobo): copy — last layer wins.
      // Additive semantics don't apply to positional/control signals.
      // Brightness channels: setBrightness() already bakes in globalBrightness and layer brightness,
      // so only transitionBrightness needs to be applied here to keep dimmer channels in sync with
      // the layer fade-in/out without double-scaling.
      if (header.offsetBrightness  != UINT8_MAX) dst[header.offsetBrightness]  = transitionBrightness < 255 ? scale8(vch[header.offsetBrightness],  transitionBrightness) : vch[header.offsetBrightness];
      if (header.offsetBrightness2 != UINT8_MAX) dst[header.offsetBrightness2] = transitionBrightness < 255 ? scale8(vch[header.offsetBrightness2], transitionBrightness) : vch[header.offsetBrightness2];
      if (header.offsetPan         != UINT8_MAX) dst[header.offsetPan]         = vch[header.offsetPan];
      if (header.offsetTilt        != UINT8_MAX) dst[header.offsetTilt]        = vch[header.offsetTilt];
      if (header.offsetZoom        != UINT8_MAX) dst[header.offsetZoom]        = vch[header.offsetZoom];
      if (header.offsetRotate      != UINT8_MAX) dst[header.offsetRotate]      = vch[header.offsetRotate];
      if (header.offsetGobo        != UINT8_MAX) dst[header.offsetGobo]        = vch[header.offsetGobo];
    });
  }
}

bool VirtualLayer::isMapped(nrOfLights_t indexV) const {
  return oneToOneMapping || (indexV < mappingTableSize && (mappingTable[indexV].mapType == m_oneLight || mappingTable[indexV].mapType == m_moreLights));
}

void VirtualLayer::blur1d(fract8 blur_amount, nrOfLights_t x) {
  const uint8_t keep = 255 - blur_amount;
  const uint8_t seep = blur_amount >> 1;
  CRGB carryover = CRGB::Black;
  for (nrOfLights_t row = 0; row < size.y; ++row) {
    CRGB cur = getRGB(Coord3D(x, row));
    CRGB part = cur;
    part.nscale8(seep);
    cur.nscale8(keep);
    cur += carryover;
    if (row) addRGB(Coord3D(x, row - 1), part);
    setRGB(Coord3D(x, row), cur);
    carryover = part;
  }
  if (size.y) addRGB(Coord3D(x, size.y - 1), carryover);
}

void VirtualLayer::blur2d(fract8 blur_amount) {
  blurRows(blur_amount);
  blurColumns(blur_amount);
}

void VirtualLayer::blurRows(fract8 blur_amount) {
  uint8_t keep = 255 - blur_amount;
  uint8_t seep = blur_amount >> 1;
  for (nrOfLights_t row = 0; row < size.y; row++) {
    CRGB carryover = CRGB::Black;
    for (nrOfLights_t col = 0; col < size.x; col++) {
      CRGB cur = getRGB(Coord3D(col, row));
      CRGB part = cur;
      part.nscale8(seep);
      cur.nscale8(keep);
      cur += carryover;
      if (col) addRGB(Coord3D(col - 1, row), part);
      setRGB(Coord3D(col, row), cur);
      carryover = part;
    }
    if (size.x) addRGB(Coord3D(size.x - 1, row), carryover);
  }
}

void VirtualLayer::blurColumns(fract8 blur_amount) {
  uint8_t keep = 255 - blur_amount;
  uint8_t seep = blur_amount >> 1;
  for (nrOfLights_t col = 0; col < size.x; ++col) {
    CRGB carryover = CRGB::Black;
    for (nrOfLights_t row = 0; row < size.y; row++) {
      CRGB cur = getRGB(Coord3D(col, row));
      CRGB part = cur;
      part.nscale8(seep);
      cur.nscale8(keep);
      cur += carryover;
      if (row) addRGB(Coord3D(col, row - 1), part);
      setRGB(Coord3D(col, row), cur);
      carryover = part;
    }
    if (size.y) addRGB(Coord3D(col, size.y - 1), carryover);
  }
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