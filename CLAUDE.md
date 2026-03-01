# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MoonLight is a fork of [ESP32-sveltekit](https://github.com/theelims/ESP32-sveltekit) for LED lighting control on ESP32 devices (D0, S3, P4). The repo is split into two layers:

- **MoonBase** (`FT_MOONBASE`) — generic IoT framework (WiFi, MQTT, OTA, IO pins, file manager, tasks). Can run standalone without lights.
- **MoonLight** (`FT_MOONLIGHT`) — lights-specific layer (effects, drivers, layouts, modifiers, physical/virtual layer).

Upstream sveltekit changes are marked `// 🌙` (moonbase additions) or `// 💫` (moonlight additions) in source. Always consider whether a change could be submitted as a PR to upstream.

Documentation: https://moonmodules.org/MoonLight/

## Build Commands

### Backend (PlatformIO)

```bash
# Build for a specific board
pio run -e esp32-d0        # Standard ESP32 (4MB, no OTA) — tight on heap/flash
pio run -e esp32-s3        # ESP32-S3 with PSRAM
pio run -e esp32-p4        # ESP32-P4
pio run -e esp32-d0-moonbase  # MoonBase only (no lights)

# Upload and monitor
pio run -e esp32-d0 -t upload
pio device monitor         # 115200 baud, with esp32_exception_decoder and log2file filters
```

Board environments are defined in `/firmware/*.ini`. The active platform is set in `platformio.ini` (currently `pioarduino-55.03.37`). The `features.ini` toggles feature flags like MQTT, NTP, battery, analytics.

The PlatformIO build automatically runs `scripts/build_interface.py` to build the SvelteKit frontend and embed it.

### Frontend (SvelteKit)

Run these from the `/interface` directory:

```bash
npm run dev      # Dev server with hot reload (proxies API to a running ESP32)
npm run build    # Production build (also triggered by PlatformIO)
npm run check    # TypeScript type checking via svelte-check
npm run lint     # Prettier + ESLint check
npm run format   # Auto-format with Prettier
```

There are no automated tests for this project.

## Architecture

### Dual-Core FreeRTOS Tasks (main.cpp)

MoonLight runs three concurrent tasks:

| Task | Core | Purpose |
|---|---|---|
| `effectTask` | Core 0 | Runs effect nodes → writes to `channelsE` |
| `driverTask` | Core 1 | Reads `channelsD` → sends to LEDs/ArtNet |
| SvelteKit loop | Core 1 | HTTP/WS/UI, module `loop()`/`loop20ms()` |

Double buffering: `channelsE` (effects write) and `channelsD` (drivers read) are swapped atomically under `swapMutex` when a new frame is ready (`newFrameReady`).

### Channel Array

The core data structure generalizes FastLED's `CRGB leds[]`. Each "light" can have `nrOfChannels` channels:
- RGB = 3 channels, RGBW = 4, moving heads = up to 32
- `LightsHeader` (in `PhysicalLayer.h`) defines offsets for each channel type (red, green, blue, white, pan, tilt, zoom, etc.)
- On PSRAM boards, `nrOfLights_t` is `uint32_t`; otherwise `uint16_t`

### Module System (MoonBase)

Every feature is a `Module` — a `StatefulService<ModuleState>` from upstream sveltekit:

- State is stored as `JsonObject` in a single shared `gModulesDoc` (saves RAM)
- Modules register with shared routers: `SharedHttpEndpoint`, `SharedWebSocketServer`, `SharedEventEndpoint`, `SharedFSPersistence`
- The Module UI is **fully generic** — no module-specific Svelte code needed. The frontend reads module state/controls and renders them using components in `/interface/src/lib/components/moonbase/` and `/interface/src/routes/moonbase/module/`
- Module loop hierarchy (called from SvelteKit task): `loop()` → `loop20ms()` → `loop1s()` → `loop10s()`

Key modules:
- `ModuleIO` — pin management via board presets (the only place pins are defined)
- `ModuleEffects` / `ModuleDrivers` — manage effect/driver node lists; extend `NodeManager`
- `ModuleLightsControl` — external API interface (DMX, IR, Home Assistant, etc.)
- `ModuleChannels` — exposes channel data to UI (Monitor)

### Node System (MoonLight)

Nodes are the units of work for effects/drivers/layouts/modifiers. All inherit from `Node` (`src/MoonBase/Nodes.h`):

- **Effects** (`E_*.h`) — write pixel colors to the virtual layer each loop
- **Layouts** (`L_*.h`) — define physical light positions via `addLight(Coord3D)`; implement `onLayout()`
- **Modifiers** (`M_*.h`) — alter positions or sizes; implement `modifySize()`, `modifyPosition()`, `modifyXYZ()`
- **Drivers** (`D_*.h`) — read channels and send to hardware (FastLED, parallel LED driver, ArtNet out, etc.)

Nodes define their own UI controls via `addControl(variable, name, type, ...)`. Controls are stored as `JsonArray` and rendered generically in the UI. `onUpdate()` is called when a control value changes.

Node lifecycle: `constructor()` → `setup()` → `loop()` / `loop20ms()` → `~destructor()`. Nodes must fully clean up their resources on destruction.

**Node coding rules** (from `Nodes.h` guidelines):
- Do not use `String` as class member variables — use `char[n]` or `Char<N>` (String can crash when node is in PSRAM and String in heap)
- No `static` variables in node classes — multiple instances of the same node must not share data

### Physical and Virtual Layers

- `PhysicalLayer` (`src/MoonLight/Layers/PhysicalLayer.h`) — owns the channel arrays, pin assignments, layout mapping, and driver dispatch
- `VirtualLayer` (`src/MoonLight/Layers/VirtualLayer.h`) — maps virtual pixel coordinates to physical channels via `PhysMap` (compact 2- or 4-byte union depending on PSRAM)
- Global singleton: `layerP` (PhysicalLayer containing multiple VirtualLayers)
- `requestMapPhysical` / `requestMapVirtual` flags trigger re-mapping when layouts or modifiers change

### Frontend Structure

- `/interface/src/routes/` — SvelteKit routes mirroring upstream structure plus `/moonbase/` for module/monitor/filemanager pages
- `/interface/src/lib/components/moonbase/` — generic module rendering (`RowRenderer.svelte`, `FieldRenderer.svelte`, `EditRowWidget.svelte`)
- `/interface/src/lib/stores/` — Svelte stores for global state (auth, websocket connection)
- WebSocket communication uses MessagePack (not JSON) by default for efficiency

### Frontend Upstream Boundary

MoonLight periodically merges upstream ESP32-sveltekit changes. To keep merges clean:

- **Minimise changes to upstream files.** Upstream files are everything in `/interface/src/` **except**:
  - `src/routes/moonbase/` — MoonLight-specific routes
  - `src/lib/components/moonbase/` — MoonLight-specific components
  - `src/lib/stores/moonbase_utilities.ts`
  - `src/lib/types/moonbase_models.ts`
  - `vite-plugin-littlefs.ts`
- **Do not reformat upstream files** with Prettier or other tools. The `.prettierignore` in `/interface/` excludes all upstream paths. Running `npm run format` will only touch MoonLight-specific files.
- **ESLint only lints MoonLight-specific files.** The `eslint.config.js` ignores all upstream paths. `npm run lint` checks only `src/routes/moonbase/`, `src/lib/components/moonbase/`, and the moonbase stores/types.
- **For real bugs found in upstream files**, prefer submitting a PR to [theelims/ESP32-sveltekit](https://github.com/theelims/ESP32-sveltekit) rather than fixing locally. Document the issue and PR in `misc/upstream-prs/`.

## Code Conventions

**Critical path** (effect/driver loops) — optimize for minimal overhead:
- Prefer `int` over `unsigned int` when negative values won't occur (avoids range checks)
- Do not add null checks for `const char*` that the developer guarantees are non-null
- Use `#pragma GCC optimize("O3")` selectively for hot functions if needed

**Non-critical** (UI/module logic) — 20ms response time is the target.

**Memory** — On S3/P4 with PSRAM, `operator new` is overridden in `main.cpp` to prefer PSRAM for allocations above a threshold. On ESP32-D0 (no PSRAM), heap is very tight; watch flash usage too (target <3MB firmware).

**Logging** — Use `EXT_LOGD` / `EXT_LOGI` / `EXT_LOGW` / `EXT_LOGE` (not `Serial.printf` directly in module/node code). Tag constants: `ML_TAG` (MoonLight), `MB_TAG` (MoonBase).
