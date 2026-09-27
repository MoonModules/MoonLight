# draw::sprite + FlyingToastersEffect spec (draft, ships with the implementation)

Classic screensavers on a light wall (a Discord request): sprites, small movable bitmaps with
transparency, and the first consumer, After Dark's flying toasters. The power-functions catalog
anticipated this: compositing was deferred "until sprites arrive"
([bottom-up](../../future/power-functions-analysis-bottom-up.md) § below-the-cut); this is the arrival.

## Division of labor (the design decision)

Power functions stay STATELESS draw primitives; state lives in kernels the effect owns. So:

- **Movement: nothing new.** A flying toaster is a `particles::Pool` entry with constant
  diagonal velocity and wrap, zero forces, the shipped movable-things kernel.
- **Appearance: one new power function.** `draw::sprite`, the multi-color sibling of
  `draw::glyph` (same home, same Canvas-first free-function shape).

No "movable sprite objects" in the power functions: that would open a second stateful home for
what the pool already does.

## draw::sprite (light/draw.h)

```
struct Sprite {                     // carried as data, the fonts.h precedent
    uint8_t w, h, frames;           // frame f is rows [f*h, (f+1)*h)
    const uint8_t* pixels;          // palette-INDEXED, w*h*frames bytes; index 0 = transparent
    const CRGB* palette;            // the sprite's own colors (small, per-sprite)
    uint8_t paletteCount;
};
void sprite(const Canvas& cv, const Sprite& s, uint8_t frame, pos_t x, pos_t y);
```

- Index 0 transparent is the classic key-color scheme; full alpha (Porter-Duff over) stays
  deferred until a consumer needs it.
- `pos_t` (24.8) placement, rounded to the pixel grid for v1 (sub-pixel sprite AA is a later
  refinement; toasters read crisper without it); clipped at every edge like `draw::glyph`.
- Doc block names the prior art (key-color blitting; After Dark as the effect's inspiration).

## FlyingToastersEffect (light/effects/)

- N toasters (default 6, control 1..16) + M toasts (default 3), all in one small
  `particles::Pool`: spawn along the top+right edge band, constant velocity toward lower-left
  (the canonical diagonal), wrap by respawn at the opposite band.
- Toasters cycle a 4-frame wing flap (frame = beatPhase-derived, per-toaster phase offset so
  wings do not sync); toast is 1 frame.
- **Original pixel art, drawn for this effect** (~16x12, a handful of palette entries). The
  After Dark art is Berkeley Systems' and famously litigated; nothing is copied from it or
  from Adafruit's derived sprite bytes (also the no-derivation rule).
- Controls: `toasters`, `toast`, `speed`; standard palette control NOT used (sprites carry
  their own colors); black background each frame.
- 2D-primary like `text`: renders a z-slice on 3D fixtures, meaningless in 1D (the pipeline's
  extrude rule covers it, same as today's 2D effects).

## Tests

- unit (draw): sprite blits frame f at (x,y) with index-0 holes, clips at all four edges,
  rejects out-of-range frame by clamping.
- golden-frame (the power-functions harness): FlyingToasters at fixed seed + fixed time,
  byte-compare.

## Verification

Desktop build + ctest; live: the effect on the HLS stream / preview, PO judges the flap and
drift; then a board (S3 128x128).

## Later (filed, not scope)

- P4 PPA acceleration behind the same `draw::sprite` signature (backlog-light entry).
- Porter-Duff `over` when a sprite/layer consumer needs real alpha.
- MoonLive exposure: scripts cannot carry bitmap data yet; lands with the stage-3 builtin
  table + array support.

---

# Plan: draw::sprite + FlyingToastersEffect

## Context

Spec: docs/backlog/sprite-toasters-spec.md (PO asked to relocate it to docs/history/plans/ —
first step below). Classic-screensaver sprites for the light wall: one new STATELESS power
function (`draw::sprite`, the multi-color sibling of `draw::glyph`) plus the first consumer,
FlyingToastersEffect, whose movement rides the existing `particles::Pool`. Original pixel art
only (After Dark's is Berkeley Systems'; nothing copied from Adafruit's derived bytes).

## Corrections to the spec discovered in exploration

- **No `CRGB` exists**: the color type is `RGB` (core/color.h:7). Sprite palette =
  `inline constexpr RGB kToasterPalette[]`, the first constexpr RGB array (fonts.h pattern).
- The golden-frame harness ALREADY exists: test/unit/light/golden_frame.h (`ScopedTestClock`,
  `renderHash`, `checkGolden`), used by unit_Effects_golden.cpp. No new harness work.
- Randomness: per-effect `Random8 rng_{fixed-seed}` (math8.h:136) — deterministic goldens for
  free; the clock is the only thing the harness pins.


## draw::sprite (src/light/draw.h, beside glyph at :749)

```cpp
namespace sprites {                       // fonts.h shape: data struct + constexpr tables
struct Sprite {
    const uint8_t* pixels;    // palette-INDEXED, w*h*frames bytes; index 0 = transparent
    const RGB* palette;       // the sprite's own colors
    uint8_t w, h, frames, paletteCount;
};
}
// draw::sprite: blit frame f at pixel (x, y); index 0 is the transparent key; every write
// goes through draw::pixel so clipping is offsetOf's sentinel, exactly as glyph does.
inline void sprite(const Canvas& cv, const sprites::Sprite& s, uint8_t frame,
                   lengthType x, lengthType y);
```
Frame clamped to `frames - 1`. Pixel-grid placement for v1 (callers convert with
`draw::toPixel`); doc block names key-color blitting as prior art. `paletteCount` guards a
bad index (out-of-range index renders nothing, degrade-visible not UB).

## Art (in the effect header, fonts.h style)

Original pixel art, drawn fresh: a 4-frame toaster ~14x10 (chrome body two greys + highlight,
dark slot, wings light grey cycling up/mid/down/mid) and a 1-frame toast ~7x7 (two browns).
Palette ~7 entries incl. transparent 0. Data as `inline constexpr uint8_t` arrays with a
static_assert on sizes.

## FlyingToastersEffect (src/light/effects/FlyingToastersEffect.h)

BallpitEffect is the template (prepare's alloc-all-then-check ScratchBuffer wiring into a
`particles::Pool`, FrameTime{60}, manual render loop instead of pool.render):

- Controls: `toasters` (1..12, default 5), `toast` (0..8, default 3), `speed` (1..255,
  default 96). `Dim::D2`, tags 🔬.
- prepare(): pool sized kPool = 20 (12+8); spawn each entry in the top/right off-screen band
  with velocity toward lower-left, magnitude from `speed` with a per-entry ±25% variation
  (Random8, fixed seed); `hue[i]` reused as sprite-kind flag (toaster/toast).
- tick(): canvas guard (w,h >= sprite size), `fill` black, FrameTime scale, `pool_.step`,
  entries leaving the lower-left band respawn into the upper-right band (killOutside +
  respawn, per the spec); render loop: `draw::sprite(cv, kind, frameOf(i), toPixel(x),
  toPixel(y))`. Wing frame = `BeatPhase`-derived with per-entry offset (`i * 0x4000`) so
  flaps never sync; toast is frame 0 of its own sprite.
- No palette use (sprites carry colors) — nothing to declare, effects only opt IN to
  colorFromPalette.

## Registration + docs

- main.cpp: include + `registerType<FlyingToastersEffect>("FlyingToastersEffect",
  "light/effects.md#flyingtoasters")` in the alphabetical list (~:234).
- docs/moonmodules/light/effects.md: the Ballpit-shaped card (anchor `flyingtoasters`,
  🔬 · 2D, controls list, Origin: MoonLight original; inspired by After Dark's Flying
  Toasters (Berkeley Systems), art drawn fresh for this effect).
- power-functions.md (docs/moonmodules/light/): the `draw::sprite` entry beside glyph/text.

## Tests (test/CMakeLists.txt additions)

- unit_draw.cpp additions (the Surface fixture from unit_Canvas.cpp / the "L probe" shape of
  the glyph orientation test): sprite blits frame 1 not frame 0 at the offset, index-0 holes
  leave the background, clips at all four edges (draw at -1,-1 and w-1,h-1), out-of-range
  frame clamps, out-of-range palette index writes nothing.
- unit_Effects_golden.cpp: `FlyingToastersEffect` golden hash at 16x16 (the Ballpit line's
  shape), with the paste-back flow on first run.

## Verification

Desktop build zero warnings; ctest green (new draw asserts + golden). Live: the effect on
localhost:8080's preview and the HLS stream (a TV full of toasters is the demo the Discord
thread wants a clip of); PO judges flap cadence and drift; then a board (S3 128x128).

## Out of scope (already filed in backlog-light)

P4 PPA acceleration behind the same signature; Porter-Duff alpha; MoonLive sprite data.
