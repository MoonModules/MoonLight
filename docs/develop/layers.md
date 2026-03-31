# Layers

## Overview

MoonLight uses a two-level layer model: one **PhysicalLayer** owns the hardware channel buffer and all **VirtualLayers**. Effects run on virtual layers in isolation; the results are composited into the physical display buffer once per frame.

```
PhysicalLayer (layerP)
├── lights.channelsD          ← single display buffer, read by drivers
├── lights.header             ← size, nrOfLights, channelsPerLight, …
├── layers[0] VirtualLayer    ← first effect stack
│   ├── virtualChannels       ← per-layer pixel buffer (effects write here)
│   ├── mappingTable[]        ← virtual→physical index lookup (PhysMap)
│   ├── nodes[]               ← effect / modifier nodes
│   └── brightness, startPct/endPct, transitionBrightness, …
├── layers[1] VirtualLayer    ← second effect stack (created on demand)
│   └── …
└── nodes[]                   ← layout / driver nodes (physical layer)
```

---

## Node types

| Type | Runs on | Purpose |
|------|---------|---------|
| **Layout** | PhysicalLayer | Calls `addLight(Coord3D)` to register physical lights and their positions |
| **Effect** | VirtualLayer | Writes pixel colours to `virtualChannels` each frame |
| **Modifier** | VirtualLayer | Transforms virtual coordinates (mirror, rotate, projection, …) |
| **Driver** | PhysicalLayer | Reads `channelsD` and sends to hardware (FastLED, ArtNet, DDP, …) |

- Each node can have user-defined controls (sliders, selects, checkboxes). ✅
- Nodes can be compiled C++ or Live Scripts (`.sc`). ✅

---

## Virtual Layer

Each `VirtualLayer` maintains:

- **`virtualChannels`** — per-layer pixel buffer. Effects write here (indexed by virtual pixel, not physical). Allocated after layout completes (`onLayoutPost()`). Persists across frames so fade-trail effects see the previous frame's data.
- **`mappingTable[]`** — array of `PhysMap` entries (one per virtual pixel). Each entry records which physical index or indices a virtual pixel maps to:
  - `m_zeroLights` — virtual pixel not mapped to any physical light
  - `m_oneLight` — maps to exactly one physical light (most common, stored inline)
  - `m_moreLights` — maps to multiple physical lights (e.g. when a modifier fans out)
- **`size` / `nrOfLights`** — virtual grid dimensions; may differ from physical size when modifiers are active.
- **`oneToOneMapping`** — set `true` when virtual and physical are identical (no mapping table walk needed; fastest path).
- **`brightness`** (0–255) — per-layer output brightness, set by the user.
- **`transitionBrightness`** — animated brightness overlay stepped per frame by `PhysicalLayer::loop()`. Allows smooth fade-in/out independent of the brightness control. `startTransition(target, durationMs)` drives it; it is triggered automatically on first layout (fade-in from 0 when a new effect is activated).
- **`startPct` / `endPct`** — layer bounds as percentages of the full fixture, allowing each layer to target a sub-region.

### Mapping table example

```
virtualChannels index:  0   1   2   3   4   5   …
PhysMap entry:          ∅  [0] [1,2] [3] [4,5,6] …
```

- Virtual pixel 0 → not mapped (stays black in channelsD)
- Virtual pixel 1 → physical light 0
- Virtual pixel 2 → physical lights 1 and 2
- Virtual pixel 4 → physical lights 4, 5 and 6

---

## Per-frame pipeline

```
effectTask (Core 0)                        driverTask (Core 1)
───────────────────                        ──────────────────
PhysicalLayer::loop()
  for each VirtualLayer:
    layer->loop()                          ← reads channelsD (previous frame)
      fadeBy virtualChannels
      run effect nodes → virtualChannels

wait: channelsDFreeSemaphore ─────────────── give after loopDrivers() finishes

compositeLayers():
  memset(channelsD, 0)
  for each VirtualLayer:
    layer->compositeTo(channelsD)          PhysicalLayer::loopDrivers()
      apply brightness                       process pending layout mapping
      apply transitionBrightness             loop driver nodes → channelsD
      additive blend into channelsD
newFrameReady = true
                                           ← picks up newFrameReady, sends to LEDs
                                           give channelsDFreeSemaphore
```

Key points:

- `virtualChannels` and `channelsD` are **separate buffers** — effectTask writes the former while driverTask reads the latter simultaneously. No race condition.
- `channelsDFreeSemaphore` (counting semaphore, max=1) is the handoff: the driver gives it after `loopDrivers()` finishes; the effectTask takes it before `compositeLayers()` writes `channelsD`.
- `swapMutex` protects only the `newFrameReady` / `isPositions` flags — it is **not** held across long operations.
- `compositeLayers()` **zeroes `channelsD` first** so additive layer blending always starts from black. Multiple layers at reduced brightness cross-fade naturally; at full brightness they saturate at 255.

---

## Compositing

`VirtualLayer::compositeTo(channelsD, header)`:

- For colour channels (R, G, B, W): **additive** — `channelsD[i] = saturate(channelsD[i] + scaled)`. Multiple layers blending at 128 brightness produce a natural cross-fade.
- For control channels (pan, tilt, zoom, …): **copy** — last layer wins.
- Effective brightness = `scale8(brightness, transitionBrightness)`.

---

## Layout mapping pipeline

When `requestMapPhysical` or `requestMapVirtual` is set (triggered by a layout or modifier change):

**Pass 1 — physical** (run in driverTask):

1. `onLayoutPre()` — reset counters
2. Layout nodes call `addLight(Coord3D)` — registers physical lights, records positions into `channelsD` for the monitor
3. `onLayoutPost()` — finalises `lights.header` (size, nrOfLights, channelsPerLight)

**Pass 2 — virtual** (run in driverTask, guarded by `swapMutex`):

1. `onLayoutPre()` for each VirtualLayer — reset mapping table entries
2. Layout nodes call `addLight(Coord3D)` again — each VirtualLayer filters by its `startPct/endPct` and builds its `mappingTable`
3. `onLayoutPost()` — allocates / reallocates `virtualChannels` to match the new virtual size; sets `oneToOneMapping` if applicable

Modifiers intercept `addLight()` during pass 2 via `modifyPosition()` / `modifySize()` to remap coordinates.

---

## Physical Layer

- `layerP` is the global singleton (`PhysicalLayer.cpp`).
- Owns `lights.channelsD` — allocated once in `setup()`. On PSRAM boards this lands in PSRAM (large allocation); on ESP32-D0 it is capped at 2048×3 bytes.
- Owns `layers[]` — pre-allocated to 16 slots, created on demand via `ensureLayer(index)`. Layer 0 always exists.
- `activeLayerCount` tracks how many slots are in use (starts at 1).

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
| 3-D virtual coordinate addressing (x + y\*sizeX + z\*sizeX\*sizeY) | 🚧 |
| Presets / playlist (swap part of the nodes model) | 🚧 |

---

## Example nodes JSON

```json
{
  "nodes": [
    {
      "name": "Lissajous 🔥🐙",
      "on": true,
      "controls": [
        { "name": "xFrequency", "type": "slider", "default": 64,  "p": 1065414703, "value": 64  },
        { "name": "fadeRate",   "type": "slider", "default": 128, "p": 1065414704, "value": 128 },
        { "name": "speed",      "type": "slider", "default": 128, "p": 1065414705, "value": 128 }
      ]
    },
    {
      "name": "Random 🔥",
      "on": true,
      "controls": [
        { "name": "speed", "type": "slider", "default": 128, "p": 1065405731, "value": 128 }
      ]
    }
  ]
}
```
