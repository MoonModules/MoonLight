# Lessons

Hard-won debugging lessons and gotchas, recorded with the code that proved them. The PR-merge *carry-forward* gate writes new entries here (CLAUDE.md § Lifecycle Events). This is the **lesson** record: a bug, its root cause, and the fix. Two neighbours hold the other genres, and a lesson belongs in whichever fits:

- A durable **rule** that graduated from a lesson lives in [CLAUDE.md](../../../CLAUDE.md) or [coding-standards.md](../../contributing/coding-standards.md), not here.
- The forward-looking **design intent** of a feature is a [plan](plans/index.md).

Entries run oldest-first by branch. A lesson fully absorbed into a rule doc or the code is pruned (per *Mandatory subtraction*): the git history is the permanent record, this file is the working narrative on top.

## Lessons from the ESP32-S3 N16R8 (DevKitC) enablement branch

Three non-obvious failures adding native-USB S3 support, all with the shape "symptom looks like X, root cause is elsewhere."

1. **USB-Serial-JTAG ≠ UART0 on ESP32-S3.** The Dev's USB-C wires through the built-in USB-Serial-JTAG peripheral, not an external bridge to UART0. IDF's secondary console (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`, default for S3) mirrors stdio both paths, so boot logs appear and the developer assumes UART0 works. The Improv-listener-on-UART0 was deaf. Fix: install BOTH drivers and read both transports; `#if SOC_USB_SERIAL_JTAG_SUPPORTED` keeps classic builds clean. Don't make it compile-time-per-board, the same binary should work with either wiring.
2. **CORS preflight is silent on the client side.** A cross-origin POST with `Content-Type: application/json` triggers an OPTIONS preflight; if the device returns 405 to OPTIONS, the browser silently drops the POST, no error, no network line, no console message. Burned a session diagnosing what looked like a fan-out bug. Fix: always implement OPTIONS (204 + `Access-Control-Allow-Origin: *`, `-Methods`, `-Headers: Content-Type`); verify with `curl -X OPTIONS …` returning 204 not 405.
3. **Cached "last applied" TX-power went stale when the WiFi stack restarted.** `appliedTxPowerSetting_` skipped redundant `esp_wifi_set_max_tx_power` calls, but an AP→STA cascade / reconnect / AP shutdown resets the radio's TX-power while the cached value stayed equal to the desired one, so `syncTxPower()` short-circuited and the cap never re-landed. Fix: every `wifiStaStop()`/`wifiApStop()` also invalidates the cache (`= -1`).

## A static "current instance" pointer needs re-election, not just claim/vacate

`AudioModule::latestFrame()` hands effects the active mic via a process-wide `static AudioModule* active_`. "setup() claims, teardown() vacates" silently breaks with **two** mics: removing the one holding `active_` leaves the seat null while a second running mic sits captured-but-unread, and every audio effect goes silent. Fix is a three-part protocol: the first live module claims in `setup()`, `teardown()` vacates, and any running module **re-claims an empty seat in `loop()`**, so the survivor takes over on its next tick for any add/remove order. (`unit_AudioModule` pins the two-mic first-wins + re-election.) A tempting CodeRabbit "fix" (gate the claim on `inited_`) was rejected: a claimed-but-uninited module publishes valid *silence* (the documented contract), and a mic-less board running `simulate` publishes synth frames without being `inited_`.

## An "Effect-role" module is not guaranteed to be an `EffectBase`

DemoReel hosts a child effect and needs its `dimensions()` to extrude it. The first cut did `static_cast<EffectBase*>(current_)->dimensions()` each frame on the `MoonModule*` child, which **crashed** (SIGBUS in RTTI/vtable) when the eligible list included a test `EffectStub` that registers with `ModuleRole::Effect` but is a bare `MoonModule`. Two lessons: role is a registration property, not a type guarantee (a downcast keyed on role is UB); and the data was already available RTTI-free, the factory probes `dimensions()` at registration via `if constexpr` and stores it, so `ModuleFactory::typeDim(index)` gives it (ESP32 builds `-fno-rtti`, so `dynamic_cast` isn't even an option).

## uint16 intermediate overflow blanks the display, and a status check doesn't prove the render works (MultiplyModifier)

`MultiplyModifier` at 8×8×4 black-screened the no-PSRAM Olimex while desktop showed it working. `Layer::rebuildLUT` computed `maxDest = logicalCount * mod->maxMultiplier()` in `nrOfLightsType`; on no-PSRAM that's `uint16_t`, and `256 * 256 = 65536` **wraps to 0**, so the LUT was sized to ~nothing and almost every light mapped nowhere. On desktop (`uint32_t`) the product fits, so the bug was invisible. Fix: compute in `uint64_t`, clamp to the ceiling, narrow back.

The harder lesson is verification: the agent's hardware test asserted the Layer **status string** ("16×16×1"), which was correct, while the *render* (driven by the corrupted LUT) was black; the product owner caught it by eye. A correctness test for a mapping/effect must assert the **buffer or LUT is non-empty with expected coverage** (the regression counts LUT destinations == physical light count), not just the declared dimensions. A width-sensitive bug is invisible on the uint32 desktop build; such paths need a uint16-typed unit test or hardware confirmation.

## Lessons from the parallel-LED-driver consolidation branch

The consolidation (three CRTP driver classes → one `ParallelLedDriver` selecting a `LedPeripheral` backend at runtime) surfaced four robustness bugs — most on the bench, where a desktop test could not reach them — each about a *cross-cutting rule reaching the wrong object*, and all found only because a driver's state lives on a swappable backend now.

- **A per-parent `quiesce()` misses a worker that walks the WHOLE tree.** Replacing a **layout** on a split-render device (the core-1 encode worker running) was a LoadProhibited use-after-free. Core already had a "stop the worker before a structural mutation" rule — `MoonModule::removeChild`/`replaceChildAt` call `this->quiesce()` — but that only stops a worker the *mutated node's parent* owns. The encode worker is owned by `Drivers`, yet it ticks the drivers and a driver walks the entire layout/layer tree (`PreviewDriver::sendFrame → Layouts::placeLights`), so freeing a node in a *sibling* subtree (a layout) never quiesced it. The lesson: when a worker reads **across** subtrees, quiescing the mutated node's own parent is not enough — the guard must reach the worker wherever it lives. Fix: a core `quiesceForMutation()` that also fires a `quiesceRenderHook_` (a function-pointer seam mirroring `setSchemaChangedHook`), wired once in `main.cpp` to `Drivers::quiesceRenderSplit()`, so core stays domain-neutral. And the completeness trap: the first fix covered `add`/`remove`/`replace` but missed the **fourth** mutator, `moveChildTo` (the drag-reorder UI) — a cross-cutting rule lifted into core must cover *every* path, not three of four. (`unit_Drivers_rendersplit` pins both a layout mutation and a reorder through the real worker thread; each is verified green→red.)

- **A control whose backing variable moves between objects is lost on reload unless persistence re-binds first.** After a watchdog reboot the giant wall's `clockPin` (and the whole MoonI80 ring cluster) reverted to their defaults — a reboot silently changing a control, which should never happen. Root cause: those controls live on the *peripheral backend* object, and which backend is live depends on the `peripheral` control's value. On reload `FilesystemModule::applyNode` overlaid the saved values in list order: `peripheral` got written but did **not** swap the live backend, so `clockPin` was written to the *default* backend's member — then the later swap to the saved peripheral discarded that backend, reverting clockPin to its constructor default. The lesson: when a module's **control set depends on one of its own control values**, a single overlay pass writes the value-dependent controls onto the wrong (about-to-be-replaced) objects. Fix: overlay → `rebuildControls()` (which re-runs `defineControls`, swapping the live backend to match the just-applied `peripheral` and re-binding the list to the *right* members) → overlay again. General (any value-dependent control set), gated by `rebuildControls`'s schema-hash so it no-ops for ordinary modules, and the second overlay is idempotent. Invisible on desktop (no real backends link, so no swap); found only on a MoonI80 board whose persisted peripheral differs from the constructor default. (`unit_FilesystemModule_persistence` pins it with a value-dependent mock, verified green→red.)

- **A `break` on the first unresolvable persisted child dropped every module after it.** A user's driver "spontaneously" vanished on reboot. Cause: `FilesystemModule::applyNode` reconciled saved children positionally (`<i>.type` must match live child `i`) and `break`ed the whole loop on the first entry it couldn't place: either a code-wired sibling whose boot order differed from the saved order, or (the real trigger) a **renamed/removed type** (this device's file still held pre-consolidation `MoonLedDriver`/`MultiPinLedDriver` entries). The `break` then dropped every *later* JSON child, so one dead entry took out the user's real modules recorded after it. The lesson: a positional reconciler must be **fault-isolating**: a single un-placeable entry skips itself and keeps going, never aborts the tail. Fix: decouple the JSON index from the live position (`i` vs `pos`): an entry that produces no live child (`ModuleFactory::create` returns null, or a code-wired child sits in a stale slot) is skipped without advancing `pos`, so the file's later user modules still map to the right index. This also makes the documented break "unknown type drops, the rest stay" behavior hold. User-module order still round-trips (user modules are created fresh in file order; only code-wired singletons, whose order is cosmetic, may reorder, self-correcting on the next save). Invisible until a container gained a *second* code-wired child (before that there was never an order mismatch) AND a device carried a renamed type. (`unit_FilesystemModule_persistence` pins both the reordered-code-wired-siblings case and a user-reorder round-trip, verified green→red.)

- **A live control-swap that frees a resource a worker reads needs the same quiesce as a tree mutation.** The `peripheral` Select frees the old bus backend (`delete peripheral_`) on switch, and the core-1 encode worker dereferences that backend (`busBuffer`/`busTransmit`) — so a live swap during the render split is a use-after-free, the same class as the tree-mutation crash, but it does NOT pass through `MoonModule::quiesceForMutation` (it is not a child-array mutation). `deinit()` drains the bus DMA but not the worker thread. The lesson: the "stop the worker before freeing what it reads" rule is not only about child-array mutations — any live free/reuse of worker-visible state needs it. Fix: fire the same render-worker hook (`MoonModule::notifyQuiesceRender()`, the public sibling of `notifySchemaChanged()`) at the top of `swapPeripheral`. Caught by the pre-merge whole-branch review, not the per-commit reviews — the swap and the mutation-quiesce fix are in different commits, and only the cumulative view sees "the branch added a quiesce rule but left one live-free path uncovered." (`unit_ParallelLedDriver_swap` pins that the hook fires before the backend is freed, verified green→red.)

- **MoonI80's whole-frame double-buffer wedged the bus after ~2 frames; the fix was to NOT double-buffer that path, not to patch the race.** Switching to the MoonI80 peripheral with `doubleBuffer` on froze the LEDs at ~200 ms/frame (5 fps) after a couple of clean frames, recovering the instant `doubleBuffer` was turned off. Root cause: MoonI80's own-GDMA whole-frame path serialized two in-flight buffers with a hand-rolled handshake — a **binary `wireFree` semaphore the EOF ISR gives unconditionally, plus a blind non-blocking pre-drain in `busTransmit`**. A give-when-already-1 is lost (binary), and the blind `xSemaphoreTake(wireFree, 0)` can swallow the very completion the next blocking wait then waits for, so every subsequent frame times out at the `kWireFreeTimeoutMs = 200` backstop — deterministic freeze, not a flake. i80/Parlio never had it: they route through a real transaction queue (esp_lcd / the Parlio driver), so a second transfer is absorbed for them. The lesson: **for MoonI80, whole-frame double-buffering overlaps ~150 µs of encode with a ~2 ms wire — a ~7% win not worth a freeze-prone concurrent handshake; its real speed is the streaming ring, not two whole-frame buffers.** So the fix subtracts rather than patches: a `LedPeripheral::supportsDoubleBuffer()` seam (default true; MoonI80 false), the orchestrator gates the second-buffer request on it, and the `doubleBuffer` control hides on a peripheral that can't run it (computed *after* the peripheral swap, or an i80→MoonI80→i80 cycle leaves the control stuck hidden). Method note: the freeze was reproduced deterministically on the bench (works-then-wedges-forever is traceable, unlike a flake), but the serial-open reset on native-USB S3 boards defeated in-place instrumentation — the decisive evidence came from the `tick:` line over HTTP-triggered state, and from measuring that double-buffer buys ~1 ms on a small frame (so removing it costs almost nothing). (`unit_ParallelLedDriver_doublebuffer` pins that a `supportsDoubleBuffer()==false` peripheral stays single-buffer with the toggle on, green→red; `scenario_peripheral_switch` guards the freeze live — a ~200 ms tick after switching to MoonI80 with double-buffer on fails it.)

## Lessons from the LCD_CAM WS2812 driver bench debug (LcdLedDriver, S3)

Bringing the 8-lane LCD_CAM driver from "compiles and ticks" to "strip animates" took three stacked root causes, each masked by the one before; every layer of indirection hid a failure the layer above couldn't see.

1. **The i80 peripheral requires ALL `bus_width` data GPIOs, a partial bus never exists.** `esp_lcd_new_i80_bus` rejects `GPIO_NUM_NC` entries, so a 1-pin config never initialized: no bus, dark strip, while the UI showed an enabled driver. Fix: demand exactly 8 pins and report `LCD bus needs exactly 8 pins`; unused lanes take `0` and idle LOW.
2. **"Capacity still fits" is not "config unchanged."** `reinit()` skipped the bus rebuild when the DMA buffer was big enough, correct for grid resizes, wrong for pin edits (moving lane 0 from GPIO 13 to 18 keeps the frame size, so the bus kept clocking the OLD pins). Fix: record the pin set the live bus was built with, compare on every reinit, any difference forces the rebuild. Same family as the S3-DevKit stale-cache lesson: the cached check was true about the wrong invariant.
3. **Gate on `CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED`, NOT `CONFIG_SOC_LCD_I80_SUPPORTED`.** The classic ESP32 also defines `SOC_LCD_I80_SUPPORTED=1` for its unrelated I2S-LCD peripheral, so gating on it wired `LcdLedDriver` at boot on classic, and `esp_lcd_new_i80_bus()` hung the watchdog → **boot-loop on every classic ESP32**. The correct macro is defined only on chips with the real LCD_CAM (S3/P4). Bisect-proven, hardware-verified.
4. **A self-test that passes while the device fails is telling you what the test doesn't cover.** The first loopback transmitted a 3-byte synthetic pattern through a 136-byte transfer and PASSED while the strip washed out max-white. Upgrading the test to transmit the driver's REAL frame (5.4 KB, multi-descriptor GDMA chain, sustained cadence) and bit-verify the whole capture eliminated two suspects in one run, leaving timing.
5. **Modern WS2812B reads a 412 ns "0" as "1", T0H max is ~380 ns on newer revisions, and 3.3 V direct drive eats the margin.** The classic 2.4 MHz encoding (slot ≈ 416 ns) decoded perfectly on RMT RX yet rendered max white with flicker; the RMT driver's 350 ns zeros ran clean, isolating timing. Fix: pclk 2.67 MHz → 375 ns slots ("0"=375, "1"=750, bit 1125 ns), inside every WS2812B revision's window; latch pad resized to keep ≥300 µs. The lineage gets away with 416 ns behind a 74HCT level shifter.

Bench notes: a board that drops STA mid-session falls back to softAP silently, poll reachability before reading an unanswered API call as a wedge. The differential test that cracked it twice: drive the same pin/strip with the proven RMT driver, it exonerates wiring, power, and strip in one move.

## Diagnosing LED flicker: eliminate firmware with hardware tests before blaming (or fixing) the wire

A classic ESP32 driving WS2812 on RMT showed random wrong colors on LEDs the effect left black. Instead of guessing (WiFi, buffering, timing), a four-step elimination each a *measurement*: (1) capture the source/preview buffer, clean, so corruption is downstream of the logical buffer; (2) whole-frame loopback self-test through a jumper, bit-exact PASS even under WiFi load, so the firmware/peripheral is innocent; (3) sweep `txPowerSetting` 20→1 dBm, flicker constant, so it's NOT WiFi coupling; (4) check pulse timing, 350/700/1250 ns spec-exact, not a timing-margin bug. Every firmware cause eliminated by test, the remaining cause is the physical data path, and "constant regardless of TX power" fingers signal integrity, a missing 3.3→5 V level shift. Fix is hardware (documented in [LED signal integrity](../../how-to/led-signal-integrity.md)).

**Red-herring note:** the flicker's *appearance* shifted (blue-only → random) after an unrelated `lightPreset` RGB→GRB default change plus a pin swap. The electrical fault was identical; only the color mapping changed. A changed symptom is not proof a code change caused it.

## ESP32-P4 support, round 1, per-board Ethernet pin config, and the P4's WiFi reality

Adding the Waveshare ESP32-P4-NANO (round 1 of 4: board + Ethernet-only).

- **The P4 has no native WiFi.** `SOC_WIFI_SUPPORTED` is absent on esp32p4 (it has EMAC, RMT, LCD_CAM, Parlio, no radio). WiFi comes from an on-board ESP32-C6 over SDIO via `esp_wifi_remote`/esp-hosted (a managed component, not in mainline v6.1-dev). Round 1 ships Ethernet-only. C6 SDIO pins on the P4-NANO (so round-2 Parlio avoids them): CLK 18, CMD 19, D0-D3 14-17, C6 reset 54.
- **Ethernet pins became a per-target compile-time config, not scattered `#ifdef`s.** `ethInit()` had Olimex RMII/PHY pins baked as literals; the P4-NANO needs IP101 PHY addr 1, MDC 31, MDIO 52, reset 51, and an *external* 50 MHz RMII clock fed IN on GPIO50 (`EMAC_CLK_EXT_IN`, opposite of Olimex's `EMAC_CLK_OUT`). An `EthPinConfig` struct + `constexpr ethPins = isEsp32P4 ? {…} : {…}` lives in `platform_config.h`; `ethInit` reads it. Keeps the platform-boundary rule (`if constexpr`, not `#ifdef` in domain code).
- **The IP101 PHY driver is a managed component in IDF v6** (`espressif/ip101`); the generic PHY (Olimex LAN8720) stays in core, so only the P4 build pulls it, behind `if constexpr (ethPins.isIp101)`. A build-script trap: the eth-fragment detector matched only `.eth`, so `sdkconfig.defaults.esp32p4-eth` would have set `MM_NO_ETH`, the matcher now accepts both `.eth` and `-eth`.

## ESP32-P4 round 2, Parlio LED driver: a simpler peripheral, the same encoder

Adding `ParlioLedDriver` (round 2 of 4) was mostly *subtraction* from the LCD driver.

- **Parlio is simpler than the i80 bus:** no sacrificial WR/DC pins (Parlio clocks internally via `output_clk_freq_hz`, so no `clockPin`/`dcPin`), and no exactly-8-pins rule (Parlio takes `data_gpio_nums[]` with unused = `-1`, so 1-8 lanes work).
- **The encoder is shared, not duplicated.** `LcdSlots.h::encodeWs2812LcdSlots` outputs one bus byte per slot, and a Parlio bus byte is byte-identical to an i80 one, so `ParlioLedDriver` reuses the same encoder and `unit_LcdLedEncoder.cpp` test, zero new encode code.
- **One Parlio constraint:** `data_width` must be a power of two, ≤ `SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH`. Always create at `data_width = 8` (matching the encoder's 8-bit byte), unused lanes' GPIOs `-1`.
- **Clock math is identical:** Parlio's `PLL_F160M` ÷ 60 = 2.67 MHz = the same 375 ns slot the LCD driver settled on for the T0H-margin reason.
- **Default pins must dodge the P4's strapping GPIOs.** The first cut defaulted `36,37,38,…` but GPIO 34-38 are strapping pins (boot-mode control). It inited fine on the bench (nothing drove them during boot) but defaulting LED *output* onto strapping pins is a latent footgun. Fixed to `pins="20,21,22,23,24,25,26,27"` with all 64 lights on lane 0 (the other 7 idle LOW at zero cost, the parallel DMA transfer time is set by the *longest* lane, not the lane count). Clear P4-NANO GPIOs after Ethernet, C6 SDIO, I2C, and strapping: 20-27, 32-33, 39-48. Pull the strapping-pin list from the datasheet before picking defaults for a new chip.

## Audio input (INMP441 mic), ship the manual core, design fresh from the datasheet

The first audio-reactive capability: `AudioModule` (a SystemModule Peripheral) reads an INMP441 I²S mic and publishes an `AudioFrame` (level + 16-band spectrum + peak) that `AudioVolumeEffect`/`AudioSpectrumEffect` consume.

- **Two seams, everything else host-tested.** Only the I²S read and the FFT *kernel* sit behind the platform boundary (`i2s_std` + esp-dsp's float `dsps_fft2r_fc32`). All signal math (DC strip, RMS, Hann window, magnitude→16-band log map) is header-only domain code. The desktop `audioFft` stub is a real naive O(n²) DFT, so the whole pipeline runs in CI on synth sines. esp-dsp float (not fixed-point) is right on an FPU chip.
- **Effects reach the producer via `AudioModule::latestFrame()` (a static accessor), not a boot setter**, because an audio effect can be added through the UI after boot and a setter only wired the boot instance. The active mic registers in `setup()`, clears in `teardown()`, returns a static silent frame when there's no mic.
- **Two hardware-only bugs, now pinned:** a missing `registerType<AudioModule>` made `create(...)->markWiredByCode()` deref null and boot-loop; the I²S read blocked the tick ~7.7 ms at a 20 ms timeout (fixed to non-blocking + a cross-tick sample accumulator, dropping to ~400 µs). The INMP441 is on the **left** I²S slot here (right reads empty), the first bench suspect when level floors with sound present.
- **Shipped the manual core; the adaptive conditioner was prototyped and removed.** Auto noise-floor + AGC + smoothing needs tuning in a *quiet* room; the dev environment (a campground van with strong varying low-frequency rumble) was the adversarial worst case that kept it from settling. Land the manual core, treat adaptive auto-tuning as its own increment, tune it where the noise floor is real-quiet. Also: `level` is overall RMS (independent of the FFT), don't derive it from the bands or it stops fluctuating with volume.
- **Designed fresh from the datasheet + textbook DSP, not a prior project.** The datasheet made even a behaviour-reference unnecessary: a flat ±3 dB mic has no per-frequency error to correct, so the hand-tuned band-correction table years of prior-project work produced was the wrong tool; the textbook defaults sufficed. The DSP choices and *why* live in [AudioService.md](../../moonmodules/core/moxygen/AudioService.md).

## ESP32-P4 round 3, WiFi via the C6: the abstraction the earlier round feared wasn't needed

Round 1 predicted round 3 would need a WiFi abstraction seam; the actual implementation was far smaller.

- **`esp_wifi_remote` is API-compatible, so there was no seam to build.** You still call `esp_wifi_init()`/`esp_wifi_connect()`/…; the component forwards them to the C6 over SDIO. The entire WiFi platform layer (~230 lines) compiles unchanged. The only new code is a two-call prelude (`esp_hosted_init()` + `esp_hosted_connect_to_slave()`) run *before* `esp_wifi_init()`, behind `if constexpr (platform::usesRemoteWifi)` (= `isEsp32P4 && hasWiFi`). Before designing a two-backend abstraction, check whether the vendor already made them API-identical.
- **Init ordering is the whole risk.** esp_hosted must be fully up before anything touches the WiFi stack; wrong ordering surfaces as NVS errors / asserts / a silent hang, not a clean error. The prelude-before-`esp_wifi_init` placement is the mitigation and the first thing to check if a P4-wifi boot misbehaves.
- **Pulled P4-only via a `rules` gate** (`if: "target == esp32p4"` in `idf_component.yml`), so other targets never fetch or compile the managed components.
- **A deliberate v6.0-floor exception:** these components live outside mainline v6.0. The floor rule's documented-exception path applies, accepted consciously, recorded at every introduction site, scoped to the P4. A floor rule with a documented-exception path beats a rigid one.

## RMT timeout: don't cancel a stuck transfer, the cancel crashes classic ESP32

A deferred "fuller error handling" item for `rmtWs2812Show`/`rmtWs2812Wait` (🐇 CodeRabbit PR#17) was mostly already done and partly actively harmful. Most had landed in the multi-pin work: `rmtWs2812Transmit` already returns the `rmt_transmit` result, and `RmtLedDriver::loop()` already tracks `started[]` so it only waits on channels whose transmit succeeded. The remaining item, cancel the in-flight transfer on timeout via `rmt_disable()`, is a trap: `rmt_disable()` while a transmission is active triggers an **interrupt-WDT panic on classic ESP32** (espressif/esp-idf#17692; classic-only). That trades a self-healing dropped frame for a crash, strictly worse. The right change was to NOT add the cancel and document why the current code is already right (next tick re-encodes and re-transmits; a still-busy channel fails its `rmt_transmit` cleanly, `started[]` skips the wait). A deferred-improvement note is a hypothesis, not a spec, verify the improvement is real AND safe on every target before implementing.

## Pin defaults: assign one only when it cannot do harm

A mic-less **classic ESP32** boot-looped (TG1WDT_SYS_RESET at ~736 ms, silent hang). Bisect (clean-built known-good still looped; disabling AudioModule wiring booted clean) fingered AudioModule: auto-wired via `addChild` + `markWiredByCode()` (gated on `platform::hasI2sMic`), it ran `setup()` → `reinit()` → `audioMicInit()` → `i2s_channel_enable()`, which on the classic's older I²S driver **blocks forever** when no mic clocks the pins. The P4 was never affected (newer I²S returns silence or fails cleanly).

The fix (a design fix, not a band-aid): don't auto-wire AudioModule (register it in the factory so it's user-addable); default the mic pins to unset (0); `reinit()` no-ops on any unset pin (`setStatus("set …"); return;`). Classic then boots 191 FPS, 0 WDT resets. This drove the general rule now in [MoonInstaller, config provenance](../../explanation/architecture/mooninstaller.md#config-provenance-mcu-devicemodel) and [coding-standards.md § Defaults](../../contributing/coding-standards.md#defaults): chip-/board-fixed pins (RMII Ethernet) *must* default (omitting them is a chicken-and-egg lockout); user-soldered pins (mic, LED strands) stay empty until set. Never auto-run a peripheral whose init can block on absent hardware.

## Live reconfiguration falls out of the prepare-pass for free, MoonLight's "initless" goal, a different mechanism

MoonLight reconfigures every module live the instant a control changes (pins, leds-per-pin, output protocol, mic pin/rate), no reboot. The design note lives in [architecture.md § Live reconfiguration](../../explanation/architecture/moonmodule.md#live-reconfiguration-every-change-applies-on-the-next-frame). The lineage is MoonLight's "initless drivers" (a driver with no `addLeds`/`initLed` step, reading a mutable Context at `show()` time). MoonLight reaches the same outcome by a different mechanism: our drivers *do* have an explicit rebuild (`RmtLedDriver::reinit()`, the i80/Parlio DMA-bus rebuild), but it's driven by the generic tier-3 `onBuildState()` sweep, not hand-built per driver, so any module returning `true` from `controlChangeTriggersBuildState` inherits live-reconfig, spanning drivers, audio, effects, layouts, modifiers, and network I/O alike. Credit the lineage for the idea, but name the property by what the user sees when the mechanism differs.

## Lessons from the catalog-driven installer branch (3-layer device model)

The installer was reworked so a board catalog sets a device up for its hardware at install time. The mechanics live in the installer README; these are the principles.

- **Inject from data, don't bury in code (vs MoonLight's `ModuleIO.h`).** MoonLight hardcoded ~20 boards' pin presets in firmware C++ behind `#ifdef CONFIG_IDF_TARGET_*`; adding a board is a recompile and every binary ships every board's table. MoonLight injects the same info from the catalog after flash, the firmware is a generic engine, the data specialises it. Adding a board is a JSON edit; board definitions become community-contributable data.
- **Investigate before building, the device side was already done.** The original plan was a new `POST /api/preset` endpoint; investigation found three install clients already fan a catalog's controls out as `POST /api/control` calls, and the real gap (a control write 404s on a fresh flash where the module doesn't exist) was solved by the already-existing idempotent `POST /api/modules` the clients weren't driving. A day of mapping the existing code replaced a new core endpoint (and a forbidden JSON-array parser).
- **`loopbackTxPin`, verify the claim against the code before deciding twice.** A `loopbackTxPin` override was proposed, dropped on "it only fits RMT's single pin, not Parlio/Lcd's lane array," then re-added after reading `ParallelLedDriver::runLoopbackSelfTest`: the Parlio/Lcd loopback only drives lane 0, so a single TX override substituting for lane 0 works on all three drivers. The "doesn't fit lane arrays" objection was an assumption, not a fact.
- **Board vs Device is a completeness spectrum, not two schemas.** A carrier/shield PCB an ESP32 module plugs into is a Board; a vendor-finished all-in-one (QuinLED Dig-2-Go) is a Device, but they're the *same* catalog entry shape, a Device is just a Board entry with more optional `modules`/`controls` filled in. So no separate-schema `devices.json`. A board's pins fall into three categories: *always-fixed* (LED outputs, status LED, default freely), *board-optional* (a populated-or-not W5500/IR, can't default blind because the same board name ships both ways), and *user-soldered* (always unset).
- **Per-board capability spec'n'test loop.** Each board's pin-layout `image` and product `url` are *inputs* to a repeatable loop: read capabilities off the image + link; for a capability we offer (I²S mic → `AudioModule`) wire it into `modules`/`controls` with real pins; for one we don't (IR receive) write a proposal, spec it, create test scripts, iterate, *then* add it. An un-implemented capability is a recorded proposal, not a half-wired control. *Specs before code* at board granularity.
- **Drivers became catalog-added, with an OTA nuance.** LED/network drivers stopped being boot-wired (only `Preview` stays, since it needs the HTTP broadcaster the catalog can't supply); each board declares its driver(s) in `modules`. A fresh-erased board boots with `Drivers = [Preview]` only. The nuance: an OTA update *without* erase keeps a device's previously-persisted drivers (saved config the new firmware reloads), so "out-of-box" and "after-update" differ.
- **A persistence overlay must distinguish "key absent" from "value 0".** The runtime-Ethernet-PHY work moved pin/PHY config into persisted controls with non-zero per-chip defaults (P4 IP101 `ethType` 2). `applyControlValue` used `json::parseInt`, which returns 0 for an absent key, indistinguishable from a real 0, and wrote that 0 under the Clamp policy, so loading an older `<Module>.json` clobbered the default. On the P4 this zeroed `ethType` (2→0=none): link LEDs on, no DHCP. Invisible on classic (eth defaults mostly 0) and on `main` (still read `constexpr ethPins`). Fix: a `json::hasKey()` guard in `applyControlValue`. Any "control resets to 0 after reboot" symptom is a persistence-overlay smell. The decisive move was a `std::printf` over the P4's secondary USB-Serial-JTAG console after a `git worktree` bisect.
- **A GPIO pin is its own control type (`ControlType::Pin`), not an overloaded int16.** Pins added as `addInt16` `-1..48` rendered as a *slider* (meaningless for a GPIO) and the cap excluded the P4's high pins (MDIO 52, clk 50); dropping the range didn't help because the UI's `int16` case always draws a slider. Fix: a dedicated `Pin` type (`int8_t` storage, a GPIO never exceeds ~54; −1 = unused; the UI renders a plain number input keyed off the `"pin"` type string; min/max are a server-side write-clamp only). Every future pin control migrates to `addPin` for free.
- **`deviceName` (identity) vs `deviceModel` (product) vs board (bare PCB), one term was doing three jobs.** "Board" had meant per-unit network identity, the catalog key, AND the bare PCB. Untangled: `deviceName` is the per-unit identity (drives mDNS/AP/DHCP, RFC-1123-coerced); `deviceModel` is the hardware product (catalog key, spaces allowed, never a hostname); "device" is the umbrella; "board" means only the bare PCB. Drove the BoardModule→SystemModule fold, the `board`→`deviceModel` rename (SET_BOARD→SET_DEVICE_MODEL, byte 0xFE unchanged), and the eth pin-map clarification (driver = firmware, pin map = firmware-seeded but deviceModel-authoritative).
- **"Improv = REST over serial", one apply-core, two transports.** The HTTPS installer couldn't POST to an `http://` device (mixed-content), and the `?deviceModel=` pull only ran if the user opened that exact link. Fix: the installer owns the USB serial port during provisioning, so push config over it as the same REST ops, a new `APPLY_OP` (0xFC) Improv vendor RPC whose payload is the same JSON a `POST /api/modules`/`/api/control` body carries. On device it routes to one transport-free apply-core (`HttpServerModule::applyAddModule/applySetControl/…`) the HTTP handlers also call, so serial and network execute identical code. This deleted the whole browser handoff. The hard part (chunk reassembly + out-of-order/duplicate sequence guard) was extracted to `src/core/util/ImprovOpReassembler.h` (header-only state machine) so it unit-tests on desktop, and the frame builders (device C++, Python, JS) are pinned by one shared golden vector in `test/python` + `test/js`. A hard mechanism buried in a platform `.cpp` that "can only be tested on hardware" is a smell, extract its pure core.
- **A periodic re-broadcast to let late joiners catch up is a hack wearing a keepalive costume.** The 3D preview re-sent the whole coordinate table every ~1 s "so a client that connected after the last rebuild catches it", rebuilding the full table from the layout every tick-second forever, on the hot path, whether or not anyone connected or anything changed. The correct construct is event-driven: send the table when it changes (`onBuildState`) and when a client asks (a new WS connection bumps `BinaryBroadcaster::clientGeneration()`, which `PreviewDriver::loop()` watches), strictly *less* code and zero idle cost. It sneaked past review because it worked in casual testing and its cost was invisible until a later change made each rebuild heavier. "Re-send periodically so it eventually syncs" is the polling-instead-of-events smell. Guard: a test that advances the clock several seconds with no client change and asserts the table is NOT re-sent.
- **When a working seam regresses after your "fix," suspect the fix, and measure with a tool faithful to what the user sees.** A later attempt to route the coordinate table + downsampled frame through the resumable send path (removing the synchronous spin-and-close) regressed every board into an intermittent stall through several variants. Three compounding lessons: (1) stop at the first failed fix on a working path (revert to known-good at attempt two, per *Anti-stalling*); (2) a plain one-shot WebSocket probe *gave up on close* where a browser *reconnects*, so it reported stalls users never saw and missed blips they did, the fix was a browser-faithful probe (`moondeck/diag/preview_health.py`: reads binary frames, 25 s keepalive ping, auto-reconnect, exactly `app.js`'s `connectWs`); (3) a **38-hour-old desktop binary** still held port 8080 so the freshly-built one couldn't bind, before bisecting a "rebuild didn't fix it" bug, confirm the artifact under test is the one you built (check process uptime / `build` timestamp / what's bound to the port).
- **Don't hold a vendor library's async handle across your own event loop, it races the library's internal timers.** The mDNS *browse* (peer discovery, distinct from advertise) used `mdns_query_async_new`, whose handle `DevicesModule` held across ticks and polled with a 0 ms timeout. The mDNS component's own task frees that handle's queue when the 3 s window expires, so a poll landing after expiry asserts on a freed queue (`xQueueSemaphoreTake queue.c:1709`), intermittent and grid-size-sensitive (a bigger grid widens the gap). A first fix (a cancel-before-mutate guard, assuming a service-table mutation tore it down) did nothing, because the freeing party is the expiry timer, not our code. Real fix: replace start/poll/stop with one synchronous `mdnsBrowse()` holding no handle across ticks. Synchronous `mdns_query_ptr` blocks the full window (~80 ms, charged to the tick), so throttle: browse one service type every ~8th tick with a ~60 ms timeout. A library's async/iterator handle is only valid between *its* lifecycle events; an intermittent load-dependent crash in a vendor backtrace is a lifecycle race, not a component bug (find the *actual* concurrent actor before fixing). Hardware-only; the reproduction (concurrent WS churn at a large grid) is the proof.
- **A dead control that was always meant to be functional belongs in the mechanism that already expresses it.** Six persisted-but-ignored Layer controls (percent region carving) were due to be wired into `rebuildLUT`; the product owner's question, *can a modifier already do this?*, was the better path. `ModifierBase`'s two virtuals (`logicalDimensions`, `mapToPhysical`) express carving exactly, so it shipped as a modifier and the Layer controls were deleted. The implementation forced the boundary rounding: inclusive-ceil made two abutting layers (0..50, 50..100) overlap by one pixel at the seam, so the product owner chose half-open `[start, end)` (with a min-1-pixel floor). "Make the default fastest" is best met by making the default the *absence* of the feature (full coverage = no modifier = the existing fast path). Reach for half-open intervals whenever regions tile a space.

## ESP32-S31 RGMII Ethernet bring-up: four bugs between "code compiles" and "link up"

Bringing up the S31's on-chip 1 Gb RGMII Ethernet took four fixes, none of them the C++, each a layer *below* the feature code that a build can't catch. For a hardware bring-up, "it compiles" tells you almost nothing; the truth is only in the boot log on the actual board.

- **A `MM_NO_ETH`-style gate keyed on a *filename* silently omits a new board.** `build_esp32.py` decided "does this firmware have Ethernet?" by pattern-matching the sdkconfig fragment *filename* for `.eth`; the S31 enables its EMAC in `sdkconfig.defaults.esp32s31` (no `.eth`), so the gate compiled the `ethInit()` stub and the board fell back to WiFi with zero eth log. A gate asking "does X enable feature Y?" must read what X *contains* (`CONFIG_ETH_USE_*=y`), never what it's named.
- **A schematic/reference pin table is a hypothesis until the boot log confirms it, ours was systematically off by one.** MDC/MDIO/TXD were all one GPIO low. Wrong data pins failed loudly (`invalid TX_CTL GPIO number`), but wrong MDIO failed as "No PHY device detected", three inference-steps from the typo. The chip's own IDF IO_MUX table (`esp32s31/emac_periph.c`) is the authority for RGMII data pins (fixed pads); verify SMI pins against `ETH_ESP32_EMAC_DEFAULT_CONFIG`; use PHY-addr auto-detect (`-1`).
- **A shared clock is a contended resource, the RGMII 125 MHz Tx clock can't come from a PLL already spoken for.** The default (AUTO) sourced it from the MPLL, but PSRAM already ran the MPLL at 400 MHz (no integer path to 125) and CPLL couldn't synthesise 125 on the 40 MHz XTAL grid; only the fractional APLL works. On an SoC where one PLL feeds multiple peripherals, "pick a clock source" is a conflict-resolution decision, check what already owns each PLL.
- **Changing a Kconfig *choice* in a defaults fragment needs a clean build.** Editing `CONFIG_ETH_EMAC_RGMII_TX_CLK_SRC_*` and building incrementally kept the *old* choice (the build dir's `sdkconfig` already had a value; defaults don't override an existing one). Two flash cycles were spent "testing APLL" that were still CPLL. `rm -rf` the build dir when a sdkconfig *choice* changes.

## W5500 SPI Ethernet (LightCrafter / SE16): interrupt service, IDF-v6 config, and a per-board reset pin

Bringing up the two Limpkin ESP32-S3 boards' external W5500 (SPI) Ethernet surfaced three traps between "the driver installs" and "the link is up."

- **Interrupt mode needs `gpio_install_isr_service()`, miss it and the IRQ silently degrades to slow polling.** The W5500 has a real INT pin (SE16 GPIO18, LightCrafter GPIO45), but the IDF driver's `gpio_isr_handler_add()` requires the per-pin ISR service already installed, which nothing did. The `add` failed with a one-line log, the interrupt never fired, and RX serviced only on the slow fallback → a ~1 s sawtooth ping latency. Install the ISR service once before driver init (`ESP_ERR_INVALID_STATE` = already installed = fine). The `int_gpio=-1` polling fallback is a workaround that hides this.
- **IDF v6's W5500 driver rejects `int_gpio_num < 0` unless a poll period is also set.** `int_gpio_num = -1` alone returns `invalid configuration argument combination`; you must also set `w5500_config.poll_period_ms` (e.g. 10). The pair is the API contract.
- **A failed Ethernet init that cascades to WiFi masks the eth bug, "why are we on WiFi?" is the tell.** Both bugs above manifested as the board quietly running on WiFi with a healthy HTTP server and no eth log. The robustness cascade is correct behaviour but turns a loud init failure into a silent capability loss; a board that *should* be on Ethernet but is on WiFi is the signal to read the eth init log.
- **A `0x00` "version mismatched" read from an SPI PHY means the chip is held in reset.** The LightCrafter's WIZ850IO holds its own `nRST` until GPIO3 is driven HIGH; without releasing it the ESP32 read `0x00` from the W5500's version register (`expected 0x04, got 0x00`). The SE16's W5500 self-resets, which is why this was board-specific. Setting `ethRstGpio` (→ the driver's `reset_gpio_num`) fixed it. An all-zeros register read from an SPI peripheral is "the chip isn't talking"; the first suspect after wiring is a reset/enable line the host must drive.

## An RMT/IR done-callback runs in ISR context: signal only, decode on the task

The first NEC-decoder version crash-looped the board (USB dropped, port re-enumerated) because the RMT `on_recv_done` callback both **decoded** the frame (calling non-`IRAM_ATTR` helpers) and **re-armed** `rmt_receive()` from interrupt context. On ESP32, an ISR that jumps to flash-resident code while the cache is disabled, or re-enters a driver call, faults. The existing working RX capture (`rmtWs2812RxCapture`) already showed the safe shape: the ISR only records the symbol count and signals a queue; the decode + re-arm happen on the task (`irRead`, from the render loop). An RMT/GPIO done-callback may touch only its stack, the passed event data, and an ISR-safe FreeRTOS primitive (`xQueueSendFromISR`); parsing, logging, and re-arming move to the waking task. When in doubt, copy the codebase's already-proven callback.

## Don't promote one specific board's pins to "the chip default"

`NetworkModule::ethType` defaulted to a per-chip `ethConfigDefault`, and the classic / P4 / S31 entries were, on inspection, **one specific board's wiring each** (classic = Olimex, P4 = Waveshare NANO, S31 = Function-CoreBoard) dressed up as a chip property. Two failures: a WiFi-only board (LOLIN-D32 Shelly) wasted an Ethernet-PHY init on hardware it lacks; and the QuinLED Dig-Octa (custom MDC/MDIO, no software eth reset, GPIO5 is an LED output there) would have inherited reset=GPIO5 and driven an **LED pin as an Ethernet reset**. Fix: `ethType` defaults to **None**, Ethernet is opt-in, every board declares its own `ethType` + wiring pins in `deviceModels.json`, and `check_devices.py` enforces "ethType set → wiring pins present." A "default" that is really one board's values is bespoke masquerading as standard; make the capability opt-in and require each consumer to state its own values, so a missing declaration fails loudly instead of inheriting a stranger's wiring.

## A new firmware default only reaches a factory-fresh device; persistence wins on an upgrade

After changing the `ethType` firmware default to None and reflashing an already-provisioned board, the device *still* reported the old value, persistence (`FilesystemModule` overlays every saved control at boot) restores the value captured during earlier provisioning, and a plain `flash write` keeps the filesystem. The same shape bit the MQTT prefix (a stale stored prefix a reflash didn't clear). This is correct persistence behaviour (saved config wins over a constructor default), but a default change is invisible to any device that has already run, only an erase-flash or explicit re-set surfaces it. When you change a default to fix a bug, the fix lands only on factory-fresh devices; an already-provisioned fleet needs an explicit migration, and when debugging "my fix didn't take," check the persisted `.config/*.json` before the firmware default.

## Fixing "blocking in the tick" at the connect misses the same violation at the write

The MQTT client's first version blocked the render loop twice, a synchronous `connect()` (with DNS) *and* an unbounded blocking `write()` retry, both inside `loop1s` (which runs in `Scheduler::tick`). The first review caught the connect (reworked to non-blocking `connectStart`/`connectPoll`); the re-review caught that `conn_.write()` was still there: a zero-window broker (accepts TCP, stops reading) fills the send buffer and the next `write` never returns, freezing the render loop permanently, and the keepalive timeout can't even fire because control never comes back. Fix: sends go through a non-blocking `writeSome` with an all-or-reset rule (control packets ≤256 B, so atomic-send is realistic). A "no blocking in the hot path" audit must sweep every syscall the path can reach, connect, DNS, read, *and* write, not just the loudest one; the re-review exists to catch exactly the partial fix that reads as complete.

## A retract/cleanup path gated on "connected" strands the resource when disconnected

HA MQTT discovery's toggle-off publishes an empty retained config (so HA removes the entity) then frees the 768 B discovery buffers. The first cut guarded the whole path on `state_ == Conn::Connected`, so toggling discovery off *while the socket was mid-reconnect* returned before freeing, stranding the buffers until teardown, and never sent the tombstone, so the broker kept the stale retained config and HA kept the entity. Found live on P4/S31 hardware; every unit test had driven retract from the connected state, so the disconnected path had zero coverage. Fix: the retract frees unconditionally (freeing local memory needs no socket) and CONNACK retracts when discovery is off (so a reconnect clears a config left retained from a previous session); only the empty-retained PUBLISH itself needs a live link. General shape: a cleanup/retract path that also releases a resource must not gate the *release* on connection state, only the *network send*; and a path only ever exercised in one state (connected) needs an explicit test in the other. (`unit_MqttModule` "retract frees even while disconnected" pins it.)

## Raw-HTML `<img src>` is not validated by MkDocs `--strict`, so a wrong `../` depth ships a silent 404

The catalog/summary pages hand-author each card image as a raw `<img src="../../assets/...">`, and the build hook moves the tag into a table cell without touching its src. MkDocs `--strict` validates markdown `![]()` links but NOT raw-HTML `<img src>`, so 34 card images with a wrong `../` depth (the catalog pages sit one level shallower than the generated moxygen pages, so they need `../../assets`, not the moxygen pages' `../../../assets`) resolved to a nonexistent repo-root `/assets/` and 404'd on the live site while every gate passed green. The desktop build, ctest, and `--strict` all miss it; only loading the deployed page shows it. Fix + guard: a `test/python` check resolves every catalog `<img src>` on disk (`test_mkdocs_slug.py::test_catalog_card_images_resolve_on_disk`). General: a generated-site check that only validates the syntaxes its linter knows leaves the others as silent-404 territory, verify the actual rendered output, or gate the un-validated syntax explicitly.

## Disable-releases-resources: one router (`applyState`), not a per-module `enabled()` check

"Disabling a module frees its hardware" (so the pin map's freed-pin display is truthful and a shared GPIO is genuinely reusable) resolves to a single core primitive, not per-module bookkeeping — but getting there passed through a wrong turn worth remembering.

- **The shipped design.** `MoonModule::applyState()` is the sole resource-lifecycle router: it calls `onBuildState()` (pure build — acquire) on an `effectivelyEnabled()` node and `teardown()` (release) otherwise, recursing the tree so each child is routed by its *own* effective-enabled. A module's `onBuildState()` therefore contains **no `enabled()` check** — it just builds; core decides whether to build or tear down. `effectivelyEnabled()` walks `parent_`, so a disabled *parent* releases its whole subtree (a disabled Layer frees its effects' heap), and a `respectsEnabled()==false` ancestor (Network/HttpServer) is neutral, never forcing children on. This mirrors the loop gate (`tickChildren` / `Scheduler::shouldRun`): the same "one place owns the enabled decision" shape, now applied to acquire/release.
- **The wrong turn: per-module `if (enabled())`.** The first cut at disable-releases-resources had each resource-holder self-gate — `if (enabled()) reinit()` pasted into `setup()`, `onBuildState()`, `onCorrectionChanged()`, and the buffer setters (~24 sites). It worked but pushed orchestration into catalog modules (a driver "remembering" a rule that isn't its job) and it was inconsistent (some `if(!enabled()) return;`, some `if(enabled()) …`). The lift into `applyState()` deleted all of it: the release code was already in each module's `teardown()`, so the `onBuildState` disabled-branch was a *duplicate* — removing it is pure subtraction. (This is the [*Complexity lives in core*](../../../CLAUDE.md) clause on "extend the core mechanism to the sibling path, don't re-implement per module" — the canonical repo example.)
- **The cold-boot symptom that started it (keep — it's the reproduction).** On the P4 (MHC-WLED shield, GPIO 20 = `O20` level-shifted output), a *disabled* RmtLed on pin 20 grabbed the RMT channel at boot, so the *enabled* ParlioLed on the same pin drove nothing — LEDs dark. Toggling RmtLed off→on→off "fixed" it live (the disable freed the RMT channel, then ParlioLed's next enabled sweep re-inited over the now-free pin) — which is exactly what masked it during runtime-only testing: **a toggle test never exercises a cold boot.** The lesson: a lifecycle bug that a live toggle papers over still bites on the boot path — test persisted-disabled-at-boot explicitly (a disabled module's `applyState()` must route to teardown, asserted host-side).
- **The gotcha `applyState` sidesteps: `teardown()` is not always a clean inverse of `setup()`.** HueDriver's `teardown()` frees its light-name buffers, but its `setup()` (inherited no-op) does NOT re-fetch them — `loop1s()`/`onBuildControls()` rebuild them lazily. Under the router this is fine (release lives in `teardown`, rebuild is lazy, `onBuildState` is pure), but it's why "disable = blindly call `teardown` then `setup`" can't be a blanket rule: route through `applyState` (build-or-teardown), never a mechanical setup↔teardown swap.
- **IrService + the `active_` election show release lives in `teardown`.** IrService has no channel member — its RMT-RX channel is a `static` behind the platform layer, keyed by pin — so its `teardown()` calls the `platform::irStop()` seam (the existing `closeChannel()`); re-acquire is lazy on the next `irRead`. AudioService/DevicesModule vacate their `active_` singleton seat in `teardown()` (and in a destructor, so a bare destruction never dangles the static). All acquisitions belong in `onBuildState`, all releases in `teardown` — `applyState` picks which runs.

## "Port enumerates but the chip is dead-silent" = the board's power/enable jumper (S31 J5), not the cable

An S31 Function-CoreBoard would not flash: the CP2102N UART port (`/dev/cu.usbserial-…`) *enumerated* fine, but esptool got `No serial data received` at every baud (115200/460800/921600), every reset method (`default-reset` / `no-reset` / `usb-reset` / a software EN pulse), and even with BOOT physically held through the connect window. A raw 20 s serial read while tapping RST returned **zero bytes** — a running or bootlooping ESP is *loud* (ROM reset banner every cycle), so total silence ruled out a bootloop too. The port even renamed (`…340`→`…434`) and dropped mid-session. Every software angle pointed at "the chip's UART0 isn't reaching the bridge," which reads as a cable/jack fault — but it wasn't. **The cause was a missing 2-pin jumper, J5, next to the 5V→3.3V DC/DC converter (U4) and the 3.3V power-on LED (D11) on the power-input side.** With J5 out the board is *half-powered*: the CP2102N runs off USB VBUS directly (so it enumerates), but the ESP32-S31's 3.3V/EN rail is incomplete, so the chip never drives UART0 — the exact "port present, chip silent, nothing in download mode" signature. Reinstalling J5 and esptool talked to the chip on the first try (same port, same cable). The lesson: when a board's USB port enumerates but the MCU emits *nothing* — not even a boot banner, not in forced download mode — suspect the board's own **power/enable jumper or supply path before the cable or the USB jack**. A bridge chip powered from VBUS will happily enumerate a dead board. (Diagnostic that would have saved the hour: a raw serial read on the first reset — zero bytes with the power LED lit means "not powered/enabled," not "reset timing.") Bench note for the S31 specifically: **J5 = power/enable; leave it installed.**

## S31 LED-output validation: RmtLed + ParlioLed pass loopback, LcdLed is S3-only

Loopback self-test on the S31 (a WS2812 frame TX'd on one pin, read back on a jumpered RX pin — `Tx=48 / Rx=47`, the two stacked pins of J2 column 7, bridgeable with one cap): **RmtLed PASS, ParlioLed PASS** on RISC-V silicon. **LcdLed reports "no valid pins"** — correct, not a bug: `LcdLedDriver` is the ESP32-**S3**-specific LCD_CAM i80-bus driver, and the S31 (RISC-V) has no LCD_CAM peripheral, so `platform::lcdLanes` is 0 there. So the S31's supported WS2812 output drivers are RmtLed + ParlioLed; LcdLed is not one of them. One gotcha surfaced while testing several drivers in a row: they all defaulted their loopback to the *same* GPIO 48, so only one driver can hold the loopback pin at a time — toggle each driver's `loopbackTest` off before testing the next, or give them distinct jumper pairs (the same single-pin-owner rule the pin map enforces everywhere).

## `tickTimeUs` is CPU-in-the-call, NOT frame time — the async-transmit measurement trap that nearly buried a working 48→76 fps win

The parallel-driver async double-buffer (Step 1.5: encode frame N+1 into a second DMA buffer while frame N clocks out, wait deferred to the start of the next reuse) was almost **reverted as a no-op** because the A/B was read off the wrong meter. The trap, and the corrections that untangled it:

- **A module's `tickTimeUs` is CPU time spent inside its `tick()` call, which includes a *blocking* wait but excludes a *deferred* one — so it is NOT the output frame time.** Synchronous (async OFF) the ParlioLed tick was ~10,800 µs = encode (~2,800) **+ blocking on the WS2812 wire (~8,000)**; the CPU sits in `busWait`, so the wire time is *in* the tick, and `1e6/10800 ≈ 92` reads like a real rate. Async ON the tick was ~3,790 µs — encode only, because `busTransmit` returns immediately and the wait, deferred to the next tick, finds the DMA already finished (it drained during the other modules' ~9 ms of ticks). `1e6/3790 = 264` — which is **physically impossible**: 256 lights × ~29 µs/light = ~7,474 µs is a hard 133 fps wire ceiling. That impossibility is the tell that `tickTimeUs` had stopped measuring frame time and started measuring only CPU. The **real** frame rate is the *system* tick (the whole render loop): 48 fps sync → **76 fps async**. The driver's "264" is CPU headroom, not fps.
- **The first "no gain" A/B compared two async-ON states.** The initial P4 firmware *defaulted* async ON, so flipping the runtime toggle never cleanly reached the synchronous 10.8 ms path — both sides of the "A/B" were ~3,790 µs, so it looked flat. The clean A/B needs the two paths genuinely distinct: toggle OFF must run the *literal original* encode→transmit→wait (which is why the shipped `tick()` has two explicit branches, `tickSync`/`tickAsync`, not one path with a stale flag — the sync branch is provably the pre-double-buffer timing, no regression).
- **The rigorous baseline was a git worktree at the previous commit, flashed to the same board+config.** The doubt ("we started at 93, now 76, now 48 — only lower numbers?!") was three *different quantities* wearing the label "fps": 93 = driver-only sync fps (`1e6/10.8ms`), 76 = system fps async, 48 = system fps sync. To kill it, `git worktree add <tmp> <prev-commit>`, build, flash, measure — same meters, same board. Result: the previous commit was **always** 48 fps overall (never 76 or 93 as a whole-system number), and today's async-OFF reproduced it to the microsecond (10,820 µs vs 10,787). No regression ever existed; the confusion was entirely meter-mixing. General: when a number looks like a regression, measure the prior commit on the identical target before touching anything — a worktree does it without disturbing the working tree.
- **The fix that makes the trap un-fall-into-able: measure the wire explicitly.** Added a `wireUs` read-only KPI = the DMA transfer duration timed start-of-`busTransmit` → done-callback (an in-order 2-slot completion FIFO in the platform pairs each done with its start stamp; `esp_timer_get_time()` is ISR-safe). It reads the *pure* WS2812 output floor independent of render load — live on the P4 at 16×256: **"7474 µs (133 fps max)"**. Now the ceiling is measured, not assumed, the "impossible 264" can't masquerade as fps, and it doubles as the overclock instrument (raise the slot rate → `wireUs` drops directly). General: when a derived metric (fps-from-tick) can silently change *what it measures* under an optimization, add a metric for the underlying physical quantity so the derived one can't lie.
- **The feature is real and defaults ON.** 48→76 fps on the P4, no regression when off, and it costs one extra DMA buffer + one frame (~8 ms) of latency — hence OFF is the sound-reactive opt-out (allocation follows the flag: OFF allocates one buffer, so off truly costs nothing). The *remaining* gap to the 133 fps wire floor is the render loop itself (the effect at ~7.3 ms is the next bottleneck), which is the multicore work (Step 2) — and `wireUs` is now the target line to watch the system tick fall toward.

## A fixed-stride per-light scratch buffer silently caps channel count — a >4-channel preset overran it and heap-corrupted into a bootloop

Setting a **multi-channel preset** (a moving-head / DMX fixture, or even **RGBCCT at 5 channels**) on a WS2812 LED driver bootlooped the SE16 (S3/LCD_CAM) with a `LoadProhibited` panic. The hunt and the design lesson:

- **The bug: a fixed 4-byte-per-light `wire` scratch, assuming ≤4 channels (RGBW).** The per-light encode ran `correction_.apply(src, wire + lane * 4)` into `wire[kMaxLanes * 4]` (parallel) / `wire[4]` (RMT), and `encodeWs2812LcdSlots` read `wire[lane * 4 + ch]` — the stride hard-coded to 4. A correction with `outChannels > 4` wrote **past its 4-byte slot**, overflowing into adjacent lanes and off the array end → heap/stack corruption. `outChannels` is set from the preset's role count, so a fixture preset (Pan/Tilt/Dimmer/… = 8–16+ ch) — or RGBCCT (5) — walks straight off the end. The persisted preset re-applied every boot, so it crashed, rebooted, crashed = bootloop.
- **The reproducing bootloop was the ideal debugger, not a disaster.** Same panic every boot → each hypothesis got a definitive yes/no. `addr2line` on the backtrace + a one-line `ESP_LOGW` dumping the state pre-crash (`st`, `io`, `buf`, `bytes`, fifo head/tail) turned "it crashes somewhere in tx_color" into hard facts. Two decisive isolation flashes (queue_depth 2→1, then PSRAM→internal buffer) **ruled OUT** the new Step 1.5 double-buffer as the cause — the crash was identical. Then flashing the **previous commit** in a git worktree crashed too, but landed in `tlsf_malloc` (`remove_free_block`) — the unmistakable signature of a **corrupted heap**, i.e. a buffer overflow, present *before* Step 1.5. General: a crash inside the allocator on a *later* malloc is almost never the allocator's fault — it's an earlier overflow that trashed the free-list; find the writer, not the reader. And a reliable repro earns instrumentation, not guesswork (the anti-stalling rule cuts *both* ways: >2 blind fix attempts = stop, but a repro you can *measure* is exactly when to keep going).
- **The fix is NOT a channel cap — it's sizing the scratch to the channel count (no maxed caps).** The first instinct (reject `outChannels > 4` with a status) was wrong: RGBCCT is already 5, and MoonLight's principle is *no arbitrary caps — the only limit is memory* ([CLAUDE.md](../../../CLAUDE.md)). So `wire` became a **heap buffer sized to `kMaxLanes × outChannels`** (parallel) / `outChannels` (RMT), allocated off the hot path (in `parseConfig`/`resizeSymbols`, grows-only, freed on release), and the encoder's lane stride became the `channels` parameter instead of a literal 4. Now any channel count lays out without overrun; the driver **drives** a 16-channel fixture, it doesn't refuse it. **Heap in the hot path?** No: allocation is off the hot path (once per config change), `tick()` only reads the member pointer — same pattern `NetworkSendDriver`'s `corrected_` and the DMA buffer already use, so no per-frame alloc and no cache penalty at these sizes (tens–hundreds of bytes stays in L1). Verified end-to-end: the SE16, still holding the multi-channel preset that bootlooped it, now boots clean and reports `driving 2304 of 16384 lights`.
- **General: a fixed-stride scratch is a silent cap.** Any `buf[N * FIXED_STRIDE]` where the real element size is runtime-derived is a maxed cap hiding as a buffer — it works until an input exceeds the stride, then it's a memory-corruption bug, not a clean rejection. Either size it to the actual stride (off the hot path) or assert the input fits; never let it overrun. The tell that this class of bug exists: a driver that assumes RGB(W) but reads its channel count from a user-settable preset.

## Six dead hypotheses on one bug: what a debugging session looks like when the *method* is broken

The 74HCT595 shift-register transport bug (frames reaching the strands garbled, thousands of GDMA `lli full` mount failures) survived **six** confident explanations in a single session. Every one of them explained the symptom; every one died on contact with a real test. The bug is still open — but the *method* failure is the transferable lesson, and it is worth more than the eventual fix.

The corpse pile, in order: (1) the descriptor pool is too small → doubled it, **no change**; (2) the DMA never hands descriptors back → refuted, `avail` climbs steadily; (3) an off-by-one in the frame's node count → refuted, a *fresh* bus mounts its full count fine; (4) a silent S3 FIFO underrun → **built on the S3's inability to report underruns**, i.e. on the absence of evidence, and falsified by a P4 (which *can* report) running the identical frame perfectly with zero underruns; (5) the frame must live in internal RAM, not PSRAM → refuted *twice* (the P4 works from PSRAM; the S3 failed from internal RAM) and, worse, a **dead end by design** — internal DMA RAM is ~110 KB, so it would cap the feature *below* what already works, sawing off the branch the driver stands on; (6) PSRAM is too slow → never plausible, the board's **octal** PSRAM has bandwidth to spare, and one look at the datasheet number would have killed it before any code was written.

What actually went wrong, and the rules that fall out:

- **A theory that "explains everything" is not evidence.** Each hypothesis was adopted because it fit the symptom, then *code was changed* to act on it, then the bench was read *through* the theory. That is the loop that produces six deaths. The discipline: before changing code, name the measurement that would **refute** the hypothesis, and take it. If no such measurement exists, the hypothesis is not yet worth acting on.
- **Absence of evidence is not evidence.** The "silent underrun" story existed *only* because the S3 has no underrun interrupt. A chip that cannot report X is not telling you X is happening. The moment a control condition appeared (a P4, which *does* report), the theory evaporated.
- **Check the datasheet number before theorising about it.** "PSRAM is too slow" cost real time and was refutable in ten seconds by looking up the octal-PSRAM bandwidth against the 26.67 MB/s the frame needs.
- **A metric you never validated is not a meter.** The GDMA error count was used as the success signal for most of the session — and then the product owner reported *visually clean, flicker-free strands in a state that still logged 121 mount failures*. The proxy and the reality had come apart, which means every "confirmed" and "no change" read off that proxy was worthless. Validate a proxy against ground truth **once**, early, or don't lean on it.
- **The PO's eyes found in one sentence what six theories missed.** "I put asyncTransmit off and it's MUCH better" and, later, a blind sweep of `ledsPerPin` landing *exactly* on the allocator's internal/PSRAM boundary (96 good / 128 bad, against a ~38 KB largest free internal block) — both were better data than anything the agent produced. When a human at the bench reports a reproducible observation, that is the primary instrument; the logs are supporting evidence. (This is the *[Invite the product owner to test — then STOP and wait](../../../CLAUDE.md#process-rules)* rule earning its place: the rule exists because the agent kept racing past the one measurement that mattered.)
- **A fix that shrinks the feature below its own baseline is not a fix.** Internal-RAM-first "worked" and was nearly shipped as the answer — while capping a shift display at ~1,500 lights, *fewer* than direct mode already drives. It ships as an explicitly-labelled **stopgap**, and the PO's push-back ("not scalable — check you're not running in a very bad circle") is what caught it.
- **Prune the record as hypotheses die.** `docs/work/future/shift-register-driver-analysis.md` § 7.5 had accumulated three successive "the mechanism is X" write-ups, each stated with confidence and each wrong — actively poisoning the next attempt. It is now split into **what is TRUE (measured)** and **what is FALSE (guessed, then refuted — do not re-derive)**. A design doc that records dead theories as findings is worse than no doc.

The one thing that *is* solid after all that: the frame renders correctly from **internal RAM** and incorrectly from **PSRAM** on the S3 — established by the PO's blind sweep, not by any of the reasoning above — while the same PSRAM frame runs perfectly on a P4. The mechanism remains **unknown**, and the next attempt starts from the PO's `asyncTransmit` observation, not from a seventh theory.

## A device that loses WiFi must climb back by itself — three stacked bugs made a dropout permanent

Two bench boards were found **rendering happily at full frame rate with no IP and no retry**, one of them with `POWERON` as its last reset reason — i.e. it had lost its network at some point and simply never come back. In a light installation (a controller in a ceiling) that is a brick that needs a ladder. Three independent defects had to line up:

- **ESP-IDF does not auto-reconnect, and nothing was calling `esp_wifi_connect()`.** The `WIFI_EVENT_STA_DISCONNECTED` handler logged the event, cleared a flag, and stopped. Espressif's own `wifi_station` example calls `esp_wifi_connect()` right there; without it a single dropped association is permanent. (Do **not** `vTaskDelay` to pace the retry — the handler runs on IDF's event-loop task, which also carries the Ethernet and IP events; blocking it stalls the whole stack. The driver's own ~1–2 s association timeout paces the retries for free.)
- **A single failed poll tore down a working STA.** `State::ConnectedSta` fell straight to AP mode the first time `wifiStaConnected()` read false — no grace period. One lost beacon and the device abandoned a perfectly good network. Now it gets the same 10 s grace `State::WaitingSta` already used for the initial connect (one named constant, `kStaGraceMs`, shared by both — the question is identical, so the answer should be too).
- **AP mode was a dead end.** The fallback called `wifiStaStop()`, which *deinits the STA radio* — so `wifiStaConnected()` could never become true again, and `State::AP`'s only upgrade check was… `wifiStaConnected()`. The device would sit on its own AP forever. It now retries STA every 60 s while parked in AP (long, because each attempt bounces the AP and the causes it recovers from — a rebooting router, a device carried back into range — play out over minutes).

**General:** any fallback state must carry a path *back* to the state it fell from, and that path must not depend on a signal the fallback itself made unreachable. Grep for the shape: `if (!connected()) { stop(); startFallback(); }` where the fallback's only exit test is `connected()`.

## The catalog was right and the *reader* was wrong: a silent lookup miss cost a board its network

The hpwit shift-register board could associate and was then dropped by the AP within a second, repeatedly — chased for an hour as a router problem, a DHCP-lease problem, an RF problem. The actual cause: **it was transmitting at full 20 dBm because its TX-power cap was never applied**, and the cap was never applied because `improv_provision.py` looked for it in the wrong place:

```python
cap = entry.get("controls", {}).get("Network", {}).get("txPowerSetting")   # no entry has a flat `controls`
```

Every catalog entry stores per-module settings in a **`modules` list** (`{"type": "NetworkModule", "controls": {"txPowerSetting": 8}}`), so the chained `.get()` resolved to `None` — **silently**, for every board, forever. `deviceModels.json` had the correct value the whole time.

**General:** a chain of `dict.get(k, {})` cannot fail loudly — it turns a schema mismatch into a `None` and a missing feature. When the value is load-bearing (a TX-power cap, a pin map, a baud rate), either assert it was found or log what was looked up and what was there. And when a device misbehaves in a way its *catalog entry* claims to prevent, suspect the reader before the hardware. (The PO's question — "did you inject txpower 8? the board needs that!" — found it in one line.)

## The shift-ring "wedge" was two mundane bugs wearing a scary mask — total-free vs largest-block, and a double-rebuild

The MoonI80 shift ring "wedged" after a loopback ON→OFF (and after any control edit): status stuck at "output stalled — the bus is not delivering frames," LEDs dark until a **reboot** — a no-reboot-principle violation chased for two sessions with theories about stale LCD_CAM shift-clock state, a detached GDMA interrupt, and an unreleased clock-source enable. Three prior speculative fixes (a `busBusy_` gate, adding `loopbackTest` to `affectsPrepare`, moving `reinit()` out of `onControlChanged`) all failed. Bench instrumentation (an ISR-bumped EOF counter + `printf` at each destroy/create) found it was **two ordinary bugs**, neither peripheral-level:

- **`InternalFits` tested total free, not the largest contiguous block.** A 144 KB shift frame (16 strands × 128) reported "fits internal" because *total* internal DMA free was megabytes — but the largest *block* was ~62 KB. So `wantsRing()` returned false, the whole-frame path took over, its single contiguous 144 KB alloc failed, fell back to PSRAM, and stalled at the expander clock (the exact thing the ring exists to avoid). The heap has the space; it just isn't contiguous. **General: when code allocates ONE contiguous block, gate on `heap_caps_get_largest_free_block`, never `heap_caps_get_free_size` — total-free says "there's room" while the single alloc that faces fragmentation says "no."** This also produced the maddening "128 worked a bit then stalled": the answer flip-flopped with heap fragmentation around the 144 KB line.

- **One control edit rebuilt the bus TWICE, and the rapid second rebuild wedged.** Within a single `Scheduler::prepareTree()` sweep, the Drivers container's `prepare()` pushes every child's correction (`passBufferToDrivers` → `rebuildCorrection` → the driver's `onCorrectionChanged()`, which did a full `reinit()`), and THEN the same sweep runs the child's own `prepare()` → `reinit()` again. Two peripheral teardown+rebuilds back-to-back; the second came up before the first's ring had fired a single frame, and its GDMA EOF never fired. The fix is a dirty-gate: `onCorrectionChanged()` reinits only when `frameBytes_` actually changed (a real RGB↔RGBW switch), so a light-count/window edit — which the correction push does NOT resize — rebuilds once, in `prepare()`. **General: a cross-cutting "rebuild on change" that fires from two independent triggers in the same sweep must be idempotent or gated on a real delta; two reinits of a stateful peripheral in quick succession is its own failure mode, distinct from either reinit alone.**

Both bugs presented identically ("output stalled, `wireUs=—`, reboot to fix"), which is why the peripheral-level theories stuck: the *symptom* was at the peripheral, the *cause* was two layers up in memory-fit routing and control-flow. **General: a symptom that manifests in an interrupt/peripheral is not evidence the bug lives there — instrument the decision that led to the peripheral call (which path was chosen, how many times it ran) before diffing registers.** The false-positive risk cuts the other way too: the bench probe flagged "WEDGE? first frame advanced EOF by 0" on every rebuild, which was just the first-frame-has-no-predecessor artifact of the probe, not a real wedge — the ring was driving fine underneath.

## `/api/types` froze the LEDs on every UI refresh — a throwaway probe reached the LIVE render split through a global hook

"Refresh the web UI and the LEDs freeze" — reported first on an ESP32-S31, so it read as S31-specific silicon: a new board on an IDF-beta. It was not. It reproduced on the P4 too (there the fast core recovered so fast the freeze was invisible to the eye, but the multicore worker task `mmEncode` provably vanished from the FreeRTOS task list on every refresh), and it fired with **every** LED driver, at the stock clock, on a flash-erased board. The one invariant: `multicore` ON + a UI refresh.

**Root cause.** A browser refresh fetches `GET /api/types`, and `HttpServerModule::writeTypeDefaults` builds a **throwaway probe** of each registered type (`ModuleFactory::create` → `defineControls` → read defaults → destroy) to publish its default control values. Constructing a `ParallelLedDriver` probe runs `selectDefaultPeripheral` → `swapPeripheral`, which called `MoonModule::notifyQuiesceRender()` **unconditionally**. That hook is a process-global that resolves to the *live* Drivers via the static `ActiveInstance<Drivers>` seat (`Drivers::active()`), and it calls `stopEncodeTask()`. So a detached probe — an instance that was never in the tree — **tore down the running render split**, and no `prepareTree()` re-engaged it. `multicore` still read `true` (the switch state is not the engaged state), so the UI showed nothing wrong; on a slow-enough core the un-overlapped output visibly froze.

**Fix (one line).** `swapPeripheral` fires the quiesce hook only when `peripheral_ != nullptr` — i.e. only when a real backend is about to be `delete`d, which is the exact use-after-free the hook exists to prevent. A fresh instance's first swap (the default-peripheral build, no prior backend) no longer touches the live worker; a genuine live swap (POST `/api/control` on `peripheral`, with a backend to free) still quiesces. The existing "a live swap quiesces the worker" test still passes; a new `unit_Drivers_rendersplit` case pins that a detached ParallelLedDriver's swap leaves a live split untouched (fails without the guard).

**General lessons:**
- **A `create → introspect → destroy` probe must be side-effect-free, and "read the defaults" is not.** Any lifecycle call on a throwaway instance (`defineControls`, `release`, a constructor that selects a default) can reach global/static state — a registry, an active-instance seat, a hook — and mutate the *live* system. If a type publishes to a process-global on construction or teardown, probing it is not read-only. (Same family as the earlier registerType `T probe` stack-overflow: a probe instance is a real object with real side effects.)
- **A global hook keyed off a static "active" seat is invisible at its call sites.** `notifyQuiesceRender()` reads innocent — but it always hits whatever `Drivers::active()` currently points at, which is never the caller when the caller is a probe. A hook that reaches "the live one" needs an attached/owned check at the fire site, or it fires for instances that have no business touching the live system.
- **"Board-specific" was a timing artifact, not a hardware fact.** The freeze was identical on every board; only the *visibility* differed with core speed. Chasing "what's different about the S31" (clock, IDF-beta SMP, RMT refill) burned days. The break came from a deterministic, board-agnostic trigger (`GET /api/types` alone kills the worker) captured with a `this`-pointer printf that showed the teardown hitting the *live* Drivers, not the probe. **When "only board X does it" resists every hardware theory, find the software action that reproduces it on demand and instrument identity (which instance), not just occurrence (that it happened).**
- **The switch state is not the engaged state.** `multicore == true` while the split was actually disengaged sent the whole investigation sideways more than once. A control that can read ON while its mechanism is OFF is its own trap; the trustworthy signals were the FreeRTOS task list (`mmEncode` present?) and `renderWait` (a number vs `—`).

## MoonI80 flickers below ~30 lights on a PSRAM frame buffer — OPEN, cause unproven

Ten GRBW lights on an SE16 (S3, octal PSRAM, one strand, direct mode) flicker continuously and periodically report `no LED output — the driver is not sending frames`. Above ~30 lights it is clean. **i80 and RMT drive the same wiring perfectly**; only MoonI80 fails. Still open — recorded so the next attempt starts from the evidence rather than from scratch.

**What is established.** Moving the direct-mode frame buffer from PSRAM to internal RAM makes the flicker stop outright (bench-verified: symptom gone, PSRAM in use dropped by exactly one frame). So the failure is tied to the buffer's memory pool, not to the encoder, the layout, the wire, or the strip. The give-up message means `busWait` timed out 8 times in a row: the transfer never signalled completion at all, which is a hard failure, not marginal timing.

**What is NOT established: why only small frames.** Three explanations were checked against arithmetic and all three fail:
- *Stall-to-frame ratio* — a ~20 µs PSRAM stall is 2.9% of a 10-light frame. Far too small to break it.
- *Wait budget too tight* — the budget is **more** generous at small sizes (55x the wire time at 10 lights vs 2.8x at 1000). Backwards.
- *PSRAM alignment padding* — zero at every size; the frame is already 64-byte aligned.

The leading untested theory is that a short frame gives the DMA no runway to prefetch through a PSRAM/cache-contention stall, where a long frame's DMA runs far enough ahead of the wire to ride it out. The instrument that would settle it is `loopbackTest` with `loopbackIntrusive` — it captures what the peripheral actually emitted, separating a corrupt frame from a stalled transfer.

**Why no workaround shipped.** An internal-RAM fallback for small frames was written, measured, and then deliberately reverted: internal RAM is the scarce pool (WiFi/HTTP/stacks share it), MoonI80 exists for large fixtures, and a short strand is the i80 backend's job anyway. A patch that spends scarce RAM to hide a cause nobody understands is worse than the open bug.

**General lessons:**
- **"Sustains the rate" and "completes a short transfer" are different claims.** The comment justifying PSRAM-primary is right about throughput at 2.67 MHz and says nothing about a latency-dominated short frame — the sort of authoritative-sounding comment that ends an investigation early.
- **A backend that works everywhere except one path is a configuration difference, not silicon.** i80 clean + MoonI80 broken on identical wiring narrowed this to code the two do not share.
- **Read a control's applicability before drawing conclusions from its value.** `doubleBuffer`, `useRing` and `ringSnapshot` all appear in `/api/state` while being inapplicable to this path (`supportsDoubleBuffer()` is hard `false` here; the ring controls are hidden unless `pinExpanderMode()`, and `wantsRing()` returns false in direct mode outright). Two hypotheses were built on those values and both were dead ends — the UI hides them for a reason, the API does not.
- **A frame-time KPI that does not scale with the frame is measuring a timeout, not a wire.** `frameTime` read ~741 µs flat from 10 to 600 lights. Flat where it should scale is a signal in itself.
- **A fix that works is not a cause that is understood.** The pool swap reliably removes the symptom, which is tempting to write up as a root cause; the arithmetic says the mechanism is still unknown.

## A DMA buffer smaller than the frame multiplies descriptor use, and the symptom looked exactly like a failing cable

Streaming panel-card frames from an S31 at ~5 300 packets/s degraded with uptime: clean for minutes, then refused frames, then a total transmit wedge every ~11 minutes that only a reboot cleared. It read as a hardware fault the whole way, and it was two config lines.

**The arithmetic that caused it.** `CONFIG_ETH_DMA_BUFFER_SIZE` defaults to 512 B. A panel-card frame is 1512 B, so every frame consumed **three** descriptors. A 10-descriptor TX ring therefore held ~3.3 frames while the driver fires 132 back-to-back. Setting the buffer to 1536 B (64-byte aligned) gives one descriptor per frame and the failures stop. **Check DMA buffer size against your actual frame size; the default is sized for IP MTU traffic, not for a burst sender.**

**The second half is a race, not a size.** `CONFIG_ETH_TRANSMIT_MUTEX` defaults to off, and `mac->transmit` advances a shared descriptor pointer with no locking. With the eth netif up, lwIP and the render task both reach it. Bench-isolated: buffer fixed but mutex off → 3 000 refused frames in 4 minutes; mutex on with only 10 descriptors → none in 11. **Ring depth was never the fix**: 30 descriptors ran no cleaner than 10, and a 30-deep TX ring totals ~46 KB of internal RAM against ~15 KB at 10.

**Why it cost hours: two wrong turns worth naming.**
- *"It is not back-pressure."* Halving the send rate left the failure rate at ~13-15%, which reads as ruling out a full ring. It does not: at 3.3 frames of depth each 132-packet burst overruns the ring at any rate. The burst was the problem, never the rate. That misreading sent the investigation toward the cable and the PHY.
- *One counter for two faults.* `esp_eth_transmit` returns `ESP_ERR_INVALID_STATE` for a down link **before** touching the MAC, and `ESP_ERR_NO_MEM` for a full ring. Collapsing both into one bool made "5 million drops" unreadable. Splitting them settled the cause in one build: every failure was `ring`, none was `link`. **When a counter can be incremented by two different faults, split it before theorising.**

**What the physical evidence did and did not prove.** Both link LEDs going dark said the wire genuinely dropped, which is true and correctly killed the "stale software flag" theory. But it does not identify a cause: a wedged MAC stops driving the wire, so the dark LED was a *consequence*. A longer cable was the visible difference from the working setup and looked compelling; it was untouched throughout and is exonerated.

**Recovery still earns its place.** `esp_eth_stop()` + `esp_eth_start()` re-runs negotiation and resets the descriptor rings: the only way back from a wedge short of a reboot, since no ioctl writes the driver's link flag. Bench-verified twice, recovering in ~17 s. Attempted once per wedge, never repeatedly: a restart cannot fix an unplugged cable, and retrying would bounce the interface under the user.


## Lessons from the power-function branch (shared toolbox, particles, shaders)

A branch that built a shared library and migrated existing effects onto it. The gotchas all
share a shape: a green build, plausible-looking output, and a defect only counting could see.

- **A ported function's SIGN is part of its contract, and getting it wrong is silent.** Our
  `sin16`/`cos16` returned unsigned 0..65535; FastLED master (`i16 sin16lut`) and WLED main
  (`int16_t sin16_t`) both return signed. WLED effects write `sin16_t(x) + 32768` when they
  want an unsigned value — arithmetic that is exactly half a scale wrong against an unsigned
  return, with no compile error and no crash. A ported effect just looks subtly off. **When
  adopting a function that exists upstream, verify its range and sign against upstream's
  `master`/`main`, not against a release and not against memory** — the contract to bind to is
  the one users will have. Local checkouts of both live beside this repo for exactly this.

- **"It looks like noise" is not evidence that it IS noise.** `fbm16` summed its octaves after
  shifting each sample down by 8 — which fits a 32-bit accumulator and produces output that
  passes every visual check, while being an 8-bit field wearing a 16-bit type: 195 distinct
  values over 20,000 samples, low byte never set. That is precisely the banding the 16-bit tier
  exists to remove, shipped inside the fix for it. **For a function whose value IS its
  distribution, assert on the distribution — count distinct values, check the range is
  reached.** An eyeball, and a test that only checks bounds, both pass.

- **Two framerate bugs can cancel, and fixing one alone looks like a regression.** Fireworks
  counts frames for its launch roll (20x the shells at 1200 fps) *and* floors its trail fade at
  1 per render (erasing the trail 20x faster). Together they roughly cancel, so the effect
  looked fine and the audit passed. Fixing only the launch made the measured ratio worse —
  three times, before the second bug was found. **When a targeted fix makes a metric worse,
  stop patching and look for the compensating bug** rather than tuning the fix.

- **A "no-op" rewrite of a shared primitive needs a differential test, not a review.**
  Replacing `wrap()`'s per-span loops with modulo changed its endpoint behaviour: the loops are
  asymmetric (reducing from above stops *at* `span`, climbing from below stops at `0`), which
  no amount of reading the new code reveals. Diffing old against new across every span and
  2.8M inputs found it in seconds. **For a hot-path primitive whose replacement is meant to be
  behaviour-identical, prove it by exhaustive comparison against the original.**

## Lessons from the MoonLive-on-Xtensa branch (the register-window frame bug)

MoonLive's JIT ran on desktop arm64 and on RISC-V, and reset every Xtensa board the moment a
script called a host function. Three days, because every check we had was looking at the
instructions and the defect was in the stack layout around them.

- **On Xtensa, the top 32 bytes of a call8 frame belong to the hardware, not to the routine.**
  The window-overflow handler (`_WindowOverflow8`, esp-idf `components/xtensa/xtensa_vectors.S`)
  writes two 16-byte bands into a spilled frame: the top 16 take an *older* frame's a0-a3
  (`s32e aN, a9, -16..-4`), and the next 16 take the routine's *own* a4-a7
  (`s32e aN, a0, -32..-20`, where a0 is that frame's top). GCC obeys the same rule — every
  call8-making function it compiles reserves locals + exactly 32, at every size. Our emitter
  reserved 16, so the parked control-arena pointer sat inside the second band and a window
  spill overwrote it with an expression temp. **A frame-layout question is answered by the
  platform's own spill handler and a compiler probe, not by the ISA prose.**

- **Window rotation is deterministic; the SPILL is not, so the contract is spatial.** Rotation
  happens only at `call8`/`retw`, in our own instruction stream. The spill is lazy: it fires on
  any interrupt that lands while a call is in flight, at an instruction we do not choose. No
  ordering, no code sequence and no "quiet moment" can dodge it — only the layout can be right
  or wrong. This is also the whole symptom explanation: deep call chains (plasma's `sin`/`beat`
  into libm) gave every tick interrupt a wide window and died in under a second, brief leaf
  calls almost never coincided, and call-free scripts never faulted at all. **A crash whose
  frequency tracks how LONG a call runs, rather than what it computes, is an interrupt-timing
  bug — look at what the hardware writes asynchronously, not at the code path.**

- **Byte-perfect encodings prove the instructions, and say nothing about the frame.** Round-trip
  checks against `xtensa-esp32-elf-as` passed for every instruction we emit, including the
  `entry` whose immediate was the bug. Two other backends running the identical IR pipeline were
  flawless, which read as "the compiler is fine, the target is haunted". **When a defect is
  target-specific and every static check is green, suspect the contract with the platform
  (frame, ABI, alignment) rather than the code generator.**

- **The guardrail had to see the bug before the fix went in.** `MM_ISA_RESERVED_TOP` was raised
  to 32 in the structural checker *first*, and it failed on the shipped emitter, naming the
  offending frame offset. Only then did the emitter constant change. A test written after a fix
  proves the fix compiles; a test that fails first proves it *detects*. The checker decodes the
  real `entry` immediate out of the emitted bytes, so it pins the invariant rather than the
  constant.

- **A reserve constant is coupled to the widest call the emitter can produce.** 32 is exact for
  `call8`; `call12` would need 48 (its extra save area holds a8-a11 as well). We only ever emit
  `call8`, so the checker's 32 is an equality — but nothing in the code says so, and a future
  wider call would leave the test green while the frames went wrong again. hpwit, who wrote the
  new-parser Xtensa compiler, independently reached the same floor from experience ("I always
  start at at least 32") and phrases it as a minimum for exactly this reason. The reserve is
  therefore derived from the emitted call opcode rather than written down, so widening the call
  moves the reserve with it or fails the build.

## Lessons from the script-functions branch: a tidier disassembly can be the broken one

Local calls and recursion in MoonLive cost four stacked defects, and every one of them was
invisible to a green test suite. The theme is narrower than "test on hardware": it is that the
evidence which *looks* most authoritative here is the evidence that lies.

- **A cleaner-looking emitted block can be the broken one.** Removing the spill pass's function
  boundary remap makes `crosshair.mlv` disassemble BETTER on Xtensa (three tidy `entry`/`retw.n`
  pairs instead of two and a stray) and keeps all 1282 host tests green. It also boot-loops an S3
  with `StoreProhibited` and the light-buffer pointer holding `0xff`: a store through a register
  that a mid-statement frame boundary left holding a colour byte. Two things conspire — the host
  backend never executes that path, and the disassembler decodes the zero padding between
  functions as instructions. **A change to function-boundary or frame code gets a flash and a
  soak, never a listing review.**

- **The host backend cannot pin an argument-passing contract, because its register map hides
  one.** A script-to-script call emitted no argument setup at all, so each callee parked whatever
  the caller had left in the argument registers and its first control read faulted at
  `EXCVADDR 0x9`, offset 9 into a null arena. On arm64 the same omission fails nothing: R0..R4 map
  onto the ABI argument registers and `bl` leaves them alone, so the values survive by luck of the
  mapping. **Where a backend's register map coincides with the ABI, a test on that backend proves
  the contract holds by accident rather than by construction — say so in the test.**

- **An index into an array a later pass rewrites is a bug with a delay.** `CallScript` first
  carried the callee's IR index. The spill pass inserts a Reload before a read, so every index past
  its first insertion shifts and the call named an op that no longer started a function. A function
  NUMBER survives any rewrite. **When one pass hands a position to another, hand it a name.**

- **`swap()` must swap the whole object.** `IrProgram::swap` exchanged the ops, counts and slots
  but not the function table, so the spill pass's carefully remapped boundaries went into the
  discarded half and the lowering read the stale ones. The remap was correct; the transfer was not.

- **An empty function is a real case in a guard that brackets a body.** The recursion depth guard
  is emitted at a function's first real op, because it must follow the host arguments being parked.
  A function whose entire body is that parking (`nop() {}` parses) never reached the emission point
  while its epilogue still decremented, so every call to it drove the counter DOWN, two calls
  wrapped the byte past zero, and the next legal call was refused as too deep. **Any guard with an
  entry and an exit needs the zero-length body as a test, not just the deep one.**

- **The toolchain answers ISA questions faster than reasoning does.** "Must an Xtensa `entry` be
  4-byte aligned?" took one `xtensa-esp32-elf-as` invocation to settle: it refuses with
  `Error: unaligned entry instruction`. The same question had already cost an afternoon of
  hypotheses. hpwit's `new-parser` hits the identical wall and leaves it unhandled, which is
  confirmation the question is real rather than self-inflicted. **Assemble the case before
  theorising about it.**

## Lessons from the particles branch

- **A stateful effect is a jitter meter; a stateless one hides the same fault.** A shader recomputes
  every pixel from `t`, so a late frame is simply skipped and the next one is correct. A particle's
  position is the previous position plus velocity, and `FrameTime` deliberately spends a whole stall
  at once to keep the trajectory true in real time, so one frame after an 80 ms gap moves every
  particle **6.7x its usual distance** (measured). The first particle effect on the branch therefore
  exposed a 1 Hz `esp_littlefs_info` scan running inline on the render thread that had been there all
  along and that no shader had ever revealed. **When motion starts stuttering after a change that
  should not have touched timing, suspect a pre-existing periodic cost, and measure the frame deltas
  before theorising.** The reverse also holds: a smooth shader is not evidence that the render loop
  is clean.

- **Standardising a duplicated sentence requires deciding which version is TRUE first.** Three front
  pages had drifted into four orderings of the same six platforms, so a check was written to hold
  them to one wording. The wording picked was the most formal-looking existing one, which happened
  to demote five of the six targets to secondary and was simply wrong. Enforcing it would have
  spread a false claim to every file the check covered. **A consistency check is only as good as the
  value it pins; establish the fact, then enforce it.**

- **A test that passes with the bug reintroduced is worse than no test.** Three separate tests on
  this branch (the fade idle-gap, the collide spread, the filesystem throttle) were each written,
  seen green, and then found to pass with their own defect deliberately restored. Two were deleted
  and one was rewritten. The habit that catches it is cheap: **sabotage the fix and confirm the test
  goes red before believing it.** Two of those three had also been failing for a reason unrelated to
  what they claimed to assert, which the control check surfaced immediately.

- **A build script that does not build what the next command tests produces a confident false
  green.** `build_desktop.py` builds `MoonLight` but not `mm_tests`/`mm_scenarios` unless given
  `--tests`, so `ctest` straight after it runs whatever binary was there before. It reported "1377
  passed" against a test binary two hours old, on a change whose new option string was not even
  present in it. The tell is cheap and worth the habit: **after changing code, confirm the test
  binary actually contains the change** (a `grep` for a new string, or a run of just the new case)
  before believing a green suite. The staleness guards in `run_scenario.py` exist for exactly this
  and are why the scenario half never went unnoticed; the unit half has no such guard.

- **A freshness guard keyed on mtime must exclude files the build itself rewrites.** The same guard
  then cried wolf: `build_info.h` embeds a `+` when `git status` reports a dirty tree, and a gate run
  dirties the tree by writing its own scenario baselines and metrics. So every gate run left the
  next one reporting a stale runner, a self-inflicted loop that reads exactly like a real staleness
  failure. **Generated build metadata is not source**, and a guard that cannot tell them apart
  teaches people to ignore it.

- **An ISA-guarded test is not run by the machine that wrote it, and a JIT's bytes are only true
  where they execute.** The MoonLive backends are `#if`-guarded per architecture, so an arm64 bench
  compiles neither the x86-64 encoder tests nor the x86-64 emitted code — a whole backend can be
  wrong while the local suite is green and confident. A Q16.16 multiply that borrowed a register the
  allocator hands out returned garbage on every x86-64 desktop; 1458 local tests passed, and CI is
  where it surfaced. Two smaller defects hid in the same blind spot: a one-byte arena where the code
  now does a 32-bit load, and an unmigrated type keyword in a test file arm64 never compiles.

  **On an Apple Silicon machine that blind spot is one command wide:** `cmake -B build/x86
  -DCMAKE_OSX_ARCHITECTURES=x86_64` then `arch -x86_64 ./build/x86/test/mm_tests`. Rosetta runs the
  emitted x86-64 instructions for real, so the tests that only exist on that host actually execute.
  Worth doing on any change to a backend, an encoder, or the register allocator — it is minutes,
  and it is the difference between finding these locally and finding them in CI.

## A lossy channel never closes a client for slowness (2026-08-25)

The preview transport spent a bench day being patched before the pattern surfaced: every
symptom (reconnect storms, blank refreshes, renderWait spikes charged to the LEDs) traced to a
synchronous send path that treated a slow socket as a fault, spinning up to a stall budget on
the output core and then **closing the client**. That converts ordinary congestion into
disconnects, disconnects into re-primed state, and re-primed state into more congestion, a
feedback loop.

The rule that replaced it, and holds generally: **on a lossy stream, drop at the source and let
TCP pace the rest.** Write only what the socket takes now (`writeSome`, never a wait), skip the
frame when the previous one has not drained, report the skip to the receiver (its one honest
congestion signal), and close only on a real error or FIN. The receiver steers quality from the
drop reports. Every give-up budget that remains must bound *lack of progress*, never elapsed
total, or slow-but-healthy transfers get truncated.

## A control test must fail against the bug it claims to catch

A reviewer flagged that caching one "coarsest divisor" per light could not answer a stride test, and the finding was right. The reasoning offered with it was not: it named (70,70,0) at stride 2, but 70's gcd is 70 and the divisor loop stops at 64, so that coordinate is unaffected. The real condition is narrower, a gcd ABOVE the loop's cap, which no coordinate under 65 can reach.

Two tests were written and both passed against the unfixed code before one finally fired. The first used a dense grid, which walks the lattice closed-form and never reads the cache. The second used a sphere whose coordinates all sat under the cap, so both predicates agreed on every light it contained. Only a radius-64 shell, which reaches coordinates whose axes share a factor above 64, separated them.

**The rule: reproduce the failure before fixing it, and prove the test fails against the old code.** A finding's headline can be sound while its example is wrong, so the example is the first thing to verify, not the claim. And a test that passes against the bug is not a weaker test, it is not a test of that bug at all. The control run, old code plus new test, is what tells the difference, and it costs one build.
