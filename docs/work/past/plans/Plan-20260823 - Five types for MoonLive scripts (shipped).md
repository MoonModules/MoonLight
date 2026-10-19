# Plan, five types for MoonLive scripts

## Context

MoonLive scripts spelled C storage widths: `uint8_t`, `uint16_t`, `int16_t`. That convention produced
four shipped bugs of one family, each presenting as "the effect renders nothing" or "the effect
renders wrong" and never as an error:

- `sin(a) - 32768` wrapped the sine's negative half to ~4.29 billion.
- `(uvX(...) - 32768) * zoom` did the same on the left half of a grid, tearing the plane into blocks.
- A `d = 60000` sentinel read back as -5536 through a 16-bit window, so every light stayed black.
- A one-byte store into a two-byte member collapsed a whole shader to one flat colour.

Not one was a mistake in a script. Each was a script author choosing a storage width and the engine
silently disagreeing. The product owner and the agent settled the replacement in
[backlog-moonlive.md](../../future/backlog-moonlive.md): **five types, `int`,
`byte`, `bool`, `fixed`, `string`, each usable as scalar or array. Every scalar occupies one uniform
4-byte slot; arrays pack by element.** A type becomes a semantic rather than a width, which deletes
the machinery instead of patching it a fifth time.

The timing was the argument: MoonLive is unlaunched, `MIGRATING.md:9` exempts it explicitly, and the
26 shipped scripts were ours to rewrite. After launch it becomes a compatibility program forever.

Approved scope: all five types in one branch, including `fixed` and `string`.

## Approach

### Storage: one slot, whatever the type

| Type | Scalar | Array element | Control |
|---|---|---|---|
| `int` | 4 bytes | 4 bytes | `Int32` (new) |
| `byte` | 4-byte slot, narrowed by the store | 1 byte | `Uint8` (a 0..255 slider) |
| `bool` | 4-byte slot, narrowed by the store | 1 byte | `Bool` |
| `fixed` | 4 bytes, Q16.16 | (refused, see below) | none |
| `string` | 4 bytes (pool offset) | not allowed | none |

The width question survives only in arrays, where it pays for itself: a `byte[]` heat map costs a
quarter of an `int[]` one, and the classic ESP32 has no PSRAM to absorb the difference.

### Typing without an AST

The compiler is single-pass with no tree, so a type rides alongside the value: every parse function
sets `exprIsFixed` before returning, and the places where two values meet compare it. `byte` and
`bool` decay to `int` on read (they are semantics on storage, not on arithmetic), so the question is
only ever "is this Q16.16 or a plain integer".

Mixing is a compile error naming the conversion, because at run time the two are the same 32 bits.
The exception is an integer **literal**, which adopts the fixed side by patching its own already
emitted `Const` (free at run time), so `v * 2`, `if (v < 0)` and `c = 5;` read naturally while a
*variable* keeps the explicit rule: a literal's meaning is visible at the site, a variable's is not.

### `fixed` gets real instructions

The JIT had no shift primitive and no multiply-high: `mulReg` is a plain 32-bit multiply and `/` was
already a host call. Routing `fixed` through host builtins would have put a call in every per-pixel
multiply and made `int`↔`fixed` conversion, one shift, a function call. So four primitives went
into all four backends: `mulhi`, `shlImm`, `shrImm`, `sarImm`, plus 32-bit slot access.

A fixed multiply is `Mulhi + Mul + two shifts`. A fixed divide goes through a **host** call (`fdiv`)
that widens in int64: any 32-bit pre-shift wraps past |128.0|, which is exactly the range shaders use.

### One control declaration

`addUint8`/`addUint16` collapse into `addControl(name, member, min, max)`, which reads the widget from
the member's declared type, so a call and a declaration can no longer disagree. A `byte` control's
descriptor points at its slot's low byte, which is only sound because the store narrows: the upper
three bytes are always zero.

## Verification

- The unit suite and 20 scenario tests, all 11 pre-commit gates, on arm64 AND on x86-64
  under Rosetta: the x86-only tests no arm64 run compiles are where two defects hid.
- Every shipped script compiles, on the host backend and on both device ISAs.
- `disasm.py` on all four backends, reading the emitted sequences by eye.
- On hardware: desktop and an ESP32-S3 (shiffy), with the product owner's eyes on metal, fractal,
  ripples, plasma and ember.

## What the design did not anticipate

Recorded because the plan was wrong about them, and the next reader should not re-derive them:

- **`fixed` needed four assembler primitives, not zero.** The design assumed the shifts existed.
- **Literal adoption had to be added.** The design said any mix is an error; that made `v * 2`
  unwritable, and the language would have been unusable for shaders.
- **Two latent bugs surfaced that predate the branch**: `movImm` silently masked constants to 16 bits
  on arm64 and Xtensa (invisible until a Q16.16 literal rode one), and arena seeding wrote a single
  byte (so `int neg = -100` seeded as 156).
- **`fixed[]` is refused, not shipped.** Element type-tracking needs the array's type to reach both
  the read and the write; scalars get that from their declaration, elements would need it per array.
  Parity with `string[]`, deferred until a script needs it.
- **`bool` truncates on store rather than normalizing** (`flag = 256` reads false): normalizing in
  the emitted code needs a compare-and-select the IR has no op for, and a branch would spend two of
  the script's sixteen labels. The byte IS normalized where it matters: at publish time, before
  the UI binding reads it through a `bool*`, which would otherwise be undefined behaviour.
- **`string` is declared but inert**: a string member cannot be initialized yet, and says so.

## The lesson that cost the most

Every hand-built Xtensa encoding was byte-reversed. The two ESP toolchain objdumps print different
conventions (`esp32-elf` shows the 24-bit word, `esp32s3-elf` shows memory bytes), so verifying
against the wrong one "matched" while emitting every instruction backwards. The reversed `slli`
decoded as `l32r a1`, a stack-pointer clobber that hung the board with no panic text while all 1400+
host tests stayed green. Encoders now build words and emit through `emit3`/`emit2`, as the
pre-existing ones always did.
