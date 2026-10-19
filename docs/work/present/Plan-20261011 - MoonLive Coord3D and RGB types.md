# Plan: MoonLive `Coord3D` and `RGB` types

Most of what a script handles is a position or a color, and today both are loose integers: a coordinate is three values or an index the script computes by hand, and a color is three bytes that cannot travel together. This plan adds the two predefined types the [language roadmap](../future/backlog-moonlive.md) names as item 4b, so a script reads as its compiled counterpart does: `addRGB(i, getRGB(j))`. It lands now because every script written before it is a script to revisit after it.

## The rule it follows

The vocabulary follows the compiled modules ([MoonLive architecture](../../explanation/architecture/moonlive.md)): `Coord3D` and `RGB` are the C++ types of the same names, and every builtin taking or returning one is a one-line call into the `draw` function that does the same for a compiled effect.

## The design: a struct is a named fixed-size array

`RGB c` is `byte c[3]` and `Coord3D p` is `int p[3]`, with `r g b` and `x y z` naming elements 0, 1 and 2. A member struct reuses the member arrays MoonLive already has, packed at element width, so an `RGB` member takes 3 bytes in one member record; a local struct takes three frame slots. The backends never see a struct: every use expands into the scalar operations a script writes today.

- **A struct argument expands into its fields.** `setRGB(i, c)` is `setRGB(i, c.r, c.g, c.b)`, so the existing builtins take structs with no overload.
- **A struct return goes through output words.** A builtin writes its fields into its argument slots, which the parser reloads; a script function returning a struct uses the script-argument block, free after the callee's entry copy.
- **A struct is a full type:** a member, a local, a parameter and a return value, with field access, whole-struct assignment, and a constructor expression `RGB(r, g, b)`.

## The steps

1. A byte-identical harness first: every shipped script's IR, with call targets named rather than addressed, recorded before any compiler change and compared after each.
2. The two types: declarations, field access, whole-struct assignment, the constructor; members as named arrays, locals as three slots.
3. Struct arguments expanding into fields; struct returns through output words, and `paletteRGB(idx, bri)` returning an `RGB` in one call where effects now make three.
4. Script functions: parameters raised from 4 to 8 words (16 more arena bytes per running script), a struct parameter taking three, and struct returns.
5. `&&`, `||` and `!` in conditions, short-circuit, lowered to the branches `if` and `while` already use; as a value they would need a set-on-condition operation and stay out.
6. `abs`, `min`, `max` and `clamp` as common builtins.
7. The assemblers' label and fixup tables off the stack and onto the compile-time heap, with the limits raised (IR labels 40 to 96, fixups 96 to 255), since each `||`, `!` and `!=` takes a label and two shipped scripts already hit "assembler overflow".
8. A clear error at every limit a struct or operator meets.
9. Tests: the compiler on every addition, scripts using them on every backend, the IR sizing against a struct-dense script, hpwit's loop-variable trap (a variable declared before a loop, used in it and after it, run on every backend), and the vocabulary page.
10. The catalog scripts rewritten where the new forms read better, each compiled and run on the desktop, the harness then recording their new IR.

## Cost

| | Flash | Memory | Hot path |
|--|--|--|--|
| Compiler | Front end only, a few KB once per firmware, nothing in the five backends | Compile time: struct uses expand to three IR ops each, so the IR buffer sizing is rechecked; label tables move to the heap | None: the emitted code is what the expanded scalar form emits today |
| A script | Unchanged code for unchanged scripts | 16 bytes more arena per running script for the 8 parameter words; an `RGB` member 3 bytes, a `Coord3D` 12 | `paletteRGB` replaces three palette calls with one per light |
| Builtins | Four small common builtins and `paletteRGB` | None | A call each, as any builtin |

## Risk

A predefined type or builtin name joins the one namespace every script shares, so a user script with a variable named `RGB`, `Coord3D`, `abs`, `min`, `max` or `clamp` stops compiling with "that name is a type" or "that name shadows a built-in function"; no shipped script does, and the compile-every-script test finds any that would.
