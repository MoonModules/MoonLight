# Generative fields — bottom-up analysis

> **Forward-looking research document — exception to CLAUDE.md present-tense rule.** A Stage-1 bottom-up analysis of the effect family this document names *generative fields*: images computed from noise fields over a coordinate mapping, either fresh every frame (a shader) or by transporting the previous frame through a velocity field (advection). Part 1 states what we want to build in the terms of the field's primary sources. Part 2 lists implementations that do similar things and where each fills in a spot. Part 3 places the family in MoonLight's architecture: what exists, measured, and what is missing. The **top-down** companion (to be written from the prompt at the end) turns Part 3 into the implementation spec. Written 2026-09-03. Modeled on [power-functions-analysis-bottom-up.md](power-functions-analysis-bottom-up.md), which this extends: the fields, shader and particle families it cataloged are taken as read.

## TL;DR

> **Status (2026-09-05): the three gaps this document names are closed.** Advection shipped as
> `draw::advect`/`advect16`, gradient noise replaced value noise behind the same names, and color
> state above 8 bits shipped as effect-owned 16-bit planes rather than a wide Layer (the top-down's
> § 11 records why). The "What we need to add" list in Part 3 is therefore built, and the builtin
> count below (55) is now 67. The rest of the document is a Stage-1 snapshot and is left as written.


- **The target is two textbook techniques over one block set.** A **procedural shader**: color as a function of (position, time), computed per pixel from noise sampled through a coordinate transform. **Advection**: the previous frame transported by a velocity field and decayed, with sources drawn into it each frame. Both stand on the same primitives: a polar coordinate mapping, gradient noise with fractional Brownian motion and domain warping, low-frequency oscillators, a contrast window and palette, anti-aliased sub-pixel rasterization, a bilinear resampler, framerate-independent exponential decay, and fixed-point arithmetic with quantization last (Part 1).
- **Every algorithm has a name and an originator.** Perlin noise (1985, 2002), fBm, domain warping (Quilez), semi-Lagrangian advection and stable fluids (Stam 1999, 2003), curl noise (Bridson 2007), flow-field particle tracing (Hobbs; Shiffman), Wu's anti-aliased lines (1991), coverage from signed distance, exponential half-life decay, LFO modulation. None needs more than adds, multiplies, a sine table and a noise function (Part 1).
- **The cost model is arithmetic and decides the design.** A shader costs samples per pixel and scales with area × samples; advection costs a few loads and lerps per pixel per pass and scales with area × channels, plus memory for color state above 8 bits. For both, framerate is part of the rendering method: transport must stay sub-pixel per frame, decay must not step, oscillators must not alias. Throughput in pixels per second, not frames per second, is the honest metric (Part 1 § Cost).
- **The field has converged on the same shapes.** A polar-noise shader engine with an oscillator bank and a coordinate mapper; an emitter-plus-flow advection engine with six flow types, a modulator bank and a half-life fade; both now available in Q16.16 fixed point with bit-exact tests, on the ESP32 class we target; anti-aliased canvases; wave and fluid solvers; particle-trail systems. These are similar to what Part 1 describes, not identical, and each fills in a spot: measured throughput, parameter vocabularies users expect, fixed-point choices that work, and precision decisions (Part 2).
- **MoonLight has most of the blocks and lacks three.** Present: polar addressing, 16-bit value noise with fBm/warp/turbulence, 16-bit oscillators, palettes, a GLSL-vocabulary shader runner, SDF coverage, a 24.8 sub-pixel splat, particles, a persisting Layer buffer, and 55 MoonLive builtins that already express a polar-noise pixel. Missing: **a bilinear resampler of the previous frame** (so no advection), **color state above 8 bits** (so trails posterize and sub-integer accumulation is impossible), and **gradient noise** (value noise reads coarser at low frequency). The MoonLive gap is structural: per-pixel host calls cost ~5 µs per pixel measured, and a script has no frame of state; both families want whole-frame kernels a script composes (Part 3).
- **The ESP32 budget is known.** ~293 cycles per pixel at 128×128 @ 50 fps on 240 MHz; one noise sample per pixel measures ~750 cycles on the S3 today. A rich shader is a panel-class effect on any MCU; advection scales to walls in cycles and needs PSRAM for state. Every target has an FPU, the P4 and S31 add SIMD and hardware loops, the P4 measures ~3× the S3 and a desktop core 20-40×, so the P4 and S31 are where this family shines on an MCU: the ESP32 class stops at about one noise sample per pixel on a 128² wall, and the desktop continues on the same contract and drives the wall over the network. The levers are standard: LUTs, fixed point, fewer samples, a field below output resolution, a field below frame rate, per-target FPU and SIMD behind one contract (Part 3 § Budget, § Per-target headroom).
- **Out of scope for Stage 1.** API names and signatures; the 8-bit versus 16-bit buffer decision; where advection state lives; the MoonLive frame-kernel shape; which showcase effects come first. All Stage 2 (Part 3 § Bridge).

## Why this document exists

The product owner's goals for this family, recorded 2026-09-03:

1. **MoonLight supports the building blocks of these effects**, via compiled functions and especially via MoonLive effects, on the power-function library ([power-functions.md](../../moonmodules/light/power-functions.md)), the same way it carries particles and SDFs.
2. **A few genuinely beautiful showcase effects** are built on those blocks, as the proof the blocks are right.
3. **The ESP32 is CPU-bound; memory is not the constraint.** Every choice is made against the per-pixel cycle budget, and the fact that these effects look better the higher the framerate is a design input.
4. **Industry-standard terminology and algorithms throughout**, per [CLAUDE.md § Principles](../../CLAUDE.md#principles): the textbook construct, named by its textbook name, from the primary source.

---

# Part 1: What we want to build

Stated from the primary sources only. Where a name is given, it is the algorithm's name in the literature; where a person is named, it is the originator of the algorithm, not an implementation.

## The two techniques

**A procedural shader** computes each pixel's color as a function of its position and the current time: `color = f(x, y, t)`. Nothing has to survive between frames, which is what makes it composable; state is welcome whenever it saves work: a polar table cached per geometry, a field rendered below the frame rate and interpolated, the dithering error carried forward, a value fed back into the next frame's parameters. Composition happens on the *coordinate*: transform where the pixel samples from (rotate, scale, fold, displace by another field) and the image transforms with it. This is the shader model of the GPU tradition, applied on a CPU one pixel at a time.

**Advection** transports a quantity (here: the color already in the frame) along a velocity field. Each frame: sources add color where they are drawn; every pixel's new value is the old value found by stepping *backward* along the velocity (the semi-Lagrangian method: Stam, *Stable Fluids*, SIGGRAPH 1999; *Real-Time Fluid Dynamics for Games*, GDC 2003), read with bilinear interpolation; and the result decays. The frame buffer is the simulation state.

They differ in one property that decides everything downstream: a shader needs no state and its cost is samples per pixel, with state used to cut samples; advection is all state and its cost is a resample per pixel per pass plus the memory to hold color above 8 bits. They share every block below the top.

## The blocks

### Coordinate mapping

- **Cartesian to polar.** For every pixel, `r = hypot(x − cx, y − cy)` and `θ = atan2(y − cy, x − cx)` about a center. Precomputed once per geometry into two tables; nothing trigonometric runs per pixel at render time except the sine and cosine of a *modulated* angle. Polar addressing is what makes motion rotate around a center rather than slide across the panel.
- **Polar transforms.** Add to θ to rotate; multiply r to zoom; add `k·r` to θ to twist into a spiral; fold θ into n mirrored wedges for a kaleidoscope (`θ' = |((θ mod 2π/n) − π/n)|`); take `1/r` for a tunnel. Each is one operation on the coordinate before sampling.
- **Domain warping.** Replace `f(p)` by `f(p + g(p))` where `g` is itself a noise field (Quilez, *Domain warping*). The canonical forms are `fbm(p + fbm(p))` and `fbm(p + fbm(p + fbm(p)))`; the intermediate displacement vectors are free color inputs. Applied in polar space, warping the angle makes the field swirl and warping the radius makes it breathe.
- **Normalized shader space.** Center the pixel and scale by the short side so a circle stays circular on a non-square panel; the GLSL `uv` convention.

### Noise

- **Gradient noise** (Perlin, *An Image Synthesizer*, 1985; *Improving Noise*, 2002): a pseudo-random gradient at each lattice corner, a dot product with the offset, a quintic fade, trilinear interpolation. Smooth to the first derivative, statistically isotropic, zero-mean. Simplex noise (Perlin 2001) is the same idea on a simplex lattice, cheaper in higher dimensions. **Value noise** interpolates random values at lattice corners instead; it is cheaper and visibly lumpier at low frequency because its extrema sit on the lattice.
- **Fractional Brownian motion (fBm).** Sum octaves at doubling frequency and halving amplitude (`lacunarity = 2`, `gain = 0.5`); structure at every scale. **Turbulence** sums the absolute value instead; the creases read as flame and smoke. Octave count is the primary cost knob.
- **Noise as a source of motion.** The third noise dimension advanced with time evolves a 2D field without scrolling it. A noise value mapped to an angle (`θ = 2π · noise`) is the classic flow-field construction (Hobbs, *Flow Fields*; Shiffman, *The Nature of Code*, autonomous agents).
- **Curl noise** (Bridson, Houriham, Nordenstam, *Curl-noise for procedural fluid flow*, SIGGRAPH 2007). In 2D the velocity is the perpendicular gradient of a scalar noise potential, `v = (∂ψ/∂y, −∂ψ/∂x)`: exactly divergence-free, so transported color swirls and never piles up, with two finite differences per pixel and no solver.

### Time: oscillators and modulation

- A **timer bank**: N independent clocks, each `phase_i = (t + offset_i) · ratio_i`, all scaled by one master speed so a single knob scales the whole animation.
- From each clock the four standard **LFO shapes**: a ramp (the phase itself, for scrolling and zoom), a wrapped phase in `[0, 2π)` (rotation), a bipolar sine in `[−1, 1]` (breathing), and a noise-driven angle `2π · noise(phase)` (wandering). Unipolar variants in `[0, 1]`.
- **Modulation**: a parameter bound to a clock and a depth, applied multiplicatively about its base value (`p' = p · (1 + depth · lfo)`), so nothing in the image is constant and every parameter can breathe. The synthesizer model, by its synthesizer names.
- **Framerate independence.** Every rate is per second, integrated with the measured `dt`; a virtual clock `t += dt · speed` keeps oscillators and decay in step under one speed control.

### Color

- **Contrast window**: clamp the field value to `[low, high]` and rescale; a black point and a white point, which is how a soft noise field becomes shapes with edges.
- **Palette mapping**: the windowed value indexes a gradient; or three field layers drive R, G, B directly; or a **cosine palette** (`a + b·cos(2π(c·v + d))`, Quilez) gives a whole ramp from twelve constants.
- **Gamma** as the last nonlinearity before output, and **quantization last**: compute in more than 8 bits, round once, with **temporal dithering** (error spread across frames) where the output is 8-bit.

### Sources: anti-aliased rasterization

Anything drawn directly into the frame, at fractional coordinates, additively:

- A **disc** by coverage from its signed distance: `cov = clamp(radius + 0.5 − dist, 0, 1)` per pixel; edges anti-alias for free.
- A **line** by Wu's algorithm (1991) or its generalization: step along the segment and distribute each step over the 2×2 neighbors with bilinear weights `(1−fx)(1−fy), fx(1−fy), (1−fx)fy, fx·fy`. The same 2×2 **splat** places a point source at sub-pixel precision.
- Sources move on the oscillators above: orbits, Lissajous curves (`x = A·sin(a·t), y = B·sin(b·t + φ)`), swarms with Reynolds steering, and borders or masks that are static geometry.

### Transport: the velocity field and the resampler

- **Velocity as a rule per pixel**, evaluated at sample time, not stored: a uniform wind (direction, speed, optional rotation); radial in or out; a spiral (rotate by α, move radially by ρ, in polar space); zoned rings (smoothstep-blended radii, each with its own swirl or drift); noise-driven shift (two independent 1D noise profiles, one per axis, applied separably); curl noise; or a stored field from a solver.
- **The resampler**: for each destination pixel, `src = dst − v·dt`; read the previous frame at `src` with **bilinear interpolation** (four loads, three lerps per channel); **wrap** at edges for a seamless tile or **clamp** so color flows off. A separable field (row shift then column shift) halves the work: two 1D passes through a scratch buffer.
- **Partial transport**: `out = (1 − b)·current + b·sampled` mixes what arrives with what was there; it reads as viscosity without a solver.
- **Stable fluids** (Stam 1999) when real fluid behavior is wanted: diffuse velocity, project to divergence-free (Jacobi or Gauss-Seidel iterations on a Poisson equation), self-advect, project again, optionally reinject lost swirl by **vorticity confinement** (Fedkiw, Stam, Jensen 2001), then diffuse and advect the dye. Orders of magnitude costlier than a velocity rule; curl noise gets most of the look for none of the solve.

### Decay

- **Exponential decay by half-life**: `k = 0.5^(dt / t½)`, multiply every channel by `k` each frame. Framerate-independent by construction; a half-life in seconds is the user-facing knob. A per-frame constant multiply (the 8-bit "fade by" idiom) is the same thing only at one fixed framerate.
- **Blur** as a second decay, separable, for softening trails.

### Arithmetic

- **Fixed point** throughout the hot path: Q16.16 for coordinates, velocities and color state; angles as a fraction of a turn in an integer (a 16-bit or 24-bit turn) so sine and cosine are table lookups and wraparound is free; square roots avoided by comparing squares or by an integer `isqrt`.
- **Lookup tables** for everything per-geometry (polar), per-turn (sine, cosine), per-curve (fade, gamma).
- **Precision graded by role**: the noise inner loop tolerates 8 fractional bits at a quality cost; color state does not.

## Cost

Per pixel, a shader pays `samples × noise_cost + transform + window + palette`; rich compositions run 3 to 12 samples. Cost is `area × samples`. Advection pays, per pixel per pass, four loads, three lerps and one multiply per channel, and per frame the field rule; cost is `area × channels × passes`, independent of noise, plus color-state memory of `area × channels × 4 bytes` (and a scratch copy, and any stored field). A 16-bit or wider color state is not optional for advection: an 8-bit state cannot hold sub-integer accumulation from splats or a slow decay, and a trail fading through 256 levels posterizes at the low end.

**Framerate is part of the rendering method.** Transport per frame is `v · dt`; when it exceeds about a pixel the bilinear filter smears instead of moving and edges tear. A decay of `0.5^(dt/t½)` with a short half-life becomes a strobe at low fps. Temporal dithering averages only above the eye's integration rate. Oscillators sampled below twice their rate alias. The consequence is a design rule: the per-frame cost must stay small enough that the frame rate stays high, and the metric to report is **pixels per second**, which is what the frame rate at a given area actually measures.

The standard levers, in the order they are usually pulled: lookup tables; fixed point; fewer samples (octaves, warp depth); render the field below output resolution and upscale bilinearly (a smooth field hides it); update the field below the frame rate and interpolate; vectorize the noise inner loop where the CPU has SIMD; use the hardware FPU for the kernels that are float by nature (square roots, arctangents, a reference algorithm kept in float), behind the same contract; move the field update to a second core; and above all of these, run the field on a machine with the cycles and feed the lights over the network.

---

# Part 2: Examples from the field

Implementations that do similar things to Part 1. None is the specification; each fills in a spot: a measured number, a parameter vocabulary users expect, a fixed-point choice that is known to work, a precision decision.

| Example | What it is | Similar to | What it fills in |
|---|---|---|---|
| **ANIMartRIX** (Stefan Petrick; also in FastLED master as `fl::Animartrix`) | A polar-noise shader engine: per-pixel polar tables, a timer bank yielding ramp / phase / bipolar sine / noise-angle signals, a "5D coordinate mapper" (`newx = (offset_x + cx − cos θ·r)·scale_x`, likewise y, `z` from a ramp), gradient noise per layer, a low/high contrast window, per-channel or palette color, gamma, sanity clamp; 50 named animations of 1 to 12 layers | Part 1 shader, polar transforms, LFO bank, contrast window | The author's throughput on the Teensy 4.0 the engine was tuned on: ~730 k RGB pixels/s per layer at 600 MHz ("20 fps on 36k LEDs"), 53 k/s on an ESP32 core, 110 k/s on two; "3 layers at 400+ fps, 10 layers at ~50 fps" on a 16×16 class panel. Float throughout, quantized to 8 bits "in the very last step", temporal dithering. The parameter names users of this style know: `scale_x/y/z`, `offset_x/y/z`, `z`, `center`, `low_limit`, `high_limit`, `master_speed`, `ratio`, `offset` per timer |
| **FastLED master's fixed-point migration of the same engine** (`3e77a096`, 2026-09-02) | Q16.16 (`s16x16`) throughout; angles as A24 (24-bit turn) into `sincos32`; four gradient-noise precisions side by side (`s16x16` reference, `q16`, an `i16`-optimized inner loop "2× faster multiplies", `s8x8` "4× faster, trades accuracy"), a 4-wide SIMD noise; **bit-identical tests** against the float engine | Part 1 arithmetic | Proof that the whole shader family is exact in Q16.16 with a table sine and a table-driven gradient noise, on the ESP32 class; the precision-by-role grading; a fixed-point type family documented for MCUs ("integer math is 5-100× faster … no rounding errors; results are exact and reproducible") |
| **FlowFields** (4wheeljive, from a 2026 concept post by Petrick; forks: `ewowi/flowfields`, Petrick's `ColorTrails`) | An advection engine: **emitters** ("anything that is drawn directly") plus **flows** ("an invisible wind that moves the previous pixels and blends them together"). Six flows as displacement rules (noise via two decoupled 1D profiles, radial, directional wind with a perpendicular wobble, three-zone rings with swirl and drift, spiral, and a full stable-fluids solver with vorticity confinement), eight emitters (orbital, swarming and audio dots, Lissajous line, rainbow border, noise kaleidoscope, cube, fluid jet), a 20-timer modulator bank with multiplicative "breathing", float RGB grids, a half-life fade `0.5^(dt/persistence)`, dithered 8-bit output, a virtual clock under one `globalSpeed` | Part 1 advection, velocity rules, sources, decay, modulation | The flow vocabulary and defaults users expect (`persistence`, `blendFactor`, `windStep`, `angularStep`/`radialStep`, `innerSwirl`/`outerSwirl`/`midDrift`, `xShift`/`yShift`, `noiseFreq`, `viscosity`, `vorticity`, `gravity`; ~35 named controls); that separable noise transport with decoupled axes reads as diagonal flow; that partial transport reads as viscosity; the solver's cost (5 Jacobi iterations per solve, three solves per frame) as the upper bound |
| **`fl::FlowField`** (FastLED master, from the above, 2026-03-21) | The same engine distilled to two emitters and the noise flow, in a float variant and a **Q16.16 variant** with `i32` color state ("allowing sub-integer color accumulation during splats") quantized at output; coverage discs and 2×2 bilinear lines; a "noise punch" impulse into the profiles | Part 1 resampler, sources, decay, arithmetic | The measured share: **advection ~80% of frame time** in fixed point, with the hoisting and `restrict` tricks that get it there; six 32-bit grids (384 KB at 128²), hence "memory is large"; defaults `persistence` 0.86 s, `flow_shift` 1.8 px, `noise_freq` 0.33/0.32 |
| **`fl::gfx` canvas** (FastLED master) | An anti-aliased 2D canvas: line, disc, ring, stroked line with caps, additive by default, float / int / fixed-point coordinates | Part 1 sources | The API shape a sub-pixel rasterizer converges on |
| **Fixed-point sub-pixel graphics** (Sutaburosu; the anti-aliased canvas demo in MoonLight as `FixedPointCanvasDemoEffect`, and in FastLED master as the fixed-integer drawing `fl::gfx` credits him for, the SKIPSM Gaussian blur behind `fl::gfx::blur`, and the Elias water effect `FxWater`) | Twelve sub-demos on a Q16.16 anti-aliased canvas (clock, orbiting discs, star web, spirograph, Lissajous, thin and thick cube, organic walkers, boids, hypotrochoid, branching tree), "blazing fast fixed integer drawing"; a binomial two-pass Gaussian blur; a 2D ripple simulation after Hugo Elias | Part 1 sources, arithmetic, blur | That anti-aliased sub-pixel drawing is fully deterministic and FPU-free in Q16.16 at panel-to-wall sizes; the blur kernel shape a trail idiom converges on; a working catalog of source geometries (orbits, Lissajous, spirograph and hypotrochoid curves, boids, a recursive tree) for the emitter set. The canvas demo itself is parked by the product owner as a port candidate |
| **`fl::WaveFx`** (FastLED master, after Shawn Silverman) | A 2D wave-equation simulation with 2× to 8× supersampling, mapped to color by gradient | Part 1 stored-field transport | Supersampling as the artifact lever for a stored field; the comment that 2× "gives the best results for the CPU consumption" |
| **`fl::Luminova`** (FastLED master) | 256 particles with per-frame fade and blur, "soft white trails" | Part 1 decay + blur, with particles as sources | Fade-plus-blur as the trail idiom for point sources |
| **Flow-field generative art** (Hobbs; Shiffman) | A grid of angles from noise; particles or pen strokes follow the angle in small steps | Part 1 noise-as-motion | The tracing form of a flow field, where the particle is the source and the field never touches the frame; Hobbs's advice to distort with something other than Perlin noise once the look is familiar |
| **Stable fluids** (Stam 1999, GDC 2003) | The reference solver and its "linear backtrace" advection | Part 1 transport | The algorithm every advection effect above descends from, and the reason semi-Lagrangian advection is unconditionally stable at any `dt` |
| **Earlier 8-bit work in the same idiom** (FunkyNoise, FunkyClouds, 2014; a self-modulating simplex noise gist) | Polar angle per pixel shifted by noise; noise modulating the offsets, scale and palette index of the next noise pass | Part 1 warping, feedback modulation | That the idiom predates FPUs and was first done in 8-bit FastLED terms |
| **MoonLight's port of ColorTrails** (2026-03) and **FastLED-MM's FlowFields sketch** (2026-04/05) | The advection engine on `fl::CanvasRGB` + `s16x16`; the same engine at 128×128 inside MoonLight's earlier module runtime with 35 registered controls | Part 1 advection, in our own prior work | That the family has already run on our hardware and behind our UI; a bench reference for the advection showcase |

Two things the examples agree on that Part 1 states as requirements: **compute above 8 bits and quantize last**, and **report pixels per second**. One thing none of them does yet: curl noise as a velocity rule, which Part 1 includes because it is the standard cheap answer to fluid-looking flow.


---

# Part 3: How this fits MoonLight

## What we have

Against Part 1's blocks, measured on this tree:

| Part 1 block | MoonLight today | Status |
|---|---|---|
| Polar mapping | `atan16`, `dist16`, `kaleido` ([math16.h](../../src/core/util/math16.h)), computed per pixel; `PolarNoiseEffect` uses them | ✅ per pixel; ⬜ no precomputed LUT type |
| Polar transforms, `uv` | `shader.h`: `uv`, `rotate`, `repeat`, `mirror`; `kaleido` | ✅ |
| Noise | `inoise8`/`inoise16` **value** noise; `fbm8`/`fbm16`, `turbulence8`, `warp8` ([noise.h](../../src/core/util/noise.h)) | ✅ fBm, warp, turbulence; ⬜ gradient noise; ⬜ curl noise |
| Oscillators | `BeatPhase`, `beat`/`beatsin`, `sin16`/`cos16`, `smoothFollow`, easings, `hashInt` | ✅ single phase; ⬜ timer bank with the four LFO shapes and modulation binding |
| Contrast, palette | `map32`, `smoothstep`, `colorFromPalette`, `cosPalette` | ✅ |
| Gamma, dithering | `Correction` curves at the driver | ✅ gamma at output; ⬜ temporal dithering |
| Sources | `coverage` (SDF), `draw::splat` (24.8 sub-pixel, additive), `draw::line` (Bresenham, not anti-aliased), `particles.h` | ✅ disc, splat; ⬜ anti-aliased line |
| Resampler | `draw::scroll` (integer shift, wrap or clear) | ⬜ **no bilinear sample of the previous frame** |
| Velocity rules | none | ⬜ |
| Decay | `Layer::fadeToBlackBy` (8-bit multiply, once per frame, MIN across effects) | ✅ 8-bit; ⬜ half-life form on wide state |
| Blur | `draw::blur`, separable, every axis | ✅ |
| Color state | `Layer` buffer, `uint8_t` per channel ([Buffer.h](../../src/light/layers/Buffer.h)), persisting between frames | ✅ persistence; ⬜ **no state above 8 bits** |
| Fixed point | 16-bit contract, uint8 angle / `angle16` turn, 24.8 positions, 16.0 noise coordinates ([power-functions-analysis-top-down.md § 2](power-functions-analysis-top-down.md)) | ✅ |
| Shader runner | `shader.h`: `each` (one function of position and time, the loop, mapping and write handled) | ✅ |
| Particles | pool, gravity, drag, bounce, collide, splat render | ✅ |
| Stored-field simulation | none | ⬜ (out of scope unless the top-down wants fluid) |

The persistence contract advection needs already exists: the Layer does not clear between frames, and "a read-prior effect reads last frame's pixels via `draw::get` / `draw::blur`; the persistence *is* its state" ([MoonLight, buffer persistence](../../explanation/architecture/moonlight.md#buffer-persistence-the-layer-keeps-what-it-drew)). What is missing is the resampler and the bit depth.

`PolarNoiseEffect` is the Part 1 shader already: polar addressing, `warp8` in polar space (the angle warped by noise), `kaleido`, a palette, with `octaves` and `warp` exposed as the cost knobs and the header stating the cost ("~4 samples/pixel at octaves=2 … on a large wall drop `octaves` to 1").

## What MoonLive has

55 builtins ([MoonLiveBuiltins_light.h](../../src/light/moonlive/MoonLiveBuiltins_light.h), [MoonLiveBuiltins_common.h](../../src/core/moonlive/MoonLiveBuiltins_common.h)): `noise`, `polarA`/`polarR`, `sin`/`cos`, `beat`/`beatsin`, `smoothstep`/`step`/`smin`, `uvX`/`uvY`, `scale`, `mod`/`div`/`fdiv`, `setRGB`/`setXYZ`/`setPaletteColor`/`fill`/`fade`/`line`, the audio set, and the particle set. A script can write a Part 1 shader pixel today (`octopus.mle` is one). The measured cost is the call rate, not the arithmetic ([moonlive-language-roadmap.md § 4c](moonlive-language-roadmap.md)): `plasma.mle` at 9 host calls per pixel is 16,031 µs on a 3,840-pixel S3 fixture (~4.2 µs/px); `polarR` alone ~3.5 µs/px because it wraps a real square root; the particle vocabulary, "one call per FRAME rather than per pixel", is 54× cheaper on the same fixture. The roadmap's conclusions apply unchanged: "per-pixel builtins want to be inline ops, not calls" and "a `frame()` entry shape would sidestep it entirely".

Advection is not expressible from a script: the 64-byte arena holds no frame of state and no builtin resamples the previous frame. The particle pool shows the shape that works for stateful families: a handle to native state, whole-pool passes per frame, the script composing them.

## The budget, measured

Per-pixel cycles at 240 MHz ([power-functions-analysis-bottom-up.md § Shaders](power-functions-analysis-bottom-up.md)): ~15,600 at 16×16 @ 60 fps, **~293 at 128×128 @ 50 fps**. The S3 render-only sweep ([performance.md](../../reference/performance.md)), µs per frame:

| Effect | 16² | 32² | 64² | 128² | per pixel at 128² |
|---|---:|---:|---:|---:|---:|
| Noise (simplex, 1 sample/px) | 913 | 2,951 | 11,661 | 51,230 | 3.1 µs (~750 cycles) |
| Plasma | 352 | 1,020 | 3,744 | 20,020 | 1.2 µs |
| Metaballs | 462 | 1,757 | 6,108 | 28,576 | 1.7 µs |
| LavaLamp | 309 | 974 | 3,612 | 21,243 | 1.3 µs |

On the classic ESP32 (no FPU, no PSRAM) the Noise effect runs 1,117 / 324 / 71 / 17 fps across the four sizes. So one noise sample per pixel is already 2.5× the wall budget, which is consistent with the field's own numbers in Part 2 once clock and FPU are normalized: **the shader half is a panel-class family on every MCU, and the engineering is in Part 1's levers.** Advection scales to walls in cycles (no noise per pixel; the field's noise is per row and column) and its cost is state: six 32-bit grids at 128² are 384 KB, PSRAM territory, and a panel-class family only on a PSRAM-less classic.

Framerate protection is therefore the rule for both, in different currencies: **samples per pixel** for a shader, **cycles per frame and bytes of state** for advection.

## Per-target headroom: FPU, SIMD, clock, and where the desktop takes over

What each shipped target brings, from the IDF SoC capability headers and our own measurements:

| Target | Cores × clock | FPU | SIMD | State memory | Measured against the S3 |
|---|---|---|---|---|---|
| classic ESP32 (Xtensa LX6) | 2 × 240 MHz | single precision | none | internal only, or 4 MB PSRAM on WROVER / 2 MB on PICO | slower; beats the S3 only on memory-bound loops (internal RAM vs PSRAM latency) |
| ESP32-S3 (Xtensa LX7) | 2 × 240 MHz | single precision | 128-bit PIE (`SOC_SIMD_INSTRUCTION_SUPPORTED`) | 8 MB octal PSRAM | the reference row in this document |
| ESP32-P4 (RISC-V) | 2 × 400 MHz | single precision | PIE + hardware loops (`SOC_CPU_HAS_PIE`, `SOC_CPU_HAS_HWLOOP`) | 32 MB PSRAM | ~3× on heavy compute ([performance.md](../../reference/performance.md)) |
| ESP32-S31 (RISC-V) | 2 × 320 MHz | single precision | PIE + hardware loops | PSRAM | between the S3 and the P4 |
| desktop | GHz class | double and single | NEON / SSE / AVX | unbounded | 20-40× an S3 per core, plus SIMD ([performance.md](../../reference/performance.md), the `collide` measurement) |
| Teensy 4.x (Cortex-M7), a future target | 1 × 600 MHz | single and double | none (DSP instructions) | 1 MB internal, no PSRAM | not measured; listed in [architecture.md § Scaling to available memory](../../reference/hardware/firmware-variants.md#scaling-to-available-memory) as a supported class |

**Can the FPU help?** Every target has one, so a float kernel is legal everywhere, and the repo already has the precedent: `raymarch.h` is compiled only where the SoC declares an FPU, as "the one bounded exception to the integer-only render path", while `shader.h` stays fixed point and runs everywhere ([power-functions.md § Raymarching](../../moonmodules/light/power-functions.md#raymarching-one-technique-inside-a-shader)). The honest expectation: on these cores a float multiply costs about what an integer multiply costs, so an FPU does not make a noise sample cheaper; it makes square roots, arctangents and trig cheap enough to skip the tables, and it lets a float reference algorithm run unconverted where an exact fixed-point port is not worth writing yet. The portable contract stays fixed point; the FPU is a per-target acceleration behind it, per the standing decision.

**Where the family shines on an MCU.** The S3 is the baseline this document measures against because it is the bench board with numbers, not because it is the target. The P4 is the natural home of the shader half: the highest clock, four-lane SIMD, hardware loops and 32 MB of PSRAM put a 64² composition and a 128² single-layer field inside its budget, and the S31 sits next to it on every axis. The classic is the portable floor, the target that keeps the contract honest. The top-down should size the showcases for the P4 and S31, keep them running on the S3, and let the classic degrade by the cost knobs.

**Can SIMD help?** Yes, and the P4 is where it pays: PIE processes four 32-bit lanes per instruction and the hardware loop removes the branch per iteration, which is exactly the shape of a noise inner loop, a bilinear lerp across four channels, or a separable advection pass. The S3 has the same width. This is the lever the field has already pulled (a 4-wide fixed-point noise exists in the wild), and it is a per-target implementation of the same function, never part of the contract. The classic has neither, which is why it is the target that decides the portable budget.

**Where the border is.** Per pixel at 50 fps, the cycle budget is `clock / (area × 50)`; one noise sample costs ~750 cycles on the S3 today, ~250 on the P4 by the measured ratio, ~20-40 on a desktop core. Samples per pixel affordable at 50 fps, rounded down:

| Grid | S3 (240 MHz) | P4 (400 MHz, ~3×) | desktop (one core, ~30×) |
|---|---:|---:|---:|
| 16×16 | 24 | 72 | hundreds |
| 32×32 | 6 | 18 | ~180 |
| 64×64 | 1 | 4 | ~45 |
| 128×128 | 0 (one sample = 19 fps) | 1 | ~11 |

So the ESP32 class stops at one sample per pixel on a 128² wall (S3) to one or two (P4), and carries a rich 3-to-12-layer composition only up to about 32×32 (S3) or 64×64 (P4). Beyond that line the desktop continues without a change of code: it is the same effect on the same contract, with SIMD and the clock on its side, and it already drives lights over the network as a processing node ([MoonLight, drivers](../../explanation/architecture/moonlight.md#drivers): ArtNet, DDP, E1.31). Advection moves the border differently: its per-pixel cost is fixed and small, so the S3 and P4 carry it to a 128² wall as long as the wide color state fits PSRAM; the classic without PSRAM stops at panel size for lack of memory, not cycles.

## What we need to add

In Part 1's order, against the table above:

1. **Gradient noise** beside value noise, in the existing 16-bit fixed vocabulary, as the quality upgrade for every field effect; keep value noise as the cheap inner-octave option (Part 2's precision-by-role grading).
2. **A polar LUT type**: angle and radius per pixel, built on the cold path, invalidated on geometry change; `PolarNoiseEffect` and every polar effect read it instead of calling `atan16`/`dist16` per pixel.
3. **An oscillator bank**: N clocks with offset and ratio under one master speed, the four LFO shapes, and a modulation binding (`parameter · (1 + depth · lfo)`), as one power function family rather than per-effect members.
4. **Curl noise** as a velocity rule, and the plain rules (wind, radial, spiral, rings, separable noise) as small functions of (pixel, t).
5. **The resampler**: bilinear sample of the previous frame at a fractional source, wrap and clamp variants, separable and full 2D forms. This is the advection kernel and the single largest missing block.
6. **Half-life decay** and an **anti-aliased line**, completing the sources-and-decay set beside `splat` and `coverage`.
7. **Color state above 8 bits** for advection and for dim trails: either a 16-bit Layer buffer (the LED-driver analysis already backlogs a "16-bit pipeline + dither", [backlog-light.md](backlog-light.md)) or effect-owned Q16.16 state quantized into the 8-bit Layer each frame. The examples are unanimous about *where* quantization goes (last) and split on *where the state lives*; the top-down decides.
8. **Temporal dithering** at the quantization step, once the state is wide.
9. **MoonLive frame kernels**: the prepare / emit / advect / decay passes and the polar-shader inner loop exposed as whole-frame builtins (inline ops or handle-based native passes), so a script composes frames the way it composes particle passes. Depends on the multi-argument host-call and arena work the language roadmap already names.
10. **Showcases**, three: a pure shader in the polar-noise idiom beyond `PolarNoiseEffect`; an advection effect with the noise, spiral and directional rules, two sources and the oscillator bank; and one that feeds a shader's output into a flow.

## Bridge to the top-down

The bottom-up settles what the top-down can take as given: the family is two techniques over one shared block set, every block has a textbook name and algorithm, the cost model is arithmetic and makes a shader a samples-per-pixel problem and advection a cycles-per-frame-and-memory problem, framerate is the rendering method for both, the field has proven the whole set exact in Q16.16 on our silicon class, and MoonLight lacks three blocks (resampler, wide color state, gradient noise) plus the bank, the LUT and the kernels that make them composable. What it leaves open is design: names and signatures in our fixed-point vocabulary; where wide state lives and whether the Layer goes to 16 bits; the MoonLive frame-kernel shape and its dependency on the host-call ABI; which per-target accelerations sit behind the one contract; the three showcases to the control level; the tests, the timing contracts, and the order the work lands in. The top-down owns those decisions and their resource accounting.

### Prompt for the agent that writes the top-down

> Read `CLAUDE.md`, `docs/explanation/architecture/index.md` (§ Hot path discipline, § Effects and Layer, § Buffer persistence, § MoonLive), `docs/contributing/coding-standards.md`, `docs/backlog/generative-fields-analysis-bottom-up.md` (this document, in full: Part 1 is the specification language, Part 2 the examples, Part 3 the gap), `docs/backlog/power-functions-analysis-top-down.md` (the shape and the standing decisions: dimension-generic, one contract everywhere with per-target acceleration, fixed point invisible to the writer, the 16-bit contract, particles as the stateful precedent), `docs/moonmodules/light/power-functions.md` (what exists), and `docs/backlog/moonlive-language-roadmap.md` § 4c and § 2 (the per-pixel call cost and the multi-argument host-call blocker).
>
> Write `docs/backlog/generative-fields-analysis-top-down.md`: the implementation spec for the generative-fields family in MoonLight, for compiled effects and for MoonLive scripts, on the power-function library. Use the house format of the power-functions top-down (status legend, TL;DR, numbered sections, decisions for sign-off, out of scope). Present tense for what exists, forward-looking only under the banner. American spelling, no em-dashes. Specify every kernel from Part 1's primary sources by its textbook name and algorithm; use Part 2 only for measured numbers, parameter vocabularies and precision choices, never as a source of code, and record prior art per module the way `PolarNoiseEffect.h` does.
>
> Decide, with rationale and resource accounting per target (classic ESP32 without PSRAM, S3, P4, desktop): (1) the power functions to add for Part 3's ten items, with signatures in the repo's fixed-point vocabulary; (2) where wide color state lives (a 16-bit Layer buffer versus effect-owned Q16.16 state quantized into the 8-bit Layer), with the memory table per fixture size and the dithering plan; (3) the MoonLive frame-kernel shape: how a script composes whole-frame passes and reads a per-pixel shader through inline ops rather than per-pixel host calls, and which language-roadmap blockers it depends on; (4) the performance plan against Part 3's measured budget and per-target headroom table: samples-per-pixel targets for shaders, cycles-per-frame and bytes-of-state targets for advection, the field-below-resolution and field-below-frame-rate levers, which kernels get FPU or SIMD (PIE) variants per target behind the one contract, where the desktop takes over as the processing node, and how framerate is protected as the rendering method; (5) the three showcases, specified to the control level; (6) the tests that pin each kernel (golden frames on the desktop, per-target timing contracts in scenarios) and the bench criteria the product owner judges by eye on the S3 and on a wall; (7) the order the work lands in, each step verifiable on the desktop first.
>
> Where a decision needs the product owner, list it under decisions for sign-off with the options and your recommendation rather than deciding silently. The bar is the measured quality and cost of the effects themselves.
