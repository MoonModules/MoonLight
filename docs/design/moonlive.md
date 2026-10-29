# MoonLive: the language roadmap

[MoonLive](../explanation/architecture/moonlive.md) is the engine as built (grammar, IR, codegen), and [power-functions.md](../moonmodules/light/power-functions.md) is the builtin surface it calls into. This analysis is the ordered plan for closing the gap between them: which language features to add next, and why that order. Every limit quoted was measured against the shipped compiler, not read off the source. The steps are one backlog item, [The next language steps, in order](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287082542), and the code cap is [its own item](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287082596).

## What this is for

MoonLive compiles a C-subset to native code on the device. The subset is deliberately small: it started at "fill a buffer with a color" and grew a feature at a time. What follows is the ordered list of what to grow next, and why that order.

**The bar is a corpus, not one effect.** The predecessor MoonLight publishes a body of live scripts at
[ewowi/MoonLight/livescripts](https://github.com/ewowi/MoonLight/tree/main/livescripts), and the goal is that MoonLive can **run them**, not that we migrate them into this repo. They are written against a language with floats, structs and a wide builtin surface, so they are an honest external measure of how far the subset still has to grow: each one that compiles unchanged is a feature below that landed, and each one that does not names the next gap.

The measure throughout is **what a real effect needs**. A simulation effect (particles, physics, anything where this frame depends on the last) is the honest test, because it exercises state, arithmetic and control flow together rather than one at a time. `moonlive/effects/particles/balls.mle` is the worked example: bouncing balls, written against the language as it stands, and every compromise it had to make is a line item below.

## The ceilings

The limits a script meets are the constants in [MoonLiveBuiltins.h](../../src/core/moonlive/MoonLiveBuiltins.h) and [MoonLiveIr.h](../../src/core/moonlive/MoonLiveIr.h), each with its reason beside it.

## What a simulation effect gives up

Each row is a compromise the balls effect makes, and the language feature that would remove it:

| forced to | because | wants |
|---|---|---|
| 4 objects, not 25 | 64-byte arena | a bigger arena, or a pool handle (shipped for particles) |
| one flat color | no `hsv()` builtin | `hsv()` |
| one array per field | no structs | structs |

## The library is already built; the engine is what is missing

The library recorded what it needs from the engine while it was built, deferred so the compiled side could finish first. That list is the real roadmap, and it is more specific than "add a language feature":

5. **Two entry shapes:** `frame()` for composing kernels (the scalable path) and `pixel(x,y,z)`
   for per-pixel ergonomics (honest ceiling around 32×32).
6. **Stateful handles** (a `Pool`, a `BeatPhase`): script-declared, arena-allocated at compile
   time, passed as an opaque first argument. No script-side memory management.

Item 5 is worth noting against the arena work below: a particle pool as a HANDLE means the script does not spend its own 64 bytes on particle state at all. That is a better answer than widening the arena, and it is already designed.

## The type system: the settled design (2026-08-23)

The types as shipped: [the six types](../moonmodules/light/moonlive.md#the-six-types) and [fractional arithmetic with `fixed`](../moonmodules/light/moonlive.md#fractional-arithmetic-with-fixed); what the design did not anticipate: [the five-types plan](https://github.com/MoonModules/MoonLight/issues/173).

**The two edges of `fixed`, stated so they are conventions rather than traps:**
- **Time never goes into `fixed` raw**: 32,768 seconds is 9.1 hours. `t` stays `int` milliseconds
  and rhythm reaches a script through `beat()`/`beatsin()`, which is already the idiom.
- **Deep fractal zoom bottoms out at ~16 bits of magnification**, where the step becomes a pixel.
  The same class of limit Q13 has at 13 bits, and better than float32 past that point (its 23-bit mantissa is shared with the integer part). Extreme zoom is a 64-bit-builtin problem, not a scalar-type problem.

**`float` stays out, with a clean conscience.** The whole compiled library, the particle kernel
and escape() are integer/fixed already; math16.h supplies sin/atan/dist/sqrt; the hot path makes float promotion a fatal warning. The FPU story splits the boards (LX6/LX7/P4 have one, the RISC-V line does not), so JIT float either runs wildly differently per board or softfloats, against the one-language-four-ISAs-identical rule. Fixed-point is bit-identical everywhere, which is also what makes an effect reproducible. If the predecessor's corpus (written with floats) ever forces the issue, the answer is compile-time float-literal-to-fixed translation, not runtime float.

**Per-type notes:**
- `bool[]` ships byte-backed first (elements are 0/1 either way), and the 8-per-byte packing lands
  later as a pure memory optimization when an automaton effect pays for its read-modify-write codegen. Same rule as every constant here: against a script that needs it, not on speculation.
- Where arrays LIVE: small arrays stay in the arena (internal RAM); grid-sized state goes through
  ScratchBuffer handles (item 9b's model), which prefer PSRAM and fall back to internal heap on the classic ESP32, whose fixtures are small by construction. `byte[]` is what keeps a classic heat map at 1x rather than 4x.

## The order, and why


Ordered by **what removing it buys**, not by implementation cost.

### 3. A bigger arena and more members: *check the handle route first; storage rules now in the type-system design above*

64 bytes of member data is why an effect holds four objects rather than twenty-five.

**But check the handle route first.** The power-functions spec's item 5, a particle pool as an
arena-allocated HANDLE, means a simulation effect stops storing its own particle state entirely, which removes the pressure without touching these constants. Widen the arena for the scripts that genuinely hold their own state; do not widen it as a substitute for handles.

Not purely a constants bump, and the blockers are known:

- `kCtrlBytes` / `kMaxCtrls` are both `uint8_t`, so the arena caps at 255 bytes before any type
  change. 150 bytes of particle state fits under that; much more does not.
- `static_assert(kCtrlBytes <= 64, "seeded_ is a 64-bit mask, one bit per script arena byte")`
  (`MoonLive.h:270`) is the real gate. Past 64 bytes the seeded-member mask needs re-indexing, and there is a worked example, because it was widened 16 → 64 once already. The assert exists because the earlier `uint32_t` version silently aliased members mod 32.
- Watch `sizeof(MoonLive)`: why growing it grows every scripted module is on [MoonLiveParticles](../../src/light/moonlive/MoonLiveParticles.h).

### 4c. Insights from porting a per-pixel effect: *the cost is in the CALLS*

A second port (`moonlive/effects/waves/octopus.mle`, a polar spiral) measured differently from the first, and the difference is the useful part.

| effect | shape | S3, 64x64 |
|---|---|---|
| balls | four small discs, ~500 lit pixels | **1278 us** |
| octopus | every pixel, every frame | **21762 us** |

A third port (`metal.mle`, an SDF shader) put a number on the most expensive builtin, measured on shiffy's 80x48:

| variant | tick | vs plasma |
|---|---|---|
| plasma (9 calls/px, no sqrt) | **16031 us** | baseline |
| metal, 2 `polarR`, no `uv` | **32843 us** | 2.0x |
| metal, 2 blobs + `uv` | **46221 us** | 2.9x |
| metal, 3 blobs + `uv` | **59600 us** | 3.7x |

**`polarR` costs ~13400 us per call site per frame at 3840 pixels: about 3.5 us per pixel, one
builtin.** It wraps `dist16`, a real square root, and `draw.h` already measures a sqrt-based SDF at ~108 cycles/px against ~14 for the squared form. A squared-distance builtin (`polarRSq`, or letting a script compare against `r * r` as `ripples.mle` does) is the cheap fix, and it is the same trick the compiled effects already use.

Also measured and **disproved**: hoisting the four loop-invariant `beatsin` calls out of the inner loop into members moved 59600 to 58459 us. Call overhead per se is NOT the cost here: the square roots are. Worth recording because it contradicts the natural first guess.

That is ~5.3 us per pixel, and it is not the arithmetic; it is **~32,000 host calls per frame**.
Each `polarA`/`polarR`/`sin`/`scale`/`beat` is a real call through the builtin ABI, and a whole-canvas effect makes eight or so per pixel.

Two things follow, and they sharpen the priorities above:

1. **Per-pixel builtins want to be inline ops, not calls.** `setPaletteColor` is a Call and that is
   fine at ~500 pixels; at 4096 the call overhead dominates. The `Inline` kind already exists (`setRGB` uses it); the polar pair and the palette write are the candidates.
2. **A `frame()` entry shape would sidestep it entirely.** The power-functions spec's item 4,
   compose kernels over a whole frame rather than script per pixel, is exactly the answer for this class of effect, and octopus is the concrete case that argues for it.

Worth stating plainly: 35 fps for a full-canvas 64x64 effect is usable, and the port needed no new language features beyond two builtins. The ceiling here is call overhead, not expressiveness.

### 4b. Predefined structs: what remains after `Coord3D` and `RGB`

**No `HSV` struct.** A hue wheel is how an effect picks color while ignoring the user's palette, so `HSV` stays in `setPalEntryHSV`, which authors a palette rather than bypassing one.

What remains: `setColorFromPalette(pos, index, brightness)` taking a `Coord3D`, and arrays of structs (§ 10).

### 6. Function arguments and return values: *moderate, removes a real footgun*

The forward call is the cheapest to build and the most confusing to meet, since the message is the one an actual typo produces: a second pass over the class body resolves it, which the code comment names as the reason it is refused rather than half-supported.

### 10. User structs, and arrays of them: *readability, once the arena is bigger*

`ball[i].x` instead of parallel arrays. Genuinely nicer and closer to how a precompiled effect reads, but parallel arrays work the moment the arena is big enough. Last because #1 removes most of the pain, not because it does not matter.

Arrays of the five scalar types already ship (`byte heat[16]` in `ember.mle`), so what is missing here is arrays OF STRUCTS, and it splits in two. An array of a PREDEFINED struct (§ 4b) is the smaller half and the one an effect reaches for first: a script holding `Coord3D pos[8]` is exactly the parallel-array flattening this item exists to remove, and the element width the array path already carries (`idxPack`) is the machinery it needs. An array of a USER struct needs the general declaration machinery above it. Worth building in that order if this is picked up.

### 11. What the StadBeest legs and eyes hit (2026-10-07)

Two effects written in one session on real hardware: `stadbeest-walk.mle` (a walk cycle on 10 tubes, dancing to music) and `stadbeest-frog-eye.mle` (a frog's eye on a 241-light ring disc). Besides the member and label limits above, each of these cost a workaround:

| limit | what it cost | wants |
|---|---|---|
| "codegen failed: assembler overflow (branch range, slot, or immediate)" | names neither the limit nor the line; finding it took a desktop bisect with `disasm.py` | the message names the table that ran out and the function it was filling |
| "too many arguments to hold" | fires when the eye's `tick()` held too many live locals, so the wording sends the author to the calls | a message about live variables when that is the cause |
| a failed recompile leaves the effect dark | on a live installation every edit that does not compile switches the light off until a fixed one arrives; it happened three times in the session | keep running the last program that compiled, and show the error beside it |
| `setPaletteColor` clamps the palette index | a palette is circular, but `hue + offset` past 255 sticks at the last color unless the script writes `mod(..., 256)` | wrap the index, as the palette itself does |
| an effect visits every grid pixel | the eye computes 1600 pixels to light 241, and the legs' grid is mostly empty between 10 tubes | a way to iterate only the layout's lights, for sparse layouts |
| a `bool` control takes a min and a max | `addControl("pulse", pulse, 0, 1)` | `addControl(name, flag)` for a bool |

### 12. A one-to-one port of hpwit's scripts, for a like-for-like comparison (2026-10-19)

hpwit's compiler (`hpwit/new-parser`, `asmparser2`) runs whole C-like programs. His `multi_effect_controller.sc` (800 lines) and the five `ProceduralEffects` were ported to MoonLive on 2026-10-18 (`moonlive/projects/hpwit/`), but as the same LOOK rather than the same program, so code size and frame time do not compare like for like. A one-to-one port needs the language to accept his programs as written. Each construct was compiled on the desktop on 2026-10-19:

| his scripts use | MoonLive | what it takes | work |
|---|---|---|---|
| ~10 KB of state: struct arrays of 160 to 220 objects, a 40×80 trail, 12 KB heat and angle tables | 64 bytes of members, 16 members | an arena sized from the declarations, on the heap or PSRAM, past the `uint8_t` and 64-bit seeded-mask limits; item 3 has the blockers | L, 3 to 5 days |
| ~30 functions: struct methods and free functions | 8 per class (`kMaxEntryPoints`) | a function table sized per script; free functions outside the class | M, 1 to 2 days |
| `struct` with fields, methods and a constructor, arrays of them | refused | item 10, user structs, then their methods and arrays | XL, 1 to 2 weeks |
| `float`, `uint8_t`, `uint32_t`, casts like `(int)(x)` | refused | `float` (every target but the RP2040 has an FPU) or float mapped onto `fixed`; the sized integer names as aliases; casts | S for the names and casts, 1 day; M to map `float` onto `fixed`, 2 days; XL for a real `float` on five backends, 1 to 2 weeks |
| `grid[y][x]` (his `[y, x]`) | refused | 2D arrays over the existing element-width path | M, 1 to 2 days |
| `while`, `break`, `continue`, `i++`, `+=`, `?:`, `switch` | all refused | the statements and operators, each a parser and IR addition | S each for `while`, `++` and `+=` (rewritten into what exists), 1 day together; M each for `break`/`continue`, `?:` and `switch`, 3 to 5 days together |
| `#define` constants | refused | a constant declaration (`const int`) rather than a preprocessor | S, half a day |
| `main()` with `while (true)`, `show()`, `delay()` | `tick()` per frame | a shim: the loop body becomes `tick()`, `show()` and `delay()` become no-ops, `millis()` reads `t` | M, 1 to 2 days |
| `hsv()`, `rand(mod)`, `atan2`, `hypot`, `sin` on radians, `printfln`, `pinInterrupt` | palette, `random16`, `polarA`/`polarR`, `sin` on 0..65535, `print`, none | host bindings with his signatures, under his names, in a compatibility table | M, 2 days, once `float` is decided |
| code past 16 KB | `kCodeCap` 16 KB | the cap per platform (§ The code cap, per platform) | S to M, 1 day |

Work is in days of agent implementation with the product owner's review, for the parser, the IR and all five backends, each with its tests; S is under a day, M one to two, L a week, XL two. In all about five weeks with `float` mapped onto `fixed`, about seven with a real `float`, the structs and the arena being most of it.

Most rows are general language growth the MoonLive scripts want anyway; the `main()` shim and his host bindings exist only for the comparison, and stay behind a compatibility table rather than entering the standard vocabulary. **Decide first** whether the comparison is worth the compatibility layer, or whether comparing on the ported looks, now in `moonlive/projects/hpwit/`, is enough.

## How to know a step landed

Two measures, one local and one external.

**Local:** re-port a simulation effect after each step and delete the row it removes from the
compromise table above. A port with no rows left (dozens of shaded, colored objects with fractional velocities on a full-size grid, at a frame rate the bench can measure) means the language carries a real simulation.

**External, and the harder bar:** take the
[predecessor's livescripts corpus](https://github.com/ewowi/MoonLight/tree/main/livescripts) and count how many compile and run unchanged. That number is the honest progress metric, because those scripts were written without regard for our subset. Track it per step; a feature that moves it is worth more than one that does not, whatever the order above guesses.
