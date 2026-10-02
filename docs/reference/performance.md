# Performance & Memory

MoonLight's per-step **performance contracts** live in the scenario JSONs. Each `test/scenarios/*.json` step carries a per-target `contract` block (`tick_us` ceiling and `free_heap` floor) and an `observed` block (the latest reading per target). The scenarios are the source of truth and the assertion surface: every PR runs against them. [testing.md § Performance contracts](testing.md#performance-contracts-contracttarget) covers the contract semantics and the renegotiation workflow. The headline numbers are in [README.md § Performance](../README.md#performance).

What scenarios cannot carry lives here: structural sizes (`sizeof`), build-variant deltas, and the WiFi and Ethernet physics that explain where a contract lands.

**Render-loop model.** The Layer's buffer **persists** from frame to frame: `Layer::tick()` keeps what the effects drew (the FastLED/WLED/MoonLight convention; see [MoonLight, buffer persistence](../explanation/architecture/moonlight.md#buffer-persistence-the-layer-keeps-what-it-drew)). Fading is one **collected fade**: `Layer::fadeToBlackBy` takes the minimum of the requested amounts and applies one buffer pass per frame, so several fading effects share one pass. That pass is one linear sweep over the buffer, small next to per-light effect compute and the output driver.

---

## Desktop (64-bit)

Desktop ArtNet sends to a non-existent IP so packets complete instantly; `freeHeap` returns 0 (unlimited). Per-step tick budgets live in per-host `contract.desktop-<os>` blocks across the scenarios: `desktop-macos` for macOS arm64, `desktop-windows` for Windows x64, `desktop-linux` for Linux. The `sizeof` and dynamic-memory numbers below apply to all 64-bit desktop targets; tick numbers differ by host CPU and live in the scenario contracts.

### sizeof (desktop, 64-bit)

| Class | sizeof (bytes) |
|-------|---------------|
| MoonModule | 120 |
| Layer | 208 |
| Drivers | 408 |
| GridLayout | 128 |
| SystemModule | 368 |
| NetworkModule | 336 |
| HttpServerModule | 168 |

Most of the 408 bytes of `Drivers` is the per-driver `Correction` stage: a 256-entry brightness LUT and a channel-order table. The 120 bytes of `MoonModule`, which every class inherits, include the rolling-range observed slot, the wired-by-code flag and the per-child `tickChildren` accounting fields.

Binary sizes:

| Target | Size | Build |
|--------|------|-------|
| macOS arm64 | 358 KB | debug-arm64 (release-strip is smaller) |
| Windows x64 | 432 KB | MSVC Release, static CRT |

### Kernel micro-bench (host)

`uv run moondeck/check/bench_kernels.py` runs the `mm_bench` target (Release, best of 5 over a 256×256 sweep of 16.0 fixed coordinates). The figures are host figures. An S3 is 20-40× slower per core, so the ratio between rows is what transfers to a board.

**Value noise against gradient noise** (2026-09-04, macOS arm64, ns per sample). Gradient noise replaces a value-noise kernel only within 1.3× per sample of it, on a board as well as on the host:

| Kernel | value noise | gradient noise | ratio |
|---|---:|---:|---:|
| inoise8 1D | 13.0 | 2.6 | 0.20 |
| inoise8 2D | 22.2 | 5.4 | 0.24 |
| inoise8 3D | 29.3 | 11.5 | 0.39 |
| inoise16 1D | 4.7 | 3.2 | 0.68 |
| inoise16 2D | 9.6 | 7.0 | 0.73 |
| inoise16 3D | 17.8 | 13.3 | 0.75 |
| fbm8 2D, 2 octaves | 22.8 | 11.7 | 0.51 |
| fbm8 2D, 4 octaves | 38.0 | 24.2 | 0.64 |
| fbm16 2D, 2 octaves | 14.8 | 13.5 | 0.91 |
| fbm16 2D, 4 octaves | 22.2 | 27.1 | 1.22 |
| turbulence8 2D, 2 octaves | 18.3 | 12.8 | 0.70 |
| warp8 2D, 1 octave | 30.6 | 21.9 | 0.72 |
| warp8 2D, 2 octaves | 36.5 | 29.8 | 0.82 |

The 8-bit tier gains most (2D is 4× faster on the host): value noise quantizes at every stage, where gradient noise carries its dot products at full width.

**ESP32-S3** (esp32s3-n16r8, 64x64, the four noise effects, tick in µs). Same board and grid; the value-noise column is the previous commit re-flashed:

| Effect (path) | value noise | gradient, first cut | gradient, shipped | ratio |
|---|---:|---:|---:|---:|
| Noise (inoise8 2D) | 4,621 | 7,009 | 5,049 | 1.09 |
| Noise2D (inoise8 3D) | 6,681 | 10,661 | 8,453 | 1.27 |
| Tunnel (fbm8) | 16,385 | 21,304 | 16,649 | 1.02 |
| PolarNoise (warp8) | 20,356 | 29,199 | 20,490 | 1.01 |

The `Noise` and `Noise2D` rows are the 2D and 3D paths of the one `Noise` effect (`Dim::D3`).

The first cut is 2.8× faster on the host and 1.5× slower on the S3. The cause is a runtime arity argument, selects that compile to branches, and eight corners in flight. An out-of-order core hides all three; an in-order core without a branch predictor pays for each. The shipped core is arity-templated, branch-free (a gradient table), 32-bit on the 8-bit tier, and hashes each corner to four bits with one multiply.

The 3D path sits closest to the bound: 3D gradient noise computes eight dot products, and an S3 3D sample takes 1.3× the instructions of value noise. To compare kernel variants before flashing, compile with the target's own compiler (`xtensa-esp32s3-elf-g++ -O2 -S`) and count instructions, branches and stack spills. P4 and S31 rows are open.

**All kernels** (2026-09-04, macOS arm64, ns per sample, best of 5). The value-against-gradient table above includes a `std::function` call per sample. These rows call each kernel directly so it inlines, which roughly halves every figure:

| Kernel | ns/sample | Msamples/s |
|---|---:|---:|
| inoise8 1D | 1.8 | 554.4 |
| inoise8 2D | 4.6 | 218.5 |
| inoise8 3D | 10.1 | 99.5 |
| inoise16 1D | 1.0 | 954.4 |
| inoise16 2D | 2.1 | 481.1 |
| inoise16 3D | 12.5 | 80.1 |
| fbm8 2D, 2 octaves | 10.0 | 100.0 |
| fbm8 2D, 4 octaves | 21.3 | 47.0 |
| fbm16 2D, 2 octaves | 5.2 | 190.7 |
| fbm16 2D, 4 octaves | 11.8 | 84.9 |
| turbulence8 2D, 2 octaves | 10.2 | 98.2 |
| warp8 2D, 1 octave | 16.6 | 60.1 |
| warp8 2D, 2 octaves | 25.5 | 39.2 |
| atan16 | 1.5 | 649.1 |
| dist16 | 8.0 | 125.3 |
| polar address (dist16 + atan16 + kaleido) | 9.9 | 101.3 |

### Fluid solver cost (host)

`scenario_Fluid_solver`, desktop macOS arm64, tick in µs. The solver is Stam's stable fluid: several passes over the grid per frame, plus `iterations` Gauss-Seidel sweeps for the pressure projection that keeps the flow divergence-free.

| Grid | iterations | tick µs |
|---|---:|---:|
| 32×32 | 1 | 20 |
| 32×32 | 5 (default) | 30 |
| 32×32 | 20 | 69 |
| 64×64 | 5 | 133 |
| 20×20×20 cube | 5 | 249 |
| 16×16 | 5 | 7 |

**`iterations` is near-linear**: 1 to 20 is 20 to 69 µs, since each iteration is one more sweep over the grid. **The forcing is free next to the solver**: 2 jets against 4, and persistence 150 against 255, measure 135 µs against 134 µs, inside the noise. The grid and the iteration count are the knobs that matter. **A cube costs depth times one panel**: twenty 20×20 slices cost 249 µs, against 133 µs for one 64×64 panel with half the lights. Each slice pays its own boundary and projection.

P4 and S3 rows are open.

### Memory at 128×128 with mirror

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 92 KB | 12 KB buffer + 80 KB LUT (uint32_t indices on 64-bit) |
| Drivers | 48 KB | output buffer (128×128×3) |

---

## ESP32: Olimex Gateway Rev G (internal RAM only, 320 KB)

Per-step tick and heap live in `contract.esp32-eth-wifi` and `contract.esp32-eth` across the scenarios, and the [README perf table](../README.md#performance) holds the headline grid×board matrix.

### Run-to-run variance

Individual measurements vary ~5–10% on the Olimex board with an unchanged configuration. The cause is ESP32 and Ethernet timing jitter: lwIP `tcpip_thread` scheduling, EMAC DMA and Ethernet ACK pacing. Scenarios use a 10% default ESP32 tolerance to absorb it, and a step that trips is re-run before it counts as a regression. The same holds for the `collect_kpi.py --commit` gate, which parses a single `tick:` line from `esp32/monitor.log`.

### What the per-effect sweep established

A 2026-06 Olimex sweep ran every effect alone over a Layer at four square grids, through the real ArtNet and Preview drivers. Per-scenario ticks are generated into [repo-health](metrics/repo-health.md#render-performance), and the light/heavy bracket of `scenario_perf_full` carries the per-effect curve. Three findings:

**At 128² the board is bound by ArtNet output.** The ~38 ms synchronous send dominates the tick, so nearly every effect lands at 12-23 FPS. Below 64² the compute shows, with Rings, Noise and Spiral heaviest and Lines and Checkerboard lightest.

**Free internal heap falls as the grid grows**, because the Layer buffer, the LUT and the driver output buffer all live in internal RAM on a no-PSRAM board.

**Three effects carry per-cell state**, and they set the fragmentation headroom at large grids. Particles and GameOfLife allocate a parallel grid-sized array (77 KB free / 34 KB largest block, and 90 KB / 46 KB at 128²), and Fire a heat map (110 KB). Every other effect holds ~126 KB free with a ~62 KB largest block. The largest free block is the number to watch: free heap can be ample while no single block fits the next allocation.

### ArtNet over WiFi vs Ethernet

| | Ethernet | WiFi STA |
|--|----------|----------|
| ArtNet (97 UDP packets) | ~27,000 µs | ~110,000 µs |
| Total tick | ~50,000 µs / 20 FPS | ~130,000 µs / 7 FPS |

WiFi `sendto()` takes ~1,140 µs per packet against Ethernet's ~280 µs, from CSMA/CA backoff, rate adaptation and link-layer retries. For ArtNet at 16K lights, use Ethernet.

### Build variant: the Ethernet buffer pool speeds up ArtNet

Same source tree, same MCU (ESP32 classic, 160 MHz):

| Board / firmware | 128×128 tick | ArtNet send |
|---|---|---|
| Olimex Gateway, WiFi-only build | 220 ms (4 FPS) | 155 ms |
| Olimex Gateway, default `esp32` (WiFi + Ethernet) | 85–95 ms (10–12 FPS) | 38 ms |

The default `esp32` build carries both the WiFi and Ethernet stacks. `sdkconfig.defaults.eth` enlarges the shared lwIP/WiFi buffer pool through `CONFIG_ETH_DMA_*`, which roughly quadruples ArtNet throughput against a WiFi-only pool. Generic ESP32 boards (no PCB-trace antenna, less stable 3V3) vary widely in WiFi TX quality against the Olimex.

### Memory at 128×128 with mirror

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 52 KB | 12 KB buffer + 40 KB LUT (uint16_t indices on ESP32) |
| Drivers | 48 KB | output buffer (128×128×3) |
| System + Network | 0 | char buffers in class, no heap |

LUT is half desktop size (uint16_t vs uint32_t per entry). The 1:1 (no-modifier) path skips the LUT entirely, which `unit_Layer_sparse_mapping` pins as the identity path against a serpentine grid that does build one.

### Heap breakdown (128×128, mirror, RainbowEffect, Ethernet + mDNS)

| Component | Bytes | Notes |
|-----------|-------|-------|
| Boot heap | 290,240 | Before any init |
| After Ethernet + mDNS init | ~240,000 | lwIP + Ethernet + mDNS driver |
| Layer buffer | 12,288 | 64×64×3 (logical, halved by mirror) |
| Mapping LUT | 40,962 | offsets + destinations (uint16_t) |
| Driver output buffer | 49,152 | 128×128×3 (physical) |
| Preview frame | 0 | Zero-copy: pointer to output buffer |
| HTTP + WebSocket | ~8,000 | server + kernel buffers |
| MoonModule instances | ~3,000 | all modules combined |
| **Free heap (running)** | **~104,000** | stable, no leaks |

---

## ESP32-S3 N16R8 Dev (16 MB flash, 8 MB octal PSRAM)

`esp32s3-n16r8` firmware at `Network.txPowerSetting=8` dBm (the brown-out cap `deviceModels.json` sets), 128×128 grid, Mirror XY, ArtNet over WiFi STA. The grid sweep of `scenario_perf_full` runs it against the live device, and per-step tick and heap live in `observed.esp32s3-n16r8` across the scenarios. The table is the 128×128 step.

| Metric | Value | Notes |
|---|---|---|
| Total tick | ~164 ms / 6 FPS | Dominated by ArtNet at the 8 dBm cap |
| ArtNetSend | ~93 ms (97 UDP packets) | ~960 µs/packet, against 38 ms in total on the Olimex `esp32-eth-wifi`; the 8 dBm cap is the cause (below) |
| Free internal RAM | ~240 KB | The comparable, scarce resource, and the S3 number in the README perf table. Flat (~238–240 KB) across all grid sizes, since the Layer buffer and LUT live in PSRAM. |
| Free heap (incl. PSRAM) | ~8,163 KB | The PSRAM-merged total (`totalHeap` reports 8 MB combined). Internal RAM is the limit, so this total says little. |
| maxBlock (internal) | ~164 KB | Internal-RAM largest contiguous block, the scarce-resource KPI. `maxAllocBlock` (any memory) reports ~8 MB on PSRAM boards, so SystemModule and scenario_runner use `maxInternalAllocBlock`. |
| Layer buffer | 92 KB | In PSRAM (auto by heap_caps preference) |
| Image | 1,307 KB | ~30% larger than `esp32-eth-wifi` due to USB-Serial-JTAG driver + Improv-dual-transport listener |

Per-grid-size FPS from the same sweep: 16×16 → 1672, 32×32 → 287, 64×64 → 25, 128×128 → 6. The steep drop is ArtNet-bound: packet count scales with the pixel count, and at 8 dBm each packet is ~3× slower on-air than the Olimex Ethernet path.

### Why ArtNet is slower at 8 dBm

The brown-out cap sets TX power 12 dB below default (8 dBm against ~20 dBm). At lower TX power, WiFi rate adaptation picks a slower MCS rate to keep link margin, so each packet spends longer on air. ~960 µs/packet × 97 packets = the ~93 ms ArtNet budget. The cap is the price of a stable association on this hardware: without it the radio browns out and sends nothing.

**Use Ethernet-capable boards for high-FPS ArtNet workloads.** The ESP32-S3 N16R8 Dev fits the "lots of PSRAM, accept the WiFi compromise" niche: large pixel buffers or feature-heavy effects beyond 320 KB of internal RAM.

### Memory at 128×128 with mirror

| Module | dynamicBytes | Notes |
|--------|-------------|-------|
| Layer | 92 KB | Buffer in PSRAM (12 KB internal on the Olimex), with a uint32_t LUT where the Olimex uses uint16_t |
| Drivers | 48 KB | Output buffer (128×128×3) |
| Free internal | ~240 KB free, ~164 KB largest block | Plenty of headroom for WiFi + lwIP + Improv-on-both-transports |

### What the render-only sweep established

A 2026-06 render-only sweep on the S3 measured raw effect cost with the output driver, audio and discovery off, so the tick is Layout, Layer and effect alone. The light/heavy bracket of `scenario_perf_full` measures that curve on-device. Two findings:

**Effect compute stays visible up to 16K pixels**, unlike on the Olimex, because the S3 holds the Layer buffer in PSRAM. The cheapest effects (Lines, Checkerboard, PlasmaPalette) clear ~100 FPS at 16K. The heaviest is Noise at 51 ms (about 19 FPS, one noise sample per pixel), then Rings and GlowParticles.

**Memory stays flat on a PSRAM board.** Free heap (PSRAM included) holds ~8.54 MB at small grids and ~8.46-8.49 MB at 16K; the difference is the grid-sized render buffer. It returns to ~8.54 MB whenever the grid shrinks, so the sweep shows no leak and no fragmentation creep. The largest free block stays ~90-110 KB throughout.

### MoonLive (scripted effect): tick and memory

A `MoonLiveEffect` compiles its script to native code for the board's ISA once, in `prepare`, and `run()` is one function-pointer call per tick. Measured on the S3 at 16×16:

| Script | Tick (µs) | Exec block (heap) |
|--------|----:|----:|
| `setRGB(5, 255, 0, 0)` (one pixel) | 26 | ~52 B |
| `setRGB(random16(256), 0, 255, 0)` (one host call) | 29 | ~140 B |
| `fill(0, 0, 255)` (loop over all lights) | 47 | ~68 B |

The rows above are a dated S3 bench record; the numbers below them are what a desktop run measures today. The tick cost is native-code speed: a `setRGB` is a bounds-guard + three byte stores (~26 µs including the per-tick module overhead), `fill` adds the per-light loop. The **exec block scales with the program**, not a fixed cap: a one-liner is tens of bytes of machine code, since `place()` allocates the emitted length, word-rounded. The module reports it as its dynamic memory (`setDynamicBytes(engine_.heapBytes())`, the exec block plus the control arena), so it shows on the UI card. What the rest costs is in the table below; [livescripts-analysis-top-down.md § 3.7](../work/future/livescripts-analysis-top-down.md) covers how it scales as the language grows.

**What MoonLive costs** (the `esp32s3-n16r8` image, 2026-10-02, from the link map and the symbol table):

| Part | Flash | RAM |
|------|------:|-----|
| Compiler and parser | 30.4 KB | |
| Xtensa assembler | 14.3 KB | |
| Register spiller | 3.9 KB | |
| Runtime | 1.6 KB | |
| Builtins and effect wrappers, compiled into other files | ~21 KB | |
| **All of MoonLive** | **~71 KB, about 3% of the 2.19 MB image** | under 200 B static |
| One loaded script's code | | tens of bytes for a one-liner, at most 16 KB, in executable internal RAM |
| One loaded script's state | | a 116 B arena and ~48 B of engine; the arena holds 64 B of members and arrays, the system variables and the call arguments |
| Compiling a script | | a few KB of staging and IR on the heap, freed once the code is placed |

A function's local variables live in registers and on the render task's stack while it runs, so they cost no RAM at rest.

**System variables cost a byte store each, per binding.** They are arena slots the binding refreshes before `run()`, with a null check and a byte store each. An **effect** writes three (`width`/`height`/`depth`) once per tick. A **modifier** writes six (those plus the `x`/`y`/`z` it is handed) on the mapping-build cold path. A **layout** writes none, since it is given no dimensions.

`t` costs no arena byte: it is an argument register the host already passes. A callee may clobber an argument register under the ABI, so a backend saves `t` across calls, and the arm64 backend stacks x3 with the vreg pool. `unit_moonlive_fill` pins that a script reading `t` after a call sees the host's value. Resolving system variables (a table checked before locals and controls) is compile-path work, once per `source` edit.

**Across the four board classes** (2026-08-17, 16×16): the same shipped scripts on every ISA, so the numbers compare directly.

| Script | S3 (Xtensa) | classic (Xtensa) | P4 (RISC-V) | S31 (RISC-V) | Exec block |
|---|---:|---:|---:|---:|---:|
| `lines.mlv` (two `line()` calls) | 76 µs | 128 µs | 30 µs | 92 µs | 2292 B |
| `plasma.mlv` (9 host calls per cell) | 780 µs | 1038 µs | 577 µs | 725 µs | ~1124 B |
| `ripples.mlv` (~15 host calls per cell) | 1695 µs | 2265 µs | 1098 µs | 1377 µs | 2372 B |

The exec block is the emitted machine code, so its size varies by ISA. `lines.mlv` is the cheapest of the three because `line()` moves the per-cell loop out of emitted code into the shared `draw::line`. That is the argument for adding power functions as builtins rather than writing them in script.

**A script calling its own functions** pays for a recursion guard: nine instructions in a function's prologue, emitted only when the program contains a `CallScript`. `crosshair.mlv` (three functions, two calls per frame) ticks at 219 µs on the classic against 204 µs without the guard, roughly 5-10%. Unbounded recursion degrades instead of resetting: the classic ran a non-terminating script for 110 s at 109 fps.

**Two cost models** (2026-08-22, shiffy's 80x48 = 3,840 lights). A shader is a function of position and time, so it pays per light. A particle script pays per object, with the per-particle work inside one C++ loop per call.

| Script | Shape | shiffy 80x48 | desktop 128x96 |
|---|---|---:|---:|
| `plasma.mle` | 9 host calls per cell | 16,031 us | |
| `metal.mle` | ~14 per cell, 3 square roots | 59,600 us | 1,557 us |
| `fountain.mle` | ~9 per FRAME, 300 particles | 1,093 us | 9 us |
| `ballpit.mle` | as above plus `collide` over 64 | 5,127 us | |

`metal.mle` against `fountain.mle` is 54× on the same fixture. `polarR` makes the shader expensive: it wraps a square root measured at ~3.5 µs per pixel, and `metal` calls it three times per pixel. `collide` is the one call here that grows faster than the pool (N-body): 0.1 µs without it, 3.2 µs at 48 particles and 53.6 µs at 200, on the host.

**`FileManagerModule` reads filesystem usage once a minute.** `esp_littlefs_info` walks every block of the partition (~80 ms on an S3) inline on the render thread. At 1 Hz it drops shiffy from ~85 to ~77 fps on average, with frame deltas per second of `83 78 80 79 82 66 72` against `83 85 85 83 84 87 85`. Particles show such a stall most, because a particle integrates the gap into its trajectory: one frame after an 80 ms gap moves every particle 6.7× its usual distance.

**The compile-time staging buffer is sized from the script's tokens**: 48 bytes per token plus a 256-byte floor, freed when the compile returns. It is allocated before a byte is emitted, so the constant is the worst case. Across every shipped script on all three backends, the densest is `random-pixel.mlv` at 28.5 B/token on RISC-V, a ~1.7× margin. It is one statement of four nested `random16()` calls, each saving the whole register pool. Density falls as a script grows, so a short call-dense script sets the bound. `gradient.mlv` is 5.9 B/token, and the longest shipped script 15.3. A per-ISA test pins that every shipped script emits under two-thirds of its budget.

**Local function calls cost 2,732 bytes of flash** on the classic (+0.16% of the image), about 20 bytes per line of source. Nearly all of it is emitter code instantiated once per backend, so one line of shared lowering becomes three copies in the image.

**The compile path's deepest frame is `lowerWith`, at 1,120 bytes on the classic**, sized by `kAsmLabels`/`kAsmFixups` at 48/96 (a class allocates one label per function). The nested chain is 144 + 288 + 576 + 1120 = 2,128 bytes, 17% of the 12 KB main task. It is compile-path stack, paid once per compile on the render task.

---

## Multi-pin LED driving (all three peripherals, 128×128 grid)

The rows name the `ParallelLedDriver` backends by the class names they were measured under: `MultiPinLedDriver` is the `i80` peripheral, `MoonLedDriver` is `MoonI80`, and `ParlioLedDriver` is `Parlio`. The `peripheral` control selects the backend at one vtable dispatch per frame, so the timings hold.

**Async double-buffering applies to `i80` and `Parlio`.** Both route through a transaction queue (esp_lcd or the Parlio driver) that absorbs the second in-flight transfer. In the Parlio row below, that hides the ~7.5 ms wire behind background DMA. `MoonI80` is single-buffer (`supportsDoubleBuffer()` is false and the control is hidden): its own-GDMA two-buffer handshake races and wedges the bus. Its whole-frame path is encode, transmit, wait, serially per frame, and its scale path is the streaming ring.

Each parallel LED driver ran on real hardware at a 128×128 = 16384-light grid with 8 lanes (2026-07-12). A row with its own lane count and date overrides that. The **GPIOs** are recorded because they seed each board's usable-pin map: the `deviceModels.json` pin defaults come from proven sets. Every listed pin drove WS2812 output on its board without conflict.

| Peripheral | Board | Pins used (8 lanes) | Result | Ceiling / bound |
|---|---|---|---|---|
| **Parlio** | ESP32-P4 (Waveshare P4-NANO) | `20,21,22,23,24,25,26,27` | `Drivers` tick ~30100 µs, fps 30 at 16384 lights (8 lanes, SWAR transpose). | Parlio's single-shot transfer caps at 65535 bytes in total, and a light costs `channels × 24 × slotBytes`. The ceiling is **897 lights/lane at 8 lanes RGB** and 673 RGBW. It halves to 442/332 at 16 lanes, since a 16-bit bus doubles `slotBytes`. Over that, the driver reports `too many lights per pin` and keeps running. The [chunked-DMA work](../work/future/backlog-light.md) lifts the ceiling (tier 1 → ~16-21K). |
| **LCD_CAM i80** (MultiPinLedDriver) | ESP32-S3 N16R8 Dev | data `18,5,6,7,8,9,10,11` · WR(clock) `12` · DC `13` | Same encoder, healthy on real i80. Encode scales ~6 µs/light (8×512 = 4096 → 23 ms; 8×1024 = 8192 → 50 ms). | **Single-DMA init ceiling 8192–12288 lights**: 8×1024 initializes, and 8×1536 fails with `LCD-IDF: bus init failed, check pins / memory`. |
| **RMT** | classic ESP32 (LOLIN D32 / WROOM) | `2,4,13,14,16,17,18,19` (pin 2 = a real 24-LED strand) | 8-pin RMT drives **8×256 = 2048 lights** at a ~12.6 ms tick. It scales to ~8192 before the tick plateaus. All lanes are healthy, and the pin-2 strand is verified lit. | **Silent alloc failure:** the RMT symbol buffer is sized for the driver's `count` window. `count=0` on a 16384 grid needs ~1.5 MB, which fails on the ~90 KB heap, and `tick()` returns with **no status** (LEDs dark). Bound the driver with the start/count window; a status for this is [backlogged](../work/future/backlog-light.md). |
| **I2S i80** | classic ESP32 (ESP32-WROVER) | data `2,4,13,14,18,19,21,22` · WR(clock) `32` · DC `33` (pin 2 = a real strand, verified lit) | The **same** `MultiPinLedDriver`, over the **I2S peripheral in i80 mode**. IDF routes the i80 API to I2S on the classic and to LCD_CAM on the S3 and P4. 8-lane doubling sweep, 128×128 grid, 2026-07-13. 64/pin (512) → 4877 µs, 128/pin (1024) → 8575 µs, 256/pin (2048) → 15638 µs. It scales linearly at **~7.6 µs/light**, against ~6 µs on the S3's LCD_CAM. `frameTime` reports the WS2812 wire floor (512 → 243 fps, 2048 → 67 fps). **16 lanes work on classic too** (16×256 = 4096 verified), but the WROVER exposes only ~13 non-strap pins, so 8 lanes is the practical set. | **Internal-RAM ceiling: 2048 lights at 8 lanes (4096 at 16).** The classic I2S backend **cannot DMA from PSRAM**: `esp_lcd_i80_alloc_draw_buffer` rejects `MALLOC_CAP_SPIRAM` ("external memory is not supported"). Its frame buffer is internal DMA RAM only (`maxBlock` ≈ 76 KB). At 8 lanes, 512/pin (4096) and above fail with `I2S-IDF: bus init failed, check pins / memory`, a **clean degrade** with uptime climbing through every rung. **The render is decoupled from this ceiling:** the same sweep rendered the full 16384-light grid at every rung (`Layer` ≈ 511 ms/frame, from PSRAM) while the output was capped. At 16K lights the render (511 ms) dwarfs the output (24 ms), so the render is the wall on this chip and multicore cannot help. |
| **LCD_CAM 16-lane** | ESP32-S3 (SE 16 V1 + LightCrafter 16, n8r8) | SE16 data `47,48,21,38,14,39,13,40,12,41,11,42,10,2,3,1` · WR/DC `5`/`6`; LC16 data `47,21,14,9,8,16,15,7,1,2,42,41,40,39,38,48` · WR/DC ghost `33`/`34` | **Reaches the full 16384 lights, where Parlio caps at 4096.** SE16 16-lane doubling sweep, 128×128 grid, `doubleBuffer` ON, 2026-07-13. 512 → 1843 µs, 1024 → 3422 µs, 2048 → 6612 µs, 4096 → 15153 µs, 8192 → 26788 µs, **16384 → 49916 µs (~20 fps)**. The driver tick is the encode alone; the WS2812 wire overlaps in background DMA, and `frameTime` reports it (16384 → 28786 µs). With `doubleBuffer` OFF: 4096 → 22518 µs and 16384 → 77732 µs, so async is ~30–56% faster. | **PSRAM-backed, so the contiguous-block limit that caps Parlio stays out of the way.** LCD_CAM allocates its DMA buffer through `esp_lcd_i80_alloc_draw_buffer` **from PSRAM**, clear of the ~368 KB largest internal block. **At 16K the encode is the wall.** The 28,786 µs wire alone allows ~35 fps, so the tick is the 49,916 µs encode at ~20 fps. Inline on **core 0**, that encode **starves the LC16's W5500 SPI-Ethernet**, which the [multicore pipeline](#multicore-the-whole-output-stage-on-core-1-multicore-step-2) resolves. |
| **Parlio 16-lane** | ESP32-P4 (testbench, n16r8) | 16 data pins `21,20,22,23,24,25,26,27,32,33,39,40,41,42,43,44` | 16-lane doubling sweep (2026-07-12), reproduced within 0.3% on a second P4. `ledsPerPin` runs 32 to 256 on a 128×128 grid. The tick scales **linearly**: 512 → 1653 µs, 1024 → 2925 µs, 2048 → 5514 µs, **4096 → 10760 µs**. With `doubleBuffer` ON (2026-07-13), the ~7.5 ms wire wait moves into background DMA. The driver tick at 256/pin is **3,790 µs against 10,820 µs (92 driver-fps) OFF**. The board runs **76 fps against 48**, with a system tick of 13.0 against 20.6 ms. `frameTime` reports the measured wire floor: **7474 µs (133 fps max)**. The tick is effect render (~7.3 ms) plus driver (~3.8 ms) in series, which the [multicore pipeline](#multicore-the-whole-output-stage-on-core-1-multicore-step-2) overlaps toward that ceiling. OFF saves one frame of latency, below the perceptual A/V-sync threshold, so ON suits every use, audio-reactive included. | **Single-DMA ceiling ≈ 4096 lights (256/pin).** 512/pin (8192) fails with `Parlio init failed, check pins / memory`. The P4 has 33 MB free heap, but its largest contiguous internal block is ~368 KB, and the 16-bit single-shot DMA buffer needs one contiguous block. It is a **contiguous-block limit rather than total memory**, and it bites before the 65535-byte cap. The full 16384 (1024/pin) needs the [Parlio chunked-transfer](../work/future/backlog-light.md) work, which splits the frame across DMA bursts. |

**LOLIN D32 (classic ESP32-WROOM) usable LED GPIOs:** `4,13,14,18,19,21,22,23,25,26,27,32,33` plus `16,17` (free on WROOM; they are the PSRAM bus on WROVER). Avoid straps `0,2,12,15`, the onboard LED on `5`, and battery-sense on `35`; input-only `34–39` can't drive an LED. (Chip-level set: [gpio-usage.md](hardware/gpio-usage.md).)

**Diagnostic used:** RMT `tickTimeUs > 1000` = actively encoding (LEDs on); a tiny ~30 µs tick = the symbol alloc failed and `tick()` bailed (dark). `dynamicBytes` for RMT is the frame buffer (`driverHeapBytes()` returns `frameCap_`: outChannels bytes per light).

**Acceptance floors**, each to be cleared on real hardware: RMT **8×256 = 2048** (verified above) and parallel-I2S (classic i80) **16×256 = 4096** (verified 2026-07-13). The virtual (shift-register) driver's floor is **48×256 = 12288**.

## Panel cards over raw Ethernet (`PanelCardDriver`, ESP32-S31)

Measured on an S31 driving two 128x64 HUB75 panels through a ColorLight 5A-75 receiver card over a gigabit RGMII link, 2026-07-31.

| | us/tick | note |
|---|---:|---|
| **PanelCardDriver** | **~2 500** | 16 384 lights: correction + 132 frames handed to the MAC |
| PreviewDriver | 5 687 | the browser preview, same buffer |
| a heavy effect (GameOfLife) | 17 592 | the render, and the largest single cost |

The panel driver is the cheapest active module despite pushing 132 packets per frame (2 brightness + 128 rows + 2 sync, at 497 pixels per row packet). The render sets the wall's frame rate: a heavy effect at 17.6 ms dominates a 26 ms tick, giving ~32 FPS. A lighter effect mix measures ~56 FPS on the same wall.

**Packets set the ceiling.** Each frame is sent synchronously from `tick()`, so cost grows with rows: a 256-row wall doubles the packet count. The card's 1 Gbit requirement is about wire time rather than bandwidth. At 100 Mbit the same bytes take ten times as long and overrun the inter-frame window the sync depends on ([drivers.md](../moonmodules/light/drivers.md#panelcard)).

**The DMA buffer size keeps it stable.** The firmware sets `CONFIG_ETH_DMA_BUFFER_SIZE` to 1536 B, one descriptor per 1512 B frame, plus `CONFIG_ETH_TRANSMIT_MUTEX`. At the 512 B default a frame spans three descriptors, so a 10-descriptor ring holds ~3.3 frames against 132 fired back-to-back. At that depth the S31 refuses ~19 000 frames and wedges twice in 20 minutes. Ring count is not the lever: 30 descriptors run no cleaner than 10. The cost is ~20 KB of internal DMA RAM, since the size applies to both rings ([lessons.md](../work/past/lessons.md)).

**Static RAM: 0 B.** The driver's 1 512 B packet buffer is a class member, allocated only when the driver is added. `check_footprint --module PanelCardDriver --firmware esp32s31` reports ~3 500 B of flash.

The numbers are a bench record rather than a scenario contract, since the driver needs a receiver card on the wire.

## HTTP cost of the P4's WiFi co-processor (`esp32p4rev1-eth-wifi`)

The P4's WiFi comes from an on-board ESP32-C6 over SDIO. Compiling that path in costs HTTP throughput **on Ethernet**, an interface it does not carry. Both images are measured over Ethernet on the same board, commit and cable, so the only variable is esp_hosted in the binary.

| build | per-request (`/api/system`) | throughput (76 KB `app.js`) |
|---|---:|---:|
| `esp32p4rev1-eth` | 10 ms flat | 1,973 KB/s |
| `esp32p4rev1-eth-wifi` | 40 ms typical, one 280 ms outlier in 12 | ~980 KB/s |

Compiling the co-processor in costs roughly **4× per request and 2× on throughput**. Render holds at 359 fps on the WiFi build, so the frame loop is unaffected. The cost is per request rather than per byte, which points at a periodic blocker a request waits out rather than a slow pipe.

Measured on IDF v6.1-rc1; the remaining penalty is tracked in [backlog-core](../work/future/backlog-core.md).

## Multicore: the whole output stage on core 1 (`multicore`, Step 2)

The `multicore` control on the Drivers container runs **every driver's per-frame work** on a **core-1 task**, while core 0 renders the next frame. That work is the LED encode, the ArtNet packet build and the preview frame build. A frame costs `max(render, output)` instead of `render + output`. It stacks with `doubleBuffer`: that hides the WS2812 wire behind DMA, and `multicore` hides the encode behind the render.

**Measured live by flipping the switch** (SE16, 64×64 grid, 16 lanes × 256 = 4096 lights; reproduced ON→OFF→ON):

| | `multicore` ON | `multicore` OFF |
|---|---|---|
| whole-board fps | **46** | 32 |
| system tick | 21,316 µs | 30,857 µs |
| **`Drivers` cost on core 0** | **2,261 µs** | 15,404 µs |
| Preview | 91 µs | 49 µs |
| HttpServer | 409 µs | 348 µs |

**85% of the output stage leaves the render core** (15,404 → 2,261 µs), for **+44% fps**. The encode itself stays ~20 ms at 4096 lights on the S3, running on core 1 in parallel with the render.

**Calling the network stack from core 1 costs ~100 µs per frame.** lwIP is pinned to core 0 (`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0`), so a driver writing a socket still hands its bytes to the network task there. Only the CPU half, the packet and frame building, moves. The cross-core lock and cache bouncing show as Preview 49 → 91 µs and HttpServer 348 → 409 µs. Against ~13,000 µs of output work removed from core 0 that is a 130:1 trade, so **every** driver moves when the split is on.

**The preview is a pull channel, which bounds those Preview numbers from above.** `PreviewDriver::tick` on core 1 only gathers and arms a message. Every socket byte moves on the transport's tick20ms on core 0, paced by TCP. With no standing client request the preview costs **zero**: the tick returns before any work. A congested link stays off the render path, and `renderWait` holds at its normal few ms. Four boards held ~12 minutes of continuous viewing with zero socket closes.

**It also removes a contention.** A ~19 ms inline encode on core 0 starves the network stack sharing that core. The LightCrafter 16's W5500 Ethernet drops its link, and HTTP times out while the render loop keeps ticking. With the encode on core 1, an HTTP hammer during a heavy 8192-light encode holds: 77 requests, median 163 ms, one timeout.

**Per-chip `renderWait`**, the read-only KPI reporting the worst core-0 wait at the frame boundary in the last second. It measures how much idle time a second handoff buffer (a ping-pong buffer) would recover:

| Board | Backend | `renderWait`, heaviest effect (Noise @ 128²) |
|---|---|---|
| SE16 (S3) | LCD_CAM i80 | ~1 µs |
| P4 | Parlio | ~7 µs |
| WROVER | classic I2S | ~15 µs |

Under a heavy effect the render dominates on every board, so core 0 never idles and the ping-pong buffer would buy nothing. It only pays when the effect is much cheaper than the output work (a light effect driving many lights), where the wait grows to milliseconds.

**Memory and degrade.** The split needs one frame buffer: the stable frame core 1 reads while core 0 renders the next. With two or more layers compositing, or one layer through a LUT map, that buffer already exists; the identity single-layer case allocates it. When it does not fit, every driver ticks inline on core 0 and `doubleBuffer`'s DMA overlap still applies. The switch stays on, and the split engages by itself once the memory is there.

## Incremental cost analysis (`scenario_perf_light` / `scenario_perf_full`)

The two scenarios start from a clean canvas and add one subsystem at a time, so each step's tick and heap delta isolates one module's cost. Measured live on three boards (2026-06-17, render-only, audio and discovery off):

- **classic**: Olimex Gateway, ESP32 @240MHz, **no PSRAM** (320KB internal), `nrOfLightsType`=uint16
- **S3**: ESP32-S3 N16R8 @240MHz, 8MB PSRAM, uint32
- **P4**: Waveshare P4-NANO, ESP32-P4 @400MHz dual-core, 32MB PSRAM, uint32

All figures tick µs at 16² unless a grid is named; ~5–10% run-to-run variance, so small (<~30µs) deltas are near the noise floor.

### Per-subsystem cost (added one at a time, 16² grid)

Absolute tick at each step (the diff vs the prior row is that subsystem's cost):

| Step | classic | S3 | P4 | Reading |
|---|--:|--:|--:|---|
| Render floor (Grid+Layer+Checkerboard) | 129 | 133 | 67 | the baseline; P4 ~2× faster |
| − Audio disabled | 116 | 111 | n/a | **audio ≈ +13–22µs/tick** (fixed I2S block-read; no mic on the P4) |
| − Devices discovery disabled | 116 | 112 | 56 | idle discovery is free (boot sweep is one-shot) |
| + MultiplyModifier (2×2) | 315 | 292 | 96 | **+180–200µs**: the per-frame blend+map over the LUT |
| + PreviewDriver | 115 | 118 | 56 | apparatus; free |
| + NetworkSendDriver | 139 | 141 | 67 | ArtNet/DDP build+send; cheap at this size |
| + RmtLedDriver (64 LEDs) | 152 | 120 | 56 | per-frame encode+transmit at a fixed 64-LED output |
| + MultiPinLedDriver (64 LEDs) | ✓³ | 142 | 57² | i80 bus: classic ESP32 → I2S, S3/P4 → LCD_CAM |
| + ParlioLedDriver (64 LEDs) | n/a¹ | n/a | 58 | P4 Parlio |

¹ Parlio is P4-only, so the classic step is skipped. ² The P4 has LCD_CAM too, but Parlio is its scale path. ³ `MultiPinLedDriver` runs on the classic over the I2S i80 backend; its measurements are in [§ Multi-pin LED driving](#multi-pin-led-driving-all-three-peripherals-128128-grid). "n/a" means the driver is absent on that chip.

Audio is a small fixed per-tick cost, idle discovery is free, and output drivers are cheap at a capped 64-LED output. The modifier's +~190 µs at 16² is the one notable per-frame add: the blend and map, which pays for itself at large grids (below).

**Multi-layer composition** (the `Drivers` composite loop): a single enabled Layer is the pass-through fast path, where the driver reads the Layer's buffer directly at zero composite cost. Each additional enabled Layer adds one `blendMap` pass over the physical buffer (integer alpha-over or additive, resolved once per layer). N enabled layers cost about N times the per-layer write. A RegionModifier costs only where present.

**Re-verified 2026-06-25** on all three boards with multi-layer composition and RegionModifier in the build: the numbers hold within run-to-run variance.

### Effect compute: light and heavy bracket across grid sizes (render-only)

Tick µs; FPS in parens for the 16K row:

| Grid (pixels) | classic | S3 | P4 |
|---|--:|--:|--:|
| **Checkerboard (light)** | | | |
| 16² (256) | 149 | 119 | 61 |
| 32² (1K) | 357 | 328 | 133 |
| 64² (4K) | 1,147 | 1,090 | 452 |
| 128² (16K) | 4,360 | 7,949 | 1,940 |
| **Noise (heavy)** | | | |
| 16² (256) | 1,010 | 799 | 313 |
| 32² (1K) | 3,203 | 2,831 | 1,120 |
| 64² (4K) | 13,547 | 11,235 | 4,358 |
| 128² (16K) | 62,316 (16 FPS) | 50,555 (20 FPS) | 17,433 (57 FPS) |

All curves scale **about linearly** in pixel count, which rules out a realloc or fragmentation pathology. The heavy effect is the 16K bottleneck on every board. On heavy compute the ranking is P4 ≫ S3 > classic, the P4's 400 MHz dual core being ~3× the S3. At light-16K the **classic (4,360 µs) beats the S3 (7,949 µs)**. The S3's PSRAM buffer has higher access latency than the classic's internal RAM, and the classic's uint16 LUT is half the size. On the heavy effect compute dominates and the S3 leads.

### MultiplyModifier: compute down, memory up (Noise effect)

With the default 2×2 kaleidoscope the modifier makes the logical grid ¼ size, so the effect computes on fewer pixels. The modifier **reduces** the tick at large grids, and its cost is the 1:N mapping-LUT **memory**.

Tick µs, Noise alone vs Noise+Multiply:

| Grid (physical) | classic alone | classic +Mult | S3 alone | S3 +Mult | P4 alone | P4 +Mult |
|---|--:|--:|--:|--:|--:|--:|
| 16² | 1,010 | 456 | 799 | 385 | 313 | 163 |
| 32² | 3,203 | 1,808 | 2,831 | 1,573 | 1,120 | 533 |
| 64² | 13,547 | 6,958 | 11,235 | 6,552 | 4,358 | 2,058 |
| 128² (16K) | 62,316 | **28,466 (35 FPS)** | 50,555 | 29,647 | 17,433 | **9,964 (100 FPS)** |

The modifier roughly **halves** the heavy tick at every grid: a quarter of the logical area, with the 1:N map adding some cost back. The memory price is the LUT-destinations array, +1.7 KB at 16² to +93 KB at 16K on the S3. The classic runs 16K with ~36 KB free heap and a ~26 KB largest block, tight but stable. So 16K Noise with Multiply runs on a board without PSRAM: 35 FPS render-only, and 10–20 FPS with ArtNet output, where the send is the limiter.

## ESP32 firmware size

Per-target image size, capacity and headroom are generated every commit into [repo-health](metrics/repo-health.md#firmware-size), across all 14 firmware variants.

What that table cannot show is where the bytes go. On the default `esp32`, roughly a third of the image is the WiFi stack (`esp_wifi`, `wpa_supplicant`, `esp_phy`). Then come lwIP at ~180 KB, mbedTLS plus the Mozilla root bundle at ~170 KB, and FreeRTOS and the IDF core at ~150 KB. MoonLight's own code is ~120 KB, about a tenth of the binary. That is why `esp32-eth` is the smaller build: excluding WiFi is the single largest saving available. Proportions shift with the IDF version and sdkconfig; measure with `idf.py -B build/esp32-esp32 size-components`.
