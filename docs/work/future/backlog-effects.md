# Backlog: Effects

To-build items for Effects, grouped by module, each with its fields on one line. Other areas: [the future index](index.md).

## AuroraEffect

### Aurora hitches on the desktop and not on the S3 (2026-09-04)

Kind: Bug · Platform: desktop

A periodic hitch in the desktop preview that the same firmware at 64x64 on an S3 does not show. Ruled out by measurement: integer overflow, the drift sawtooth wrap (7 of 255 worst case), field speed (the desktop moves 15x less per frame), the contrast window's easing, and the preview transport at a large grid. The one difference left is about 385 fps against 25, so the next step is a desktop frame-rate cap: if the hitch tracks render rate, something is per-frame where it should be per-unit-time. A weighted layer crossover (cubed weights, 2.7x less frame-to-frame change for 3.4% more time per pixel) was built and reverted; it is worth another look once the hitch is understood.

## BlurzEffect

### BlurzEffect: a compounding blur has no rate carry

Kind: Bug

`Layer::fadeToBlackBy` takes a rate per reference frame and the Layer scales it once, the pattern every fade uses ([architecture § Where the machinery lives](../../explanation/architecture/moonlight.md)). `draw::blur` does not fit it: blur COMPOUNDS, so applying it twice at half strength is not one blur at full strength, and the fractional carry that makes a fade frame-rate independent produces the wrong result. BlurzEffect is the effect this bites. The open question is what the right construct is — a per-frame blur budget, a single blur at an accumulated strength, or leaving blur explicitly frame-gated and documenting it as not-a-rate.

## BouncingBallsEffect

### BouncingBalls onto the particle kernel: a rework, not a swap

Kind: Restructure · Platform: all

The effect computes each ball's height from the closed-form projectile equation in float, exact and frame-rate independent, so a plain Euler swap is a downgrade: against the closed form it drifts 8 cm on a 1 m trajectory after one second, more than a pixel on a 16-row panel. What the kernel adds is what the closed form cannot: balls that collide with each other and move sideways instead of each in its own vertical lane. The effect keeps up to 16 balls per column in per-column arrays, so the rework is one shared pool plus a decision about how many balls a wide fixture gets, judged on the panel against the original and landing only if at least as beautiful.

## DemoReelEffect

### DemoReel: a 3D test for the hosted D1/D2 extrude

Kind: Feature

DemoReel extrudes a hosted D1/D2 effect after its tick; its test hosts on a 2D grid only. Pin it with a DemoReel test that hosts a D1 and a D2 effect on a 3D grid and checks the spread.

## FixedPointEffect

### FixedPoint fades outside the Layer's aggregation (2026-09-07)

Kind: Restructure

`FixedPointEffect` calls `draw::fade` directly where every other fading effect calls
`layer()->fadeToBlackBy`, so it skips what the Layer adds: the per-frame aggregation (N effects on
one layer cost one buffer pass, gentlest rate wins), the framerate scaling, the sub-unit carry, and
the buffer-generation bump. Invisible until a second effect shares the layer.

The swap is one line and was tried: it makes the effect **3.1x brighter at high framerate than at
low**, against the 1.35x band `unit_Effects_framerate` enforces. The two calls mean different
things. `draw::fade(cv, n)` applies n every frame; `fadeToBlackBy(n)` is a RATE per reference frame
that Layer scales by elapsed time. The `fade` control (default 70, range 0..255) was tuned against
the first meaning, so moving it needs the default re-tuned and the trail looked at on a device, not
a silent swap.

Worth doing, as its own change: it is the last effect outside the shared fade path, and while it
stays outside, "every fade goes through the Layer" is not true.

## FluidEffect

### Speed up the fluid solver: 4 fps at 128x128, and it is not the divide (2026-09-05)

Kind: Feature

Measured on an S31 (RISC-V, 320 MHz, octal PSRAM at 200 MHz), `iterations` 5, depth 1:

| grid | Fluid tick | fps | cycles per cell-update |
|---|---|---|---|
| 32x32 | 8.5 ms | 109 | 151 |
| 64x64 | 38 ms | 24 | 159 |
| 128x128 | 204 ms | 4 | 205 |

A cell-update is four loads, three adds, a multiply and a store: under 20 cycles of arithmetic. It
costs **151 even at 32x32**, where the whole working set is small enough to cache, so the loop
itself is roughly 8x more expensive than the work it does. Memory adds a further 35% by 128x128 but
is not the wall: at 4 fps the solver moves ~25 MB/s, about 5% of what this PSRAM delivers.

**A wrong turn worth recording.** The first diagnosis blamed the 64-bit divide in `relax()`, on the
reasoning that Xtensa has no integer divide instruction. It was implemented (a power-of-two shift
dispatched once per call, bit-exact over 8M values) and measured on the board: **no change, 326 ms
before and after**, so it was reverted. Two errors: the S31 is RISC-V rather than Xtensa, and at
151 cycles per update the divide was never the dominant term. A desktop measurement could not have
caught either, since arm64 divides in hardware; only the board settles it.

**Allocation placement is worth 1.6x, and nobody chose it.** Same firmware, same grid, same
controls: **326 ms after a fresh boot, 204 ms after resizing the grid to 32 and back to 128**,
reproducible across reboots. The solver's six buffers are ~638 KB and land in PSRAM either way, but
where they land at boot is slower than where they land once the heap has moved. Whatever is done
about speed, this says a boot-time allocation can be paying a large penalty invisibly, and it is
worth understanding before optimizing the loop around it.

Ordered by expected return:

1. **Cut the 64-bit arithmetic in the inner loop.** Every cell computes an `int64` shift, an
   `int64` multiply and an `int64` divide on a 32-bit core, where each is several instructions and
   a register pair. This is the most likely source of the 151 cycles. A 32-bit formulation, or a
   narrower intermediate with a proven bound, is the first thing to measure. Precedent: the SWAR
   work found the 32-bit pair form bit-identical and 41% smaller.
2. **Hoist `idx()`.** The loop addresses five neighbors per cell through `idx(x, y)`, each a
   multiply-add. Walking row pointers instead is the standard fix and removes most of the address
   arithmetic.
3. **Understand the allocation-placement effect above**, since it is worth more than most loop
   tuning and costs nothing to trigger deliberately once understood.
4. **Solve the pressure at half resolution.** Pressure is smooth, so a half-scale solve with a
   bilinear upsample of its gradient costs a quarter of the cells. Same trade `fieldScale` already
   makes for noise fields, measured 3.0x there. Changes the picture slightly, unlike 1 to 3.
5. **Fewer iterations, documented per target.** Linear in cost: 5 to 2 is 2.5x, and the picture
   gets springier. The card should say what a target can afford rather than leaving a user to find
   4 fps.
6. **Question whether the full solver belongs on this class of board at all.** See the ColorTrails
   entry below: a separable noise advection gets a flowing, swirling picture for two passes over
   the grid and no solve. The fluid's own header already calls it a desktop and P4 effect.

**Why `fluid.mle` runs at 13 fps while the compiled effect runs at 3.** They are not the same
algorithm: the script is a curl flow, not a solver
([generative effects](../../explanation/generative-effects.md)). Per frame at 128x128 the script visits ~16k cells
and divides nowhere; the compiled solver visits ~429k, of which 318k carry the 64-bit divide. That
is 27x the work, and the measured gap is only 4.3x because the interpreter gives most of it back in
dispatch overhead. So a COMPILED curl effect would beat both. What curl cannot do is what a solver
does: no pressure, no interaction between jets, and no vortex forming out of the flow's own
history. Whether that is worth 27x is a question for the product owner's eyes.

**Measure on hardware, not on the desktop**: this is an in-order-core property and the desktop
divides in hardware, which is exactly how the wrong diagnosis above survived a desktop check.
Record before and after in performance.md per target.

## NetworkReceiveEffect

### E1.31 multicast receive (IGMP join)

Kind: Feature

NetworkReceiveEffect accepts E1.31 via unicast only — the same scope MoonLight ships. Multicast senders address the per-universe group `239.255.{universe_hi}.{universe_lo}`, which a receiver must join via IGMP. **The platform half of this now exists**: `UdpSocket::joinMulticast()` shipped with the WLED audio-sync work (2026-08-29) and is used in anger there, on both desktop and ESP32. What is left for E1.31 is the join-per-accepted-universe bookkeeping, not the socket support. Add when a multicast-only sender actually shows up on a bench; until then the spec documents "point sACN senders at the device's IP".

## Across modules

### The catalog sweep: every natural-motion effect onto the power functions

Kind: Restructure · Platform: all

[Migrating an effect](../../moonmodules/light/power-functions.md#migrating-an-effect) asks for it; [the inventory](analysis-effects-power-functions-inventory.md) records per effect what it uses today and which kernels it could run on, and a row is deleted when its effect is rewritten. Batches go by kernel: the noise effects, the polar and oscillation effects, then Echo, Lissajous and PaintBrush onto `advect`, `decay` and `lineAA`, each compared on the panel against how it ran and re-baselined with the reason. An effect a showcase supersedes, as Trails supersedes Lissajous, is deleted rather than kept beside it.

### Moving-head effects from MoonLight, including two of troyhack's (2026-09-04)

Kind: Feature

MoonLight has several moving-head effects that have no equivalent here, two of them troyhack's.
Migrate them all, on the power functions per the standing mandate rather than traced across.

### Sprite follow-ups

Kind: Feature · Platform: P4

Deliberately deferred when sprites landed: P4 PPA acceleration behind the same `draw::sprite`
signature (the 2D-DMA blitter the WLED-MM-P4 world uses via LovyanGFX; ours would sit in the
platform layer, no vendored GFX library), Porter-Duff alpha when a real consumer arrives, and
MoonLive sprite data (needs the stage-3 builtin table + arrays).

### A real 2D/3D PacMan

Kind: Feature

The migrated 1D PacMan and 1D Ant effects were removed — a chase rendered on a single strip reads as a blob of moving dots, not a game, so they weren't worth keeping. A proper **2D (or 3D) PacMan** — an actual maze, Pac-Man and ghosts navigating it, power dots, the blue-ghost flee state — would be a genuinely fun signature effect. Build it fresh as a grid game (not a strip port); the WLED/MoonLight 1D versions are reference for the state machine only, not the rendering.

### Add real z-axis variation to 2D effects

Kind: Feature

Only **NoiseEffect**, **PlasmaEffect** and **RipplesEffect** have z-aware math. The other honest-D2 effects use `Layer::extrude` to duplicate the z=0 plane, so every z-slice is identical on 3D layers. Candidates for genuine D3 promotion: Metaballs/GlowParticles (add z to blob coordinates), Plasma palette/Spiral (add z-driven phase term), Fire (z-drift heat grid), Rings/LavaLamp/Checkerboard/Particles (add z to each element). Prioritise after seeing real 3D installations; each promoted effect also needs its `dynamicBytes` budget for the full 3D buffer.

### worley cellular noise, when an effect needs it

Kind: Feature · Trigger: an effect needs it

- **`worley` cellular noise, when an effect needs it** (deferred 2026-08-07, PO). The one power function the plan names that stayed unbuilt on purpose. Worley (cellular / Voronoi) noise measures the distance to the nearest of a set of scattered feature points, which is what produces the look nothing else does — cracked mud, scales, stained glass, a caustic. The fields we have cannot fake it: `fbm` and `warp` are smooth by construction, so their creases never form cells.

  **Why it waits.** An industry-standardness audit (Fable, 2026-08-07) put it explicitly on the "do NOT add yet" list: a practitioner does not consider it table stakes, and every other field function in the library earned its place by having a caller. Building it now would make it the only entry whose justification is "the canon has one". The trigger to build it is an effect that wants cells — at which point it lands with that effect, the way `hashInt` landed with Dissolve and `sampleWrap` with Echo.

  **What it costs when it comes:** the standard cheap form is a 3×3 neighborhood scan around the sample's cell, hashing each neighbor's feature point and keeping the nearest distance — nine `hashInt` calls and nine distance tests per pixel, so meaningfully dearer than one noise sample. `dist16` and `hashInt` already exist, so the function itself is short; the cost question is what it does to a per-pixel budget on a large fixture.

### 16-bit noise: warp16 / turbulence16

Kind: Feature · Trigger: an effect wants a large smooth field

- **16-bit noise: `warp16` / `turbulence16`** (2026-08-08), unbuilt, to match the 8-bit compositions. The trigger is an effect that wants a large smooth field; it lands with that effect and its measurements, the way the 8-bit compositions landed with theirs.

### Clear the grid on an effect's FIRST frame

Kind: Bug

- **Clear the grid on an effect's FIRST frame** (2026-08-08). Fourteen effects still show the previous effect's picture on the frame right after a switch: BouncingBalls, Fireworks, FreqMatrix, FreqSaws, GEQ, GEQ3D, GameOfLife, Lissajous, NoiseMeter, PaintBrush, Random and three more. Most predate the power-function branch.

  **What the user sees.** Switching to one of these leaves the old frame underneath for a moment — an audio effect with no signal yet, or a simulation still seeding, paints nothing and inherits whatever was there. Owning the background is each effect's job ([Layer.h](../../../src/light/layers/Layer.h)).

  **Why it waits.** It is fourteen effects' worth of change across audio-reactive and simulation families, each needing its own judgement about whether to clear, fade, or seed differently — not a mechanical sweep. `unit_Effects_gridsweep.cpp` already measures it (`afterFirst`) and asserts only the settled frame, so the number is visible without blocking.
