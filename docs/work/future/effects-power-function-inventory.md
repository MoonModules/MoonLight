# Effects × power functions — inventory

> **Forward-looking backlog document — exception to CLAUDE.md present-tense rule.** One row per effect in the tree on 2026-09-03 (58), recording what each uses today and which power functions it could run on: the ones that exist ([power-functions.md](../../moonmodules/light/power-functions.md)) and the ones the [generative-fields top-down](generative-fields-analysis-top-down.md) builds (gradient noise, `PolarLut`, `Oscillators`, `advect` and the velocity rules, `decay`, `lineAA`/`disc`; the 16-bit Layer was proposed and then reversed, top-down § 11). It is the input for the catalog sweep that top-down names as a follow-on plan, under the product owner's rule: **every effect runs on the power functions for whatever they cover; an effect that does not is rewritten on them; algorithmic effects keep their logic; a rewrite lands only if at least as beautiful as the effect ran before.** Rows shrink as effects are rewritten; a rewritten effect's row is deleted, not ticked.

**Kind.** N = natural motion (physics, fields, noise, trails, rotation, oscillation): rewritten on the kernels. A = algorithmic (a rule set or a game is the effect): keeps its logic, draws through the library. M = audio meter (bars, levels, spectra): the geometry and ballistics rows apply. U = utility or test: no rewrite owed. Anything in doubt is N.

**Uses today** is read from the source: `noise` (`inoise*`/`fbm`/`warp`), `polar` (`atan16`/`dist16`/`kaleido`), `beat` (`BeatPhase`/`beatsin`), `sin` (`sin16`/`sin8`), `fade` (`fadeToBlackBy`), `particles`, `splat`, `blur`, `geom` (`draw::line`/`rect`/`bar`), `shader`, `scratch` (`ScratchBuffer`), `rnd`, `pal` (`colorFromPalette`), `audio`, and **float** where the effect still computes per light in float.

| Effect | Kind | Uses today | Could use, existing | Could use, to be built | Note |
|---|---|---|---|---|---|
| AudioSpectrum | M | geom pal audio | `bar`, `smoothFollow`, `peakHold` | 16-bit bars (smooth tops) | |
| AudioVolume | M | pal audio | `smoothFollow` | `decay`, `Oscillators` for the idle breathe | writes raw bytes: migrates to `draw::` in the 16-bit phase |
| Ballpit | N | particles scratch rnd pal | on `particles` already | 16-bit `splat`, `decay` | |
| Blurz | N | fade blur rnd pal audio **float** | `disc` via `coverage`, `blur` | `decay` by half-life, `disc`, `Oscillators`; float to fixed | |
| BouncingBalls | N | fade scratch rnd pal **float** | `particles` (mandated: gains inter-ball collisions), `splat` | `decay` | the power-functions top-down's "worth converging" is now "converge"; writes raw bytes |
| Breakout | **A** | beat rnd pal audio | `fillRect` | | game |
| DemoReel | U | rnd | | | orchestration, not rendering |
| Dissolve | N | beat rnd pal | `hashInt`, `easeInOutQuad` already | `Oscillators` for the timing | |
| DistortionWaves | N | beat sin pal **float** | `sin16`, `warp8` | `PolarLut`, `Oscillators`, `warp16`; float to fixed | a Family A shader in all but name |
| Echo | N | polar beat sin particles scratch pal | | `advect` with the spiral rule (zoom + rotation IS a velocity rule), `decay` | the prototype of Family B; becomes a few kernel calls |
| Fire | N | scratch rnd pal | `blur` for heat diffusion | 16-bit heat on the Layer, gradient noise for the sparks, `decay` | writes raw bytes |
| Fireworks | N | beat fade particles splat scratch rnd pal | on `particles` already | `decay` by half-life, `lineAA` for shell trails | |
| FishTank | N | beat particles scratch rnd pal audio | on `particles` for motion | `Oscillators` for tail and fin motion | sprite drawing stays |
| FixedRectangle | U | fade | | | test effect |
| FlyingToasters | A | beat particles scratch rnd audio | on `particles` for drift | | sprites; keeps its choreography |
| FreqMatrix | M | pal audio | `scroll` | 16-bit gradient, `decay` | |
| FreqSaws | M | beat fade pal audio | `bar` | `Oscillators` | its `invert` is a hidden mirror modifier (known extraction) |
| GEQ3D | M | beat fade geom pal audio | `bar`, `peakHold` | 16-bit bars | |
| GEQ | M | fade geom scratch pal audio | `bar`, `peakHold` | 16-bit bars | |
| GameOfLife | **A** | scratch rnd pal | `draw::pixel` only | | **kept as is**: the cautionary case; its logic is the effect |
| LavaLamp | N | beat pal | `blobField`, `smin` | 16-bit falloff, `decay`, `Oscillators` for blob paths | |
| Lines | U | | | | test effect; writes raw bytes |
| Lissajous | N | sin fade pal | `sin16`, `splat` | `lineAA`, `decay`, `Oscillators` | it is a Trails emitter; may be superseded by Trails |
| Metaballs | N | beat pal | `blobField`, `smin` | 16-bit field, `Oscillators` | |
| MovingHead | N | beat sin pal audio | `BeatPhase` | `Oscillators` for formations | fixture roles, not pixels |
| NetworkReceive | U | scratch | | widens into the 16-bit Layer | writes raw bytes by design |
| NoiseMeter | M | noise beat sin fade pal audio | `smoothFollow` | gradient noise, `Oscillators` | |
| Pacman | **A** | beat particles scratch rnd pal audio | on `particles` for motion | | game logic is the effect |
| PaintBrush | N | polar beat sin fade geom rnd pal audio **float** | `lineAA` via `splat` | `lineAA`, `decay`, `PolarLut`, `Oscillators`; float to fixed | |
| Particles | N | particles scratch rnd pal | on `particles` already | 16-bit `splat` | |
| Plasma | N | beat sin pal | `sin16` | `PolarLut`, `fbm`/`warp` as a shader, `Oscillators`, 16-bit | |
| Pong | **A** | beat rnd pal audio | | | game |
| Praxis | **A** | beat sin pal | | `Oscillators` | algorithmic palette pattern |
| Rainbow | N | pal | | 16-bit gradient (no banding), `Oscillators` | writes raw bytes; the default effect |
| Random | U | fade particles rnd pal | `hashInt` | | |
| Raymarch | N | beat sin shader pal **float** | on `raymarch.h` (FPU-gated by design) | 16-bit output | float is the design here |
| Rings | N | polar rnd pal | | `PolarLut`, `decay`, `Oscillators` | |
| Ripples | N | pal **float** | `dist16`, `sin16` | `PolarLut`, 16-bit; float to fixed | a radial shader |
| RubiksCube | **A** | rnd pal **float** | | | keeps its logic; its float rotation can go fixed later |
| SdfShapes | N | beat sin shader pal | on `shader.h` already | `Oscillators`, 16-bit coverage | |
| Sine | N | beat sin **float** | `sin16` | 16-bit; float to fixed | |
| Solid | U | scratch pal | | | |
| SpaceInvaders | **A** | beat rnd pal audio | | | game |
| Spectrum | M | geom scratch pal audio | `bar`, `smoothFollow`, `peakHold` already | 16-bit bars | |
| SphereMove | N | fade rnd pal **float** | `sdSphere` via `raymarch.h`/`shader.h` | 16-bit; float to fixed | |
| SpriteFountain | N | beat sin particles scratch rnd audio | on `particles` already | `decay` | |
| StarField | N | fade shader scratch rnd pal **float** | `shader::project` already | `splat`, `decay`; float to fixed | |
| StarSky | N | fade scratch rnd pal | `hashInt` | `Oscillators` for the twinkle, `decay` | the earlier verdict "not a particle" stands; oscillation is its mechanism |
| Tetrix | **A** | particles scratch rnd pal **float** | `FrameTime` | | state machine is the effect |
| Text | U | pal | `scroll` | 16-bit anti-aliased glyphs later | |
| Truchet | N | beat shader rnd pal | on `shader.h` already | `Oscillators` | |
| VectorBalls | N | beat sin shader pal | on `shader.h` already | 16-bit | |
| WaterRipple | N | scratch rnd pal | already a wide-state kernel | renders into the 16-bit Layer without narrowing; `disc` for drops; `decay` | |
| Wave | N | noise beat sin scratch pal | `sin16` | gradient noise, `Oscillators` | |

**Counts.** 54 rows remain of the 58 the table opened with, the difference being effects since rewritten on the kernels and deleted per the rule above. 11 still compute per light in float outside the FPU-gated raymarch (BouncingBalls, Blurz, DistortionWaves, PaintBrush, Ripples, RubiksCube, Sine, SphereMove, StarField, Tetrix, Raymarch by design). 17 effect headers write `buffer()` bytes directly rather than going through `draw::`; that is a measured OPTIMIZATION on the hot path rather than debt (a row pointer costs 0.26 ns/light where `draw::pixel` with known coordinates costs +170%), so it is not a migration target. The 16-bit phase that was to migrate them was reversed (top-down § 11).

**What the sweep would look like when it becomes a plan.** Batches by kernel, as each lands: the noise effects with phase 0; the polar and oscillation effects with phase 1; Echo, Lissajous, PaintBrush and the trail effects with `advect`/`decay`/`lineAA`; BouncingBalls onto `particles` on its own, since it is a rework with a bench judgment. Each batch under ~100 files, each effect compared against how it ran, the golden re-baselined with the reason. Effects a showcase supersedes (Lissajous by Trails, possibly Echo) are deleted rather than kept beside it.
