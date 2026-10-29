# Backlog: Drivers

To-build items for Drivers, grouped by module, each with its fields on one line. Other areas: [the future index](index.md).

## DriverBase

### A control change reaches a driver at the frame boundary (2026-10-21)

Kind: Restructure

With the render split on, a driver's `tick()` runs on the encode task while its `onControlChanged`, `onCorrectionChanged`, `tick20ms` and `tick1s` run on the main loop, and only structural changes and `setEnabled` park the encode task first.
An audit of `src/light/drivers/` found the main loop writing state the encode task reads: `DriverBase`'s `correction_` (rebuilt on a control change, ramped by `tick20ms`, `motionHeld` set by `tick1s`), `effectiveBrightness_`, the `wire_` buffer a channel-count change reallocates, the `start` and `count` window; HueDriver's pairing and light tables; NetworkSendDriver's `dest_` addresses on the inline send path and its `corrected_` buffer; PwmLightDriver's `lights_` and `duty_`; RmtLedDriver's and ParallelLedDriver's whole geometry through `parseConfig` and `reinit`, ParallelLedDriver calling `parseConfig` and `setStatus` from the encode task itself; RtspDriver's and ParallelLedDriver's `setStatus` from `tick()`; PreviewDriver's `downscale_` and `keptIdxAllocFailed_`.
The textbook move is the one live reconfiguration already promises, a change applied on the next frame: the main loop records the change, and `Drivers::tick` applies it at the frame boundary, after `quiesceEncode()` and before it notifies the worker, so a driver's state has one writer at a time. The thread sanitizer lane reports none of these today, since no test changes a control while the encode task runs; the first test that does pins the move.
The same pass covers ParallelLedDriver's `peripheral` swap, which disengages the render split through the quiesce hook and, as `peripheral` is not in its `affectsPrepare`, leaves it off until another prepare runs.

### An identity correction is one copy (2026-10-14)

Kind: Restructure

Every driver runs `Correction::apply` on every light, even when the correction changes nothing: brightness 255 (global and local), the linear curve, a fixture profile whose channels match the layer's, and no dimmer, white or motion channels.
The hottest-path rule says that case is a copy: `Correction` knows at rebuild whether it is the identity, and a driver that sends its buffer whole (Network Send, the panel cards) copies the layer's bytes instead of looking each one up.
The default CIE curve is not the identity at 255, so the skip applies only with the linear curve. It applies to every driver once the sender already sends the output's byte order; network and panel drivers then send one copy, while LED strips still encode bits but drop the per-byte lookup and reordering.

### Stop reading fixtureRef after the release that follows 2026-10-05

Kind: Restructure · Trigger: the release after 2026-10-05

- **Stop reading `fixtureRef` after the release that follows 2026-10-05.** A driver saves its profile's name in `fixture` since then, and `DriverBase` still reads the hidden `fixtureRef` an older config saved beside a row number. With the next release out, remove the control and `DriverBase::adoptLegacyName` with its two calls, and add a `migrate.js` entry mapping `fixtureRef` onto `fixture` so an older backup still restores.

## GpioDmaPeripheral

### A '595 expander on the Teensy's GPIO-DMA bus

Kind: Feature · Platform: Teensy

`supportsPinExpander()` is false, so a Teensy drives 16 strands at most. The port write carries no shift clock of its own, so the clock has to come from the writes themselves, at a DMA rate not yet measured. TriantaduoWS2811 drives 32 strands from 3 pins through FlexIO clocking '595s at zero CPU cost, and FlexIO is what the Teensy plan set aside for want of free contiguous pins beside the W5500. The reasoning: [the LED-driver analysis](analysis-leddriver.md#the-pin-expander-on-the-other-peripherals).

## HlsDriver

### HLS upscaling is cache-hostile on large walls (2026-08-28)

Kind: Investigation

`HlsDriver`'s `scale` control replicates each light into a scale x scale block. Measured on the
bench P4 at 128x128: **~1 ms per frame at 1:1, ~60 ms at scale 4** (a 512x512 output). The frame
is 16x larger, so ~16 ms would be the honest cost; the extra 4x is the access pattern. The loop
walks LIGHTS and writes each block as `scale` short rows, the first filled and the rest copied
from it by `memcpy`, scattered across a 786 KB buffer, so consecutive lights touch distant addresses and every write misses cache, where the
1:1 path writes straight through sequentially.

**Not urgent, because the default path never hits it:** auto-scale only engages on walls below
the encoder's floor ([watching the output](../../reference/watching-the-output.md)), where the output is small by construction. The expensive case is a manual scale on an already-large wall, which is also
where upscaling has the least to offer.

**The fix when it earns its place:** iterate the OUTPUT rows rather than the input lights, so
writes are sequential: for each output row, walk its source row once and emit `scale` copies of
each light's color, then `memcpy` that finished row to the remaining `scale - 1` rows of the
block. Same output, one pass through the destination in address order.

## Hub75Driver

### The shift-register driver init

Kind: Feature

A HUB75 "panel" is a family, and the differences are not discoverable from its dimensions. FM6124 and ICN2038S panels need a different init sequence; WLED-MM picks FM6124 for "outdoor" panels, and its comment says the symptom of the wrong choice is a dark panel or pastel colors. Build trigger: a panel showing that.

### Repaint only what changed

Kind: Investigation

`tick()` corrects and encodes the entire frame every time, whatever moved. The library tracks a dirty bit per pixel in `setPixelColor` and repaints only those in `show()`. For a full-frame effect the two are comparable; for a sparse overlay, a clock, or a mostly-static scene, the difference is the whole encode.

Worth measuring before building: our encode is a tight loop over a packed buffer, and a dirty-bit test per pixel is not free either. The win is real only where the frame is mostly unchanged.

### Depth that degrades instead of failing

Kind: Feature

`bitDepth` is a user control with a hard ceiling, so asking for more than the wall can hold fails init with a memory error. WLED-MM reduces depth automatically by pixel count (8 bits under `MAX_PIXELS_8BIT`, then 6, 4, 3), so a large wall gets a dimmer picture rather than no picture.

That fits the allocate-and-degrade rule every other output seam here follows. It interacts with the weighting item below, since the library's `lsbMsbTransitionBit` is the same trade expressed as refresh rather than as memory, so build them together rather than adding two overlapping automatic reductions.

### Four-scan panels whose physical layout is not their logical one

Kind: Feature · Trigger: someone owns such a panel

Some panels scan in a pattern where the physical row order does not match the logical image, and driving them without remapping produces a scrambled picture that looks like a wiring fault. The library handles this with `VirtualMatrixPanel` and `setPhysicalPanelScanRate` (FOUR_SCAN_32PX_HIGH, FOUR_SCAN_64PX_HIGH), and WLED-MM exposes them as bus types 105 to 108.

Our `Hub75Geometry::rowsPerScan()` handles a panel driving more than two rows per address step, which is the common multi-pair case, but not an arbitrary remap. Build trigger: someone owns such a panel. The remap belongs in the Layout rather than the driver, since it describes where lights physically are, which is what a Layout is for.

### Repeated planes: full top brightness and 8-bit depth

Kind: Feature · Trigger: a wall that reads too dim at 6-bit, or a user who needs 8-bit gradients

The planes are weighted by their lit window ([HUB75 panels](../../reference/hardware/hub75-panels.md)). That costs top brightness, since the lower planes are dark for most of their pass: full white is about 47% of the row time at 4-bit and 33% at 6-bit. It also caps depth at 6, where plane 0 of a 64-column row still lights 2 words. The reference library repeats the upper planes instead, and windows only the lowest (`lsbMsbTransitionBit`, chosen automatically for a refresh floor): about 95% top brightness and 8-bit depth, at roughly 290 Hz for one 64x64 panel.

**The approach: repeat DMA descriptors, not data.** Plane `p` is stored once and the descriptor chain points at it 2^p times, so the weighting costs descriptors rather than frame memory. mrcodetastic's ESP32-HUB75-MatrixPanel-DMA does exactly this (`configureDMA`, the `for (int k = 0; k < (1 << (i - lsbMsbTransitionBit - 1)); k++)` loop), and it carries a refinement worth taking with it: `lsbMsbTransitionBit` is a dial, not a constant. Planes below it show once each; planes above get full weighting. The library raises it until a configured minimum refresh is met, trading exactness at the low end for rate. That makes depth and refresh one control instead of two fighting ones.

**What it costs here: a new LCD_CAM backend.** The blocker is not the weighting loop, it is that `esp_lcd_panel_io_tx_color` owns its descriptors and gives no way to aim several at one buffer. The library programs the silicon directly instead (`gdma_new_channel`, `gdma_connect(GDMA_TRIG_PERIPH_LCD)`, hand-linked `dma_descriptor_t.next`). So this is the same step this project already took for WS2812, from `platform_esp32_i80.cpp` (675 lines, esp_lcd) to `platform_esp32_moon_i80.cpp` (1,952 lines, own GDMA), with the same hazards that took bench bisects to find there: re-mounting the chain per frame, terminator placement, producer/consumer headroom.

**Parlio cannot express it.** Its only repeat primitive is `loop_transmission`, which repeats the whole buffer forever with no per-plane control. Weighted depth would be an LCD_CAM capability, and the `peripheral` select has to say so rather than offering a choice that silently changes the picture.

Build trigger: a wall that reads too dim at 6-bit, or a user who needs 8-bit gradients. It stays automatic, chosen from `bitDepth` and a refresh floor, with no new setting.

## HueDriver

### Migrate HueDriver's name buffers to ScratchBuffer (finish the ScratchBuffer sweep)

Kind: Restructure

`HueDriver::ensureNameBuffers()` / `freeNameBuffers()` is the alloc-in-build / free-in-release / null-guard bundle that `ScratchBuffer` absorbs — and it does **no** `dynamicBytes` accounting, so the module's memory readout under-reports its Hue-light name buffers. It was skipped in the ScratchBuffer migration (its buffers are `char[]` name tables, not the grid-sized state the sweep targeted). Now that `DriverBase::release()` chains to `MoonModule::release()` (fixed on PR #43), a `ScratchBuffer` member here would free correctly on disable and self-report its bytes. Mechanical, one-file, behaviour-preserving. Flagged by the 👾 Reviewer on PR #43.

## MoonI80Peripheral

### The ring's refill jitter (2026-10-22, from the internal RAM plan)

Kind: Investigation · Platform: S3

LCD-MM's refill interrupt fires every 89 µs at 3-row slices, yet almost every second it goes about 1.1 ms without firing.
So `ringAuto` keeps 54 rows of lead ([LED drivers](../../reference/hardware/led-drivers.md)), where hpwit's S3 driver runs on 10.

Measured with `ringDbg`'s `gap`, `ml` and `c` fields. The gap ends mid-frame as often as at the idle tail.

Ruled out:
- the flash cache: `co` stays 0;
- the core: registered on core 1 it was the same, and it broke the cache guard, so it stays on core 0.

Next, the gap measured against the frame's own events, the arm and the snapshot, then against other interrupts on core 0.

A lead of 10 rows would free most of the ring's internal RAM.

### MoonI80 streaming ring: open instruments and cleanups

Kind: Investigation · Platform: S3

The ring ships and is wall-verified through 48 strands × 256 (12,288 lights). The mechanism lives in [MoonI80Peripheral.h](../../../src/light/drivers/MoonI80Peripheral.h) and [LED drivers](../../reference/hardware/led-drivers.md); the design arc in `docs/work/past/plans/`. Open items:

- **Last-8-panels white flash, the "44-46 flash" (OPEN: the lanes ≥ 32 encode path).** On the 48×256 wall, brief WHITE/bright flashes (not wrong colors) over an otherwise-correct image, confined to strands 40-47 (the last 8 panels), mostly panels 44-46, wandering within the last 8. **Brightness-gated: clean below 5, flashes at ≥5**: since brightness scales pixel values through a LUT, below 5 the frame collapses to near-all-zero bits, so the gate really means *the flash needs SET (one) data bits*. **The same physical wall runs clean on hpwit's driver, so it is OUR driver, not the hardware.** **A live pin reorder located it:** the reorder moved the strands and the flash stayed with lanes 32 to 47, so it is the lanes ≥ 32 (`maskHi32`) encode path under the wall's exact active set, not the last '595, GPIO 17 or the latch bit. Ruled out by live hardware tests (do NOT re-chase): (1) the encode source (PSRAM vs internal; the ISR-source staging fix did nothing, reverted); (2) the ISR/lapping regime (the flash is on prime-encoded rows too); (3) it being a tail-*rows* phenomenon; (4) shift-clock margin via `shiftOverclock` (already at the 20 MHz OFF setting, still flashes); (5) bus-bit latch adjacency (a `latchBitHigh` diagnostic moved the latch to bit 7, still flashes). The encoder passed a host fuzz over RANDOM masks (`ParallelSlots.h` byte-for-byte correct), which never hit the wall's trigger: next, reproduce on the host with the wall's real geometry, active set and content, starting at the 64-bit mask round-trip (`maskLo32 | maskHi32 << 32` in `ParallelLedDriver.h` against the encoder's re-split in `ParallelSlots.h`). All diagnostics from this investigation are reverted, and the tree is clean.
- **MoonI80 direct mode flickers below ~30 lights on a PSRAM frame buffer (OPEN, cause unproven).** The write-up, what is established and the theory to test: [lessons.md](../../work/past/lessons.md#mooni80-flickers-below-30-lights-on-a-psram-frame-buffer-open-cause-unproven). Next instrument: `loopbackTest` + `loopbackIntrusive`, which captures what the peripheral actually emitted and separates a corrupt frame from a stalled transfer (needs the RX jumper pin — the SE16 routes its LED outputs, so pick one that can read back).
- **Ring bus init hard-fails instead of stepping down when `ringBufs` is raised past what RAM allocates** (wall went dark until the control was lowered again; the pool alloc steps down but a later allocation, likely the descriptor link list, does not). A control change must degrade, never dark the output.
- **~1-frame white/colored flash every ~5 s** seen at some configs — plausibly fixed by the frame-close latch word (a strand whose last data bit ended HIGH missed its reset that frame); soak-observe on the wall before closing.
- **Prime barrier fps cost.** The ring's prime holds a busy-wait until the previous frame's deterministic wire end (`waitWireDrained` in `primeRingRange`). The wait is ~0 when the snapshot + render gap already span the wire, but on fast frames it serializes wire → prime and caps fps at 1/(wire + snapshot + prime). If the fps work wants that overlap back, the barrier can go finer-grained (per-buffer: buffer b is safe once the drain passes slice `b + nSlices − ringBufs`) — measure first.
- **Multi-strand loopback**: the instrument drives one `loopbackStrand`, so it is structurally blind to a multi-strand fault (it passed while the wall was visibly corrupt).
- **Shift-mode loopback host coverage**: nothing in the suite drives shift-mode loopback end-to-end through the mock bus — the detection gap that let two loopback bugs live unnoticed. (The stall bug itself is fixed: capture-first alloc + pool step-down; verified on two boards.)
- **Loopback teardown leak + heap fragmentation**: cycling the private-bus loopback drops ~80 KB internal and the heap stays fragmented (measured maxBlock 13–44 KB with 240–310 KB free). Capture-first + step-down made runs reliable despite it, but the leak itself stands — chase when the loopback is next touched.
- **Ride-the-live-ring loopback — parked**: built (`loopbackIntrusive` + the snapshot pattern hold) but an RMT-RX cannot capture on a GPIO the LCD_CAM is actively driving (reads 0 sym). Revival path: briefly output-detach just the RX pin for the capture window.
- **Diagnostic surface** (`ringDbg` incl. tw/ts/tp + sg/se, the dbg statics, the `kCyPerUs = 240` hardcode): kept deliberately through the tuning era; gate/remove when the ring fps work closes.
- **fps header reports the module TICK rate**, not the frame rate (`frameTime` is the real one); any fps claim predating 2026-07-17 reads with that in mind.
- **`ringAuto` "just works" — verify the geometry pick on smaller configs.** The pick is wall-verified at 48×256; the smaller common configs (16×256 and below) still need a bench pass. If any has a strictly better geometry `ringAuto` misses, teach it that ONE *measured* rule (no per-effect/per-density heuristics — the flicker was never geometry). `ringAuto` itself stays visible as the recourse until this proves it always picks right, then it can go expert-only too.

### The ring's memory readout counts one buffer, not the pool (CodeRabbit, 2026-09-09)

Kind: Bug · Platform: S3

`LedPeripheral::busHeapBytes()`, which MoonI80 does not override, counts `busCapacity()`,
the size of ONE buffer, twice when `busBuffer(1)` exists, which
is right for the double-buffered path and wrong for the ring, where `createRingState` allocates
`ringBufs` slices (up to 30 at the 48x256 geometry) plus the shared zero-pad. So a ring board
under-reports its DMA memory by roughly the buffer count, on the card that exists to make exactly
that number visible.

Not a correctness bug: the memory is spent either way, and nothing sizes an allocation from this
value. It is a reporting bug in the one readout a user consults before adding lights.

**What it takes:** a `busHeapBytes` override on MoonI80 (the ring knows `ringBufs * bufBytes + pad`
at creation). Wants a ring-mode
accounting test alongside, since the existing ones only cover the non-ring path. Touching
`createRingState` means a bench pass on the wall, which is why this is its own change.

### MoonI80 ring: the first frame after a rebuild trips a transient give-up status (2026-07-16)

Kind: Bug · Platform: S3

A cosmetic residual left after the rebuild-wedge fix (below): on boot, and for a beat after any shift-ring rebuild, the driver shows **"output stalled"** even though `wireUs` reports live completions — the *first* frame after a fresh ring build occasionally misses its completion window and trips the dead-frame give-up before the ring settles, so the stale error latches until the next interaction clears it. It is NOT the old wedge (that stayed dead until reboot; this self-clears on any control edit and the ring is genuinely driving underneath). Two clean fixes to weigh: (a) don't count the very first frame after a rebuild toward `deadFrames_` (give the fresh ring one grace frame), or (b) have the give-up retry re-derive status the moment `wireUs` shows a real completion. Low priority — the LEDs drive correctly; only the status string is briefly wrong.

## NetworkSendDriver

### ArtPoll discovery: know which tubes are alive

Kind: Feature

`NetworkSendDriver` unicasts to a list of receivers (`hosts` + `lightsPerHost`), which is the Art-Net-4-conformant model. What it cannot do is **tell whether a receiver is actually there**: UDP is fire-and-forget, so a dead tube is invisible to the sender. The spec's own answer is discovery — *"The transmitting device must regularly ArtPoll the network to detect any change in devices which are subscribed"* — and it is the natural next increment.

**Why it earns its place (product-owner experience, 2026-07-13):** a dead tube in a prior setup *made the other tubes hiccup*. The mechanism is that a send to an unresolvable address stalls or errors **inside the frame loop**, delaying the packets for every destination after it — so one dark tube degrades the live ones, every frame. The send loop tolerates that (a failed send drops only that packet), but tolerating is not the same as **knowing**: with discovery we can simply **skip destinations that haven't answered**, which removes the stall at its source rather than absorbing it.

**Scope (~130–150 lines + tests; the receive half already exists).** `ArtNetPacket.h` already builds+parses, `UdpSocket` already binds and reports the sender's IP:
- **ArtPoll send** — broadcast a 14-byte poll every ~3 s (this is the one packet Art-Net *does* broadcast, by design).
- **ArtPollReply parse** — fixed-layout 239-byte reply: node name, IP, and its subscribed universes.
- **A node table** — IP → last-seen; mark offline after ~9 s of silence (3 missed polls, the spec's own cadence).
- **Use it:** skip offline destinations in the send loop; surface the live/dead list as a read-only status. **Bonus, and arguably the real prize: auto-populate `hosts`** — the user stops typing addresses at all, which is how a professional controller behaves.

Do it as its own increment. The multi-destination unicast it builds on has shipped.

## PanelCardDriver

### MoonLight hangs when a USB network adapter is re-plugged (2026-09-28)

Kind: Bug · Platform: desktop

Unplugging and re-plugging a USB Ethernet adapter while PanelCardDriver is active wedges the whole application. Measured: the render task stops logging entirely (0 bytes in 8 s), one thread spins at 74% of a core while the other two sit in `Wait`, the listener still accepts so connections pile up unanswered (11 of them), and HTTP never returns, tested to 45 s. The last line written is always `NetworkModule: Ethernet up (recovered from Idle)`, so it enters the spin somewhere in the Ethernet-recovery path on the render thread rather than blocking on a lock.

Reproduced once, on a bench with a Realtek USB GbE adapter feeding a ColorLight card. The trigger is ordinary: anyone driving panels from a USB dongle does this. The suspects are the pcap calls that recovery reaches, since `ethBindRawInterface` runs `pcap_findalldevs` and `pcap_open_live` on an adapter that has just re-enumerated, and both are called from `prepare()` on the render task. Worth a second reproduction with a debugger attached before guessing further: a spinning thread with a live listener is a loop, not a deadlock, and the loop has a home.

### The interface picker re-enumerates adapters on every control rebuild (2026-09-28)

Kind: Bug · Platform: desktop

`rawInterfaces()` caches nothing, and `defineDriverControls()` calls it on every rebuild of a driver that owns an interface Select. On Windows `pcap_findalldevs` opens each adapter in turn and measures ~2.3 s, so every control change on PanelCardDriver, every `/api/types` that builds the type catalog, and every UI refresh paid it. Measured with a panel driver present: `POST fixture` 2.2 to 6.7 s, `/api/types` 2.3 to 4.4 s, `/api/state` 2.2 s. The same payloads on a config with no panel driver: 23 to 36 ms. POSIX is not affected, since it enumerates with `getifaddrs`.

A cache with a 60 s window took those to 8 to 35 ms and was reverted before the release as too much risk for hardware-critical code. Two things learned from doing it: the window has to outlast a person pausing to think, because a 3 s one leaves every deliberate control change paying full price; and skipping the rebind needs `ethLinkUp()` in the condition, because an open pcap handle whose link is down still wants rebinding and guarding on "already bound" alone removes the only recovery a user has. Scope any cache to Windows, since it buys POSIX nothing and costs it hot-plug latency.

### Multi-card walls: does a daisy chain work today?

Kind: Investigation · Trigger: ask before building

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

Kind: Feature

- **Read the wall layout from the ColorLight cards.** The cards can report their configuration and at least one user's own tool already does it; it would remove the manual layout step.
- **Per-card color temperature and brightness**, via the ColorLight sync-packet bytes, grouped by sync group — used to color-match mixed panel batches live.

### Configure a ColorLight card without Windows (2026-10-16, asked)

Kind: Feature

Asked on Discord by robwomp: a way to set up ColorLight receiving cards from a Linux command line, so a ready-made MoonLight image (a Pi running the desktop build with PanelCardDriver) could configure the card itself and make a card and its panels plug and play.

Today the card's configuration (panel size, panel count, scan, driver IC) is written once with ColorLight's LEDVision, and its firmware with LED Upgrade, both Windows-only ([panel cards §4 and §7](../../reference/hardware/panel-cards.md)). The protocol for those writes is ColorLight's own and undocumented; the pixel and discovery frames are known from [Harald Kubota's write-up](https://hkubota.wordpress.com/2022/01/31/winter-project-colorlight-5a-75b-protocol/), the configuration frames are not.

The route: capture what LEDVision sends while it writes a known configuration (Wireshark on the card's Ethernet), decode the frames, and replay them. [REA](https://github.com/morluto/rea) (MIT) does the decode step: its packet mode reads the capture through TShark and reports repeated values and field candidates, run from the terminal (`npx rea-agents …`) rather than installed into the agent's configuration. Its static .NET inspection could read how LEDVision builds the frames, if LEDVision is .NET and its license allows it. Then replay them from PanelCardDriver as a "configure card" action taking the wall's panel size, count, scan and IC. Reading the configuration back (the "read the wall layout" ask above) shares the same decoding. The firmware path stays with LED Upgrade: a bad write bricks a card, which a configuration write does not. Ask before building: it needs a card on the bench and a Windows machine running LEDVision for the capture.

## ParlioPeripheral

### A '595 expander on Parlio

Kind: Investigation · Platform: P4

`supportsPinExpander()` is false, so a P4 on Parlio drives 16 strands at most. Parlio generates its pixel clock itself and `platform_esp32_parlio.cpp` leaves `clk_out_gpio_num` unconnected since WS2812 ignores it; connected, it is the '595's shift clock, and the latch rides as a bus bit as it does on `i80`. Whether the clock output and the bit packing carry the expander's eight words a slot is a bench question, and the eightfold frame meets the single-transaction ceiling the chunked-transfer item lifts. The reasoning: [the LED-driver analysis](analysis-leddriver.md#the-pin-expander-on-the-other-peripherals).

### P4 Parlio streaming ring: lift the ceiling past about 21K lights

Kind: Feature · Platform: P4

**Port the ring concept to the P4 Parlio path**, to drive far more than its current whole-frame ceiling. troyhacks' MoonLight Parlio driver reaches **~21K LEDs RGB (~16K RGBW)** at 16 lanes — but NOT by materialising the whole encoded frame: he stages into a **fixed ~512 KB PSRAM buffer** and DMAs it out in **64 KB chunks** (`max_transfer_size = 65535`), so the DMA never needs the whole frame contiguous. That is the SAME idea as our MoonI80 ring, applied to Parlio on the P4 (where — unlike the S3 shift clock — the DMA *can* sustain PSRAM reads at the WS2812 rate). His ceiling is a *chosen buffer size*, not a hardware wall, so it caps at ~21K.

**Two tiers, in order:**
1. **Match him (moderate):** chunk our Parlio DMA the way he does (bounded PSRAM staging + 64 KB bursts) instead of one contiguous encoded frame — this alone lifts our P4 Parlio ceiling to his ~16–21K. Our Parlio path already uses PSRAM; the change is the chunked-DMA transfer shape, not a new architecture.
2. **Beat him (the ring):** run our MoonI80 **streaming ring** on P4 Parlio — small internal buffers, refilled per drain from a PSRAM source, holding internal RAM *constant* regardless of light count (the hpwit model our ring already implements). troyhacks' 512 KB buffer is still *bounded*; the ring is not, so this goes past ~21K to light-count-independent. It reuses the ring mechanics we already have (linear self-terminating chain, per-drain refill, drain-count termination); the P4 Parlio backend gets a ring variant beside its whole-frame path, the way MoonI80 has both.

**Prerequisite / sequencing:** tier 2 wants the **ISR-refill + `MM_HOT`** work done first (same as the classic ring above), since a high-rate Parlio refill benefits from the ISR-grade determinism. Confirm the CURRENT P4 Parlio tested ceiling first (re-measure on the P4 bench) before claiming a head-to-head — the ~21K figure is troyhacks' *buffer-fit* limit, not a verified-on-wire number, and his code steps the clock down at 256/512 LEDs per lane (signal integrity at long strands), which our own measurement must account for. Reference: the parlio source is `src/MoonLight/Nodes/Drivers/parlio.cpp` in the predecessor; our Parlio backend is `platform_esp32_parlio.cpp` + `ParlioLedDriver.h`.

## PreviewDriver

### Preview does not resume after a long tab hibernation (observed once, 2026-08-27)

Kind: Bug · Platform: desktop

Desktop instance at 1024x1024: after the browser tab sat backgrounded for about an hour, the
preview pane stayed static on return. PreviewDriver ticked at ~1 us (no standing request served)
while a FRESH WebSocket client received frames normally, and the Drivers card showed the
encode-worker-stalled latch. A page refresh reportedly did NOT revive it; toggling `multicore`
(a Drivers re-prepare) did. Suspects: the tab-hide hibernation path's tracked retry vs the
wake-up re-request, or per-driver lease state that only prepare() resets. Needs a reproduction
with the WS uplink logged before it can be fixed.

### Self-describing preview frame header

Kind: Restructure

The preview wire format is a private opcode protocol: `0x02` per-frame channels, `0x03` coordinate table, each a hand-rolled byte layout, and the color payload is **always RGB** regardless of the buffer's `channelsPerLight`. Every new data kind (RGBW display, beam direction, …) means inventing another opcode and another fixed layout by hand. The minimal fix that stops that sprawl: a small **typed header** — `[type][format][count][stride]` where `format` enumerates `{RGB, RGBW, …}` — so one message kind carries any per-light channel layout and the browser shader reads `format` to interpret the payload. Do it concrete-first, when RGBW *display* (below) is actually wanted, not speculatively. Prereq for both items below.

## RmtLedDriver

### RMT over DMA on the S3/P4, with a completion callback (Funkelfetisch, July 2026)

Kind: Feature · Platform: S3

Funkelfetisch's fork carries a finished branch, `codex/upstream-rmt-rgbw-performance`, that the
classic-ESP32 flicker work of 2026-09-05 makes worth adopting: on chips with RMT DMA
(`SOC_RMT_SUPPORT_DMA`: S3, P4) it sets `with_dma` with the IDF-recommended 1024-symbol block, so
the frame streams from RAM and the refill interrupt that causes flicker on a DMA-less chip does not
exist at all. It replaces the blocking `rmt_tx_wait_all_done` with `rmt_tx_register_event_callbacks`
(`on_trans_done`) plus a per-channel busy flag so the next tick skips while a frame is in flight,
and reports `"RMT DMA"` in the driver status so a user can see which path is live. Files:
`platform_esp32_rmt.cpp` (+105), `RmtLedDriver.h` (+75), `LedDriverConfig.h`, `Correction.h`
(RGBW profiles, a separate topic in the same branch), with unit tests.

What it does NOT address: on the classic ESP32 the DMA half compiles to nothing, and nothing in it
moves the channel's interrupt off core 0 (the root cause found on the Dig-Next-2, fixed by
creating the channel from core 1). The two are complementary, one driver with the right answer
per chip: DMA where the silicon has it, the core-1 refill where it does not. Adopt his DMA and
callback path and keep the core hop. Study, do not copy: write it against the seam as it stands,
credit the branch.

Re-read against the wire-byte driver (2026-09-09), which changed two of the assumptions above. The
driver ships the correction's wire bytes and the peripheral expands them
([RmtLedDriver.h](../../../src/light/drivers/RmtLedDriver.h)). So (a) his `mem_block_symbols = 1024` is now the
only symbol memory in play and is cheap, and (b) `rmt_transmit` already takes the bytes, so the DMA
path needs no buffer change, only `flags.with_dma` plus the callback. The `txInFlight_` guard STAYS
whatever happens: the peripheral streams straight out of the driver's frame buffer, so a rebuild
that frees it mid-frame still tears; his busy flag would replace the blocking wait, not that guard.
The win left on the table is the blocking `rmtWs2812Wait` on S3/P4, which today costs the tick the
whole wire time of the longest strand.

### RmtLedDriver's out-of-memory status has no test, and cannot have one yet (2026-09-09)

Kind: Feature

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

### RMT int_ena read-modify-write race (classic ESP32, level-5 refill)

Kind: Bug · Platform: classic

- **RMT `int_ena` read-modify-write race (classic ESP32, level-5 refill).** `RMT.int_ena` is one register written by both the render task (arming a frame) and the level-5 refill handler (disarming a finished one), through `rmt_ll_enable_interrupt`'s `|=` / `&=`. A handler firing between the task's read and its write loses the task's update, leaving a channel armed or silent. Never observed: the window is a few instructions and the two writes rarely target one channel, so the symptom would be a stuck channel after hours rather than anything the bench shows. Left unfixed deliberately, because the two obvious guards are both wrong here and each was tried on hardware: `portENTER_CRITICAL_ISR` spins on a lock the level-5 handler has just preempted (it runs above `XCHAL_EXCM_LEVEL` 3 by design) and deadlocks the core, and a compare-and-swap builds but crashes, since `S32C1I` addresses only data memory and a peripheral register raises `EXCCAUSE` 3. The remaining candidate is masking to level 5 (`XTOS_SET_INTLEVEL`) around the two-instruction update, which needs no lock and no atomic bus access; it compiles but is unproven on hardware and wants a soak before it displaces firmware that is flicker-free on two boards.

## Across modules

### Clocked and one-lane strips over SPI

Kind: Feature · Platform: all

APA102, SK9822 and HD107S take a clock and a data line, which is SPI at tens of MHz, so one pin pair carries ten thousand lights where WS2812 carries 2,048; the P4's octal SPI adds eight data lines for a large bank. The same peripheral drives one WS2812 lane on MOSI, each bit encoded in three or four SPI bits at 2.4 to 3.2 MHz by DMA with no CPU: the route for a chip with two RMT channels such as the C3, and the one every Raspberry Pi shares. Both are one driver behind a platform seam in the init, buffer, transmit and wait shape the other LED peripherals use, with a fixture profile per chip type; `PinList.h` already carries the higher cap a clocked type passes.

### Frames to a board over USB serial, the desktop as renderer

Kind: Feature · Platform: desktop

A desktop has no LED pins and reaches a board through the network drivers today. A board on a USB cable is the other route: the desktop renders and sends RGB frames over USB serial, and the board clocks them out through its own LED driver. An S3's native USB-Serial-JTAG sustains about 12 Mbit/s, which carries about 10,000 lights at 50 fps; a UART bridge (CP210x, CH340) about 1 Mbit/s, so past that Art-Net, E1.31 or DDP over Ethernet to several boards stays the scale route. It needs the board-side receiver, the serial sibling of `NetworkReceiveEffect`, and a desktop driver beside `NetworkSendDriver`.

### MoonI80 prime-only ring: no stall backstop (sibling-path gap)

Kind: Bug · Platform: S3

**Found:** 👾 Reviewer, pre-commit on the whole-frame stall fix (2026-07-22).

The MoonI80 wait-timeout backstop (`moonI80Ws2812Wait`, `platform_esp32_moon_i80.cpp`) now recovers a lost/coalesced EOF on **two** of the driver's three completion paths: the lapping ring (`nSlices > ringBufs`, oracle-gated) and the whole-frame path (`!isRing`). The **prime-only ring** (`isRing && nSlices <= ringBufs`) falls through both conditions, so a lost terminator-EOF there leaves `busy` stuck true with no recovery — and every prime/arm/transmit-ring path refuses under `busy`, the same permanent-wedge class the whole-frame fix just closed.

Mitigated in practice: prime-only fires exactly one EOF per frame (no intra-frame coalescing), so the lost-EOF trigger is far rarer than on the whole-frame or lapping paths. But it is the same defect, and per the CLAUDE.md sibling-path rule (a cross-cutting recovery that core owns for one path should cover the sibling path, not be re-implemented per case) the backstop should extend to it rather than leave a third path uncovered.

**Fix:** widen the backstop's condition so a stuck prime-only ring finalizes too — likely a single "any ring frame whose wire time has elapsed with `busy` still set" oracle that subsumes both ring branches, calling the shared `finalizeStalledTransfer`. Verify on the expander wall (prime-only = the small-strand ring config), since the ring recovery is not desktop-testable.

**Related (same file, same wall dependency):** the whole-frame backstop stops the hardware before draining the FIFO and the EOF ISR drops a firing against an empty FIFO, which rejects a late EOF that arrives after recovery. A tighter guarantee — an ISR that has *already passed* the empty-FIFO guard cannot then clear a freshly-recovered `busy` or give `wireFree` for the next transfer — would need explicit serialization (disable the EOF interrupt across finalize+rearm, or an explicit recovery/rearm state the ISR checks). The window is extremely narrow (an ISR in-flight at the instant of finalize) and stopping GDMA+LCD first already disables the source; the hardened version is an ISR-concurrency change that must be proven on the wall, not landed blind.

### A power budget the device knows about (2026-09-02)

Kind: Feature

**Measured on MM-StadBeest** (LightCrafter 16, 1440 lights, through the new `power.mls` readout):

| brightness | rail | current |
|---|---|---|
| 16 | 4.86 V | 0.9 A |
| 60 | 3.9 V | 4.3 A |
| 120 | (browned out) | |

Current is roughly linear in the control value, so the usable range is compressed into the bottom of
the travel and the top of the slider is a power hazard rather than a setting. The board crashed
twice during this session at brightnesses a user would reasonably try.

**What prevents the crash: a power budget from measured values.** A board that measures its input voltage or current dims when the voltage sags below a threshold or the current passes a limit (see the power-monitoring entry below). A board without sensing relies on safe mode and the fade-in, which bring it back reachable after any supply failure.

NOTE the rail sag above is NOT the supply's fault: an LRS-350-5 delivers 60 A, and this browned out
at 4.3 A. Roughly 0.25 ohm of series resistance in the feed, so wiring and injection points, which
is worth measuring before tuning anything in firmware.

**Measured again 2026-10-09**, full white on all four channels: 4.7 V at 0.5 A, 4.1 V at 9 A, 3.3 V at 18.4 A, held for minutes while USB powered the board. The feed is now about 0.08 ohm (2.5 m of 2 mm² speaker cable each way), about 26 W of heat at full load. Powered from the feed alone, the board hangs under that sag rather than resetting, since the chip's supply stays above the brownout level; raising that level to 2.98 V, the S3's level 3, still hung without a reset. Two power cycles bring a hung board back dark and reachable through [safe mode](../../reference/troubleshooting.md) (bench-verified). What remains is the device acting on what it measures: dimming when the input sags or the current passes a limit, on boards that measure.

### Graceful blank-on-stall: a stalled bus darkens the strip (2026-07-15)

Kind: Feature

When the bus stalls mid-frame the WS2812 strip is left holding **random / max-brightness lights** (often all-white — the all-ones failure pattern) that only a **power cycle** clears. That is a robustness gap: WS2812s latch their last received color and hold it until re-clocked or power-cycled, so a frame that dies mid-stream leaves every light past the failure point stuck bright. The give-up guard today stops *spending the render thread* on a dead bus (correct) but does nothing about the *strip's* state, so the user sees a wall of garbage LEDs and reaches for the plug.

**The fix: on give-up (and on a rebuild that SHRINKS the reachable range), clock out ONE clean all-black frame** — every lane LOW → every light receives 0,0,0 → the strip goes dark. This turns "stall = a wall of random bright LEDs until power-cycle" into "stall = strip cleanly dark," which is the honest *degraded, not crashed* state the *[Robustness](../../explanation/architecture/moonmodule.md#robustness)* rule asks for. It also covers the PO's specific case (drop `ledsPerPin` 256→128 and the abandoned 128–256 range stays lit): a full-length black frame on the shrinking rebuild blacks the whole physical strip once, no boundary to compute.

**The load-bearing caveat:** if the bus is wedged *because it cannot complete a transfer*, a black frame may not clock out either — so this is a best-effort **attempt**, not a guarantee: try the black frame on give-up; if it clocks, the strip darkens; if the DMA is truly dead, we are no worse off than today (and the rebuild-wedge fix above is the real cure for *that* class). Note it must be a genuine transmitted frame (all lanes driven LOW through the normal encode+transmit), not merely zeroing the DMA buffer — the strip only changes on a clocked frame. Pin it with a test: after `kDeadFramesBeforeGiveUp` dead frames, the driver emits one all-zero frame through the transmit seam (the mock asserts a zero frame was handed to the bus), and a subsequent recovery resumes normal content.

### Classic-ESP32 shift-register ring on raw I2S: the high-light-count classic driver

Kind: Feature · Platform: classic

**The PO wants shift-register output on classic ESP32 boards at the MAXIMUM light count**, which the whole-frame `i80` backend cannot deliver: its frame buffer must be internal RAM, which caps it near 2048 lights ([performance.md](../../reference/performance.md#multi-pin-led-driving-all-three-peripherals-128128-grid)). Above that, classic needs the **hpwit refill-ring model**: a handful of tiny internal DMA buffers (~1.2 KB total, light-count-independent) refilled by the I2S EOF ISR, which transposes the next pixel row out of a **PSRAM** framebuffer as it goes. The DMA never touches the source array — only the CPU/ISR does — so the source lives in PSRAM and internal RAM stays constant. This is how hpwit (and WLED-MM/MoonLight on his driver) reach **48×256 ≈ 12K lights at ~100 fps on classic silicon with WiFi up**, with the same '595 expander.

**Why it must be a SEPARATE driver, not a flag on `I80LedDriver`:** `esp_lcd` owns the classic I2S DMA and only does whole-frame, so the ring cannot be bolted onto the esp_lcd path — it needs a second classic driver written on the **raw I2S registers** (`i2s_ll` / the LCD-mode register file directly, below esp_lcd, the way the S3/P4 MoonI80 backend sits below esp_lcd on LCD_CAM). Its ISR is small enough to fit the classic's ~70 KB IRAM (hpwit's does), and — the key move — it registers the interrupt **without** `ESP_INTR_FLAG_IRAM` (his source comment: removed "to avoid Cache Disabled but Cached Memory Region Accessed") so the refill ISR is legally permitted to read the PSRAM framebuffer. The trade it accepts vs. our whole-frame path: it **gives up the whole-frame path's WiFi-underrun immunity** (a WiFi burst that starves the refill ISR can glitch a frame), which the ring mitigates with a tunable buffer-count cushion (`nbDmaBuffer`, hpwit's default 6). For a ≤2K-light WiFi-busy install the whole-frame i80 is still the better choice; the ring is for the >2K-light case classic cannot otherwise reach.

**Reference (study, don't copy; write fresh against our architecture):** the line-by-line source read is in [the LED-driver analysis](analysis-leddriver.md#the-classic-esp32-past-2048-lights). The S3/P4 MoonI80 ring is the closest in-tree prior art for the ring mechanics (linear self-terminating chain, per-drain refill, drain-count termination), but its refill is a task and its buffers are internal-only *because the LCD_CAM GDMA can't sustain a PSRAM read at the shift clock*; the classic I2S ring is the inverse (PSRAM framebuffer legal, ISR refill mandatory), so it borrows the *shape* but not the constraints. Do the S3/P4 **ISR-refill + `MM_HOT`** work first (it proves the ISR-refill pattern in-tree on the friendlier unified-DIRAM chips); the classic raw-I2S ring is the next tier up, reusing that pattern where IRAM is genuinely tight.

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
instance 1** and leave 0 for audio, for the reason [platform_esp32_i80.cpp](../../../src/platform/esp32/platform_esp32_i80.cpp) records,
and owning the peripheral directly
it can simply ASK for instance 1 rather than steering `esp_lcd` by parking instance 0, which is the
workaround the current backend needs. And it inherits the package-aware pin refusal: a pin the
package lacks wedges the flash cache silently, which cost a full day of bisection on the
ESP32-PICO-V3-02.

### Multicore step 2b: a ping-pong second output buffer

Kind: Feature · Trigger: workload-gated

A second output buffer, whose recoverable wait [`renderWait`](../../reference/performance.md#multicore-the-whole-output-stage-on-core-1-multicore-step-2) measures, costs one full frame buffer (~48 KB at 16K lights). A **light effect on many lights** (output-bound — a solid color or slow gradient across 16K lights) shows 6–11 ms of wait, which 2b recovers whole. So it's a real but narrow win: **gated on the product owner naming that light-effect/many-lights workload as worth the 48 KB**, not on a generic fps gain.

### Brightness belongs on a fixture's DIMMER channel, not only in the color values (WANTED)

Kind: Feature

`Correction::apply()` holds a profile's `Dimmer` channel wide open at 255 and puts brightness into
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

**The rule to implement:** when a profile declares a `Dimmer` role, route the driver's brightness to
that channel and drive the colors at full saturation; keep today's behavior (scale the colors)
only where the fixture has no dimmer. The per-light color values still carry the effect's own
shading, so an effect that paints one light dark stays dark. Watch the one case where fixture and
light are not 1:1: several light cells behind ONE master dimmer cannot express per-cell brightness
through it, so those keep the color-scaling path.

Found on the bench 2026-08-28 wiring the first moving head. Note the dimmer was not written AT ALL
before that day (the shipped `IRGB` profile could never light a fixture); writing it at 255 is the
fix that made a fixture light, not the finished design.

### Pan/tilt travel is hardcoded, and positioning is 8-bit (WANTED)

Kind: Feature

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
- **The fixture profile**: correct once two different heads run at once, but a profile is a
  *channel-role* layout today, and carrying physical travel widens what a profile means. Belongs
  with the [fixture model](backlog-layouts.md#fixture-model-moving-heads-and-beams), not before it.

**Positioning is 8-bit while the fixture offers 16.** The bench head has a fine channel for each
axis ([light fixtures reference](../../reference/hardware/light-fixtures.md)); both repeat the coarse byte, so pan resolves
to 540/256 = about 2.1 degrees per step. Across a room that is a visible jump on a slow sweep, and
it is the bigger fidelity win of the two. Needs a 16-bit path from the effect's sweep through
`FixtureChannels` to the profile's fine-channel roles, so it is the larger job.

### RS-485 and DMX-512 wired output: the physical-DMX driver

Kind: Feature

MoonLight already speaks DMX **over the network** (Art-Net / sACN via `NetworkReceiveEffect`). The missing half is **wired DMX-512 out**: driving DMX fixtures (moving heads, par cans, wired pixel controllers) directly over an RS-485 differential pair, which is what the RS-485 hardware on carrier boards like the [MHC-WLED ESP32-P4 shield](../../reference/hardware/mhc-wled-esp32-p4-shield.md) is *for*. DMX-512 is a 250 kbps async serial frame (a break + mark-after-break + 513 bytes: start code + 512 channels) shipped over RS-485 — the textbook fixture-control transport. A DMX driver would map the light buffer (or a fixture/attribute model — see the [Fixture model — moving heads, beams](backlog-layouts.md#fixture-model-moving-heads-and-beams) item below) to DMX channels and clock the frame out a UART in RS-485 mode.

**What it needs that we don't have yet:**
- **A `platform::` UART-RS485 seam.** The ESP32 UART has a hardware RS-485 half-duplex mode (`uart_set_mode(UART_MODE_RS485_HALF_DUPLEX)`) that auto-drives the transceiver's **DE/RE** (driver-enable / receiver-enable) line — the thing our current pin handling has no concept of (we drive pins as plain GPIO). A DMX driver is where DE/RE control first earns its place, and only for a **bidirectional** channel: firmware DE/RE toggling is what lets one channel switch Tx↔Rx without a hardware switch. A **fixed-transmit** channel needs none — its transceiver is hard-wired to drive. On the [MHC-WLED ESP32-P4 shield](../../reference/hardware/mhc-wled-esp32-p4-shield.md) that split is physical, with a mechanical slide switch on the one bidirectional channel. Firmware DE/RE control is what a board would need to make a channel bidirectional *without* such a switch.
- **The DMX frame timing** — the break/MAB is generated by a baud-rate switch or a GPIO toggle around the UART frame; standard, host-testable as an encoder.
- **A fixture/channel-mapping model** — trivial for a dumb pixel-per-channel strip, real work for typed fixtures (pairs with the moving-head fixture-model item; a wired-DMX driver and a network-DMX(Art-Net) input would share that fixture model).

**The channel-mapping half is unblocked.** The per-light encode path handles an arbitrary channel count ([lessons.md](../../work/past/lessons.md)). A DMX universe is exactly that model: a light with `channelsPerLight = <fixture footprint>` (16-ch moving head, 7-ch par, …), and the buffer's bytes ARE the DMX channel values. So a DMX driver's "map the buffer to channels" step is now the trivial part — it ships the light buffer's bytes straight into the 512-channel frame. What remains genuinely new is the **transport** (the RS-485 UART seam + break/MAB timing) and the **typed-fixture model** (naming which channel is Pan vs Dimmer — the moving-head fixture item), not the encode.

**Can a board drive XLR fixtures directly? Yes, with an RS-485 transceiver — that's the one required part.** DMX-512 is RS-485: a *differential* pair (D+/D−, ±2–6 V), not the 3.3 V single-ended UART the MCU emits, so an MCU TX pin can NOT wire straight to XLR. A transceiver chip (MAX485 / SN75176 / THVD-class, ~$0.50) sits between the UART and the connector and drives the differential pair; the DE/RE line (the UART-RS485 seam above) flips it Tx↔Rx. **3-pin XLR** carries it: pin 1 = ground, pin 2 = D−, pin 3 = D+. With a transceiver present, daisy-chaining ~10 moving heads (10 × 16 ch = 160, inside one 512-channel universe) over standard DMX in→out is well within the RS-485 limits (32 unit loads / 1200 m); the last fixture wants a 120 Ω terminator (a fixture/cable concern, not the MCU). So whether a catalog board can drive XLR *directly* hinges on one schematic question: does it carry an RS-485 transceiver + XLR/terminal (then yes, direct, as [the MHC-WLED ESP32-P4 shield](../../reference/hardware/mhc-wled-esp32-p4-shield.md) does), or only the WS2812 level-shifted outputs (then a ~$0.50 breakout is needed).

Sequencing: it's a **driver** (`src/light/drivers/`) + a platform UART-RS485 seam + a fixture model shared with the Art-Net path — the buffer→channel encode is already done. Plan when a DMX fixture is actually on the bench and a catalog board's `supported`/`planned` list points at wired DMX. The [PinsModule pin-assignment work](backlog-core.md#pinsmodule-strict-reject-on-add-mode) covers the RS485/DMX TX/RX/DE slot; this is the driver that consumes it.

### One encode, two readers: HLS and RTSP sharing the encoder (2026-09-22)

Kind: Restructure

The claim ([watching the output](../../reference/watching-the-output.md)) refuses the second of HLS and RTSP. The better end state is the one [the RTSP plan](../past/plans/Plan-20260922%20-%20RTSP%20video%20out%20on%20the%20P4%20and%20the%20desktop%20(shipped).md) describes and the P4 already half-implements: **one encode, two readers**, each taking what it needs from the same encoded frame, which is cheaper than either driver running alone twice over.

What makes it a design task rather than a patch: the two drivers must **agree** on geometry, fps and bitrate, and nothing makes them agree now. HLS derives its bitrate from the grid and picks an encoder by name; RTSP takes the frames raw and names no encoder. The open questions are whose configuration wins when both are enabled, what happens when one changes `targetFps` mid-stream, and whether a reader that stops should relax the shared settings. A refcount alone answers none of those, which is why the claim ships first.

Build trigger: someone wanting both streams from one device at once. Until then the claim is the honest behavior.

### Spout and Syphon as video outputs, only if proven

Kind: Feature · Platform: desktop · Trigger: only if proven

One user runs OBS → Spout → his own VLAN-tagged card driver; he asked whether MoonLight could be a Spout source. Spout (Windows, DirectX/OpenGL) and Syphon (macOS, Metal/OpenGL) share a GPU texture zero-copy, so they are lower latency and bit-exact, but they are **same-machine only**, are **two** platform implementations, and leave **Linux and the Pi with nothing**. At LED-wall pixel counts (a 256x256 wall is 65K pixels) the latency difference is far below one frame of the render loop, so it does not decide the choice; coverage does. A Spout user is also reachable through [NDI](../../reference/watching-the-output.md) in one hop, since OBS, Resolume and TouchDesigner all speak both.

Also note MoonLight renders into a CPU buffer, so a Spout/Syphon path would upload to the GPU purely to hand off, spending the zero-copy advantage it was chosen for.

### Chunked transfer (Step 4): the 16K lever

Kind: Feature

- **Chunked transfer (Step 4) — the 16K lever, and now the ONE mechanism behind three separate ceilings.** Split a frame into transactions the DMA can actually swallow, feeding them back-to-back. It was scoped as a Parlio fix; it is really a **core-path** fix, and the shift-register expander is only its third beneficiary.

  **The three ceilings it lifts, all unshifted-first:**
  1. **P4 Parlio: ~4,096 lights**, a contiguous-internal-block limit rather than the 65,535-byte cap ([performance.md § Multi-pin](../../reference/performance.md#multi-pin-led-driving-all-three-peripherals-128128-grid)). So chunking is **the only path to the 16×1024 = 16,384 lights the 16-lane widening promised.** This is the headline win and has nothing to do with shift registers.
  2. **Classic ESP32: 2,048 lights.** Its I2S DMA cannot reach PSRAM at all, so the frame must fit internal RAM. The **staged** form below lifts it: the DMA only ever reads small INTERNAL chunks that the CPU fills from a PSRAM frame.
  3. **The 74HCT595 expander.** `i80` holds the whole frame and caps near 96 lights per strand; `MoonI80` streams it ([LED drivers](../../reference/hardware/led-drivers.md#wiring-a-74hct595-pin-expander)). An add-on, and explicitly **not** the reason to build this.

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

### 16 bits at the output: what remains

Kind: Feature

- **16 bits at the output: what remains (2026-10-05).** Fine roles and the 16-bit curve table shipped, as [FixtureProfiles](../../moonmodules/light/supporting.md#fixtureprofiles) describes; the 16-bit Layer stays stashed, since it measured +56% frame cost.
  - Verify on a WS2816 strip that the dark end of a fade is smooth, with the built-in `GRB 16-bit`.
  - TM1814 and TM1914: an inverted line and a settings preamble before the pixels (8 bytes of current, 6 of mode, each followed by its inverse), as NeoPixelBus sends them; a driver protocol, not a profile.
  - UCS7604: its RGB or RGBW mode is set by configuration, so it needs its datasheet before a profile.
  - WS2916A-RGBW (Worldsemi): 16-bit GRBW with a 5-bit current gain per channel. Worldsemi's ESP32 example sends a 32-bit gain header once per frame, then 64 bits per pixel, MSB first.
    - The header holds the G, R, B and W gains, 5 bits each, with the remaining 12 bits set to 1.
    - A bit is 1.3 µs (T0H 300 ns, T1H 700 ns), and the reset is longer than 280 µs, past WS2812's.
    - Needs a per-frame header in the LED drivers, the gains as controls, and a `GRBW 16-bit` profile; check our reset against 280 µs.
  - WS2915 (Worldsemi): five channels, RGB plus two independent whites, 5-bit gain and 16-bit data, 5 to 48 V. Its frame layout is unpublished; the WS2916A's header with a fifth gain is the likely shape, to confirm from the datasheet.
  - WS2916B: RGB at 16 bits (48-bit pixels), 3.3 to 5.5 V.
  - Sources: [Worldsemi product list](https://www.world-semi.com/) and [Worldsemi's examples](https://github.com/WorldsemiLED/worldsemi-led-examples); the 4 kHz PWM reported for the WS2915 is unconfirmed.
  - Temporal dithering on an 8-bit wire, fed by the same 16-bit table.
  - Pan and tilt beyond 8 bits, once a layer slot carries motion wider than 8 bits; today their fine byte repeats the coarse one.
