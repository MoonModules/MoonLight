# Generative fields — top-down build spec

> **Forward-looking design document — exception to CLAUDE.md present-tense rule.** Stage 2 of the generative-fields work: turns the [bottom-up analysis](generative-fields-analysis-bottom-up.md) (Part 1 the specification language, Part 2 the examples, Part 3 the gap) into an implementable spec: homes, types, signatures, the MoonLive surface, budgets, tests, and a step-by-step plan. Written 2026-09-03 against the five product-owner decisions in § 0. Where this document makes a NEW decision it is marked **(proposal)** and listed in § 10. Companions: the [power-functions top-down](power-functions-analysis-top-down.md) owns the library's homes, types and standing decisions, which this document extends rather than restates; the [MoonLive roadmap](moonlive-language-roadmap.md) owns the engine.

## Status legend

| | Meaning |
|---|---|
| ✅ | **Done.** Built and in the tree. |
| 🔨 | **To do.** Open, with the reason stated. |
| ❓ | **Unsure.** A claim or number not yet verified; a question, not a fact. |

An unmarked line is context or rationale.

## TL;DR

- **Five decisions taken by the product owner (2026-09-03)** (the first was later reversed on measurement, § 11): the Layer buffer goes to 16 bits per channel as the one format, subject to the memory analysis in § 2; gradient noise replaces value noise in place (one solution, the goldens move once); MoonLive gets each kernel in the same phase as the compiled function, with a shipped script; the stable-fluids solver is in scope as the last phase, a P4 and desktop showcase; every effect that does not run on the power functions is rewritten on them (algorithmic effects such as Game of Life excepted), judged against how it ran before and never made worse.
- **(proposal) The Layer width is a per-layer property fixed on the cold path, one template instantiated twice, never a per-light branch.** 16-bit is the default wherever it fits; 8-bit is the degradation step the existing adaptive-allocation cascade already has, so the classic without PSRAM keeps its proven 128² pipeline. The pipeline stays 16-bit end to end and quantizes **once, per driver, in `Correction`**, to the width that driver's wire takes: 8-bit with temporal dithering for WS2812-class LEDs and every network, Hue, video and preview output, 16-bit straight through for a 16-bit LED. Memory doubles per layer and for the output buffer (98 KB each at 128² RGB), bandwidth doubles in blend, extrude and preview, and the S3's PSRAM latency makes that measurable on cheap effects; the KPI gate reads it per batch (§ 2).
- **(proposal) Seven kernels, in the existing homes.** `core/noise.h`: gradient noise in place, plus `curl16`. `light/polar.h` (new): `PolarLut`. `core/oscillators.h` (new): `Oscillators`, the timer bank with the four LFO shapes and modulation. `light/draw.h`: `advect` (the bilinear previous-frame resampler, separable and 2D, wrap and clamp), the velocity rules, `decay` by half-life, `lineAA`, `disc`, and the 16-bit quantize with temporal dithering. `light/fluid.h` (new, last): the Stam solver. Every function names its canonical source; nothing is ported (§ 3).
- **(proposal) MoonLive operates on the Layer, not on a handle.** With the Layer at 16 bits the frame buffer IS the wide state, so advection, decay and the emitters are whole-frame builtins on the script's own layer, the shape `fill` and `fade` already have; the polar LUT and the oscillator bank are handles declared in `defineControls()`, the shape `pool()` has. Multi-argument host calls already ship (`emit` takes seven), so nothing here waits on the engine; the one engine change is the pixel-store inline ops learning the 16-bit element (§ 4).
- **Budgets are the bottom-up's, restated as targets per target.** Shaders: samples per pixel against ~750 cycles per sample on the S3, ~250 on the P4; advection: cycles per frame plus bytes of state, PSRAM-class for walls. Framerate is protected by rule: every kernel is `dt`-driven, transport stays sub-pixel per frame, and a showcase must hold its stated fps on its stated fixture or its cost knob is the default (§ 5).
- **Four showcases, three plus fluid**, specified to the control level: *Aurora* (a pure shader), *Trails* (emitters and flows), *Nebula* (a shader feeding a flow), *Fluid* (the solver, P4 and desktop) (§ 6).
- **Eight phases, desktop first, bench last; the product owner decides when enough is done to commit.** Noise swap; fields and Aurora; three dimensions everywhere; Coord3D in MoonLive; the 16-bit Layer; advection and Trails; composition, dithering and SIMD; fluid. The catalog sweep that decision 5 mandates is a follow-on plan, fed by the [effects × power functions inventory](effects-power-function-inventory.md); only the effects a phase touches directly (the noise effects in phase 0, PolarNoise in phase 1, the raw-byte writers in phase 2) are rewritten inside this plan. Each phase lands with its unit tests, its goldens, its scenario, its MoonLive builtins and script, and its catalog cards (§ 8).

## 0. Inputs and the decisions taken

From the bottom-up: two techniques over one block set; every block named from its primary source; the cost model; MoonLight's three missing blocks (previous-frame resampler, color state above 8 bits, gradient noise) plus the LUT, the bank and the kernels that compose them; the per-target headroom table with the P4 and S31 as the MCU home of the family. From the power-functions top-down, carried unchanged: dimension-generic; one contract everywhere with per-target acceleration; fixed point invisible to the writer; the 16-bit contract (`angle16`, `frac16`, `pos_t` 24.8); particles as the stateful precedent; determinism (pure function of position, time, seed; stateful kernels declare a resync point); golden frames as hashes; the `Canvas` descriptor.

Product-owner decisions, 2026-09-03:

1. **Wide color state: the 16-bit Layer**, as the cleanest, least-code, fastest alternative; decided per Layer at run time from free memory with 8-bit as the fallback (option B, agreed); and **quantized once per driver in `Correction`**, so the pipeline is 16-bit end to end and a 16-bit LED is a driver, not a pipeline change (agreed). Detail in § 2.
2. **Gradient noise replaces value noise in place.** One solution; no second function kept for compatibility (CLAUDE.md minimalism).
3. **MoonLive per phase, same PR.**
4. **Stable fluids in scope**, as the last phase; if it costs CPU it is a P4 and desktop showcase.
5. **Every effect runs on the power functions for whatever they cover; an effect that does not is rewritten.** Most effects were developed quickly to demonstrate a mechanism, and keeping them rendering the same is not a goal. The rule is a mandate, not an option: an effect whose mechanism is one the library provides (motion, fields, noise, polar addressing, trails, particles, oscillators) is rewritten on the library's kernel. Bouncing balls run on `particles`; every noise-like effect runs on the noise this plan builds; every trail decays by half-life; every polar effect reads the LUT. The one exception is the **algorithmic** effect, where a rule set *is* the effect rather than a natural motion: Game of Life is the type case, and it keeps its logic and uses the library only to draw. The guard that came with "behave identically first" stays: an early Game of Life port was much worse than its MoonLight origin, so a rewrite is compared against the effect as it ran and lands only if at least as beautiful on the panel. The aim is effects at a level no other LED firmware reaches, not a 1:1 rendition of another project's catalog.

## 1. Homes and style ✅ *(the rule; the headers are new or grown)*

The power-functions rule stands: grow existing homes, one style, free functions over `const Canvas&`, fixed-point vocabulary, a doc block naming the canonical source. **(proposal)**

| Home | Gains | Notes |
|---|---|---|
| `core/noise.h` | gradient noise behind the existing `inoise8/16`, `fbm`, `warp`, `turbulence` names; `curl16` | Domain-neutral. The name promises the field; the algorithm changes underneath (decision 2). |
| `core/oscillators.h` **(new)** | `Oscillators`: N clocks, four LFO shapes each, modulation binding, one master speed, `dt`-driven | Domain-neutral: audio meters and services want the same LFOs. |
| `light/polar.h` **(new)** | `PolarLut`: `angle16` and radius per pixel, built at `prepare()`, 8-bit default with a 16-bit opt-in (the power-functions decision) | Was planned there and never built; `kaleido` stays in `math16.h`. |
| `light/draw.h` | `advect` (separable and 2D), the velocity rules, `decay`, `lineAA`, `disc`, `quantize` with dithering | The frame-ops family, which is where `scroll` and `blur` already live. |
| `light/layers/Buffer.h`, `BlendMap.h`, `Layer.h` | bytes per channel; the 16-bit format and its quantize-last | § 2. |
| `light/fluid.h` **(new, last)** | `Fluid`: velocity grids, diffuse, project, advect, vorticity confinement, dye transport | Stateful kernel with a resync point, like particles. |
| `light/moonlive/MoonLiveBuiltins_light.h` | one builtin per kernel, per phase | § 4. |

## 2. The 16-bit Layer

### What the codebase says

[MoonLight, memory strategy](../../explanation/architecture/moonlight.md#memory-strategy): all buffers are raw `uint8_t*` arrays sized `channelsPerLight × nrOfLights`; "there is no fixed channel layout: `channelsPerLight` is a runtime value, so RGB, RGBW and multi-channel DMX fixtures all use the same code path; the buffer simply gets wider". Adaptive allocation checks heap before every allocation with a 32 KB reserve, and the degradation cascade "reduces layer dimensions until the buffer fits, minimum 8×8". The architecture "does not assume PSRAM"; the classic without PSRAM is "proven up to 16 K lights (128×128 measured live on Olimex)". CLAUDE.md: minimal memory, fastest hot path, "the standard, complete construct beats a hand-rolled special case", and "no `#ifdef`, no per-light virtual call, data over objects". The LED-driver analysis already planned this as its mode 3, "16-bit pipeline (incl. dither): doubles RAM; best gradient quality; required for 16-bit-native LEDs (UCS7604, HD108)", with the driver declaring its input width and the pipeline building Layer buffers accordingly ([leddriver-analysis-top-down.md § 7.3](../../work/future/leddriver-analysis-top-down.md)).

So the width is a property the architecture already treats as runtime data (like `channelsPerLight`), the cascade already knows how to shrink a layer that does not fit, and the pipeline decision was already made in principle. What is new is doing it.

### Memory, per fixture, RGB, one layer

The last column is an **illustration** from the classic's measured free heap today (~104 KB running at 128² with mirror, Ethernet and mDNS up), not a threshold: the decision is made at run time from free memory, never from the grid size, so the same layout may run 16-bit on one device and 8-bit on another depending on what else is loaded.

| Grid | 8-bit | 16-bit | Illustration: would it fit today's measured classic heap without PSRAM? |
|---|---:|---:|---|
| 16×16 | 768 B | 1.5 KB | yes |
| 32×32 | 3 KB | 6 KB | yes |
| 64×64 | 12 KB | 24.6 KB | yes |
| 96×96 | 27 KB | 55 KB | yes, one layer |
| 128×128 | 49 KB | 98 KB | **no** as a second allocation beside the 49 KB output buffer: the reserve is 32 KB |
| 48×256 (the wall) | 37 KB | 74 KB | PSRAM already required by `nrOfLightsType` |

The driver output buffer, where one exists (two or more layers, or a mapping LUT), follows the Layer's width, so the composite case costs two 16-bit buffers; it is part of the same runtime allocation check. Advection needs a scratch copy for the separable pass (a second buffer of the same size) and, for the fluid solver, velocity, pressure and divergence grids (four more `int32` planes). Those are effect-owned `ScratchBuffer`s, allocated at `prepare()`, reported through `dynamicBytes()`, PSRAM-preferred; the memory ladder shows them exactly as it shows a particle pool.

### Can 8 or 16 be decided dynamically without hot-path cost? Yes, and it is the existing mechanism

The width is decided **per Layer at `prepare()`**, on the cold path, by the same allocation check that decides whether a LUT or an output buffer exists: the Layer attempts the 16-bit allocation against the free heap after the 32 KB reserve, keeps it when it succeeds, and otherwise takes 8-bit as the first step of the cascade (before dimension reduction). It is a measurement, not a table: no grid size is special, and the answer re-decides on every `prepare()` (a layout change, an added layer, a freed module). Every per-pixel loop that reads or writes Layer bytes is written **once as a template on the sample type** and instantiated for `uint8_t` and `uint16_t`; the choice happens **once per pass per frame** (a function pointer or a branch at the top of `blendMap`, `extrude`, `fadeToBlackBy`, `advect`), never per light. That is the "data over objects, no virtual call per light" rule satisfied, and it is one algorithm, not two implementations. The price is code size (two instantiations of the frame-op set) and a second configuration to test, which the goldens cover by running both widths.

**(proposal)** Two options, with the recommendation:

- **A. 16-bit only.** One format, the cascade shrinks a layer that does not fit. Least code and one test configuration; the classic without PSRAM loses its stated 128² full pipeline (it drops to 96² single-layer, 64² with two layers), which is a documented capability regression on a shipping board class.
- **B. 16-bit default, 8-bit as the first cascade step** (recommended). One template, two instantiations, chosen per layer on the cold path; the classic keeps 128²; the hot path pays nothing per light. Minimalism is respected in the way that matters here: one algorithm with one home, and the 8-bit path is a fallback the architecture already promises, not a second feature.

### Quantize last: once, per driver, in `Correction` (agreed)

The pipeline is 16-bit end to end: Layer → `blendMap` → output buffer, all at the Layer's width. Quantization happens **once, in `Correction::apply`, per driver**, which every driver already runs through `DriverBase` (LED, ArtNet/DDP/E1.31, PanelCard, Hue, NDI, HLS, Preview): white balance, brightness, the power limiter and the perceptual curve run at 16-bit input, and the last step narrows to the width the driver's wire declares. **8-bit with temporal dithering** for WS2812-class LEDs and every network, video and preview output; **16-bit straight through** for a 16-bit LED. This is mode 3 of the LED-driver analysis ("driver input 16-bit, wire 8-bit, the driver downsamples and dithers; required for 16-bit-native LEDs"), and its `inputBitsPerChannel()` stub becomes real: the driver declares its wire width, `Correction` quantizes to it.

Why here and not at `blendMap`: quantizing earlier throws the extra bits away before the perceptual curve, which is exactly where 8-bit input crushes the low end, and it makes a 16-bit LED impossible. One site for the whole system also means one dither state (per driver, one byte per channel of carried error, or a stateless ordered matrix) and one place to test.

Two consequences. The zero-copy path (a single layer with no LUT, the driver reading the Layer directly) needs no view: `Correction` reads the Layer's width like any other source. The `Correction` LUTs stay 256 entries with interpolation on the 16-bit input (the shape `sin16` uses), not a 64K table. In phase 2 the narrowing is `>> 8` so every existing effect renders bit-identically and no golden moves; dithering is the phase 4 switch.

**The four combinations, and the hot path.** The Layer's width (decided by memory) and the wire's width (declared by the driver) are independent facts that meet in one `Correction` template; the combinations fall out, none is a mode to maintain:

| Layer | Wire | `Correction` does | When |
|---|---|---|---|
| 16 | 16 | curve and brightness at 16, no quantization | the full pipeline: a PSRAM board with a 16-bit LED |
| 16 | 8 | curve and brightness at 16, quantize once with dithering | the common case: 16-bit fits, WS2812-class LEDs, and every network, video and preview output |
| 8 | 8 | today's path, byte for byte | a board where memory forced the 8-bit fallback |
| 8 | 16 | widen (`v · 257`), curve and brightness at 16, out at 16 | the fallback board driving a 16-bit LED; smoother dimming than 8-bit scaling, the effect's own 8-bit steps remain |

No path you have today gets slower: the 8-bit instantiations are the same code compiled for `uint8_t`, chosen once per pass per frame, so the per-light instruction count on an 8-bit Layer is unchanged. The 16-bit paths cost what they carry: twice the bytes moved (the S3's PSRAM latency shows this on cheap effects; the KPI gate reads it per batch), the same ALU ops on a 32-bit core, one interpolation per channel in `Correction` on top of the LUT lookup, and dithering as one add and one compare per channel, only on a 16-to-8 wire and only when switched on. No per-light branch, no virtual call, no allocation on the path.

**16-bit LEDs are enabled, not built.** HD108 (16-bit per channel, clocked SPI) and UCS7604 (16-bit per channel, one-wire) are the known families; APA102/SK9822 are 8-bit color with a 5-bit current gain and do not qualify. Each is its own driver increment: verify the datasheet first, then an encoder that takes 16-bit wire values from `Correction` (an SPI driver for HD108, a wider symbol table for UCS7604).

### The surface, counted

What touches Layer bytes and therefore changes in phase 2: `Buffer` (bytes per channel, 1 or 2), `Canvas` (a sample-width field), the 25 `Canvas` and 12 `Buffer` overloads in `draw.h` (one template each), `BlendMap` (identity and LUT paths, both widths), `Layer::extrude` and `fadeToBlackBy` (bytes-per-light already, width-agnostic after the template), `Correction::apply` (16-bit input, quantize to the declared wire width; the one quantization site), `DriverBase` (the declared wire width), `PreviewDriver` (quantized through the same path), `NetworkReceiveEffect` (widens), the **7 effects that write raw bytes** (AudioVolume, BouncingBalls, Fire, Lines, NetworkReceive, Rainbow, Noise), `colorFromPalette` (unchanged: returns 8-bit RGB, widened at the write), the MoonLive `StoreElem` / `FillElems` inline ops on all three backends (element = `channelsPerLight` × the Layer's bytes per channel, NOT a fixed 3 × 2: the ops already store N bytes rather than a fixed RGB, and a phase that hardcoded three channels would break the RGBW and multi-channel DMX fixtures the same buffer serves), and the zero-copy driver path. Modifiers are coordinate-only and untouched; the mapping LUT is index-only and untouched.

## 3. The kernels

Signatures are shapes, not final names; the PR is the spec. All fixed point, all `dt`-driven where time enters, all pure functions of (position, time, seed) unless stated. **(proposal)**

### 3.1 Gradient noise, in place (decision 2)

`inoise8/16(x[,y[,z]])` keep their names, their 16.0 coordinate convention and their output ranges; the implementation becomes **Perlin's improved noise** (2002): a 256-entry permutation table, gradients from the 12 edge directions of a cube (8 in 2D), the quintic fade `6t⁵ − 15t⁴ + 10t³`, integer throughout, output rescaled to the full range the header already promises. `fbm`, `warp` and `turbulence` sit on top unchanged. The seven noise effects (Noise, NoiseMeter, Noise2D, PolarNoise, Wave, Tunnel, and EffectBase's users) change look in one commit; their goldens move in that commit with the reason in the message. Under decision 5 the question on the panel is whether they look at least as good as before; any that look worse are re-tuned before the swap is accepted, since a noise effect that ran beautifully on value noise may not degrade on gradient noise. The header's note that ports from FastLED "look slightly different for a reason the author cannot see" is deleted: they no longer do.

Cost target: within 1.3× of the current value noise per sample (the gradient dot products replace the value lerps; the fade is the same), measured by the micro-bench in phase 0 before the swap is accepted.

### 3.2 `PolarLut` (`light/polar.h`)

```cpp
polar::Lut lut;                              // POD over ScratchBuffer
lut.build(dims, center, precision);          // prepare(): angle16 + radius per pixel; 8-bit each by default, 16-bit opt-in
lut.angle(i); lut.radius(i);                 // hot path: two loads per pixel, no atan, no sqrt
```

Rebuilt on geometry or center change through `affectsPrepare`; reported as `dynamicBytes`. `PolarNoiseEffect` migrates to it (pixel-identical at the 16-bit precision, golden-pinned; the 8-bit default is a deliberate, bench-judged divergence). A polar modifier consuming the same LUT is the modifier-first path the earlier decision named; it is not in these phases.

### 3.3 `Oscillators` (`core/oscillators.h`)

```cpp
Oscillators<N> osc;                          // N clocks; N is the effect's choice (4 to 20)
osc.set(i, ratio, offset);                   // per clock, at prepare() or on control change
osc.advance(dtMs, masterSpeed);              // once per frame: phase_i += dt · ratio_i · master
osc.ramp(i); osc.phase(i);                   // 32-bit ramp; angle16 wrapped phase
osc.bipolar(i); osc.unipolar(i);             // sin16 of the phase; 0..65535 form
osc.noiseAngle(i);                           // angle16 from inoise16(ramp), the wandering angle
osc.modulate(base, i, depth);                // base · (1 + depth · bipolar(i)), the multiplicative binding
```

Frame-rate independent by construction (`dt`, never a frame count, per coding-standards § Animate on elapsed time). Determinism: the phases derive from the shared time origin, so two devices agree (§ 6b of the power-functions top-down). `BeatPhase` stays for single-tempo effects.

### 3.4 Velocity rules and `curl16` (`draw.h`, `noise.h`)

A velocity rule is a function `(x, y, t, params) → (vx, vy)` in `pos_t` per frame; none is stored. The set from Part 1: `flowWind` (direction, speed, rotation rate, perpendicular wobble), `flowRadial` (in or out), `flowSpiral` (angular step, radial step), `flowRings` (three zones with smoothstep boundaries, per-zone swirl and drift), `flowNoise` (two 1D profiles, one per axis, decoupled), and `curl16(x, y, t, scale)`: the perpendicular gradient of a scalar noise potential by two central differences, divergence-free by construction (Bridson 2007). The profiles of `flowNoise` are computed once per row and column per frame, not per pixel.

### 3.5 `advect` (`draw.h`)

```cpp
draw::advect(dst, src, rule, Edge::Wrap|Clamp);            // backward sample, bilinear, per channel
draw::advectSeparable(cv, scratch, xProf, yProf, Edge);    // two 1D passes through the scratch plane
```

**Shipped (2026-09-04), with the 3D cost measured.** The signature took `dst, src` rather than
`cv, scratch` and dropped `dtMs`: the two planes must differ (reading and writing one buffer samples
pixels the same pass already moved, smearing along the walk order instead of the flow), and the rule
returns sub-pixel units per frame, so only the caller knows the cadence to scale by. `sampleWrap`
already existed and covered the Wrap half; `sampleClamp` and `sampleEdge` are new, and Clamp is what
a wind wants, since a trail leaving the panel must not reappear on the far side.

**The PO's prerequisite, that 3D awareness must not cost the 2D path, holds: 1%.** Desktop, median
of 11, every destination byte verified written: a 20x20 panel is 16.98 ns/light, the 20x20x20 cube
17.09. The z loop runs once at depth 1, so a panel pays a loop counter. In absolute terms the cube
advects in **0.14 ms**, under 1% of a 20 ms frame, so transport is cheap and the flow rules'
noise sampling is what will set the cost.

The semi-Lagrangian step (Stam 1999): for each destination pixel, `src = dst − v · dt`, read the previous frame at `src` with bilinear interpolation (four loads, three lerps per channel), write to `scratch`, swap. On the 16-bit Layer the color state is the Layer itself; `scratch` is one effect-owned plane of the same width. Partial transport (`blend` fraction) is a parameter. Budget: ~40 cycles per pixel per channel per pass on the S3 (the field's 80% share is what this is measured against).

### 3.6 `decay` (`draw.h`)

`draw::decay(cv, halfLifeMs, dtMs)`: multiply every sample by `k = 0.5^(dt / t½)`, `k` computed once per frame from a 16-bit `pow2` table (`mm::halfLifeKeep`). Framerate-independent by definition; a unit test pins `decay(2·dt) == decay(dt)²` within one LSB.

**Shipped, with a measured limit (2026-09-04): an 8-BIT plane cannot hold this decay at a high framerate, and no rounding rule fixes it.** Decaying 200 over a 500 ms half-life in 500 ms of frames, exact answer 100: truncating gives 96 at 50 ms frames, 73 at 5 ms and **0** at 1 ms; rounding gives 100, 100 and **200**, the trail frozen solid. Both are the QUANTIZATION rather than the weight, the value being re-rounded to a byte hundreds of times a second. A 16-bit accumulator holds 100/101/102. So `decay` on a byte canvas is honest at a slow cadence and `decay16` on a wide scratch plane is what a trail uses: the precision belongs in the ACCUMULATOR, not the frame buffer, which is the same conclusion phase 2 reached from the other side. The Layer's collected `fadeToBlackBy` remains the one 8-bit-per-frame decay for effects that want it; `decay` is the half-life form on wide state.

### 3.7 `lineAA`, `disc` (`draw.h`)

`lineAA` steps the segment at ~3 samples per pixel and splats each through the existing 2×2 bilinear `splat` (Wu 1991). `disc(cv, center, radius, color)` writes coverage `clamp(r + 0.5 − dist, 0, 1)` per pixel in the bounding box, using the squared-distance `coverage` path already in `draw.h`. Both additive with saturation, the particle default.

### 3.8 `quantize` with dithering (`draw.h`, phase 4)

`draw::quantize(src16, dst8, ditherState)`: `>> 8` in phase 2; in phase 4 the truncation error is carried per pixel into the next frame (one byte per channel of state, effect-owned, or a 4×4 ordered matrix with no state, selectable). Applies at every 16-to-8 boundary listed in § 2.

### 3.9 `Fluid` (`light/fluid.h`, phase 5)

The Stam 1999 solver over effect-owned `int32` Q16.16 grids: `diffuse(velocity)`, `project()`, `advect(velocity by itself)`, `project()`, then dye advected by `draw::advect16` along the stored field. Wall boundaries; `addVelocity(x, y, dvx, dvy, z)` and the dye splat are the source terms. SHIPPED with three differences: the relaxation is Gauss-Seidel rather than Jacobi (it converges in fewer sweeps for the same code, and the sweep count is the `iterations` control, default 5); vorticity confinement was not built; and the dye is the EFFECT's own 16-bit plane rather than the Layer, which is what kept the buffer 8-bit (§ 11). Declares a resync point (re-seed to rest). Sized for panels; the P4 and desktop are its home.

## 4. MoonLive: the frame kernels, per phase (decision 3)

**The Layer is the state, so the kernels are whole-frame builtins on the script's layer**, the shape `fill(r,g,b)` and `fade(amt)` already have: no handle, no arena bytes, one host call per frame. Stateful helpers that are not the frame (the polar LUT, the oscillator bank, the fluid grids) are **handles declared in `defineControls()`**, exactly like `pool(n)`: sized once, allocated by the binding outside the arena, whole-object passes per frame. Multi-argument host calls ship (`emit` takes seven), the builtin table holds 96, so nothing waits on the engine. **(proposal)**

**What shipped, against this table.** The vocabulary below is the design proposal; the names in the
code differ where implementation taught otherwise, and the third column says which. Read the
registered set in `MoonLiveBuiltins_light.h` as the authority.

| Phase | Builtins | Script shipped |
|---|---|---|
| 1 fields | SHIPPED as `osc(rate, ms, shape)` alone, which is STATELESS: two oscillators sharing a rate hold their phase relationship without a bank, so the handle and its eight accessors were not needed. `polarA`/`polarR` still compute per pixel (no LUT handle). The gradient-noise swap is invisible to scripts, as planned | `aurora.mle` |
| 3 advection | SHIPPED as `trail(1)`, `flowNoise(zoom, strength)`, `flowCurl(zoom, strength)`, `trailDecay(halfLifeMs)`, `emitTrail(x, y, z, index, bri, radius)`. The flow builtins ADVECT rather than setting a rule for a separate `advect()`, which halves the host calls; `decay` became `trailDecay` because a plain `decay` is a name scripts already declare. `flowWind`/`flowRadial`/`flowSpiral` exist in `draw.h` with no script binding; `lineAA` shipped earlier (2026-08-07) and has no script binding either; only `flowRings` is absent | `trails.mle` |
| 4 composition | SHIPPED as `fieldRate(n)`, the frame-skip lever a per-pixel script needs on a large fixture. `fieldScale` stayed compiled-only: a script cannot own the second plane it needs | `nebula.mle` |
| 5 fluid | NOT BUILT: the solver has no script binding, so `fluid.mle` is jets pouring into a curl flow through the phase-3 builtins, which looks like the effect without being it. Exposing `Fluid` to scripts is open | `fluid.mle` |

The per-pixel shader path improves without new builtins: the LUT turns `polarA`/`polarR` from a 3.5 µs square root into two loads, and the roadmap's inline-op work (§ 4c) is what removes the remaining call cost; that work is the roadmap's, not this document's.

The one engine change this plan requires: the `StoreElem` and `FillElems` inline ops lower a 16-bit element on all three backends when the layer is 16-bit (phase 2). The lowering already carries the element width as "N bytes"; the width becomes a per-binding constant.

## 5. Performance plan

**Currencies.** Shaders: samples per pixel, against ~750 cycles per sample on the S3 and ~250 on the P4 (bottom-up, Part 3), with the gradient-noise swap held within 1.3× of that. Advection: cycles per frame and bytes of state. The target is ~40 cycles per pixel per channel PER PASS, and separable advection is two passes (x then y), so a 128² RGB advect is 128·128·3·40·2 cycles: **~16 ms on the S3** at 240 MHz and **~10 ms on the P4** at 400 MHz. (An earlier draft said 4 ms and 1.3 ms, which matched neither one pass nor two: the arithmetic gives 8.2 ms for a single S3 pass. The numbers below follow from the corrected figures.) That is 60 fps on the P4 with room for the emitters, and on the S3 it is the whole frame at 60 fps, so a 128² wall runs advection at 30 fps or drops to 64² for 50. The phase-3 scenario sets the contract per target from measurement rather than from this estimate.

**Framerate as the rendering method, by rule.** Every kernel takes `dt`; no kernel takes a frame count. `advect` clamps the per-frame displacement to one pixel and reports when it clamps (the `frameTime`-style status), so a slow frame smears rather than tears. `decay` is a half-life. The oscillator bank is `dt`-integrated. A showcase's stated fps on its stated fixture is a scenario contract; when it fails, the default of its cost knob moves, not the contract.

**Levers, in the order the plan pulls them.** LUTs (phase 1); fixed point (already); cost knobs as controls with honest defaults (every showcase); field below output resolution with bilinear upscale (phase 4: a `fieldScale` control, the shader renders into a smaller scratch plane and `draw::upscale` bilinearly fills the layer); field below frame rate (phase 4: `fieldRate`, the shader updates every N frames and the oscillators still advance every frame); SIMD (phase 4: a PIE variant of the gradient-noise inner loop and of the bilinear lerp for the S3 and P4, behind the platform boundary, selected by `platform_config.h` capability flags, bit-identical to the scalar form and tested as such); FPU (a float variant of `curl16` and the fluid solver where `hasFpu`, the `raymarch.h` precedent, same bit-identical-within-tolerance test); a second core (the field update as a `multicore` producer, not in these phases).

**Where the desktop takes over.** The same effects, the same controls; the desktop as processing node feeds a wall over ArtNet or DDP. The per-target headroom table in the bottom-up is the guide: rich compositions above 32² (S3) or 64² (P4) are desktop work, and a showcase's card says so.

## 6. Showcases

Each is a compiled effect and a MoonLive script of the same look, so the script proves the builtin surface. Controls are the cost knobs and the look knobs, with defaults that hold the stated fps on the stated fixture. **(proposal)**

**Aurora** (phase 1; shader; 💫🖌️ 3D). Layers of gradient noise over a `PolarLut`, each layer's transform driven by its own oscillators; which layer wins picks the palette region, and a contrast window decides what is visible at all. SHIPPED controls: `speed`, `scale`, `layers` (1 to 4, the cost knob), `warp`, `twist`, `segments` (kaleidoscope), `contrast`, `octaves`, plus `PolarLut`'s `polarTable`, `polarTable16` and `mapping`.

**Trails** (phase 3; advection; 💫🖌️ 3D). Emitters drawn into the effect's own 16-bit plane, then advected and decayed. SHIPPED controls: `speed`, `dots` (a density, scaled by the fixture), `scale`, `persistence` (a half-life), `breathe`. The planned `emitter`, `orbit`, `flow` and `blend` controls were not built: one Lissajous walk and one flow rule proved enough, and a flow SELECT is open.

**Nebula** (phase 4; composition; 💫🖌️ 3D). A noise field, thresholded against its own measured range so most of it is black, is the emitter; a curl flow carries it. SHIPPED controls: `speed`, `scale`, `contrast`, `persistence`, `octaves`, `fieldScale` (compute the field at half or quarter resolution), `fieldRate` (recompute it every N frames).

**Fluid** (phase 5; solver; 💫🖌️ 3D; P4 and desktop). Jets pour velocity and dye into a Stam solver, one independent medium per depth slice. SHIPPED controls: `jets` (1 to 4), `force`, `swirl`, `viscosity`, `persistence`, `iterations` (the pressure solve, and the cost knob). `angle`, `vorticity` and `gravity` were not built.

Each showcase gets its card in [effects.md](../../moonmodules/light/effects.md) in the PolarNoise form (one line per control, the cost knob named as such), and its numbers in [performance.md](../../reference/performance.md).

## 7. Tests and bench criteria

- **Unit, per kernel, behavior-named.** Gradient noise: zero-mean over a large sample, range, continuity across cell boundaries, the same value on every ISA (a fixed-seed table of 64 samples). `PolarLut`: the LUT equals per-pixel `atan16`/`dist16` at 16-bit precision; the 8-bit default within one LSB of angle. `Oscillators`: `dt`-independence (two steps of `dt` equal one of `2·dt`), wrap, modulation bounds. `advect`: a single lit pixel moves exactly `v·dt` (sub-pixel, so two neighbors share it by the bilinear weights), wrap and clamp edges, the separable form equals the 2D form for an axis-aligned field, no write outside the buffer. `decay`: `decay(2·dt) == decay(dt)²` within one LSB, and a value never rises. `curl16`: divergence of the sampled field is zero within tolerance on a grid. `lineAA`/`disc`: total coverage of a disc equals its area within 2%; a line's coverage is independent of direction within 5%. `quantize`: `>> 8` bit-identical to the 8-bit path; dithering averages to the 16-bit value over 256 frames. 16-bit Layer: every `draw::` primitive tested at both widths.
- **Goldens.** A golden pins the plumbing, not the look (decision 5): phase 2 moves none, because a bit-identical quantize is the proof that widening the Layer is mechanical; phase 0 and phase 6 move goldens deliberately, with the reason in the commit and the product owner's comparison against the old frame on the panel, and that is the harness working. Each showcase adds its golden at both widths.
- **Scenarios, per family.** Fields: a `PolarLut` sized at prepare and rebuilt on a grid resize, with the memory ladder. Advection: Trails on a live pipeline at 16², 32², 64², 128² with the scratch plane in the ladder and the tick within its per-target contract. The 16-bit Layer: the existing `scenario_Layer_memory_1to1` and `scenario_MultiplyModifier_memory_lut` at both widths, and the cascade stepping from 16 to 8 bits on a constrained heap. Fluid: its grids in the ladder and its tick contract on the P4. Every showcase's fps target is a `contract` in its scenario. As shipped the contracts are
`desktop-macos` only and expressed in `tick_us`; no device contract exists yet, because the S3 and P4
numbers have not been taken (§ 8, phase 3 step 5 and phase 5 step 3).
- **Perf.** The micro-bench target from the power-functions plan gains a row per kernel; `collect_kpi.py --commit` per phase on the S3 and P4; the desktop tick is the fast alarm.
- **The final gate is the product owner's eyes.** Per phase: the S3 bench board (shiffy, 80×48) for the panel class, the P4 (.139) for the MCU home, the desktop at 128×96 for the ceiling, and a wall run for Trails and Nebula. The criteria are visual: no tearing at the stated fps, no visible steps in a trail's tail, a shader that turns rather than scrolls, and the classic at 32² still smooth.

## 8. Step-by-step implementation plan

The steps below are work order, not commit order. Each step is finished on the desktop first, then the S3, then the P4, then the wall where it applies, and the product owner tests as steps land. **When to commit, and how much rides in one commit or PR, is the product owner's call** ([CLAUDE.md § Commit](../../CLAUDE.md#commit)): the pre-commit and pre-merge gates cost real time, so a commit carries several steps and a PR carries several phases. Nothing here implies one PR per phase. Order chosen so the first showcase lands before the one cross-cutting change.

### Phase 0: measure, then swap the noise (small)
1. Add the kernel rows to the micro-bench target: value noise as it is, per sample, on desktop; record.
2. Implement Perlin improved noise behind `inoise8/16` (2D and 3D; 1D as the 2D with y = 0), integer, same coordinate convention, same output ranges. Delete the "value noise" note in `noise.h`.
3. Run the micro-bench: accept only within 1.3× per sample; else optimize (the 8-gradient 2D form, a 12-entry gradient table) before continuing.
4. Desktop build, `ctest`: the seven noise goldens fail as expected; update them in the same commit with the reason.
5. On the product owner's go-ahead (CLAUDE.md § Build: a board is written to only when they say so), flash S3 and P4; `collect_kpi.py --commit`; they judge Noise, PolarNoise and Tunnel on the panel. Catalog cards: no text change unless the look note in a card mentions value noise.

### Phase 1: fields and Aurora (medium)
1. `core/oscillators.h` with its unit tests.
2. `light/polar.h`: `PolarLut` with 8-bit default and 16-bit opt-in; unit tests against `atan16`/`dist16`.
3. Migrate `PolarNoiseEffect` to the LUT (golden-pinned at 16-bit; the 8-bit default is a deliberate move judged on the panel) and to the bank for its drift.
4. `AuroraEffect`: the three-layer shader, controls per § 6, card in effects.md, golden.
5. MoonLive: `polar()` and `oscillators()` handles with their builtins; `polarA`/`polarR` read the LUT when declared; `aurora.mle`; the compile-every-script test and a script golden.
6. Scenario: fields (LUT rebuild on resize, memory ladder), Aurora's fps contract per target.
7. Desktop first; then S3 and P4 on the product owner's go-ahead; performance.md rows; their look.

### Open from phase 1

**Aurora hitches on the desktop and not on the S3 (2026-09-04).** The product owner sees a periodic
hitch in the desktop preview; the same firmware on an ESP32-S3 at 64x64 looks smooth, and the S3's
LEDs were not yet judged. Five causes were tested and ruled out with measurements:

| ruled out | the measurement |
|---|---|
| an integer overflow | every expression stays in range at every control setting, checked by hand |
| the drift sawtooth wrapping | worst single-frame field change at a wrap is 7 of 255, no spike |
| the field moving too fast | per FRAME the desktop moves 15x LESS than the S3 (0.009 against 0.136 noise cells), so it should read smoother |
| the contrast window's per-frame easing | its absolute worst frame is SMALLER at 385 fps (17.6) than at 25 fps (30.2) |
| the preview transport at a large grid | the product owner set the desktop to 64x64, matching the S3, and the hitch stayed |

What is known: brightness changes are distributed rather than discrete (median 17 per frame, 99th
percentile 101), so there is no single glitching frame; and the desktop renders at ~385 fps against
the S3's 25, which is the one large difference left standing. The next thing to try is a frame-rate
cap on the desktop: if the hitch tracks the render rate rather than the wall clock, something in the
pipeline is per-frame where it should be per-unit-time, and the effect is not the place to look.

**Aurora's layer crossover (2026-09-04).** With a winner-takes-all palette index, 12.7% of pixels
change which layer owns their color every frame, and 35% on the worst frame; each change moves the
hue a whole palette region. A weighted crossover (cubed weights, so a dominant layer still
dominates) removes the discontinuity and measured 2.7x less frame-to-frame change, for 3.4% more
time per pixel (42.8 against 41.4 ns on the host). Built, measured, then REVERTED on the product
owner's call: it did not address the hitch above, and the cost is real. Revisit when the hitch is
understood, since the two may be related.

### Phase 1b: three dimensions everywhere (medium)

Dimension-generic is a stated input of this plan (§ 0), and the library is not there yet. The gap is
uneven rather than absent, and the hot-path worry it raises turns out not to be the blocker.

**Where it already holds.** `draw::` is `Coord3D` throughout (pixel, line, blendPixel, addPixel,
glyph). `blur` runs a third pass behind `if (z > 1)`, so a 2D layout pays nothing for the capability.
`inoise8` and `inoise16` have real 1D, 2D and 3D forms, each compiling to its own straight-line body
over exactly its corners after the phase 0 swap.

**Where it does not.** `fbm16`, `turbulence8` and `warp8` stop at 2D; only `fbm8` has a 3D overload.
`PolarLut` stores angle and radius on a plane, with no z. `atan16` and `dist16` are planar by nature.

**A 3D-capable kernel costs a 2D layout nothing**, and the tree already shows both ways to get that:
the third axis is either a template parameter that compiles away (`if constexpr (Dims > 2)` in the
noise core) or a runtime guard on a value that is 1 (`blur`). Neither is a per-pixel branch. So the
missing overloads are mechanical.

The one that is not mechanical is **polar in three dimensions**, and the answer is a control rather
than a decision. Spherical (two angles and a radius), cylindrical (an angle, a radius and a height)
and distance-from-center are three different looks, and which one is right is a property of the
FIXTURE: a sphere wants the first, a tube or curtain the second, a cube may want any of them. The
choice costs nothing per pixel because it is made once when the table is built, and the table is the
same size either way. Cylindrical is the default: it reduces to exactly today's behavior when depth
is 1, so no existing 2D fixture changes.

**What "dimension-generic" does NOT claim.** The KERNELS take any arity; an EFFECT still looks good
only at the arities its design has. Aurora on a strip is one radial line through the field, which
reads as a slow flicker rather than as curtains, and no amount of library work changes that. The
Layer already handles the mismatch (a D2 effect on a 3D layer has its z=0 slice copied across z, a
D1 effect its column copied across x), so an effect that declares less than the fixture is correct
rather than broken. Which effects earn a genuine 1D or 3D form is per-effect judgment, made in the
catalog sweep, not a property this phase delivers.

1. `fbm16`, `turbulence8` and `warp8` gain their 3D overloads, matching `fbm8`'s existing shape.
2. `PolarLut` gains a z axis and a `mapping` control (cylindrical default, spherical, radial), with
   the 2D path bit-identical to today's at depth 1.
3. The effects that are `Dim::D2` only because their kernels were: Aurora, PolarNoise and Tunnel are
   the three that a volumetric fixture would show something new, and all three are blocked on step 2
   rather than on their own code. Noise2D needs no polar work at all: it already samples 3D noise
   with time on z, so a volumetric form is passing the light's own z instead.
4. ✅ **Noise2D and Noise were the same effect**, and are now one: Noise
   already uses the light's real z when depth > 1, and the two differ only in control names and
   scale defaults (`bpm`/4 against `speed`/64). Merging them is a catalog decision with a golden and
   a card, so it was done here rather than deferred: `motion` chooses drift (the old Noise) or morph
   (the old Noise2D), the drift golden is unchanged, and the second effect is deleted.
5. Unit tests per kernel that a 2D call is exactly the 3D call with z held at zero, which is what
   makes the extension safe to make everywhere else. ✅ *(shipped: `fbm8`, `fbm16`, `turbulence8` and
   `warp8` all have 3D forms, and `warp8`'s 2D form is now literally the 3D one at z = 0, so the two
   cannot drift apart.)*

### Phase 1c: Coord3D in MoonLive (medium)

A script computes a position as three loose integers today, and flattens it by hand
(`mod(bx+dx, width) + mod(by+dy, height) * width`), which is the buffer layout leaking into every
effect. The engine already passes `Coord3D` everywhere, so the script vocabulary is the odd one out.

This is **not** general struct support. The MoonLive roadmap settles that: a predefined struct is one
the compiler knows by layout, needing no user-declarable struct machinery
([roadmap § 4b](moonlive-language-roadmap.md), with user structs deferred to § 10 for readability
rather than capability). It depends on the multi-value call ABI (§ 2 there), which is what makes a
coordinate in and a color out expressible at all.

**`Coord3D` yes; the color struct waited for phase 2, which was reversed (§ 11), so the dependency is void and the struct is its own question.** The roadmap pairs `Coord3D` with a `CRGB`,
and that pairing predates the 16-bit decision. A script-visible color fixed at three `uint8_t` would
be the one place in the pipeline where the channel width stops being runtime data, exactly as phase 2
makes `draw.h` a template over both widths and has `Correction` quantize once at the driver. So the
color type is specified AFTER phase 2 knows what it is, and it takes our own name rather than
FastLED's (CLAUDE.md: our own code, our own names). A coordinate has no such dependency, which is
why it goes first.

**How far it goes: as far as every other type.** A predefined struct that only appears in builtin
signatures would be a special case, and the language does not need another one. `Coord3D` is a
class member, a local, a function argument and a return value, the same as `int`, `byte`, `bool` and
`fixed`. Arguments and returns are roadmap § 6, which this step therefore depends on. Two
consequences worth stating: a member costs three of the 8 member records and 6 of the 64 arena bytes
unless the compiler packs it as one record, which is worth deciding when the type is added rather
than after; and a local costs one frame slot per field under today's flat allocator, so a script
holding several coordinates meets the 16-slot ceiling faster (§ 8b there).

Ordering note: this is the step that makes a 3D script possible, so it follows phase 1b rather than
leading it. A script that can hold a coordinate but calls kernels that ignore z has gained nothing.

### Phase 2: the 16-bit Layer: BUILT, MEASURED, REVERSED (2026-09-04)

**Decision: the wide frame buffer is not the way to precision; phases 3 onward are 8-bit with
effect-owned 16-bit planes.** The whole arc, the measurement that reversed it, what replaced it and
what is worth salvaging: [§ 11](#11-the-16-bit-question-what-it-was-both-answers-and-what-is-left).


### Phase 3: advection and Trails (medium)

**Volumetric from the start (PO, 2026-09-04).** What shipped is per-slice: every slice gets its own
flow and they differ, but `advect16`'s rule yields vx and vy only, so light is carried WITHIN a slice
and not between them. The trilinear sampler and the vz below were not built, and 3D transport is
open. TrailsEffect is `Dim::D3`, as Aurora is: the bench
S3 is a 20x20x20 cube, so a flat trail would be the thing on the wall. Per function: `decay` and
`decay16` are already dimension-blind (they walk the buffer flat); `advect` needs a TRILINEAR
sampler for 3D, since `sampleWrap` takes `pos_t x, y` with a whole-pixel z, which is 8 corner reads
instead of 4 plus a `vz` from the rule; `flowWind`, `flowRadial`, `flowSpiral` and `curl16` all have
3D forms (3D curl is a cross product of gradients, not one perpendicular) while `flowRings` stays
planar; `disc` becomes a sphere through `sdSphere`. The effect is written 3D-aware from the start
because `dimensions()` and the plane sizing are decided once, but `advect` ships 2D first and gains
the trilinear path in step 4, so step 3 still reaches the wall early. **Memory is the constraint a
volumetric trail meets first**: a 16-bit RGB plane is 48 KB on a 20-cube and 1.5 MB on a 64-cube
(PSRAM only), against 24 KB for a 64x64 panel.

1. ✅ `advect`, the velocity rules, `curl16`, `decay`, `disc`, with their unit tests. Two parts of the step did not ship: the SEPARABLE form of `advect` (the 2D and 16-bit forms are what exist), and `lineAA`, which turned out to be already in `draw.h` from 2026-08-07.
2. ✅ `TrailsEffect`: emitters, flows, the bank for breathing, scratch plane in `ScratchBuffer`, card, golden.
3. ✅ MoonLive: the flow, advect, decay and emitter builtins; `trails.mle`.
4. ✅ Scenario: `scenario_Trails_ladder`.
5. Desktop ✅ and the product owner's look ✅. S3, P4 and the wall are open, and performance.md has no Trails row yet.

### Phase 4: composition, dithering, levers, SIMD (medium)
1. ✅ `quantize` with temporal and ordered dithering; tests.
2. ✅ `fieldScale` and `fieldRate` (`draw::upscale16`, and a frame counter the binding owns).
3. ✅ `NebulaEffect` and `nebula.mle`; card and golden. No Nebula scenario: the Fluid one covers the shared field machinery, so it was not worth its own.
4. The PIE variants of the noise inner loop and the bilinear lerp for S3 and P4 behind `src/platform/`, bit-identical tests against the scalar forms; the FPU `curl16` where `hasFpu`.
   - **The P4's PPA is for `upscale`, not for advection (2026-09-04).** The Pixel Processing
     Accelerator is a 2D-DMA blitter: it transforms a whole rectangle by ONE affine transform
     (scale, rotate, translate, blend) with no CPU in the loop. Advection displaces every pixel by
     its OWN velocity, so the interesting rules (`flowNoise`, `curl16`) do not map onto it at all,
     and the ones that would (a rigid scroll, `flowRadial`) are the cheap cases anyway. Where it
     does fit is `fieldScale`'s bilinear `draw::upscale` above, which is exactly a scaled blit, and
     the sprite blit already backlogged under [Sprite follow-ups](backlog-light.md). Both are P4
     only, so PPA can be an acceleration behind an existing signature, never the main path.
5. KPI per target; the product owner's look on the wall.

### A scripted control's name can disagree with the file that declares it (2026-09-04, open)

Reproduced on the desktop with `Nebula.mle`, and the three facts contradict each other:

- the module's `script` control holds `Nebula.mle` (so the UI is NOT lowercasing the selection);
- `GET /api/file?path=/moonlive/Nebula.mle` returns a file whose line 20 is `addControl("rate", ...)`;
- the UI renders that slider as **`rate1`**, a name the file no longer contains anywhere.

So the compiled program disagrees with its own source. `resolveScript` is exact-match and prefers
the user's copy, which is correct, and the picker sets `option.value` to the exact filename, so
neither is the cause. The `rate1` text is a name the file held EARLIER in the session, which points
at the control list surviving a recompile rather than at the resolver or the UI.

**Ruled out by measurement, so a future look need not repeat it**: the built-in table has room (67 of
96); `kMaxCtrls` is 8 against 5 declared; the 64-byte arena holds five `byte` members; declaration
ORDER is irrelevant (moving the control first changed nothing); the control NAME is irrelevant
(renaming it changed nothing); `aurora.mle` and `trails.mle` have the identical shape and are fine.

**Where to start**: `rebuildControls()` after a recompile, and whether a declared control that
vanishes from the script is cleared from `ControlList` or merely left behind. A test that compiles a
script, edits one control's name, recompiles and asserts the OLD name is gone would pin it.

**A second, smaller thing this exposed**: `/moonlive/Nebula.mle` and `/.moonlive/nebula.mle` are two
files on the device and one on a case-insensitive desktop, and nothing stops a user creating a
capital-N file beside a lowercase catalog entry. The fork mechanism assumes the two names match.

### Phase 5: fluid (medium; P4 and desktop)
1. ✅ `light/fluid.h` with its unit tests (divergence after `project` within tolerance; a jet moves dye; rest state is stable).
2. ✅ `FluidEffect`; card, golden, and `scenario_Fluid_solver` (host; a P4 run needs a board). `fluid.mle` ships too, but NOT on the solver: the plan's `fluid`/`fluidJet`/`fluidStep` builtins were not built, so the script is jets pouring into a curl flow, which looks like the effect without being it. Exposing the solver to scripts is open.
3. ✅ performance.md carries the desktop rows; the P4 and S3 rows are open, since they need a board.

**Two bugs the jets taught, both worth keeping.** The first shipped picture was a hollow RING, and
the cause was the forcing rather than the solver: jets pinned to one circle at `w/3`, all sweeping
the same direction, all aimed purely tangentially. Measuring speed by radius showed the energy in a
band (1.35 at r5 against 0.33 at the center) while `|grad v|` was a healthy 0.30 per cell, so the
medium was working and being asked the wrong question. The fix is three-part: the radius breathes
between center and wall, the aim leans either side of the tangent, and alternate jets sweep against
each other so they collide, because colliding jets are what roll up vortex pairs. After it, the peak
sits mid-panel (0.78 at r2) and the outer band halves.

The second: a purely time-paced pour never fires on the opening tick, because `dt` is deliberately 0
there, and on a fast device it then takes many frames to owe a whole period. The panel stays black
in the meantime and every resize repeats it. The first pour is now unconditional. The scenario's
`buffer non-zero after render` check is what caught it, at 16×16.

### After the phases: the catalog sweep (large; its own plan)
Decision 5 mandates that every natural-motion effect runs on the power functions. That sweep is not a phase of this plan: it is sized and sequenced in its own plan once the kernels exist, from the [effects × power functions inventory](effects-power-function-inventory.md), which records for each of the 58 effects what it uses today and what it could use. Two rules carry over: each rewrite is compared on the panel against the effect as it ran and lands only if at least as beautiful, and an effect a showcase supersedes outright is deleted, the way a particle-system effect replaces its non-PS twin.

## 9. Resource accounting

- **Flash:** the gradient noise is a 16-entry gradient table (Perlin's twelve cube-edge gradients padded to sixteen so the index is a mask) and NO permutation table: the corner hash is a multiply, which is what made it branch-free on Xtensa; the LFO bank and LUT are small; the 16-bit Layer instantiates the frame-op set twice (estimate low single-digit KB per target ❓, read from the repo-health flash table per batch); the SIMD variants are per-target only.
- **RAM:** all sized buffers are `prepare()`-time, PSRAM-preferred, `dynamicBytes`-reported, zero static (`check_footprint`). As shipped: `PolarLut` 2 B/pixel default; a wide-plane effect carries two 16-bit planes plus a 1-byte dither carry, so 15 B/light on top of the Layer's 3 (5 bytes per channel, three channels) (measured, Trails at 64x64x10: 614,400 B against the Layer's 122,880); the fluid holds two velocity grids sized to the volume plus four working grids sized to ONE SLICE (`int32` throughout). The 16-bit Layer's row is gone with the decision (§ 11).
- **Cycles:** budgets in § 5; the KPI gate per phase; the desktop tick as the fast alarm.
- **Repo:** goldens are hashes; scenarios record their observations; each phase adds ~1 effect, ~1 script, ~1 scenario.
- **Boundary:** `noise`/`oscillators` core; `polar`/`draw`/`fluid` light; SIMD and FPU variants only under `src/platform/`, selected by `platform_config.h` flags, never an `#ifdef` outside.
- **Complete construct, real consumer:** every kernel lands with its showcase and its script in the same PR.

## 10. Decisions for sign-off

1. ↩️ **Layer width mechanism: B** (16-bit default, 8-bit as the first cascade step, decided per Layer at prepare from free memory, one template). Agreed 2026-09-03, BUILT, then REVERSED on the measurement (§ 11): the buffer is 8 bits and precision is held in effect-owned planes instead.
10. ⬜ **Quantization lives in `Correction`, once per driver, to the driver's declared wire width**; the pipeline is 16-bit end to end and 16-bit LEDs become driver increments. Agreed 2026-09-03, NOT built: it followed from decision 1, which was reversed. Quantization lives in `draw::blit16`, at the one point a wide plane meets the 8-bit buffer.
2. **Gradient noise swap accepted on the 1.3× per-sample bound**, goldens moved in one commit, judged on S3 and P4 (§ 3.1).
3. **MoonLive kernels operate on the script's Layer; LUT, bank and fluid are handles** (§ 4).
4. **Phase order**: noise, fields and Aurora, the 16-bit Layer, advection and Trails, composition, fluid (§ 8). Alternative: the 16-bit Layer first, if the cross-cutting risk should be retired before any showcase.
5. **Showcase names and targets** (§ 6): the fps-per-fixture numbers become scenario contracts on acceptance.
6. **`PolarLut` 8-bit default** (the earlier decision, restated because Aurora's angular resolution on a 128² wall may want 16-bit as its default) ❓.
7. **Temporal dithering default**: on for 16-bit layers once phase 4 lands, or off with a per-driver switch (the leddriver analysis's per-driver mode) ❓.
8. **Fluid on the S3 and classic**: runs with its card stating the panel size it holds, or hidden behind a capability flag. Recommendation: runs, honestly labeled.
9. **Two documents still state the old stance and need the PO's edit under decision 5**: [power-functions.md § Migrating an effect](../../moonmodules/light/power-functions.md) ("Step 1, the port: behave identically") and the header of `test/unit/light/golden_frame.h` ("pixel-identical by default"). Proposal: "faithful first" stays, and its purpose is stated: it guards against degrading an effect into a quick-and-dirty one (the early Game of Life port), not against improving it. Two sentences follow it: every effect runs on the power functions for whatever they cover, and an effect that does not is rewritten on them, with the effect as it ran as the reference and "at least as beautiful on the panel" as the bar; algorithmic effects (a rule set is the effect) keep their logic. The golden is re-baselined in the same commit with the reason, and the harness comment reads "a golden pins the plumbing; a deliberate re-baseline records an improvement the product owner judged".

## 11. The 16-bit question: what it was, both answers, and what is left

The plan opened with a decision to widen the Layer buffer to 16 bits per channel (§ 10, decision 1).
That was built, measured, and reversed, and the precision problem it existed to solve was then solved
a different way. This section is the whole arc, because the reversal is the most useful thing the
work produced and a stash is a poor place to keep a lesson.

### The problem is real, and it is narrower than it looks

An 8-bit channel multiplied by slightly less than one, many times a second, has nowhere to go. A
decay to a half-life of two seconds at 60 fps wants to multiply by about 0.994 per frame: at 8 bits
a value of 100 either truncates to 99 and keeps truncating, reaching 0 far too fast, or rounds back
to 100 and never fades at all. The tail either vanishes or freezes, and neither is a tail.

That is the case that motivated the phase. The important observation, which took building the wrong
thing to see, is WHERE it happens: in a plane that DECAYS, frame after frame. The Layer buffer is
written fresh by every effect on every frame, so its precision is a display question, not an
accumulation one. Banding on a buffer is a dither problem. Banding on a decaying plane is a width
problem.

### Answer A: widen the Layer (built, measured, reversed)

Phase 2 made `Buffer`, `Canvas` and every `draw::` primitive width-templated, taught every driver and
both preview paths to read at the layer's width, and kept every golden matching through a narrowed
hash. It worked. Then it was measured:

| | 8-bit | 16-bit | cost |
|---|---|---|---|
| ESP32-S3, 128x128 | 18.25 ms | 28.58 ms | **+56.5%** (effect +50%, driver read +63%) |
| desktop arm64, 128x128 | 0.48 ms | 0.76 ms | +57.9% |

Two architectures that usually disagree agreed within 1.5 points, which is what makes the number
trustworthy. **On the S3 the DRIVER READ was the larger half.** Reassembling two bytes into a channel
happens `lights x channels` times per frame whatever the effect does, and an in-order core with no
branch predictor punishes it. The 8-bit dispatch itself cost nothing measurable, so the work was
correct; it was aimed one layer too high.

### Answer B: widen the plane that accumulates (shipped)

Trails, Nebula, Fluid and the scripted trail each own a 16-bit plane, advect and decay it at full
width, and narrow ONCE on the way out through `draw::blit16` with temporal dithering. The buffer
stays 8 bits, so the driver read is untouched, which is the entire +56% avoided.

Measured on the desktop at 64x64x10, the same grid for both:

| | tick | dynamic bytes |
|---|---:|---:|
| Trails (two 16-bit planes + carry) | 1913 us | 614,400 |
| Noise (8-bit, no state) | 920 us | 0 |
| the Layer buffer itself | | 122,880 |

**The cost moved to memory, and it is larger per effect than the wide buffer would have been**: 5
bytes per light (two 16-bit RGB planes plus a 1-byte dither carry) against the 3 the Layer holds, so
2.5x what a 16-bit buffer would have cost. Three properties make that acceptable where the buffer was
not: only effects that need it pay, it is `prepare()`-time and PSRAM-preferred, and it is freed when
the effect is removed. A 64-cube would want 3.75 MB for the set (two 1.5 MB planes plus the carry), which is a PSRAM-class effect and the
cards say so.

### What to learn from it

**Measure the whole path, not the kernel.** The effect-side cost of a wide buffer was the obvious
half and the smaller one. Nobody predicted the driver read, and no host benchmark would have found it
either: it is an in-order-core property.

**Precision is not a pipeline property, it is a per-consumer one.** The plan reasoned "the pipeline
should be 16-bit end to end", which is a tidy sentence and an expensive design. What the code needed
was 16 bits in the four places that accumulate.

**A reversal is worth more than the feature.** Phase 2 cost a day and produced a number that will
stop this being proposed again for years. That is why the numbers live here rather than in a commit
message.

**Building it was the only way to measure it.** The estimate before the work was "some overhead"; the
answer was +56%. Neither number could have been argued to.

### Follow-ups, in the order they are worth doing

1. **Tag the stash, or lose it.** The phase-2 work is 65 files, +1826/-362, in `stash@{0}` and
   NOWHERE else: no branch, no tag. A stash does not survive `git stash drop`, a fresh clone, or a
   dead disk. `git tag phase2-16bit-layer stash@{0}` costs nothing and makes "recoverable if the wall
   says otherwise" true. Product owner's call, since it is a git operation.
2. **Cherry-pick `Correction`'s dithering** (`src/light/drivers/Correction.h`, +158 in the stash).
   Decision 10 put quantization in `Correction`, once per driver, at the driver's declared wire
   width. That decision followed from the wide buffer and so was never built, but the DRIVER-side
   half stands on its own: an 8-bit buffer dithered at the wire is the cheapest remaining win for
   banding on a real panel, and it is the one piece of phase 2 whose cost does not scale with lights
   x channels.
3. **The five shared-primitive bugs the phase found are still real** under any wide buffer: `fill`,
   Canvas `fade`, `blur`, `Layer::extrude` and `effectSetChannel` all assume one byte per channel.
   They are correct at 8 bits, so nothing is broken today, but they are a trap for the next wide
   thing. Worth a comment at each site rather than a fix.
4. **16-bit LEDs remain unanswered.** Some drivers can carry more than 8 bits to the wire. With the
   buffer at 8 that is now a driver-increment question rather than a pipeline one, which is a smaller
   and better-shaped problem than the plan assumed.

## Out of scope

16-bit-native LED drivers (HD108, UCS7604): enabled by the per-driver quantization, each its own increment starting with the datasheet; a polar modifier on the LUT (modifier-first path, its own increment); the second-core field producer; 3D variants of the velocity rules and the LUT (the dimension audit's "waits for a consumer" rule); a MoonLive per-pixel inline-op program beyond the LUT loads (the roadmap's § 4c work); GPU acceleration on desktop; supersync's protocol (each stateful kernel declares its resync point, no more).
