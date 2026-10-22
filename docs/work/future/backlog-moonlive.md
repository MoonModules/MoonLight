# Backlog: MoonLive

> **Forward-looking backlog document, an exception to the CLAUDE.md present-tense rule.** Third in a
> family: [livescripts-analysis-top-down.md](livescripts-analysis-top-down.md) designs the ENGINE
> (grammar, IR, codegen), [power-functions-analysis-top-down.md](power-functions-analysis-top-down.md)
> designs the BUILTIN SURFACE the engine calls into. This one is the ordered plan for closing the
> gap between them: which language features to add next, and why that order. Every limit quoted
> below was measured against the shipped compiler, not read off the source.

## What this is for

MoonLive compiles a C-subset to native code on the device. The subset is deliberately small: it
started at "fill a buffer with a color" and grew a feature at a time. This document is the ordered
list of what to grow next, and why that order.

**The bar is a corpus, not one effect.** The predecessor MoonLight publishes a body of live scripts at
[ewowi/MoonLight/livescripts](https://github.com/ewowi/MoonLight/tree/main/livescripts),
and the goal is that MoonLive can **run them**, not that we migrate them into this repo. They are
written against a language with floats, structs and a wide builtin surface, so they are an honest
external measure of how far the subset still has to grow: each one that compiles unchanged is a
feature below that landed, and each one that does not names the next gap.

The measure throughout is **what a real effect needs**. A simulation effect (particles, physics,
anything where this frame depends on the last) is the honest test, because it exercises state,
arithmetic and control flow together rather than one at a time. `moonlive/effects/particles/balls.mle` is the
worked example: bouncing balls, written against the language as it stands today, and every
compromise it had to make is a line item below.

## The ceilings, measured

Five hard limits, all found by hitting them:

| limit | value | where |
|---|---|---|
| script state | **64 bytes** shared by all members | `kCtrlBytes`, `MoonLiveBuiltins.h` |
| distinct members | **16** ✅ | `kMaxCtrls`, same file |
| branch labels | **96** in the IR, **255** labels and **255** fixups in the assembler, counted across the whole script (an `if` or `for` takes up to 2, and each `||`, `!` and `!=` one more) | `kIrLabels`, `kAsmLabels`, `kAsmFixups`, `MoonLiveIr.h` |
| frame slots | **32**, shared by live variables, loop counters and staged call arguments ✅ | `kMaxLocals`, `MoonLiveIr.h` |
| native code | **16 KB** per script, the same on every chip; the RISC-V backend emits the most, and the largest script is at 13.7 KB there | `kCodeCap`, `MoonLiveBuiltins.h`; see § The code cap, per platform |
| ~~numeric types~~ | ~~`uint8_t`, `uint16_t`, `int16_t`~~ → **`int`, `byte`, `bool`, `fixed`, `string`** ✅ | still no float: `fixed` is Q16.16 |
| ~~builtin table~~ | ~~16, and 16 used~~ → **96** ✅ | `BuiltinTable::kMax`, raised, with an overflow assert |

The branch budget was binary-searched with generated scripts: **6 `if`/`else` + 2 `for` compiles,
7 does not.** The state budget is what caps the balls effect at 4 balls rather than 25: six fields
per ball needs ~150 bytes and six members.

The frame-slot budget was hit writing `aurora.mle` (2026-09-04), the first two-layer shader: it
compiles only after folding intermediates back into the expressions that use them, which is the
readability the language exists to give. Factoring the loop body into a helper would relieve it,
once helpers can take arguments and return values (§ 6). See § 8b.

### The code cap, per platform (backlog, 2026-10-16)

Measured across all 53 shipped scripts with `moondeck/moonlive/disasm.py` (asked on Discord by hpwit, author of ESPLiveScript): compiled bytes per source byte run Thumb-2 1.4x, Xtensa 1.7x, arm64 2.2x, RISC-V 2.8x. The largest script, `stadbeest-sauron.mle` (135 lines, 5.7 KB of source), compiles to 7.8 KB on Xtensa and 13.7 KB on RISC-V, so a script about 20% bigger is refused on a C3 or a P4.

A script's code meets three ceilings and the lowest wins: `kCodeCap`, the staging the compiler reserves per token (`codeCapFor`, capped at `kCodeCap`), and the executable memory `allocExec` finds. Only the last is the hardware: IRAM on a classic ESP32 holds tens of KB, while a P4's 768 KB of internal memory is executable and a desktop's is unbounded. The fix is the code cap as a per-platform value from the platform layer: 16 KB on a classic ESP32 and a C3, 32 KB on an S3, 64 KB on a P4 and the desktop. Pinned by a test that a script past 16 KB compiles on a platform whose cap allows it and is refused with a clear message where it does not. The members, frame slots, entry points and labels are separate budgets, costing memory per script on every chip, so they stay until a script needs more.

## What a simulation effect gives up today

Each row is a compromise the balls effect makes, and the language feature that would remove it:

| forced to | because | wants |
|---|---|---|
| 4 objects, not 25 | 64-byte arena | a bigger arena, or a pool handle (shipped for particles) |
| whole-pixel motion | no fractional type | fixed-point or float |
| one flat color | no `hsv()` builtin | `hsv()` |
| one array per field | no structs | structs |
| the helper reads a member for its index | functions take no arguments | arguments |
| guards folded into `mod()` | 16 branch labels | a bigger label budget |

## The library is already built; the engine is what is missing

This is the part worth stating plainly: **the power-functions library exists and is largely
shipped** (particle kernels, the typed math surface, twelve showcase effects). It was built to make
MoonLive rich, and today almost none of it is reachable from a script.

Its top-down spec already records what it needs from the engine, in
[§ 4 MoonLive requirements](power-functions-analysis-top-down.md), deferred at the time so the
library could finish compiled-side first. That list is the real roadmap, and it is more specific
than "add a language feature":

1. **A builtin table of ≥ 64 entries.** ✅ *shipped*. `BuiltinTable::kMax` was **16, and the light
   domain registered exactly 16**: the table was FULL, `add()` returned false past the limit, and
   nothing checked the result, so the seventeenth builtin was dropped silently and the script
   failed later with "unknown function". `kMax` is now 96, and an overflow is loud rather than
   silent (`MM_ASSERT_NO_BUILTIN_OVERFLOW`).
2. **A larger branch budget (`kIrLabels`, today 16).** Every `if` costs up to two labels and every
   `for` costs two, counted across the WHOLE script including its helper functions, and labels are
   never reused once a scope closes. A script hits "too many branches in one script" well before it
   feels complex. **Concrete case (2026-08-20):** giving `balls.mle` the wall bounces its own name
   promises (four `if`s, one per wall) exceeded the budget and would not compile, even reduced to
   one branch per wall by writing the direction bit arithmetically instead of toggling it. The
   effect is 4 balls on a 64-byte arena. It ships bouncing only because the motion was rewritten as
   a `beatsin()` per axis, where the reversal is inherent in the sine and costs no branch at all.
   That worked out better here, but it was a workaround found under the ceiling rather than the
   obvious way to write the effect, and the next script will not always have one.
   **The cost is stack**, already measured and documented at `kIrAsmLabels`: ~4 bytes per label in
   `lowerWith` (a local in the assembler), plus `Loop loops[kIrLabels]` and `int32_t
   labelAt[kIrLabels]` in the spill pass. Raising 16 → 32 is a one-constant change; the number to
   watch is `lowerWith`'s frame on the classic ESP32, which the existing note records going 480 →
   1120 bytes when the assembler tables were last raised. Reusing a label once its scope closes
   would lift the ceiling without the stack, but that is a real allocator change rather than a
   constant.
3. **Typed multi-argument host calls** (≤ 6 args, optional return). Today a builtin takes one
   `uint32_t` and returns one, which is why `line()` had to be given a bespoke seven-argument
   staging path and why most of the library is inexpressible.
4. ✅ **Script symbols**: SHIPPED as `t`, `width`, `height`, `depth`, `xPos`, `yPos`, `zPos`.
5. **Two entry shapes:** `frame()` for composing kernels (the scalable path) and `pixel(x,y,z)`
   for per-pixel ergonomics (honest ceiling around 32×32).
6. **Stateful handles** (a `Pool`, a `BeatPhase`): script-declared, arena-allocated at compile
   time, passed as an opaque first argument. No script-side memory management.

Item 5 is worth noting against the arena work below: a particle pool as a HANDLE means the script
does not spend its own 64 bytes on particle state at all. That is a better answer than widening
the arena, and it is already designed.

## The type system: ✅ *shipped*

Shipped as designed, with three things worth carrying forward that the design did not anticipate:

- **`fixed` needed three new assembler primitives**, not the one the design implied. A Q16.16
  multiply is `mulhi` + `mul` + two shifts, and the JIT had no shift instruction and no
  multiply-high at all: `mulReg` is a plain 32-bit multiply. `shlImm`, `sarImm`, `shrImm` and
  `mulhi` went into all four backends, every encoding checked byte-for-byte against the real
  assembler. Worth it: the alternative made every fixed multiply a host call inside a per-pixel
  loop, and made `int`↔`fixed` conversion (one shift) a function call.
- **An integer literal ADOPTS fixed at a meet point.** The design said any mix is an error; that
  made `v * 2` and `if (v < 0)` unwritable. A bare literal now converts at compile time by
  patching its own `Const` (free at run time), while a *variable* still names its conversion,
  because a literal's meaning is visible at the site and a variable's scaling is not.
- **`movImm` truncated above 16 bits on arm64 and Xtensa.** A latent bug the whole time, masked
  `& 0xffff` under a comment warning about exactly that failure mode: invisible while literals
  capped at 65535, and fatal the moment a Q16.16 literal (2.0 is 131072) rode a `Const`. Both
  backends now materialize the full 32 bits.

`bool` truncates rather than normalizing (`flag = 256` reads false), because normalizing needs a
compare-and-select the IR has no op for: there is no `Sub` and no bitwise op. Waiting for a script
that writes a non-boolean expression into a bool.

The design as agreed follows, unchanged.

## The type system: the settled design (2026-08-23)

Decided with the product owner after the signed-values work, whose four bugs were all
storage-width plumbing at the scalar boundary (a wrapped uint16 member, a sentinel read through a
16-bit window, a one-byte store into a two-byte member, a sign-blind array load). The design
removes that boundary rather than patching it again. This section supersedes the per-width
thinking in items 3, 5 and 10 below; those entries stay for their history and their measurements.

**Five types, each usable as scalar or array; one storage rule each way:**

> `int`, `byte`, `bool`, `fixed`, `string`. Every SCALAR occupies one uniform 4-byte slot,
> whatever its type. ARRAYS pack by element type. Strings are literals.

A type is a SEMANTIC, not a storage width, for scalars: a `byte` scalar is an int the compiler
masks to 8 bits on assignment (one AND), a `bool` normalizes to 0/1 (one compare), `fixed` is an
int with scaled literals and a shifted multiply, `int` is bare. That single move is what buys
orthogonality without resurrecting the width machinery: one scalar load, one scalar store, no
sign windows, no width-matched bindings, no alignment rules. The width question survives only in
arrays, where it pays for itself in memory, and the array access path already carries an element
width (`idxPack`), so packed arrays are existing machinery rather than new.

| Type | Min | Max | Step | The range is enough because |
|---|---|---|---|---|
| `int` | -2,147,483,648 | 2,147,483,647 | 1 | The domain's biggest integers: the ms clock (2^31 ms = 24.8 days, the known wrap), 12,288 lights, squared on-grid uv distances (<= 2^30). Anything wider lives in 64-bit inside a builtin, as escape() already does. |
| `byte` | 0 | 255 | 1 | Exactly a hardware channel, a palette index, a heat cell. The LED's range, not a chosen one. |
| `bool` | 0 | 1 | - | Normalized on store. |
| `fixed` (Q16.16) | -32,768.0 | +32,767.99998 | 1/65,536 | Coordinates top out near 256 px and uv at +-4.0, orders of magnitude inside the range. The step gives 16 fraction bits against the 8 a channel displays, so two chained factors still land at the display floor. One turn as 1.0 steps at exactly angle16's resolution, the precision sin16 already consumes. Q16.16 is libfixmath's fix16_t, the de-facto embedded standard. |
| `string` | - | one of the script's literals | - | An index into the compile-time pool. |

**Cost on the hot path: none where it matters.** Scalar loads/stores are plain 32-bit forms
(on Xtensa `l32i.n`, 2 bytes against `l8ui`'s 3, so smaller than today). A `byte[]` element's
zero-extend and truncation happen INSIDE the load and store instructions; the only added
instructions are one mask on a `byte` scalar store and one normalize on a `bool` store. A `fixed`
multiply costs the mul-high + shift pair, which is the inherent price of fixed-point math the
builtins already pay inside their Q13/Q8.8 conventions; it just becomes visible and uniform.

**`int` <-> `fixed` conversion is EXPLICIT, never implicit.** The conversion itself is one shift
(`<<16` / `>>16`), so the cost is trivial; the rule exists so a script never silently mixes a
pixel count into a Q16.16 expression and gets a number 65,536 times off. A mixed expression is a
compile error naming the conversion to write.

**The two edges of `fixed`, stated so they are conventions rather than traps:**
- **Time never goes into `fixed` raw**: 32,768 seconds is 9.1 hours. `t` stays `int` milliseconds
  and rhythm reaches a script through `beat()`/`beatsin()`, which is already the idiom.
- **Deep fractal zoom bottoms out at ~16 bits of magnification**, where the step becomes a pixel.
  The same class of limit Q13 has at 13 bits, and better than float32 past that point (its 23-bit
  mantissa is shared with the integer part). Extreme zoom is a 64-bit-builtin problem, not a
  scalar-type problem.

**`float` stays out, with a clean conscience.** The whole compiled library, the particle kernel
and escape() are integer/fixed already; math16.h supplies sin/atan/dist/sqrt; the hot path makes
float promotion a fatal warning. The FPU story splits the boards (LX6/LX7/P4 have one, the RISC-V
line does not), so JIT float either runs wildly differently per board or softfloats, against the
one-language-four-ISAs-identical rule. Fixed-point is bit-identical everywhere, which is also what
makes an effect reproducible. If the predecessor's corpus (written with floats) ever forces the issue,
the answer is compile-time float-literal-to-fixed translation, not runtime float.

**Per-type notes:**
- `bool[]` ships byte-backed first (elements are 0/1 either way), and the 8-per-byte packing lands
  later as a pure memory optimization when an automaton effect pays for its read-modify-write
  codegen. Same rule as every constant here: against a script that needs it, not on speculation.
- `string` is a literal reference: assignable from literals, comparable, passable to a builtin,
  one int of storage. NO concatenation, ever: runtime string building means allocation inside the
  render loop.
- Where arrays LIVE: small arrays stay in the arena (internal RAM); grid-sized state goes through
  ScratchBuffer handles (item 9b's model), which prefer PSRAM and fall back to internal heap on
  the classic ESP32, whose fixtures are small by construction. `byte[]` is what keeps a classic
  heat map at 1x rather than 4x.

**Migration**: 26 shipped scripts spell C widths (`uint8_t`, `uint16_t`, `int16_t`). MoonLive is
not launched, so this is a clean mechanical break now and a compatibility program forever after.
That timing is a large part of why the decision was taken when it was.

## The order, and why


Ordered by **what removing it buys**, not by implementation cost.

### 1. A bigger builtin table: ✅ *shipped*

`BuiltinTable::kMax` was 16 and the light domain registered exactly 16: **the table was full**,
and it failed SILENTLY: `add()` returned false, no caller checked it, and the next builtin would
have surfaced as "unknown function" in a script with nothing pointing at the cause.

Now 96 (what the power-functions spec asks for), with `MM_ASSERT_NO_BUILTIN_OVERFLOW` so a dropped
registration is loud at startup rather than silent. This gated every other builtin; the palette
work below went in immediately behind it.

### 2. Typed multi-argument host calls: ✅ SHIPPED

SHIPPED: a builtin takes `const uintptr_t* args` with arities up to seven, plus typed flags
(`fixedArgs`, `fixedReturn`, `byRef`, `byStr`), and `emit` uses all seven. The paragraph below is the
problem as it stood before that.

A builtin took one `uint32_t` and returned one. `line()` needed a bespoke seven-argument staging
path to exist at all, and most of the power-functions surface cannot be expressed without this.
The spec asks for ≤ 6 arguments plus an optional return.

Doing #1 and #2 together is what actually opens the library; either alone leaves it stranded.

### 3. A bigger arena and more members: *check the handle route first; storage rules now in the type-system design above*

64 bytes of member data is why an effect holds four objects rather than twenty-five.

**More members: ✅ shipped (2026-10-07).** `kMaxCtrls` is 16, measured at +280 bytes per scripted module on the S3 (class size 1312 → 1592 B). The StadBeest legs need 11 members, 6 controls and 5 values kept between frames, and before the raise packed four of those values into one `int` by bit ranges. The arena stays at 64 bytes, and the rest of this section is about the arena.

**The two limits bind at very different points, and it is the COUNT that bites first.**
`fractal.mle` wanted 4 controls plus 4 scratch members plus a loop counter: 9 members costing
**12 of the 64 arena bytes**. It compiled once the counter was dropped (a `for` counter does not
have to be a member), so the script lost nothing, but the ceiling it hit was `kMaxCtrls` with 81%
of the arena still free. Any script with a handful of controls and a handful of intermediates
meets the same wall. If only one of the two moves, the count is the one worth moving: the four
tables it sizes were `DeclaredControl[8]` at 24 B each; the raise to 16 measured +280 B per
scripted module on the S3.

**But check the handle route first.** The power-functions spec's item 5, a particle pool as an
arena-allocated HANDLE, means a simulation effect stops storing its own particle state entirely,
which removes the pressure without touching these constants. Widen the arena for the scripts that
genuinely hold their own state; do not widen it as a substitute for handles.

Not purely a constants bump, and the blockers are known:

- `kCtrlBytes` / `kMaxCtrls` are both `uint8_t`, so the arena caps at 255 bytes before any type
  change. 150 bytes of particle state fits under that; much more does not.
- `static_assert(kCtrlBytes <= 64, "seeded_ is a 64-bit mask, one bit per script arena byte")`
  (`MoonLive.h:355`) is the real gate. Past 64 bytes the seeded-member mask needs re-indexing,
  and there is a worked example, because it was widened 16 → 64 once already. The assert exists
  because the earlier `uint32_t` version silently aliased members mod 32.
- Watch `sizeof(MoonLive)`. It is held BY VALUE in every scripted module. `registerType`'s probe
  builds one on the heap, since a probe on the boot stack is what boot-looped the P4 at 1440 bytes.
  Growing the arena grows every scripted module.

### 4. `setPaletteColor()`: ✅ *shipped*

A script now writes one call where it used to write three:

```c
setPaletteColor(x, y, index, brightness);
```

Three per-channel palette calls shipped first and worked, but the shape was wrong: three host calls per
pixel, and, because the compiler evaluates each argument independently, three evaluations of
whatever expression produced the brightness. A per-pixel radial falloff made that visible.

**Measured on an S3, 64x64 grid: 2331 us → 1838 us, 21% off the effect's own cost**, and the call
site went from three copies of a six-term expression to one. It also takes x/y rather than a flat
index, so the buffer layout (`mod(x, width) + mod(y, height) * width`) stops being open-coded in
every script.

No engine change was needed: `line()` already proved a Call builtin can write pixels through the
per-run draw canvas, so this rode the existing seam. `paletteRGB(i, bri)` answers the color as an `RGB` for a script that wants the value rather than a pixel (#4b).

### 4c. Insights from porting a per-pixel effect: *the cost is CALLS, not math*

A second port (`moonlive/effects/waves/octopus.mle`, a polar spiral) measured very differently from the
first, and the difference is the useful part.

| effect | shape | S3, 64x64 |
|---|---|---|
| balls | four small discs, ~500 lit pixels | **1278 us** |
| octopus | every pixel, every frame | **21762 us** |

A third port (`metal.mle`, an SDF shader) put a number on the most expensive builtin, measured on
shiffy's 80x48:

| variant | tick | vs plasma |
|---|---|---|
| plasma (9 calls/px, no sqrt) | **16031 us** | baseline |
| metal, 2 `polarR`, no `uv` | **32843 us** | 2.0x |
| metal, 2 blobs + `uv` | **46221 us** | 2.9x |
| metal, 3 blobs + `uv` | **59600 us** | 3.7x |

**`polarR` costs ~13400 us per call site per frame at 3840 pixels: about 3.5 us per pixel, one
builtin.** It wraps `dist16`, a real square root, and `draw.h` already measures a sqrt-based SDF at
~108 cycles/px against ~14 for the squared form. A squared-distance builtin (`polarRSq`, or letting
a script compare against `r * r` as `ripples.mle` does) is the cheap fix, and it is the same trick
the compiled effects already use.

Also measured and **disproved**: hoisting the four loop-invariant `beatsin` calls out of the inner
loop into members moved 59600 to 58459 us. Call overhead per se is NOT the cost here: the square
roots are. Worth recording because it contradicts the natural first guess.

That is ~5.3 us per pixel, and it is not the arithmetic; it is **~32,000 host calls per frame**.
Each `polarA`/`polarR`/`sin`/`scale`/`beat` is a real call through the builtin ABI, and a
whole-canvas effect makes eight or so per pixel.

Three things follow, and they sharpen the priorities above:

1. **No common-subexpression elimination.** `polarR(x - cx, y - cy)` appears twice in one
   expression and is CALLED twice. The compiler has no way to name an intermediate, so a script
   cannot hoist it either, the same limitation that made a palette pixel cost three brightness
   evaluations before #4. A single-assignment local (part of #4b/#6's territory) would remove a
   third of this effect's calls on its own.
2. **Per-pixel builtins want to be inline ops, not calls.** `setPaletteColor` is a Call and that is
   fine at ~500 pixels; at 4096 the call overhead dominates. The `Inline` kind already exists
   (`setRGB` uses it); the polar pair and the palette write are the candidates.
3. **A `frame()` entry shape would sidestep it entirely.** The power-functions spec's item 4,
   compose kernels over a whole frame rather than script per pixel, is exactly the answer for
   this class of effect, and octopus is the concrete case that argues for it.

Worth stating plainly: 35 fps for a full-canvas 64x64 effect is usable, and the port needed no
new language features beyond two builtins. The ceiling here is call overhead, not expressiveness.

**A naming lesson, learned by breaking two scripts:** the polar builtins were first called
`angle`/`radius`, which silently broke `ring.mll` and `rose.mll`: both declare a `radius` member,
and a builtin shadows a member name. Builtins share one namespace with every script's own
variables, so a new builtin should take a name a script would not: `polarA`/`polarR` rather than
the obvious ones. The compile-every-script test caught it immediately, which is the argument for
keeping that test cheap to run.

### 4b. Predefined structs: `Coord3D` and `RGB`, together: ✅ *shipped*

A struct is a named fixed-size array the compiler expands into scalar operations: a member, a local, a parameter and a return value, with field access, whole-struct assignment and a constructor. A struct argument fills one position per field, so every three-channel builtin takes an `RGB`. `getRGB` and `paletteRGB` answer one. The language reference holds the rest: [RGB and Coord3D](../../moonmodules/light/moonlive.md#rgb-and-coord3d).

**No `HSV` struct.** A hue wheel is how an effect picks color while ignoring the user's palette, so `HSV` stays in `setPalEntryHSV`, which authors a palette rather than bypassing one.

What remains: `setColorFromPalette(pos, index, brightness)` taking a `Coord3D`, and arrays of structs (§ 10).

### 5. Fractional arithmetic: *superseded by `fixed` in the type-system design above*

Every remaining visual compromise traces back to this: smooth motion, real velocities, a bounce
that conserves speed, and any falloff term that makes a shape look round rather than flat. Without
it each is a separate workaround.

Two routes, and the choice matters more than the schedule:

- **Fixed-point** (a `q8_8` over the existing `uint16_t`): no new backend work: it lowers to the
  integer ops all three ISAs already have. Enough for positions, velocities and most ramps.
- **True `float`**: closer to a precompiled effect's source, which is the near-verbatim porting
  goal the top-down analysis sets. Costs an FPU path per backend; the P4 and S3 have one, the
  classic ESP32 does not, so it would soft-float or refuse.

Fixed-point first is the pragmatic order: most of the quality at a fraction of the cost, and it
does not foreclose float.

**Settled** by the type-system design above: `fixed` is Q16.16 on a uniform int slot, float
stays out, and the range analysis lives there.

### 6. Function arguments and return values: *moderate, removes a real footgun*

`draw(i)` instead of setting a member the helper reads. The current shape is not just verbose:
caller and callee agree by convention and nothing checks it, so a helper called from two places
with different state silently does the wrong thing. It is also what makes helpers composable.

Script functions DO exist and ship: `balls.mle` calls `drawBall()`, `crosshair.mle` calls three
helpers, and every layout and modifier is one. Recursion works. Three limits sit on top of them, and
`aurora.mle` hit all three at once writing a `bright(v)` helper to apply one contrast window to two
layers (2026-09-04). **The return value shipped the same day, and eight parameter words with structs on 2026-10-12**, so one remains:

| limit | what the compiler says |
|---|---|
| no forward calls: a helper must be declared ABOVE its caller | `unknown function`, with the column but not the name |

A function may now RETURN a value: `int f() { return ...; }` and the call is an expression. Each
backend's `callLabel` preserves the whole vreg pool and delivers the result into the destination
register, so a value survives the calls that follow it in the same expression.

The forward call is the cheapest to build and the most confusing to meet, since the message is the
one an actual typo produces: a second pass over the class body resolves it, which the code comment
names as the reason it is refused rather than half-supported.

Parameters and returns also relieve § 8b: a helper's variables are live only inside it, so factoring
a loop body into a function is how a script stays under the frame budget.

### 7. Signed values: ✅ *shipped*

`int16_t` members, signed comparison, signed `/` and `%`, and `uvX`/`uvY` returning a signed
coordinate with no bias to subtract.

**The framing this item had was wrong about the cause, which is worth recording.** It described the
problem as `a - b` wrapping, and prescribed signed comparison. Writing a Mandelbrot effect produced
four bugs in one session and **not one of them was a comparison bug**: no `<` or `>` ever produced a
wrong picture. Three were the *biased-unsigned* convention, where a builtin returned a value centered
on 32768 and the author had to subtract that bias, which is exactly the subtraction unsigned
arithmetic breaks. The fourth was a byte argument truncating instead of saturating. Every one
presented as "the effect renders nothing", never as an error.

So what shipped is smaller than "make the language signed" and removes more than it adds:

- `signedArg`'s undocumented **16-bit window** is gone. It was the inverse of `uint16_t` member
  truncation, written down in neither place, and it is what made `d = 60000` read as -5536.
- The **bias on `uvX`/`uvY`** is gone: a coordinate has an origin, so the center is 0.
- `sin`/`cos` **keep** their bias, deliberately. A wave has no origin, and `scale(sin(a), n)`
  sweeping a full axis is the idiom 14 shipped call sites use. A script wanting a signed wave
  writes `sin(a) - 32768`, which works now.
- **Comparison** is a separate `BranchGeS` op, not a change to `BranchGe`: the array-index clamp
  and the loop guards need unsigned, and a negative index arriving as a huge value is what lets one
  branch catch both ends of a range.
- **`int8_t` is deliberately absent.** Xtensa has no signed byte load, so it would need a
  sign-extend sequence the other three ISAs do not, for a width no script has asked for.

`escape()` stays a builtin regardless: its Q13 squaring needs 64-bit intermediates, which a 32-bit
script value cannot express however signed it is.

### 8. More branch labels: ✅ *raised to 96 IR labels and 255 assembler labels and fixups, the tables on the compile-time heap*

16 was tight enough that a straightforward nested draw did not fit. At 40 IR labels and 48 assembler labels with 96 fixups two scripts still hit them on 2026-10-07: the StadBeest legs with a bass envelope, and the StadBeest eye with darts, blinks and a ripple, each at roughly 20 to 25 `if`s across all its functions. Both fit only after branches were rewritten as arithmetic (§ 11). Raising `kIrLabels` costs
compile-time table space and nothing at run time. Measure what a realistic effect needs before
picking a number: the balls port wanted ~12 and had to be folded down.

### 8b. More frame slots: *the encoding is NOT the limit; measure the stack*

**32 live variables**, shared by a script's named variables, its loop counters, and the arguments it
stages for a call. The budget is what is live AT ONCE rather than a total: a call hands its staging
slots back, and an `if`, `else` or `for` block hands its locals back at the closing brace
([MoonLive](../../moonmodules/light/moonlive.md) documents both). A script that exceeds
it fails with "too many variables in this function", "too many arguments to hold" or "too many loop
variables".

Sixteen is enough for the shipped corpus and was not enough for the first two-layer shader:
`aurora.mle` wants an oscillator per layer, the grid centre, a polar angle and radius, and a field
sample per layer, all live in the same scope inside two nested loops, with the loop counters and
each call's staged arguments drawn from the same 16.

`kMaxLocals`'s own comment says raising it "means widening the frame on all three backends together,
the slot index is an instruction field". **Measured on the shipped assemblers, that is not so:**

| backend | how a slot is addressed | slots the encoding allows |
|---|---|---|
| Xtensa | `s32i/l32i`, an 8-bit offset counting 4-byte words | 256 |
| RISC-V | `sw/lw`, a 12-bit signed offset from the frame pointer | 2048 |
| host (arm64, x86-64) | an offset from a parked frame pointer | not encoding-bound |

So the real cost is **stack**, not encoding: each slot is 4 bytes of frame on the render task, and
`kAsmLabels`/`kAsmFixups` next door carry the warning that this project has already bootlooped a P4
on an oversized stack frame. 16 slots is 64 bytes; 32 would be 128. That is a cheap change on its
face, and the measurement settled it: **`kMaxLocals` is 32** (2026-09-04), 128 bytes of frame, and
the encoding was never the limit on any backend.

### 9. Division: ✅ *shipped*

`/` and `%` are operators, at multiplication's precedence. Both lower to a host call the way `mod`
already did, so the operator costs nothing the capability did not already cost: the divide itself
is the expense, and it is a host call wherever it appears: fine on a cold path, deliberate
per-pixel. The parser resolves both through the builtin table (`div`, `mod`) rather than knowing
either by name, so core stays domain-neutral and a domain that registers neither simply has no
operator. `mod(a, b)` stays registered: it is the name the cyclic case reads best under.

### 9b. A ScratchBuffer pool handle, ✅ *shipped for particles*

A script sizes its own particle pool with `pool(n)` from `defineControls()`, and the buffers live in
`MoonLiveParticles` (six `ScratchBuffer`s the binding owns) rather than in the 64-byte arena, which
would have held about five particles. Sizing is reachable ONLY from that one moment: the sizing sink
is installed around the `defineControls` run, so `pool()` from `tick()` is a no-op reporting the live
count and no allocation ever reaches the render path.

Nine builtins, all whole-pool passes: `pool`, `emit`, `gravity`, `drag`, `step`, `age`, `render`,
`bounce` and `collide`. The last two shipped after measurement: collide is an N-body check (3.2 us
at 48 particles against 0.1 us without, 53.6 us at 200), so the quadratic is real but the absolute
cost at ball-pit sizes is not, and the numbers ride the builtin so an author knows what a big pool
would cost.
The cost model is the point. `fountain.mle` measures **9 us** on a 128x96 desktop grid against
`metal.mle`'s **1557 us** on the same grid: the first script vocabulary whose cost scales with the
OBJECTS rather than with the grid.

**Structs were NOT needed, and that is a finding rather than a deferral.** Every pool operation is
whole-pool or takes plain scalars, so a script never names a particle field. #10 below is about
`ball[i].x` INSTEAD of parallel arrays, which a pool removes the need for; #4b is `Coord3D`/`RGB`
for per-pixel shader signatures, whose real prerequisite is #2.

Not exposed, each with a reason: `spray`
(`emit` with a wide cone is one), `spawn` (per-particle in a whole-pool API), `force`/`forceSmall`
(needs the `acc` buffer for wind nothing needs yet), `attract`, `wrap`, `liveCount`, `clear`.

### 10. User structs, and arrays of them: *readability, once the arena is bigger*

`ball[i].x` instead of parallel arrays. Genuinely nicer and closer to how a precompiled effect
reads, but parallel arrays work the moment the arena is big enough. Last because #1 removes most
of the pain, not because it does not matter.

Arrays of the five scalar types already ship (`byte heat[16]` in `ember.mle`), so what is missing
here is arrays OF STRUCTS, and it splits in two. An array of a PREDEFINED struct (§ 4b) is the
smaller half and the one an effect reaches for first: a script holding `Coord3D pos[8]` is exactly
the parallel-array flattening this item exists to remove, and the element width the array path
already carries (`idxPack`) is the machinery it needs. An array of a USER struct needs the general
declaration machinery above it. Worth building in that order if this is picked up.

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

| his scripts use | MoonLive today | what it takes | work |
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
compromise table above. A port with no rows left (dozens of shaded, colored objects with
fractional velocities on a full-size grid, at a frame rate the bench can measure) means the
language carries a real simulation.

**External, and the harder bar:** take the
[predecessor's livescripts corpus](https://github.com/ewowi/MoonLight/tree/main/livescripts) and
count how many compile and run unchanged. That number is the honest progress metric, because those
scripts were written without regard for our subset. Track it per step; a feature that moves it is
worth more than one that does not, whatever this document guesses.
