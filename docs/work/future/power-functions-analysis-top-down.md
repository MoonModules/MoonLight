# Power functions — top-down build spec

> **Forward-looking design document — exception to CLAUDE.md present-tense rule.** Stage 2 of the power-functions work: turns the [bottom-up catalog](power-functions-analysis-bottom-up.md) into an implementable spec — homes, types, signatures, migration order, tests, budgets. Written 2026-08-06 against the nine product-owner decisions recorded there. Where this document makes a NEW decision it is marked **(proposal)** and listed in § Decisions for sign-off. Companion boundary: the [livescripts top-down](livescripts-analysis-top-down.md) owns the MoonLive *engine* (grammar, IR, codegen); this document owns the *builtin surface* the engine calls into.


## Status legend

Every claim below carries its status, checked against the code rather than against intent:

| | Meaning |
|---|---|
| ✅ | **Done.** Built and in the tree. |
| 📖 | **Moved.** The durable version now lives in the docs or the code, so this copy is a historical note. Where it went is named inline. |
| 🔨 | **To do.** Still open, and the reason it is open is stated. |
| ❓ | **Unsure.** A claim, a number or a decision that has not been verified — treat as a question, not a fact. |

An unmarked line is context or rationale rather than a deliverable.

## TL;DR

- ✅ **The set lands in the existing homes, grown — not a new parallel library.** `core/math16.h` (new: the 16-bit contract tier), `core/noise.h` (grows fbm/warp/16-bit sampling), `light/draw.h` (grows splat, AA line, circle, rect/bar, scroll, the SDF trio), `light/particles.h` (new: the particle kernel), `light/polar.h` (new: the polar/kaleido LUT, modifier-first), `light/Palette.h` (grows cosine palettes + gamma). One style: same free-function shape `draw::` already has, same fixed-point vocabulary everywhere **(proposal)**.
- ✅ **Three shared types carry the whole contract:** `pos_t` = `int32_t` positions in **24.8 sub-pixel fixed point** (±8M pixels — covers a 16K-light strip where WLED-PS's int16 cannot; one word on every 32-bit target); `angle16` = `uint16_t`, 65536 = full turn; `frac16` = `uint16_t` 0..65535 fractions. Velocities are `int16_t` 8.8 per frame. The 8-bit tier (`math8.h`) stays as the internal fast path and for inherently mod-256 domains (palette index, hue) **(proposal)**.
- ✅ **The 22-effect boilerplate dies with one struct:** `draw::Canvas{buf, dims, cpl}`, returned by `EffectBase::canvas()`. Taken as `const Canvas&` (measured: a non-const reference costs ~3% more instructions in a tight per-pixel loop, because the extents become memory re-loads the compiler cannot hoist past a possible alias with the buffer; passing dims by value avoids that today). The gain is **correctness, not speed**: buffer and dims are currently two independent arguments nothing checks for agreement, and the pairing becomes unrepresentable-if-wrong — plus the 16 `depthDim()` copies are deleted rather than centralised, and `splat`/SDF-coverage/projection get a home for their context instead of adding loose parameters at every call site. The existing `(Buffer&, dims)` overloads remain during migration and their removal is **mandatory, not aspirational** — a permanent two-API window is worse than either option alone.

  **Const means the surface, not the pixels.** Every draw call takes `const Canvas&` yet writes to the buffer: the Canvas is a *descriptor* (a raw pointer plus extents), so const protects the description — nothing can retarget a call to a different buffer or silently change the extents mid-frame — while the pixels behind the pointer stay writable. This is the same shape as a `std::span` passed by const reference. Stated because a call that mutates through a `const&` is otherwise a surprise **(proposal)**.
- ✅ **`particles` is a pool the effect owns, not a module:** SoA arrays in ScratchBuffer, allocated at `prepare()`, semi-implicit Euler `step()`, the named forces (`gravity/force/drag/bounce/attract`), two emitters, optional binned collisions, rendered through the sub-pixel `splat`. Sized by the effect; zero static RAM when unused.
- ✅ **Noise: keep value noise, widen it — gradient noise is a swap-in upgrade, not a blocker.** `noise16(x,y,z)` returns full-range 16-bit (our existing value noise rescaled and interpolated up); the name promises the *field*, not the algorithm, so Perlin gradient noise can replace the core later without touching any caller **(proposal)**.
- ✅ **Migration order is by leverage, cheapest risk first:** *(phases ① ② ④ ⑤ done; ③'s kernel is built and the five convergences are 🔨 open)*  ① `beatPhase` + `map16` + `Canvas` (mechanical, pixel-identical, kills the three biggest hand-roll counts) → ② geometry + bars (4 audio effects) → ③ `splat` + `particles`, converging the five particle-shaped effects (bench-judged, the PS-replaces-twin decision) → ④ fields + polar (LavaLamp/Metaballs/Rings/Spiral) → ⑤ hidden-modifier extraction as encountered (FreqSaws `invert` first). Each pixel-identical claim is pinned by a **golden-frame test** (fixed seed, fixed time, byte-compare) — a new, small test harness capability.
- 🔨 **MoonLive exposure is stage 3 and states only its requirements here:** a built-in table of ≥ 64 entries, typed multi-arg host calls (up to 6 args + return), the symbols `x/y/z/w/h/d/time` (already threaded to the runtime, unexposed), and a per-frame entry point alongside the per-pixel one — the bottom-up's feasibility math says scripts *compose* kernels per frame; they do not interpret per pixel on large surfaces. The calling convention itself belongs to the livescripts engine work.
- ✅ 📖 **Measured on hardware (ESP32-S3, 240 MHz, 128×128, 2026-08-06)** *(the per-effect numbers now live in [effects.md](../../moonmodules/light/effects.md) beside each effect)*  — the theoretical budget below was an upper bound; these are the real numbers, and they reframe it. Today's *existing* effects already cost **305–692 cycles/pixel** and run at **21–48 fps** on a 128×128 panel, so "292 cycles/pixel at 50 fps" describes a frame rate this fixture size does not reach in the first place, with or without power functions. What the budget genuinely constrains is *added* cost per pixel, and the measured SDF forms are small against that: `sdBox` ≈ 6, squared-distance `sdCircle` ≈ 14, and the full `isqrt` form ≈ 108 cycles/pixel (desktop instruction counts; the ESP32 divide penalty makes the last one worse, the first two barely move). A squared-form SDF plus `smin` plus a palette lookup is a fraction of what Plasma already spends. **Design consequence:** the squared forms are the default path and the sqrt form is opt-in for true distance (outline width, linear glow).
- ✅ **Budgets are stated per family and gated:** the render loop's ceiling stays the bottom-up's 293 cycles/pixel at 128×128@50; the particle budget is ~40 cycles/particle/frame (2048 particles ≈ 0.34 ms at 240 MHz); every function gets a host micro-benchmark and the migrations ride the existing `collect_kpi` gate. Zero static RAM for everything unused (`check_footprint`).

## 1. Homes and style ✅ 📖 *(shipped; the headers themselves are the reference now)*

CLAUDE.md's rule is extend-don't-duplicate, and the PO's one-codebase decision demands a single style. Both are satisfied by growing the existing homes with one consistent convention rather than opening a parallel `fx::` library:

| Home | Gains | Notes |
|---|---|---|
| `core/math16.h` **(new)** | `sin16/cos16` — 130-byte quarter-wave 16-bit table + lerp (0.031% error; the zero-table variant was tried and rejected — see §6), `triwave16/quadwave16/cubicwave16`, `ease16InOutQuad/Cubic`, `map16/map32` (fencepost-safe), `isqrt32`, `dist16`, `scale16`, `BeatPhase` (the stateful uint64 accumulator, `phase(bpm, ms)` → `angle16`), `beatsin16` rebuilt on the LUT+lerp sine | The contract tier. `math8.h` is unchanged and becomes internal/domain-specific (palette index, hue). |
| `core/noise.h` | `noise16(x[,y[,z]])` full-range, `fbm16(p, octaves)`, `warp16` (the one composition rule) | Same 16.0 fixed coordinate convention it already has. |
| `light/draw.h` | `Canvas`, `splat` (24.8 sub-pixel Wu write, the PS/WLED weight math with the inverse-gamma note), `lineAA` (Wu 1991), `circle/fillCircle` (midpoint), `rect/fillRect/bar`, `scroll(axis, delta, wrap)`, `sdCircle/sdBox/sdSegment + smin` + `coverage(d)` AA helper | Free functions, `const Canvas&` first arg — the `draw::` shape it already has. |
| `light/particles.h` **(new)** | `particles::Pool` (SoA over ScratchBuffer), `step`, `gravity/force/drag/bounce/attract`, `spray/angleEmit`, `collide` (x-binned, optional), `render(const Canvas&, palette)` | The industry name (Reeves 1983). Fire2012-style heat, Elias ripple, and the CA step are siblings in the same header — stateful field kernels. |
| `light/polar.h` **(new)** | `PolarLut` (per-pixel r,θ baked at prepare), `kaleido(n)` fold | Modifier-first per the PO decision; effects may consume the LUT read-only. |
| `light/Palette.h` / `core/color.h` | `cosPalette` (Quilez 12-constant, baked to the existing 16-entry `Palette` on change), `gamma8` LUT | `colorFromPalette` stays the one hot-path seam. |

Every function's doc block names its canonical source — the convention the effects already follow.

## 2. Types ✅

- **`pos_t = int32_t`, 24.8 fixed point.** One pixel = 256 sub-units. Chosen over WLED-PS's int16+6-bit (±512 px — too small for a 16K 1D strip) and our ParticlesEffect's 12.4 (±2048 px — same problem). int32 is single-word on every target; `>>8` decodes; the sign-corrected shift idiom from the bottom-up applies (never bare `>>` on negatives).
- **`angle16 = uint16_t`**, 65536 = full turn; overflow is the free 2π wrap. The 8-bit angle survives only inside `math8.h`.
- **`frac16 = uint16_t`** 0..65535 for interpolation/easing inputs and outputs.
- **Velocity `int16_t` 8.8 per frame**; forces via the 3.4 accumulator (the WLED-PS smooth-sub-unit trick, reimplemented fresh).
- **Time**: `elapsed()` ms as today; `BeatPhase` owns the uint64 numerator-divide-late idiom the nine effects hand-roll.
- **Dimension-generic rule**: every geometry/field function takes `Coord3D`; 1D/2D degenerate by extent (the `draw::blur` model — one call, every axis with extent > 1).
- **Dimension audit (verified against the dimension-generic decision):** fully generic by construction — frame ops, pixel ops (`splat` = 2/4/8 corners for 1D/2D/3D), fields (`noise16` has all three arities), time/color/random, and the particle kernel (SoA per axis — one system where WLED-PS maintains two; 3D collisions correct, x-binning just less selective). The SDF trio is the strongest case: `|p|−r` IS two points / circle / sphere, one formula. Five named 2D-primary items, each with its path: `lineAA` (3D = splat along the 3D Bresenham line — falls out of the generic splat), `text` (glyphs are 2D; renders a z-slice on 3D fixtures, meaningless in 1D), `PolarLut`/`kaleido` (cylindrical/spherical variants wait for a consumer), `angleEmit` (3D needs the spherical two-angle form), `ripple`/fire (volumetric variants wait for a consumer). None is a blocker: the pipeline already lifts lower-dim output via `Layer::extrude()`, so a 2D-primary function stays usable on every fixture, like today's 2D effects.
- **Fixed point is the default and invisible (standard approach, PO decision):** an effect writer works in `pos_t`/`angle16`/`frac16` and the power functions, and never chooses a width or a representation per case — the vocabulary IS fixed point. The only per-case judgment left is effect-private math outside the power functions, already governed by the existing rule: per-frame float allowed, per-light float not ([coding-standards § numeric types](../../contributing/coding-standards.md)).

## 3. The particle kernel ✅ 📖 *(built; the API listing lives in [power-functions.md](../../moonmodules/light/power-functions.md#particles))*

```cpp
particles::Pool pool;                    // POD view over ScratchBuffer arrays
pool.init(scratch, count);               // at prepare(): SoA x/y/z, vx/vy/vz, ttl, hue — no allocation later
pool.gravity(g);                         // one dv per frame, applied to all (branch costs more than work)
pool.force(i, fx, fy);                   // 3.4 accumulator per particle
pool.drag(k);                            // v *= (256-k)/256
pool.step();                             // semi-implicit Euler: v += a; x += v  (Fiedler)
pool.bounce(e, roughness);               // reflect at walls, v = -(v*e)>>8; roughness scatters
pool.attract(p, strength);               // inverse-square, near-field clamped
pool.spray(emitter); pool.angleEmit(emitter, angle16, speed);
pool.collide();                          // optional; x-binned broad phase, impulse response
pool.render(canvas, palette);            // sub-pixel splat per live particle; ttl fades brightness
```

**Defaults (standard approach, PO decision):** `render()` composites **additively with saturation** (light adds; hue-preserving rescale on overflow, never clip-to-white) through the **sub-pixel splat** — the effect writer gets both without deciding. Case-by-case is opt-OUT: `RenderStyle::Hard` for single-pixel retro rendering, nothing else to choose. Trails are deliberately NOT a pool feature: decay stays the one existing mechanism (the collected `fadeToBlackBy`), so the system has a single decay path rather than a second one hidden inside particles.

Costs (from the bottom-up's measured prior art): step ≈ 6 ops/axis, splat ≈ 4 mul + 4 saturating adds, collide only when enabled. Budget ~40 cycles/particle/frame without collisions. Pool size is the effect's choice against its ScratchBuffer — the pay-for-what-you-use rule; nothing static.

**What the kernel is FOR (clarified 2026-08-07, PO):** new effects, first. Anything that behaves
like matter — sparks, rain, snow, smoke, confetti, a fountain, a swarm, debris, an audio band
throwing off embers — is the same forces over the same state, so a working integrator + emitter
makes a new look a few lines of composition. Converging the effects that already hand-roll this is a
real but SECOND goal: it follows from the kernel being right, and each move is bench-judged on its
own rather than done as a batch.

**Convergence outcome (2026-08-07), checked one at a time against the code rather than as a batch:**

- ✅ **Particles** — converged. The private 12.4 struct, the hand-rolled integrate and the four wall
  tests are gone; the pool owns them. No golden moved.
- ✅ **StarField** — converged onto `shader::project`. Its float pinhole and the integer form were
  verified identical across 1264 samples of the (x, z) range it produces, and its golden did not
  move — which is the proof that matters, since a ported effect that looks different is a regression.
- 🔨 **StarSky — NOT converged, and should not be.** Its stars never move: the index is assigned at
  respawn and only the brightness animates, so there is no position, velocity or force to share.
  Converging it would mean a linear index stored in a position field, four unused velocity arrays,
  and a pool that never calls `step()` — more state and less clarity than the four small arrays it
  has. The plan listed it for "SoA aging = ttl", but the aging IS the whole effect rather than a
  particle behaviour.
- 🔨 **BouncingBalls — worth converging, and it is a REWORK not a swap.** Assessed 2026-08-07.
  Its physics is the closed-form projectile equation (position from elapsed time), which is exact,
  cheap and already framerate-independent — so Euler alone would be a downgrade: measured against the
  closed form it drifts 8 cm low on a 1 m trajectory after one second, more than a pixel on a 16-row
  panel, and the error grows every bounce. What the kernel genuinely adds is what the closed form
  *cannot* express: balls that collide with EACH OTHER and can move sideways, instead of each being
  trapped in its own vertical lane. Under the corrected migration rule ("beauty is the goal") that is
  an improvement worth making and worth judging on the panel. The work is real though — the effect
  keeps up to 16 balls per COLUMN (hundreds on a wide panel) in per-column arrays, so converging means
  one shared pool plus a decision about how many balls a wide fixture should have. Its own commit.
- ✅ **Tetrix — NOT converging, correctly.** The plan already said "the state machine keeps its logic,
  positions ride `pos_t`". Its falling brick is a state machine (idle → start-roll → falling → landed)
  with a stack height per column, not a particle: nothing bounces, nothing collides, and `pos` is a
  single scalar per column. It already uses the shared `FrameTime` for its fall rate and start-roll,
  which was the part worth sharing.

The five convergence candidates and what each would pin: Particles (12.4 → 24.8, wall bounce), BouncingBalls (analytic float → Euler + restitution — the named non-identical case, bench-judged), StarField (perspective divide stays effect-side; the pool carries state), StarSky (SoA aging = ttl), Tetrix (state machine keeps its logic, positions ride `pos_t`).

## 4. MoonLive requirements 🔨 *(stage 3, deferred by the PO — do this after the library is complete)*

What the builtin surface needs from the engine, recorded for the livescripts work:

1. Built-in table ≥ 64 entries (today 16).
2. Typed multi-arg host calls, ≤ 6 args + optional return (today: one `uint32_t` in, one out — `drawLine` is inexpressible).
3. Script symbols `x/y/z/w/h/d/time` — already threaded to the runtime entry point, needs only grammar exposure.
4. **Two entry shapes:** `frame()` (compose kernels — the scalable path per the 293-cycles/pixel math) and `pixel(x,y,z)` (the PixelBlaze-ergonomics path, honest ceiling ~32×32 interpreted). Scripts choose; large fixtures use `frame()`.
5. Stateful objects (a `Pool`, a `BeatPhase`) exposed as *handles* — script-declared, arena-allocated at compile, passed as an opaque first arg. No script-side memory management.

Until the ABI lands, stages 1–2 proceed compiled-side; nothing here blocks on the engine.

<a id="migration-plan"></a>

## 5. Migration plan and example effects ✅ *(11 of 12 showcases built; VectorBalls landed 2026-08-07)*

Order by leverage, cheapest risk first; every batch lands with its tests and the branch stays under ~100 files. **These five phases are the project's one numbering for this work** — the bottom-up document's nine *families* group functions by algorithm, while the phases below group them by what lands in the repo together, so each phase names the families it carries.

1. ✅ **Foundations** — `math16.h`, `Canvas`, `BeatPhase`, `map16`: mechanical replacement in the 9 phase-accumulator effects, the 6 `imap` copies, the 22 preambles, the 16 `depthDim()`s. Pixel-identical (same arithmetic, one home) → golden-frame pinned. *(families 5 Time & motion, Support)*
2. ✅ **Geometry** — `bar/rect` into the 4 audio meters; `scroll` into FreqMatrix; `splat` lands with its unit tests; the SDF trio + `smin` + `coverage`. *(families 1 Frame ops, 2 Pixel ops, 3 Geometry)*
3. 🔨 **`particles`** — the kernel is ✅ built and two new effects use it; the five convergences are OPEN. Per the PO the kernel targets future effects first, so each convergence is bench-judged on its own rather than done as a batch. The kernel + the five convergences, one effect per commit, bench-judged (PS-replaces-twin decision); the old private representations deleted. *(family 6)*
4. ✅ **Fields + polar** — the shared blob oscillator (LavaLamp ≡ Metaballs) onto `sin16`+`splat`; Rings/Spiral onto `PolarLut`; `noise16` under Noise2D with a rescale note. *(family 4 Fields)*
5. ✅ **Hidden-modifier extraction** — audit each effect for a transform that belongs to the modifier chain as it migrates (the effects-vs-modifiers decision). *(no family: an orthogonal cleanup the migration surfaces)*

   **Audited 2026-08-06, and nothing was extracted.** The named candidate was "FreqSaws `invert` →
   MirrorModifier". Two findings, in order:

   - That mapping does not hold. `MirrorModifier` folds an axis onto itself, HALVING the logical
     extent; FreqSaws flips alternate columns end-to-end at full extent. Different transforms.
   - A general `WeaveModifier` was then built and **reverted**. The right test is "does this add
     value to every effect?", and on the grid it looked like it did. But the control exists for
     FreqSaws columns mapped onto RINGS, where flipping alternate columns makes adjacent wheels
     appear to counter-rotate. Measured against `WheelLayout`: a spoke spans many grid columns
     (spoke 0 covers columns 6-11, spoke 3 runs 5 down to 1), so a column flip cuts across spokes
     and cannot reproduce that look. The modifier would have carried the name of an effect it does
     not achieve. `invert` stays in FreqSaws, where the geometry it depends on is known.

   The lesson worth keeping: "would this help every effect?" is the right question, but answering it
   requires knowing what the control is FOR. Here the intent lived in the product owner's head, not
   in the code or its comment.

**GEQ3D checked, and it does NOT want `project` (2026-08-07, PO asked).** The family-9 entry lists it
as one of three effects hand-rolling projection, but reading it: GEQ3D draws its perspective as
`draw::line(..., shorten)` running toward a moving vanishing point — the third form that entry names,
not the `1/z` divide. It is therefore ALREADY on a shared primitive, and pushing `project` into it
would replace a working construct with a worse-fitting one. StarField's `1/z` pinhole is the genuine
`project` candidate; RubiksCube's voxel-to-face classification is a third thing again.

Families 7 Color, 8 Random and 9 Projection carry no phase of their own: they land inside whichever phase first needs them (`cosPalette`/`gamma8` with the showcases, `hashInt` with Dissolve, `project` with VectorBalls).

**Commit split (PO decision, 2026-08-06).** The remaining work ships in two commits: **everything except particles and shaders now** — the rest of phase 2, then phases 4 and 5, with their showcase effects — and **particles plus the shader tier next** (phase 3, `FireworksEffect`, `BallpitEffect`, `RaymarchEffect`). The split keeps each commit reviewable line-by-line and well under the ~100-file CodeRabbit ceiling; it also puts the two items needing bench judgement (the PS-replaces-twin decision, the `hasHeavyCompute` float exception) together in one commit rather than spread across both.

**Golden-frame harness (new, small):** render N frames at fixed seed/fixed `elapsed()` into a buffer, hash, compare against a checked-in golden. Only for effects claiming pixel-identical; a deliberate divergence replaces the golden in the same commit with the bench note. Lives beside the existing effect tests.

Two things learned building it (2026-08-06), both by mutation-testing the harness rather than trusting it:

- **A short render proves nothing.** At a typical default speed the phase advances a few units over 8 frames, moving nothing by a whole pixel on a 16-wide grid — the hash compared two near-identical frames and passed even with the animation perturbed 7x. The harness renders 200 frames (4 s) for that reason.
- **A golden is only as strong as the effect's visible output, and it is NOT a statement that the effect looks good.** It pins what the code renders today so a "changes nothing" refactor can be checked. Several effects are awaiting a tuning pass (some generated rather than derived, with arbitrary parameters — two saturate their field to full brightness at default settings and render a nearly static frame). When tuning moves a golden deliberately, that is the system working. The only rule is that no hash moves *silently*.

**Corollary for the migration: power-function work and effect tuning feed each other.** Migrating an effect surfaces what its parameters actually do (the saturation above was found by a phase mutation, not by looking), and tuning decisions then re-baseline the goldens. Neither waits for the other; the goldens simply record where each effect stands.

### Stage 2 — new showcase effects

The goal is **beautiful effects, not conditioned ones** (PO decision): each showcase leans on the toolbox for its heavy lifting — the named power functions carry the effect's core mechanic — and is otherwise free to add any effect-local code that makes it better. That is the coverage proof (the library did the hard part) and the reference value (a writer sees the functions in real use), without a purity rule that would make an effect worse to keep a list clean. New showcases exist only where stage 1's migrations do not already exercise a family; everything else is proven by the rewrites themselves.

| Effect | Showcases | Power functions exercised |
|---|---|---|
| ✅ `FireworksEffect` | **particles** — the full kernel in one look | `Pool`, `spray`/`angleEmit`, `gravity`, `drag`, ttl fade, sub-pixel `splat`, additive default |
| ✅ `BallpitEffect` | **particles collisions** — the piece Fireworks leaves off | `collide` (binned, impulse), `bounce` + wall roughness, `force` tilt via controls |
| ✅ `SdfShapesEffect` | **the shader look** — anti-aliased morphing shapes | `sdCircle/sdBox/sdSegment`, `smin`, `coverage` AA, `cosPalette`, `beatPhase` |
| ✅ `PolarNoiseEffect` | **the Petrick idiom** — the named coverage target | `PolarLut`, `fbm16`, `warp16`, `colorFromPalette`, per-target headroom |
| ✅ `WaterRippleEffect` | **the field kernels** — a true propagating simulation (the existing `RipplesEffect` is closed-form) | `ripple` (Elias two-buffer), `splat` drops, `blur` |
| ✅ `RaymarchEffect` *(any target with a hardware FPU)* | **the ceiling clause made visible** — a raymarched 3D SDF scene (rotating smooth-min blobs, soft shadows, the Quilez canon) | the SDF *concepts* in 3D; `cosPalette`; gated on a `hasHeavyCompute` platform constant (the `hasNetwork` pattern) — streamable to a real wall via NetworkSend. **Per-light float is a stated exception here, bounded by measurement** (see § the float exception below) |
| ✅ `TunnelEffect` | **the gather primitive** — the structural gap the canon survey found; one effect proves the whole texture-mapping third of the canon | `sampleWrap` (G1), `mat23` (G3) for the per-frame rotation, `PolarLut`, ping-pong buffers, `cosPalette` |
| ✅ `EchoEffect` | **feedback composition** — that feedback is 3 lines once gather exists, not a primitive (the survey's own argument, made visible) | `sampleWrap` + `fade` + `combine` (G2, screen/max op), ping-pong swap convention |
| ✅ `VectorBallsEffect` | **projection + filled geometry** — a rotating 3D object, the classic demoscene proof | `project` (family 9), `depthSort`, `fillTriangle` (G6), `lineAA`, `circle/fillCircle`, `mat23` |
| ✅ `SpectrumEffect` | **the audio primitives** — replaces GEQ's hand-rolled meter machinery with the real ballistics | asymmetric envelope (G4), `peakHold`, `smoothFollow`, `map8_to_16`, `bar`; beat-locked motion via the audio service's onset/PLL (G5) |
| ✅ `DissolveEffect` | **stateless randomness + dithering** — a transition carrying zero per-pixel state | `hashInt` (position-addressable), `bayerDither` (G7), `easeInOutQuad` (Penner), `gamma8` |

Attached to the effects above rather than earning their own: 🔨 `worley` (G8) was deliberately NOT built — [backlogged](backlog-light.md) on the industry audit's advice, since no effect wants cells yet; `attract` joins `BallpitEffect` (an attractor well the balls fall into); `kaleido` joins `TunnelEffect` (the same polar LUT, folded); `quadwave/cubicwave` and `isqrt/dist16` are used wherever they are the cheaper shape, not showcased for their own sake.

✅ Added after this table was written: **`TruchetEffect`** — the representative 2D shader (space folding, position-addressed variation, distance + smoothstep), cheap enough for any target, and a better introduction to the form than the raymarcher.

Families with no new effect, deliberately: frame ops, geometry bars, time/motion and color are exercised by the stage-1 migrations (the audio meters, the nine `beatPhase` conversions, the 27 palette users); the CA kernel already has `GameOfLifeEffect`; `text` has `TextEffect`. A showcase that duplicates a migration would not earn its place.

**How far each type goes (coverage vs limits, stated up front):**

- **Particles**: everything the WLED-PS canon expresses (32 effects' worth of emitters/forces/collisions/fire) is expressible. Ceilings: *count* — thousands on ESP32 (~40 cycles, ~16 B each; 2048 ≈ 0.34 ms/frame), far more on desktop, never GPU-class millions; *deferred families* — constraint chains (Verlet/Jakobsen rope-cloth) and boids wait for a consuming effect; WLED-PS's per-particle size/wobble renderer is covered by SDF-circle glow instead of a second render path.
- **Petrick idiom**: fully expressible (polar + layered warped noise + palette). The limit is per-pixel budget, not vocabulary: 5–10 field samples/pixel is full-rate on ≤32×32 classic, medium sizes on S3, uncapped on desktop — but a 128×128@50 wall affords ~1 sample/pixel. Animartrix itself is FPU-bound to Teensy/S3-class at moderate sizes; the escape hatches are half-resolution field + upscale (the virtual-layer downscale lever), a field rate below the render rate, or desktop headroom.
- **Shader look**: anti-aliased shapes, outlines, glow, smooth-min morphing — yes, everywhere; general Shadertoy — never via GLSL (it is composition of our kernels, not a transpiler), and on ESP32 **it depends on the fixture size, not on the chip**. The budget is per pixel, so it scales with pixel count (240 MHz, measured): **16×16@60 = 15,600 cycles/pixel** (raymarching, fractals and feedback all reachable — a small panel is a legitimate shader target), **32×32@60 = 3,900** (rich multi-sample fields), **64×64@50 = 1,170** (a few samples), **128×128@50 = 292** (one field sample + palette + blend). So an advanced shader effect is not "desktop-only" — it is *small-fixture-and-desktop*, and the same effect simply needs a bigger machine as the wall grows. An effect that wants both can scale its own sample count from `nrOfLights()`. Three SDFs ship (circle/box/segment); more of Quilez's catalog only with a consuming effect. **On desktop the ceiling clause applies**: thousands of cycles per pixel make raymarching, fractals and feedback genuinely reachable — `RaymarchEffect` is the named showcase, gated on a `hasHeavyCompute` platform constant, and desktop frames stream to physical fixtures over NetworkSend, so the heavy tier lights real walls, not just the preview.

  **The float exception, stated rather than implied.** [coding-standards](../../contributing/coding-standards.md) prefers integers and bars per-light float on the render path; a raymarch loop is per-light float by nature, so `RaymarchEffect` needs an explicit exception rather than a quiet one. Its bound: the effect is **compiled only where `hasHeavyCompute` is true**. **Revised 2026-08-07 (PO): that is targets with a hardware FPU — desktop, ESP32-S3 and ESP32-P4 — not desktop alone.** The original desktop-only framing made a decision on the wrong axis: the cost is per PIXEL, not per chip (measured 0.30 ms/frame at 32x32 on desktop), so a small panel on an S3 is a legitimate target while a 128x128 wall is not, on any hardware. The classic ESP32 has no FPU and carries none of the code, so the rule stands unweakened where it matters most. Running it on a small ESP32 panel — which the cycle budget above says is arithmetically reachable — requires that constant to be true for that target, which is a **separate, measured decision** (single-precision FPU on S3/P4, none on classic ESP32), not something this showcase grants. Every *portable* power function stays integer; this is one gated effect, not a precedent for the contract.

## 6. Resource accounting ✅ ❓ *(the measurements hold; the flash-delta projections were never re-checked after the kernels landed)*

Verified against CLAUDE.md § Principles and [architecture.md § Hot path discipline / § Core and light domain](../../explanation/architecture/index.md). What the set costs, what it removes, and the gates that keep the balance visible:

- **Flash:** the 16-bit tier costs **130 bytes of table** plus code. The zero-table variant (interpolating the existing 8-bit `sin8_lut`) was implemented first and **rejected on measurement**: rounding the endpoints to 8 bits distorts the segments the interpolation runs between, giving 1.1% of amplitude — worse than the 0.69% it was supposed to beat. Measured against FastLED **master** (b2a1344): classic `lib8tion sin16` 0.69%; **ours 0.031%** (130 B); master's `fl::sin32` near-exact but 1040 B plus two int64 multiplies per call. 130 bytes for a 22x improvement over lib8tion is the minimalism call — and the estimate-then-verify order is the lesson: the first design's headline number was an unmeasured guess. New kernels (particles, geometry, SDF) add low-single-digit KB; the migrations *delete* the nine phase accumulators, six `imap`s, sixteen `depthDim`s, five private particle representations and the local `plot`/`triangle8` re-implementations, and PS-replaces-twin removes whole effect bodies (WLED's same move saved ~12 KB). **Gate: the per-target flash table in repo-health is read per migration batch; a batch that grows flash needs its reason in the commit.**
- **RAM:** everything sized is `prepare()`-time ScratchBuffer/`platform::alloc` (PSRAM-preferred), zero static — `check_footprint` enforces. Two honest costs, stated rather than hidden: a 2D particle at `pos_t` is ~16 B vs WLED-PS's 10 B (the price of addressing a 16K strip WLED's int16 cannot; pools are effect-sized, so small fixtures pay small); `PolarLut` defaults to **8-bit r,θ (2 B/pixel — 24 KB on 48×256)**, with the 16-bit variant (4 B/pixel) as an explicit opt-in — large fixtures require PSRAM already (`nrOfLightsType` gates on it).
- **Cycles:** per-light work is integer throughout (the fixed-point-default decision); budgets in § Testing; the KPI tick gate catches a regression at its cadence.
- **Repo:** golden-frame tests store **hashes, never frame blobs** — repo-health's size trend stays flat.
- **Boundary:** `math16`/`noise` are domain-neutral core (no light knowledge — "core primitives, not one-offs": each has many callers by construction); `draw`/`particles`/`polar` are light domain. No new mixing.
- **Complete construct, real consumer:** per architecture.md's surviving rule, each power function is built as the cleanest complete version (no crippled subsets) — and lands in the same PR as its first real consumer, so nothing ships speculatively: `beatPhase` is *extracted from* the nine effects that prove it.
- **Subtraction closes the loop:** after stage 1, `math8.h` keeps only entries with remaining callers (palette/hue and internal fast paths); superseded 8-bit forms and the temporary `(Buffer&, dims)` overloads are removed, and the five converged effects' private state code is deleted, not deprecated.

## 6b. Determinism ✅ 📖 *(the rule now lives in [the architecture](../../explanation/architecture/moonlight.md#effects); supersync's controls ride OSC, its shared clock is unbuilt)*

A planned capability — **supersync**, one effect rendered across several devices — constrains this API, and honoring it now is nearly free while retrofitting it later is not. The requirement: two devices given the same time and the same controls must produce the same frame, without exchanging pixels.

**The rule: a power function is a pure function of (position, time, seed) unless it has a stated reason not to be.** "Time" means a **shared origin**, not each device's own `elapsed()` — that is the part the rule stands or falls on, so it is stated first:

- **A shared epoch, distributed once.** Devices agree on a common `t0` and derive `syncTime = now - t0` from it; `elapsed()` (milliseconds since *this* device's render start) differs per device by however long each has been powered, so two devices reading their own clocks agree on nothing. Which device is authoritative, and how the epoch is distributed and corrected for drift, belongs to supersync's own design, not here. What belongs here is the seam: every time-driven power function reads **one** time source, so pointing that source at a synced clock is a wiring change rather than a rewrite of nine effects.
- **Quantised, so rounding cannot split the group.** `hashInt(x, y, t, seed)` and any other time-seeded randomness take a **quantised** time — a frame index derived from `syncTime / frameMs`, not raw milliseconds — because two devices sampling a continuous clock a millisecond apart would otherwise hash to different values and render different pixels. Quantising makes "close enough in time" mean "identical output".
- **Stateful kernels resync by replay or keyframe.** Particles, ripple, fire and CA evolve state that no formula reconstructs from time alone, so a device that joins late or drops a frame cannot catch up by computing harder. Two mechanisms, both standard in lockstep networking: **deterministic replay** (same seed + same input sequence from the epoch → same state, which works when the input is small and the history short) or an explicit **keyframe** (the authoritative device ships the pool/grid state periodically). Which one per kernel is a supersync decision; what this document fixes is that each kernel exposes a deterministic re-seed entry point so either is possible.

Three further consequences, each checkable:

- **Time, never frame count.** `BeatPhase` already satisfies this — it integrates `elapsed()`, so a device that drops frames still arrives at the same phase. This is the property that makes the nine-accumulator migration *more* than tidying: each hand-rolled copy also added `now * bpm` on its first tick, so its phase depended on device uptime and two devices could never agree. That is removed by construction (verified: it is the sole cause of the one golden that moved).
- **Position-addressable randomness beside the stream.** `Random8` advances per *call*, so a device that renders one extra frame — or a different light count — desynchronizes permanently and never recovers. `hashInt(x, y, t, seed)` (identified in the canon survey as the dissolve-transition primitive) is the supersync form: ask "what is this pixel's random value" rather than "what is next in the stream". Both ship; the hash form is the default for anything a synced effect uses, the stream stays for effects that are legitimately local.
- **Stateful kernels declare a resync point.** Particles, ripple, fire and CA carry evolving state that cannot be recomputed from time alone; a lost or late device cannot silently drift. Each exposes a deterministic re-seed from (time, seed) so a joining device can be placed into the same state — the same "keyframe" idea lockstep networking uses. Their *inputs* (emitters, forces) stay pure so only the state needs syncing, not the physics.

Two non-goals here: this does not specify the sync protocol (clock distribution, keyframe cadence, and which device is authoritative are supersync's design, not the power functions'), and it does not forbid local-only effects — it requires that an effect which *wants* to be synced can be, without rewriting the primitives underneath it.

## 6c. Two questions answered by the migration so far ✅

**Does using power functions make an effect more 3D-compatible?** Indirectly yes, but it is not automatic and it is worth being precise about which half it solves. Measured today: 21 effects declare `D2`, 13 declare `D3`, and 12 never read `depth()` at all. The `draw::` primitives are already dimension-generic (`line` is 3D Bresenham, `blur` covers every axis with extent > 1), so an effect built from them inherits 3D addressing for free — `FixedRectangle` and `PaintBrush` are `D3` on 3-4 `draw::` calls, while `Plasma` hand-rolls a `for z` loop to reach the same place. So power functions **remove the mechanical barrier** (addressing, extents, clipping, the `depthDim()` guard) and the `Canvas` migration removes it for the remaining 22 preambles.

What they do *not* remove is the conceptual one: an effect is 3D when its idea is 3D. `Metaballs` computes `dx² + dy²`; no primitive turns that into a sphere — someone must add `dz²`. The family that genuinely closes this is the one not yet built: the **SDF trio**, where `|p| − r` is a circle in 2D and a sphere in 3D from identical code (§ the dimension audit). Expect 3D coverage to move with family 3, not with the current batches.

**Is all the power functionality out of the effects yet?** No — roughly a third. Extracted so far: the nine BPM accumulators and five `imap` copies (the two highest-count patterns). Still embedded, from the bottom-up inventory: **22 `Canvas` preambles and ~15 `depthDim()` copies** (only StarSky migrated), **14 effects hand-rolling flat-index pixel writes**, **5 different particle representations**, **4 different distance implementations**, **3 private scratch-plane fade-and-blit idioms**, `FreqMatrix`'s scroll, and **4 bar-fill implementations**. Each is already a named candidate in the catalog; the remainder lands with families 1-3 and 6.

## 6d. Live performance ("DeeJaying") ❓ *(an argument that it is reachable, not a built capability)*

A stretch goal worth recording because the infrastructure is largely built: **playing effects live from the control surface — pads, faders, encoders — with no code changes.** What already exists: control changes reach a running module without a rebuild (`MoonModule::onControlChanged`), the surface routes faders and encoders through `Scheduler::setControl` (the same domain-neutral primitive IR and MQTT use), Effects composite with blend modes and opacity, and presets snapshot and restore whole subtrees.

Power functions sharpen this in a specific way: **the more of an effect's mechanics live in shared, control-driven primitives, the more of it is playable rather than fixed.** A hand-rolled accumulator is private state a surface cannot reach; a `BeatPhase` fed from a control is a tempo a performer can ride. The same holds for `particles` (gravity, drag, emission as live parameters) and the field family (warp amount, octaves).

What is genuinely missing, so the gap is not overstated:

- **Crossfade between states.** Presets apply instantly; a performer needs a transition (the `easeInOut` family plus Layer opacity is most of it, and applying a preset *into* a second layer rather than over the live one is the shape to consider).
- **Beat lock.** The audio service's onset/PLL (G5) is what makes `beatPhase` follow the music rather than a number — the difference between "animated" and "on the beat".
- **Per-control assignment.** `faderTarget` is currently hardcoded (`fader1` → `Drivers.brightness`); a performer needs to bind any control to any fader, which is a UI + persistence job on the existing seam, not new core.
- **Latency budget.** Untested end to end: a physical surface's control change must reach the render within a frame or two to feel live.

Deliberately not designed here — this is a capability the power functions and the ControlModule surface *enable*, recorded so neither is built in a way that closes the door on it. It also sets a direction for MoonLive: a script whose parameters are surface-bound is a live instrument, not just a compact effect.

## 7. Testing and budgets ✅ 📖 *(the harness rules live in `test/unit/light/golden_frame.h`)*

- **Unit**: every power function gets behavior-named tests (bounds, wrap, saturation, the fencepost cases the effects documented); the particle kernel gets the WLED-PS-derived edge list (tunneling lookahead, zero-distance pairs, sticky pile-up) implemented as behaviors, not ported assertions.
- **Golden frames** as above.
- **Scenario coverage, per family rather than one token case.** CLAUDE.md's rule is that a full pipeline gets a scenario test, and a family is only *finished* when one exists for it. The rule that decides which: a scenario is owed wherever a family changes the pipeline's **shape or timing** — allocation at `prepare()`, per-frame state, or a control that resizes something — because that is what a unit test on a bare buffer cannot reach. By family: **particles** (pool allocation, control-driven emission, the collision path), **fields** (a `PolarLut` sized at prepare and rebuilt on a resize), **frame ops** (`scroll` + `blur` on a live pipeline at several grid sizes), and the **desktop tier** (`RaymarchEffect` behind its platform gate). Families with no such surface — geometry, time/motion, color, random, projection — are covered by unit tests plus the golden frames of the effects that consume them, which is the approved exception rather than an omission.
- Each scenario lands **with its family**, not batched at the end; a family whose scenario is missing is not done.
- **Perf**: host micro-bench per function (a small `bench_powerfunctions` target, numbers into performance.md); on-device via the existing `collect_kpi` gate per migration batch. Ceilings: 293 cycles/pixel composite at 128×128@50; ~40 cycles/particle; `sin16` ≤ 12 cycles; `splat` ≤ 30.
- **Footprint**: `check_footprint` zero-static for every family; pools and LUTs are ScratchBuffer/prepare-time only.
- **The final gate is the wall**: stage-1 batches 3–5 get judged on the big fixture, per the pixel-identical divergence clause.

## 8. Decisions for sign-off ✅ *(all signed off; this is the record of what was decided and why)*

1. Homes: grow existing headers + `math16.h`/`particles.h`/`polar.h`; no umbrella namespace (§1).
2. Types: `pos_t` int32 24.8, `angle16`, `frac16`, velocity 8.8 (§2).
3. `Canvas` + `EffectBase::canvas()`; old overloads subtracted after migration (§1, §2).
4. Noise: widen value noise to `noise16` now; gradient noise is a swap-in later (§ TL;DR).
5. Migration order and the golden-frame harness (§5, §7).
6. MoonLive requirement list handed to the livescripts work; stages 1–2 do not block on it (§4).
7. Determinism: pure-function-of-(position,time,seed) as the default, `hashInt` beside `Random8`, resync points on stateful kernels (§6b).
8. The resource accounting and its gates: flash read per batch, PolarLut 8-bit default, goldens as hashes, complete-construct-with-real-consumer, the math8 subtraction pass (§6).

Carried unchanged from the bottom-up: dimension-generic; one set everywhere without capping desktop; demo effects/pixel-identical-by-default; effects-vs-modifiers; Petrick coverage target; one codebase; `particles` naming; PS-replaces-twin; the 16-bit contract. Added on review (PO, 2026-08-06): **particle blending and fixed point are defaults, not per-effect decisions** — additive+splat rendering out of the box with a single opt-out, and the fixed-point vocabulary invisible to the writer (§2, §3).

## Out of scope *(deliberate non-goals, not open work — an unmarked list on purpose)*

Exact per-function signatures beyond §3's shapes (the PR is the spec); the MoonLive grammar/ABI design (livescripts work); GPU acceleration of the contract on desktop; boids/Verlet-constraints/Porter-Duff (below-the-cut list stands); palette-system changes beyond `cosPalette`; a public "effect SDK" doc page (falls out of the migrated effects + catalog when stage 2 lands).
