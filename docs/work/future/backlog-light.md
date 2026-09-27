# Backlog — light domain

Forward-looking to-build items for the **light domain** (`src/light/`: drivers, effects, layouts, modifiers, preview) and its sensors. The core/infrastructure counterpart is [backlog-core.md](backlog-core.md); cross-domain items are in [backlog-mixed.md](backlog-mixed.md). Index + overview: [README.md](index.md). Completed items are removed.

- ❌ **Cap the particle frame scale** (open): `FrameTime` spends a whole stall in one frame, so an
  80 ms hiccup moves every particle **6.7x** its usual distance in a single step (measured). That is
  the rule working as designed, and it keeps the trajectory correct in real time, but a particle
  INTEGRATES the gap where a shader just redraws from the clock and skips a frame invisibly. So
  particles are the first effect class that makes system jitter visible, and they did: they exposed
  a 1 Hz LittleFS scan on the render thread (since fixed) and they still show the previewer's
  socket write and a UI reload.

  The fix would clamp the scale a pool sees to ~2 reference frames, trading real-time accuracy for
  smoothness. **Deliberately not done**: it makes motion lie about elapsed time, which
  [the tick-rate rule](../../explanation/architecture/index.md) exists to prevent, and every stall it
  hides is a real defect somewhere else that would stop being visible. WLED-PS takes the opposite
  side (`ParticleSystem2D::update()` advances a fixed amount per call, with no `millis()` anywhere),
  so its motion speed is a property of the frame rate. **Build trigger**: a stall we cannot remove
  at its source, on hardware a user actually has.

## Effects

### Moving-head effects from MoonLight, including two of troyhack's (2026-09-04)

MoonLight has several moving-head effects that have no equivalent here, two of them troyhack's.
Migrate them all, on the power functions per the standing mandate rather than traced across.

### RMT over DMA on the S3/P4, with a completion callback (Funkelfetisch, July 2026)

Funkelfetisch's fork carries a finished branch, `codex/upstream-rmt-rgbw-performance`, that the
classic-ESP32 flicker work of 2026-09-05 makes worth adopting: on chips with RMT DMA
(`SOC_RMT_SUPPORT_DMA`: S3, P4) it sets `with_dma` with the IDF-recommended 1024-symbol block, so
the frame streams from RAM and the refill interrupt that causes flicker on a DMA-less chip does not
exist at all. It replaces the blocking `rmt_tx_wait_all_done` with `rmt_tx_register_event_callbacks`
(`on_trans_done`) plus a per-channel busy flag so the next tick skips while a frame is in flight,
and reports `"RMT DMA"` in the driver status so a user can see which path is live. Files:
`platform_esp32_rmt.cpp` (+105), `RmtLedDriver.h` (+75), `LedDriverConfig.h`, `Correction.h`
(RGBW presets, a separate topic in the same branch), with unit tests.

What it does NOT address: on the classic ESP32 the DMA half compiles to nothing, and nothing in it
moves the channel's interrupt off core 0 (the root cause found on the Dig-Next-2, fixed by
creating the channel from core 1). The two are complementary, one driver with the right answer
per chip: DMA where the silicon has it, the core-1 refill where it does not. Adopt his DMA and
callback path and keep the core hop. Study, do not copy: write it against the seam as it stands,
credit the branch.

Re-read against the wire-byte driver (2026-09-09), which changed two of the assumptions above. The
driver no longer holds pre-expanded symbols at all: it ships the correction's wire bytes and the
peripheral expands them (IDF bytes encoder on the DMA chips, the level-5 refill on classic), so the
frame buffer is ~3 bytes per light rather than 96. So (a) his `mem_block_symbols = 1024` is now the
only symbol memory in play and is cheap, and (b) `rmt_transmit` already takes the bytes, so the DMA
path needs no buffer change, only `flags.with_dma` plus the callback. The `txInFlight_` guard STAYS
whatever happens: the peripheral streams straight out of the driver's frame buffer, so a rebuild
that frees it mid-frame still tears; his busy flag would replace the blocking wait, not that guard.
The win left on the table is the blocking `rmtWs2812Wait` on S3/P4, which today costs the tick the
whole wire time of the longest strand.

### A script's setControl rebuilds a control subtree on every write (2026-09-06)

Measured on the P4 at .139: **2 fps**, with `MoonLive-2` at 251 ms and `MoonLive-3` at 245 ms per
tick, together 498 ms of a 506 ms frame, and HTTP down to a 0.5 s round trip because it is served
from the same loop. Both services ran `sweep.mls`, whose `tick20ms` writes four faders. Two copies
at 50 Hz is 400 control writes a second, and `Scheduler::setControl` calls `rebuildControls()`
unconditionally on each one:

```
clearControlsRecursive();   // wipes this module's controls AND every child's, recursively
defineControls();           // then rebuilds them all
```

So each fader write tears down and re-creates the whole `Control` subtree. A person moving a slider
does this a few times a second and nobody notices; a script at 50 Hz multiplies it by hundreds.

Two things to fix, and they are independent. **The rebuild should be conditional**: a `live`
control's value change cannot alter the schema, and `rebuildControls` already computes
`schemaSignature()` before and after to decide whether to notify, so it knows. Rebuilding only when
the shape can actually change (what `setLive` and `affectsPrepare` already distinguish) removes the
cost for every value write, scripted or human. **And the script tick is too fast for its job**: a
fader sweep does not need 50 Hz, so `sweep.mls` should run on a slower tick, which also caps the
damage any future script can do through this path.

Not a regression from the RMT work: it predates it and was found while measuring an unrelated slow
board.

### Speed up the fluid solver: 4 fps at 128x128, and it is not the divide (2026-09-05)

Measured on an S31 (RISC-V, 320 MHz, octal PSRAM at 200 MHz), `iterations` 5, depth 1:

| grid | Fluid tick | fps | cycles per cell-update |
|---|---|---|---|
| 32x32 | 8.5 ms | 109 | 151 |
| 64x64 | 38 ms | 24 | 159 |
| 128x128 | 204 ms | 4 | 205 |

A cell-update is four loads, three adds, a multiply and a store: under 20 cycles of arithmetic. It
costs **151 even at 32x32**, where the whole working set is small enough to cache, so the loop
itself is roughly 8x more expensive than the work it does. Memory adds a further 35% by 128x128 but
is not the wall: at 4 fps the solver moves ~25 MB/s, about 5% of what this PSRAM delivers.

**A wrong turn worth recording.** The first diagnosis blamed the 64-bit divide in `relax()`, on the
reasoning that Xtensa has no integer divide instruction. It was implemented (a power-of-two shift
dispatched once per call, bit-exact over 8M values) and measured on the board: **no change, 326 ms
before and after**, so it was reverted. Two errors: the S31 is RISC-V rather than Xtensa, and at
151 cycles per update the divide was never the dominant term. A desktop measurement could not have
caught either, since arm64 divides in hardware; only the board settles it.

**Allocation placement is worth 1.6x, and nobody chose it.** Same firmware, same grid, same
controls: **326 ms after a fresh boot, 204 ms after resizing the grid to 32 and back to 128**,
reproducible across reboots. The solver's six buffers are ~638 KB and land in PSRAM either way, but
where they land at boot is slower than where they land once the heap has moved. Whatever is done
about speed, this says a boot-time allocation can be paying a large penalty invisibly, and it is
worth understanding before optimizing the loop around it.

Ordered by expected return:

1. **Cut the 64-bit arithmetic in the inner loop.** Every cell computes an `int64` shift, an
   `int64` multiply and an `int64` divide on a 32-bit core, where each is several instructions and
   a register pair. This is the most likely source of the 151 cycles. A 32-bit formulation, or a
   narrower intermediate with a proven bound, is the first thing to measure. Precedent: the SWAR
   work found the 32-bit pair form bit-identical and 41% smaller.
2. **Hoist `idx()`.** The loop addresses five neighbors per cell through `idx(x, y)`, each a
   multiply-add. Walking row pointers instead is the standard fix and removes most of the address
   arithmetic.
3. **Understand the allocation-placement effect above**, since it is worth more than most loop
   tuning and costs nothing to trigger deliberately once understood.
4. **Solve the pressure at half resolution.** Pressure is smooth, so a half-scale solve with a
   bilinear upsample of its gradient costs a quarter of the cells. Same trade `fieldScale` already
   makes for noise fields, measured 3.0x there. Changes the picture slightly, unlike 1 to 3.
5. **Fewer iterations, documented per target.** Linear in cost: 5 to 2 is 2.5x, and the picture
   gets springier. The card should say what a target can afford rather than leaving a user to find
   4 fps.
6. **Question whether the full solver belongs on this class of board at all.** See the ColorTrails
   entry below: a separable noise advection gets a flowing, swirling picture for two passes over
   the grid and no solve. The fluid's own header already calls it a desktop and P4 effect.

**Why `fluid.mle` runs at 13 fps while the compiled effect runs at 3.** They are not the same
algorithm, and the script is not a faster fluid: it is not a solver at all. `fluid.mle` calls
`flowCurl`, which is curl noise, a divergence-free velocity read analytically from noise
derivatives in one advection pass, with no solve. Per frame at 128x128 the script visits ~16k cells
and divides nowhere; the compiled solver visits ~429k, of which 318k carry the 64-bit divide. That
is 27x the work, and the measured gap is only 4.3x because the interpreter gives most of it back in
dispatch overhead. So a COMPILED curl effect would beat both. What curl cannot do is what a solver
does: no pressure, no interaction between jets, and no vortex forming out of the flow's own
history. Whether that is worth 27x is a question for the product owner's eyes.

**Measure on hardware, not on the desktop**: this is an in-order-core property and the desktop
divides in hardware, which is exactly how the wrong diagnosis above survived a desktop check.
Record before and after in performance.md per target.

## Drivers

### Logarithmic brightness, and a power budget the device knows about (2026-09-02)

`Drivers.brightness` scales the output linearly, and perceived lightness is not linear: the eye is
closer to logarithmic, so the bottom quarter of the slider spends half the power budget for a modest
visible change while the top half buys little and costs a lot.

**Measured on MM-StadBeest** (LightCrafter 16, 1440 lights, through the new `power.mls` readout):

| brightness | rail | current |
|---|---|---|
| 16 | 4.86 V | 0.9 A |
| 60 | 3.9 V | 4.3 A |
| 120 | (browned out) | |

Current is roughly linear in the control value, so the usable range is compressed into the bottom of
the travel and the top of the slider is a power hazard rather than a setting. The board crashed
twice during this session at brightnesses a user would reasonably try.

**Two pieces, and the second is what actually prevents the crash:**

- **A gamma or log curve** from the control value to the output duty, so equal slider steps look like
  equal brightness steps. The open question is WHERE: on the `brightness` control (every driver
  inherits it, but the number then means something different to OSC, MQTT and every saved preset) or
  in each driver's output stage (no meaning change, duplicated per driver).
- **A power budget.** A device that knows its supply limit can cap brightness instead of browning
  out. Boards with a sense resistor can measure it (see the power-monitoring entry below); boards
  without can estimate from light count and channel values, which is what WLED's ABL does.

NOTE the rail sag above is NOT the supply's fault: an LRS-350-5 delivers 60 A, and this browned out
at 4.3 A. Roughly 0.25 ohm of series resistance in the feed, so wiring and injection points, which
is worth measuring before tuning anything in firmware.

### SE16 / LightCrafter power monitoring: sense pins now free, module still to build (2026-09-02)

Both boards carry voltage and current sensors, and MoonLight records the pins
(`MoonBase/Modules/ModuleIO.h`, board presets): **SE 16 V1 voltage GPIO 8, current GPIO 9;
LightCrafter 16 voltage GPIO 5, current GPIO 6**.

Those pins were occupied by our LED driver's `clockPin`/`dcPin`, which looked like a hard conflict
until the meaning of those controls settled it: they are the **sacrificial** WR and DC lines
`esp_lcd` mandates to build an i80 bus, toggled harmlessly with nothing wired to them
(`MultiPinLedDriver::addBusControls`). Any free GPIO does, so they moved rather than the sensors:
SE16 to 16/17 (it has 4/16/17 spare, so its native USB on 19/20 stays free) and LightCrafter to
19/20 (its only spare pair, and it uses the UART bridge on 43/44 anyway).

**Still open:** `AnalogService` shipped (plan step 3) and is host-verified, so the consumer exists.
What remains is per board: `voltagePin`/`currentPin` in the two device definitions, "Power
monitoring" moving from `planned` to `supported`, a scale factor for each (the divider ratio and
shunt value, which are NOT in MoonLight's preset and need the schematic), and a reading checked
against a meter.

Worth doing because it also gives the ADC seam a real bench rig: an on-board analog signal beats a
hand-wired potentiometer. NOTE the pin move is unverified on hardware, both boards being offline
when it was made: confirm LED output still works after the change before trusting it.

### MoonI80 streaming ring — 48×256 shipped; open instruments and cleanups

The ring's two regimes ship and are wall-verified through 48 strands × 256 (12,288 lights): prime-only when the frame fits the pool, the clock-oracle lapping ring above it (the near-prime pool — the ISR encodes only `nSlices − ringBufs` slices per frame), with `ringAuto` deriving the geometry per config and `shiftOverclock` trading the fps ceiling against '595 shift margin. The mechanism lives in the code + the technical page; the design arc in `docs/work/past/plans/` (the MoonI80 plans, all marked). Open items:

- **Last-8-panels white flash — the "44-46 flash" (OPEN, cause not yet found; 6 theories ruled out).** On the 48×256 wall, brief WHITE/bright flashes (not wrong colors) over an otherwise-correct image, confined to strands 40-47 (the last 8 panels = the last physical data pin, GPIO 17 = bus bit 5 = the 6th and final 74HC595 in the daisy chain), mostly panels 44-46, wandering within the last 8. **Brightness-gated: clean below 5, flashes at ≥5** — since brightness scales pixel values through a LUT, below 5 the frame collapses to near-all-zero bits, so the gate really means *the flash needs SET (one) data bits on that pin*. **The same physical wall runs clean on hpwit's driver, so it is OUR driver, not the hardware.** Ruled out by live hardware tests (do NOT re-chase): (1) the encode source (PSRAM vs internal — the ISR-source staging fix did nothing, reverted); (2) the ISR/lapping regime (the flash is on prime-encoded rows too); (3) it being a tail-*rows* phenomenon (it is per-strand-group); (4) shift-clock margin via `shiftOverclock` (already at the 20 MHz OFF setting, still flashes); (5) the encoder itself (`ParallelSlots.h` proven byte-for-byte correct for this geometry by a host compile, incl. a pin-5 walking-one test — no all-HIGH "white" value ever appears in a pin-5 data word); (6) bus-bit latch adjacency (a `latchBitHigh` diagnostic moved the latch off bit-5's neighbour to bit 7 — still flashes). **Reopened clue:** the shift-register analysis doc records hpwit running the '595 SRCLK at **19.2 MHz**, *slower* than our `shiftOverclock`-OFF **20 MHz** — we treated 20 MHz as "the slow floor" but it is above his proven-good rate; the ~4% could be the deepest chip's margin. Untested angles to try next: driving the '595 clock below 20 MHz (needs a new divider — 16 MHz is all-white, so the window is narrow); GPIO-17 drive-strength / edge-rate specifically; whether it follows bit-5-*position* or GPIO-17 (swap which strands ride bit 5 via the pin order); the `ringPad` inter-slice settle window; and a direct A/B of our per-slice frame timing vs hpwit's for the last chip. All diagnostics from this investigation (the `latchBitHigh` toggle, the ISR-staging code) are reverted — the tree is clean.
- **MoonI80 direct mode flickers below ~30 lights on a PSRAM frame buffer (OPEN, cause unproven).** Ten GRBW lights on an SE16 (S3, octal PSRAM, one strand) flicker continuously and periodically report `no LED output` — the dead-frame guard after 8 consecutive `busWait` timeouts, i.e. the transfer never signalled completion. Above ~30 lights it is clean, and **i80 and RMT drive the same wiring perfectly**, so it is not the encoder, the layout, the wire, or the strip. **Established:** moving the buffer from PSRAM to internal RAM makes it stop outright (bench-verified). **Not established:** why only small frames — the stall-to-frame ratio (2.9% at 10 lights), the wait budget (*more* generous at small sizes: 55x the wire time vs 2.8x at 1000 lights) and PSRAM alignment padding (zero; the frame is already 64-byte aligned) were all checked and all fail to explain it. Leading untested theory: a short frame gives the DMA no runway to prefetch through a PSRAM/cache-contention stall. **An internal-RAM fallback for small frames was written, measured and deliberately reverted** — internal RAM is the scarce pool, MoonI80 targets large fixtures, and a short strand is the i80 backend's job; a patch that spends scarce RAM to hide an unexplained cause is worse than the open bug. Next instrument: `loopbackTest` + `loopbackIntrusive`, which captures what the peripheral actually emitted and separates a corrupt frame from a stalled transfer (needs the RX jumper pin — the SE16 routes its LED outputs, so pick one that can read back). Full write-up in [lessons.md](../../work/past/lessons.md).
- **Ring bus init hard-fails instead of stepping down when `ringBufs` is raised past what RAM allocates** (wall went dark until the control was lowered again; the pool alloc steps down but a later allocation, likely the descriptor link list, does not). A control change must degrade, never dark the output.
- **~1-frame white/colored flash every ~5 s** seen at some configs — plausibly fixed by the frame-close latch word (a strand whose last data bit ended HIGH missed its reset that frame); soak-observe on the wall before closing.
- **Prime barrier fps cost.** The ring's prime holds a busy-wait until the previous frame's deterministic wire end (`waitWireDrained` in `primeRingRange` — the barrier that keeps the next prime off buffers the DMA is still draining; the frame's last slices lap into the FIRST buffers, so the prime hits them first and no counter sees the repaint). The wait is ~0 when the snapshot + render gap already span the wire, but on fast frames it serializes wire → prime and caps fps at 1/(wire + snapshot + prime). If the fps work wants that overlap back, the barrier can go finer-grained (per-buffer: buffer b is safe once the drain passes slice `b + nSlices − ringBufs`) — measure first. Do NOT drain-gate `done` in the ISR instead (deadlocks; wall-measured as flicker-then-"no LED output").
- **Multi-strand loopback**: the instrument drives one `loopbackStrand`, so it is structurally blind to a multi-strand fault (it passed while the wall was visibly corrupt).
- **Shift-mode loopback host coverage**: nothing in the suite drives shift-mode loopback end-to-end through the mock bus — the detection gap that let two loopback bugs live unnoticed. (The stall bug itself is fixed: capture-first alloc + pool step-down; verified on two boards.)
- **Loopback teardown leak + heap fragmentation**: cycling the private-bus loopback drops ~80 KB internal and the heap stays fragmented (measured maxBlock 13–44 KB with 240–310 KB free). Capture-first + step-down made runs reliable despite it, but the leak itself stands — chase when the loopback is next touched.
- **Ride-the-live-ring loopback — parked**: built (`loopbackIntrusive` + the snapshot pattern hold) but an RMT-RX cannot capture on a GPIO the LCD_CAM is actively driving (reads 0 sym). Revival path: briefly output-detach just the RX pin for the capture window.
- **Diagnostic surface** (`ringDbg` incl. tw/ts/tp + sg/se, the dbg statics, the `kCyPerUs = 240` hardcode): kept deliberately through the tuning era; gate/remove when the ring fps work closes.
- **fps header reports the module TICK rate**, not the frame rate (`frameTime` is the real one); any fps claim predating 2026-07-17 reads with that in mind.
- **`ringAuto` "just works" — verify the geometry pick on smaller configs.** `ringAuto` (default on) derives `ringRows`/`ringBufs`/`ringPadUs`, and those manual knobs are already dev-only (`setAdvanced`, expert mode). The pick is wall-verified at 48×256; the smaller common configs (16×256 and below) still need a bench pass. If any has a strictly better geometry `ringAuto` misses, teach it that ONE *measured* rule (no per-effect/per-density heuristics — the flicker was never geometry). `ringAuto` itself stays visible as the recourse until this proves it always picks right, then it can go expert-only too. (The fork-join flicker that once blocked this shipped fixed — snapshot fork removed.)

### PSRAM-at-shift-clock: verified NOT a viable lever for more lights/strand (research, 2026-07-16)

A recurring idea is to "borrow from direct mode": direct mode streams a huge frame straight from PSRAM (2048 lights, clean; SE16 drives 8192 lights direct/whole-frame at ~19.5 ms), so could shift mode run a lower pclk and stream its whole frame from PSRAM too, trading fps for unlimited length? **Verified answer: no.** The shift pclk is bounded by the WS2812 waveform, not by the '595 and not by divider elegance: slot = 8 bus words / pclk, and *lowering* the clock lengthens T0H toward the max-white washout. The practical floor is the `shiftOverclock`-OFF rate, 20 MHz (T0H 400 ns — wall-verified; 16 MHz is already all-white), which barely dents the PSRAM demand. There is no shift pclk that is both slow enough to stream from contended PSRAM and fast enough to keep T0H under the 0-vs-1 threshold.

The bandwidth arithmetic (datasheet-derived): DMA demand = bus-bytes × pclk. Direct 8/16-bit = 2.67/5.33 MB/s; shift 8/16-bit = **26.7 / 53.3 MB/s**. S3 OPI PSRAM (octal, 80 MHz DDR) is 160 MB/s *theoretical* but only **~40–84 MB/s sustained/contended** in practice (Espressif's external-RAM guide: DMA-to-PSRAM bandwidth "is very limited, especially when the core is trying to access external RAM at the same time"; PSRAM shares the flash cache region). So direct demand sits far under the floor (streams fine — proven), while shift 16-bit demand *exceeds* the ~40 MB/s contended floor and shift 8-bit sits inside the underrun zone once WiFi/HTTP/CPU cache traffic competes. Because WS2812 is one unbroken self-clocked stream, one FIFO underrun garbles the rest of the frame. This is **datasheet-consistent with**, and MEASURED to match controlled A/B (board B, same PSRAM/chain, only the clock varied: 2.67 MHz PSRAM drives, 26.67 MHz PSRAM never completes at any size) and the 2026-07-16 `forceRing` re-confirmation (whole-frame at 2880 stalls). **Proven:** the effect (PSRAM stalls at the shift clock, drives at the direct clock). **Not instrumented (needs a bench measurement if ever doubted):** the exact mechanism — contended-sustained-rate FIFO underrun vs PSRAM read latency vs cache/MMU contention — was inferred from the clock being the sole variable, never isolated with underrun/bandwidth counters.

**Conclusion — this does not open a new path; the proper ring fix already is the path.** The internal-RAM footprint of the ring is NOT set by light count: the ring transposes from a PSRAM-resident source into a small fixed internal buffer pool, so PSRAM is never on the DMA's read path at all. The 240-light wall is the `kRingBufs=16` no-reuse stopgap (the wrap read-while-write race), NOT the ring's design — and "more buffers" is a confirmed dead end. The shipped ring (above) holds internal RAM constant at arbitrary light count, which is exactly the "unlimited lights/strand" the PSRAM-hybrid idea was reaching for — obtained the correct way, at the mandatory shift clock, without PSRAM on the read path. **Action: none — the ring shipped; the "lower shift pclk + PSRAM whole-frame" hybrid is closed as physically blocked and should not be re-attempted.** (If the mechanism is ever contested, the one bench measurement worth doing is registering GDMA underrun/FIFO-empty counters at 26.67 MHz whole-frame-PSRAM to distinguish underrun from latency — but it would not change the conclusion.)

### RmtLedDriver's out-of-memory status has no test, and cannot have one yet (2026-09-09)

`frameUnusable_` reports a frame buffer the driver could not get (or, on classic, one that landed in
PSRAM where the refill cannot read it), and two guards keep that error from being overwritten by the
resting "driving N of N" status. All of it shipped unpinned, and not by oversight: the desktop
`allocInternal` is a flat `trackedAlloc` that never refuses and ignores `setTestMaxAllocBlock`, and
`ptrIsPsram` is hard-coded false. A test written against that would pass whatever the driver did.
`unit_JsonSink_overflow` shows the trap: it grows until a 64-bit host might refuse and accepts "no
refusal" as a pass, so it tests nothing on a machine with RAM.

**What it takes:** a cap inside the desktop `allocInternal` that `setTestMaxAllocBlock` (or a new
`setTestInternalCap`) drives, so a test can make the frame allocation fail deterministically, plus a
`setTestPsramFrom(ptr)` seam so the PSRAM branch is reachable off-target. Then three cases: the
status appears, `prepare()` does not replace it with "driving N of N", and `reinit()` does not clear
it (the bug the bench found: the error retracted microseconds after it was set). A platform change
with its own review, which is why it is here and not in the change that shipped the status.

### The ring's memory readout counts one buffer, not the pool (CodeRabbit, 2026-09-09)

`ParallelLedDriver::driverHeapBytes()` derives its DMA figure from `peripheral_->busCapacity()`,
which is `st->cap`: the size of ONE buffer. It then adds a second when `busBuffer(1)` exists, which
is right for the double-buffered path and wrong for the ring, where `createRingState` allocates
`ringBufs` slices (up to 30 at the 48x256 geometry) plus the shared zero-pad. So a ring board
under-reports its DMA memory by roughly the buffer count, on the card that exists to make exactly
that number visible.

Not a correctness bug: the memory is spent either way, and nothing sizes an allocation from this
value. It is a reporting bug in the one readout a user consults before adding lights.

**What it takes:** an aggregate size on the peripheral (the ring knows `ringBufs * bufBytes + pad`
at creation) rather than arithmetic in the driver, which cannot see the pool. Wants a ring-mode
accounting test alongside, since the existing ones only cover the non-ring path. Touching
`createRingState` means a bench pass on the wall, which is why this is its own change.

### MoonI80 ring — boot / first-frame-after-rebuild trips a transient give-up status (2026-07-16)

A cosmetic residual left after the rebuild-wedge fix (below): on boot, and for a beat after any shift-ring rebuild, the driver shows **"output stalled"** even though `wireUs` reports live completions — the *first* frame after a fresh ring build occasionally misses its completion window and trips the dead-frame give-up before the ring settles, so the stale error latches until the next interaction clears it. It is NOT the old wedge (that stayed dead until reboot; this self-clears on any control edit and the ring is genuinely driving underneath). Two clean fixes to weigh: (a) don't count the very first frame after a rebuild toward `deadFrames_` (give the fresh ring one grace frame), or (b) have the give-up retry re-derive status the moment `wireUs` shows a real completion. Low priority — the LEDs drive correctly; only the status string is briefly wrong.

### MoonI80 whole-frame async double-buffer corrupts output on the ESP32-S31 (2026-07-23)

`doubleBuffer` (default on) runs the async deferred-wait path (`tickAsync`): encode frame N+1 into buf[1] while the GDMA clocks frame N out of buf[0], costing `max(encode, wire)` per tick instead of `encode + wire`. On the **MoonI80** backend (our own GDMA below esp_lcd) this is a genuine speed win and is **clean on the S3 and P4** — bench-verified on a P4 driving a 64-light strip on Parlio-adjacent pins, and it's what lifted the P4 whole-board rate 48→76 fps. **On the ESP32-S31 it flickers** (corrupted/torn frames), on **any** pin — bench-isolated on GPIO 60 *and* GPIO 42, so it is NOT pin 60 (the onboard WS2812) and NOT the ring (the ring path is unreached here: expander off → `wantsRing()` false → whole-frame). Turning `doubleBuffer` OFF (single-buffer `tickSync`) is clean on the S31. So the fault is **chip-specific**: MoonI80's hand-rolled async alternation reprograms the GDMA link list per frame and relies on GDMA EOF-in-start-order semantics + a fixed DMA→FIFO settle delay (`esp_rom_delay_us` before `lcd_ll_start`), both tuned on S3/P4; the S31 is a newer RISC-V chip whose GDMA/LCD_CAM revision evidently differs (EOF timing or settle requirement), so the async path displays a still-in-flight or half-encoded buffer. **This is bench-verification territory** — do NOT "fix" it from code reading (the whole GDMA/ring subsystem is bench-proven and code-read corrections to it have been wrong before); it needs a logic-analyzer / GDMA-underrun-counter pass on the S31 to find the exact EOF-timing or settle-delay difference. **Decision (PO, 2026-07-23):** leave `doubleBuffer` default-on (it is a real, correct win on i80 / Parlio / MoonI80-on-S3/P4 — confirmed all three peripherals genuinely honor the second buffer, NOT i80-only), and backlog this. Two fixes to weigh when picked up: (a) **chip-gate** — MoonI80's whole-frame `busInit` declines buf[1] on the S31 only (`if constexpr (platform::isEsp32S31)` → falls to clean `tickSync`), preserving the async win everywhere it works and losing nothing on the S31 (async never worked there); or (b) **root-cause** the S31 GDMA timing so async works there too (recovers the S31 speed, more bench work). Workaround today: on the S31, MoonI80 with `doubleBuffer` OFF, or use `i80`/`Parlio` (both clean with double-buffer on the S31).

### Graceful blank-on-stall — a stalled bus should DARKEN the strip, not leave it lit with garbage (2026-07-15)

When the bus stalls mid-frame the WS2812 strip is left holding **random / max-brightness lights** (often all-white — the all-ones failure pattern) that only a **power cycle** clears. That is a robustness gap: WS2812s latch their last received color and hold it until re-clocked or power-cycled, so a frame that dies mid-stream leaves every light past the failure point stuck bright. The give-up guard today stops *spending the render thread* on a dead bus (correct) but does nothing about the *strip's* state, so the user sees a wall of garbage LEDs and reaches for the plug.

**The fix: on give-up (and on a rebuild that SHRINKS the reachable range), clock out ONE clean all-black frame** — every lane LOW → every light receives 0,0,0 → the strip goes dark. This turns "stall = a wall of random bright LEDs until power-cycle" into "stall = strip cleanly dark," which is the honest *degraded, not crashed* state the *[Robustness](../../explanation/architecture/moonmodule.md#robustness)* rule asks for. It also covers the PO's specific case (drop `ledsPerPin` 256→128 and the abandoned 128–256 range stays lit): a full-length black frame on the shrinking rebuild blacks the whole physical strip once, no boundary to compute.

**The load-bearing caveat:** if the bus is wedged *because it cannot complete a transfer*, a black frame may not clock out either — so this is a best-effort **attempt**, not a guarantee: try the black frame on give-up; if it clocks, the strip darkens; if the DMA is truly dead, we are no worse off than today (and the rebuild-wedge fix above is the real cure for *that* class). Note it must be a genuine transmitted frame (all lanes driven LOW through the normal encode+transmit), not merely zeroing the DMA buffer — the strip only changes on a clocked frame. Pin it with a test: after `kDeadFramesBeforeGiveUp` dead frames, the driver emits one all-zero frame through the transmit seam (the mock asserts a zero frame was handed to the bus), and a subsequent recovery resumes normal content.


### Classic-ESP32 shift-register ring on raw I2S — the high-light-count classic driver (WANTED)

**The PO wants shift-register output on classic ESP32 boards at the MAXIMUM light count**, which the current whole-frame `I80LedDriver` (esp_lcd i80 → I2S on classic) cannot deliver: its frame buffer must be **internal RAM** (the classic I2S DMA cannot read PSRAM — IDF rejects it outright), so it scales against a ~76 KB wall and caps at **~2048 lights**. Above that, classic needs the **hpwit refill-ring model**: a handful of tiny internal DMA buffers (~1.2 KB total, light-count-independent) refilled by the I2S EOF ISR, which transposes the next pixel row out of a **PSRAM** framebuffer as it goes. The DMA never touches the source array — only the CPU/ISR does — so the source lives in PSRAM and internal RAM stays constant. This is how hpwit (and WLED-MM/MoonLight on his driver) reach **48×256 ≈ 12K lights at ~100 fps on classic silicon with WiFi up**, with the same '595 expander.

**Why it must be a SEPARATE driver, not a flag on `I80LedDriver`:** `esp_lcd` owns the classic I2S DMA and only does whole-frame, so the ring cannot be bolted onto the esp_lcd path — it needs a second classic driver written on the **raw I2S registers** (`i2s_ll` / the LCD-mode register file directly, below esp_lcd, the way the S3/P4 MoonI80 backend sits below esp_lcd on LCD_CAM). Its ISR is small enough to fit the classic's ~70 KB IRAM (hpwit's does), and — the key move — it registers the interrupt **without** `ESP_INTR_FLAG_IRAM` (his source comment: removed "to avoid Cache Disabled but Cached Memory Region Accessed") so the refill ISR is legally permitted to read the PSRAM framebuffer. The trade it accepts vs. our whole-frame path: it **gives up the whole-frame path's WiFi-underrun immunity** (a WiFi burst that starves the refill ISR can glitch a frame), which the ring mitigates with a tunable buffer-count cushion (`nbDmaBuffer`, hpwit's default 6). For a ≤2K-light WiFi-busy install the whole-frame i80 is still the better choice; the ring is for the >2K-light case classic cannot otherwise reach.

**Reference (study, don't copy — write fresh against our architecture):** the line-by-line source read is in [led-driver-psram-ring-analysis.md](led-driver-psram-ring-analysis.md); the framing is [MoonI80Peripheral](../../moonmodules/light/moxygen/MoonI80Peripheral.md) (which calls the internal-RAM-ring-with-CPU-refill "the only thing that can ever work on the classic ESP32," deferred to a phase 2). The S3/P4 MoonI80 ring is the closest in-tree prior art for the ring mechanics (linear self-terminating chain, per-drain refill, drain-count termination) — but its refill is a task and its buffers are internal-only *because the LCD_CAM GDMA can't sustain a PSRAM read at the shift clock*; the classic I2S ring is the inverse (PSRAM framebuffer legal, ISR refill mandatory), so it borrows the *shape* but not the constraints. Do the S3/P4 **ISR-refill + `MM_HOT`** work first (it proves the ISR-refill pattern in-tree on the friendlier unified-DIRAM chips); the classic raw-I2S ring is the next tier up, reusing that pattern where IRAM is genuinely tight.

**Why MoonI80 cannot serve the classic, and what this driver inherits (2026-09-06).** `MoonI80`
is written against **LCD_CAM**: it drives the GDMA link list and the LCD registers directly
(`gdma_link_*`, `lcd_ll_*`) to bypass `esp_lcd`'s per-transaction peripheral reset. The
classic ESP32 has no LCD_CAM at all; its i80 is the **I2S** block in LCD mode, a different
peripheral with its own register file (`i2s_ll_*`) and its own DMA, so none of MoonI80's code
applies and `MoonLedDriver::lanesAvailable()` reports `platform::lcdLanes`, which is 0 there. The
picker hides the backend rather than gating it, which is why the classic has exactly one parallel
route today: `esp_lcd`'s I2S backend, whole-frame, internal-RAM-only, capped near 2048 lights.
This driver is the second route, and it stands in the same relation to `esp_lcd` on I2S as MoonI80
does on LCD_CAM: same shape, no shared code.

Two things it inherits from the 2026-09-06 i80 work, both worth keeping. It should claim **I2S
instance 1** and leave 0 for audio, for the reason recorded in the instance-split entry above
(instance 0 alone carries the PDM converters, nothing needs 1), and owning the peripheral directly
it can simply ASK for instance 1 rather than steering `esp_lcd` by parking instance 0, which is the
workaround the current backend needs. And it inherits the package-aware pin refusal: a pin the
package lacks wedges the flash cache silently, which cost a full day of bisection on the
ESP32-PICO-V3-02.

### P4 Parlio streaming ring — lift the P4 Parlio ceiling past ~21K to light-count-independent (WANTED)

**Port the ring concept to the P4 Parlio path**, to drive far more than its current whole-frame ceiling. troyhacks' MoonLight Parlio driver reaches **~21K LEDs RGB (~16K RGBW)** at 16 lanes — but NOT by materialising the whole encoded frame: he stages into a **fixed ~512 KB PSRAM buffer** and DMAs it out in **64 KB chunks** (`max_transfer_size = 65535`), so the DMA never needs the whole frame contiguous. That is the SAME idea as our MoonI80 ring, applied to Parlio on the P4 (where — unlike the S3 shift clock — the DMA *can* sustain PSRAM reads at the WS2812 rate). His ceiling is a *chosen buffer size*, not a hardware wall, so it caps at ~21K.

**Two tiers, in order:**
1. **Match him (moderate):** chunk our Parlio DMA the way he does (bounded PSRAM staging + 64 KB bursts) instead of one contiguous encoded frame — this alone lifts our P4 Parlio ceiling to his ~16–21K. Our Parlio path already uses PSRAM; the change is the chunked-DMA transfer shape, not a new architecture.
2. **Beat him (the ring):** run our MoonI80 **streaming ring** on P4 Parlio — small internal buffers, refilled per drain from a PSRAM source, holding internal RAM *constant* regardless of light count (the hpwit model our ring already implements). troyhacks' 512 KB buffer is still *bounded*; the ring is not, so this goes past ~21K to light-count-independent. It reuses the ring mechanics we already have (linear self-terminating chain, per-drain refill, drain-count termination); the P4 Parlio backend gets a ring variant beside its whole-frame path, the way MoonI80 has both.

**Prerequisite / sequencing:** tier 2 wants the **ISR-refill + `MM_HOT`** work done first (same as the classic ring above), since a high-rate Parlio refill benefits from the ISR-grade determinism. Confirm the CURRENT P4 Parlio tested ceiling first (re-measure on the P4 bench) before claiming a head-to-head — the ~21K figure is troyhacks' *buffer-fit* limit, not a verified-on-wire number, and his code steps the clock down at 256/512 LEDs per lane (signal integrity at long strands), which our own measurement must account for. Reference: the parlio source is `src/MoonLight/Nodes/Drivers/parlio.cpp` in MoonLight; our Parlio backend is `platform_esp32_parlio.cpp` + `ParlioLedDriver.h`.


### Extract shared lane-driver scaffolding when the 3rd parallel backend lands (deferred)

The `I80LedDriver` (classic-ESP32 I2S + S3/P4 LCD_CAM, both via the i80 bus) and `ParlioLedDriver` (P4 Parlio) share ~245 of 362 lines, and their platform-side loopback capture+verify is ~100 lines byte-for-byte identical (`platform_esp32_parlio.cpp` even notes "The RX capture half is byte-for-byte identical" to the i80 one). The status-string lifecycle (`failBuf_` / `configErr_` / `clearFailBuf` / `clearConfigErr`) is triplicated across all three LED drivers (RMT/i80/Parlio), ~60 lines. The branch deliberately extracted the *encoders* (`ParallelSlots.h` shared by i80+Parlio, `RmtSymbol.h`, `PinList.h`) on the "extract when the second user lands" rule, but stopped at the lifecycle/loopback scaffolding. **Accepted for this merge** (the reviewer agreed driver-level extraction can wait): the duplication is in mechanical lifecycle/test scaffolding, not domain logic, and a DriverBase-level refactor touching three drivers is riskier than the duplication it removes. **Do it when the third parallel backend arrives** (16-lane widening, or Teensy FlexIO), at which point the pattern is proven three ways: (a) a `detail::` platform helper for capture+verify (the only per-peripheral difference is the transmit call, pass a callback, beside the already-shared `loopbackJumperOk`), and (b) a small owned-status helper or DriverBase members for the fail/config strings. Until then the cost is line count, not correctness.

### Multicore Step 2b — ping-pong second output buffer (deferred, workload-gated)

The shipped render↔encode split (Step 2a, `multicore` control) uses one `Drivers::outputBuffer_`, so the composite is serialized at the boundary. A **second** output buffer would overlap even that (core 0 composites into B while core 1 encodes A), at the cost of one full frame buffer (~48 KB at 16K lights). The read-only `stall` KPI was built as the trigger metric, and it splits cleanly: a **heavy effect** (render-bound) shows `stall` ~1 µs (S3) / ~15 µs (classic) — 2b recovers *nothing*, core 0 already fills the encode window; a **light effect on many lights** (output-bound — a solid color or slow gradient across 16K lights) shows `stall` 6–11 ms — 2b recovers that whole wait. So it's a real but narrow win: **gated on the product owner naming that light-effect/many-lights workload as worth the 48 KB**, not on a generic fps gain. (Design context in the shipped Plan-20260713 - Multicore Step 2.)

### Frame pacing — decided against (record)

MoonLight targets a fixed 60 fps; MoonLight deliberately does not (settled with the PO 2026-07-12). The architecture is *render-uncapped + time-aware effects* (`beatsin8`/`millis()`-driven, a CLAUDE.md hard rule), so a whole-engine fps cap is redundant with that rule and would only *reduce* quality below the hardware ceiling; the LED wire rate already paces render physically (30 µs/light), and UI/WiFi responsiveness comes from the per-tick `vTaskDelay(1)` yield, not frame-rate control. Parked as a ~15-line opt-in (`targetFps=0` = unlimited default) *only if* a genuinely CPU-starved device ever appears.

### Brightness belongs on a fixture's DIMMER channel, not only in the color values (WANTED)

`Correction::apply()` holds a preset's `Dimmer` channel wide open at 255 and puts brightness into
the R/G/B/W values through `briLut`. That is correct output and it is the only rule that serves
every fixture (an addressable strip has no dimmer channel), but it is not how a lighting console
drives a fixture that HAS one, and it costs real quality:

- **Resolution.** At 10% brightness the colors are driven at 0-25, about 25 usable levels instead
  of 255. A slow fade to black steps visibly; the dimmer channel exists so colors keep full 8-bit
  range while intensity varies.
- **Dimming quality.** A moving head's dimmer is often 16-bit or curve-corrected for the LED's
  response. Linear scaling of 8-bit color values is the crudest dimming there is, and it is worst
  at the low end where the eye is most sensitive.
- **Matching.** A PAR with a real dimmer and a strip without one, both at 20%, do not read as the
  same brightness: one dims in the LED driver, the other in integer arithmetic.

**The rule to implement:** when a preset declares a `Dimmer` role, route the driver's brightness to
that channel and drive the colors at full saturation; keep today's behavior (scale the colors)
only where the fixture has no dimmer. The per-light color values still carry the effect's own
shading, so an effect that paints one light dark stays dark. Watch the one case where fixture and
light are not 1:1: several light cells behind ONE master dimmer cannot express per-cell brightness
through it, so those keep the color-scaling path.

Found on the bench 2026-08-28 wiring the first moving head. Note the dimmer was not written AT ALL
before that day (the shipped `IRGB` preset could never light a fixture); writing it at 255 is the
fix that made a fixture light, not the finished design.

### Blending adds motion channels, which is meaningless for aim (WANTED)

`blendMap` treats a light as `n` opaque bytes, so ADDITIVE blending sums pan and tilt across
layers. Two layers driving one moving head aim it at neither position, and saturate to hard-over
as soon as both are moderately positioned.

Only additive is wrong. **Opacity blending on motion is a feature and must be kept**: it is an
interpolation, so a layer fading in sweeps the head smoothly from the old aim to the new one,
which is what a console does and better than a snap. The rule (architecture.md) is that
interpolating ops are valid on every channel while accumulating ops are valid only on emissive
ones, so the fix is narrow: in the additive path, ASSIGN motion channels (topmost writer wins)
instead of summing them, leaving the opacity path alone.

Not yet reachable in a harmful way: it needs two enabled layers on a fixture carrying motion
channels, and the single-layer path is a memcpy. Worth doing with the motion-writer work, since
both need `FixtureChannels`' offsets on the blend path.

### Zoom, rotate and gobo have no writer (WANTED)

Pan and tilt now have a writer: an effect sets them through `EffectBase::setPan`/`setTilt` and
`Correction::apply()` maps the layer slots onto the fixture's channels (`MovingHeadEffect` is the
worked example, bench-driven). **`Zoom`, `Rotate` and `Gobo` still have none**: a preset can map
them and `Correction` carries their offsets, but no effect writes them, so they sit at 0.

This is the other half of driving a moving head, and it is a domain question, not a plumbing one: a
light is a point with a color, while a moving head is a fixture that emits a BEAM in a direction it
controls live. The backlog's fixture-model item ("moving heads, beams", per-emitter targets) is
where the model belongs; this entry records the concrete gap in the meantime. Bench fixture and its
channel map: [light fixtures reference](../../reference/hardware/light-fixtures.md).

### Pan/tilt travel is hardcoded, and positioning is 8-bit (WANTED)

Two fidelity gaps in how an aim becomes a real beam, both found on the bench 2026-08-29 reviewing
the preview's beam cones.

**Travel is assumed, not declared.** `EffectBase` and the preview both hardcode *540 degrees of
pan, 180 of tilt*, which is the common mid-size convention and happens to match the bench fixture.
Real heads vary: 630 pan on larger bodies, 360 or 530 on compact ones, 270 tilt on beam fixtures.
Nothing declares which fixture is plugged in, so the preview draws every rig with the bench head's
travel and silently lies about where a beam points. Zero point varies too (some fixtures put
mechanical zero at DMX 0 rather than centering at 128), and the axis direction is a setting on the
fixture's own display (`rPAN` / `rTIL` on the bench head), so even one fixture has no fixed mapping.

**The DMX byte is already the right abstraction, and it is why nothing can over-rotate.** An effect
never emits an angle: `MovingHeadEffect::axis()` maps its sweep onto a 0..255 byte and clamps, so
0 and 255 *are* the mechanical stops whatever they are in degrees, and the head cannot be asked
past its travel. A sweep that reaches a stop flattens there (the beam parks) rather than wrapping,
which is what the fixture itself does. Degrees exist only for DRAWING.

So the fix is scoped to the preview, and the open question is where travel is declared:

- **`PreviewDriver` controls** (`panTravel`, `tiltTravel` in degrees, sent in the aim message) —
  small, live-reconfigurable like every other setting, and honest that travel is a property of the
  fixture someone plugged in. The leanest thing that stops the lie.
- **The light preset** — correct once two different heads run at once, but a preset is a
  *channel-role* layout today, and carrying physical travel widens what a preset means. Belongs
  with the [fixture model](#fixture-model-moving-heads-beams-long-term), not before it.

**Positioning is 8-bit while the fixture offers 16.** The bench head has a fine channel for each
axis ([light fixtures reference](../../reference/hardware/light-fixtures.md)); both sit unused, so pan resolves
to 540/256 = about 2.1 degrees per step. Across a room that is a visible jump on a slow sweep, and
it is the bigger fidelity win of the two. Needs a 16-bit path from the effect's sweep through
`FixtureChannels` to the preset's fine-channel roles, so it is the larger job.

### Built-in light presets never reach a device that has a saved config (WANTED)

`LightPresetsModule` seeds its built-ins only when the preset list is empty
(`if (count_ == 0) seedBuiltins()`), and the persisted list replaces them wholesale. So a newly
shipped built-in preset appears only on a device that has never saved one: every existing device
keeps the older set forever. Adding the mini moving head's preset on 2026-08-28 needed a hand-patch
of the bench device's `Drivers.json`, which does not scale past one board.

**The fix:** merge by name at boot, seeding any built-in the saved list does not already carry, so
shipping a fixture preset reaches existing devices on their next update. A user's own presets and
their edits to a built-in must survive that merge untouched.

### ArtPoll discovery — know which tubes are alive (next increment on NetworkSendDriver)

`NetworkSendDriver` now unicasts to a list of receivers (`ips` + `lightsPerIp`), which is the Art-Net-4-conformant model. What it cannot do is **tell whether a receiver is actually there**: UDP is fire-and-forget, so a dead tube is invisible to the sender. The spec's own answer is discovery — *"The transmitting device must regularly ArtPoll the network to detect any change in devices which are subscribed"* — and it is the natural next increment.

**Why it earns its place (product-owner experience, 2026-07-13):** a dead tube in a prior setup *made the other tubes hiccup*. The mechanism is that a send to an unresolvable address stalls or errors **inside the frame loop**, delaying the packets for every destination after it — so one dark tube degrades the live ones, every frame. The send loop is now written to tolerate that (a failed `sendToAddr` drops that packet and moves on, and under `multicore` the whole send is off the render core), but tolerating is not the same as **knowing**: with discovery we can simply **skip destinations that haven't answered**, which removes the stall at its source rather than absorbing it.

**Scope (~130–150 lines + tests; the receive half already exists).** `ArtNetPacket.h` already builds+parses, `UdpSocket` already binds and reports the sender's IP:
- **ArtPoll send** — broadcast a 14-byte poll every ~3 s (this is the one packet Art-Net *does* broadcast, by design).
- **ArtPollReply parse** — fixed-layout 239-byte reply: node name, IP, and its subscribed universes.
- **A node table** — IP → last-seen; mark offline after ~9 s of silence (3 missed polls, the spec's own cadence).
- **Use it:** skip offline destinations in the send loop; surface the live/dead list as a read-only status. **Bonus, and arguably the real prize: auto-populate `ips`** — the user stops typing addresses at all, which is how a professional controller behaves.

Do it as its own increment. The multi-destination unicast it builds on has shipped.

### RS-485 / DMX-512 wired output (future) — the physical-DMX driver

MoonLight already speaks DMX **over the network** (Art-Net / sACN via `NetworkReceiveEffect`). The missing half is **wired DMX-512 out**: driving DMX fixtures (moving heads, par cans, wired pixel controllers) directly over an RS-485 differential pair, which is what the RS-485 hardware on carrier boards like the [MHC-WLED ESP32-P4 shield](../../reference/hardware/mhc-wled-esp32-p4-shield.md) is *for*. DMX-512 is a 250 kbps async serial frame (a break + mark-after-break + 513 bytes: start code + 512 channels) shipped over RS-485 — the textbook fixture-control transport. A DMX driver would map the light buffer (or a fixture/attribute model — see the [Fixture model — moving heads, beams](#fixture-model-moving-heads-beams-long-term) item below) to DMX channels and clock the frame out a UART in RS-485 mode.

**What it needs that we don't have yet:**
- **A `platform::` UART-RS485 seam.** The ESP32 UART has a hardware RS-485 half-duplex mode (`uart_set_mode(UART_MODE_RS485_HALF_DUPLEX)`) that auto-drives the transceiver's **DE/RE** (driver-enable / receiver-enable) line — the thing our current pin handling has no concept of (we drive pins as plain GPIO). A DMX driver is where DE/RE control first earns its place, and only for a **bidirectional** channel: firmware DE/RE toggling is what lets one channel switch Tx↔Rx without a hardware switch. A **fixed-transmit** channel needs none — its transceiver is hard-wired to drive. On the [MHC-WLED ESP32-P4 shield](../../reference/hardware/mhc-wled-esp32-p4-shield.md) that split is physical: GPIO 4, 22, 24 are fixed-transmit (no DE/RE control wanted), and only the switchable GPIO 3 channel is bidirectional — the shield handles it with a *mechanical* slide switch (which is how its loopback works). Firmware DE/RE control is what a board would need to make a channel bidirectional *without* such a switch.
- **The DMX frame timing** — the break/MAB is generated by a baud-rate switch or a GPIO toggle around the UART frame; standard, host-testable as an encoder.
- **A fixture/channel-mapping model** — trivial for a dumb pixel-per-channel strip, real work for typed fixtures (pairs with the moving-head fixture-model item; a wired-DMX driver and a network-DMX(Art-Net) input would share that fixture model).

**The channel-mapping half is now unblocked.** The per-light encode path handles an arbitrary channel count as of 2026-07-13 (the WS2812 drivers' per-light scratch is heap-sized to `outChannels`, no fixed cap — the fix from the multi-channel-preset bootloop, see [lessons.md](../../work/past/lessons.md)). A DMX universe is exactly that model: a light with `channelsPerLight = <fixture footprint>` (16-ch moving head, 7-ch par, …), and the buffer's bytes ARE the DMX channel values. So a DMX driver's "map the buffer to channels" step is now the trivial part — it ships the light buffer's bytes straight into the 512-channel frame. What remains genuinely new is the **transport** (the RS-485 UART seam + break/MAB timing) and the **typed-fixture model** (naming which channel is Pan vs Dimmer — the moving-head fixture item), not the encode.

**Can a board drive XLR fixtures directly? Yes, with an RS-485 transceiver — that's the one required part.** DMX-512 is RS-485: a *differential* pair (D+/D−, ±2–6 V), not the 3.3 V single-ended UART the MCU emits, so an MCU TX pin can NOT wire straight to XLR. A transceiver chip (MAX485 / SN75176 / THVD-class, ~$0.50) sits between the UART and the connector and drives the differential pair; the DE/RE line (the UART-RS485 seam above) flips it Tx↔Rx. **3-pin XLR** carries it: pin 1 = ground, pin 2 = D−, pin 3 = D+. With a transceiver present, daisy-chaining ~10 moving heads (10 × 16 ch = 160, inside one 512-channel universe) over standard DMX in→out is well within the RS-485 limits (32 unit loads / 1200 m); the last fixture wants a 120 Ω terminator (a fixture/cable concern, not the MCU). So whether a catalog board can drive XLR *directly* hinges on one schematic question: does it carry an RS-485 transceiver + XLR/terminal (then yes, direct), or only the WS2812 level-shifted outputs (then a ~$0.50 breakout is needed). Confirm against the [MHC-WLED ESP32-P4 shield](../../reference/hardware/mhc-wled-esp32-p4-shield.md) schematic before treating direct-XLR as a shipping capability.

Sequencing: it's a **driver** (`src/light/drivers/`) + a platform UART-RS485 seam + a fixture model shared with the Art-Net path — the buffer→channel encode is already done. Plan when a DMX fixture is actually on the bench and a catalog board's `supported`/`planned` list points at wired DMX. The [PinsModule pin-assignment work](backlog-core.md#pinsmodule-strict-reject-on-add-mode-the-one-remaining-increment) covers the RS485/DMX TX/RX/DE slot; this is the driver that consumes it.

## Integration with other LED and visuals tools

Distilled from a Discord thread with panel-card users (2026-08-24), where two people drove ColorLight walls from MoonLight and described the pipelines they already run.

### Preview does not resume after a long tab hibernation (observed once, 2026-08-27)

Desktop instance at 1024x1024: after the browser tab sat backgrounded for about an hour, the
preview pane stayed static on return. PreviewDriver ticked at ~1 us (no standing request served)
while a FRESH WebSocket client received frames normally, and the Drivers card showed the
encode-worker-stalled latch. A page refresh reportedly did NOT revive it; toggling `multicore`
(a Drivers re-prepare) did. Suspects: the tab-hide hibernation path's tracked retry vs the
wake-up re-request, or per-driver lease state that only prepare() resets. Needs a reproduction
with the WS uplink logged before it can be fixed.

### One encode, two readers: HLS and RTSP sharing the encoder (2026-09-22)

Both video drivers call `platform::encoderStart`, and the encoder is a single instance on every target: one ffmpeg child on the desktop, one hardware session on the P4. The second caller used to reconfigure the first's stream silently, so HLS kept serving at RTSP's geometry and bitrate without saying so. **Shipped instead: a claim**, so the second driver is refused with a visible status, the way a driver already reports a port another module holds. That makes the conflict impossible rather than unlikely, and it costs a guard.

The better end state is the one the RTSP plan describes and the P4 already half-implements: **one encode, two readers**. `rtspTakeFrame` and `hlsSegment` read the same encoded frame today, so a device serving both would encode once and each reader would take what it needs, which is cheaper than either driver running alone twice over.

What makes it a design task rather than a patch: the two drivers must **agree** on geometry, fps and bitrate, and nothing makes them agree now. HLS derives its bitrate from the grid and picks an encoder by name; RTSP takes the frames raw and names no encoder. The open questions are whose configuration wins when both are enabled, what happens when one changes `targetFps` mid-stream, and whether a reader that stops should relax the shared settings. A refcount alone answers none of those, which is why the claim ships first.

Build trigger: someone wanting both streams from one device at once. Until then the claim is the honest behavior, and the refusal message names the other driver.

### HLS upscaling is cache-hostile on large walls (measured, 2026-08-28)

`HlsDriver`'s `scale` control replicates each light into a scale x scale block. Measured on the
bench P4 at 128x128: **~1 ms per frame at 1:1, ~60 ms at scale 4** (a 512x512 output). The frame
is 16x larger, so ~16 ms would be the honest cost; the extra 4x is the access pattern. The loop
walks LIGHTS and writes each block as `scale` separate short rows scattered across a 786 KB
buffer, so consecutive lights touch distant addresses and every write misses cache, where the
1:1 path writes straight through sequentially.

**Not urgent, because the default path never hits it:** auto-scale only engages on walls below
the encoder's 80-pixel floor, where the output is small by construction (a 20x10 wall becomes
160x80, 38 KB). The expensive case is a manual scale on an already-large wall, which is also
where upscaling has the least to offer.

**The fix when it earns its place:** iterate the OUTPUT rows rather than the input lights, so
writes are sequential: for each output row, walk its source row once and emit `scale` copies of
each light's color, then `memcpy` that finished row to the remaining `scale - 1` rows of the
block. Same output, one pass through the destination in address order.

### Sprite follow-ups (draw::sprite + FlyingToasters shipped; spec + plan in the plans archive)

Deliberately deferred when sprites landed: P4 PPA acceleration behind the same `draw::sprite`
signature (the 2D-DMA blitter the WLED-MM-P4 world uses via LovyanGFX; ours would sit in the
platform layer, no vendored GFX library), Porter-Duff alpha when a real consumer arrives, and
MoonLive sprite data (needs the stage-3 builtin table + arrays).

### MoonLight as a video source — NDI first, Spout/Syphon only if proven (open)

Users asked for MoonLight's rendered output to feed *their* tools, not the other way round. One runs OBS → Spout → his own VLAN-tagged card driver; he asked whether MoonLight could be a Spout source. Input is not the gap: `NetworkReceiveEffect` already binds Art-Net, E1.31/sACN and DDP at once and answers ArtPoll, so any controller can already drive MoonLight.

**NDI is the recommended first implementation.** It is the AV industry's standard for video over IP, one implementation covers Windows, macOS, Linux and ARM, it discovers by name, and it crosses machines. Spout (Windows, DirectX/OpenGL) and Syphon (macOS, Metal/OpenGL) share a GPU texture zero-copy, so they are lower latency and bit-exact, but they are **same-machine only**, are **two** platform implementations, and leave **Linux and the Pi with nothing**. At LED-wall pixel counts (a 256x256 wall is 65K pixels) the latency difference is far below one frame of the render loop, so it does not decide the choice; coverage does. A Spout user is also reachable through NDI in one hop, since OBS, Resolume and TouchDesigner all speak both.

**The licence shapes the design, and the shape is already established here.** MoonLight is GPL-3.0 and the NDI runtime is proprietary, so MoonLight must not *redistribute* it: bundling would require MoonLight's own licence to carry NDI's restrictions downstream, which GPL-3 forbids. The user installs the NDI runtime themselves, exactly as they already install **Npcap** for the panel-card driver, and MoonLight calls whatever is present.

That is the arrangement `platform_desktop.cpp` uses for Npcap today: resolve the library with `LoadLibrary`/`dlopen` rather than linking it, declare the handful of functions with the library's own signatures rather than including its headers (so the SDK never becomes a build requirement for CI or contributors), and report the feature unavailable when it is absent instead of failing to link. Two independent installs that talk to each other, like Resolume on the same desktop.

Also note MoonLight renders into a CPU buffer, so a Spout/Syphon path would upload to the GPU purely to hand off, spending the zero-copy advantage it was chosen for.

### M5Stack Tab5 as a display target — MIPI-DSI, not the H.264 path (open)

The Tab5 is an ESP32-P4 with a 1280x720 MIPI-DSI panel, and a P4 is already a supported target, so
the question is what its *screen* would show. The P4's H.264 block does not answer it: that encoder
exists to compress an incoming MIPI-CSI camera feed, and the P4 has no hardware H.264 **decoder**
at all (Espressif's own FAQ points at software decode, which will not hold 720p). Driving the panel
is the **MIPI-DSI** peripheral plus the PPA / 2D-DMA blitter, which take raw pixels and never touch
a codec. So the three things a Tab5 could be are separate pieces of work, and only the first is free:

- **An HLS source**, like any other P4: it encodes its own rendered grid and streams to a TV. Its
  panel is incidental, and this needs nothing beyond the P4 HLS work itself.
- **A local wall preview or touch console** — the interesting one, and the real ask: a `platform::`
  MIPI-DSI display seam plus a UI on the panel. Related to the PPA acceleration noted under sprite
  follow-ups above (same 2D-DMA block), and it is a display *output* seam MoonLight does not have
  today; the nearest prior art is the WLED-MM-P4 world's LovyanGFX usage, which we would not vendor.
- **An HLS/video player**, showing another device's stream: blocked on the missing hardware decoder,
  so not worth planning.

### Multi-card walls — does a daisy chain work today? (open, ask before building)

The ColorLight format has **no card addressing in the PIXEL path**: the destination MAC is a fixed
constant and every card filters on it, so every card on a segment shows the same image. A user with
six cards on a switch observed exactly that.

The DISCOVERY path does distinguish them. A discovery reply (0x08) carries a controller number at
payload offset 0x62, and the acknowledgement echoes it plus one, which is how a sender tells several
cards apart. Documented by a reader of [Harald Kubota's protocol
write-up](https://hkubota.wordpress.com/2022/01/31/winter-project-colorlight-5a-75b-protocol/) and
confirmed by its author. That is an identity, not a destination: it does not let a sender aim pixel
data at one card, so the same-image behavior above stands. It is what a per-card brightness or
color-temperature feature below would key on.

The industry-standard answer is **daisy-chaining** — a sending card's ports each drive a chain, and each card takes its region by position in the chain. That user works around it with per-card VLANs and a managed switch instead, which he built for throughput and for per-card color-temperature grouping across mixed panel batches; he described it as his own solution, not a standard.

**Establish first whether a daisy chain already works with MoonLight** (one contact has a 96K daisy-chained rig). If the cards self-assign by chain position, the standard multi-card case is already solved and nothing is needed. Only if it does not work is there a feature here, and it should follow the daisy-chain standard rather than the VLAN workaround. 802.1Q tagging is technically a clean fit for a raw-L2 sender (the tag is part of the Ethernet header, the switch strips it before the card, so card firmware is unaffected), but it serves one bespoke architecture.

### Smaller asks from the same thread

- **Read the wall layout from the ColorLight cards.** The cards can report their configuration and at least one user's own tool already does it; it would remove the manual layout step.
- **Per-card color temperature and brightness**, via the ColorLight sync-packet bytes, grouped by sync group — used to color-match mixed panel batches live.
- **Docker image**, asked for by a user tracking updates in an IoT system. The Linux binary and `.deb` already ship, so this is packaging rather than new capability.

## Sensors and audio-reactive input

### The sensors an installation needs (2026-09-01)

**Why this is a commitment rather than a wish list.** One of the intended uses is art installations,
and an installation people can interact with has to sense them: that is stated in
[the architecture](../../explanation/architecture/index.md#the-problem). Sensing is therefore part of the product, not a
convenience, and an input peripheral is first-class alongside an output driver. The scope is still
narrow on purpose: a lighting controller that senses its audience, not a home-automation platform.

What shipped, and what a piece can already react to: **audio** (AudioService: RMS level and a 16-band
FFT, I2S mic or line-in, or a peer's stream over the network), **IR** (IrService, a learnable
remote), and **a button** (ButtonService on the GPIO seam, which covers foot pedals in momentary
mode). An **IMU** exists on an unmerged branch (GyroDriver, below).

The sensors worth having next, in the order an installation asks for them:

- **Motion / presence (PIR)**: the single most common interactive trigger: someone walks up, the
  piece wakes. Electrically a digital pin, so `ButtonService` almost covers it; what differs is the
  semantic (a level, not a press) and the hold time. MoonLight models it as its own pin role
  (`pin_PIR`, "HIGH = lights on, LOW = lights off"), which is the shape to follow.
- **Distance (ultrasonic HC-SR04, or a ToF like the VL53L0X)**: turns presence into a continuous
  value: a hand's distance drives brightness, a visitor's approach drives an effect parameter. The
  ToF is I2C, which today means SCANNING only: the register-level read and write a sensor needs is
  the unmerged GyroDriver prerequisite (see input-mapping-analysis.md and the scripted-sensors
  plan), not something that exists. The ultrasonic is a trigger pulse and an echo-width measure,
  which needs a timing seam the GPIO one does not yet provide.
- **Touch**: the classic ESP32's capacitive touch pins need no external part, so a conductive
  surface becomes an input. The Dig-2-Go's own button is on a touch-capable pin, though it reads
  fine as a plain digital switch.
- **Light level (LDR or a BH1750)**: an installation that dims itself to the room, and the obvious
  companion to a piece that runs day and night.
- **Rotary encoder**: the physical knob for an installation without a screen, and the one input
  that maps onto ControlModule's encoder bank directly.

**None of these need new architecture**, which is the point of recording them together: each is a
Service under the core `Services` container. How it reaches the rest of the system depends on what
it produces. A THRESHOLD is an event ("closer than 50 cm"), and an event drives a control through
`Scheduler::setControl`, exactly as the button and infrared rows do. A continuous VALUE is not an
event: an effect reading a distance per frame needs the number, and pushing one through a control
per sample would serialize a stream through a settings path. That is published as a shared frame
the way `AudioService::latestFrame()` already does, and a sensor typically does both. What they need is a
platform seam per sensing modality (the GPIO seam shipped; I2C exists; a pulse-timing seam does
not), and a module each.

### Read a vehicle's CAN bus and drive lights from it (2026-09-11)

A MoonLight device in a car, reading the vehicle's own bus and driving aftermarket lighting from what it sees: underglow that pulses with engine RPM, sweeps with the indicator, flares on the brake. The vehicle to develop against is a VW Transporter (the product owner's).

**Read-only, and that is the design, not a limitation.** Writing to a vehicle's bus reaches systems that are mandated safety equipment (indicators and brake lights are UNECE R48), usually on a segment shared with ABS and airbags, and modifying them is illegal in most jurisdictions. Reading has none of that exposure and gets the interesting half anyway: the car reports its state, MoonLight renders it. Anything the device drives is aftermarket lighting on its own output, untouched by the vehicle.

**A Service, not a driver.** It senses rather than renders, so it belongs under the core `Services` container beside AudioService and ButtonService, and it produces both shapes the sensors entry above describes: an EVENT (indicator on, brake pressed) drives a control through `Scheduler::setControl`, while a continuous VALUE (RPM, speed) is published as a shared frame the way `AudioService::latestFrame()` is. Effects then read it, the input-mapping layer routes it, and a MoonLive script reads it as a system variable, so "underglow sweeps with the indicator" is a script someone writes and shares rather than firmware.

**Two tiers, and only the first is write-once.**

| | Standard | What it yields | How |
|---|---|---|---|
| OBD-II / UDS | Yes, legally mandated (EU: 2001 petrol, 2004 diesel) | RPM, speed, coolant, throttle, engine load | Request/response against ID `0x7DF`, so polling, a few times a second |
| Manufacturer body frames | No, proprietary per model and year | Indicators, brakes, doors, lights | Passive sniffing and reverse engineering |

J1939 is NOT the answer here despite being the "CAN lighting standard" people reach for: it is the heavy-duty protocol (trucks, agricultural, marine), and a Transporter is passenger-car architecture running VW's own frames. J1939 would only apply if MoonLight targeted commercial vehicle equipment.

So phase 1 is OBD-II only, which genuinely works on any car and needs no per-vehicle data. Phase 2 adds raw-frame sniffing, and there the frame IDs must be CONFIGURATION rather than compiled-in constants: a Transporter owner who discovers theirs shares a config file, and no firmware release is involved. That is what keeps a per-vehicle feature from becoming a per-vehicle maintenance burden.

**What it needs:**

- A `platform::twai*` seam (init, send, receive, stop). TWAI is the ESP32's CAN controller and is present on every chip MoonLight ships: 1 controller on classic and S3, 3 on the P4.
- **An external transceiver**: the ESP32 has the controller but no CAN PHY, and this is the only part that needs buying. An SN65HVD230 board (3.3V native, unlike the 5V TJA1050) plus an OBD-II pigtail to reach pins 6 (CAN-H) and 14 (CAN-L), so nothing on the vehicle is cut. Watch for a termination resistor fitted on the transceiver board: the vehicle bus is already terminated at both ends and a third resistor disturbs it. Power the device from USB while developing rather than OBD pin 16, so a crash cannot load the vehicle supply.
- A `VehicleService` module, and a desktop stub so the logic is testable without a car.

**Phase 1 is already confirmed on the van**: an ELM327 running Torque pulls PIDs from it today, so the OBD port is live, CAN is reachable there, and the standard request/response layer answers. Nothing further needs discovering before building the seam.

**Phase 2 is the open question, and one free test decides it.** Torque and ELM327 adapters speak the FILTERED request/response layer: you ask for a PID and the ECU replies, which says nothing about what else is on the wire. Torque itself can do the test: it has a dedicated `canbus monitor` protocol (selected in the vehicle profile, beside a hybrid mode that polls OBD2 and monitors at once) and imports DBC files in the PID editor to decode signals out of the stream. Traffic beyond one's own polled PIDs means the body frames are reachable on that port; only one's own requests and replies coming back means they sit on another segment behind a gateway, so phase 2 needs a different tap point. **The constraint is the adapter, not the app**: monitor-all (`AT MA`) is in the genuine ELM327 specification and many clones omit it outright, which makes them fine for standard PIDs and useless for reverse engineering. An ELM327 identifier app probes which AT commands a given adapter actually recognizes, so that is the thing to check first. Worth the five minutes before designing anything for tier 2.

**Prior art, checked 2026-09-11.** The building blocks are all open source and the closest match is [can_wizard](https://github.com/okhsunrog/can_wizard) (MIT): an ESP32-C3 plus SN65HVD230 sniffer on ESP-IDF's TWAI driver, with hardware acceptance filters, live bus state, error counters and bus-off recovery, which is close to the seam described above. It uses the legacy `driver/twai.h`, deprecated in IDF 6.x, so the new `esp_twai.h` is what to write against. Also useful: [esp32-slcan](https://github.com/mintynet/esp32-slcan) exposes an ESP32 as a standard slcan device so desktop CAN tooling works against it, and [an Alfa Romeo dashboard project](https://github.com/ClaudeMarais/AlfaRomeoGiulia_DashboardInfo_ESP32-S3) runs an ESP32-S3 on two CAN buses at once in a real car. Driving LEDs from OBD2 RPM is well-trodden ground, which is another way of saying tier 1 carries little risk.

**What is NOT available, and it shapes the design.** [awesome-automotive-can-id](https://github.com/iDoka/awesome-automotive-can-id), the main community collection, carries VW data for the MQB platform, Golf, Polo, Touran and e-Up, but **nothing for the Transporter and no lighting signals for any VW**: its coverage is drivetrain and engine management, not body control. So tier 2 starts from an empty database however much searching precedes it, which is exactly why the frame IDs belong in configuration a user discovers and shares rather than in a compiled-in table. (T6 owner forums do report the reverse signal travelling over CAN, so the body signals exist on a bus; they are simply not publicly documented.)

**Related:** a convoy light show across several cars needs nothing from this. That is MoonSync (planned, not built) synchronizing timebase and effect state between devices, with each car rendering locally through whatever driver it has. The two compose for free and neither blocks the other.

### Audio-reactive follow-ups

The manual level + 16-band FFT spectrum has shipped (AudioService; what landed and why is in [lessons.md](../../work/past/lessons.md)). These are the deferred follow-ups, each its own increment:

- **Adaptive conditioning** — auto noise-floor / auto-gain / smoothing so the display self-calibrates to a room ("sound off → dark, sound on → vivid") instead of being tuned by hand. A self-calibrating version was prototyped and removed; the manual `floor`/`gain` is the shipped baseline. Reinvent from scratch when wanted, and **tune it in a quiet room** — a noisy environment (a strong, varying low-frequency ambient) is the adversarial case that made the prototype hard to settle. (The per-band floor above is the first piece of this.)
- **Adaptive noise gate** — replace the borrowed `squelch`/`floor`-as-gate with a real noise gate: asymmetric bang-bang timing (open fast, close slow), a relative "detect silence" test (thresholds as factors of a learned floor, not absolute sample counts), keying off the RMS envelope we already compute, GEQ/FFT bands left untouched. A softhack007 concept; analysed and judged in full (good idea, industry-standard, but tight on the <30ms budget; decompose into steps rather than overhaul) in AudioService.md § Adaptive noise gate. The recommended sequencing: the per-band floor above is step 1 (its complementary frequency-domain half), the relative-threshold-over-RMS is the cheap high-value cherry-pick as step 2, hysteresis/timing step 3, log-domain + soft-gate optional. Eventually retires the manual squelch.
- **Pin auto-scan** — detect the mic's `sdPin` with `wsPin`/`sckPin` fixed (a noise-prompt + confirm convenience); ships today with explicit pin controls.
- **Beat / onset detection** beyond the raw peak; more audio effects (2D / palette-driven frequency-reactive).

### GyroDriver → core Service move + AudioService-consistency pass (branched, not merged)

A working **GyroDriver** (MPU6050 IMU over I²C) exists on an unmerged branch (commit `11f8eb7`, "Add GyroDriver (MPU6050) + generic platform I2C layer"); it is not in this branch's tree. This entry reverse-engineers that commit so the move is tracked now. **Verify against the real implementation when the branch merges, then delete this entry.**

What the commit contains (reverse-engineered):

- `src/light/drivers/GyroDriver.h` — reads an MPU6050 over I²C and surfaces five read-only telemetry controls (`gyroX`/`gyroY`/`gyroZ` rates in °/s, `pitch`/`roll` tilt angles). Polls the sensor in `loop20ms()` (50 Hz), formats the display strings in `loop1s()`. WHO_AM_I probe + wake on `setup()`, big-endian 14-byte burst parse, `atan2`-based tilt (no fusion filter).
- A **generic, domain-neutral platform I²C master** (`platform::i2cInit`/`i2cWriteReg`/`i2cReadRegs`, 7-bit addressing) so future sensors reuse it; ESP32 impl on the IDF v6 `i2c_master` driver in a new `platform_esp32_i2c.cpp`, plus an MPU6050-shaped desktop simulation so the UI and host tests see live values without hardware.
- `unit_GyroDriver.cpp` — WHO_AM_I probe, simulated burst parse, control formatting, time-ramp tracking.

The move: it currently masquerades as an input-only **driver** under the Drivers container (a no-op `setSourceBuffer(Buffer*) override {}` is the tell). It belongs as a **Service** — a user-added child of the `Services` container, exactly like AudioService — both are sensor services that poll hardware and publish read-only telemetry. On the move, make it consistent with AudioService (the established sibling pattern):

- **Relocate** `src/light/drivers/GyroDriver.h` → `src/core/` and its spec `docs/moonmodules/light/drivers/GyroDriver.md` → `docs/moonmodules/core/`; change `role()` to `Service`; delete the `setSourceBuffer` no-op; rewrite the doc's "input-only driver under the Drivers container" framing. (Rename to `GyroService` too, to match the `AudioService`/`IrService` convention — a Service names itself by its subcategory.)
- **Pin controls + rebuild path.** GyroDriver hardcodes SDA/SCL (`static constexpr` 21/22, with its own "Hardcoded until BoardModule exposes I2C pin mapping" comment). AudioService already shows the pattern: editable `uint16` pin controls + `controlChangeTriggersBuildState` + a `reinit()` on `onBuildState`. Adopting it retires the hardcoded-pins TODO and satisfies the robustness rule (reconfigure in any order).
- **Lifecycle.** GyroDriver has `setup()` only — no `teardown()`. Add teardown for symmetry with AudioService's setup/teardown/reinit (the shared I²C bus has little per-instance state to free, so this is consistency, not a leak fix).
- **Document the cadence difference.** GyroDriver polls in `loop20ms()` (50 Hz is plenty for tilt); AudioService reads in `loop()` every tick because I²S DMA must be drained promptly or it overflows. Both are correct; add a one-line "why this cadence" comment at each so the two siblings aren't "harmonised" into a bug.
- **Add it** to the deviceModel catalog under `parent_id: "Services"` (user-added per board), the same shape as AudioService — a gyro sensor is optional per board, so it's a Service, not a wired-by-code System child.

Already done on this branch (the reverse direction): AudioService's two live read-outs were switched from `addText`+`setReadOnly` to `addReadOnly` (the display-only type, matching SystemModule and the way GyroDriver already does it correctly) — so the telemetry idiom is consistent before the gyro branch even lands.

### Sensor input on Raspberry Pi 5 — microphone, IMU, line-in (post-1.0, multi-commit)

Audio-reactive lighting (and motion-reactive) is core to what WLED-MM / MoonLight are known for. The Pi 5 is the right host for it: it has the CPU and RAM for real FFT-based audio analysis that the Xtensa ESP32 struggles with, and a full Linux audio + I²C stack. None of this exists today — the codebase has no sensor, audio, or IMU concept, and the Pi currently runs the **desktop** platform backend (there is no `src/platform/rpi/`), which has no hardware access. So this is a domain expansion built on a real platform-backend prerequisite, not a small add.

**Target sensors and their Pi 5 interfaces:**

- **Microphone** — I²S MEMS mic, or a USB audio device read via ALSA. The high-value one: FFT → frequency bands + beat detection drive audio-reactive effects.
- **Line-in** — the Pi 5 has no native analog input, so this is a USB audio interface / DAC HAT feeding the same audio pipeline as the mic; only the source differs.
- **IMU / gyro** — an I²C device (MPU-6050 / 9250-class) on the Pi's I²C bus; tilt / motion → effect parameters.

**How it fits the architecture (the load-bearing part):**

1. **The module category exists — `ModuleRole::Service`.** Services are user-add/deletable children of the `Services` container (a gyro `Service` lands there via the GyroDriver→core move). What's missing for audio-reactive is the *consumption* side: a sensor reads hardware and *produces* values (audio bands, IMU axes) that effects consume — the producer side of the [producer/consumer data-exchange model](../../explanation/architecture/moonmodule.md#data-exchange-between-modules) (a sensor produces an `AudioFrame` / `ImuState` the way effects produce a buffer that drivers consume). Define the producer struct domain-neutrally so it isn't audio-specific. Today's services are display-only; wiring them into effects is the new work.
2. **All hardware access stays behind the platform boundary.** New `platform::` APIs (e.g. `readAudio()` returning PCM/FFT, `readImu()` returning axes) with the ALSA / I²S / I²C implementation in a real `src/platform/rpi/` backend — which is itself the prerequisite that doesn't exist yet (the Pi uses the desktop backend today). No ALSA/I²C include or call outside `src/platform/`.
3. **Effects consume sensor data the same way they read the layer.** An audio-reactive effect reads the current `AudioFrame` (bands/level/beat) the way `PreviewDriver` reads what `Layer` produces — through a plain data structure wired in `main.cpp`, not a direct hardware call.

**Increments (each a normal domain addition, picked up one at a time):**

1. A real `src/platform/rpi/` hardware backend (GPIO/I²C/I²S/ALSA) — the prerequisite; until it lands, the Pi runs the desktop backend with no sensors.
2. The producer struct(s) (`AudioFrame` / `ImuState`) + the `platform::read*` APIs. (The `Service` role + Services-container add/delete already exist.)
3. The first audio service — **MicrophoneModule** (canonical, highest value: FFT bands + beat).
4. The first audio-reactive effect(s) consuming it.
5. IMU and line-in slot into the same source-module + platform-API shape afterwards.

Study the proven audio pipeline in MoonLight / WLED-MM (FFT band layout, AGC, beat detection) to inform our own — reference the approach, don't port their code, per [history](../past/index.md) practice. Specs before code: a `MicrophoneModule.md` (and the source-category contract) get written and reviewed before implementation.

## Effects and preview

### DemoReel: extrude hosted lower-D effects (pending)

DemoReel hosts one effect at a time and drives its `loop()` directly. A hosted **D1/D2** effect only writes its own slice (D1 → the x=0 column; D2 → the z=0 plane), and — unlike a normal Layer child — it does NOT get `Layer::extrude()` applied, so on a 2D/3D grid its output stays on one column/slice instead of spreading. A first fix (call `layer()->extrude(child->dimensions())` after the child's `loop()`, mirroring `Layer::loop`) **crashed the test suite with a heap/vtable smash** — `Layer::extrude` copies within `buffer_` using the Layer's `width_/height_/depth_`, and something in the reel's host path leaves those out of sync with the allocated buffer (root-cause not yet pinned). Redo it carefully: verify the Layer's dims match its buffer at the extrude call, add a bounds guard in `extrude` (or a reel-local extrude that reads the child's real dims/buffer), and pin it with a DemoReel test that hosts a D1 and a D2 effect on a 3D grid and checks the spread. Until then the reel renders D1/D2 hosts on a single slice (visible but not full-grid) — acceptable, not a crash.

### BlurzEffect — a compounding blur has no rate carry (open)

`Layer::fadeToBlackBy` takes a rate per reference frame and the Layer scales it once, the pattern every fade uses ([architecture § Where the machinery lives](../../explanation/architecture/index.md)). `draw::blur` does not fit it: blur COMPOUNDS, so applying it twice at half strength is not one blur at full strength, and the fractional carry that makes a fade frame-rate independent produces the wrong result. BlurzEffect is the effect this bites. The open question is what the right construct is — a per-frame blur budget, a single blur at an accumulated strength, or leaving blur explicitly frame-gated and documenting it as not-a-rate.

### A real 2D/3D PacMan (pending)

The migrated 1D PacMan and 1D Ant effects were removed — a chase rendered on a single strip reads as a blob of moving dots, not a game, so they weren't worth keeping. A proper **2D (or 3D) PacMan** — an actual maze, Pac-Man and ghosts navigating it, power dots, the blue-ghost flee state — would be a genuinely fun signature effect. Build it fresh as a grid game (not a strip port); the WLED/MoonLight 1D versions are reference for the state machine only, not the rendering.

### Add real z-axis variation to 2D effects (pending)

Only **NoiseEffect**, **PlasmaEffect** and **RipplesEffect** have z-aware math. The other honest-D2 effects use `Layer::extrude` to duplicate the z=0 plane, so every z-slice is identical on 3D layers. Candidates for genuine D3 promotion: Metaballs/GlowParticles (add z to blob coordinates), Plasma palette/Spiral (add z-driven phase term), Fire (z-drift heat grid), Rings/LavaLamp/Checkerboard/Particles (add z to each element). Prioritise after seeing real 3D installations; each promoted effect also needs its `dynamicBytes` budget for the full 3D buffer.

### Self-describing preview frame header (mid term)

The preview wire format is a private opcode protocol: `0x02` per-frame channels, `0x03` coordinate table, each a hand-rolled byte layout, and the color payload is **always RGB** regardless of the buffer's `channelsPerLight`. Every new data kind (RGBW display, beam direction, …) means inventing another opcode and another fixed layout by hand. The minimal fix that stops that sprawl: a small **typed header** — `[type][format][count][stride]` where `format` enumerates `{RGB, RGBW, …}` — so one message kind carries any per-light channel layout and the browser shader reads `format` to interpret the payload. Do it concrete-first, when RGBW *display* (below) is actually wanted, not speculatively. Prereq for both items below.

### RGBW preview end-to-end (mid term)

The light `Buffer` already holds `channelsPerLight = 4` (RGBW), and the device output drivers handle it, but the **preview only ever sends/draws RGB** — the W channel is invisible in the UI. (The full-res fast path no longer penalises a cpl≥3 buffer — see the short-term fix — but it still drops W on the wire.) Once the self-describing header lands, carry the W channel on the wire and render it in the shader (W as a warm-white tint / brightness lift on the disc). Small, but gated on the header so it isn't another bespoke opcode.

### Fixture model — moving heads, beams (long term)

Today a "light" is a point at a static coordinate with a color. A **moving head** is a fixture that emits a *beam* in a direction it controls live (pan + tilt), plus color, beam-width, etc. — per-light **vector** state, not just color, and a different draw (a cone/ray, not a disc). The static-positions-`0x03` + color-`0x02` split can't express "this fixture's beam now points here." The industry-standard model is **DMX/GDTF fixtures**: a fixture has a position *and* a set of typed attributes (color, pan, tilt, beam). The preview becomes a fixture renderer (disc for a pixel, cone for a beam); this is also the "make Preview a general-purpose module, not light-specific" goal. A domain-model change (the fixture/attribute model), not just transport. Plan when moving heads are actually on the bench.

**Sub-item — per-emitter targets, and fixing the Yellow/UV RGB-synthesis placeholder.** `Correction::apply()` today synthesizes every non-RGB emitter (White, WarmWhite, Yellow, UV) from RGB via the one `whiteMode` gate. That conflates two different physics: **White/WarmWhite are broadband illumination** with a real achromatic basis (`min(R,G,B)` is a sound approximation — warm vs cold differ only in phosphor CCT a byte value can't express), but **Yellow/Amber (~590 nm) and UV (~400 nm) are saturated/out-of-gamut emitters with no honest RGB pre-image.** Yellow's `min(R,G)` stand-in reads greener than a true amber die AND fires on far too much (any red+green content — yellows, whites, skin tones — muddies the fixture); UV's blue-excess is an eyeball hack. Amber is a *real, common, targetable* emitter (RGBA / RGBAW PARs ship a dedicated ~590 nm die), so the right model is an **effect that targets amber DIRECTLY** (a typed attribute), not RGB-and-synthesize. The fixture model is where that lands. **Decision to make when built:** whether Yellow/UV should default to **off** (0, like `whiteMode::None`) until an effect drives them — more honest than always firing a wrong approximation — vs. keeping the "light it up to eyeball the wiring" placeholder. **Practical test to run then:** on a real RGBA/RGBAW+UV PAR, compare the synthesized amber against a directly-driven amber die (does `min(R,G)` look acceptably yellow, or muddy?), and confirm the White subtraction under `Accurate` reads correct with all emitters present. Rationale + the current formulas live in `Correction.h`'s emitter-field comment.

### Mixing light types in one Layouts — open design question (undesigned)

Today each layout child describes one light type (all LED strips, or all par lights), and the current model is one Layouts container per light type. Whether a single Layouts should hold mixed types (LED strips + par lights together), and how the per-channel layout would reconcile across them, isn't designed. Deferred until a concrete need forces it; it's adjacent to the fixture model above (a real fixture/attribute model may reframe how mixed types are expressed). (Moved from architecture.md § What we leave undesigned; a deferred design decision, not a settled 🚧 one.)


## LCD / DMA driver work

### Drop the i80 WR/DC sacrificial pins — done for MoonI80, open for I80

**Shipped for `MoonI80LedDriver` (2026-07-14).** Owning the GPIO matrix is what bought it: the matrix is a routing fabric, so a peripheral signal that is never connected to a pad simply stays inside the peripheral. `dcPin` is **gone entirely** (DC separates command from data bytes for an LCD panel; WS2812 has no such concept, and the peripheral holds it at a constant level), and WR is routed **only** when a '595 needs it as SRCLK — in direct mode WS2812 is self-clocked, nothing reads WR, and the pin stays free for a strand. Same trick frees the *spare data lanes*: they are simply not routed, rather than parked on a "ghost" GPIO. So a direct-mode MoonI80 board spends its GPIOs on strands alone.

**Still open for `I80LedDriver`, and it cannot be fixed there.** IDF's `esp_lcd` hard-requires both (`esp_lcd_panel_io_i80.c`: `wr_gpio_num >= 0 && dc_gpio_num >= 0`) and rejects an NC data pin, which is *why* it must park spare lanes on a real GPIO. Reclaiming the two pins on that driver means leaving `esp_lcd` — which is precisely what MoonI80 already is. So this is not a change to make to `I80LedDriver`; it is a reason to prefer MoonI80 once it is proven. Parlio (P4) never needed the pins (`clk_out_gpio_num = GPIO_NUM_NC`).

### Parlio DMA frame buffer → PSRAM (free internal SRAM for big frames)

For driving **lots of LEDs**, internal SRAM is the scarce resource and the parallel-driver DMA frame buffer is the biggest consumer (8 lanes × lights × outCh × 24 slot-bytes + latch pad). The **i80 driver already allocates PSRAM-first on the LCD_CAM chips** (S3/P4) — `platform_esp32_i80.cpp` tries `MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM` under `#if SOC_LCDCAM_I80_LCD_SUPPORTED`, falling back to internal — which is why the SE16 reaches the full 16384-light frame (see [performance.md § Multi-pin](../../reference/performance.md#multi-pin-led-driving-all-three-peripherals-128128-grid)). The classic-ESP32 I2S i80 backend stays internal-only (its DMA can't reach PSRAM — a hardware limit, not a TODO). **Parlio still allocates internal-only** (`platform_esp32_parlio.cpp`), so a large Parlio frame can exhaust DRAM while PSRAM sits unused; the IDF confirms Parlio's GDMA can burst from PSRAM (`esp_driver_parlio/src/parlio_tx.c` sets `access_ext_mem = true  // support transmit PSRAM buffer`). (RMT already does the right thing — its symbol buffer goes through `platform::alloc`, PSRAM-first with an internal fallback.)

**The change (Parlio only):** allocate the Parlio buffer `MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM` first, falling back to internal when PSRAM is absent/full, using the **external-memory alignment** the IDF requires (`gdma_get_alignment_constraints` → `ext_mem_align`, typically the cache line) and keeping the buffer cache-aligned + its size a multiple of that alignment. **Why its own increment:** it changes the proven hot DMA path, PSRAM DMA has real caveats (cache-line alignment, write-back/coherence on the encode→DMA handoff, and lower PSRAM bandwidth that the IDF guards with a CPU-MAX DFS lock during transmit), and it **must be re-proven on P4 hardware** (the loopback self-test bit-verifies it, then a real strip). It also raises the Parlio ceiling toward the [chunked-transfer](#led-drivers-deferred) goal. Measure the bandwidth headroom too: a very wide, long frame at speed may want internal SRAM regardless.


## LED drivers — deferred

The LED-driver increments **shipped**: increment 1 (RMT/WS2812B single-strand on classic ESP32 — [`RmtLedDriver.h`](../../src/light/drivers/RmtLedDriver.h), `RmtSymbol.h`, `platform_esp32_rmt.cpp`) and increment 2 (2a multi-pin RMT, 2b parallel LCD_CAM on the S3 — [`LcdLedDriver.h`](../../src/light/drivers/LcdLedDriver.h) via [`ParallelLedDriver.h`](../../src/light/drivers/ParallelLedDriver.h), `platform_esp32_lcd.cpp`), all with host + on-board-loopback tests, hardware-proven. The locked decisions, file-by-file phases, the WiFi-flicker test-rig analysis, and the bench deviations (8-GPIO i80 bus, 2.67 MHz slot clock, SOC-macro gate, real-frame loopback) are in [lessons.md](../../work/past/lessons.md), the [driver docs](../../moonmodules/light/moxygen/RmtLedDriver.md), and the [analysis docs](../../work/future/leddriver-analysis-top-down.md). What remains here is only the work that has **not** shipped and is tracked nowhere else.

- **RMT `int_ena` read-modify-write race (classic ESP32, level-5 refill).** `RMT.int_ena` is one register written by both the render task (arming a frame) and the level-5 refill handler (disarming a finished one), through `rmt_ll_enable_interrupt`'s `|=` / `&=`. A handler firing between the task's read and its write loses the task's update, leaving a channel armed or silent. Never observed: the window is a few instructions and the two writes rarely target one channel, so the symptom would be a stuck channel after hours rather than anything the bench shows. Left unfixed deliberately, because the two obvious guards are both wrong here and each was tried on hardware: `portENTER_CRITICAL_ISR` spins on a lock the level-5 handler has just preempted (it runs above `XCHAL_EXCM_LEVEL` 3 by design) and deadlocks the core, and a compare-and-swap builds but crashes, since `S32C1I` addresses only data memory and a peripheral register raises `EXCCAUSE` 3. The remaining candidate is masking to level 5 (`XTOS_SET_INTLEVEL`) around the two-instruction update, which needs no lock and no atomic bus access; it compiles but is unproven on hardware and wants a soak before it displaces firmware that is flicker-free on two boards.

- **sigrok/fx2lafw cross-check + MoonDeck "LED driver test" Python script** — the independent-clock proof and the run-from-MoonDeck flow ([analysis §5.3](../../work/future/leddriver-analysis-top-down.md)). The on-board RMT-RX loopback (shipped) is the cheap CI correctness gate but a *compromised witness* for WiFi-induced flicker — the RX capture runs on the same ESP32 whose WiFi causes the glitch. The real flicker test is a **sustained capture (seconds) with WiFi associated + a packet flood**, decoding every frame for a byte-slip or reset-gap deviation; it validates the SHIPPED render↔encode split's WiFi isolation (drivers tick on core 1; WiFi lives on core 0). A DSLogic Plus (100 MS/s) upgrade is reactive — only if a flicker reproduces that 24 MS/s can't resolve.
- **Chunked transfer (Step 4) — the 16K lever, and now the ONE mechanism behind three separate ceilings.** Split a frame into transactions the DMA can actually swallow, feeding them back-to-back. It was scoped as a Parlio fix; it is really a **core-path** fix, and the shift-register expander is only its third beneficiary.

  **The three ceilings it lifts, all unshifted-first:**
  1. **P4 Parlio: ~4,096 lights.** The 2026-07-12 16-lane sweep found the single-DMA ceiling at 256/lane × 16, reproduced within 0.3% on a second P4 — and the cause is **not** the 65,535-byte cap (256/lane is far under the 897/lane limit). The P4 has 33 MB free heap but the largest *contiguous internal block* is ~368 KB, and a single-shot 16-bit DMA buffer needs one contiguous block: at 512/lane init fails outright. So chunking is **the only path to the 16×1024 = 16,384 lights the 16-lane widening promised.** This is the headline win and has nothing to do with shift registers.
  2. **Classic ESP32: 2,048 lights.** Its I2S DMA cannot reach PSRAM at all, so the frame must fit internal RAM. *(This entry previously said chunking "would not lift that" — that assumed chunking a PSRAM frame the DMA still had to read. It does not hold for the **staged** form below: if the DMA only ever reads small INTERNAL chunks that the CPU fills from a PSRAM frame, the classic chip is lifted too.)*
  3. **The 74HCT595 expander.** Currently capped at ~96 lights/strand because its ×8 frame only renders correctly from internal RAM ([§ 7.5](../../work/future/shift-register-driver-analysis.md)). An add-on, and explicitly **not** the reason to build this.

  **Two distinct limits, one idea — keep them straight.** For **Parlio** the constraint is *transaction size* (contiguous block + 65,535 bytes), so chunking means smaller transactions. For **i80** there is no single-shot cap at all (it chains DMA descriptors) — there the constraint is *where the DMA reads from*, so the win comes from **staging**: keep the frame in PSRAM, but have the CPU copy it a chunk at a time into small internal-RAM buffers that the DMA reads. Same mechanism, different reason, and conflating the two is what muddled the shift-register investigation.

  **Prior art, and why staging is the shape.** hpwit's drivers never DMA from PSRAM: the pixel data may live there, but an EOF ISR transposes it into small **internal bounce buffers** that the DMA clocks out (`owner_check = false`, a fixed ring, built once). Espressif do the same thing under a different name — the RGB-LCD driver's **bounce buffers**. The one place hpwit *does* DMA straight from PSRAM is the **P4 Parlio** path, and he chunks it (≤64 KB via IDF's parlio driver) — which is consistent with our own finding that the P4 handles a PSRAM whole-frame shift transfer fine while the S3 does not. The staged form gets hpwit's underrun immunity **without leaving `esp_lcd`** (still IDF-maintained, still one code path across classic/S3/P4, no raw-register driver) and **without putting the CPU back on a per-LED ISR deadline** — a bulk sequential PSRAM→internal copy per chunk is what PSRAM is good at, unlike a real-time streaming read.

  **The two mechanisms do DIFFERENT jobs, and the 100 fps target needs both.** The WS2812 wire time is a physical constant — 30 µs per light, serial down each strand — so the *only* lever on frame rate is **lights per strand**, never the chip:

  | strands × lights | total | wire time | fps ceiling |
  |---|---|---|---|
  | 16 × 1024 (direct, 16 GPIOs) | 16,384 | 30.7 ms | **33 fps** |
  | **48 × 256 (6 pins × '595)** | **12,288** | **7.7 ms** | **130 fps** ✅ *(hpwit + PO proved this in practice — StarLight)* |
  | **64 × 256 (8 pins × '595)** | **16,384** | **7.7 ms** | **130 fps** ✅ |

  So: **chunking/staging buys LIGHTS (memory); the expander buys FRAMES PER SECOND (short strands).** Neither substitutes for the other, and 12–16K at 100+ fps needs both — the expander's own ~145 KB frame is exactly the one that does not work from PSRAM today, so it *depends* on the staging fix. (A third piece is needed too: at 130 fps the CPU encode must finish in <7.7 ms, and it measures ~24 ms at 16K — that is what the multicore render↔encode split exists for.)

  **The classic ESP32 is a target, not a write-off.** It is routinely dismissed for work like this, and the dismissal is wrong: hpwit and the PO have *run* 48 strands × 256 at ~100 fps on classic silicon (StarLight), with the same '595 expander, while WiFi was up. 240 MHz, two cores, and a 30 µs/light budget is a lot of headroom. The thing that would stop us is **our own encode cost**, which is software we control, not a property of the chip — and the ~24 ms/16K figure quoted in the multicore analysis is (a) measured on a **P4**, not a classic, and (b) **pre-dates the SWAR transpose** that shipped since. Do not carry that number into a classic-ESP32 feasibility argument; measure the real one on the real chip. What the classic genuinely needs is the **staged** form of chunking (its I2S DMA cannot reach PSRAM at all), which this item provides.

  **Build and prove chunking on the unshifted path first** (Parlio 4,096 → 16,384 is the measurable win, on proven code), then let shift mode inherit it — that is a sequencing rule about *where to de-risk the mechanism*, **not** a claim that the expander is optional. It is not: it is the only route to 100 fps at this scale without spending 48+ GPIOs. Correct WS2812 inter-chunk timing is the one hard constraint: the lines must idle LOW for < 300 µs between chunks or the strand latches mid-frame. The driver already rejects an over-limit frame with a loud status. Measured detail: [performance.md § Multi-pin](../../reference/performance.md#multi-pin-led-driving-all-three-peripherals-128128-grid).
- **`rmtWs2812Show` fuller error handling** (deferred from PR #17 / 🐇 CodeRabbit). The shipped path has a finite `rmt_tx_wait_all_done` timeout (1 s) so a wedged DMA can't hang the render tick forever, and a dropped frame self-heals (the driver re-encodes the whole frame next tick). The fuller version — `rmt_transmit` return check, `rmt_tx_stop` to cancel an in-flight transfer on timeout, `show()` returning failure so `loop()` won't reuse `symbols_` mid-transmit — belongs with the **core-1 driver-task** work, since that task owns the buffer lifetime and in-flight state the cancel logic needs.
- **Auto-derived DMA buffer count** (7 / 30 / 75 per [analysis §7.4](../../work/future/leddriver-analysis-top-down.md)), **16-bit pipeline + dither** ([§7.3](../../work/future/leddriver-analysis-top-down.md)), **shift-register expander stubs** ([§7.5](../../work/future/leddriver-analysis-top-down.md)).
- **IR RX live-reconfigure recovery — unconfirmed, park until it recurs** (bench 2026-07-13, SE16). IR reception on the SE16 (`IrService` pin 5) went dead mid-session and only a **hard reset** brought it back; a warm/API path did not. **Ruled out:** not hardware (hard reset fixed it, receiver+switch+wiring fine), not LED-count (IR survives the full 16384-light / 8 fps load — a received code still toggled a control at max load), not a regression from the i80 commit (`platform_esp32_ir.cpp` untouched, the 1250 ns glitch-filter fix intact). **Prime suspect (unproven):** the session's live pin churn — including transiently setting the i80 `clockPin` to **5, which IS the IR pin** — left GPIO 5 routed to the wrong peripheral, and the RMT-RX channel (a pin-keyed static behind `platform::irStop`/`ensureChannel`) didn't re-acquire cleanly on the next `irRead`; only a full GPIO re-init (hard reset) cleared it. This may be pure test artifact (nothing in a *normal* user flow points two live modules at GPIO 5). **To conclude:** from a fresh hard reset (IR working), in isolation set i80 `clockPin=5` then restore `clockPin=8` and check whether IR dies and whether it self-recovers *without* a hard reset — self-recovers → no bug (test artifact); stays dead → a real live-reconfigure gap in the IR channel re-acquire worth fixing (per *No reboot to apply a configuration change*). Small robustness/repro work; do it only if IR breaks again in real use.
- **Moving-head preview = peer interpreter.** When moving heads land, the previewer must interpret channel semantics (pan/tilt/RGBW-at-arbitrary-indices) to render a moving fixture — the same light-preset model physical drivers use, interpreted to screen. This is *why* the increments named the abstraction "interpret the preset" rather than "apply correction / opt out": so Preview becomes a full peer here without a rename. Its own design plan when moving-head support starts.
- **Sparse light-preset editor.** A LightPresets row currently shows one role Select per channel across the whole `channels` width — including the unmapped `—` gaps a wide moving head has between its functions. For a fixture you usually only care about the few channels you drive (rgb, pan, tilt). The refinement: show only the *mapped* channels + an "add channel" affordance (pick a role → fills the first gap or grows the fixture), over the unchanged dense `roles[]` storage. A first attempt shipped and was reverted for edit bugs; redo it cleanly (the dense editor is the reliable interim). Prior art: GDTF / QLC+ fixture profiles (a fixture is a sparse `{channel → function}` map, not a dense per-channel array).

- **`worley` cellular noise, when an effect needs it** (deferred 2026-08-07, PO). The one power function the plan names that stayed unbuilt on purpose. Worley (cellular / Voronoi) noise measures the distance to the nearest of a set of scattered feature points, which is what produces the look nothing else does — cracked mud, scales, stained glass, a caustic. The fields we have cannot fake it: `fbm` and `warp` are smooth by construction, so their creases never form cells.

  **Why it waits.** An industry-standardness audit (Fable, 2026-08-07) put it explicitly on the "do NOT add yet" list: a practitioner does not consider it table stakes, and every other field function in the library earned its place by having a caller. Building it now would make it the only entry whose justification is "the canon has one". The trigger to build it is an effect that wants cells — at which point it lands with that effect, the way `hashInt` landed with Dissolve and `sampleWrap` with Echo.

  **What it costs when it comes:** the standard cheap form is a 3×3 neighbourhood scan around the sample's cell, hashing each neighbour's feature point and keeping the nearest distance — nine `hashInt` calls and nine distance tests per pixel, so meaningfully dearer than one noise sample. `dist16` and `hashInt` already exist, so the function itself is short; the cost question is what it does to a per-pixel budget on a large fixture.

- **16-bit noise: tuning still open** (2026-08-08). The tier ships (`inoise16` 1/2/3D, `fbm16`), and two defects found by review are fixed: `lerp16`'s product is 64-bit (the 32-bit form was signed overflow, undefined behaviour, on roughly a quarter of samples), and `fbm16` sums its octaves at full width instead of shifting each down by 8 — which had made the output 8-bit wearing a 16-bit type (195 distinct values over 20,000 samples; now 15,118).

  **What is still open.** It is VALUE noise, like the 8-bit tier: cell corners are hashed and interpolated. Gradient (Perlin) noise hashes a gradient per corner and takes a dot product, which removes the faint axis-aligned grid value noise leaves — visible on a large smooth field, which is the case this tier exists for. Also unbuilt: a 3D `fbm16`, and `warp16` / `turbulence16` to match the 8-bit compositions. The trigger is an effect that wants a large smooth field; it lands with that effect and its measurements, the way the 8-bit compositions landed with theirs.

- **Clear the grid on an effect's FIRST frame** (2026-08-08). Fourteen effects still show the previous effect's picture on the frame right after a switch: BouncingBalls, Fireworks, FreqMatrix, FreqSaws, GEQ, GEQ3D, GameOfLife, Lissajous, NoiseMeter, PaintBrush, Random and three more. Most predate the power-function branch.

  **What the user sees.** Switching to one of these leaves the old frame underneath for a moment — an audio effect with no signal yet, or a simulation still seeding, paints nothing and inherits whatever was there. `Layer::tick` deliberately does not clear (the documented break: an effect may fade its own last frame for trails), so owning the background is each effect's job.

  **Why it waits.** It is fourteen effects' worth of change across audio-reactive and simulation families, each needing its own judgement about whether to clear, fade, or seed differently — not a mechanical sweep. `unit_Effects_gridsweep.cpp` already measures it (`afterFirst`) and asserts only the settled frame, so the number is visible without blocking.

- **A scripted modifier needs a way to drop a light** (2026-08-10). A coordinate slot is a byte, so a script that computes past 255 wraps: `shift.mlv` with a large `amount` lands lights back at the left edge instead of walking them off it. The Layer already drops an out-of-bounds position, but a script has no way to SAY out-of-bounds — every value it can write is a valid coordinate. Needs a sentinel the binding recognises (or a wider coordinate slot), at which point the "walks off the edge" behaviour a scroll modifier wants becomes expressible.

- **A scripted layout loses its control values across a reboot** (2026-08-11). Persistence SAVES them correctly (`/.config/Layouts.json` holds `"0.width":64`), but on boot they are loaded into a control set that does not exist yet: a script's controls are created by `defineControls`, which can only publish what the ENGINE declared, and the engine does not compile until `prepare()` — Scheduler phase 4, after the phase-2b re-bind. So the loaded value has nowhere to land and the script's declared default wins; a saved 64×64 grid comes back 16×16. Verified on the P4 bench.

  Compiling inside `defineControls` is NOT the fix (tried): it makes the default script's controls exist before `setSource` runs, and swapping the source then re-seeds every control from its new declared default — the same value-loss, moved. The real fix is ordering: the engine must compile once the persisted `source` is in place but before controls are published, which is a Scheduler-phase question (the same parent-before-child ordering the `const_cast` in `MoonLiveLayout::compile` already works around). Affects all three MoonLive bindings, not just the layout.

- **A scripted modifier that reshapes the grid** (2026-08-10). `ModifierBase::modifyLogicalSize` lets a modifier change the logical `width`/`height`/`depth` — a Multiply kaleidoscope grows the grid, a crop shrinks it — and a compiled modifier uses it. A SCRIPTED one cannot: system variables are read-only, so `MoonLiveModifier` writes the box in and never reads it back. Needs a writable system variable — the binding reads the slots after the script returns and reports the result through `modifyLogicalSize` — which is a new `SysVarKind` (or a mutable flag on `SysVar`) plus the read-back, not a new builtin. Until then a scripted modifier can fold coordinates but not resize the grid they live in.

- **The compile-failure latch is not provable on the host** (2026-08-18). `MoonLiveScript::sync`
  refuses to re-attempt a script that failed until its (name, content) changes. The latch exists for
  a device-only reason: each attempt is two LittleFS reads (~5 ms on an S3), a layout is asked from
  `lightCount()`/`placeLights()` as well as `prepare()`, and the pipeline asks repeatedly while
  sizing a fixture, so the retries starve the render task until the watchdog resets the device.

  On the host a re-read costs microseconds and nothing observable differs. Four test shapes were
  tried and each still passed with the latch REMOVED ENTIRELY, so none was kept: what survives pins
  only that a script fixed in place compiles without a rename, which is control-checked. Closing
  this needs either a counting seam (a compile counter the test can read) or a platform fake whose
  reads are observable. Until then the latch is protected by its comment and by hardware, not by a
  test.

- **Catch device-backend operand defects on the host** (2026-08-18). Two array-codegen bugs shipped
  to an S3 while all 1313 host tests stayed green, and both were control-checked: reintroducing
  either one leaves the suite fully passing. `IrInst::c`/`d` are VREG fields the spill pass
  renumbers, so a width parked there becomes a register number; and `sourcesOf` writes its sources
  back POSITIONALLY, so reporting `kArg4` first shifts every real operand one place along. arm64's
  register map absorbs both, which is exactly why the suite cannot see them.

  Three test shapes were tried and deleted for failing their control run: emitted-bytes difference
  tests (wrong bytes still differ from other wrong bytes) and a register-liveness walk (the index
  satisfies it whatever happens to the value). What DOES work is asserting an assembler primitive
  directly, as `unit_moonlive_codegen_xtensa.cpp` now does for `addImm`. The general form is
  probably an IR-level invariant check rather than a bytes-level one: assert that no op reports a
  fixed ABI vreg among its positional sources, and that non-register operands never occupy c/d.
  That is a property of the IR the host CAN evaluate, unlike the emitted code.

- **Size the MoonLive control arena to the script** (2026-08-18). `kCtrlBytes` is a fixed 64 bytes
  per engine (three engines per pipeline), so a script declaring one byte pays for 64 and one
  wanting a 128-light array is refused. The arena is already `platform::alloc`'d, so the constant is
  habit rather than necessity, and the member byte count is known at compile time.

  What blocks it: the system variables sit ABOVE the script region at compile-time constant offsets
  (`kSysWidth = kCtrlBytes + 0`), baked into emitted code as `LoadCtrl` immediates and cached as
  slot POINTERS by the bindings. A script-sized region moves every one of them. Closing it means
  putting the system variables BELOW the script region so their addresses stop depending on it,
  which touches the sysvar table, the bindings and every emitted immediate. The hard ceiling stays
  255 either way, since an arena offset is a `uint8_t` in the record, in `controlSlot` and in the
  instruction. Until then, raising the constant is one edit and the failure is a clear compile
  error naming the arena, so hitting it is visible rather than silent.

- **Drain MoonLive's `print()` through a queue** (2026-08-09). `print(v)` writes to serial directly, and an EFFECT script runs on the render tick — so a print inside one blocks the frame for as long as the UART takes. The burst cap bounds it (a handful of writes per compile, then a compare and a return), but bounded is not free, and `tick()` is annotated `MM_NONBLOCKING`.

  **What it costs when it comes:** a small preallocated record queue the built-in writes into, drained from a housekeeping path through the existing platform output seam. The budget and the burst-spent message stay as they are; only where the bytes are written moves. Worth doing when a script is left with a print in it on a real fixture, which is the case the cap exists for.

- **A bus pin that belongs to another peripheral is accepted, and takes the board off the
  network** (2026-09-06). ParallelLedDriver's classic-ESP32 WR/DC defaults are 18/23, which are
  IDF's Ethernet MDIO/MDC defaults on the same chip, so an Olimex ESP32-Gateway that added the
  driver lost its Ethernet link within seconds and looked crashed (the firmware kept ticking on
  serial; it stayed reachable only through its WiFi fallback). A DC pin of 5, the Gateway's PHY
  reset line, did the same. The driver's own comment claims 18/23 are "unused by the catalog's
  boards", which is false for every RMII board. Two fixes, both wanted: (1) classic defaults that
  collide with nothing common (the RMII set 0/16-19/21-23/25-27, flash 6-11, straps 0/2/12/15,
  and the Dig-Next-2's relays 5/20-22 and I2C 14/15 all excluded); (2) the pin registry already
  detects a double claim (`PinsModule::flagConflicts`) but only colors the edge, so a control
  write that lands on a GPIO another control owns should be refused with a status naming the
  owner, the way a reserved flash pin already is. A collision with a network pin is worse than
  most: it ends the session that could have fixed it, and the SMI pins stay re-muxed until a
  reboot even after the driver releases them.

- **Bench-verified classic parallel setups, not yet in the catalog** (2026-09-06). Every classic
  board in `deviceModels.json` ships `RmtLedDriver`, so none carries a `ParallelLedDriver` entry,
  and the driver's defaults (`clockPin` unset, `dcPin` 33) work on a classic board without one.
  Two setups are proven on hardware and worth recording before they are lost: the **Olimex
  ESP32-Gateway** drives 64 lights on GPIO 16 with `clockPin` 32 / `dcPin` 4, and its free pins are
  scarce enough that a catalog entry should pin them explicitly rather than inherit a default (its
  Ethernet PHY holds 18/23 as MDIO/MDC and 5 as reset, all of which the old defaults collided
  with); the **QuinLED Dig-Next-2** drives 256 lights on GPIO 2 with `clockPin` unset / `dcPin` 33.
  Add them when a classic board actually ships parallel output as its default, rather than
  speculatively across the other 16 classic boards, none of which has been tested this way.

- **Classic-ESP32 I2S instance split: LEDs on 1, audio on 0** (2026-09-06, SHIPPED, kept as the
  rationale). The classic ESP32's parallel LED bus IS an I2S peripheral, so it contends with the
  audio input, and the 2026-09-03 "hangs in `esp_lcd_new_i80_bus`" entry is closed: that was a pin
  fault (the ESP32-PICO-V3-02 has no GPIO 18/23, which were the WR/DC defaults), not contention.
  The split is fixed in silicon rather than chosen: instance 0 alone carries the PDM converters
  (`I2S_LL_PDM2PCM_SUPPORTED_PORT_MASK` is `1U << 0`), and NOTHING on this chip requires instance
  1, so the LED bus is the one consumer that can always yield. It therefore takes 1 unconditionally
  and audio takes 0, which removes the boot race entirely, leaves 0 for every audio source (PDM,
  standard I2S, line-in ADC, codec), and costs nothing. `esp_lcd` picks the first FREE instance
  rather than taking one by number, so 1 is claimed by holding 0 across bus creation. For contrast,
  hpwit's I2SClocklessLedDriver hard-codes `I2S_DEVICE 0` and would take the PDM instance instead.
  Both sides also retry once a second while they want a busy instance, so the loser of any
  contention recovers without the user touching a control.

- **Bench-verified classic parallel setups, not yet in the catalog** (2026-09-06). Every classic
  board in `deviceModels.json` ships `RmtLedDriver`, so none carries a `ParallelLedDriver` entry,
  and the driver's defaults (`clockPin` unset, `dcPin` 33) work on a classic board without one.
  Two setups are proven on hardware and worth recording before they are lost: the **Olimex
  ESP32-Gateway** drives 64 lights on GPIO 16 with `clockPin` 32 / `dcPin` 4, and its free pins are
  scarce enough that a catalog entry should pin them explicitly rather than inherit a default (its
  Ethernet PHY holds 18/23 as MDIO/MDC and 5 as reset, all of which the old defaults collided
  with); the **QuinLED Dig-Next-2** drives 256 lights on GPIO 2 with `clockPin` unset / `dcPin` 33.
  Add them when a classic board actually ships parallel output as its default, rather than
  speculatively across the other 16 classic boards, none of which has been tested this way.

- **Classic-ESP32 parallel LEDs and a PDM microphone are exclusive** (2026-09-06). The
  2026-09-03 "hangs in `esp_lcd_new_i80_bus`" entry is closed: the QuinLED Dig-Next-2 carries an
  ESP32-PICO-V3-02, whose package has no GPIO 18/23 (its pads serve the in-package flash and PSRAM),
  and the classic WR/DC defaults were exactly 18/23. The driver now refuses a pin the package lacks
  and defaults both lines to unset (sunk onto input-only pads). What remains: IDF's LCD mode exists
  on I2S0 only, and `esp_lcd` falls through to I2S1 when I2S0 is taken, which wedges the chip the
  same silent way. A PDM microphone is also I2S0-only in hardware, so the platform refuses the bus
  with a named status when I2S0 is held. Two follow-ups: report the I2S1 fallthrough upstream (it
  should return an error), and note that the raw-I2S classic driver above (the ring, on I2S1 as
  hpwit's driver runs) is what lets the two coexist, on top of lifting the 2048-light cap. A
  standard I2S microphone (INMP441) is content on I2S1 and coexists today when the LED bus claims
  I2S0 first.

(The shared lane-driver scaffolding extraction — when a 3rd parallel backend lands — is tracked separately under [§ Extract shared lane-driver scaffolding](#extract-shared-lane-driver-scaffolding-when-the-3rd-parallel-backend-lands-deferred) above.)

## FixedPoint fades outside the Layer's aggregation (2026-09-07)

`FixedPointEffect` calls `draw::fade` directly where every other fading effect calls
`layer()->fadeToBlackBy`, so it skips what the Layer adds: the per-frame aggregation (N effects on
one layer cost one buffer pass, gentlest rate wins), the framerate scaling, the sub-unit carry, and
the buffer-generation bump. Invisible until a second effect shares the layer.

The swap is one line and was tried: it makes the effect **3.1x brighter at high framerate than at
low**, against the 1.35x band `unit_Effects_framerate` enforces. The two calls mean different
things. `draw::fade(cv, n)` applies n every frame; `fadeToBlackBy(n)` is a RATE per reference frame
that Layer scales by elapsed time. The `fade` control (default 70, range 0..255) was tuned against
the first meaning, so moving it needs the default re-tuned and the trail looked at on a device, not
a silent swap.

Worth doing, as its own change: it is the last effect outside the shared fade path, and while it
stays outside, "every fade goes through the Layer" is not true.

## Downloading a scripted palette can repoint the active one (2026-09-06)

Found while testing the palette-download fixes. A `.mlp` that sorts BEFORE an already-installed one
silently changes what the current selection points at: with `fire.mlp` active at index 60,
downloading `beat-flash.mlp` re-sorts the scripted tail and index 60 becomes `beat-flash.mlp`. The
stored value never moved; what it means did.

[Palette.h](../../src/light/util/Palette.h) already reasons about exactly this and solves half of it. A
palette selection is an index: it persists, it rides `seg[0].pal` over the WLED API, and Home
Assistant renders `paletteNames` positionally, so scripted palettes sort AFTER the built-ins and the
sixty built-in indices are fixed forever. What is not solved is the scripted tail among itself, and
that tail now grows at runtime, which is what the download path made reachable.

Nobody has reported it, and the blast radius is small: a rig with one scripted palette cannot hit it,
and the value re-reads correctly the moment the user picks again. It matters most where a preset or
an MQTT/HA automation stores the index and replays it later, which is the case where nobody is
watching the LEDs when it changes.

**What it would take:** persist the scripted palette by NAME alongside the index and re-resolve the
index on load, so a stored selection survives a re-sort. `paletteScript` already holds the resolved
file name for the editor, so the value exists; what is missing is using it as the authority when the
list changes. The alternative, appending new scripts rather than sorting them, keeps indices stable
but makes the picker unreadable as the list grows, which is the trade the sort was chosen over.

## Classic-board memory: the wins are system-level, not per-module (2026-09-09)

The Dig-Octa on a clean boot with RmtLed has **88,284 bytes free internal, 86,016 largest block**.
Under load and after driver swaps that largest block fell to 24,576, so fragmentation matters as
much as the total.

What the modules hold is NOT where the RAM went. The whole tree reports **9,342 bytes** of
`dynamicBytes` (Layer 3,586, Preview 2,560, RmtLed 1,536, Drivers 1,536). So per-module trimming has
almost nothing left to win, and the remaining ~230 KB of internal RAM in use is WiFi, lwIP, FreeRTOS
task stacks and the binary's static data.

Where to look, in order of likely return:

- **Task stack sizes.** Every task allocates its stack from internal RAM at creation, and the
  defaults are generous. The Tasks module already reports them, so the measurement exists.
- **lwIP pool sizes** (`CONFIG_LWIP_*`): TCP PCBs, pbuf counts, and the send/receive windows are all
  sdkconfig knobs, sized for a general-purpose device rather than a controller with a handful of
  connections.
- **WiFi buffer counts** (`CONFIG_ESP32_WIFI_*_BUFFER_NUM`): the static RX/TX buffer pools are the
  single largest tunable block on a classic board.
- **Fragmentation, not just totals.** A 24 KB largest block with 50 KB free is a placement problem;
  allocating the long-lived buffers early and together is what fixes that.

Each is an sdkconfig change measurable on the bench in minutes (`freeInternal` and `maxBlock` on a
clean boot), and each risks a different failure: too few WiFi buffers drops packets under load, too
small a stack overflows under a rare path. So measure one at a time on a board that is actually
serving the UI, not idle. The payoff is real: at 88 KB free a classic board is one large allocation
away from trouble, which is what drove both driver decisions on 2026-09-09.


## HUB75, native (2026-09-16)

The [Hub75Driver](../../../src/light/drivers/Hub75Driver.h) drives a panel from the board's own pins, as against [PanelCardDriver](../../../src/light/drivers/PanelCardDriver.h) which sends ColorLight frames to a receiving card. It ships unverified: no pixel has come out of it on real hardware, and the entries below are what a comparison against WLED-MM's path found missing. WLED-MM does not implement HUB75 itself; it wraps [mrcodetastic/ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) v3.0.12 in about 600 lines (`wled00/bus_manager.cpp:677`), so the items here are measured against a library with years of hardware debugging behind it.

### Run it on a panel, which nothing has done (the blocker for everything else)

Every other item here is a judgment made from reading. This one is the measurement. What a first session settles, roughly in the order the failures appear: whether anything lights at all; whether the bit order on the bus matches what the ribbon expects; whether the latch pulse lands correctly relative to the address change, which shows as the previous row ghosting into the current one; whether the blanking width is enough; and whether the clock phase suits the panel.

The defect class this catches is exactly the one host tests cannot. F1 of the pre-merge review (the encoder truncating each slot to 8 bits, so the address, latch and output-enable lines never reached the wire) made the committed driver incapable of lighting a panel, and every host test passed throughout because the test remapped those lines into the low byte. A panel answers that question in seconds.

Also unverified for the same reason: the ISR restart path and the PSRAM cache handling in [platform_esp32_hub75.cpp](../../../src/platform/esp32/platform_esp32_hub75.cpp), since the desktop build never compiles that file.

### Panel quirk controls: latch blanking, clock phase, shift-register driver

A HUB75 "panel" is a family, and the differences are not discoverable from its dimensions. The library exposes three knobs we have none of, each for a failure mode with a distinct look:

- `latch_blanking` (1 to 3, default 2): widens the dark window around the latch. Too narrow and the previous row ghosts into the current one.
- `clkphase`: which clock edge the panel samples on. Wrong, and the image shears by a pixel or shows the wrong column.
- The shift-register driver: FM6124 and ICN2038S need a different init sequence. WLED-MM picks FM6124 for "outdoor" panels and its comment says the symptom of the wrong choice is a dark panel or pastel colors.

Build trigger: a panel that misbehaves in one of these ways. Adding all three speculatively is three controls nobody can act on; adding the one whose symptom appears is a control with a reason.

**The trigger has appeared: `latch_blanking` (2026-09-22).** A tester reports the final column dark on two panels, everything else correct. The encoder emits ONE blank word per row ([Hub75Slots.h](../../../src/light/drivers/Hub75Slots.h), the word carrying `oe | lat` after each row's data), where the reference library defaults to 2 and makes 1 to 4 configurable. A single pulse leaves the last column sitting in the shift register as LAT fires, which is the column that goes dark, and the loop already encodes every column so the data is present rather than missing. The fix is a `latchBlanking` control that repeats that blanking word, defaulting to 2 to match the prior art. A panel showing the symptom is the measurement, since the right count is visible in one flash.

### Panel brightness through the output-enable window

Brightness works: `Correction::apply` writes every channel through `briLut`, so the slider dims a HUB75 panel exactly as it dims a strip. The question is HOW it dims, and on this driver it costs color resolution.

Scaling pixel values squeezes the available levels into the bottom of the range. At 4-bit depth that is 16 levels per channel at full brightness and roughly 8 at half, so the banding the cap already causes gets worse the further the slider comes down, which is the opposite of what a user expects from a dimmer.

The library modulates the OE window per plane instead (`brtCtrlOEv2`), shortening how long each plane is lit rather than changing its value. The panel keeps its full depth at any brightness. WLED-MM therefore passes brightness straight through to `display->setBrightness(_bri)` rather than folding it into the pixel data.

This shares its whole mechanism with the plane weighting below, so the two are one piece of work rather than two. Worth noting what it would cost elsewhere: `Correction` is one shared pipeline across every driver, so HUB75 taking brightness out of `briLut` means either a per-driver opt-out or a second path, and that design question is the real work rather than the OE arithmetic.

### Repaint only what changed

`tick()` corrects and encodes the entire frame every time, whatever moved. The library tracks a dirty bit per pixel in `setPixelColor` and repaints only those in `show()`. For a full-frame effect the two are comparable; for a sparse overlay, a clock, or a mostly-static scene, the difference is the whole encode.

Worth measuring before building: our encode is a tight loop over a packed buffer, and a dirty-bit test per pixel is not free either. The win is real only where the frame is mostly unchanged.

### Depth that degrades instead of failing

`bitDepth` is a user control with a hard ceiling, so asking for more than the wall can hold fails init with a memory error. WLED-MM reduces depth automatically by pixel count (8 bits under `MAX_PIXELS_8BIT`, then 6, 4, 3), so a large wall gets a dimmer picture rather than no picture.

That fits the allocate-and-degrade rule every other output seam here follows. It interacts with the weighting item below, since the library's `lsbMsbTransitionBit` is the same trade expressed as refresh rather than as memory, so build them together rather than adding two overlapping automatic reductions.

### Four-scan panels whose physical layout is not their logical one

Some panels scan in a pattern where the physical row order does not match the logical image, and driving them without remapping produces a scrambled picture that looks like a wiring fault. The library handles this with `VirtualMatrixPanel` and `setPhysicalPanelScanRate` (FOUR_SCAN_32PX_HIGH, FOUR_SCAN_64PX_HIGH), and WLED-MM exposes them as bus types 105 to 108.

Our `Hub75Geometry::rowsPerScan()` handles a panel driving more than two rows per address step, which is the common multi-pair case, but not an arbitrary remap. Build trigger: someone owns such a panel. The remap belongs in the Layout rather than the driver, since it describes where lights physically are, which is what a Layout is for.

### Bit depth is not yet weighted

`Hub75Slots.h` emits every bit plane once, so bit 7 lights for the same time as bit 0 and a value renders as a flat ramp: 0x7F and 0x80 are indistinguishable. Binary coded modulation is what the format expects, and the header documents it as the design. `bitDepth` is capped at 4 meanwhile, because an unweighted plane above that costs a scan pass and a share of the frame buffer for a difference the eye cannot find.

**The approach: repeat DMA descriptors, not data.** Plane `p` is stored once and the descriptor chain points at it 2^p times, so the weighting costs descriptors rather than frame memory. mrcodetastic's ESP32-HUB75-MatrixPanel-DMA does exactly this (`configureDMA`, the `for (int k = 0; k < (1 << (i - lsbMsbTransitionBit - 1)); k++)` loop), and it carries a refinement worth taking with it: `lsbMsbTransitionBit` is a dial, not a constant. Planes below it show once each; planes above get full weighting. The library raises it until a configured minimum refresh is met, trading exactness at the low end for rate. That makes depth and refresh one control instead of two fighting ones.

**What it costs here: a new LCD_CAM backend.** The blocker is not the weighting loop, it is that `esp_lcd_panel_io_tx_color` owns its descriptors and gives no way to aim several at one buffer. The library programs the silicon directly instead (`gdma_new_channel`, `gdma_connect(GDMA_TRIG_PERIPH_LCD)`, hand-linked `dma_descriptor_t.next`). So this is the same step this project already took for WS2812, from `platform_esp32_i80.cpp` (675 lines, esp_lcd) to `platform_esp32_moon_i80.cpp` (1,952 lines, own GDMA), with the same hazards that took bench bisects to find there: re-mounting the chain per frame, terminator placement, producer/consumer headroom.

**Parlio cannot express it.** Its only repeat primitive is `loop_transmission`, which repeats the whole buffer forever with no per-plane control. Weighted depth would be an LCD_CAM capability, and the `peripheral` select has to say so rather than offering a choice that silently changes the picture.

Worth doing after the driver has run on a real panel, not before: a flat ramp is visible in seconds on hardware, and so are the ghosting and latch-timing faults that only hardware finds. Brightness belongs in the same piece of work, since the OE window is where both live: today `Correction` scales pixel values before encoding, which spends color resolution exactly where there is least of it.
