# Plan: particles in MoonLive, plus a fade builtin

> **Shipped 2026-08-22** in `cc51874b` (particles) and `537fc928` (the fade builtin that step 1
> called for). All five steps landed, verified on shiffy's 80x48. Three things went differently
> from the plan below, and the differences are the useful part:
>
> - **`collide` and `bounce` shipped**, though the plan rejected `collide` as an O(n^2) foot-gun.
>   Measuring changed it: 3.2 us at 48 particles against 0.1 us without, 53.6 us at 200. The
>   quadratic is real, the absolute cost at ball-pit sizes is not, and the numbers now ride the
>   builtin so an author knows what a big pool costs. `ballpit.mle` exists because of it.
> - **Four example scripts, not one.** `fountain.mle` alone made the API look narrower than it is,
>   because it pins the emitter to one place. `comet-trail`, `rain` and `ballpit` show that `emit()`
>   takes a position and an angle the script computes per frame.
> - **The plan's cost-model claim held, and then paid off unexpectedly.** `fountain.mle` measures
>   1,093 us against `metal.mle`'s 59,600 us on the same fixture. It also exposed a 1 Hz LittleFS
>   scan running inline on the render thread that no shader had ever revealed, because a particle
>   integrates a stall into its trajectory where a shader redraws past it.

## Context

`src/light/particles.h` is a complete fixed-point particle kernel: integrator, forces, wall
behavior, emitters, and a renderer that already reads the active palette. Nine C++ effects use it.
No script can, because nothing in the MoonLive stack includes it and no builtin exposes it.

This realizes backlog item **9b** in `docs/backlog/backlog-moonlive.md`, written this
session, which already names the two open questions: who owns the frame order, and what a second
script asking for a pool gets. Both are answered below.

**Why particles need a new seam at all.** A `Pool` is eight parallel arrays. At the 64-byte arena
and 8-member ceiling a script could hold about five particles against the hundreds a particle look
needs, and widening the arena is the wrong fix: `sizeof(MoonLive)` is held by value in every
scripted module and probed on the main task's stack by `registerType`, which boot-looped the P4 at
1440 bytes. Particle state must live OUTSIDE the arena. `ScratchBuffer<T>` is already exactly that
primitive: one `platform::alloc`, PSRAM-backed where the target has it, tied to its owning module,
freed by `MoonModule::release()`'s free-list walk, counted into `dynamicBytes`.

**Structs are NOT needed, and this is a real finding rather than a deferral.** Every Pool operation
is either whole-pool (`step`, `gravity`, `age`, `render`) or takes plain scalars (`spawn`,
`angleEmit`). A script never names a particle field, because the arrays live in ScratchBuffers the
script cannot address. The roadmap's struct items are about something else: #10 is `ball[i].x`
*instead of parallel arrays*, which a pool removes the need for, and #4b is `Coord3D`/`CRGB` for
per-pixel shader signatures, whose real prerequisite is the multi-value call ABI (#2). Both stay
where they are. Bundling a language-level type change into this branch would make one branch two
features and neither reviewable.

**The headline is the cost model.** `metal.mle` costs ~14 host calls PER PIXEL and runs at 17 fps
on shiffy's 80x48. A particle script costs ~9 host calls PER FRAME, with the per-particle work
inside C++ loops. This is the first script whose cost scales with the objects rather than the grid,
and measuring that on the wall matters more than the effect itself.

## Design

### 1. `fade(amt)`, FastLED's `fadeToBlackBy`

Not particle-specific and useful to every effect, which is why it leads. It lowers to
`layer()->fadeToBlackBy(amt)`, the idiom `FireworksEffect`, `BlurzEffect` and `FreqSawsEffect`
already use.

Deliberately the LAYER's collected fade rather than a direct buffer pass: `Layer::tick` MINs each
request into `fadeBy_` and applies it ONCE per frame before the effects run, so N fading effects on
one layer cost one buffer pass and the gentlest amount wins. A builtin that faded the buffer itself
would be N passes and would fight the other effects on a shared layer.

Reached through the same per-thread sink the canvas uses, so a script calling `fade` from a layout
or modifier does nothing, exactly as `line` and `setPaletteColor` already behave.

### 2. `MoonLiveParticles`, a new helper beside `MoonLiveScript.h`

Six `ScratchBuffer`s (x, y, vx, vy, ttl, hue), a `Pool`, and a `FrameTime`. Omits `acc` and `size`:
`particles.h` documents both as optional, `valid()` does not require them, and neither feeds a
builtin in the set below. `FireworksEffect` sizes exactly these six.

Not folded into `MoonLiveEffect`, which is a thin binding on purpose, and following the precedent
`MoonLiveScript` set this cycle: a concern three bindings would each grow their own copy of gets
one home, held by value. `MoonLiveEffect` gains one member, `MoonLiveParticles particles_{*this}`,
which is the declaration form `ParticlesEffect` already uses and which inherits the free-list walk
and `dynamicBytes` accounting with no new code.

### 3. `pool(n)` sizes it, from `defineControls()` only

`defineControls()` is the one moment that is after the compile, on the cold path, exactly once per
script edit, and already an established "the script tells the host something" event with a sink
built for it.

**A resize outside that moment is refused, not honored.** The pool sink is installed by the same
bracket `runDefineControls` already uses for the control sink; the per-frame builtins read a
separate handle that `MoonLiveEffect::tick()` installs and detaches next to `setDrawCanvas`. A
script calling `pool(400)` from `tick()` therefore gets a no-op returning the live count: no
allocation on the render tick, ever.

`pool(n)` returns the count actually available, so a failed allocation is visible from inside the
script. A script that never calls it allocates nothing, which is what stops every shader effect
from paying for particles it does not use.

Sizing follows live edits for free: editing the text recompiles, re-runs `defineControls`, and
`resizeBytes` reallocs only when the byte count actually changed. Editing a slider does not
recompile and does not touch the pool, which is correct.

### 4. Seven particle builtins

| builtin | args | why it cannot be omitted |
|---|---|---|
| `pool(n)` | 1 | without it there is no memory; the only non-per-frame call |
| `emit(x, y, angle, speed, n, life, hue)` | 7 | wraps `angleEmit`, the whole emitter story in one call. Exposing `spawn` instead puts the script back in per-particle land |
| `gravity(g)` | 1 | the force that makes matter read as matter |
| `drag(k)` | 1 | without it a pool under constant force accelerates until it teleports |
| `step()` | 0 | the integrator; nothing moves without it |
| `age(rate)` | 1 | without it particles are immortal, the pool fills, and `emit` silently stops |
| `render(maxLife)` | 1 | draws every particle and already reads the active palette |

The binding supplies the `FrameTime` scale to every one, so framerate independence is a property of
the system rather than something an author remembers to type. `step()` also runs `killOutside` at
the grid bounds: a particle that has left the grid draws nothing and costs a slot forever, so
leaking is a bug in every effect rather than a choice.

**Rejected:** `bounce` (5 args, 3 of them physics jargon; the closest call, and the first to add
later), `collide` (the only non-linear pass, an O(n^2) foot-gun in a language with no cost model),
`spray` (`emit` with a wide cone IS a spray), `spawn` (per-particle in a whole-pool API),
`force`/`forceSmall` (two-axis gravity, needs the `acc` buffer, buys wind nothing needs yet),
`attract` (a second effect, cheap to add later), `wrap`, `liveCount`, `clear`.

### 5. The frame order is the SCRIPT's

`gravity; drag; step; age; render` written out, not hidden behind one `frame()` call.

The usual argument against, call overhead, does not apply here: five calls per frame against
`metal.mle`'s ~57,000 per frame is unmeasurable. A hidden frame would need four arguments and an
invisible order where five one-argument calls have a visible one, and the explicit form matches
`FireworksEffect::simulate()` line for line, which is the stated goal of the whole scripting
surface. No order crashes, because every pass is bounds-safe on its own, so a wrong order costs a
look the author is already staring at.

### 6. `moonlive/effects/fountain.mle`

Sparks thrown from the floor, arcing over under gravity, fading as they die, with `fade()` giving
them trails. The nozzle leans on a slow sine so the plume sways. Roughly 12 lines of code.

The arc is not drawn: sparks leave at an angle, gravity pulls every frame, and where they turn over
is wherever the physics puts them. Turning `lift` up makes the fountain taller with no second
control saying so. That is the demonstration, and it is a different one from the shader's.

Comments stay minimal: a `.mle` is user-facing, shown in the device's own editor.

## Files

- `src/light/moonlive/MoonLiveParticles.h` — new: six ScratchBuffers, the Pool, the FrameTime,
  `resize`/`advance`/`release`
- `src/light/moonlive/MoonLiveBuiltins_light.h` — `fade` plus the seven particle registrations; a
  pool sink added to `detail::SinkSlot`, **and the matching fourth term in `releaseIfEmpty`** (its
  comment says the halves detach independently; missing this releases a slot while a handle is live)
- `src/light/moonlive/MoonLiveEffect.h` — holds `particles_{*this}`, installs and detaches the pool
  handle inside the EXISTING `setDrawCanvas` bracket, calls `particles_.release()` before chaining
- `src/light/moonlive/MoonLiveScript.h` — `sync()`'s `runDefineControls` bracket installs the pool sink
- `moonlive/effects/fountain.mle` — new; picked up automatically by the folder-walking compile sweep
- `test/unit/light/unit_MoonLiveParticles.cpp` — new
- `docs/moonmodules/light/MoonLiveEffect.md` — the vocabulary table
- `docs/backlog/backlog-moonlive.md` — delete item 9b, which this realizes

## Verification

**Unit**, each pinning a behavior a user could state:
- "a script sizes its own pool and gets the count it asked for"
- "a script that never calls pool() allocates nothing" — the pay-for-what-you-use rule on the
  memory that matters most
- "asking for a pool while the frame is running changes nothing" — **the most important test here**:
  the no-allocation-on-the-render-tick guarantee
- "editing a script to a different pool size resizes it" — the live-edit rule applied to memory
- "disabling a scripted effect frees its particles" — including that the pool reports invalid
  afterwards rather than pointing at freed memory (the trap `ParticlesEffect.h:60` documents)
- "a pool that could not be allocated reports zero and renders nothing" — degrade, never crash
- "a spark thrown upward comes back down" — the test that documents why the feature exists
- "emitting into a full pool stops rather than overwriting"
- "a script's particles fade and free their slots" — without it a long-running fountain silently
  stops emitting after a minute, which only shows up on the bench
- "two scripted effects each get their own particles" — the second open question, as a test
- "a particle call from a layout or modifier does nothing" — the absent-handle path
- "a fountain reaches the same height however fast the device renders" — proves the binding applied
  FrameTime rather than a raw 256, the exact class of bug the ParticlesEffect port once shipped
- "fading a layer twice in one frame costs one pass, at the gentler amount" — the collected fade

Not written: re-asserting `particles.h`'s own numeric contract, which `unit_Particles.cpp` owns.

**Scenario**: none needed. The MoonLive scenarios stage their own inline scripts, and `fountain.mle`
is compile-checked by the existing sweep.

**Hardware (the final guardrail)**: flash shiffy, run `fountain.mle` on the 80x48, and read
`tickTimeUs` off the card against `metal.mle`'s 59,600us on the same fixture. The claim under test
is the per-frame cost model, and only the bench settles it. Then the product owner's eyes.

## Steps

Each stops where the product owner can judge. Commit timing is theirs and not part of this plan.

1. **`fade(amt)`** with its test. Independent of everything else, useful to every effect, and it
   makes the fountain's trails possible.
2. **`MoonLiveParticles` + `pool(n)`** with the sizing and lifetime tests, including the
   `releaseIfEmpty` fourth term. The step with the real risk, because it allocates.
3. **The six per-frame builtins** with the behavior tests.
4. **`fountain.mle`**, the doc table, and deleting roadmap item 9b.
5. **Bench measurement** on shiffy, and the product owner's judgement on the wall.

Structs stay out, per the finding above. If a later effect genuinely needs per-particle field
access, roadmap #4b and #10 are where that argument belongs.
