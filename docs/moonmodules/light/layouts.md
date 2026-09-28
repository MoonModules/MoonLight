# Layouts

![layouts controls](../../assets/core/Layouts.png)

Every layout, one block each: what it does and what each control means — together. A layout maps light indices to physical `(x, y, z)` positions — it defines the *shape* an [effect](effects.md) draws onto and a [driver](drivers.md) sends out. The [Layouts](moxygen/Layouts.md) container holds one or more layout children and composes them into one coordinate space; a [Layer](moxygen/Layer.md) renders over that combined space. (For how this page maps to the source/asset folders, see the [folder-structure decision](../../contributing/documentation-standards.md#module-pages).)

<video src="../../assets/uiscenarios/05-layouts.webm" controls playsinline width="720" title="Layouts: a grid, a ring, a spiral, a cube, tubes, and two written in MoonLive."></video>

## MoonLight layouts

<a id="carlights"></a>

### Car Lights

A pair of concentric-ring "headlight" clusters (nested rings of 1/8/12/16/24 LEDs) positioned to mimic a car's front lights — a fixed arrangement composed from [Ring](#ring) geometry.

<img src="../../assets/light/layouts/CarLightsLayout.gif" width="300" alt="Car Lights layout preview">

- `scale` — overall size scale (1–10).

Origin: Eric Marciniak (Discord) — custom car-lights fixture

Detail: [technical](moxygen/CarLightsLayout.md)

<a id="cube"></a>

### Cube

<img src="../../assets/light/layouts/CubeLayout.gif" width="300" alt="Cube layout preview">

A 3D cube volume, `width`×`height`×`depth`, wired in a configurable axis order with optional per-axis serpentine — the 3D generalisation of Panel.

- `width` / `height` / `depth` — cube extent per axis (1–128).
- `wiringOrder` — the axis nesting order the strip follows.
- `X++` / `Y++` / `Z++` — count up (vs down) along that axis.
- `snakeX` / `snakeY` / `snakeZ` — serpentine on that axis.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/CubeLayout.md)

<a id="humansizedcube"></a>

### Human-Sized Cube

<img src="../../assets/light/layouts/HumanSizedCubeLayout.gif" width="300" alt="Human-Sized Cube layout preview">

A hollow walk-in cube built from five LED-curtain faces (front, back, top, left, right), each a `width`×`height`×`depth` curtain — for large/room-scale cube installations.

- `width` / `height` / `depth` — cube extent per axis (1–20).

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/HumanSizedCubeLayout.md)

<a id="panel"></a>

### Panel

<img src="../../assets/light/layouts/PanelLayout.gif" width="300" alt="Panel layout preview">

A 2D matrix panel with full wiring control: choose the axis order, per-axis direction, and serpentine — the general matrix layout ([Grid](#grid) is the simple case).

- `panelWidth` / `panelHeight` — panel size in lights (1–512).
- `wiringOrder` — `XY` (rows) or `YX` (columns) nesting.
- `X++` / `Y++` — count up vs down along that axis.
- `snake` — serpentine wiring (alternate lines reverse).

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/PanelLayout.md)

<a id="panels"></a>

### Panels

<img src="../../assets/light/layouts/PanelsLayout.gif" width="300" alt="Panels layout preview">

Tiles an M×N grid of full matrix panels into one large display: an outer walk over the panel grid plus an inner walk over each panel's lights, both independently wired — for multi-panel video walls.

- `horizontalPanels` / `verticalPanels` — panel-grid size (1–32 each).
- `wiringOrderP` / `X++P` / `Y++P` / `snakeP` — the panel-to-panel wiring.
- `panelWidth` / `panelHeight` — each panel's size (1–512).
- `wiringOrder` / `X++` / `Y++` / `snake` — the per-panel light wiring.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/PanelsLayout.md)

<a id="ring"></a>

### Ring

<img src="../../assets/light/layouts/RingLayout.gif" width="300" alt="Ring layout preview">

A single ring of LEDs evenly spaced around a circle — `nrOfLEDs` points, starting at `angleFirst`, spanning `rotation` degrees.

- `nrOfLEDs` — LEDs around the ring (1–255).
- `angleFirst` — starting angle in degrees.
- `rotation` — arc spanned (360 = full circle).
- `clockwise` — direction of travel.
- `scale` — spacing/radius scale.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/RingLayout.md)

<a id="rings241"></a>

### Rings 241

<img src="../../assets/light/layouts/Rings241Layout.gif" width="300" alt="Rings 241 layout preview">

The classic 241-LED concentric-ring disc: nested rings of 1, 8, 12, 16, 24, 32, 40, 48, 60 LEDs sharing a center.

- `scale` — overall radius scale (1–10).
- `outside in` — light 0 on the outer ring, wired inward rather than outward.
- `angleFirst` — where light 0 of each ring sits, in degrees.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/Rings241Layout.md)

<a id="singlecolumn"></a>

### Single Column

<img src="../../assets/light/layouts/SingleColumnLayout.gif" width="300" alt="Single Column layout preview">

A vertical line of LEDs at a fixed X — the 1D column primitive.

- `starting Y` — the column's start row.
- `height` — LEDs in the column (1–1000).
- `X position` — the column's x.
- `reversed order` — wire top-to-bottom instead of bottom-to-top.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/SingleColumnLayout.md)

<a id="singlerow"></a>

### Single Row

<img src="../../assets/light/layouts/SingleRowLayout.gif" width="300" alt="Single Row layout preview">

A horizontal line of LEDs at a fixed Y — the 1D row primitive.

- `starting X` — the row's start column.
- `width` — LEDs in the row (1–1000).
- `Y position` — the row's y.
- `reversed order` — wire right-to-left instead of left-to-right.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/SingleRowLayout.md)

<a id="spiral"></a>

### Spiral

<img src="../../assets/light/layouts/SpiralLayout.gif" width="300" alt="Spiral layout preview">

A conical spiral: `ledCount` LEDs winding up a cone from `bottomRadius` to a point over `height`.

- `ledCount` — LEDs along the spiral (1–2048).
- `bottomRadius` — radius at the base.
- `height` — spiral height.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/SpiralLayout.md)

<a id="torontobargourds"></a>

### Toronto Bar Gourds

Maps a set of decorative "gourd" objects (a specific bar installation), each rendered at one of three granularities — one light per gourd, per side, or per LED.

<img src="../../assets/light/layouts/TorontoBarGourdsLayout.gif" width="300" alt="Toronto Bar Gourds layout preview">

- `granularity` — one light per gourd, per side, or per LED.
- `nrOfLightsPerGourd` — LEDs per gourd in the coarsest mode (1–128).

Origin: [troyhacks](https://github.com/troyhacks/WLED) — custom Toronto bar gourd installation

Detail: [technical](moxygen/TorontoBarGourdsLayout.md)

<a id="tubes"></a>

### Tubes

<img src="../../assets/light/layouts/TubesLayout.gif" width="300" alt="Tubes layout preview">

Parallel vertical tubes: `nrOfTubes` columns of `ledsPerTube` LEDs, spaced `tubeDistance` apart.

- `nrOfTubes` — number of tubes (1–64).
- `ledsPerTube` — LEDs per tube (1–255).
- `tubeDistance` — spacing between tubes.
- `reversed` — reverse the wiring order.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/TubesLayout.md)

## MoonLight-native layouts

<a id="moonlive"></a>

### MoonLive

<img src="../../assets/light/layouts/MoonLiveLayout.gif" width="300" alt="MoonLive scripted layout preview">

Where the lights physically are, written as text on the running device. A layout is the one part of the pipeline that differs for every build: a ring, a spiral staircase, a costume sewn last night. A script means the person who hung the lights describes where they went, and sees it immediately. The language is [MoonLive](moonlive.md).

- `script`: which `.mll` file runs, picked from the library and edited here.
- Everything the script declares appears as a real control.

Origin: MoonLight original

Detail: [technical](moxygen/MoonLiveLayout.md) · [how the count is known](#moonlive-details)

[Tests](../../reference/tests/unit-tests.md#moonlivelayout)

<a id="grid"></a>

### Grid

<img src="../../assets/light/layouts/GridLayout.gif" width="300" alt="Grid layout preview">

A dense 3D grid, row-major (x fastest, then y, then z); every position maps to a light.

- `width` / `height` / `depth`: lights per axis (to 3840, 2160 and 512).
- `serpentine` — every other row runs in reverse, matching a snaked strip.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/GridLayout.md)

[Tests](../../reference/tests/unit-tests.md#gridlayout)

<a id="gridblacks"></a>

### GridBlacks

<img src="../../assets/light/layouts/GridBlacksLayout.gif" width="300" alt="GridBlacks layout preview">

A [Grid](#grid) with **mid-strand dark columns**, held black in every row, for a sealed panel that must stay dark down a strip or a slat wall. A dark column is still a wire position the driver clocks, so data flows through the unlit LEDs to the lit columns beyond. The lit columns keep their true positions, so an effect maps straight across the gap.

- `width` / `height` / `depth` — grid extent on each axis in lights (1–512).
- `serpentine` — boustrophedon-wire alternate rows.
- `blackCount` — how many dark columns; `0` renders exactly like a Grid.
- `blackStart` — first dark column (shown only once `blackCount` is set).

Origin: MoonLight

Detail: [technical](moxygen/GridBlacksLayout.md)

[Tests](../../reference/tests/unit-tests.md#gridblackslayout)

<a id="sphere"></a>

### Sphere

<img src="../../assets/light/layouts/SphereLayout.gif" width="300" alt="Sphere layout preview">

Lights on the surface of a hollow sphere — a one-light-thick shell inside a `(2·radius+1)³` box, no interior lights.

- `radius` — the shell's radius in light-units (1–64).

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/SphereLayout.md)

[Tests](../../reference/tests/unit-tests.md#spherelayout)

<a id="wheel"></a>

### Wheel

<img src="../../assets/light/layouts/WheelLayout.gif" width="300" alt="Wheel layout preview">

A bicycle-wheel: `spokes` straight rows radiate from a center hub, each carrying `ledsPerSpoke` LEDs spaced one unit apart outward.

- `spokes` — number of spokes radiating from the hub (2–64).
- `ledsPerSpoke` — LEDs along each spoke, one unit apart.

Origin: MoonLight · via [MoonLight](https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h)

Detail: [technical](moxygen/WheelLayout.md)

[Tests](../../reference/tests/unit-tests.md#wheellayout)

The [Layouts](moxygen/Layouts.md) container itself takes no controls — see its page for coordinate iteration, reordering, and rebuild propagation.

## MoonLive, details

#### How the light count is known

A layout answers how many lights before it produces a single coordinate, because the Layer sizes its buffer from that number and only then asks where each light is. A script cannot be asked how many without running it.

So it runs twice. The first pass counts what `addLight` places, and the second emits each position. Same script and same arithmetic, so a deterministic script agrees with itself, which is what the compiled layouts do for the same reason.

Nothing is stored between the passes. Staging 16,384 coordinates costs 48 KB, which a classic ESP32 driving that many lights does not have spare. Running the script again is cheaper than remembering what it said.

A script calling `random16` breaks that determinism. The passes disagree on the count when a random value decides a loop bound, while a random coordinate keeps the count right and places the lights elsewhere on the second pass.

#### What a layout script is given

`addLight(x, y, z)` places the next light along the strand. There is no index, because the order the script calls it in is the strand order.

`t` is the one system variable a layout reads, and it is always 0: the script runs twice per rebuild and must agree with itself. `width`, `height` and `depth` read 0, since a layout is upstream of the grid it defines.

A script names its own size controls, such as `cols` and `rows`. The pipeline derives the bounding box from the coordinates actually placed, so a size passed in from outside would be a second answer that could disagree with the first.
