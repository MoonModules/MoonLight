/**
    @title     MoonLight
    @file      VirtualLayer.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
           https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

  #include <FastLED.h>

  #include <vector>

  #include "MoonBase/utilities/LayerFunctions.h"
  #include "PhysMap.h"  // pure types: MapTypeEnum, PhysMap — no ESP32 deps
  #include "PhysicalLayer.h"

// ----------------------------------------------------------------------------
// VirtualLayer — a logical 3-D grid of virtual pixels mapped to physical lights.
// Effects and modifiers operate on virtual pixels; this layer translates them
// to physical channel writes via the mapping table.
//
// Lifecycle: constructed by PhysicalLayer, setup() called once, then
// loop() / loop20ms() called every frame from effectTask / SvelteKit task.
// The mapping table is rebuilt by onLayoutPre → addLight → onLayoutPost
// whenever requestMapVirtual is set.
// ----------------------------------------------------------------------------
class VirtualLayer {
 public:
  // Number of virtual lights (set by onLayoutPre, updated by addLight).
  nrOfLights_t nrOfLights = 256;

  // Virtual grid dimensions (may differ from physical size if modifiers are applied).
  Coord3D size = {16, 16, 1};  // not 0,0,0 to avoid divide-by-zero in effects like Octopus2D

  // Convenience aliases updated every time size changes.
  Coord3D start = {0, 0, 0}, middle = size / 2, end = size - 1;

  // Virtual→physical mapping table (one PhysMap per virtual pixel).
  // Kept allocated between layout passes to avoid heap fragmentation.
  PhysMap* mappingTable = nullptr;
  size_t mappingTableSize = 0;

  // Secondary lookup for m_moreLights entries: each entry is a list of physical indices.
  // Preserved and reused across layout passes (entries are cleared, vector not freed).
  std::vector<std::vector<nrOfLights_t>, VectorRAMAllocator<std::vector<nrOfLights_t>>> mappingTableIndexes;
  nrOfLights_t mappingTableIndexesSizeUsed = 0;

  // Pointer to the owning physical layer (set by PhysicalLayer constructor).
  PhysicalLayer* layerP = nullptr;

  // Effect / modifier / layout nodes assigned to this virtual layer.
  std::vector<Node*, VectorRAMAllocator<Node*>> nodes;

  // Dimensionality of the current effect (1D / 2D / 3D).
  uint8_t effectDimension = _3D;

  // Dimensionality of the virtual layer (derived from size in onLayoutPre).
  uint8_t layerDimension = UINT8_MAX;

  // Previous size, used to detect size changes and trigger onSizeChanged().
  Coord3D prevSize = {0,0,0};

  // When true, every virtual index maps 1:1 to a physical index (no mappingTable needed).
  bool oneToOneMapping = false;

  // Per-layer brightness (0–255). Scales pixel output within this layer. Default 255 = full.
  uint8_t brightness = 255;

  // Transition animation: auto-stepped brightness overlay applied in compositeTo() independently
  // of the user-set brightness. Allows smooth fade-in/out without changing the brightness control.
  // Effective brightness = scale8(brightness, transitionBrightness).
  uint8_t transitionBrightness = 255;  // current animated value (255 = no extra dimming)
  uint8_t transitionTarget     = 255;  // value to animate toward; animation stops here
  int16_t transitionStep       = 0;    // signed delta added per frame; 0 = idle

  // Fade amount requested by effects this frame (consumed by PhysicalLayer::loop() next frame).
  uint8_t fadeBy = 0;

  // Per-layer virtual pixel buffer. Effects write here (indexed by virtual pixel index, not
  // physical). compositeTo() maps virtualChannels → physical channelsD after all layers render.
  // Allocated in onLayoutPost(), freed in destructor. nullptr until first layout completes.
  uint8_t* virtualChannels     = nullptr;
  size_t   virtualChannelsByteSize = 0;

  // Layer boundaries as percentages (0–100) of the total fixture size. Default 100% = full fixture.
  Coord3D startPct = {0, 0, 0};
  Coord3D endPct = {100, 100, 100};

  // Physical coordinates computed from percentages during onLayoutPre (used by addLight to filter/remap).
  Coord3D startPhy = {0, 0, 0};
  Coord3D endPhy = {0, 0, 0};

  VirtualLayer();
  ~VirtualLayer();

  // No per-node setup here — nodes are set up by addNode() individually.
  void setup();

  // Run one effect frame: apply fade, update brightness channels, loop all effect nodes.
  // Called from effectTask (Core 0) every frame.
  void loop();

  // Composite virtualChannels into dest[], applying per-layer brightness and the physical
  // mapping (forEachLightIndex). Uses additive compositing (saturates at 255) so multiple
  // layers at full brightness sum correctly, and overlapping layers at reduced brightness
  // crossfade naturally. Called by PhysicalLayer::loop() after each layer->loop().
  void compositeTo(uint8_t* dest, const LightsHeader& header);

  // Run 20 ms periodic updates for all nodes (called from SvelteKit task, Core 1).
  void loop20ms();

  // Register an additional physical light index for a given virtual pixel.
  // Upgrades physMap from m_zeroLights → m_oneLight → m_moreLights as needed.
  void addIndexP(PhysMap& physMap, nrOfLights_t indexP);

  // Map a 3-D virtual coordinate to a flat virtual index, applying all active modifiers.
  // NOTE: position is passed by reference because modifiers may alter it in-place.
  nrOfLights_t XYZ(Coord3D& position);

  // Map a 3-D virtual coordinate to a flat virtual index without applying modifiers.
  // Inline for hot-path use by effects that skip XYZ modifier processing.
  nrOfLights_t XYZUnModified(const Coord3D& position) const { return position.x + position.y * size.x + position.z * size.x * size.y; }

  // Apply lightPreset correction to a physical index (e.g. RGB2040 interleaving).
  void presetCorrection(nrOfLights_t& indexP) const;

  // ----------------------------------------------------------------------------
  // forEachLightIndex — core dispatch: invoke callback for every physical light
  // that a given virtual index maps to.
  // Must remain in the header as a template (cannot be moved to .cpp).
  // onlyOne = true: stop after the first physical light (used by get* functions).
  // ----------------------------------------------------------------------------
  template <typename Callback>
  void forEachLightIndex(const nrOfLights_t indexV, Callback&& callback, bool onlyOne = false) {
    if (indexV < mappingTableSize) {
      switch (mappingTable[indexV].mapType) {
      case m_oneLight: {
        nrOfLights_t indexP = mappingTable[indexV].indexP;
        presetCorrection(indexP);
        callback(indexP);
        break;
      }
      case m_moreLights:
        if (mappingTable[indexV].indexesIndex < mappingTableIndexes.size()) {
          for (nrOfLights_t indexP : mappingTableIndexes[mappingTable[indexV].indexesIndex]) {
            presetCorrection(indexP);
            callback(indexP);
            if (onlyOne) return;
          }
        }
        break;
      }
    } else {                                                                                      // no mapping table — direct pass-through
      // if ((indexV + 1) * layerP->lights.header.channelsPerLight <= layerP->lights.maxChannels) {  // bounds check
        callback(indexV);                                                                         // no presetCorrection here (lightPreset_RGB2040 has a mapping)
      // } else
      //   EXT_LOGD(ML_TAG, "%d", indexV);
    }
  }

  // ----------------------------------------------------------------------------
  // Pixel write methods — inline hot path (called per pixel per frame).
  // All positions/indices are virtual (mapped to physical by forEachLightIndex).
  // ----------------------------------------------------------------------------

  // Set one channel value (by offset within a light's channel block) at virtual index indexV.
  // Writes to virtualChannels; compositeTo() maps to channelsD after all layers render.
  void setLight(const nrOfLights_t indexV, uint8_t offset, uint8_t value) {
    if (virtualChannels && indexV < nrOfLights)
      virtualChannels[indexV * layerP->lights.header.channelsPerLight + offset] = value;
  }

  // Write RGB colour to virtualChannels at indexV.
  // compositeTo() maps to channelsD after all layers have rendered.
  // Per-layer brightness is applied in compositeTo().
  void setRGB(const nrOfLights_t indexV, CRGB color) {
    if (virtualChannels && indexV < nrOfLights) {
      memcpy(&virtualChannels[indexV * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGBW], (void*)&color, sizeof(color));
    } else if (indexV < mappingTableSize && mappingTable[indexV].mapType == m_zeroLights) {
      // m_zeroLights: store in mappingTable so getRGB() can read it back before first layout completes
  #ifdef BOARD_HAS_PSRAM
      memcpy(mappingTable[indexV].rgb, (void*)&color, 3);
  #else
      mappingTable[indexV].rgb = ((MIN(color[0] + 3, 255) >> 3) << 9) + ((MIN(color[1] + 3, 255) >> 3) << 4) + (MIN(color[2] + 7, 255) >> 4);
  #endif
    }
  }
  void setRGB(Coord3D pos, CRGB color) { setRGB(XYZ(pos), color); }

  // Write white channel value (only when offsetWhite is configured).
  void setWhite(const nrOfLights_t indexV, const uint8_t value) {
    if (layerP->lights.header.offsetWhite != UINT8_MAX) {
      setLight(indexV, layerP->lights.header.offsetRGBW + 3, value);
    }
  }
  void setWhite(Coord3D pos, const uint8_t value) { setWhite(XYZ(pos), value); }

  // Write brightness channel, scaled by global and layer brightness (only when offsetBrightness is configured).
  void setBrightness(const nrOfLights_t indexV, uint8_t value) {
    if (layerP->lights.header.offsetBrightness != UINT8_MAX) {
      setLight(indexV, layerP->lights.header.offsetBrightness, scale8x8(value, layerP->lights.header.brightness, brightness));
    }
  }
  void setBrightness(Coord3D pos, const uint8_t value) { setBrightness(XYZ(pos), value); }

  // Moving-head channel setters — each is a no-op if the corresponding offset is not configured.
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

  // Write RGB to secondary RGBW blocks (moving heads with multiple colour wheels).
  void setRGB1(const nrOfLights_t indexV, CRGB color) {
    if (layerP->lights.header.offsetRGBW1 != UINT8_MAX && virtualChannels && indexV < nrOfLights)
      memcpy(&virtualChannels[indexV * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGBW1], (void*)&color, sizeof(color));
  }
  void setRGB1(Coord3D pos, CRGB color) { setRGB1(XYZ(pos), color); }

  void setRGB2(const nrOfLights_t indexV, CRGB color) {
    if (layerP->lights.header.offsetRGBW2 != UINT8_MAX && virtualChannels && indexV < nrOfLights)
      memcpy(&virtualChannels[indexV * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGBW2], (void*)&color, sizeof(color));
  }
  void setRGB2(Coord3D pos, CRGB color) { setRGB2(XYZ(pos), color); }

  void setRGB3(const nrOfLights_t indexV, CRGB color) {
    if (layerP->lights.header.offsetRGBW3 != UINT8_MAX && virtualChannels && indexV < nrOfLights)
      memcpy(&virtualChannels[indexV * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGBW3], (void*)&color, sizeof(color));
  }
  void setRGB3(Coord3D pos, CRGB color) { setRGB3(XYZ(pos), color); }

  // Write secondary brightness channel, scaled by global and layer brightness.
  void setBrightness2(const nrOfLights_t indexV, uint8_t value) {
    if (layerP->lights.header.offsetBrightness2 != UINT8_MAX) {
      setLight(indexV, layerP->lights.header.offsetBrightness2, scale8x8(value, layerP->lights.header.brightness, brightness));
    }
  }
  void setBrightness2(Coord3D pos, const uint8_t value) { setBrightness2(XYZ(pos), value); }

  // ----------------------------------------------------------------------------
  // Pixel read methods — inline hot path.
  // Reads from the first physical light mapped to indexV (onlyOne = true).
  // ----------------------------------------------------------------------------

  // Read one channel value from the virtual buffer at indexV (falls back to channelsD before first layout).
  uint8_t getLight(const nrOfLights_t indexV, uint8_t offset) {
    if (virtualChannels && indexV < nrOfLights)
      return virtualChannels[indexV * layerP->lights.header.channelsPerLight + offset];
    return 0;  // before first layout completes
  }

  // Read RGB from the primary RGBW block at indexV.
  // Phase 1: reads from virtualChannels when available — returns THIS layer's own pixel value,
  // not the shared blended channelsD. This is the key correctness fix for multi-layer effects
  // that use getRGB+setRGB patterns (blur, scroll, blend): they now operate on their own buffer.
  CRGB getRGB(const nrOfLights_t indexV) {
    if (virtualChannels && indexV < nrOfLights) {
      return *reinterpret_cast<CRGB*>(&virtualChannels[indexV * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGBW]);
    }
    // Fallback: no virtualChannels yet (before first layout completes)
    if (indexV < mappingTableSize && mappingTable[indexV].mapType == m_zeroLights) {
  #ifdef BOARD_HAS_PSRAM
      return *(CRGB*)&mappingTable[indexV].rgb;
  #else
      CRGB result;
      ((uint8_t*)&result)[0] = (mappingTable[indexV].rgb >> 9) << 3;           // R: bits [13:9]
      ((uint8_t*)&result)[1] = ((mappingTable[indexV].rgb >> 4) & 0x1F) << 3;  // G: bits [8:4]
      ((uint8_t*)&result)[2] = (mappingTable[indexV].rgb & 0x0F) << 4;         // B: bits [3:0]
      return result;
  #endif
    }
    return CRGB::Black;  // before first layout completes
  }
  CRGB getRGB(Coord3D pos) { return getRGB(XYZ(pos)); }

  // Add color to the existing RGB value at a virtual position (saturating add).
  void addRGB(const Coord3D& position, const CRGB& color) {
    CRGB result = getRGB(position);
    result += color;
    setRGB(position, result);
  }

  // Blend a colour toward the existing pixel (blendAmount 0 = all new, 255 = all existing).
  void blendColor(const nrOfLights_t indexV, const CRGB& color, uint8_t blendAmount) { setRGB(indexV, blend(color, getRGB(indexV), blendAmount)); }
  void blendColor(Coord3D position, const CRGB& color, const uint8_t blendAmount) { blendColor(XYZ(position), color, blendAmount); }

  // Read white channel from the first physical light at indexV.
  uint8_t getWhite(const nrOfLights_t indexV) { return getLight(indexV, layerP->lights.header.offsetRGBW + 3); }
  uint8_t getWhite(Coord3D pos) { return getWhite(XYZ(pos)); }

  // Read RGB from secondary colour blocks.
  CRGB getRGB1(const nrOfLights_t indexV) {
    if (layerP->lights.header.offsetRGBW1 == UINT8_MAX) return CRGB::Black;
    if (virtualChannels && indexV < nrOfLights)
      return *reinterpret_cast<CRGB*>(&virtualChannels[indexV * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGBW1]);
    return CRGB::Black;
  }
  CRGB getRGB1(Coord3D pos) { return getRGB1(XYZ(pos)); }

  CRGB getRGB2(const nrOfLights_t indexV) {
    if (layerP->lights.header.offsetRGBW2 == UINT8_MAX) return CRGB::Black;
    if (virtualChannels && indexV < nrOfLights)
      return *reinterpret_cast<CRGB*>(&virtualChannels[indexV * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGBW2]);
    return CRGB::Black;
  }
  CRGB getRGB2(Coord3D pos) { return getRGB2(XYZ(pos)); }

  CRGB getRGB3(const nrOfLights_t indexV) {
    if (layerP->lights.header.offsetRGBW3 == UINT8_MAX) return CRGB::Black;
    if (virtualChannels && indexV < nrOfLights)
      return *reinterpret_cast<CRGB*>(&virtualChannels[indexV * layerP->lights.header.channelsPerLight + layerP->lights.header.offsetRGBW3]);
    return CRGB::Black;
  }
  CRGB getRGB3(Coord3D pos) { return getRGB3(XYZ(pos)); }

  // ----------------------------------------------------------------------------
  // Frame-level fill / fade operations
  // ----------------------------------------------------------------------------

  // Schedule a fade-to-black for this layer. Applied to virtualChannels at the start of the
  // next loop() call, before effects run, so trail effects accumulate correctly.
  void fadeToBlackBy(uint8_t amount = 255) { fadeBy = fadeBy ? MIN(fadeBy, amount) : amount; }

  // Start an animated brightness transition toward target (0=invisible, 255=full).
  // durationMs: approximate duration assuming ~50 fps (20 ms/frame).
  // Calling with durationMs=0 snaps immediately.
  void startTransition(uint8_t target, uint16_t durationMs) {
    transitionTarget = target;
    if (transitionBrightness == target || durationMs == 0) {
      transitionBrightness = target;
      transitionStep = 0;
      return;
    }
    uint16_t frames = MAX((durationMs + 19) / 20, 1);  // round up, ~50 fps, min 1
    int16_t diff = (int16_t)target - (int16_t)transitionBrightness;
    transitionStep = diff / (int16_t)frames;
    if (transitionStep == 0) transitionStep = (diff > 0) ? 1 : -1;  // ensure progress
  }

  // Fill all virtual lights with a solid colour.
  void fill_solid(const CRGB& color);

  // ----------------------------------------------------------------------------
  // Layout mapping (called from PhysicalLayer::mapLayout during pass 2)
  // ----------------------------------------------------------------------------

  // Reset mapping state and apply modifier sizes before addLight() calls.
  void onLayoutPre();

  // Allocate (or reuse) the mapping table and seed it with a 1:1 mapping.
  // Called internally when a non-1:1 mapping is first detected.
  void createMappingTableAndAddOneToOne();

  // Finalise the mapping table after all addLight() calls; log mapping statistics.
  void onLayoutPost();

  // Register one physical light (at the given position) into this virtual layer.
  // Applies modifier positions and routes to addIndexP().
  void addLight(Coord3D position);

  // Returns true if the virtual light at indexV has at least one physical light mapped to it.
  bool isMapped(nrOfLights_t indexV) const;

  // ----------------------------------------------------------------------------
  // Drawing primitives — operate on the virtual grid via setRGB/getRGB.
  // Large implementations live in VirtualLayer.cpp.
  // ----------------------------------------------------------------------------

  // 1-D blur along a column (x) of the virtual grid.
  void blur1d(fract8 blur_amount, nrOfLights_t x = 0);

  // 2-D blur: blur all rows then all columns.
  void blur2d(fract8 blur_amount);

  // Blur every row independently (horizontal 1-D blur).
  void blurRows(fract8 blur_amount);

  // Blur every column independently (vertical 1-D blur).
  void blurColumns(fract8 blur_amount);

  // Draw a 2-D line between (x0,y0) and (x1,y1).
  // soft = true: Xiaolin Wu anti-aliasing; depth: shorten line (255 = full length).
  void drawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, CRGB color, bool soft = false, uint8_t depth = UINT8_MAX);

  // Draw a 3-D line from point a to point b (convenience overload).
  void drawLine3D(const Coord3D& a, Coord3D b, CRGB color, bool soft = false, uint8_t depth = UINT8_MAX) { drawLine3D(a.x, a.y, a.z, b.x, b.y, b.z, color, soft, depth); }

  // Draw a 3-D line using Bresenham's 3-D algorithm.
  void drawLine3D(uint8_t x1, uint8_t y1, uint8_t z1, uint8_t x2, uint8_t y2, uint8_t z2, CRGB color, bool soft = false, uint8_t depth = UINT8_MAX);

  // Draw a circle centred at (cx, cy) with the given radius.
  // soft = true: Xiaolin Wu anti-aliasing.
  void drawCircle(int cx, int cy, uint8_t radius, CRGB col, bool soft);

  // Render one ASCII character (32–126) at pixel position (x, y).
  // shiftPixel: sub-character horizontal scroll offset; shiftChr: character index within a string.
  void drawCharacter(unsigned char chr, int x = 0, int y = 0, uint8_t font = 0, CRGB col = CRGB::Red, uint16_t shiftPixel = 0, uint16_t shiftChr = 0);

  // Render a null-terminated ASCII string at pixel position (x, y), with optional horizontal scroll.
  void drawText(const char* text, int x = 0, int y = 0, uint8_t font = 0, CRGB col = CRGB::Red, uint16_t shiftPixel = 0);
};

// Free function: applies FastLED fadeToBlackBy directly on a raw CRGB array.
void fastled_fadeToBlackBy(CRGB* leds, uint16_t num_leds, uint8_t fadeBy);

#endif
