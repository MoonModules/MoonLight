# Performance & Memory

MoonLight's per-step **performance contracts** live in the scenario JSONs: each `test/scenarios/*.json` step carries a per-target `contract` block (`tick_us` ceiling + `free_heap` floor) and an `observed` block (the latest reading per target). The scenarios are the source of truth and the assertion surface: every PR runs against them. See [testing.md § Performance contracts](testing.md#performance-contracts-contracttarget) for the contract semantics and renegotiation workflow. The headline numbers users care about are in [README.md § Performance](../README.md#performance).

This document holds what scenarios can't carry: structural sizes (`sizeof`), build-variant deltas, and the WiFi/Ethernet physics that explain *why* a contract comes out where it does.

**Render-loop model.** The Layer's buffer **persists** frame-to-frame, and `Layer::tick()` does not clear it (the FastLED/WLED/MoonLight convention; see [MoonLight, buffer persistence](../explanation/architecture/moonlight.md#buffer-persistence-the-layer-keeps-what-it-drew)). This removed the per-frame full-buffer `memset` that a clear-every-frame model pays, and replaced N per-effect `draw::fade` passes with a single **collected fade** (`Layer::fadeToBlackBy` MINs the requested amounts and applies one buffer pass per frame), so a layer with several fading effects now pays one fade pass, not N. Net hot-path effect on the tick numbers below is small (the clear/fade are one linear pass over the buffer, dwarfed by per-light effect compute and the output driver), but the *model* is what the scenario `observed` blocks were re-measured against on this cycle.

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

`Drivers` grew from 120 → 408 with the per-driver `Correction` stage (256-entry brightness LUT + channel-order table). Other classes grew ~16-32 bytes each as `MoonModule` itself grew (rolling-range observed slot + wired-by-code flag + per-child `tickChildren` accounting fields).

Binary sizes:

| Target | Size | Build |
|--------|------|-------|
| macOS arm64 | 358 KB | debug-arm64 (release-strip is smaller) |
| Windows x64 | 432 KB | MSVC Release, static CRT |

### Kernel micro-bench (host)

`uv run moondeck/check/bench_kernels.py` (the `mm_bench` target, Release, best of 5 over a 256×256 sweep of 16.0 fixed coordinates). A report that gates a kernel swap: the generative-fields plan accepts gradient noise behind the same names only within 1.3× of the row it replaces. Host figures; the S3 is 20-40× slower per core, so the ratio between rows is what transfers to a board.

**Phase 0, before and after the swap to Perlin improved gradient noise (2026-09-04, macOS arm64).** The bound for accepting the swap was 1.3x per sample against the row it replaces, and it had to hold on a board, not only here: the first gradient cut was 2.8x FASTER on this host and 1.5x SLOWER on an ESP32-S3, because a runtime arity argument, select expressions that compile to branches, and eight corners in flight cost nothing on an out-of-order core and everything on an in-order one without a branch predictor. The shipped core is arity-templated, branch-free (a gradient table), 32-bit on the 8-bit tier, and hashes each corner to four bits with one multiply (three hashed to a byte nothing read). Host, ns per sample:

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

**ESP32-S3 (esp32s3-n16r8, 64x64, the four noise effects, tick in µs)**, the measurement that decided it. Same board, same grid, value noise re-flashed from the previous commit for the before column:

| Effect (path) | value noise | gradient, first cut | gradient, shipped | ratio |
|---|---:|---:|---:|---:|
| Noise (inoise8 2D) | 4,621 | 7,009 | 5,049 | 1.09 |
| Noise2D (inoise8 3D) | 6,681 | 10,661 | 8,453 | 1.27 |
| Tunnel (fbm8) | 16,385 | 21,304 | 16,649 | 1.02 |
| PolarNoise (warp8) | 20,356 | 29,199 | 20,490 | 1.01 |

The two noise effects have since merged: `Noise` is `Dim::D3` and renders what `Noise2D` did, so the two rows above are one effect's 2D and 3D paths under the names they carried when the swap was measured.

The 3D path is the closest to the bound and the reason: 3D gradient noise does eight dot products the value form never did, and the S3 instruction count for a 3D sample is 1.3x the old one. Method worth keeping: compile the kernel with the target's own compiler (`xtensa-esp32s3-elf-g++ -O2 -S`) and count instructions, branches and stack spills BEFORE flashing; three restructurings were compared that way in seconds, and the one flash went to the winner. The P4 and S31 numbers are open until those boards are back on the bench.

**All rows, ns per sample, best of 5.** Re-measured 2026-09-04 after the benchmark stopped dispatching through `std::function`: a type-erased call cannot be inlined, so it added an indirect call to every sample and the old figures were part kernel and part harness. Every row roughly halved, which is the size of what was being attributed to the kernels:

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

That reading held: the 8-bit tier improved most (2D by 4x on the host), because the value form quantized at every stage where the gradient form carries its dot products at full width.

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

Two properties an author picks a setting from. **`iterations` is near-linear**: 1 to 20 is 20 to 69 µs, since each is another sweep over the whole grid. **The forcing is free next to the solver**:
going from 2 jets to 4, and persistence from 150 to 255, moved 135 µs to 134 µs, inside the noise.
So the grid and the iteration count are the two knobs that matter, and the jets are a look rather than a cost. **A cube is depth times one panel**: twenty 20×20 slices, each its own medium, cost 249 µs against 133 for one 64×64 panel with about half the lights, which is the per-slice solve paying its boundary and projection twenty times over.

Device rows are open: the P4 and S3 numbers need a board and have not been measured, so what this effect can carry on either is an open question rather than a claim.

### Memory at 128×128 with mirror

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 92 KB | 12 KB buffer + 80 KB LUT (uint32_t indices on 64-bit) |
| Drivers | 48 KB | output buffer (128×128×3) |

---

## ESP32: Olimex Gateway Rev G (no PSRAM, 320 KB internal)

Per-step tick/heap live in `contract.esp32-eth-wifi` and `contract.esp32-eth` across the scenarios; see the [README perf table](../README.md#performance) for the headline grid×board matrix. The notes below cover what those rows don't.

### Run-to-run variance

Individual measurements vary ~5–10% on the Olimex board with no configuration change — inherent ESP32/Ethernet timing jitter (lwIP `tcpip_thread` scheduling, EMAC DMA, Ethernet ACK pacing). Scenarios use 10% default ESP32 tolerance to absorb this; when a step trips, re-run before treating it as a real regression. The `collect_kpi.py --commit` gate parses a single `tick:` line from `esp32/monitor.log` and can flag an unlucky sample — same rule applies.

### What the per-effect sweep established

A 2026-06 Olimex sweep ran every effect alone over a Layer at four square grids, through the real ArtNet and Preview drivers. The rows are gone (the per-effect measurement merged into the light/heavy bracket of `scenario_perf_full`, and per-scenario ticks are generated into [repo-health](metrics/repo-health.md#render-performance)); three findings survive it.

**At 128² the board is ArtNet-output-bound**, not compute-bound: the ~38 ms synchronous send dominates the tick, so nearly every effect converges to 12-23 FPS and effect cost washes out. Below 64² the compute is visible, with Rings, Noise and Spiral heaviest and Lines and Checkerboard lightest.

**Free internal heap falls as the grid grows**, because the Layer buffer, the LUT and the driver output buffer all live in internal RAM on a no-PSRAM board.

**Three effects carry real per-cell state**, and they are the ones to watch for fragmentation headroom at large grids: Particles and GameOfLife allocate a parallel grid-sized array (77 KB free / 34 KB largest block, and 90 KB / 46 KB at 128²), and Fire a heat map (110 KB). Every other effect holds the same ~126 KB free with a ~62 KB largest block, since its per-cell state is negligible beside the buffers. The largest free block is the signal that matters rather than the total: free heap can be ample while fragmentation leaves no single block big enough for the next allocation.

### ArtNet over WiFi vs Ethernet

| | Ethernet | WiFi STA |
|--|----------|----------|
| ArtNet (97 UDP packets) | ~27,000 µs | ~110,000 µs |
| Total tick | ~50,000 µs / 20 FPS | ~130,000 µs / 7 FPS |

WiFi `sendto()` is ~1,140 µs/packet vs Ethernet's ~280 µs — CSMA/CA backoff, rate adaptation, link-layer retries. Not a code regression; WiFi physics. For ArtNet at 16K lights, use Ethernet.

### Build-variant note: WiFi-only `esp32` is slow on Olimex

Same source tree, same MCU (ESP32 classic, 160 MHz):

| Board / firmware | 128×128 tick | ArtNet send |
|---|---|---|
| Olimex Gateway, old WiFi-only `esp32` (pre-collapse) | 220 ms (4 FPS) | 155 ms |
| Olimex Gateway, default `esp32` (WiFi + Ethernet) | 85–95 ms (10–12 FPS) | 38 ms |

The default `esp32` build carries both the WiFi and Ethernet stacks, and `sdkconfig.defaults.eth` enlarges the shared lwIP/WiFi buffer pool via `CONFIG_ETH_DMA_*` — those buffers roughly quadruple ArtNet throughput versus a WiFi-only buffer pool. Generic ESP32 boards (no PCB-trace antenna, less stable 3V3) vary wildly in WiFi TX quality vs the Olimex.

### Memory at 128×128 with mirror

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 52 KB | 12 KB buffer + 40 KB LUT (uint16_t indices on ESP32) |
| Drivers | 48 KB | output buffer (128×128×3) |
| System + Network | 0 | char buffers in class, no heap |

LUT is half desktop size (uint16_t vs uint32_t per entry). The 1:1 (no-modifier) path skips the LUT entirely; see `scenario_Layer_memory_1to1` vs `scenario_MultiplyModifier_memory_lut`.

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

## ESP32-S3 — ESP32-S3 N16R8 Dev (16 MB flash, 8 MB octal PSRAM)

`esp32s3-n16r8` firmware on the ESP32-S3 N16R8 Dev at `Network.txPowerSetting=8` dBm (the brown-out cap injected by `deviceModels.json`). 128×128 grid, Mirror XY, ArtNet over WiFi STA — the grid sweep (now part of `scenario_perf_full`) against the live device. Per-step tick/heap live in `observed.esp32s3-n16r8` across the scenarios. Numbers below are the 128×128 step.

| Metric | Value | Notes |
|---|---|---|
| Total tick | ~164 ms / 6 FPS | Dominated by ArtNet at the 8 dBm cap |
| ArtNetSend | ~93 ms (97 UDP packets) | ~960 µs/packet — slower than full-power WiFi (cf. Olimex `esp32-eth-wifi` at 38 ms) because the cap cuts radio TX margin, association-rate adaptation lands at a lower MCS rate, and packets retry more |
| Free internal RAM | ~240 KB | The comparable, scarce resource. Stays flat (~238–240 KB) across all grid sizes — the Layer buffer + LUT live in PSRAM, so growing the grid doesn't touch internal RAM. This is the number the README perf table shows for the S3, so devices compare on the same axis. |
| Free heap (incl. PSRAM) | ~8,163 KB | The PSRAM-merged total (`totalHeap` reports 8 MB combined). Looks huge but isn't the constraint: PSRAM is ample; internal RAM is the limit. |
| maxBlock (internal) | ~164 KB | Internal-RAM largest contiguous block — the scarce-resource KPI. `maxAllocBlock` (any-memory) reports ~8 MB on PSRAM boards and is meaningless as a pressure signal; SystemModule + scenario_runner use `maxInternalAllocBlock` instead. |
| Layer buffer | 92 KB | In PSRAM (auto by heap_caps preference) |
| Image | 1,307 KB | ~30% larger than `esp32-eth-wifi` due to USB-Serial-JTAG driver + Improv-dual-transport listener |

Per-grid-size FPS from the same sweep: 16×16 → 1672, 32×32 → 287, 64×64 → 25, 128×128 → 6. The steep drop is ArtNet-bound: packet count scales with the pixel count, and at 8 dBm each packet is ~3× slower on-air than the Olimex Ethernet path.

### Why ArtNet is slower at 8 dBm

The brown-out cap drops TX power 12 dB below default (8 dBm vs ~20 dBm). At lower TX power, the WiFi PHY rate-adaptation algorithm picks a slower MCS rate to maintain link margin — for a frame burst this means more time on-air per packet. ~960 µs/packet × 97 packets = the ~93 ms ArtNet budget. The cap is the price of a stable association on this hardware; without it the radio brown-outs and ArtNet doesn't get sent at all.

**Use Ethernet-capable boards for high-FPS ArtNet workloads.** The ESP32-S3 N16R8 Dev fits the "lots of PSRAM, accept WiFi compromise" niche — large pixel buffers or feature-heavy effects that wouldn't fit in 320 KB internal RAM.

### Memory at 128×128 with mirror

| Module | dynamicBytes | Notes |
|--------|-------------|-------|
| Layer | 92 KB | Buffer lives in PSRAM (vs 12 KB on Olimex internal) — full uint32_t LUT instead of halved uint16_t |
| Drivers | 48 KB | Output buffer (128×128×3) |
| Free internal | ~240 KB free, ~164 KB largest block | Plenty of headroom for WiFi + lwIP + Improv-on-both-transports |

The PSRAM-merged heap (`totalHeap() > totalInternalHeap()`) is auto-detected — SystemModule binds the `psram` progress control only when this comparison is true. See `docs/moonmodules/core/SystemModule.md`.

### What the render-only sweep established

A 2026-06 render-only sweep on the S3 measured raw effect cost: no output driver, audio and discovery disabled, so the tick is Layout to Layer to effect alone. The rows are gone, since `scenario_perf_full`'s light/heavy bracket measures that curve on-device now. Two findings survive.

**Effect compute stays visible to 16K pixels**, unlike the no-PSRAM Olimex, because the S3 holds the Layer buffer in PSRAM and never hits an output-bound floor. The cheapest (Lines, Checkerboard, PlasmaPalette) clear ~100 FPS even at 16K; the heaviest is Noise at 51 ms, about 19 FPS, paying a noise sample per pixel, then Rings and GlowParticles.

**Internal RAM is not the constraint on a PSRAM board.** Free internal heap holds ~8.54 MB at small grids and ~8.46-8.49 MB at 16K, the delta being the grid-sized render buffer, and it returns to ~8.54 MB whenever the grid shrinks: no leak and no fragmentation creep across the sweep. The largest free block stays ~90-110 KB throughout.

### MoonLive (scripted effect) — tick + memory

A `MoonLiveEffect` compiles its script to native code for whichever ISA the board runs, once on the cold path (`prepare`), then `run()` is a single function-pointer call each tick. Measured on the S3 at 16×16 (the bench grid the engine is exercised on; the per-tick cost is the native loop, not interpretation):

| Script | Tick (µs) | Exec block (heap) |
|--------|----:|----:|
| `setRGB(5, 255, 0, 0)` (one pixel) | 26 | ~52 B |
| `setRGB(random16(256), 0, 255, 0)` (one host call) | 29 | ~140 B |
| `fill(0, 0, 255)` (loop over all lights) | 47 | ~68 B |

The rows above are a dated S3 bench record; the numbers below them are what a desktop run measures today. The tick cost is native-code speed: a `setRGB` is a bounds-guard + three byte stores (~26 µs including the per-tick module overhead), `fill` adds the per-light loop. The **exec block scales with the program**, not a fixed cap: a one-liner is tens of bytes of machine code (`place()` allocates the emitted length, word-rounded), reported as the module's dynamic memory (`setDynamicBytes(engine_.heapBytes())`, the exec block plus the control arena) so it shows on the UI card. At rest the engine itself is ~48 B of members + that exec block; the compile path's transient buffers (staging, IR, assembler ≈ 4 KB) live on the cold-path stack and are freed on return, see [docs/backlog/livescripts-analysis-top-down.md § 3.7](../work/future/livescripts-analysis-top-down.md) for how this scales as the language grows.

**System variables cost a byte store each, per binding.** They are arena slots the binding refreshes before `run()` — a null check and a byte store apiece, replacing nothing, so the per-tick figure above is unchanged by them. An **effect** writes three (`width`/`height`/`depth`) once per tick; a **modifier** writes six (those plus the `x`/`y`/`z` it is handed) on the mapping-build cold path, not per frame; a **layout** writes none, since it is given no dimensions. `t` adds no arena byte: it is an argument register the host already passes. Not quite free, though — a callee may clobber an argument register under the ABI, so a backend saves it across calls (the arm64 one stacks x3 with the vreg pool; `unit_moonlive_fill` pins that a script reading `t` after a call still sees the host's value). The compile path grew (a system-variable table, resolved before locals and controls) but that is cold-path, once per `source` edit.

**Across the four board classes** (2026-08-17, 16×16, after the Xtensa frame fix): the same shipped scripts on every ISA, so the numbers compare directly.

| Script | S3 (Xtensa) | classic (Xtensa) | P4 (RISC-V) | S31 (RISC-V) | Exec block |
|---|---:|---:|---:|---:|---:|
| `lines.mlv` (two `line()` calls) | 76 µs | 128 µs | 30 µs | 92 µs | 2292 B |
| `plasma.mlv` (9 host calls per cell) | 780 µs | 1038 µs | 577 µs | 725 µs | ~1124 B |
| `ripples.mlv` (~15 host calls per cell) | 1695 µs | 2265 µs | 1098 µs | 1377 µs | 2372 B |

The exec block is the emitted machine code, so it varies by ISA (the RISC-V rows above are the larger encoding); the tick is the native loop. `lines.mlv` is the cheapest of the three because `line()` moves the per-cell loop out of emitted code and into the shared `draw::line`, which is the argument for adding power functions as builtins rather than writing them in script.

**A script calling its own functions** costs what the call costs, and nothing when a script makes none. `crosshair.mlv` (three functions, two calls per frame) ticks at 219 µs on the classic against 204 µs for the same drawing without the recursion guard, so roughly 5-10% on a script that calls. A script with no local call emits no guard code at all, so every shipped script is byte-identical to before: the guard is nine instructions in a function's prologue, emitted only when the program contains a `CallScript`.

The **depth guard** is one arena byte, incremented on entry and decremented in the epilogue. A refused call returns rather than the caller branching around it, which is why the cost sits in the callee and not at every call site. Unbounded recursion therefore degrades instead of resetting: the classic ran a deliberately non-terminating script for 110 s at 109 fps, with the deepest calls doing nothing.

**Two cost models** (2026-08-22, shiffy's 80x48 = 3,840 lights). A shader is a function of position
and time, so it pays per LIGHT; a particle script pays per OBJECT, with the per-particle work inside one C++ loop per call. This is the first script vocabulary where that distinction shows.

| Script | Shape | shiffy 80x48 | desktop 128x96 |
|---|---|---:|---:|
| `plasma.mle` | 9 host calls per cell | 16,031 us | |
| `metal.mle` | ~14 per cell, 3 square roots | 59,600 us | 1,557 us |
| `fountain.mle` | ~9 per FRAME, 300 particles | 1,093 us | 9 us |
| `ballpit.mle` | as above plus `collide` over 64 | 5,127 us | |

`metal.mle` against `fountain.mle` is 54x on the same fixture. `polarR` is what makes the shader expensive: it wraps a real square root, measured at ~3.5 us per pixel for that one builtin, and `metal` calls it three times per pixel. `ballpit` shows `collide`'s N-body cost, which is the one call here that is not linear in pool size: 3.2 us at 48 particles against 0.1 us without, 53.6 us at 200 (host figures; an S3 is 20-40x slower).

**A 1 Hz filesystem scan was stuttering every device.** `FileManagerModule::tick1s()` called
`esp_littlefs_info`, which walks every block of the partition (~80 ms on an S3), inline on the render thread, to feed one progress bar. Frame deltas per second on shiffy went from `83 78 80 79 82 66 72` to `83 85 85 83 84 87 85` once it was throttled to once a minute: the dip is gone and average throughput rose from ~77 to ~85 fps. It is pre-existing, and particles are what made it visible, because a particle INTEGRATES a stall into its trajectory where a shader redraws past it.
One frame after an 80 ms gap moves every particle 6.7x its usual distance.

**Desktop tick across this cycle:** 150 → 133 µs (6666 → 7518 fps), measured by `collect_kpi.py --commit` at each commit. The gain is not from MoonLive — it tracks the two heap-overrun fixes and the register-reuse work landing earlier in the branch. No scenario `contract` was renegotiated on this branch: all 20 scenarios pass inside their existing budgets, which is the assertion surface this page defers to.

**The compile-time staging buffer is sized from the script's tokens**, at 48 bytes per token plus a
256-byte floor, and freed when the compile returns. The constant is the worst case rather than the average, because the buffer is allocated before a byte is emitted: measured across every shipped script on all three backends, the densest is `random-pixel.mlv` at 28.5 B/token on RISC-V (one statement, four nested `random16()` calls, each saving the whole register pool), so 48 is a ~1.7x margin. Density FALLS as a script grows, so a short call-dense script sets the bound: `gradient.mlv` is 5.9 and the longest shipped script is 15.3.

It was 64, measured before host arguments moved into frame slots shrank what a call saves. At that figure the two longest scripts asked for more than the 16 KB sanity bound and were served by the clamp, so a script's buffer had stopped tracking its size. Re-measuring took 25% off the transient allocation, which matters on a classic ESP32 where the compile shares a 12 KB task. A per-ISA test pins that every shipped script still emits under two-thirds of its budget.

**Flash**, measured by building the classic at the branch point and again with the local-call work: 1723295 → 1726027 bytes, +2732 (+0.16%). High per line of source (about 20 bytes for ~137 net lines of code) because nearly all of it is emitter code instantiated once per backend, so one line of the shared lowering becomes three copies of emitted-instruction sequences in the image.

**The compile path's stack grew 640 bytes.** `kAsmLabels`/`kAsmFixups` went from 16/32 to 48/96 because a class allocates a label per function, so `lowerWith`'s frame went 480 → 1120 bytes on the classic — the largest on the chain (144 + 288 + 576 + 1120 = 2128 nested, 17% of the 12 KB main task). It is a compile-path local, not a per-tick cost, but the compile runs on the render task.

---

## Multi-pin LED driving (all three peripherals, 128×128 grid)

The rows below name the peripherals by their pre-consolidation driver-class names (`MultiPinLedDriver` = the `i80` peripheral, `MoonLedDriver` = `MoonI80`, `ParlioLedDriver` = `Parlio`). They are dated bench records kept as measurements; the three are now one `ParallelLedDriver` whose `peripheral` control selects the backend. The timings are unchanged by the consolidation (one vtable dispatch per frame, never per light).

**Async double-buffer is peripheral-specific.** The `doubleBuffer` win recorded in the Parlio row below (the ~7.5 ms wire hidden behind background DMA) applies to `i80` and `Parlio` — they route through a real transaction queue (esp_lcd / the Parlio driver) that absorbs the second in-flight transfer. `MoonI80` runs **single-buffer only** (`supportsDoubleBuffer()` false, and the control is hidden on it): its own-GDMA whole-frame two-buffer handshake races and wedges the bus, and its speed comes from the streaming ring, not from double-buffering a whole frame. On MoonI80 the whole-frame path is therefore encode → transmit → wait, serial per frame (the ring is the scale path).

Each parallel LED driver run on real hardware at a 128×128 = 16384-light grid, 8 lanes (2026-07-12). The **GPIOs used are recorded** because they double as the seed for each board's usable-pin map (the per-model `deviceModels.json` pin defaults are built from proven-working sets, not datasheet guesses). Every listed pin drove WS2812 output on that board without conflict.

| Peripheral | Board | Pins used (8 lanes) | Result | Ceiling / bound |
|---|---|---|---|---|
| **Parlio** | ESP32-P4 (Waveshare P4-NANO) | `20,21,22,23,24,25,26,27` | `Drivers` tick ~30100 µs, fps 30 at 16384 lights (8 lanes, SWAR transpose) | Parlio's single-shot transfer caps at 65535 bytes TOTAL (not per lane), and a light costs `channels × 24 × slotBytes`, so the ceiling is **897 lights/lane at 8 lanes RGB**, 673 RGBW, and halves to 442/332 at 16 lanes (a 16-bit bus doubles `slotBytes`). Over that, the driver reports `too many lights per pin` and keeps running; lifting the ceiling is the [chunked-DMA work](../work/future/backlog-light.md) (tier 1 → ~16-21K). |
| **LCD_CAM i80** (MultiPinLedDriver) | ESP32-S3 N16R8 Dev | data `18,5,6,7,8,9,10,11` · WR(clock) `12` · DC `13` | Same encoder, healthy on real i80; encode scales ~6 µs/light (8×512 = 4096 → 23 ms; 8×1024 = 8192 → 50 ms) | **single-DMA init ceiling 8192–12288 lights** (8×1024 inits; 8×1536 → "LCD init failed — check pins/memory"). A data lane on WR/DC only corrupts *that* lane (it carries the bus-control waveform, not pixels), so the driver **warns and keeps running** — a board that wires all lanes but drives fewer strands can legitimately park WR/DC on an unused data pin. WR and DC on the *same* GPIO is rejected up front (the bus needs two distinct control lines). |
| **RMT** | classic ESP32 (LOLIN D32 / WROOM) | `2,4,13,14,16,17,18,19` (pin 2 = a real 24-LED strand) | 8-pin RMT drives **8×256 = 2048 lights** (tick ~12.6 ms), scales to ~8192 before the tick plateaus; all lanes healthy, pin-2 strand verified lit | **silent alloc-fail:** the RMT symbol buffer sizes for the driver's `count` window, so `count=0` on a 16384-grid needs ~1.5 MB, fails on the ~90 KB heap, and `tick()` bails with **no status** (LEDs dark). Bound the driver with the start/count window; a status for this is [backlogged](../work/future/backlog-light.md). |
| **I2S i80** | classic ESP32 (ESP32-WROVER) | data `2,4,13,14,18,19,21,22` · WR(clock) `32` · DC `33` (pin 2 = a real strand, verified lit) | The classic ESP32 runs the **same** `MultiPinLedDriver` over the **I2S peripheral in i80 mode** (IDF routes the i80 API to I2S here, to LCD_CAM on the S3/P4 — one driver, chip-picked backend). 8-lane doubling sweep (128×128 grid, 2026-07-13): 64/pin (512) → 4877 µs, 128/pin (1024) → 8575 µs, 256/pin (2048) → 15638 µs. Scales linearly at **~7.6 µs/light** (heavier than the S3's LCD_CAM ~6 µs — the classic I2S clock path). `frameTime` reports the WS2812 wire floor (512 → 243 fps, 2048 → 67 fps). The `MultiPinLed` status reports the live count. **16 lanes work on classic too** (the I2S peripheral does the 16-bit i80 bus, 16×256 = 4096 verified), but the WROVER exposes only ~13 non-strap pins, so 8-lane is the practical set. | **Internal-RAM ceiling: 2048 lights at 8 lanes (4096 at 16).** The classic I2S backend **cannot DMA from PSRAM** (`esp_lcd_i80_alloc_draw_buffer` rejects `MALLOC_CAP_SPIRAM` — "external memory is not supported"), so its frame buffer is internal-DMA-RAM only (`maxBlock` ≈ 76 KB). Swept at 8 lanes on a 128×128 grid (2026-07-13): 64/pin (512) ✅, 128/pin (1024) ✅, **256/pin (2048) ✅ — then 512/pin (4096) and above → `i80 bus init failed — check pins / memory`**, a **clean degrade, not a crash** (uptime kept climbing through every rung). That lands exactly on the parallel-I2S acceptance floor (8×256 = 2048), so the classic chip meets its floor and no more. The opposite of the LCD_CAM row below, which reaches 16384 via PSRAM — the classic chip's DMA simply can't get there. **The render is decoupled from this ceiling:** the same sweep kept rendering the full 128×128 = 16384-light grid at every rung (`Layer` ≈ 511 ms/frame, from PSRAM) while the *output* was capped — so a big grid still renders, it just can't all reach the LEDs. At 16K lights the effect render (511 ms) dwarfs the output (24 ms), so multicore cannot help: the render is the wall on this chip. Two classic-only quirks the driver handles: the I2S i80 tx has an unconditional command phase whose busy-wait hangs to a watchdog reset unless given a real 8-bit command (`lcd_cmd_bits=8` / `kI80Cmd=0`), and the draw buffer + a done-ISR marked `IRAM_ATTR`. |
| **LCD_CAM 16-lane** | ESP32-S3 (SE 16 V1 + LightCrafter 16, n8r8) | SE16 data `47,48,21,38,14,39,13,40,12,41,11,42,10,2,3,1` · WR/DC `5`/`6`; LC16 data `47,21,14,9,8,16,15,7,1,2,42,41,40,39,38,48` · WR/DC ghost `33`/`34` | **Reaches the full 16384 lights (the 16K target) where Parlio caps at 4096.** SE16 16-lane doubling sweep (128×128 grid), **async double-buffer ON** (re-measured 2026-07-13 after Step 1.5): 512 → 1843 µs, 1024 → 3422 µs, 2048 → 6612 µs, 4096 → 15153 µs, 8192 → 26788 µs, **16384 → 49916 µs (~20 fps)**: the driver tick is now the *encode* alone, the WS2812 wire wait overlapped in background DMA (`frameTime` reports it separately: 16384 → 28786 µs). That's **~30–56 % faster than the pre-Step-1.5 blocking path** the earlier row measured (async **OFF** reproduces it within 3 %: 4096 → 22518 µs, 16384 → 77732 µs vs the old 21945 / 76979 µs, so the [lcd→i80 rename](../moonmodules/light/drivers.md#led-drivers) is behavior-neutral; the speedup is Step 1.5, not the rename). The `MultiPinLed` status reports the live count (`driving N of 16384 lights`). | **No contiguous-block ceiling, the key difference from Parlio.** LCD_CAM allocates its DMA buffer via `esp_lcd_i80_alloc_draw_buffer` **from PSRAM**, so it isn't bound by the ~368 KB largest-internal-block limit that caps Parlio at 4096 lights; it drives all 16384. **16K is now ~20 fps** (up from ~13 fps pre-Step-1.5). The ENCODE is the wall here, not the wire: async hides the 28,786 µs wire behind DMA (which alone would allow ~35 fps), so the tick *is* the 49,916 µs encode → ~20 fps. Recovering the rest of the deep-per-lane wall (§ Step 3, [multicore top-down](#multicore-the-whole-output-stage-on-core-1-multicore-step-2)), though the ~50 ms encode still runs on **core 0**, which on the LC16 **starves the W5500 SPI-Ethernet** (also core 0) → link drops, HTTP times out while the render loop keeps ticking. This is the measured contention that justifies the [multicore pipeline (Step 2)](#multicore-the-whole-output-stage-on-core-1-multicore-step-2) on classic/S3, a core-budget limit, not a fault. |
| **Parlio 16-lane** | ESP32-P4 (testbench, n16r8) | 16 data pins `21,20,22,23,24,25,26,27,32,33,39,40,41,42,43,44` | 16-lane doubling sweep (`ledsPerPin` 32→256/pin on a 128×128 grid, 2026-07-12; reproduced within 0.3% on a second P4). Tick scales **linearly** with lights: 512 → 1653 µs, 1024 → 2925 µs, 2048 → 5514 µs, **4096 → 10760 µs** at 256/pin. **Async double-buffer shipped (Step 1.5, 2026-07-13):** with `doubleBuffer` ON, the ~7.5 ms WS2812 wire wait moves into background DMA, so the *driver* tick at 256/pin drops **10,820 → 3,790 µs** and the whole board rises **48 → 76 fps** (system tick 20.6 → 13.0 ms). The **`frameTime`** KPI reports the measured wire floor directly: live **7474 µs (133 fps max)** here (the true, measured output ceiling). With the wire hidden, the tick is now **effect render (~7.3 ms) + driver (~3.8 ms) serial**, so the effect is the next bottleneck, which the [multicore pipeline (Step 2)](#multicore-the-whole-output-stage-on-core-1-multicore-step-2) overlaps toward the 133 fps `frameTime` ceiling. (`doubleBuffer` OFF reproduces the pre-Step-1.5 10,820 µs / 92 driver-fps exactly: the synchronous path, kept as the opt-out. ON is simply the better configuration; the switch exists to A/B it. Its one-frame latency saving is below the perceptual A/V-sync threshold, so there is no user class (audio-reactive included) that should run it OFF for latency.) The `ParlioLed` status reports the live count (`driving N of 16384 lights`). | **Single-DMA ceiling ≈ 4096 lights (256/pin).** 512/pin (8192) → `Parlio init failed, check pins / memory`. The P4 has 33 MB free heap but the largest *contiguous* internal block is ~368 KB, and the 16-bit single-shot DMA buffer needs one contiguous block, so it's a **contiguous-block limit, not total memory** (it bites well before the 65535-byte/lane byte cap). Reaching the full 16384 (1024/pin) needs the [Parlio chunked-transfer](../work/future/backlog-light.md) work (frame split across DMA bursts), deferred indefinitely since >~65K lights on one chip is a network-distribution problem. |

**LOLIN D32 (classic ESP32-WROOM) usable LED GPIOs:** `4,13,14,18,19,21,22,23,25,26,27,32,33` plus `16,17` (free on WROOM — they're the PSRAM bus only on WROVER). Avoid straps `0,2,12,15`, the onboard LED on `5`, and battery-sense on `35`; input-only `34–39` can't drive an LED. (Chip-level set: [gpio-usage.md](hardware/gpio-usage.md).)

**Diagnostic used:** RMT `tickTimeUs > 1000` = actively encoding (LEDs on); a tiny ~30 µs tick = the symbol alloc failed and `tick()` bailed (dark). `dynamicBytes` for RMT is the frame buffer (`driverHeapBytes()` returns `frameCap_`: outChannels bytes per light).

The **acceptance floors** these establish for the parallel backends: RMT **8×256 = 2048** (verified above); the parallel-I2S (classic i80) driver **16×256 = 4096** (verified 2026-07-13); the virtual (shift-register) driver **48×256 = 12288** — each backend must clear its floor on real hardware.

## Panel cards over raw Ethernet (`PanelCardDriver`, ESP32-S31)

Measured on an S31 driving two 128x64 HUB75 panels through a ColorLight 5A-75 receiver card over a gigabit RGMII link, 2026-07-31.

| | us/tick | note |
|---|---:|---|
| **PanelCardDriver** | **~2 500** | 16 384 lights: correction + 132 frames handed to the MAC |
| PreviewDriver | 5 687 | the browser preview, same buffer |
| a heavy effect (GameOfLife) | 17 592 | the render, and the largest single cost |

The panel driver is the cheapest active module despite pushing 132 packets per frame (2 brightness + 128 rows + 2 sync, at 497 pixels per row packet). The wall's frame rate is set by the render, not by the output: a heavy effect at 17.6 ms dominates a 26 ms tick, giving ~32 FPS, while a lighter effect mix measures ~56 FPS on the same wall.

**The ceiling is packets, not pixels.** Each frame is sent synchronously from `tick()`, so a taller
wall costs proportionally more rows; a 256-row wall would double the packet count. The card format's 1 Gbit requirement is a wire-time constraint rather than a bandwidth one: at 100 Mbit the same bytes take ten times as long and overrun the inter-frame window the sync depends on ([drivers.md](../moonmodules/light/drivers.md#panelcard)).

**The DMA ring is what makes it stable.** `CONFIG_ETH_DMA_BUFFER_SIZE` defaults to 512 B, so a
1512 B frame spanned three descriptors and a 10-descriptor ring held ~3.3 frames while the driver fires 132 back-to-back. At that depth the S31 refused ~19 000 frames and wedged twice inside 20 minutes; at 1536 B per buffer (one descriptor per frame) plus `CONFIG_ETH_TRANSMIT_MUTEX`, it runs clean. Both are bench-isolated, and ring COUNT is not the lever: 30 descriptors ran no cleaner than
10. Cost: ~20 KB of internal DMA RAM, since the size applies to both rings
([lessons.md](../work/past/lessons.md)).

**Static RAM: 0 B.** The driver's 1 512 B packet buffer is a class member, so it costs nothing on a
board that never adds the driver; `check_footprint --module PanelCardDriver --firmware esp32s31` reports ~3 500 B of flash and no static RAM.

No scenario contract yet: the driver needs a receiver card on the wire, so the numbers above are a bench record rather than an asserted ceiling.

## HTTP cost of the P4's WiFi co-processor (`esp32p4rev1-eth-wifi`)

The P4 has no native radio: WiFi comes from an on-board ESP32-C6 over SDIO. Compiling that path in costs HTTP throughput **on an interface it does not carry**, which is why it is measured over Ethernet on both images: same board, same commit, same cable, so the only variable is whether esp_hosted is in the binary.

| build | per-request (`/api/system`) | throughput (76 KB `app.js`) |
|---|---:|---:|
| `esp32p4rev1-eth` | 10 ms flat | 1,973 KB/s |
| `esp32p4rev1-eth-wifi` | 40 ms typical, one 280 ms outlier in 12 | ~980 KB/s |

So roughly **4x per request and 2x on throughput** for having the co-processor compiled in. Render is unaffected (359 fps on the WiFi build), so this is not frame-loop contention: the cost is per-REQUEST rather than per-byte, which points at a periodic blocker a request waits out rather than a slow pipe.

Measured on IDF v6.1-rc1. The penalty was far worse on v6.1-beta1 (33-60x per request, 17x throughput, with requests alternating 0.4/0.8 s); most of that is gone and what remains is tracked in [backlog-core](../work/future/backlog-core.md).

## Multicore: the whole output stage on core 1 (`multicore`, Step 2)

The `multicore` control on the Drivers container runs **every driver's per-frame work** — the LED encode, the ArtNet packet build, the preview frame build — on a **core-1 task**, while the render loop draws the next frame on core 0. A frame costs `max(render, output)` instead of `render + output`. It stacks with the driver's `doubleBuffer` (which hides the WS2812 *wire* behind DMA on one core); this hides the *encode* behind the *render* on the other.

**Measured live by flipping the switch** (SE16, 64×64 grid, 16 lanes × 256 = 4096 lights; reproduced ON→OFF→ON):

| | `multicore` ON | `multicore` OFF |
|---|---|---|
| whole-board fps | **46** | 32 |
| system tick | 21,316 µs | 30,857 µs |
| **`Drivers` cost on core 0** | **2,261 µs** | 15,404 µs |
| Preview | 91 µs | 49 µs |
| HttpServer | 409 µs | 348 µs |

**85 % of the output stage leaves the render core** (15,404 → 2,261 µs), for **+44 % fps**. The encode itself is unchanged (~20 ms at 4096 lights on the S3) — it simply runs on the other core, where the render no longer waits for it.

**Calling the network stack from core 1 costs ~100 µs/frame, and it does not matter.** lwIP is pinned to core 0 (`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0`), so a driver that writes a socket still hands its bytes to the network task there — only the *CPU half* (packet / frame building) offloads. The cross-core lock and cache bouncing show up as Preview 49 → 91 µs and HttpServer 348 → 409 µs. That ~100 µs is set against the ~13,000 µs of output work removed from core 0 — a 130:1 trade, which is why no driver is special-cased: when the split is on, **all** of them move.

**The pull-model preview transport bounds those Preview numbers from above.** Since the preview became a pull channel (one resumable drain, no socket writes from the encode thread at all), `PreviewDriver::tick` on core 1 only gathers and ARMS a message — every socket byte moves on the transport's tick20ms on core 0, paced by TCP. Two consequences the table rows cannot show: with no standing client request the preview costs **zero** (the tick returns before any work, so an unwatched device spends nothing), and a congested link no longer touches the render path at all — `renderWait` stays at its normal few ms where the prior direct-send path could hold the output stage up to its ~150 ms stall budget (bench-observed as 180,000 µs renderWait spikes on S3/WiFi, gone after the change; four boards then held ~12 minutes of continuous viewing with zero socket closes).

**It also removes a contention.** A ~19 ms inline encode on core 0 starves the network stack sharing that core: the LightCrafter 16's W5500 Ethernet drops its link and HTTP times out while the render loop keeps ticking. With the encode on core 1, an HTTP hammer during a heavy 8192-light encode holds: 77 requests, median 163 ms, one timeout.

**Per-chip `renderWait`** — the read-only KPI reporting the worst core-0 wait at the frame boundary in the last second. It says how much idle time a *second* handoff buffer (the deferred ping-pong step) would recover, so the decision is measured rather than assumed:

| Board | Backend | `renderWait`, heaviest effect (Noise @ 128²) |
|---|---|---|
| SE16 (S3) | LCD_CAM i80 | ~1 µs |
| P4 | Parlio | ~7 µs |
| WROVER | classic I2S | ~15 µs |

Under a heavy effect the render dominates on every board, so core 0 never idles and the ping-pong buffer would buy nothing. It only pays when the effect is much cheaper than the output work (a light effect driving many lights), where the wait grows to milliseconds.

**Memory and degrade.** The split needs one frame buffer — the stable frame core 1 reads while core 0 renders the next. When two or more layers composite (or a single layer needs a LUT map) that buffer already exists; in the identity single-layer case, which is otherwise zero-copy, the split allocates it. If it does not fit, the split **does not engage**: no task is spawned, every driver ticks inline on core 0, and the driver reads the layer buffer directly — exactly the pre-multicore behavior, and `doubleBuffer`'s DMA overlap still applies. The switch stays on (the user's intent is kept) and the split re-engages by itself once the memory is there, so there is no half-split state where the two cores could wait on each other.

## Incremental cost analysis (`scenario_perf_light` / `scenario_perf_full`)

These two scenarios start from a clean canvas and add one subsystem at a time, measuring the tick/heap delta per step, so each module's cost is isolated. Measured live (2026-06-17, render-only, audio + discovery disabled) on all three boards:

- **classic** — Olimex Gateway, ESP32 @240MHz, **no PSRAM** (320KB internal), `nrOfLightsType`=uint16
- **S3** — ESP32-S3 N16R8 @240MHz, 8MB PSRAM, uint32
- **P4** — Waveshare P4-NANO, ESP32-P4 @400MHz dual-core, 32MB PSRAM, uint32

All figures tick µs at 16² unless a grid is named; ~5–10% run-to-run variance, so small (<~30µs) deltas are near the noise floor.

### Per-subsystem cost (added one at a time, 16² grid)

Absolute tick at each step (the diff vs the prior row is that subsystem's cost):

| Step | classic | S3 | P4 | Reading |
|---|--:|--:|--:|---|
| Render floor (Grid+Layer+Checkerboard) | 129 | 133 | 67 | the baseline; P4 ~2× faster |
| − Audio disabled | 116 | 111 | n/a | **audio ≈ +13–22µs/tick** (fixed I2S block-read; no mic on the P4) |
| − Devices discovery disabled | 116 | 112 | 56 | idle discovery is free (boot sweep is one-shot) |
| + MultiplyModifier (2×2) | 315 | 292 | 96 | **+180–200µs** — the per-frame blend+map over the LUT |
| + PreviewDriver | 115 | 118 | 56 | apparatus; free |
| + NetworkSendDriver | 139 | 141 | 67 | ArtNet/DDP build+send; cheap at this size |
| + RmtLedDriver (64 LEDs) | 152 | 120 | 56 | per-frame encode+transmit at a fixed 64-LED output |
| + MultiPinLedDriver (64 LEDs) | ✓³ | 142 | 57² | i80 bus: classic ESP32 → I2S, S3/P4 → LCD_CAM |
| + ParlioLedDriver (64 LEDs) | n/a¹ | n/a | 58 | P4 Parlio |

¹ Parlio is P4-only; its classic row is **not** a real measurement (the driver isn't compiled/registered on classic, the optional add is skipped, so the row re-measures the prior pipeline). Gating drivers out per chip is done in `main.cpp` (each `registerType` is `#if`-gated on the SOC macro). ² P4 has LCD_CAM too, but Parlio is its scale path. ³ `MultiPinLedDriver` **is** registered on classic (over the I2S i80 backend) as of 2026-07-13, so the classic i80 row is now a real measurement — see [§ Multi-pin LED driving](#multi-pin-led-driving-all-three-peripherals-128128-grid). "n/a" = driver absent on that chip.

**Expected, and confirmed everywhere:** audio is a small fixed per-tick cost; idle discovery is free; output drivers are cheap at a capped 64-LED output (none dominates the render path). The modifier's +~190µs at 16² is the one notable per-frame add — explained below (it's the blend+map, and it *pays for itself* at large grids).

**Multi-layer composition** (the `Drivers` composite loop): a single enabled Layer is the pass-through fast path (the driver reads the Layer's buffer directly — zero composite cost, the figures above). Each *additional* enabled Layer adds one `blendMap` pass over the physical buffer (integer alpha-over or additive, branch-resolved once per layer), so N enabled layers cost ≈ N × the per-layer write — linear in layer count, same shape as the per-effect sweep. The RegionModifier adds nothing unless present (no modifier = the identity fast path).

**Branch re-verification (2026-06-25, multi-layer + RegionModifier):** the live perf scenarios (`scenario_perf_light` / `_full` / `_modifier_swap`) were re-run on all three boards on this branch's firmware. The per-effect and per-modifier numbers match the tables here within run-to-run variance — the new composite/RegionModifier code does not regress the single-layer pipeline (it's opt-in: a one-layer tree runs the same path as before).

### Effect compute — light vs heavy bracket, across grid sizes (render-only)

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

All curves scale **~linear in pixel count** (no superlinear blowup → no realloc/fragmentation pathology). The heavy effect is the 16K bottleneck on every board, and the board ranking is P4 ≫ S3 > classic on heavy compute (the P4's 400MHz dual-core is ~3× the S3). **Surprise worth noting:** at light-16K the *classic* (4,360µs) beats the S3 (7,949µs): the S3's PSRAM-resident buffer has higher access latency than the classic's internal RAM for the cheap Checkerboard inner loop, and classic's uint16 LUT is half the size; on the heavy effect the compute dominates and the S3 pulls ahead again. Fixed-point / strided-sampling ideas are on the [backlog](../work/future/index.md).

### MultiplyModifier — compute down, memory up (Noise effect)

Often misread (I misread it first): with the default 2×2 kaleidoscope the modifier makes the *logical* grid ¼-size, so the effect computes on fewer pixels — the modifier **reduces** tick at large grids, and its real cost is the 1:N mapping-LUT **memory**.

Tick µs, Noise alone vs Noise+Multiply:

| Grid (physical) | classic alone | classic +Mult | S3 alone | S3 +Mult | P4 alone | P4 +Mult |
|---|--:|--:|--:|--:|--:|--:|
| 16² | 1,010 | 456 | 799 | 385 | 313 | 163 |
| 32² | 3,203 | 1,808 | 2,831 | 1,573 | 1,120 | 533 |
| 64² | 13,547 | 6,958 | 11,235 | 6,552 | 4,358 | 2,058 |
| 128² (16K) | 62,316 | **28,466 (35 FPS)** | 50,555 | 29,647 | 17,433 | **9,964 (100 FPS)** |

So the modifier roughly **halves** the heavy tick at every grid (¼ logical area, but the 1:N map adds back some cost). The memory price is the LUT-destinations array — on the S3 it cost +1.7KB(16²)→+93KB(16K); on the **classic at 16K it ran with ~36KB free heap / ~26KB largest block** — tight but working, no crash, no degrade. This **confirms the no-PSRAM viability**: 16K Noise+Multiply runs on the classic at 35 FPS render-only (and has historically run at 10–20 FPS when also sending out over **ArtNet** — that send, not the render, was the limiter). Not a no-PSRAM blocker.

## ESP32 firmware size

Per-target image size, capacity and headroom are generated every commit into [repo-health](metrics/repo-health.md#firmware-size), across all 14 firmware variants.

What that table cannot show is where the bytes go. On the default `esp32`, roughly a third of the image is the WiFi stack (`esp_wifi`, `wpa_supplicant`, `esp_phy`), then lwIP at ~180 KB, mbedTLS plus the Mozilla root bundle at ~170 KB, FreeRTOS and the IDF core at ~150 KB, and MoonLight's own code at ~120 KB, about a tenth of the binary. That is why `esp32-eth` is the smaller build: excluding WiFi is the single largest saving available. Proportions shift with the IDF version and sdkconfig; measure with `idf.py -B build/esp32-esp32 size-components`.
