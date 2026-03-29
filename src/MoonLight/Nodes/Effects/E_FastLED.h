/**
    @title     MoonLight
    @file      E_FastLED.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

class RainbowEffect : public Node {
 public:
  static const char* name() { return "Rainbow"; }
  static uint8_t dim() { return _1D; }
  static const char* tags() { return "⚡️"; }
  static const char* category() { return "FastLED"; }

  uint8_t speed = 8;  // default 8*32 = 256 / 256 = 1 = hue++
  uint8_t deltaHue = 7;
  uint8_t chanceOfGlitter = 80;
  bool usePalette = false;

  void setup() {
    addControl(speed, "speed", "slider", 0, 32);
    addControl(deltaHue, "deltaHue", "slider", 0, 32);
    addControl(chanceOfGlitter, "chanceOfGlitter", "slider", 0, 100);
    addControl(usePalette, "usePalette", "checkbox");
  }

  uint16_t hue = 0;

  void loop() override {
    if (usePalette) {
      uint8_t paletteIndex = hue >> 8;
      for (int i = 0; i < layer->nrOfLights; ++i) {
        layer->setRGB(i, ColorFromPalette(layerP.palette, paletteIndex));
        paletteIndex += deltaHue;
      }
    } else {
      layer->fill_rainbow(hue >> 8, deltaHue);  // hue back to uint8_t
    }
    hue += speed * 32;

    if (chanceOfGlitter && random8() < chanceOfGlitter) {
      layer->setRGB(random16(layer->nrOfLights), CRGB::White);
    }
  }
};

class FLAudioEffect : public Node {
 public:
  static const char* name() { return "FLAudio"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "⚡️♫"; }
  static const char* category() { return "FastLED"; }

  uint8_t fade = 70;

  void setup() { addControl(fade, "fade", "slider"); }

  uint8_t beatLevel = 0;

  void loop() override {
    layer->fadeToBlackBy(fade);

    if (sharedData.fl_beat) beatLevel = 255;

    // EXT_LOGD(ML_TAG, "%f %f %d %f", sharedData.fl_bassLevel, sharedData.fl_trebleLevel, sharedData.fl_beat, beatLevel, sharedData.fl_vocalsActive ? sharedData.fl_vocalConfidence : 0);

    uint8_t columnNr = 0;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.fl_bassLevel, CRGB::Red);
    columnNr++;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.fl_midLevel, CRGB::Orange);
    columnNr++;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.fl_trebleLevel, CRGB::Green);
    columnNr++;

    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.volume, CRGB::Yellow);
    columnNr++;
    // Normalize BPM to 0-1 range (assuming typical range 60-200 BPM)
    float normalizedBpm = constrain(sharedData.fl_bpm, 60.0f, 200.0f);
    normalizedBpm = (normalizedBpm - 60.0f) / 140.0f;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * normalizedBpm, CRGB::Yellow);
    columnNr++;

    // vocal
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.fl_vocalConfidence, CRGB::Purple);
    columnNr++;

    // beat
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * sharedData.fl_beatConfidence, CRGB::Blue);
    columnNr++;
    layer->drawLine(columnNr, layer->size.y - 1, columnNr, layer->size.y - 1 - layer->size.y * beatLevel / 255.0f, CRGB::Blue);
    columnNr++;
    if (sharedData.fl_beat) layer->setRGB(Coord3D(columnNr, layer->size.y - 1), CRGB::Blue);
    columnNr++;

    // percussion
    if (sharedData.fl_hihat) layer->setRGB(Coord3D(columnNr, layer->size.y - 1), CRGB::Cyan);
    columnNr++;
    if (sharedData.fl_kick) layer->setRGB(Coord3D(columnNr, layer->size.y - 1), CRGB::Cyan);
    columnNr++;
    if (sharedData.fl_snare) layer->setRGB(Coord3D(columnNr, layer->size.y - 1), CRGB::Cyan);
    columnNr++;
    if (sharedData.fl_tom) layer->setRGB(Coord3D(columnNr, layer->size.y - 1), CRGB::Cyan);

    // beat decay
    if (beatLevel && layer->size.y > 0) beatLevel -= MIN(255 / layer->size.y, beatLevel);
  }
};

  #include <fl/gfx/gfx.h>

  #include "fl/math/fixed_point/s16x16.h"

using coord = fl::s16x16;

static const coord kTwoPi = coord::from_raw(411774);  // 2π = 6.2831853
constexpr uint8_t kTrailMax = 80;

// by Sutaburosu
class FixedPointCanvasDemoEffect : public Node {
 public:
  static const char* name() { return "Fixed-Point Canvas Demo"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥🆕"; }
  static const char* category() { return "FastLED"; }

  uint8_t fade = 70;
  uint8_t effect = 0;

  void setup() override {
    addControl(fade, "fade", "slider");
    addControl(effect, "effect", "select");
    addControlValue("All");
    addControlValue("Clock");
    addControlValue("Orbiting Discs");
    addControlValue("Star Web");
    addControlValue("Spirograph");
    addControlValue("Lissajous");
    addControlValue("Cube Thin");
    addControlValue("Cube Thick");
    addControlValue("Organic Walkers");
    addControlValue("Boids");
    addControlValue("Hypotrochoid");
    addControlValue("Branching Tree");
  }

  CRGB* canvasBuf = nullptr;  // local buffer when needed, or points to channelsE
  size_t canvasBufSize = 0;
  bool canvasBufOwned = false; // true if we allocated canvasBuf (must free it)

  coord starSpin{};
  uint8_t dimAfterShow = 255;
  uint32_t sFrameCount = 0;
  uint8_t lastDemo = 255;

  // Returns true when canvas operations need a separate CRGB buffer
  // (non-RGB lights or virtual mapping means channelsE isn't a flat CRGB array)
  bool needsLocalBuf() { return !layer->oneToOneMapping || layerP.lights.header.channelsPerLight != 3; }

  void onSizeChanged(const Coord3D& prevSize) override {
    nrOfLights_t nrOfLights = layer->size.x * layer->size.y;
    if (needsLocalBuf()) {
      if (!canvasBufOwned) { canvasBuf = nullptr; canvasBufSize = 0; } // don't realloc the aliased channelsE pointer
      reallocMB2<CRGB>(canvasBuf, canvasBufSize, nrOfLights, "fpcdBuf");
      canvasBufOwned = true;
    } else {
      if (canvasBufOwned && canvasBuf) { freeMB(canvasBuf, "fpcdBuf"); }
      canvasBuf = (CRGB*)layerP.lights.channelsE;
      canvasBufSize = nrOfLights;
      canvasBufOwned = false;
    }
  }

  void loop() override {
    // Refresh buffer if uninitialized or if mapping/channel config changed without a size change
    if (!canvasBuf || canvasBufOwned != needsLocalBuf()) onSizeChanged(Coord3D());
    if (!canvasBuf) return;

    // Fade the local canvas buffer directly (works regardless of channelsPerLight)
    fastled_fadeToBlackBy(canvasBuf, canvasBufSize, fade);

    coord cx = coord::from_raw((int32_t)layer->size.x << 15);           // 16.0 px: matrix centre x
    coord cy = coord::from_raw((int32_t)layer->size.y << 15);           // 16.0 px: matrix centre y
    coord r = coord::from_raw((int32_t)(layer->size.x / 2 - 3) << 16);  // 13.0 px: radius
    uint32_t ms = fl::millis();
    // gLastLoopMs = ms;  // updated each frame; preserved in .noinit across a crash-reset
    ++sFrameCount;
    coord t = coord::from_raw((int32_t)((uint32_t)(ms % 6284u) * (uint32_t)kTwoPi.raw() / 6284u));
    // orbit the drawing origin to stress subpixel AA
    coord ocx = cx + fl::s16x16::sin(t) * fl::s16x16::from_raw(163840);  // *2.5 px
    coord ocy = cy + fl::s16x16::cos(t) * fl::s16x16::from_raw(163840);  // *2.5 px

    static constexpr uint32_t kSlotMs = 8192u;
    static constexpr uint32_t kTransMs = 768u;     // half-transition window (ms)
    static constexpr uint32_t kFadeMs = kTransMs;  // fade spans the full zoom window

    uint8_t demo;
    uint32_t msInSlot;
    coord tScale;
    uint8_t fadeBr;

    if (effect > 0) {
      // Fixed effect: no cycling, no transitions
      demo = effect - 1;
      tScale = coord::from_raw(65536);  // full scale
      fadeBr = 255u;
    } else {
      // Cycle through all demos
      demo = (ms / kSlotMs) % 11;
      msInSlot = ms % kSlotMs;

      // Half-cosine scale envelope: 1 during the body, tapers to 0 at slot edges.
      static const coord kPi = coord::from_raw(205887);  // π = 3.14159265
      if (msInSlot < kTransMs) {
        coord phase = coord::from_raw((int32_t)msInSlot * 65536 / (int32_t)kTransMs);
        tScale = (coord::from_raw(65536) - fl::s16x16::cos(kPi * phase)) / coord(2);  // 0 -> 1
      } else if (msInSlot >= kSlotMs - kTransMs) {
        coord phase = coord::from_raw((int32_t)(msInSlot - (kSlotMs - kTransMs)) * 65536 / (int32_t)kTransMs);
        tScale = (coord::from_raw(65536) + fl::s16x16::cos(kPi * phase)) / coord(2);  // 1 -> 0
      } else {
        tScale = coord::from_raw(65536);
      }

      // Brightness fade at slot boundaries
      if (msInSlot < kFadeMs) {
        fadeBr = (uint8_t)((uint32_t)msInSlot * 255u / kFadeMs);
      } else if (msInSlot >= kSlotMs - kFadeMs) {
        fadeBr = (uint8_t)((kSlotMs - msInSlot) * 255u / kFadeMs);
      } else {
        fadeBr = 255u;
      }
    }

    bool reinit = (demo != lastDemo);
    lastDemo = demo;

    // Scale radius; lerp orbiting origin toward display centre so transition stays centred.
    coord tr = r * tScale;
    coord tcx = cx + (ocx - cx) * tScale;
    coord tcy = cy + (ocy - cy) * tScale;

    // Apply previous frame's dim request on canvasBuf; always hard-clear on a demo transition.
    if (reinit || dimAfterShow == 255) {
      ::memset(canvasBuf, 0, canvasBufSize * sizeof(CRGB));
    } else if (dimAfterShow > 0) {
      fastled_fadeToBlackBy(canvasBuf, canvasBufSize, dimAfterShow);
    }

    uint8_t wantDim;
    if (demo == 0) {
      wantDim = demoClock(cx, cy, tr, ms);
    } else if (demo == 1) {
      wantDim = demoOrbitingDiscs(tcx, tcy, tr, tScale);
    } else if (demo == 2) {
      wantDim = demoStarWeb(cx, cy, tr, starSpin, tScale);
    } else if (demo == 3) {
      wantDim = demoSpirograph(cx, cy, r, tScale, reinit);  // full r: geometry must never shrink below kInnerR
    } else if (demo == 4) {
      wantDim = demoLissajous(cx, cy, r, tScale, reinit);  // full r: matches spirograph; trail scaled at render
    } else if (demo == 5) {
      wantDim = demoCubeImpl(cx, cy, true, tScale);  // thin Wu-AA lines
    } else if (demo == 6) {
      wantDim = demoCubeImpl(cx, cy, false, tScale);  // depth-cued thick lines
    } else if (demo == 7) {
      wantDim = demoOrganicWalkers(cx, cy, tScale, reinit);  // fading walker trails
    } else if (demo == 8) {
      wantDim = demoBoids(cx, cy, r, tScale, reinit);  // boids flocking
    } else if (demo == 9) {
      wantDim = demoHypoRand(cx, cy, reinit);  // cumulative random hypotrochoid
    } else {
      wantDim = demoBranchingTree(cx, cy, r, ms);  // fractal wind-swaying tree
    }
    // Apply brightness envelope at slot transitions (replaces original FastLED.setBrightness(fadeBr)).
    // fadeBr < 255 only during the first/last kFadeMs of each slot, creating a fade-to-black transition.
    if (fadeBr < 255) fastled_fadeToBlackBy(canvasBuf, canvasBufSize, 255 - fadeBr);

    dimAfterShow = wantDim;

    starSpin = starSpin + coord::from_raw(1966);  // +0.03 rad/frame
    if (starSpin.raw() > kTwoPi.raw()) starSpin = starSpin - kTwoPi;

    // Copy canvasBuf to channelsE via setRGB when using local buffer
    if (canvasBufOwned) {
      nrOfLights_t nrOfLights = layer->size.x * layer->size.y;
      for (nrOfLights_t i = 0; i < nrOfLights && i < canvasBufSize; i++) {
        layer->setRGB(i, canvasBuf[i]);
      }
    }
  }

  // Shared trail ring-buffer reused by demoSpirograph (60 pts) and demoLissajous (80 pts).
  // Sized for the larger consumer; the two demos are never active on the same frame.
  coord gTrailX[kTrailMax], gTrailY[kTrailMax];
  uint8_t gTrailHead = 0;

  // Helper: render a rainbow-gradient trail connecting points in the shared trail buffer.
  // Saturation differs between spirograph (220) and lissajous (230) for color tuning.
  void renderRainbowTrail(const coord& cx, const coord& cy, coord tScale, uint8_t numPts, uint8_t saturation) {
    fl::CanvasRGB canvas(fl::span<CRGB>(canvasBuf, layer->size.x * layer->size.y), layer->size.x, layer->size.y);

    for (uint8_t i = 0; i < numPts - 1; ++i) {
      uint8_t a = (gTrailHead + i) % numPts;
      uint8_t b = (gTrailHead + i + 1) % numPts;
      uint8_t br = (uint8_t)((uint16_t)i * 255u / (numPts - 1));
      coord ax = cx + (gTrailX[a] - cx) * tScale, ay = cy + (gTrailY[a] - cy) * tScale;
      coord bx = cx + (gTrailX[b] - cx) * tScale, by = cy + (gTrailY[b] - cy) * tScale;
      // drawLine(ax, ay, bx, by, CHSV(br, saturation, br));
      canvas.drawLine(CHSV(br, saturation, br), ax, ay, bx, by);
    }
  }

  // ── clock face with three rotating hands ──────────────────────────
  // Hour, minute and second hands rotate in the real 1:12:144 ratio, accelerated
  // so one "second" completes in 6 s of wall time: second every 6 s, minute every
  // 72 s, hour every 864 s.  Tick marks at all 12 hour positions; heavier marks
  // at 12, 3, 6, 9.  Angles start at 12 o’clock (top) and run clockwise.
  uint8_t demoClock(coord cx, coord cy, coord r, uint32_t ms) {
    static constexpr uint32_t kSecPeriod = 6000u;
    static constexpr uint32_t kMinPeriod = 72000u;
    static constexpr uint32_t kHrPeriod = 864000u;
    static const coord kOffset = coord::from_raw(102943);  // π/2 = 1.5707963, East→North offset

    // angle = (ms % period) * 2π / period — pure integer arithmetic
    coord secA = coord::from_raw((uint32_t)(ms % kSecPeriod) * (uint32_t)kTwoPi.raw() / kSecPeriod) - kOffset;
    coord minA = coord::from_raw((int32_t)((uint64_t)(ms % kMinPeriod) * (uint32_t)kTwoPi.raw() / kMinPeriod)) - kOffset;
    coord hrA = coord::from_raw((int32_t)((uint64_t)(ms % kHrPeriod) * (uint32_t)kTwoPi.raw() / kHrPeriod)) - kOffset;

    // Zoom: slow sine (period 20 s). When positive, zoom in on the second-hand tip.
    coord zPhase = coord::from_raw((int32_t)((uint64_t)(ms % 20000u) * (uint32_t)kTwoPi.raw() / 20000u));
    coord zSin = fl::s16x16::sin(zPhase);
    // zoom ∈ [1.0, 2.5]: 1 + max(0, zSin) * 1.5
    coord zoom = coord::from_raw(65536) + (zSin.raw() > 0 ? zSin * coord::from_raw(98304) : coord{});  // 1.0 + max(0,zSin)*1.5

    // Second hand trig (needed for both tip tracking and drawing)
    coord sc = fl::s16x16::cos(secA), ss = fl::s16x16::sin(secA);
    coord tipX = cx + sc * r;
    coord tipY = cy + ss * r;

    // Camera blends linearly from clock centre (zoom=1) to tip (zoom=max).
    coord zoomFrac = (zoom - coord::from_raw(65536)) / coord::from_raw(98304);  // (zoom-1)/1.5
    coord camX = cx + (tipX - cx) * zoomFrac;
    coord camY = cy + (tipY - cy) * zoomFrac;

    // screen = displayCentre + (world - cam) * zoom
    coord eff_cx = cx + (cx - camX) * zoom;
    coord eff_cy = cy + (cy - camY) * zoom;
    coord eff_r = r * zoom;
    coord eff_zoom = zoom;

    fl::CanvasRGB canvas(fl::span<CRGB>(canvasBuf, layer->size.x * layer->size.y), layer->size.x, layer->size.y);

    // Face ring
    canvas.drawRing(CRGB(40, 40, 40), eff_cx, eff_cy, eff_r, eff_zoom);  // thickness = 1.0 * eff_zoom

    // 12 tick marks; major ticks (every 3rd) are longer and brighter
    for (uint8_t i = 0; i < 12; ++i) {
      coord ta = coord::from_raw((int32_t)i * (kTwoPi.raw() / 12)) - kOffset;
      coord tc = fl::s16x16::cos(ta), ts = fl::s16x16::sin(ta);
      bool major = (i % 3 == 0);
      coord inner = eff_r * coord::from_raw(major ? 47185 : 53739);  // *0.72 or *0.82
      canvas.drawLine(major ? CRGB(200, 200, 200) : CRGB(80, 80, 80), eff_cx + tc * inner, eff_cy + ts * inner, eff_cx + tc * eff_r, eff_cy + ts * eff_r);
    }

    // Hour hand: thick and short
    {
      coord hc = fl::s16x16::cos(hrA), hs = fl::s16x16::sin(hrA);
      coord hLen = eff_r * coord::from_raw(36044);                                                                                             // *0.55
      canvas.drawStrokeLine(CRGB(200, 200, 200), eff_cx, eff_cy, eff_cx + hc * hLen, eff_cy + hs * hLen, coord::from_raw(163840) * eff_zoom);  // thick=2.5
    }

    // Minute hand: medium thickness, nearly full length
    {
      coord mc = fl::s16x16::cos(minA), ms_v = fl::s16x16::sin(minA);
      coord mLen = eff_r * coord::from_raw(52428);                                                                                      // *0.80
      canvas.drawStrokeLine(CRGB::White, eff_cx, eff_cy, eff_cx + mc * mLen, eff_cy + ms_v * mLen, coord::from_raw(98304) * eff_zoom);  // thick=1.5
    }

    // Second hand: thin red line + short counterbalance tail
    {
      coord tail = eff_r * coord::from_raw(16384);  // *0.25 (exact)
      canvas.drawLine(CRGB(255, 40, 40), eff_cx - sc * tail, eff_cy - ss * tail, eff_cx + sc * eff_r, eff_cy + ss * eff_r);
    }

    // Centre cap
    canvas.drawDisc(CRGB(220, 220, 220), eff_cx, eff_cy, coord::from_raw(98304) * eff_zoom);  // r=1.5
    return 255;
  }

  // ── nested rings, each internally tangent to its parent ──────────────
  // The chain is:
  //   outer ring (fixed, radius R at display centre)
  //     └─ ring[0] orbits inside R,        outer edge touches R's inner edge
  //          └─ ring[1] orbits inside [0], outer edge touches [0]'s inner edge
  //               └─ ring[2] orbits inside [1], outer edge touches [1]'s inner edge
  //                    └─ ring[3] orbits inside [2], outer edge touches [2]'s inner edge
  //
  // Each ring has inner radius r_i and thickness t.  Its outer edge is at r_i + t.
  // For the child's outer edge to sit exactly on the parent's inner edge:
  //   orbit = pr - ri - t
  // Radii: 13, 9.5, 6, 3.5, 1.25  →  orbit radii: 2.25, 2.25, 1.25, 0.98  with t=1.25.
  // Each ring is drawn in a rainbow colour; no filled discs.
  coord angles[4] = {};

  uint8_t demoOrbitingDiscs(coord cx, coord cy, coord R, coord tScale) {
    static const CRGB kColors[] = {
        CRGB::Red, CRGB::Orange, CRGB::Yellow, CRGB::Green, CRGB::Blue,
    };
    struct Level {
      coord r;
      coord speed;
    };
    static const Level levels[] = {
        {coord::from_raw(622592), coord::from_raw(65536)},   //  9.5, 1.00
        {coord::from_raw(393216), coord::from_raw(111411)},  //  6.0, 1.70
        {coord::from_raw(229376), coord::from_raw(190054)},  //  3.5, 2.90
        {coord::from_raw(81920), coord::from_raw(334233)},   //  1.25, 5.10
    };
    static const coord kRingThickness = coord::from_raw(81920);  // 1.25 px

    // Each ring gets its own angle that advances by baseDelta*speed per frame
    // and wraps independently — avoids the phase jump that occurs when a shared
    // spin counter is multiplied by a non-integer speed then wrapped to [0,2π).
    static const coord kBaseDelta = coord::from_raw(2621);  // 0.04 rad/frame

    fl::CanvasRGB canvas(fl::span<CRGB>(canvasBuf, layer->size.x * layer->size.y), layer->size.x, layer->size.y);

    // Scale all ring radii and thickness proportionally with tScale so the
    // chain geometry stays consistent as the scene zooms in/out.
    coord thick = kRingThickness * tScale;
    canvas.drawRing(kColors[0], cx, cy, R, thick);

    // Walk down the chain; each iteration becomes the new parent.
    coord pcx = cx, pcy = cy, pr = R;
    for (uint8_t i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i) {
      coord ri = levels[i].r * tScale;
      // orbit places child outer edge (ri + t) tangent to parent inner edge (pr)
      coord orbit = pr - ri - thick;
      coord ncx = pcx + fl::s16x16::cos(angles[i]) * orbit;
      coord ncy = pcy + fl::s16x16::sin(angles[i]) * orbit;
      canvas.drawRing(kColors[i + 1], ncx, ncy, ri, thick);
      // Advance and wrap this ring's angle independently
      angles[i] = angles[i] + kBaseDelta * levels[i].speed;
      if (angles[i].raw() > kTwoPi.raw()) angles[i] = angles[i] - kTwoPi;
      pcx = ncx;
      pcy = ncy;
      pr = ri;  // this ring becomes the next parent
    }
    return 255;
  }

  // ── rotating star polygon ───────────────────────────────────────────
  // N points equally spaced on a circle just inside a bounding ring, connected
  // by thick lines to the point K steps ahead — forming a {N/K} star polygon.
  // A small disc marks each vertex.  The star rotates and the line thickness
  // pulses sinusoidally.  Two concentric static rings (inner and outer) frame the star.
  uint8_t demoStarWeb(coord cx, coord cy, coord R, coord spin, coord tScale) {
    static constexpr uint8_t N = 5;  // pentagon vertices
    static constexpr uint8_t K = 2;  // connect to 2nd neighbour -> pentagram

    // Inner accent ring at the natural inner-pentagon radius (golden ratio ≈ 0.382 R).
    coord innerR = R * coord::from_raw(25034);  // R * 0.382 (inner pentagon radius)

    fl::CanvasRGB canvas(fl::span<CRGB>(canvasBuf, layer->size.x * layer->size.y), layer->size.x, layer->size.y);

    canvas.drawRing(CRGB::Blue, cx, cy, R, coord::from_raw(131072) * tScale);  // thick=2.0
    canvas.drawRing(CRGB(0, 60, 120), cx, cy, innerR, tScale);

    // Vertex circle sits on the outer ring so the star fills the display.
    coord pr = R;

    // Compute N vertex positions
    coord px[N], py[N];
    for (uint8_t i = 0; i < N; ++i) {
      coord a = spin + coord::from_raw((int32_t)i * (kTwoPi.raw() / N));
      px[i] = cx + fl::s16x16::cos(a) * pr;
      py[i] = cy + fl::s16x16::sin(a) * pr;
    }

    // Pulsing line thickness, scaled with tScale
    coord thick = (coord::from_raw(81920) + fl::s16x16::sin(spin * coord::from_raw(196608)) * coord::from_raw(49152)) * tScale;  // 1.25 + sin(3θ)*0.75

    // Draw the star edges in warm white, then thin spokes from centre in dim white
    for (uint8_t i = 0; i < N; ++i) {
      uint8_t j = (i + K) % N;
      canvas.drawStrokeLine(CRGB(220, 220, 180), px[i], py[i], px[j], py[j], thick);
      canvas.drawLine(CRGB(60, 60, 60), cx, cy, px[i], py[i]);
    }

    // Vertex discs and centre disc, scaled with tScale
    for (uint8_t i = 0; i < N; ++i) canvas.drawDisc(CRGB::Yellow, px[i], py[i], coord::from_raw(98304) * tScale);  // r=1.5
    canvas.drawDisc(CRGB::Orange, cx, cy, coord::from_raw(131072) * tScale);                                       // r=2.0
    return 255;
  }

  // ── spirograph (hypotrochoid) with rainbow trail ─────────────────────
  // An inner circle (radius r) rolls inside the outer track (radius R).
  // A pen arm of length h traces a hypotrochoid.  The last kPts positions are
  // stored in a ring buffer and drawn as connected AA line segments with a
  // rainbow gradient (oldest=dim, newest=bright).
  //
  // Parameters: R=13, r=6, h=5  →  kRatio fixed at 3/2=1.5 for a clean 3-lobed curve
  //             closes after 2 full outer rotations (theta=4π)
  //             max pen radius = (R-r)+h = 7+5 = 12 px  (fits the display)
  coord spiroTheta{};

  uint8_t demoSpirograph(coord cx, coord cy, coord R, coord tScale, bool reinit) {
    // static constexpr float   kInnerR = 6.0f;   // rolling circle radius
    // static constexpr float   kPenArm = 5.0f;   // pen arm length
    // static constexpr float   kRatio  = 1.5f;  // fixed 3/2 ratio → 3-lobed curve (independent of R)
    static constexpr uint8_t kPts = 60;                  // ring-buffer trail length
    static const coord kDelta = coord::from_raw(14417);  // 0.22 rad/step
    // The 3/2-ratio hypotrochoid closes after 2 full outer rotations (theta=4π),
    // so wrap at 4π, not 2π, to avoid a mid-curve position discontinuity.
    static const coord fourPi = coord::from_raw(823548);  // 4π = 12.5663706

    // Rolling-circle centre
    coord arm = R - coord::from_raw(393216);  // R - kInnerR(6.0)
    coord rcx = cx + fl::s16x16::cos(spiroTheta) * arm;
    coord rcy = cy + fl::s16x16::sin(spiroTheta) * arm;
    // Pen position: roll angle = theta * ratio; minus sign on sin gives correct
    // rotation direction for a rolling (not sliding) inner circle.
    coord roll = spiroTheta * coord::from_raw(98304);                    // kRatio = 1.5
    coord penX = rcx + fl::s16x16::cos(roll) * coord::from_raw(327680);  // kPenArm = 5.0
    coord penY = rcy - fl::s16x16::sin(roll) * coord::from_raw(327680);  // kPenArm = 5.0

    // On (re-)activation pre-simulate kPts steps so the trail is fully populated.
    if (reinit) {
      gTrailHead = 0;
      coord t = spiroTheta - kDelta * coord(kPts);
      while (t.raw() < 0) t = t + fourPi;
      for (uint8_t i = 0; i < kPts; ++i) {
        coord pa = R - coord::from_raw(393216);  // R - kInnerR(6.0)
        coord prx = cx + fl::s16x16::cos(t) * pa;
        coord pry = cy + fl::s16x16::sin(t) * pa;
        coord pro = t * coord::from_raw(98304);                                      // kRatio = 1.5
        gTrailX[gTrailHead] = prx + fl::s16x16::cos(pro) * coord::from_raw(327680);  // kPenArm=5.0
        gTrailY[gTrailHead] = pry - fl::s16x16::sin(pro) * coord::from_raw(327680);  // kPenArm=5.0
        gTrailHead = (gTrailHead + 1) % kPts;
        t = t + kDelta;
        if (t.raw() > fourPi.raw()) t = t - fourPi;
      }
    }
    gTrailX[gTrailHead] = penX;
    gTrailY[gTrailHead] = penY;
    gTrailHead = (gTrailHead + 1) % kPts;

    // Scale all geometric elements toward the display centre during transitions.
    // Geometry runs at full scale so arm = R - kInnerR is always positive.
    coord s_rcx = cx + (rcx - cx) * tScale;
    coord s_rcy = cy + (rcy - cy) * tScale;
    coord s_penX = cx + (penX - cx) * tScale;
    coord s_penY = cy + (penY - cy) * tScale;

    fl::CanvasRGB canvas(fl::span<CRGB>(canvasBuf, layer->size.x * layer->size.y), layer->size.x, layer->size.y);

    canvas.drawRing(CRGB(0, 0, 180), cx, cy, R * tScale, coord::from_raw(98304) * tScale);       // thick=1.5
    canvas.drawRing(CRGB(0, 100, 140), s_rcx, s_rcy, coord::from_raw(393216) * tScale, tScale);  // r=6.0
    canvas.drawLine(CRGB(50, 50, 50), s_rcx, s_rcy, s_penX, s_penY);

    // Rainbow trail: stored positions are in full-scale world space; scale at render time.
    renderRainbowTrail(cx, cy, tScale, kPts, 220);

    // Pen dot at scaled position
    canvas.drawDisc(CRGB::White, s_penX, s_penY, coord::from_raw(98304) * tScale);  // r=1.5

    spiroTheta = spiroTheta + kDelta;
    if (spiroTheta.raw() > fourPi.raw()) spiroTheta = spiroTheta - fourPi;
    return 255;
  }

  // ── morphing Lissajous figure with rainbow trail ─────────────────────
  // x = R·sin(3θ+φ),  y = R·sin(2θ)
  // φ advances slowly each frame, continuously morphing the knot through all
  // Bowditch forms.  θ wraps at 2π (the 3:2 figure closes at that period).
  // An 80-point ring buffer stores the trail; drawn as fading rainbow AA lines.
  coord lissaTheta{};
  coord lissaPhase{};

  uint8_t demoLissajous(coord cx, coord cy, coord R, coord tScale, bool reinit) {
    static constexpr uint8_t kPts = 80;
    static const coord kStep = coord::from_raw(7864);  // 0.12 rad/frame
    static const coord twoPi = kTwoPi;

    // lissaPhase is a class member: φ drifts to morph the figure

    // Current pen position
    coord px = cx + fl::s16x16::sin(lissaTheta * coord::from_raw(196608) + lissaPhase) * R;  // sin(3θ+ϕ)
    coord py = cy + fl::s16x16::sin(lissaTheta * coord::from_raw(131072)) * R;               // sin(2θ)

    // On (re-)activation pre-simulate kPts steps so the trail is fully populated.
    if (reinit) {
      gTrailHead = 0;
      coord t = lissaTheta - kStep * coord(kPts);
      coord p = lissaPhase - coord(0.007f * kPts);
      while (t.raw() < 0) t = t + twoPi;
      while (p.raw() < 0) p = p + twoPi;
      for (uint8_t i = 0; i < kPts; ++i) {
        gTrailX[gTrailHead] = cx + fl::s16x16::sin(t * coord::from_raw(196608) + p) * R;  // sin(3t+p)
        gTrailY[gTrailHead] = cy + fl::s16x16::sin(t * coord::from_raw(131072)) * R;      // sin(2t)
        gTrailHead = (gTrailHead + 1) % kPts;
        t = t + kStep;
        if (t.raw() > twoPi.raw()) t = t - twoPi;
        p = p + coord::from_raw(458);  // +0.007 rad
        if (p.raw() > twoPi.raw()) p = p - twoPi;
      }
    }
    gTrailX[gTrailHead] = px;
    gTrailY[gTrailHead] = py;
    gTrailHead = (gTrailHead + 1) % kPts;

    fl::CanvasRGB canvas(fl::span<CRGB>(canvasBuf, layer->size.x * layer->size.y), layer->size.x, layer->size.y);

    // Trail: oldest → dim, newest → bright, full rainbow.
    // Scale stored positions toward cx/cy by tScale to match the transition zoom.
    renderRainbowTrail(cx, cy, tScale, kPts, 230);

    // Leading dot — scaled toward display centre like the trail.
    coord s_px = cx + (px - cx) * tScale;
    coord s_py = cy + (py - cy) * tScale;
    canvas.drawDisc(CRGB::White, s_px, s_py, coord{1.5f} * tScale);

    lissaTheta = lissaTheta + kStep;
    if (lissaTheta.raw() > twoPi.raw()) lissaTheta = lissaTheta - twoPi;

    lissaPhase = lissaPhase + coord(0.007f);
    if (lissaPhase.raw() > twoPi.raw()) lissaPhase = lissaPhase - twoPi;
    return 255;
  }

  // ── spinning wireframe cube (thin and thick lines) ──────────────────────────────────
  // 8 vertices (±kS, ±kS, ±kS) rotated around Y (fast) and X (slow) axes with
  // perspective division.  Both demos share a single rotation state so the
  // transition from one to the other feels continuous.
  //   thin=true:  drawLine — 1-px Wu AA; depth cued by brightness only.
  //   thin=false: canvas.drawStrokeLine — depth cued by both thickness and brightness.
  coord spinY{};
  coord spinX{};

  uint8_t demoCubeImpl(coord cx, coord cy, bool thin, coord tScale) {
    static const coord kS = coord::from_raw(458752);         // 7.0
    static const coord kSThin = coord::from_raw(720896);     // 11.0
    static const coord kEyeDist = coord::from_raw(1703936);  // 26.0
    const coord kSeff = thin ? kSThin : kS;

    static const int8_t kVerts[8][3] = {
        {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},  // back face  (z = -1)
        {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1},   // front face (z = +1)
    };
    static const uint8_t kEdges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},  // back face
        {4, 5}, {5, 6}, {6, 7}, {7, 4},  // front face
        {0, 4}, {1, 5}, {2, 6}, {3, 7},  // pillars
    };

    const coord cosY = fl::s16x16::cos(spinY);
    const coord sinY = fl::s16x16::sin(spinY);
    const coord cosX = fl::s16x16::cos(spinX);
    const coord sinX = fl::s16x16::sin(spinX);

    coord px[8], py[8], pz[8];
    for (uint8_t i = 0; i < 8; ++i) {
      coord x{kVerts[i][0] * kSeff};
      coord y{kVerts[i][1] * kSeff};
      coord z{kVerts[i][2] * kSeff};

      coord x2 = x * cosY - z * sinY;
      coord z2 = x * sinY + z * cosY;
      coord y3 = y * cosX - z2 * sinX;
      coord z3 = y * sinX + z2 * cosX;

      coord denom = kEyeDist - z3;
      if (denom.raw() < 65536) denom = coord::from_raw(65536);  // clamp < 1.0 → 1.0
      coord scale = kEyeDist / denom;
      // Apply transition scale: zoom projected position toward display centre.
      px[i] = cx + x2 * scale * tScale;
      py[i] = cy + y3 * scale * tScale;
      pz[i] = z3;  // positive = towards viewer
    }

    fl::CanvasRGB canvas(fl::span<CRGB>(canvasBuf, layer->size.x * layer->size.y), layer->size.x, layer->size.y);

    for (uint8_t e = 0; e < 12; ++e) {
      const uint8_t a = kEdges[e][0], b = kEdges[e][1];
      coord avgZ = (pz[a] + pz[b]) / coord(2);
      // avgZ in [-kS, +kS] → brightness in [100, 255]: near = bright, far = dim.
      // Clamp before uint8_t cast: post-rotation z can exceed kS (up to kS·√2).
      int br_i = 178 + avgZ.to_int() * 77 / kSeff.to_int();
      if (br_i > 255) br_i = 255;
      if (br_i < 0) br_i = 0;
      CRGB col((uint8_t)br_i, (uint8_t)br_i, (uint8_t)br_i);
      if (thin) {
        canvas.drawLine(col, px[a], py[a], px[b], py[b]);
      } else {
        coord thick = (coord::from_raw(117964) + avgZ * (coord::from_raw(65536) / kSeff)) * tScale;  // (1.8 + z/kSeff)
        canvas.drawStrokeLine(col, px[a], py[a], px[b], py[b], thick);
      }
    }

    spinY = spinY + coord::from_raw(6553);  // +0.10 rad/frame
    if (spinY.raw() > kTwoPi.raw()) spinY = spinY - kTwoPi;
    spinX = spinX + coord::from_raw(2490);  // +0.038 rad/frame
    if (spinX.raw() > kTwoPi.raw()) spinX = spinX - kTwoPi;
    return 255;
  }

  // ── organic random walkers with fading trails ───────────────────────────────
  // Six walkers undergo damped random walks with a soft spring pulling them back
  // toward the display centre, so they wander freely without flying off-screen.
  // Returns 20 so the caller dims the framebuffer slightly each frame, leaving
  // long glowing colour trails that shift hue slowly over time.
  coord wx[6] = {};
  coord wy[6] = {};
  coord wvx[6] = {};
  coord wvy[6] = {};
  uint8_t whue[6] = {};

  uint8_t demoOrganicWalkers(coord cx, coord cy, coord tScale, bool reinit) {
    static constexpr uint8_t kN = 6;

    if (reinit) {
      for (uint8_t i = 0; i < kN; ++i) {
        wx[i] = cx;
        wy[i] = cy;
        wvx[i] = coord{};
        wvy[i] = coord{};
        whue[i] = (uint8_t)(i * 43u);  // spread hues ~60° apart
      }
    }

    // Motion constants (fixed-point)
    // Equilibrium spread σ ≈ kJitter / sqrt(2·kSpring·(1−kDamp)) ≈ 9 px.
    static const coord kDamp = coord::from_raw(61440);    // ≈ 0.9375  velocity damping per frame
    static const coord kJitter = coord::from_raw(16384);  // ≈ 0.250 px/frame² random kick scale
    static const coord kSpring = coord::from_raw(393);    // ≈ 0.006   spring pull toward centre
    static const coord kMaxV = coord::from_raw(196608);   //   3.0 px/frame velocity cap

    fl::CanvasRGB canvas(fl::span<CRGB>(canvasBuf, layer->size.x * layer->size.y), layer->size.x, layer->size.y);

    for (uint8_t i = 0; i < kN; ++i) {
      // Random acceleration + weak spring toward display centre
      coord ax = coord::from_raw(((int32_t)(int8_t)random8()) * kJitter.raw() / 128) + (cx - wx[i]) * kSpring;
      coord ay = coord::from_raw(((int32_t)(int8_t)random8()) * kJitter.raw() / 128) + (cy - wy[i]) * kSpring;

      wvx[i] = wvx[i] * kDamp + ax;
      wvy[i] = wvy[i] * kDamp + ay;

      // Clamp each velocity component independently
      if (wvx[i].raw() > kMaxV.raw()) wvx[i] = kMaxV;
      if (wvx[i].raw() < -kMaxV.raw()) wvx[i] = coord::from_raw(-kMaxV.raw());
      if (wvy[i].raw() > kMaxV.raw()) wvy[i] = kMaxV;
      if (wvy[i].raw() < -kMaxV.raw()) wvy[i] = coord::from_raw(-kMaxV.raw());

      wx[i] = wx[i] + wvx[i];
      wy[i] = wy[i] + wvy[i];

      whue[i] += 1;  // slowly drift hue each frame

      // Scale position toward display centre during slot transitions
      coord sx = cx + (wx[i] - cx) * tScale;
      coord sy = cy + (wy[i] - cy) * tScale;
      canvas.drawDisc(CHSV(whue[i], 230, 255), sx, sy, coord::from_raw(131072) * tScale);  // r=2.0
    }
    return 20;  // caller applies fadeToBlackBy(20) → trails persist ~20 frames
  }

  // ── boids flocking simulation with fading trails ────────────────────────────
  // N boids follow the classic three rules: separation, alignment, cohesion,
  // plus a soft wall force keeping them inside the display area.  Each boid is
  // drawn as a short velocity-aligned stroke.  Returns 18 so the caller dims
  // the framebuffer each frame and trails persist for roughly 20 frames.
  coord bx[7] = {};
  coord by[7] = {};
  coord bvx[7] = {};
  coord bvy[7] = {};
  uint8_t bhue[7] = {};

  uint8_t demoBoids(coord cx, coord cy, coord r, coord tScale, bool reinit) {
    static constexpr uint8_t kN = 7;

    if (reinit) {
      for (uint8_t i = 0; i < kN; ++i) {
        bx[i] = cx + coord::from_raw(((int32_t)(int8_t)random8()) * 5120);  // ±10 px
        by[i] = cy + coord::from_raw(((int32_t)(int8_t)random8()) * 5120);
        bvx[i] = coord::from_raw(((int32_t)(int8_t)random8()) * 512);  // ±1.0 px/fr
        bvy[i] = coord::from_raw(((int32_t)(int8_t)random8()) * 512);
        bhue[i] = (uint8_t)(i * 36u);  // spread across hue wheel
      }
    }

    // Neighbour radii via L∞ (Chebyshev) distance — avoids overflow when
    // squaring large s16.16 position values.
    static const coord kSepR = coord::from_raw(3 * 65536);  // separation zone: 3 px
    static const coord kNbrR = coord::from_raw(8 * 65536);  // alignment / cohesion zone: 8 px
    // Steering weights
    static const coord kSepW = coord::from_raw(8192);  // ≈ 0.125
    static const coord kAliW = coord::from_raw(4915);  // ≈ 0.075
    static const coord kCohW = coord::from_raw(1311);  // ≈ 0.020
    // Wall: proportional repulsion — force scales with depth inside the margin zone
    static const coord kMargin = coord::from_raw(6 * 65536);  // 6 px warning zone
    static const coord kWallScale = coord::from_raw(5243);    // ≈ 0.080 force/px penetration
    // Per-component speed cap and velocity damping
    static const coord kMaxV = coord::from_raw(131072);  // 2.0 px/frame
    static const coord kDamp = coord::from_raw(63897);   // ≈ 0.975

    fl::CanvasRGB canvas(fl::span<CRGB>(canvasBuf, layer->size.x * layer->size.y), layer->size.x, layer->size.y);

    for (uint8_t i = 0; i < kN; ++i) {
      coord ax{}, ay{};

      // ── Soft boundary repulsion ──────────────────────────────────────────
      {
        coord left = bx[i] - (cx - r);
        coord right = (cx + r) - bx[i];
        coord top = by[i] - (cy - r);
        coord bot = (cy + r) - by[i];
        if (left < kMargin) ax = ax + (kMargin - left) * kWallScale;
        if (right < kMargin) ax = ax - (kMargin - right) * kWallScale;
        if (top < kMargin) ay = ay + (kMargin - top) * kWallScale;
        if (bot < kMargin) ay = ay - (kMargin - bot) * kWallScale;
      }

      // ── Boid rules (neighbour scan) ──────────────────────────────────────
      coord sepX{}, sepY{}, aliX{}, aliY{}, cohX{}, cohY{};
      uint8_t nbrCnt = 0;

      for (uint8_t j = 0; j < kN; ++j) {
        if (j == i) continue;
        coord dx = bx[j] - bx[i];
        coord dy = by[j] - by[i];
        int32_t adx = dx.raw() < 0 ? -dx.raw() : dx.raw();
        int32_t ady = dy.raw() < 0 ? -dy.raw() : dy.raw();
        if (adx < kSepR.raw() && ady < kSepR.raw()) {
          sepX = sepX - dx;
          sepY = sepY - dy;
        }
        if (adx < kNbrR.raw() && ady < kNbrR.raw()) {
          aliX = aliX + bvx[j];
          aliY = aliY + bvy[j];
          cohX = cohX + bx[j];
          cohY = cohY + by[j];
          nbrCnt++;
        }
      }

      ax = ax + sepX * kSepW;
      ay = ay + sepY * kSepW;
      if (nbrCnt > 0) {
        coord n = coord::from_raw((int32_t)nbrCnt << 16);
        ax = ax + (aliX / n - bvx[i]) * kAliW + (cohX / n - bx[i]) * kCohW;
        ay = ay + (aliY / n - bvy[i]) * kAliW + (cohY / n - by[i]) * kCohW;
      }

      // ── Integrate ────────────────────────────────────────────────────────
      bvx[i] = (bvx[i] + ax) * kDamp;
      bvy[i] = (bvy[i] + ay) * kDamp;
      if (bvx[i].raw() > kMaxV.raw()) bvx[i] = kMaxV;
      if (bvx[i].raw() < -kMaxV.raw()) bvx[i] = coord::from_raw(-kMaxV.raw());
      if (bvy[i].raw() > kMaxV.raw()) bvy[i] = kMaxV;
      if (bvy[i].raw() < -kMaxV.raw()) bvy[i] = coord::from_raw(-kMaxV.raw());
      bx[i] = bx[i] + bvx[i];
      by[i] = by[i] + bvy[i];
      // Hard clamp — ensures boids never escape the display area
      if (bx[i] < cx - r) {
        bx[i] = cx - r;
        if (bvx[i].raw() < 0) bvx[i] = coord{};
      }
      if (bx[i] > cx + r) {
        bx[i] = cx + r;
        if (bvx[i].raw() > 0) bvx[i] = coord{};
      }
      if (by[i] < cy - r) {
        by[i] = cy - r;
        if (bvy[i].raw() < 0) bvy[i] = coord{};
      }
      if (by[i] > cy + r) {
        by[i] = cy + r;
        if (bvy[i].raw() > 0) bvy[i] = coord{};
      }
      bhue[i] += 1;

      // ── Draw ─────────────────────────────────────────────────────────────
      coord hx = cx + (bx[i] - cx) * tScale;
      coord hy = cy + (by[i] - cy) * tScale;
      coord tx = hx - bvx[i] * coord(3) * tScale;  // tail: 3 velocity steps back
      coord ty = hy - bvy[i] * coord(3) * tScale;
      canvas.drawLine(CHSV(bhue[i], 220, 160), tx, ty, hx, hy);
      canvas.drawDisc(CHSV(bhue[i] + 20, 200, 160), hx, hy, coord::from_raw(65536) * tScale);  // r=1.0 head highlight
    }
    return 40;  // caller applies fadeToBlackBy(40) → trails persist ~8 frames
  }

  // ── random hypotrochoid, drawn cumulatively ─────────────────────────────────
  // Each cycle picks a random (r, h) pair from a curated table and draws the
  // complete hypotrochoid one AA segment per frame with no dimming.  When the
  // curve closes the display is cleared and a fresh random variant begins.
  // Closing angle = 2π × r/gcd(r, R−r); all entries use R=13 (prime) so gcd=1
  // for every row, giving clean petal counts with 3–9 rotations each.
  // Returns 0 while drawing (no fade), 255 on completion (clear next frame).
  uint8_t idx = 0;
  coord hypoTheta{};
  coord rollPhase{};
  coord prevX{};
  coord prevY{};
  bool firstPoint = true;
  uint8_t hue = 0;
  int32_t closeRaw = 0;

  uint8_t demoHypoRand(coord cx, coord cy, bool reinit) {
    struct Params {
      uint8_t r;
      uint8_t h;
    };
    // R=13. max pen radius = (R−r)+h.  Closing angle = 2π × r (since gcd=1 always).
    static const Params kTable[] = {
        {5, 8},  //  5 rot, max_r=16, dramatic 5-petal with inner loops
        {4, 8},  //  4 rot, max_r=17, wide 4-petal
        {6, 7},  //  6 rot, max_r=14, 6-petal rosette
        {8, 6},  //  8 rot, max_r=11, compact 8-petal
        {9, 6},  //  9 rot, max_r=10, tight 9-petal
        {3, 7},  //  3 rot, max_r=17, bold 3-lobed
    };
    static constexpr uint8_t kNTable = sizeof(kTable) / sizeof(kTable[0]);
    const uint8_t kR_int = layer->size.x / 2 - 3;  // 13
    const coord kR = coord::from_raw((int32_t)kR_int << 16);
    const coord kStep = coord::from_raw(16384);  // 0.25 rad/frame

    // Pick a new random variant and reset curve state.
    if (reinit || closeRaw == 0) {
      idx = random8() % kNTable;
      hue = random8();
      hypoTheta = coord{};
      rollPhase = coord{};
      firstPoint = true;
      // closeAngle = 2π × r  (gcd(r, R−r) = 1 for all table entries with R=13 prime)
      closeRaw = (int32_t)kTable[idx].r * kTwoPi.raw();
    }

    coord ri_c = coord::from_raw((int32_t)kTable[idx].r << 16);
    coord h_c = coord::from_raw((int32_t)kTable[idx].h << 16);
    coord arm = kR - ri_c;                   // (R − r)
    coord rollDelta = kStep * (arm / ri_c);  // advance roll by kStep × (R−r)/r

    coord penX = cx + fl::s16x16::cos(hypoTheta) * arm + fl::s16x16::cos(rollPhase) * h_c;
    coord penY = cy + fl::s16x16::sin(hypoTheta) * arm - fl::s16x16::sin(rollPhase) * h_c;

    fl::CanvasRGB canvas(fl::span<CRGB>(canvasBuf, layer->size.x * layer->size.y), layer->size.x, layer->size.y);

    if (!firstPoint) {
      canvas.drawLine(CHSV(hue, 220, 255), prevX, prevY, penX, penY);
      hue++;
    } else {
      firstPoint = false;
    }
    prevX = penX;
    prevY = penY;

    hypoTheta = hypoTheta + kStep;
    rollPhase = rollPhase + rollDelta;
    if (rollPhase.raw() >= kTwoPi.raw()) rollPhase = rollPhase - kTwoPi;

    if (hypoTheta.raw() >= closeRaw) {
      // Cycle complete — pick next variant; signal caller to clear the screen.
      idx = random8() % kNTable;
      hue = random8();
      hypoTheta = coord{};
      rollPhase = coord{};
      firstPoint = true;
      closeRaw = (int32_t)kTable[idx].r * kTwoPi.raw();
      return 255;
    }
    return 0;  // accumulate without any fading
  }

  // ── branching organic tree with wind sway ────────────────────────────────
  // A 6-level fractal binary tree redrawn fresh every frame.  A sinusoidal
  // wind angle accumulates branch-by-branch toward the tips, so leaves sway
  // while the trunk stays almost still.  Colour grades from warm brown at the
  // trunk to bright spring-green at tips.  Returns 255: hard-clear each frame.
  coord gTreeSwayStep;       // wind step/level; set by demoBranchingTree, read by treeSegment
  coord gTreeGrowFrac;       // growth phase [0,1]: trunk appears first, tips last
  coord gTreeSpread;         // current branch spread angle (opens as plant grows)
  uint8_t gTreeNest = 0;     // current recursion depth inside treeSegment
  uint8_t gTreeNestMax = 0;  // high-water mark of nesting depth this frame

  void treeSegment(coord x0, coord y0, coord angle, coord length, uint8_t depth) {
    ++gTreeNest;
    // Update crash-point register at every entry so the last write before a reset
    // tells exactly which depth and step was executing when the crash occurred.
    // gCPFrame = gTreeNest;
    // gCPGrow = gTreeGrowFrac.raw();
    // gCP = (uint8_t)((depth << 4) | 0x1);  // step 1: entered, before guards

    if (gTreeNest > gTreeNestMax) {
      gTreeNestMax = gTreeNest;
    }
    if (depth == 0) {
      --gTreeNest;
      return;
    }  // hard stop — guards against uint8_t wrap on runaway recursion
    static constexpr uint8_t kMax = 6;

    // Per-level growth: trunk (depth=kMax) appears first, tips (depth=1) last.
    // levelGrow = clamp(growFrac*kMax − (kMax−depth), 0, 1)
    int32_t lgRaw = (int32_t)gTreeGrowFrac.raw() * kMax - ((kMax - depth) << 16);
    if (lgRaw <= 0) {
      --gTreeNest;
      return;
    }
    coord levelGrow = coord::from_raw(lgRaw < 65536 ? lgRaw : 65536);

    coord effLen = length * levelGrow;
    if (effLen.raw() < 22938) {
      --gTreeNest;
      return;
    }  // stop below 0.35 px

    // gCP = (uint8_t)((depth << 4) | 0x2);  // step 2: passed guards, about to sin/cos

    // Wind sway: 0 at trunk, 5× at tips
    coord swayAcc = gTreeSwayStep * coord::from_raw((int32_t)(kMax - depth) << 16);
    coord drawn = angle + swayAcc;
    coord x1 = x0 + fl::s16x16::sin(drawn) * effLen;
    coord y1 = y0 - fl::s16x16::cos(drawn) * effLen;
    // gCP = (uint8_t)((depth << 4) | 0x3);  // step 3: sin/cos done, about to drawLine

    // Mature colour: trunk → warm brown; tips → spring-green
    uint8_t d = kMax - depth;
    uint8_t gv = (uint8_t)(50u + (uint16_t)d * 34u);
    uint8_t rv = (uint8_t)(80u - (uint16_t)d * 14u);
    uint8_t bv = (depth == 1) ? 20u : 0u;
    // Fresh growth blends toward bright yellow-white (255,255,100)
    uint8_t young = (uint8_t)((65536u - (uint32_t)levelGrow.raw()) >> 8);
    if (young > 0) {
      rv = rv + (uint8_t)(((uint16_t)(255u - rv) * young) >> 8);
      gv = gv + (uint8_t)(((uint16_t)(255u - gv) * young) >> 8);
      bv = bv + (uint8_t)(((uint16_t)(100u - bv) * young) >> 8);
    }
    CRGB col(rv, gv, bv);

    static const coord kScale = coord::from_raw(53739);  // 0.82 branch-length ratio

    fl::CanvasRGB canvas(fl::span<CRGB>(canvasBuf, layer->size.x * layer->size.y), layer->size.x, layer->size.y);

    canvas.drawLine(col, x0, y0, x1, y1);
    // gCP = (uint8_t)((depth << 4) | 0x4);  // step 4: drawLine done, about to recurse left

    coord childLen = length * kScale;
    treeSegment(x1, y1, angle - gTreeSpread, childLen, depth - 1);
    // gCP = (uint8_t)((depth << 4) | 0x5);  // step 5: left child returned, about to recurse right
    treeSegment(x1, y1, angle + gTreeSpread, childLen, depth - 1);
    // gCP = (uint8_t)((depth << 4) | 0x6);  // step 6: right child returned
    --gTreeNest;
  }

  uint8_t sLastNestMax = 0;

  uint8_t demoBranchingTree(coord cx, coord cy, coord r, uint32_t ms) {
    // Wind sway: 9 s period
    coord windPhase = coord::from_raw((int32_t)((uint64_t)(ms % 9000u) * (uint32_t)kTwoPi.raw() / 9000u));
    gTreeSwayStep = fl::s16x16::sin(windPhase) * coord::from_raw(3932);  // ±0.06 rad/level

    // Growth cycle: smooth 0→1→0 via (1−cos)/2, period 10 s.
    // Trunk grows first; tips emerge last.  Reverse on the way down.
    coord growPhase = coord::from_raw((int32_t)((uint64_t)(ms % 10000u) * (uint32_t)kTwoPi.raw() / 10000u));
    gTreeGrowFrac = (coord::from_raw(65536) - fl::s16x16::cos(growPhase)) / coord(2);
    if (gTreeGrowFrac.raw() > 65536) gTreeGrowFrac = coord::from_raw(65536);  // clamp to 1.0

    // Spread angle: opens from 0 to full 28° over the first half of growth
    static const coord kFullSpread = coord::from_raw(32035);  // 28°
    coord spreadScale = gTreeGrowFrac * coord(2);
    if (spreadScale.raw() > 65536) spreadScale = coord::from_raw(65536);
    gTreeSpread = kFullSpread * spreadScale;

    coord trunkX = cx;
    coord trunkY = cy + r * coord::from_raw(65011);  // cy + 0.99 r (near bottom)
    coord trunkLen = r * coord::from_raw(36045);     // 0.55 r ≈ 7 px
    gTreeNest = 0;
    gTreeNestMax = 0;
    // gCP = 0;  // 0 = outside treeSegment
    treeSegment(trunkX, trunkY, coord{}, trunkLen, 6);
    // Report max nesting depth reached this frame (printed once per new maximum).
    if (gTreeNestMax != sLastNestMax) {
      sLastNestMax = gTreeNestMax;
    }
    return 255;
  }

  ~FixedPointCanvasDemoEffect() override {
    if (canvasBufOwned && canvasBuf) freeMB(canvasBuf, "fpcdBuf");
  }
};

// Ported from AuroraPortal colorTrails by u/StefanPetrick / 4wheeljive
// https://github.com/4wheeljive/AuroraPortal/blob/main/src/programs/colorTrails_detail.hpp
// Noise-driven advection flow field with color emitters (orbiting circles, Lissajous line, rainbow border).
// Uses fl::CanvasRGB for subpixel-accurate emitter drawing and s16x16 fixed-point math.
class ColorTrailsEffect : public Node {
 public:
  static const char* name() { return "Color Trails"; }
  static uint8_t dim() { return _2D; }
  static const char* tags() { return "🔥🆕"; }
  static const char* category() { return "FastLED"; }

  // Controls
  uint8_t bpm = 60;        // overall animation speed (60 = normal)
  uint8_t mode = 0;        // 0=all, 1=orbital, 2=lissajous, 3=border
  uint8_t fade = 250;      // 0=fast fade, 255=slow fade
  uint8_t flowSpeed = 128; // noise scroll speed
  uint8_t flowShift = 128; // max pixel shift per row/column
  uint8_t noiseScale = 85; // noise spatial frequency
  uint8_t colorSpeed = 128;
  uint8_t emitterSize = 128; // orbit diameter or endpoint speed
  bool smear = false;      // reverse X noise profile

  void setup() override {
    addControl(bpm, "bpm", "slider");
    addControl(mode, "mode", "select");
    addControlValue("All");
    addControlValue("Orbital");
    addControlValue("Lissajous");
    addControlValue("Border");
    addControl(fade, "fade", "slider");
    addControl(flowSpeed, "flowSpeed", "slider");
    addControl(flowShift, "flowShift", "slider");
    addControl(noiseScale, "noiseScale", "slider");
    addControl(colorSpeed, "colorSpeed", "slider");
    addControl(emitterSize, "emitterSize", "slider");
    addControl(smear, "smear", "checkbox");

    noiseX.init(42);
    noiseY.init(1337);
    t0 = fl::millis();
    lastFrameMs = t0;
  }

 private:
  // 1D Perlin noise generator (no static state — each instance is independent)
  struct Perlin1D {
    uint8_t perm[512];

    void init(uint32_t seed) {
      uint8_t p[256];
      for (int i = 0; i < 256; i++) p[i] = (uint8_t)i;
      uint32_t s = seed;
      for (int i = 255; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        int j = (int)((s >> 16) % (uint32_t)(i + 1));
        uint8_t tmp = p[i]; p[i] = p[j]; p[j] = tmp;
      }
      for (int i = 0; i < 256; i++) {
        perm[i]       = p[i];
        perm[i + 256] = p[i];
      }
    }

    float noise(float x) const {
      int   xi = ((int)fl::floorf(x)) & 255;
      float xf = x - fl::floorf(x);
      float u  = xf * xf * xf * (xf * (xf * 6.0f - 15.0f) + 10.0f);
      float ga = (perm[xi]     & 1) ? -xf         :  xf;
      float gb = (perm[xi + 1] & 1) ? -(xf - 1.f) : (xf - 1.f);
      return ga + u * (gb - ga);
    }
  };

  Perlin1D noiseX{}, noiseY{};
  float* xProf = nullptr;  // one noise value per column
  float* yProf = nullptr;  // one noise value per row
  CRGB* canvasBuf = nullptr;  // local buffer when needed, or points to channelsE
  CRGB* tempBuf = nullptr;    // intermediate buffer for advection (always allocated)
  size_t canvasBufSize = 0;
  size_t xProfSize = 0;
  size_t yProfSize = 0;
  size_t tempBufSize = 0;
  bool canvasBufOwned = false; // true if we allocated canvasBuf (must free it)
  unsigned long t0 = 0;
  unsigned long lastFrameMs = 0;
  uint8_t lastActiveMode = 0;

  static float clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
  }

  static float fmodPos(float x, float m) {
    float r = fl::fmodf(x, m);
    return r < 0.0f ? r + m : r;
  }

  bool needsLocalBuf() { return !layer->oneToOneMapping || layerP.lights.header.channelsPerLight != 3; }

  void allocBuffers() {
    int W = layer->size.x;
    int H = layer->size.y;
    size_t nrOfLights = W * H;
    if (needsLocalBuf()) {
      if (!canvasBufOwned) { canvasBuf = nullptr; canvasBufSize = 0; } // don't realloc the aliased channelsE pointer
      reallocMB2<CRGB>(canvasBuf, canvasBufSize, nrOfLights, "ctCanvas");
      canvasBufOwned = true;
    } else {
      if (canvasBufOwned && canvasBuf) { freeMB(canvasBuf, "ctCanvas"); }
      canvasBuf = (CRGB*)layerP.lights.channelsE;
      canvasBufSize = nrOfLights;
      canvasBufOwned = false;
    }
    reallocMB2<float>(xProf, xProfSize, W, "ctXProf");
    reallocMB2<float>(yProf, yProfSize, H, "ctYProf");
    reallocMB2<CRGB>(tempBuf, tempBufSize, nrOfLights, "ctTemp");
  }

  // Sample 1D Perlin noise into a profile array
  // freq scales inversely with count so flow structures stay proportional on any display size
  void sampleProfile(const Perlin1D& n, float t, float speed, float amp, float scale, int count, float* out) {
    const float freq  = 0.23f * (16.0f / MAX(count, 1));
    const float phase = t * speed;
    for (int i = 0; i < count; i++) {
      float v = n.noise(i * freq * scale + phase);
      out[i]  = clampf(v * amp, -1.0f, 1.0f);
    }
  }

  // Two-pass fractional advection (bilinear interpolation) + per-pixel fade
  void advectAndDim(CRGB* buf, int W, int H, float dt) {
    float fadePerSec = fl::powf((0.90f + fade * 0.000392f), 60.0f);
    float fadeMul = fl::powf(fadePerSec, dt);
    float fShift = (flowShift / 85.0f) * (MIN(W, H) / 16.0f); // scales with display (original ~1.8 on 16px)

    // Pass 1 — horizontal row shift (Y-noise drives X movement)
    for (int y = 0; y < H; y++) {
      float sh = (y < (int)yProfSize) ? yProf[y] * fShift : 0.0f;
      for (int x = 0; x < W; x++) {
        float sx  = fmodPos((float)x - sh, (float)W);
        int   ix0 = (int)fl::floorf(sx) % W;
        int   ix1 = (ix0 + 1) % W;
        float f   = sx - fl::floorf(sx);
        float inv = 1.0f - f;
        CRGB& s0 = buf[y * W + ix0];
        CRGB& s1 = buf[y * W + ix1];
        tempBuf[y * W + x] = CRGB(
          (uint8_t)(s0.r * inv + s1.r * f),
          (uint8_t)(s0.g * inv + s1.g * f),
          (uint8_t)(s0.b * inv + s1.b * f)
        );
      }
    }

    // Pass 2 — vertical column shift (X-noise drives Y movement) + dim
    for (int x = 0; x < W; x++) {
      float sh = (x < (int)xProfSize) ? xProf[x] * fShift : 0.0f;
      for (int y = 0; y < H; y++) {
        float sy  = fmodPos((float)y - sh, (float)H);
        int   iy0 = (int)fl::floorf(sy) % H;
        int   iy1 = (iy0 + 1) % H;
        float f   = sy - fl::floorf(sy);
        float inv = 1.0f - f;
        CRGB& s0 = tempBuf[iy0 * W + x];
        CRGB& s1 = tempBuf[iy1 * W + x];
        buf[y * W + x] = CRGB(
          (uint8_t)fl::floorf((s0.r * inv + s1.r * f) * fadeMul),
          (uint8_t)fl::floorf((s0.g * inv + s1.g * f) * fadeMul),
          (uint8_t)fl::floorf((s0.b * inv + s1.b * f) * fadeMul)
        );
      }
    }
  }

  // Inject 3 orbiting rainbow circles using canvas drawDisc
  void injectOrbiting(fl::CanvasRGB& canvas, float t, int W, int H) {
    coord cx = coord::from_raw((int32_t)W << 15);
    coord cy = coord::from_raw((int32_t)H << 15);
    int minDim = MIN(W, H);
    // orbit radius scales with display: emitterSize controls fraction of half-display (0→0%, 255→~90%)
    float orbRadius = (emitterSize / 255.0f) * (minDim * 0.45f);
    coord orad = coord(orbRadius);
    float orbSpeed = emitterSize / 365.0f; // ~0-0.7
    float colSpeed = colorSpeed / 2550.0f;
    float base = t * orbSpeed;
    // circle size scales with display (relative to a 16px baseline)
    float circleDiam = 1.5f * (minDim / 16.0f);

    for (int i = 0; i < 3; i++) {
      float a = base + i * (2.0f * 3.14159265f / 3.0f);
      coord dcx = cx + coord(fl::cosf(a)) * orad;
      coord dcy = cy + coord(fl::sinf(a)) * orad;
      float hue = fmodPos(t * colSpeed + i / 3.0f, 1.0f);
      CHSV hsv((uint8_t)(hue * 255.0f), 255, 255);
      CRGB rgb;
      hsv2rgb_rainbow(hsv, rgb);
      canvas.drawDisc(rgb, dcx, dcy, coord(circleDiam * 0.5f));
    }
  }

  // Previous Lissajous endpoint positions for inter-frame interpolation
  float prevLx1 = -1, prevLy1 = -1, prevLx2 = -1, prevLy2 = -1;

  // Inject a Lissajous line with rainbow color using canvas drawLine + drawDisc
  void injectLissajous(fl::CanvasRGB& canvas, float t, int W, int H) {
    float cx = (W - 1) * 0.5f;
    float cy = (H - 1) * 0.5f;
    float s = emitterSize / 365.0f;
    float amp = (MIN(W, H) - 4) * 0.5f;
    float colShift = colorSpeed / 2550.0f;

    float lx1 = cx + (amp + 1.5f) * fl::sinf(t * s * 1.13f + 0.20f);
    float ly1 = cy + (amp + 0.5f) * fl::sinf(t * s * 1.71f + 1.30f);
    float lx2 = cx + (amp + 2.0f) * fl::sinf(t * s * 1.89f + 2.20f);
    float ly2 = cy + (amp + 1.0f) * fl::sinf(t * s * 1.37f + 0.70f);

    // On large displays, endpoints can jump several pixels between frames.
    // Interpolate sub-steps between previous and current positions to fill gaps.
    int nSub = 1;
    if (prevLx1 >= 0) {
      float maxJump = MAX(
        MAX(fl::fabsf(lx1 - prevLx1), fl::fabsf(ly1 - prevLy1)),
        MAX(fl::fabsf(lx2 - prevLx2), fl::fabsf(ly2 - prevLy2))
      );
      nSub = MAX(1, (int)(maxJump * 0.5f)); // one sub-step per ~2 pixels of motion
    }

    for (int sub = 0; sub < nSub; sub++) {
      float frac = (nSub == 1) ? 1.0f : (float)(sub + 1) / (float)nSub;
      float sx1, sy1, sx2, sy2;
      if (prevLx1 >= 0 && nSub > 1) {
        sx1 = prevLx1 + (lx1 - prevLx1) * frac;
        sy1 = prevLy1 + (ly1 - prevLy1) * frac;
        sx2 = prevLx2 + (lx2 - prevLx2) * frac;
        sy2 = prevLy2 + (ly2 - prevLy2) * frac;
      } else {
        sx1 = lx1; sy1 = ly1; sx2 = lx2; sy2 = ly2;
      }

      // Draw the line with rainbow gradient using multiple segments
      float dx = sx2 - sx1;
      float dy = sy2 - sy1;
      float maxd = fl::fabsf(dx) > fl::fabsf(dy) ? fl::fabsf(dx) : fl::fabsf(dy);
      int steps = MAX(1, (int)(maxd * 3.0f));
      for (int i = 0; i < steps; i++) {
        float u0 = (float)i / (float)steps;
        float u1 = (float)(i + 1) / (float)steps;
        // blend sub-step fraction into hue for temporal continuity
        float tSub = t - (1.0f - frac) * 0.033f; // approximate temporal offset
        float hue = fmodPos(tSub * colShift + u0, 1.0f);
        CHSV hsv((uint8_t)(hue * 255.0f), 255, 255);
        CRGB rgb;
        hsv2rgb_rainbow(hsv, rgb);
        canvas.drawLine(rgb,
          coord(sx1 + dx * u0), coord(sy1 + dy * u0),
          coord(sx1 + dx * u1), coord(sy1 + dy * u1));
      }
    }

    // Endpoint discs at final position
    float hue0 = fmodPos(t * colShift, 1.0f);
    float hue1 = fmodPos(t * colShift + 1.0f, 1.0f);
    CRGB ca, cb;
    hsv2rgb_rainbow(CHSV((uint8_t)(hue0 * 255.0f), 255, 255), ca);
    hsv2rgb_rainbow(CHSV((uint8_t)(hue1 * 255.0f), 255, 255), cb);
    float discR = 0.85f * (MIN(W, H) / 16.0f);
    canvas.drawDisc(ca, coord(lx1), coord(ly1), coord(discR));
    canvas.drawDisc(cb, coord(lx2), coord(ly2), coord(discR));

    prevLx1 = lx1; prevLy1 = ly1; prevLx2 = lx2; prevLy2 = ly2;
  }

  // Inject rainbow colors along the grid border
  void injectBorder(CRGB* buf, float t, int W, int H) {
    float colShift = colorSpeed / 2550.0f;
    int total = 2 * (W + H) - 4;
    int idx = 0;
    auto rainbow = [&](int i) -> CRGB {
      float hue = fmodPos(t * colShift + (float)i / total, 1.0f);
      CHSV hsv((uint8_t)(hue * 255.0f), 255, 255);
      CRGB rgb;
      hsv2rgb_rainbow(hsv, rgb);
      return rgb;
    };
    for (int x = 0; x < W; x++) { buf[x] = rainbow(idx++); }
    for (int y = 1; y < H; y++) { buf[y * W + W - 1] = rainbow(idx++); }
    for (int x = W - 2; x >= 0; x--) { buf[(H - 1) * W + x] = rainbow(idx++); }
    for (int y = H - 2; y > 0; y--) { buf[y * W] = rainbow(idx++); }
  }

 public:
  void onSizeChanged(const Coord3D& prevSize) override { allocBuffers(); }

  void loop() override {
    int W = layer->size.x;
    int H = layer->size.y;
    if (W <= 0 || H <= 0) return;

    // Refresh buffers if uninitialized or if mapping/channel config changed without a size change
    if (!canvasBuf || canvasBufOwned != needsLocalBuf()) allocBuffers();
    if (!canvasBuf || !tempBuf || !xProf || !yProf) return;

    unsigned long now = fl::millis();
    float dt = (now - lastFrameMs) * 0.001f;
    if (dt > 0.5f) dt = 0.033f; // clamp on first frame or long stalls
    lastFrameMs = now;
    float t = (now - t0) * 0.001f * (bpm / 60.0f); // bpm scales animation speed (60 = normal)

    // Map controls to float ranges
    float fFlowSpeed = flowSpeed / 75.0f;  // ~0-3.4
    float fNoiseScale = noiseScale / 255.0f; // 0-1.0

    // Build noise profiles
    sampleProfile(noiseX, t, fFlowSpeed, 1.0f, fNoiseScale, MIN(W, (int)xProfSize), xProf);
    sampleProfile(noiseY, t, fFlowSpeed * 0.99f, 1.0f, fNoiseScale * 0.97f, MIN(H, (int)yProfSize), yProf);

    // Apply smear (reverse X profile)
    if (smear) {
      int half = MIN(W, (int)xProfSize) / 2;
      for (int i = 0; i < half; i++) {
        float tmp = xProf[i];
        xProf[i] = xProf[MIN(W, (int)xProfSize) - 1 - i];
        xProf[MIN(W, (int)xProfSize) - 1 - i] = tmp;
      }
    }

    // Inject emitters
    fl::CanvasRGB canvas(fl::span<CRGB>(canvasBuf, W * H), W, H);
    uint8_t activeMode = mode;
    if (activeMode == 0) { // All: cycle every 8 seconds
      activeMode = ((now - t0) / 8000) % 3 + 1;
    }
    if (activeMode != lastActiveMode) {
      prevLx1 = -1; // reset Lissajous interpolation on mode switch
      lastActiveMode = activeMode;
    }
    switch (activeMode) {
      default:
      case 1: injectOrbiting(canvas, t, W, H); break;
      case 2: injectLissajous(canvas, t, W, H); break;
      case 3: injectBorder(canvasBuf, t, W, H); break;
    }

    // Advect + fade
    advectAndDim(canvasBuf, W, H, dt);

    // Copy canvas to channelsE via setRGB when using local buffer
    if (canvasBufOwned) {
      nrOfLights_t nrOfLights = W * H;
      for (nrOfLights_t i = 0; i < nrOfLights && i < canvasBufSize; i++) {
        layer->setRGB(i, canvasBuf[i]);
      }
    }
  }

  ~ColorTrailsEffect() override {
    if (canvasBufOwned && canvasBuf) freeMB(canvasBuf, "ctCanvas");
    if (xProf) freeMB(xProf, "ctXProf");
    if (yProf) freeMB(yProf, "ctYProf");
    if (tempBuf) freeMB(tempBuf, "ctTemp");
  }
};

#endif