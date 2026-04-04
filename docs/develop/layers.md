# Layers

## Overview

MoonLight uses a two-level layer model: one **PhysicalLayer** owns the hardware channel buffer and all **VirtualLayers**. Effects write to isolated per-layer buffers; the results are composited into the display buffer once per frame.

```text
PhysicalLayer (layerP)
├── lights.channelsD          ← single display buffer, read by drivers
├── lights.header             ← size, nrOfLights, channelsPerLight, …
├── layers[0] VirtualLayer    ← first effect stack
│   ├── virtualChannels       ← per-layer pixel buffer (effects write here)
│   ├── mappingTable[]        ← virtual→physical index lookup
│   ├── nodes[]               ← effect / modifier nodes
│   └── brightness, startPct/endPct, transitionBrightness
├── layers[1] VirtualLayer    ← second effect stack (created on demand)
└── nodes[]                   ← layout / driver nodes (physical layer)
```

`layerP.layers` is a 16-element `std::vector<VirtualLayer*>` pre-allocated in the `PhysicalLayer` constructor with every slot set to `nullptr`. `activeLayerCount` tracks how many slots are currently instantiated. Hot-path loops iterate the full vector with `if (!layer) continue;` — new code must follow the same pattern and never assume all slots are populated.

---

## Node types

| Type | Runs on | Purpose |
|------|---------|---------|
| **Layout** | PhysicalLayer | Calls `addLight(Coord3D)` to register physical lights and positions |
| **Effect** | VirtualLayer | Writes pixel colours to `virtualChannels` each frame |
| **Modifier** | VirtualLayer | Transforms virtual coordinates (mirror, rotate, projection, …) |
| **Driver** | PhysicalLayer | Reads `channelsD` and sends to hardware (FastLED, ArtNet, DDP, …) |

---

## Virtual Layer

Each `VirtualLayer` holds:

- **`virtualChannels`** — per-layer pixel buffer, indexed by virtual pixel. Allocated after layout completes, persists across frames (fade-trail effects see previous frame data).
- **`mappingTable[]`** — one `PhysMap` entry per virtual pixel:
  - `m_zeroLights` — not mapped (stays black)
  - `m_oneLight` — maps to one physical light
  - `m_moreLights` — maps to multiple physical lights (fan-out from modifiers)
- **`oneToOneMapping`** — `true` when virtual = physical, no table needed; fastest path.
- **`allOneLight`** — `true` when no fan-out exists; enables the serpentine fast path.
- **`brightness`** (0–255) — per-layer output brightness.
- **`transitionBrightness`** — animated brightness stepped per frame for smooth fade-in/out; triggered automatically when a new effect is activated.
- **`startPct` / `endPct`** — layer bounds as percentages of the full fixture.

### Mapping table example

```text
virtual index:  0    1    2    3    4
PhysMap:        ∅   [0]  [1,2] [3]  [4,5,6]
```

Virtual pixel 0 → unmapped, pixel 1 → physical 0, pixel 2 → physical 1 and 2, etc.

---

## Speed and memory

Symbols: **N** = virtual LEDs, **M** = average fan-out (1:N case), **L** = layers, **cpl** = channels/light (3=RGB, 4=RGBW, 5=RGBCCT, 15–32=moving heads).

`channelsD` is shared across all layers: **P × cpl bytes**.
Per-layer: `virtualChannels` **N×cpl**, `mappingTable` **N×2/4 B** (no PSRAM / PSRAM), `mappingTableIndexes` **≈N×M×2/4 B**.

| Scenario | Speed (relative) | Memory per layer | Layer overlap behaviour |
|----------|-----------------|------------------|------------------------|
| **1:1 mapping, 1 layer** | **1× (fastest)** | N×cpl | — |
| **1:1 mapping, L layers** | L× | L×N×cpl | No overlap: each pixel written once. Partial overlap: additive (saturates at 255 at full brightness, cross-fades when dimmed). |
| **Serpentine / shifted panel, 1 layer** | ~1.2× | N×cpl + N×2/4 B | — |
| **Serpentine / shifted panel, L layers** | ~1.2×L | L×(N×cpl + N×2/4 B) | Same overlap semantics as 1:1 |
| **1:N modifier (rings, circles…), 1 layer** | ~M× | N×cpl + N×(1+M)×2/4 B | — |
| **1:N modifier, L layers** | ~L×M× | L×(above) | Same; random writes amplified by L |

**Notes:**
- `cpl > 3` (RGBW, moving heads) always uses the general compositor path. Extra channels add memory proportionally (e.g. N×4 for RGBW vs N×3) and one `scale8` per white channel per pixel; control channels (pan, tilt, …) are a copy.
- 1:N random writes to `channelsD` become costly at large fixture sizes on PSRAM boards (~80 ns/access vs ~8 ns sequential). At N=1024 RGB, all data fits in L1 cache; at N=4096+ the 1:N penalty is measurable.
- Non-overlapping layers (via `startPct/endPct`) cost the same as a single full layer since each physical pixel is written exactly once.

**Example — N = 1024, cpl = 3, M = 4:**

| | 1:1, 1L | 1:1, 4L | Serpentine, 1L | Serpentine, 4L | 1:N M=4, 1L | 1:N M=4, 4L |
|---|---|---|---|---|---|---|
| virtualChannels | 3 KB | 12 KB | 3 KB | 12 KB | 3 KB | 12 KB |
| mappingTable | — | — | 2/4 KB | 8/16 KB | 2/4 KB | 8/16 KB |
| mappingTableIndexes | — | — | — | — | 8/16 KB | 32/64 KB |
| **Total (no PSRAM / PSRAM)** | **3 KB** | **12 KB** | **5/7 KB** | **20/28 KB** | **13/23 KB** | **52/92 KB** |

---

## Per-frame pipeline

```text
effectTask (Core 0)                        driverTask (Core 1)
───────────────────                        ──────────────────
PhysicalLayer::loop()
  for each VirtualLayer:
    layer->loop()                          reads channelsD (previous frame)
      fadeBy virtualChannels
      run effect nodes → virtualChannels

wait: channelsDFreeSemaphore ─────────────── give after loopDrivers() finishes

compositeLayers():
  memset(channelsD, 0)
  for each VirtualLayer:
    layer->compositeTo(channelsD)          PhysicalLayer::loopDrivers()
      scale by brightness                    process layout mapping
      additive blend → channelsD             loop driver nodes
newFrameReady = true
                                           picks up newFrameReady, sends to LEDs
                                           give channelsDFreeSemaphore
```

`virtualChannels` and `channelsD` are **separate buffers** — effectTask writes while driverTask reads, with no race. `channelsDFreeSemaphore` gates the handoff; `swapMutex` protects only the `newFrameReady` flag.

---

## Compositing

`compositeTo()` blends one VirtualLayer into `channelsD`:

- **Colour channels** (R, G, B, W): additive `+=`, saturates at 255. Two layers at brightness 128 cross-fade naturally. Rationale: additive blending matches physical light — two sources always sum.
- **Control channels** (pan, tilt, zoom, …): copy — last layer wins. Rationale: summing control signals (e.g. pan angles) is meaningless; last-wins lets effects override safely without coordination.
- Effective brightness = `scale8(brightness, transitionBrightness)`.

---

## Layout mapping pipeline

Normally triggered as a coupled two-pass operation via `requestMappings()`: layout nodes set `requestMapPhysical`; modifier-only changes set `requestMapVirtual` directly. In `PhysicalLayer::loopDrivers()`, completing pass 1 always sets `requestMapVirtual`, so pass 2 always follows pass 1. The one exception is the `/rest/monitorLayout` endpoint in `ModuleEffects`, which calls `mapLayout(pass=1, monitorPass=true)` directly to run pass 1 only — without touching the request flags and without triggering pass 2.

**Pass 1 — physical** (driverTask): layout nodes call `addLight(Coord3D)` to count lights, record positions, and assign pins.

**Pass 2 — virtual** (driverTask): layout nodes call `addLight()` again; each VirtualLayer filters by `startPct/endPct` and builds its `mappingTable`. Modifiers intercept via `modifyPosition()`. `onLayoutPost()` allocates `virtualChannels` and sets `oneToOneMapping` / `allOneLight`.

---

## Design decisions

### Virtual-layer pixel writes from driver nodes

Drivers that receive external pixel data (Network In, DMX In) and target a virtual layer **must** write into `VirtualLayer::virtualChannels[]` directly — not into `layerP.lights.channelsD` via `forEachLightIndex()`.

**Why:** `compositeLayers()` calls `memset(channelsD, 0)` before compositing every frame. Any direct write to `channelsD` is silently zeroed on the next composite cycle.

**Correct pattern:**

```cpp
xSemaphoreTake(swapMutex, portMAX_DELAY);
if (vLayer->virtualChannels && ledIndex < vLayer->nrOfLights)
    memcpy(&vLayer->virtualChannels[ledIndex * cpl], src, cpl);
xSemaphoreGive(swapMutex);
```

`compositeTo()` is called by the layer pipeline and applies the mapping table — the driver does not need to traverse it.

**Flat-index assumption:** Direct writes to `virtualChannels[ledIndex]` assume a 0-based flat virtual index. For non-trivial mapping tables (zigzag, segment) this is still correct because the mapping table is applied by `compositeTo()`. However, if the *sender* is transmitting in physical-LED order rather than virtual order and the virtual layer has a non-flat map, pixels will appear misplaced. Such setups should use the physical layer (`layer == 0`) instead.

**Null check:** Always validate `vLayer->virtualChannels` inside the mutex. Allocation happens during layout (under the `isPositions` gate, not under `swapMutex`), but a re-check inside the mutex eliminates the TOCTOU gap.

---

### LayerManager: backward-compat guard for legacy preset JSON keys

Pre-multilayer presets stored global values under bare keys `brightness`, `start`, `end`. New presets use per-layer keys `brightness_0`, `start_0`, `end_0` (suffix = layer index).

When the `LayerManager` update handler receives a bare key update it checks whether the corresponding `_0` key is present in `state->data`:

```cpp
if (state->data["brightness_0"].isNull()) return true;  // old preset — ignore
```

If absent, the update is silently dropped and per-layer defaults are kept (brightness 255, start {0,0,0}, end {100,100,100}). This prevents the bare global value from overwriting per-layer state with data that carries no layer context.

**Convention for new per-layer controls:** Follow the same `_N` suffix pattern and add an equivalent `isNull()` guard for the `_0` key in the update handler. Without the guard, any old preset JSON that happens to contain the bare key name will corrupt per-layer state silently.

---

### fadeToBlackBy accumulation

`VirtualLayer::fadeToBlackBy(amount)` writes the request into the per-layer `fadeBy` field using `fadeBy = fadeBy ? MIN(fadeBy, amount) : amount`. When multiple callers invoke it in the same frame the **smallest** amount wins — a layer requesting a slow trail (small amount) takes precedence over one requesting a faster decay. This prevents a secondary effect from erasing the trail data a primary effect depends on.

### scale8x8 rounding constant

`scale8x8(v, f1, f2)` in `LayerFunctions.h` computes `(v * f1 * f2 + 32512) / 65025`. The constant `32512 = (255×255−1)/2` gives correct round-half-up behaviour for a denominator of 65025. Do not change it to 32767 — that is only correct for power-of-two denominators.

### Pure helper extraction for native testing

Any pure arithmetic helper that needs host-side unit testing must live in `src/MoonBase/utilities/` in a header with no Arduino/ESP32 includes, then `#include`d from both the production file and the test. `LayerFunctions.h` (containing `scale8x8`, fade helpers, etc.) is the canonical second example of this pattern; the first is `BoardNames.h` (PR #158). Never copy the function body into the test file — copied code drifts and the test verifies a stale snapshot.

---

## Status

| Feature | Status |
|---------|--------|
| Multiple VirtualLayers with isolated buffers | ✅ |
| Per-layer brightness and transitions | ✅ |
| Additive layer compositing | ✅ |
| Layer bounds (startPct / endPct) | ✅ |
| Layout nodes (1D/2D/3D physical positions) | ✅ |
| Effect nodes (virtual coordinate space) | ✅ |
| Modifier nodes (mirror, rotate, projections, …) | 🚧 |
| Graphical node/noodle editor | 🚧 |
| Presets / playlist | 🚧 |
