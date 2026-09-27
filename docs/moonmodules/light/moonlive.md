# MoonLive

The MoonLive language: what a script may declare, what the engine hands it, and what it can call.
A script compiles to native code on the device, so it runs at the speed of a compiled module.
The library that ships with MoonLight is [moonlive/](https://github.com/MoonModules/projectMM/tree/main/moonlive), and the shortest way in is [Write your first script](../../tutorials/first-script.md).

## A script is a class

Each script declares a **class**, and the host calls its functions: `tick()` for an effect, `placeLights()` for a layout, `modifyLogical()` for a modifier. A function is called when it is present and its moment arrives, so which entry points a class defines is what decides what it does.
The class name is independent of the file name, the way a C file and the functions in it are.

A class may also define functions of its own and **call them**, including calling itself:

```
class CrosshairEffect {
  byte bpm = 30;

  void defineControls() { addControl("bpm", bpm, 1, 240); }

  void column(int cx) { for (int y = 0; y < height; y = y + 1) { setRGB(y * width + cx, 255, 40, 0); } }
  void tick()         { fill(0, 0, 0); column(scale(beat(bpm, t), width)); }
}
```

These are real calls, not pasted-in text: the callee gets its own frame when it runs, which is what lets one helper call another and lets a function recurse. Arguments are passed BY VALUE, so a function that writes a parameter changes its own copy and the caller's variable is untouched.
`effects/crosshair.mle` is the worked example; `layouts/sixteen-rings.mll` is the one where the arguments earn their place, calling one helper sixteen times with different coordinates.

### A script is C++

Every shipped script compiles under `c++ -std=c++20 -fsyntax-only`, pinned by `test/python/test_scripts_are_cpp.py`.
The language therefore stays a subset rather than drifting into a dialect one feature at a time.
An editor highlights a script correctly, and a reader brings their C++ intuition to it.

One shape difference is deliberate, and the test bridges exactly that one: a class here needs no `public:` and no trailing semicolon.

Anything else a compiler rejects is a divergence, and the test is where it surfaces.

### Every function declares what it returns

A script does this the way the compiled module it stands in for does.
`void` acts and answers nothing, `int` hands back a number, and `string` hands back one of the script's own literals.

`return` leaves a function, with a value or without one. Inside `tick()` a bare `return;` is an early exit, which is what a guard wants:

```
void tick() {
  if (width < 2) { return; }        // nothing to draw on a single column
  fill(0, 0, 0);
}
```

`string` names what comes back rather than introducing a string type: a script returns a literal, and building, joining or comparing text is out of scope.

### Members, and which become controls

`byte bpm = 30;` is state the script owns: visible in every function, surviving every tick. Naming it in `defineControls()` with `addControl("bpm", bpm, 1, 240)` also puts it on the UI as a slider, which is the same call a compiled module makes. A member no `addControl` names stays private to the script, which is how a stateful effect holds a value the user should not see.

The default comes from the declaration, the range from the call, and the quoted name is the UI label, free to differ from the member's name.

### A member can be written

That is what makes it state. `level = level + 10;` assigns, and the value is still there on the next tick, because a member lives in storage that outlives the call. A loop variable can be assigned too. A [system variable](#system-variables) (`width`, `t`, `xPos`) cannot: the engine rewrites it before every call, so the store would vanish.

A control CAN be assigned, and the effect is visible rather than surprising: the value moves under the slider until the user drags it again. Whether a member is a control is decided by `defineControls()` at run time, so the language does not distinguish the two here.

### Branching

`if` and `else` take `<`, `<=`, `>`, `>=`, `==` and `!=`, and both sides are ordinary expressions:

```c
if (heat[i] > 40) { setRGB(i, 255, 90, 0); }
else { setRGB(i, 0, 0, 0); }
```

### Wider members, and arrays

`byte` spans 0..255, and `int` spans
-2,147,483,648..2,147,483,647, which is what a position on a wall wider than 255 needs. An array is
declared with a literal length and starts at zero:

```c
int phase = 900;      // a value a byte cannot hold
byte  heat[16];         // sixteen elements, all zero to begin with
```

### Every variable is declared

That includes a loop's counter. A member states its type, an assignment to a name that was never declared is refused, and a `for` writes `for (int i = 0; ...)`. One rule with no exception, and the same line C++ would take.

An index is an arbitrary expression such as `heat[i * 2 + 1]`, and one outside the array is **clamped to the last element**.
A script computes indices from live control values, so out of range is an ordinary run-time state.
The fixture shows a repeated last light rather than crashing.

All of a class's members share a small fixed budget (`kCtrlBytes`), so a class that declares more than fits is a compile error naming the arena, not a failed allocation while a fixture runs.

`effects/ember.mle` is the worked example: a heat array that decays and re-ignites, so what it draws this frame depends on the last one. That is the line between an effect that evaluates a formula and one that runs a simulation, and it is the reason arrays exist. `plasma.mle` would look identical if every frame started from scratch; `ember.mle` would go dark.

### Helpers and recursion

Declare a helper above the function that calls it. Only functions already parsed are visible, so a call to one declared further down reports `unknown function`. A function can always call itself.

Recursion is bounded. About 30 calls deep, a further call does nothing and returns. A render task has a fixed stack, so the alternative to a limit is a device that resets mid-frame. What you see if you hit it is the picture being wrong where the recursion stopped, on a device that keeps running. Nothing is reported; the exact depth is `kMaxCallDepth`.

### A script says what it is

`dimensions()` and `tags()` are both optional, both named after the member functions a compiled module declares (`Dim dimensions() const override`, `const char* tags() const override`), and both read once when the script compiles.

```
class RainEffect {
  int dimensions() { return 2; }        // an x/y picture
  string tags() { return "✨"; }         // shown on the card and in the picker

  void tick() { fill(0, 0, 40); }
}
```

`dimensions()` returns 1, 2 or 3, and it decides how the layer EXTRUDES the script. A script that returns 1 paints the x=0 column and the framework fans it across the width; one that returns 2 paints the z=0 slice and the framework copies it through the depth. So a script fills a rig it never indexed, and a wrong answer is visible: declare 1 and paint a picture, and only the first column survives. A script that stays silent is treated as 2, which is what every script rendered as before this existed.

`tags()` returns the emoji shown beside the script, so a row in the picker reads like a compiled effect's. The vocabulary is shared with the compiled modules: 📊 audio-reactive, ✨ particles, 🎯 aims moving heads. A script that declares none shows 📝, the mark of a scripted effect.

Both reach the picker before a factory script is downloaded, because the build extracts them from the source into the catalog. That copy is for display only: once a script is on the device, the compiled script is what decides.

## The five types

A type says what a value means, and the storage is the compiler's business.
Every scalar occupies the same 4-byte slot whatever its type, and only arrays pack by element.
That is where width still earns its keep: a `byte[]` heat map costs a quarter of an `int[]` one.

| Type | Range | For |
|---|---|---|
| `int` | -2,147,483,648 to 2,147,483,647 | counts, indices, milliseconds, anything whole |
| `byte` | 0 to 255 | a channel, a palette index, a heat cell: the LED's own range |
| `bool` | `true` or `false` | a flag |
| `fixed` | -32,768.0 to 32,767.99998, in steps of 1/65,536 | coordinates and anything fractional |
| `string` | one of the script's own literals | a name passed to a builtin |

An initializer is range-checked against its type, so `byte n = 300;` is a compile error naming the member rather than a silent 44.
A `string` array is refused, since there is no runtime string to fill one with.

## Fractional arithmetic with `fixed`

`fixed` is Q16.16: the number is stored scaled by 65,536, which is how every coordinate in the engine works.
There is no float anywhere, so a result is bit-identical on all four backends and an effect is reproducible.

```c
fixed ux = 0.0;
ux = uvX(x, width, height);     // uvX and uvY hand back a fixed coordinate
ux = ux * 2 + 0.5;              // ordinary arithmetic, decimals written as decimals
setRGB(0, toInt(ux * 100), 0, 255);
```

Mixing a whole number and a fixed value is a compile error naming the conversion to write.
At run time the two are the same 32 bits, so a silent mix is a number 65,536 times off with nothing reporting it.
`toFixed(v)` and `toInt(v)` convert explicitly, each one instruction.

An integer literal is the exception: it adopts the fixed side and converts at compile time, so `ux * 2` and `ux = 5;` read naturally and cost nothing.
A variable keeps the explicit rule, because its scaling is invisible where it is used.

## System variables

Some names are reserved: the engine defines them, a script reads them, and a declaration reusing one is a compile error.
One vocabulary serves every role, so a name means the same thing in a layout, an effect and a modifier.

| name | what it is |
|---|---|
| `t` | elapsed milliseconds, the clock an animation is written against |
| `width`, `height`, `depth` | the logical grid |
| `xPos`, `yPos`, `zPos` | the light being transformed, which a modifier is handed |

Each is a full 32-bit value, so a 768-wide wall reports 768 rather than the 255 a byte-wide slot once reported.
A modifier handed a coordinate it cannot place passes it through untransformed.
So a script always sees a value that means what it says.

The coordinate is `xPos`/`yPos`/`zPos` rather than `x`/`y`/`z`, which keeps `x` and `y` free as loop counters in every script.
Reserving those globally would break the most ordinary code there is.

An effect is told its canvas rather than declaring it, since a size restated as a control is a second answer that can disagree with the first.
A layout is upstream of that grid, so it names its own controls such as `cols` and `rows`.

## The vocabulary

Registered by the light domain rather than built into the compiler, so the list is one edit in `MoonLiveBuiltins_light.h`.
The core owns the grammar and a generic call mechanism, and nothing else.

| call | does |
|---|---|
| `setRGB(index, r, g, b)` | write one light |
| `setXYZ(x, y, z)` | write one position, for a modifier |
| `fill(r, g, b)` | write every light |
| `addLight(x, y, z)` | place the next light, for a layout |
| `line(x1, y1, x2, y2, r, g, b)` | a straight segment, via the shared `draw::line` |
| `random16(n)` | a value in `[0, n)` |
| `mod(a, b)` | the wrap a cyclic animation needs |
| `beat(bpm, t)` | a `0..65535` sawtooth at `bpm` |
| `beatsin(bpm, t, high)` | a sine `0..high` at `bpm` |
| `noise(x, y, z)` | `0..255` gradient noise, the field behind fire, clouds and plasma |
| `scale(value, n)` | a `0..65535` value onto `0..n-1`, which lands a wave on an axis |
| `sin(angle)`, `cos(angle)` | the circle: one turn is `0..65535`, centered at 32768 |
| `turn(n)` | one revolution split `n` ways, the angle step for `n` points on a circle |
| `print(v)` | log a value and return it, which belongs in a cold path |
| `a / b`, `a % b` | divide and remainder, each a host call. Dividing by zero saturates, and a remainder by zero is 0 |
| `toFixed(v)`, `toInt(v)` | convert between a whole number and a fixed one |
| `smoothstep(e0, e1, v)` | a soft `0..65535` ramp between two edges, the anti-aliasing primitive |
| `uvX(x, w, h)`, `uvY(y, w, h)` | shader space as a fixed value, centered on 0.0 and normalized on the short side |
| `smin(a, b, k)` | the smooth minimum of two distances, so shapes melt into one surface |
| `fade(amt)` | dim every light toward black, the trail primitive |
| `polarA(dx, dy)`, `polarR(dx, dy)` | angle and distance from a center, for a radial effect |
| `fbm(x, y, octaves)` | octaves of noise summed at doubling frequency, `0..255`: the cloud and terrain field |
| `warp(x, y, strength)` | the field sampled where the field displaced it, `0..255`: the marbled look |
| `fbm3(x, y, z, octaves)`, `warp3(x, y, z, strength)` | the same two fields with a third axis |
| `osc(rate, ms, shape)` | an oscillator, `0..65535`, at `rate` cycles per minute. Shapes: 0 sine, 1 triangle, 2 sawtooth, 3 square |
| `escape(cx, cy, jx, jy, iters)` | the Mandelbrot escape count, `0..255`, `0` inside the set |
| `setPaletteColor(x, y, index, bri)` | one light from the active palette, in one call |
| `setPaletteColorZ(x, y, z, index, bri)` | the same, addressing a light in a volume |
| `paletteR(i, bri)`, `paletteG`, `paletteB` | one palette channel, for a value rather than a pixel |
| `trail(1)` | ask for a trail plane, from `defineControls()`. Returns whether it got one |
| `flowNoise(zoom, strength)`, `flowCurl(zoom, strength)` | carry the trail plane one frame along a flow |
| `trailDecay(halfLifeMs)` | fade the trail by a half-life, so a tail holds its length at any framerate |
| `emitTrail(x, y, z, index, bri, radius)` | throw light into the trail as a disc |
| `fieldRate(n)` | true once every n frames, the lever that makes a per-pixel loop affordable |
| `pool(n)` | size this script's particle pool, from `defineControls()`. Returns what it got |
| `emit(x, y, angle, speed, n, life, hue)` | throw `n` particles from a point |
| `gravity(g)`, `drag(k)` | the two forces |
| `step()` | move every particle, and drop what left the grid |
| `age(rate)` | count down life; a dead particle frees its slot |
| `bounce(e)` | reflect off the grid walls, keeping `e`/256 of the speed |
| `collide(radius)` | particles notice each other and pile up, at a cost above linear |
| `render(maxLife)` | draw the pool from the active palette |
| `audioLevel()`, `audioSmooth()` | how loud the room is: the raw level, and the one that swells |
| `audioBand(i)` | one of sixteen log-spaced magnitudes, bass at 0 and treble at 15 |
| `audioPeakHz()`, `audioBeat()` | the dominant frequency, and true on a transient |
| `setPan(i, v)`, `setTilt(i, v)` | aim one moving head, which a strip ignores |
| `setZoom(i, v)`, `setRotate(i, v)`, `setGobo(i, v)` | shape that head's beam, wheel and pattern |
| `setPalEntry(i, r, g, b)` | write one of the sixteen active palette entries |
| `setPalEntryHSV(i, h, s, v)` | the same, in the space a palette is usually reasoned in |
| `addControl(name, member, min, max)` | surface a member in the UI, from `defineControls()` |
| `gpioRead(pin)`, `gpioWrite(pin, v)` | read and drive a pin, from a service script |
| `adcRead(pin)`, `adcMv(pin)`, `adcMax()` | an analog reading: raw, in millivolts, and its full scale |
| `setControl(module, control, v)` | drive another module's control, which is what a service does |

Every audio call answers 0 without audio, so a script written for a rig with a microphone still runs on one without.
A motion call reaches nothing on a light that carries no such channel, so one script serves a moving head and a strip.
The last four belong to a service script, which runs on the poll rather than the render tick.

Each particle call is one pass over the whole pool per frame rather than one per light.
A 300-spark script therefore costs far less than a shader touching every pixel.
Size the pool from `defineControls()`, which keeps an allocation off the render path.
The vocabulary follows the [WLED Particle System](https://github.com/wled/WLED) by Damian Schneider ([@DedeHai](https://github.com/DedeHai)); the fixed-point kernel and this binding are ours.

`sin` and `cos` return an unsigned wave centered on 32768, so a coordinate comes from scaling by the full span.
`uvX` and `uvY` return a signed coordinate with the grid's center at 0, because a coordinate has an origin where a wave does not.

`noise` takes 16.8 fixed-point coordinates: the high byte selects the noise cell and the low byte interpolates within it.
Its time axis must be monotonic, so feeding it a `beat()` sawtooth reads as a hiccup once per beat.

`turn(n)` exists because a full revolution is 65536, one past the largest number a script can write.

## Debugging: print

`print(v)` writes a value to the serial log and returns it, so it wraps any part of an expression without changing the result.
`addLight(print(xx), yy, 0)` places the same light and reports what `xx` was.

**Take it out again when the script works.** A serial write blocks, and a script runs on the render
tick, so a print costs frame time every frame it survives. Each compile grants a short burst and then goes quiet, which bounds the damage and gives every edit a fresh window; it does not make a print free. No script in this folder ships with one.

## Source

[MoonLive](../core/moxygen/MoonLive.md) is the engine, [MoonLiveCompiler](../core/moxygen/MoonLiveCompiler.md) the front-end, and [MoonLiveBuiltins_light](moxygen/MoonLiveBuiltins_light.md) the table registering every call above. [The lowering](../core/moxygen/moonlive_lower.md) turns the intermediate form into machine bytes, written once for every backend.
