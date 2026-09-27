# Plan — Migrate MoonLight effects / modifiers / layouts (multi-stage)

> **Superseded on 2026-09-22** by [Plan-20260922 - MoonLight, from v5.0.0 to the rename](../../present/Plan-20260922%20-%20MoonLight,%20from%20v5.0.0%20to%20the%20rename.md), which consolidates the five MoonLight files into one. Kept for the reasoning behind decisions already taken.

## Goal & shape

Bring MoonLight's full library of **effects, modifiers and layouts** into MoonLight. This is large, so it is **staged**: each stage ships independently, builds on the previous, and is its own `/plan` + commit. This document is the *map* — the per-stage plans get written when we reach them. Stages 1–2 are specified enough to start; later stages are scoped, not detailed.

**Why this matters beyond features:** this migration is the execution vehicle for the **effect-breadth parity gate** in the MoonLight → MoonLight rename plan — taking the MoonLight name requires the library not to feel thin next to the predecessor's 60+ effects. The rename's bar is "enough batches landed," not "every stage done"; this plan is *how* that bar is reached. (The two docs stay in their folders — the rename is the forward-looking backlog item that sets the bar; this is the approved staged plan that meets it — linked, not duplicated.)

Two cross-cutting rules govern every stage, from [CLAUDE.md](../../../CLAUDE.md):

- **Industry standards, our own code.** MoonLight effects are studied for *behaviour and algorithm*, then written **fresh** against our architecture (our `EffectBase`, our primitives, our names). We do **not** trace MoonLight/WLED/FastLED structure or copy code. For *effects specifically* the **visual behaviour is the spec** — we reproduce what the effect looks like faithfully (the product owner's clarification), but the implementation is ours. Prior art credited per-module + in `history/`.
- **A shared light primitive library.** Effects need a common set of small math/color helpers (a beat/sine oscillator, integer noise, saturating add/subtract, scale, fade, a color blend, a fast PRNG, draw primitives). MoonLight provides these, extending the `color.h` set (`scale8`, `sin8`, `cos8`, `hsvToRgb` already there): **hot-path-tuned** (integer-only, LUT-backed, no float in the per-light path) and **dimension-agnostic where it makes sense** (the product owner's steer: our 3D-native model means a primitive like `drawLine` works 1D→3D, written once, not re-implemented per effect).
  - **Naming follows *Common patterns first* + *Industry standards, our own code*: the recognisable name AND our own implementation.** The LED-embedded world's canonical resource is FastLED, and its names (`beatsin8`, `inoise8`, `qadd8`, `nscale8`, `random8`/`random16`, `ColorFromPalette`) are exactly the ones a contributor recognises in 30 seconds — and consistent with the `scale8`/`sin8` we already ship. So **we use those names** (carrying the established convention), **write our own implementation** against our engine, and **credit FastLED as prior art** in each module's "Prior art" section. The point of the principle is independence-by-construction (own code, own architecture, behaviour pinned by tests), *not* a renamed copy — so the names stay recognisable; only the implementation is ours. Each primitive's design is justified at its introduction site, and we reorganise a borrowed concept when ours is genuinely cleaner (e.g. the dimension-agnostic draw set).

## What exists today (baseline)

- **Primitives:** `src/core/util/color.h` has `RGB`, `hsvToRgb`, `scale8`, `sin8`/`cos8` (LUT). `src/light/util/light_types.h` has `Coord3D`, `Dim`, `lengthType`. That's it — no beat/noise/blend/random helpers, no shared palette, no draw primitives.
- **Palette:** none shared. `PlasmaPaletteEffect` hard-codes a 256-entry `RGB palette_[256]` in flash — the pattern to generalise.
- **Effects:** ~21 already ported (Rainbow, Noise, Plasma, Fire, Particles, Metaballs, GameOfLife, Wave, …). GameOfLife (272 lines) is flagged by the product owner as **not faithful — re-port from the real algorithm**.
- **Modifiers:** Multiply, Rotate, Region, Checkerboard, RandomMap. **Layouts:** Grid, Sphere, Wheel.
- **Tags/emoji:** MoonLight already has `tags()` + UI-derived role/dim emoji (architecture.md § Web UI). MoonLight's legend (🔥 effect, 💎 modifier, ♫ audio, 🧊 3D, …) becomes the **canonical basis** (product owner's choice).
- **Docs:** one `.md` per module (21 effect specs already), enforced by `check_specs.py` (it `rglob`s each `.h` → a matching `.md`). Moving to **per-library pages** (`effects_<library>.md`, compact table rows) — see Stage 2 and the folder-structure decision. This requires changing the spec-check contract.
- **Assets:** **already reorganised** to `docs/assets/{core, light/{effects,modifiers,layouts,drivers}, ui}/` (the per-module move done ahead of the migration). Stage 2's gif work is *adding* MoonLight previews into this structure, not re-homing.

## Dependency analysis (what must come first)

1. **Palette** — hard prerequisite. Many MoonLight effects color via `ColorFromPalette`. Nothing palette-dependent can be faithfully ported until this lands. **Stage 1.**
2. **The shared primitive library** (beat / noise / blend / scale / random / draw) — most effects need several. **Stage 1.**
3. **Tags/emoji legend** — must be settled before batch-migrating, so every migrated module is consistent from the first batch. Cheap; **Stage 1** (a doc + a sweep of existing `tags()`).
4. **Doc model change** — must land before the doc explosion, i.e. before batch migration. A page per **library** (type-first name, underscore-joined): `effects_moonlight.md`, `effects_wled.md`, … (and `modifiers_<lib>.md` etc. only where a library has them; most are effects-only). Library is a *doc* split only — NOT a `src`/`assets`/`tests` folder (those stay `domain/type` flat; library is the `tags()` emoji there). Fixed by the folder-structure decision. **Stage 2**.
5. **Audio** — audio-reactive effects (♫) depend on `AudioModule::latestFrame()` (already exists). A later stage; not a blocker for non-audio effects.
6. **Moving heads / Art-Net fixtures** — `E_MovingHeads` targets DMX moving heads; depends on fixture-layout + Art-Net (partly present). Last, separate.

No other hidden hard dependencies: our `EffectBase` + extrude (now 1D-along-Y, matching MoonLight) + `Buffer` already provide the render context.

## Status — verified 2026-09-07

Measured against both trees on 2026-09-07 (96 commits after the previous status), by counting
MoonLight's `name()` declarations against our registered modules and script library. Counts are
what the trees say, not what the stages below predicted.

| | MoonLight | MoonLight | Gap |
|---|---|---|---|
| Effects | 88 | 66 compiled + 32 scripted | see below |
| Modifiers | 9 (+1 template) | 11 | **none: complete, plus 2 of our own** |
| Layouts | 16 (+1 template) | 17 | **3 absent, all installation-specific** |
| Drivers | 11 (+1 template) | 17 | **3 absent: DMX in/out, HUB75, IMU** |

| Stage | State |
|---|---|
| 1 — Foundations | **shipped.** Palette, draw primitives, the FastLED-named set, GoL re-port. |
| 2 — Doc model | **shipped, differently.** The per-library `effects_<library>.md` split was NOT built; the catalog is one page per TYPE (`effects.md`, `layouts.md`, `modifiers.md`, `drivers.md`) with a table row per module. That solves the same problem the stage existed for (no per-module explosion) with fewer pages, and `check_specs.py` enforces it. **The stage as written is obsolete: what shipped is better and the ADR's premise (library as a doc split) went unused.** |
| 3 — Effect batches | **substantially done.** 66 compiled effects and 32 scripted, against a ~21 baseline. |
| 4 — Modifiers + layouts | **done for modifiers** (all 9 ported). **Layouts: 14 of 16**, the three absent ones being specific installations rather than shapes. |
| 5 — Moving heads / DMX | **partial, and now the largest gap.** The EFFECT side shipped (`MovingHeadEffect` plus 5 `mh-*.mle` scripts, with pan/tilt/zoom/rotate/gobo reachable from both C++ and script). The TRANSPORT did not: there is still no DMX-512 output driver, and no DMX input. |

### What is genuinely missing (2026-09-07)

**Drivers, the real gap.** Three of MoonLight's have no counterpart here:

- **DMX Out** (and **DMX In**). The fixture model, the channel roles and the moving-head effects all
  landed, so a head can be driven over Art-Net today; what is missing is WIRED DMX-512 over RS-485.
  Tracked in [backlog-light § RS-485](../../future/backlog-light.md), where the analysis notes the
  channel-mapping half is already solved and what remains is the transport (a UART in RS-485 mode,
  break/mark timing) plus a physical transceiver. **This is the one Must-class gap for the rename.**
- ~~**HUB75.**~~ **Out of scope, decided 2026-09-07.** MoonLight drives these panels; MoonLight
  will not. No longer a gap: a choice.
- **IMU.** Sensor input beyond the microphone. The rename doc already files this as a Could.

**Layouts (3):** `16 Rings`, `SE16`, `LightCrafter16`. Each is one installation's wiring rather than
a reusable shape. **The product owner owns all three (2026-09-07)**, so they are real parity items
and each is bench-verifiable once written: a layout is a coordinate iterator, so these are small,
and the hardware to check them against is on hand.

**Effects: 57 of MoonLight's 88 do not match ours by NAME, but a behavior-by-behavior read of both
trees puts the real gap at 31.** The other 26 exist here under a different name or in a reduced form:

- **15 covered.** Fixed-Point Canvas Demo is our `FixedPointEffect`, Scrolling Text is `TextEffect`,
  Noise 2D and Noise Move are two `motion` settings of one `NoiseEffect`, Waterfall and Freq Wave
  are both `FreqMatrixEffect`, Julia is `fractal.mle`, the Troy / Wowi / Ambient heads are the
  `mh-*.mle` scripts, and our `VuMetersEffect` is richer than the original.
- **11 partial**, where something related exists but is meaningfully less: Audio Rings vs
  `RadialSpectrum` (no per-band ring history), Meteor vs `comet-trail.mle` (no randomized per-pixel
  trail decay), Drip vs `rain.mle` (no bounce physics), Popcorn vs `Ballpit` (no per-kernel pop),
  Puddles vs `Blurz`, Noise Fire vs `Fire`, and the Troy / Freq Colors audio-band-to-gobo mapping.
- **31 absent**, and the shape of that list is the useful finding:

| Theme | Count | Effort |
|---|---|---|
| Audio-reactive (Grav*, DJ Light, Freq Map/Pixels, Rocktaves, Ripple Peak, Waverly, Funky Plank) | 11 | A few lines each. Our `AudioFrame` is a SUPERSET of MoonLight's `sharedData`, so these are mechanical. The `Grav*` trio wants one shared ~15-line gravity/peak helper. |
| Geometric / oscillator (Blackhole, DNA, Frizzles, Oscillate, Radar, Pixel Map, Blink Rainbow) | 7 | A few lines each on `BeatPhase` + `draw::line` + `blur`. Radar wants a ~25-line perimeter walk. |
| Noise (Phased Noise, Plasmoid) | 2 | A few lines: 1D phase accumulators. |
| 1D strip (Flow, Police) | 2 | A few lines each. |
| Fire / volumetric (Spiral Fire) | 1 | Moderate, ~50 lines: a real 3D cone-surface test on the existing `PolarLut`. |
| Particle agents (Ants) | 1 | Substantial, ~150 lines: a food-gathering agent model with no analogue here. |
| Content-bound (Mario Test, Moon Man) | 2 | Mario is a few lines on the existing sprite path. **Moon Man is not portable**: it needs M5GFX PNG decoding and an embedded blob. |
| Other (Heartbeat, FLAudio, and the partials above) | 5 | Heartbeat is a few lines. **FLAudio is not an effect gap but an AUDIO-PIPELINE one**: it visualizes `fl_kick`/`fl_snare`/`fl_bpm`/`fl_vocalConfidence`, fields our analyzer does not produce. |

**So the effect work is mostly small and unblocked.** About 25 of the 31 are few-line ports against
primitives that already exist; two are genuinely not portable as-is (Moon Man's PNG dependency,
FLAudio's missing audio fields), and two are real work (Ants, Spiral Fire). Nothing structural
blocks any of it: the palette, primitives, audio pipeline and draw set all exist.

### The v5.0.0 gates, re-checked

The previous status recorded four gates beyond effect breadth. Two have since shipped:

- **MoonLive palettes — SHIPPED.** The research below is now history rather than a design note: the
  `setPalEntry`/`setPalEntryHSV` builtins exist, `.mlp` is the palette script kind, and 5 factory
  palettes ship. The open design questions it lists were answered by the implementation (a palette
  script is a module ticked by its binding, and the picker lists `.mlp` files from both the user and
  factory directories). **Keep the section for its record of where the design came from; do not
  read it as outstanding work.**
- **DMX light bars and moving heads — HALF.** Effects and the fixture/channel model shipped and are
  bench-verified over Art-Net; wired DMX output has not. See the driver gap above.
- **LightsControl maturity — NOT STARTED.** No such module exists. `LightPresetsModule` is the
  fixture-preset library, a different thing. Still backlogged
  ([backlog-mixed](../../future/backlog-mixed.md)).
- **Documentation pass — OPEN**, and cheaper than it was: the catalog pages exist and
  `check_specs.py` keeps them honest, so what remains is a read-through rather than a build-out.

### MoonLive palettes — how MoonLight does it (research, 2026-08-24)

Our MoonLive palette builtins (`setPaletteColor`, `paletteR/G/B`) all **read** the active palette. Nothing lets a script **define or animate** one. MoonLight has this, and the design is small enough to carry over nearly unchanged.

The mechanism (`ModuleLightsControl.h`, `livescripts/Palettes/`): a palette live script is a file named **`P_*.sc`**. The module walks the filesystem for that prefix and adds each hit to the `palette` control's dropdown under a `LiveScript` category, alongside the built-in gradients. Selecting one instantiates it as a `LiveScriptNode` and calls `setup()`. Two builtins write the 16 entries:

- `setPalEntry(i, r, g, b)`
- `setPalEntryHSV(i, h, s, v)`

`setup()` alone defines a **static** palette; a `loop()` makes it **animated**. Both shipped examples are tiny and are the whole contract:

```c
// P_Fire.sc — static, setup() only
void setup() {
  setPalEntry(0,   0,   0,   0);
  setPalEntry(4, 128,   0,   0);
  setPalEntry(8, 255,  64,   0);
  setPalEntry(12,255, 200,  40);
  setPalEntry(15,255, 255, 200);
}

// P_Shift.sc — animated, loop() shifts hue every frame
uint8_t hueShift;
void loop() {
  for (uint8_t i = 0; i < 16; i++)
    setPalEntryHSV(i, hueShift + i * 16, 255, 255);
  hueShift++;
}
```

**Why it transfers cheaply:** our `Palette` is already the same 16-entry model (`Palette::kEntries = 16`), and every effect already reads through `colorFromPalette(Palettes::active(), …)`. So a scripted palette needs no effect changes at all — 47 of our 52 effects pick it up for free. What is missing is the two write builtins, a script-kind convention (our `.mle`/`.mll` naming needs a palette equivalent), and the dropdown listing.

**Open design questions**, to settle in the stage plan rather than now:
- Where an animated palette script is ticked. MoonLight runs it as a node in the layer; our MoonLive scripts are modules, and a palette is global state owned by Drivers, so the tick site is not automatic.
- Whether animating the active palette every frame is acceptable on the hot path, given the 256-entry expansion our `colorFromPalette` interpolates against.
- Interaction with the eventual LightsControl hub ([backlog-mixed](../../future/backlog-mixed.md)), which is slated to absorb the palette control from Drivers.

Checkout note: the MoonLight tree read for this research was at `65869217` (2026-05-26) and may lag upstream; re-fetch before implementing.

## What is left to replace MoonLight (the product owner's decision list)

The question this plan now answers is not "how do we migrate" but "what is still missing before
MoonLight can take the name". Grouped by whether it BLOCKS the rename, on the evidence above.

**Blocking, in the sense that a predecessor user would notice it missing:**

1. **Wired DMX-512 output.** The only Must-class gap. Everything above the wire exists (fixture
   model, channel roles, moving-head effects, bench-verified over Art-Net); what is missing is the
   RS-485 transport and a board that carries a transceiver. This is also the item with a hardware
   dependency, so it has the longest lead time: worth starting before the smaller work.
2. ~~A decision on HUB75.~~ **DECIDED 2026-09-07: OUT OF SCOPE.** MoonLight drives HUB75 panels and
   MoonLight will not. Recorded here so it stays a decision rather than resurfacing as an unknown.

**Not blocking, and mostly small:**

3. **The 31 absent effects**, of which roughly 25 are few-line ports on primitives that already
   exist. This is the "does the library feel thin" gate, and at 66 compiled plus 32 scripted against
   MoonLight's 88 it is arguably already met on count. Worth picking the ones that close CATEGORY
   gaps a user would feel (the audio-reactive eleven) rather than working the list top to bottom.
4. **The 11 partials**, each a refinement of something that already works.
5. **Three layouts** (`16 Rings`, `SE16`, `LightCrafter16`), each one installation's wiring.
   **The product owner owns this hardware (2026-09-07), so all three are real parity items** rather
   than speculative ports, and each can be verified on the bench once written.
6. **LightsControl**, the one v5 gate that has not started. Still a backlog design question.
7. **DMX In and IMU**, both filed as Could in the rename doc and unchanged by this review.

**Moon Man is NOT portable as-is**: it needs M5GFX PNG decoding plus an embedded image blob.

**FLAudio is an audio-pipeline question, not an effect one**, and it splits cleanly. It draws nine
columns; four are already expressible with what we have, five are not:

- **Already ours.** Bass / mid / treble levels are aggregates of our `bands[]`. Its beat flag and
  beat confidence are our `onset` and `flux`, where `onset` is arguably better: it fires on the one
  block a hit is detected rather than staying latched.
- **Per-instrument onsets** (`kick`, `snare`, `tom`, `hihat`): spectral flux computed PER FREQUENCY
  REGION rather than across the whole spectrum, plus a per-drum debounce. Textbook, and the per-band
  data it needs already exists. **Worth building on its own merit**: per-band onset is a capability
  many effects would use, not just this one.
- **BPM**: tempo estimation from inter-onset intervals (autocorrelation over several seconds of
  history). Real DSP with a memory budget. Useful for anything that locks to tempo; a bigger job.
- **Vocal detection** (`vocalsActive`, `vocalConfidence`): formant-band energy against the total, a
  spectral-shape classifier. Highest cost, least reliable, and one effect consumes it.
  **Recommended: skip**, and let FLAudio ship without those two columns if it ships at all.

**What is already done and no longer needs tracking:** modifiers (all of them, plus two of ours),
14 of 16 layouts, the palette and primitive foundation, MoonLive palettes, the moving-head effect
and fixture model, and the doc model (in a better shape than this plan proposed).

## Stages

### Stage 1 — Foundations (palette + primitives + GoL re-port)

The proving-ground stage: build the shared tools, prove them on one hard effect.

- **Palette.** Take **MoonLight's palette set** (~80 gradient palettes, [palettes.h](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Modules/palettes.h) — study + carry the gradient *data*, written into our own format). The definition format is the textbook **gradient-stop** one: a compact `{position, R, G, B, …}` list (position 0..255, terminating at 255), expanded off-loop into a 256-entry lookup. Our `Palette` type + `colorFromPalette(palette, index, brightness)`: the per-light lookup is an array index + one `scale8` (hot-path-tuned; the 256-entry table precomputed on selection, not per frame). Generalises `PlasmaPaletteEffect`'s hard-coded table.
  - **Ownership (decided 2026-06-30):** the **active palette is global**, owned by the **Drivers** container (already the home of global render params — brightness, lightPreset, the shared Correction) via a new `palette` select control. Effects read it through a static `Palettes::active()` seam (the `AudioModule::latestFrame()` pattern), so an effect just calls `colorFromPalette(Palettes::active(), idx)`. This mirrors MoonLight's global `layerP.palette` without needing MoonLight's `ModuleLightsControl` — which, with **presets** and the **external-controller hub** concept, is **backlogged** ([backlog-mixed.md](../../future/backlog-mixed.md)) and will absorb the palette control from Drivers when built. Presets are *not* a palette dependency — separate feature, backlogged.
  - Palettes are light-domain → live under `src/light/` (file split decided in the stage plan).
- **The shared primitive library** (file split — one `light/Fx.h` vs focused `light/Beat.h`/`Noise.h`/`Blend.h` — decided in the stage plan; recognisable names, our implementation, FastLED credited as prior art). Hot-path-tuned, integer-only, LUT-backed:
  - *timing/beat:* `beatsin8/16`, `beat8/16`, `triwave8` (on `sin8` + `elapsed()`).
  - *noise:* `inoise8` 1D/2D/3D (promote + generalise `NoiseEffect`'s existing hash — the textbook value/Perlin noise).
  - *blend/scale:* `qadd8`/`qsub8` (saturating), `nscale8`, `fadeToBlackBy`, `blend(RGB, RGB, amt)` (`scale8` already in `color.h`).
  - *random:* `random8`/`random16` — a small fast seedable PRNG, hot-path-cheap (not `std::rand`).
  - *draw (the dimension-agnostic part the product owner called out):* `drawPixel`/`drawLine` (and later `drawCircle`/fill) operating on `Coord3D`, working **1D→3D** against the `Buffer`, so effects and modifiers share one set instead of re-rolling Bresenham per effect. This is the "core absorbs the hard part" principle — geometry primitives live once.
- **Re-port Game of Life** properly — the *real* MoonLight GoL algorithm (the cellular-automaton rules + its palette coloring + blur/mutation it actually uses), on top of the new palette + primitives, replacing the current 272-line version. This is the stage's proof: a real effect that exercises palette + random + neighbour math, done faithfully.
- **Tags/emoji legend.** Write the canonical legend (MoonLight as basis) into architecture.md § Web UI / a tags reference, and sweep existing effects' `tags()` to match. Lightweight.

Stage-1 exit: palette + primitives compile (-Werror), are unit-tested (each primitive pinned: `beatsin8` range, `inoise8` determinism, `qadd8` saturation, `drawLine` endpoints in 1D/2D/3D), GoL re-port renders correctly + has a scenario, tags legend documented. **No doc explosion yet** (GoL keeps its existing single `.md`; the doc-model change is Stage 2).

### Stage 2 — Doc model: per-library pages  ← SUPERSEDED (see Status)

**What shipped instead is one page per TYPE**, not per library: `effects.md`, `layouts.md`,
`modifiers.md`, `drivers.md`, each a table of module rows. It solves the doc-explosion problem
this stage existed for, with four pages rather than a dozen, and `check_specs.py` enforces a
row per registered module. The plan below is kept as the record of what was considered; the
per-library page names and the `registerType` remapping it describes were not built and are not
wanted. ADR-0015's conclusion still holds for
`src`/`assets`/`tests` (library is a tag, not a folder); only its doc-page half went unused.

Before migrating dozens of effects (which would create dozens of `.md`s), switch the doc model. The naming + structure is fixed by the folder-structure decision: **`src`/`assets`/`tests` are `domain/type` folders, flat — library is NOT a folder there**, only a `tags()` emoji; **docs** are the one place library splits, as a **page name** (type-first, underscore-joined, matching how you'd read the folder path): `effects_moonlight.md`, `effects_wled.md`, `effects_projectmm.md`, … (and `modifiers_<lib>.md` etc. only where a library has that type — most libraries are effects-only).

- **New per-library pages:** each effect is a **compact table row** — `| name + tags | gif | one-line description | controls |` — dropping the per-module `Tests`/`Design notes`/`Source` boilerplate (source is derivable, tests auto-discovered), so a ~30-effect page is ~120 lines (avoids both the per-module explosion *and* the one-giant-file extreme). Migrate the ~21 existing per-module effect specs into the right `effects_<library>.md` by origin (from each effect's "Prior art"/tags — see the effect inventory reference). A short index page links the set.
- **Rewrite `check_specs.py`** to the new contract: every registered module's **control names** must appear *somewhere in its library page* (preserves the anti-drift guarantee the per-module check gave). The `registerType` second arg changes from `Foo.md` to the library page (`effects_moonlight.md`, or `…#foo`).
- **Gifs:** the per-module asset *move* is **already done** (assets are now `docs/assets/{core, light/{effects,…}}/` per the folder decision). What remains for this stage: **download MoonLight's preview gifs** (the WLED-Utils `FX_*.gif` set + the user-attachment gifs listed on MoonLight's effects/layouts/modifiers/drivers pages) into the matching `light/effects/…` folders as the new effects land, crediting source.
- **Wire-contract docs** that don't fit a table row (the genuinely technical ones — HueDriver's API, NetworkSend's protocols) keep a deeper section; the library page links to it.

Stage-2 exit: the library pages render with gifs, `check_specs.py` green on the new contract, the per-module effect `.md`s deleted (subtraction). This is the "kills the explosion permanently" stage.

### Stage 3+ — Effect migration in batches

With foundations + doc model in place, migrate MoonLight effects in **themed batches**, each a stage/commit: study behaviour → write fresh on our primitives → unit + scenario test → add to `effects.md` + gif. Batching keeps each commit reviewable.

**Scope: ALL effects across MoonLight's `Nodes/Effects/E_*.h` files**, not a cherry-picked subset — the [breadth-parity gate](rename-to-moonlight%20(superseded).md) needs the full set. The source files (each an effect library, mapped to our origin sections + future per-library doc pages):
- **`E_MoonModules.h`** (MoonModules-authored, 3): **GameOfLife** (Conway, 2D/3D, rulesets/wrap/color-aging/infinite-mode), **GEQ3D** ♫ (perspective 3D equalizer bars), **PaintBrush** ♫ (frequency-modulated animated lines, chaos/softness). — verified 2026-06-30 from source.
- **`E_MoonLight.h`** (MoonLight-original geometric set).
- **`E_WLED.h`** (WLED ports/enhancements).
- moving-head / DMX effect files → Stage 5.

The batch order below is by dependency/complexity (refine per batch), and **cuts ACROSS the source files** (an audio-reactive batch pulls GEQ3D+PaintBrush from E_MoonModules and the GEQ/Blurz family from E_WLED together) rather than migrating one file at a time — themed batches keep each commit coherent:

**Superseded by the 2026-09-07 status: 3a, 3b, 3c and 3e are substantially done.** What remains,
re-derived from the two trees rather than from the original guess at themes:

- **3d — audio-reactive**, and it is now the LARGEST and CHEAPEST remaining batch: eleven effects
  (`Grav Center`, `Grav Centric`, `Grav Freq`, `DJ Light`, `Freq Map`, `Freq Pixels`, `Rocktaves`,
  `Ripple Peak`, `Waverly`, `Funky Plank`, and the `Puddles`/`Puddle Peak` partials). Our
  `AudioFrame` already exposes more than MoonLight's `sharedData`, so these are mechanical ports on
  an existing pipeline. The three `Grav*` share one small gravity/peak helper, which is the only new
  primitive the batch needs. **Recommended next batch if effect breadth is the goal.**
- **3f — geometric leftovers**: `Blackhole`, `DNA`, `Frizzles`, `Oscillate`, `Radar`, `Pixel Map`,
  `Blink Rainbow`, `Phased Noise`, `Plasmoid`, `Flow`, `Police`, `Heartbeat`. A few lines each.
- **3g — the genuinely substantial two**: `Ants` (an agent model with food-gathering rules, no
  analogue here) and `Spiral Fire` (a 3D cone-surface test on the existing `PolarLut`). Worth their
  own commit rather than being buried in a batch.
- **Not portable, do not batch**: `Moon Man` (M5GFX PNG dependency) and `FLAudio` (needs audio
  fields our analyzer does not produce). See the status section.

### Stage 4 — Modifiers + layouts migration

The MoonLight modifiers (mirror/tile/kaleidoscope/pinwheel/transpose…) and layouts (panel/cube/ring/sphere/spiral/fixture variants) not yet ported. Modifiers are pure geometry (they fit our modifier model cleanly — and per architecture.md, geometry transforms belong in modifiers, not effects). Layouts are coordinate iterators. Smaller than the effect batches; can interleave with Stage 3.

### Stage 5 — Moving heads / DMX fixtures (last)

**Split in two by what actually happened, and only half is left.**

**Done (2026-09):** the fixture model (`FixtureChannels`, the `ChannelRole` vocabulary),
`MovingHeadEffect` with formations and audio reactivity, the five role setters reachable from
both C++ and MoonLive scripts (pan, tilt, zoom, rotate, gobo), and five `mh-*.mle` scripts
carrying MoonLight's Troy / Wowi / Ambient looks. A head is drivable today over Art-Net.

**Left:** the WIRED transport. A DMX-512 output driver over RS-485 (UART, break/mark-after-break
timing, a transceiver on the board) and, if wanted, DMX input. The channel-mapping half is
already solved by the per-light channel model, so this is a transport and a hardware question
rather than a domain one: [backlog-light § RS-485](../../future/backlog-light.md) has the
analysis. **This is the one remaining Must-class item for the rename**, and since 2026-09-07 it is this
plan's alone: the Release 4 scope plan also listed it, shipped without it, and closed pointing here.
One home for it now.

## Riskiest parts

1. **Palette + primitives are the load-bearing wall** — if their API or performance is wrong, every later effect inherits it. Stage 1 must get the hot-path shape right (measure tick cost; these run per-light). Worth over-investing in.
2. **Primitive implementation is ours** — the temptation under deadline is to copy a source's implementation, not just its recognisable name. The names follow the established FastLED convention (what a contributor recognises); the *code* is written fresh against our engine, behaviour pinned by tests, FastLED credited as prior art. Guard: independence-by-construction (own implementation + own architecture), not a renamed copy and not a traced one.
3. **Dimension-agnostic draw** — making `drawLine` etc. genuinely 1D→3D (not 2D with a z-loop bolted on) needs thought; get the abstraction right in Stage 1 or effects will work around it.
4. **Doc-model migration is a one-way door** — deleting 21 per-module `.md`s and rewriting the spec-check; do it as one coherent Stage-2 change, not piecemeal, so docs are never half-migrated.
5. **GoL "done right"** — we already got it wrong once; Stage 1 must pin the real algorithm against a reference (the actual rules + coloring), tested, so it's faithful this time.
6. **Scope discipline** — "migrate all of it" is dozens of modules. The batching is what keeps it from becoming one un-reviewable mega-diff; resist merging batches.

## Verification (per stage)

Every stage: desktop build (-Werror), `ctest` (new primitives/effects pinned), scenarios, spec-check, ESP32 build, KPI (watch the per-light hot-path cost as primitives land). Bench on hardware for the visual effects. Each stage saves its own `/plan` and commits independently.

## Open questions (settle at each stage, not now)

- Exact home + file split of the primitive library (one `Fx.h` vs several focused headers) — Stage 1 plan.
- Palette storage (flash tables vs computed gradients) + how the UI selects a palette (a `palette` control type?) — Stage 1 plan.
- The `registerType` → category-page mapping mechanics — Stage 2 plan.
- Per-batch effect list + order — each Stage-3 sub-plan.
