# Plan: MoonLive `Coord3D` and `RGB` types

Most of what a script handles is a position or a color, and today both are loose integers: a coordinate is three values or an index the script computes by hand, and a color is three bytes that cannot travel together. This plan adds the two predefined types the [language roadmap](../future/moonlive-language-roadmap.md) names as item 4b, so a script reads as its compiled counterpart does: `addRGB(i, getRGB(j))`. It lands now because every script written before it is a script to revisit after it.

## The rule it follows

The vocabulary follows the compiled modules ([MoonLive architecture](../../explanation/architecture/moonlive.md)): `Coord3D` and `RGB` are the C++ types of the same names, and every builtin taking or returning one is a one-line call into the `draw` function that does the same for a compiled effect.

## The design: fields under one name

The compiler treats a struct as its fields under one name: `RGB c` is three byte slots `c.r`, `c.g` and `c.b`, and `Coord3D p` is `p.x`, `p.y` and `p.z`. The backends never see a struct, so none of the five changes.

- **A struct argument expands into its fields.** `setRGB(i, c)` is `setRGB(i, c.r, c.g, c.b)`, the existing four-argument builtin, so no overload is needed and no shipped script breaks.
- **A struct return goes through output arguments.** `getRGB(i)` writes its three fields through the by-reference arguments builtins already support; a script function returning a struct gets the same hidden output slots.
- **A struct is a full type:** a class member, a local, a parameter and a return value, with field access, assignment of a whole struct, and a constructor expression.

## The steps

1. The two types in the parser and the symbol table: declarations, field access, whole-struct assignment, the constructor expression.
2. Expansion into fields for every use, builtin arguments and returns included.
3. The color builtins in struct form: `getRGB(i)` returning an `RGB`, and the existing color calls taking one through expansion.
4. The two limits a struct meets: four parameters per script function, which one `Coord3D` mostly fills, and the 32 frame slots, of which each struct takes three.
5. Tests: the compiler on both types, scripts using them on every backend, the shipped scripts' code byte-identical, and the vocabulary page.
6. One compiler trap from hpwit's new parser that applies here, while the work is in the same compiler: a variable declared before a loop, used inside it and after it, run on every backend, so the register allocator keeps it alive across the loop's back edge. His two other traps (an optimizer pass, a code-size estimate) have no counterpart in MoonLive.

## Cost

| | Flash | Memory | Hot path |
|--|--|--|--|
| Compiler | The type and expansion code in the front end | None at run time | None: the emitted code is what the expanded scalar form emits today |
| A script using them | Nothing new | Three slots or member bytes per struct, as three scalars take | Unchanged |

## Risk

A predefined type name joins the one namespace every script shares, so a script with a variable named `RGB` or `Coord3D` stops compiling; the compile-every-script test finds any such script at once.
