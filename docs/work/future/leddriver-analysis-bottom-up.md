# LED driver landscape analysis

> **Forward-looking research document — exception to CLAUDE.md present-tense rule.** This is a Stage-1 landscape survey that grew into an architectural sketch + a recommendation. It maps the field of LED-driver libraries across ESP32, Teensy, Raspberry Pi, and desktop; proposes a two-axis reference architecture (Backend × Multiplex); and recommends a path (build everything ourselves, see "Recommendation — walk Scenario B" near the end). Stage 2 deep-dives validate the architecture's load-bearing assumption (that hpwit's shift-register multiplex can be factored out of the I2S-LCD peripheral code without losing performance). All repos read at **main/master HEAD** on **2026-05-25**, not at last release tag. Source citations use file paths and line numbers where available; inferred claims are marked.

## TL;DR

- **What this is.** Stage-1 of a two-stage investigation. Maps the LED-driver landscape, proposes a reference architecture, names which path to walk in Stage 2.
- **Field surveyed.** WLED / WLED-MM (NeoPixelBus under the hood, ESP32-family). hpwit's I2SClocklessLedDriver + I2SClocklessVirtualLedDriver (I2S-LCD parallel; the virtual one adds 74HC595 shift-register multiplex). FastLED master — note: master diverges sharply from the September 2025 release; a modular driver refactor + runtime-driver-switch API landed in May 2026. NightDriverStrip (FastLED legacy + core-pinning patterns). ESP-IDF's `led_strip` (managed component, reference only). Teensy via OctoWS2811 / ObjectFLED. Raspberry Pi via rpi-ws281x (broken on Pi 5 today). Desktop via ESP32-over-USB bridge as the dominant pattern.
- **Reference architecture.** One narrow boundary line: `LedDriver::push(std::span<const uint8_t>)`. Below the line is **two orthogonal axes**: Backend (RMT, I2S-LCD, LCD-CAM, parlio, FlexIO, RP1 PIO, SPI MOSI, BareMetal) × Multiplex (None vs. ShiftReg). The virtual driver is *not* a sibling backend — it's the ShiftReg multiplex applied on top of any parallel-clocked backend. Composition by member, switchable at runtime; no templates, no CRTP.
- **Identity-mapping fast path preserved.** When `MappingLUT::hasLUT()` is false, drivers receive a span pointing directly at `layer_->buffer()` — zero copy through the wire. This optimisation drove a major part of the architecture's shape.
- **Feasibility on hpwit's code (read at HEAD, 2026-05-25).** Two-target paths interleaved through one file (30 `#ifdef CONFIG_IDF_TARGET_ESP32S3` blocks). The multiplex code isn't cleanly factored out today — refactoring it into a backend-agnostic `ShiftRegMultiplex` layer is real engineering work, not free composition. `Backend × ShiftReg` is high-confidence on classic ESP32 + S3 (hpwit's lib is the reference), medium-confidence on parlio + FlexIO (no reference, plausibly feasible), low-confidence on RP1 PIO (research-grade).
- **What ESP-IDF gives us.** Peripheral plumbing (RMT5, parlio_tx_unit, esp_lcd_panel_io_i80, SPI master DMA, GDMA, GPIO LL) — yes. Anything LED-specific (WS2812 encoder, transposition helpers, multi-protocol abstraction) — no. The `led_strip` component is community, not first-party. Per-backend we'd write ~500-700 lines of new code; ~300-500 lines for the multiplex layer.
- **Recommendation: walk Scenario B (build ourselves).** Reasons in priority order: the multiplex axis is our MoonLight-defining value-add and doesn't compose cleanly onto FastLED; runtime driver switching aligns with our own contract; the identity-mapping fast path needs the driver reading `layer_->buffer()` directly; IDF gives us the plumbing for free; license + binary-size tax shrinks Scenario A's surplus. **Cost accepted:** 3 weeks → 3 months to first working driver, mitigated by spike-ordering.
- **First two Stage-2 spikes (load-bearing).** (1) `LcdCamLedDriver (None)` on ESP32-S3 against the existing `Drivers::loop()` shape — ~1 week, proves the contract. (2) Refactor hpwit's S3 path into `LcdCamLedDriver (ShiftReg)` — ~2-3 weeks, validates that the multiplex can be factored out without losing performance. **If spike 2 fails, fall back to hybrid** (Scenario A for non-multiplex backends + Scenario B for multiplex). The architecture supports both.
- **Out of scope for Stage 1.** Per-driver benchmarking on real hardware; PR-to-FastLED to expose its DMA buffer; final IDF version pin decision; license pick. Stage 2.

## Why this document exists

MoonLight's `src/light/drivers/` ships an ArtNet send driver and a WebSocket preview driver. There is **no LED-strip driver yet** — that's the gap this survey serves. The pipeline expects a `DriverBase` (`src/light/drivers/Drivers.h:11`) child that reads from the Drivers container's shared output buffer and pushes bytes to hardware. The requirements (1-10K LEDs must, 10-30K should, 30K+ interesting, 50 FPS, hot-reconfigurable pin/protocol/count, WiFi-coexistent) narrow but don't pick a library. This document characterises the candidates.

## ESP32 — primary depth

### WLED — https://github.com/wled/WLED

- **HEAD**: `0f34973` (2026-05-23), `main` branch, 18,115 stars, EUPL-1.2.
- **Abstraction**: delegates to `NeoPixelBus` (Makuna) for WS281x output via the `PolyBus` wrapper. `BusDigital` (`wled00/bus_manager.cpp:299`, `_driverType` 0=RMT / 1=I2S) selects between RMT and I2S backends at runtime. SPI (APA102/SK9822) goes through NeoPixelBus's two-pin protocol path (`bus_manager.cpp:322`).
- **Platforms**: classic ESP32, S2, S3, C3, C6, H2, P4 — variant-specific paths in `bus_wrapper.h` (e.g. lines 253-258 note that S3 uses NeoPixelBus's LCD parallel `X8` method). RMT5 vs legacy RMT is selected by NeoPixelBus, not WLED directly.
- **Protocols**: WS281x family, SK6812 RGBW, APA102, SK9822, TM1814, WS2801, plus PWM analog, on/off relay, HUB75 (S3 via `BusHub75Matrix`), and network buses (ArtNet, DDP).
- **Parallel pins**: up to 8 parallel channels on S3 via NeoPixelBus's I2S/LCD path; RMT channels divided per variant. `OUTPUT_MAX_PINS` caps it.
- **Threading**: `show()` is synchronous; NeoPixelBus handles DMA/ISR underneath. WLED runs a single-task render loop.
- **Hot reconfig**: `BusManager::add()` (`bus_manager.cpp:1478`) and `removeAll()` (`:1559`) let buses be created/destroyed at runtime, but no per-bus pin/count mutation in place — full reallocation is the pattern. UI "save & reboot" is the common path; on-the-fly is possible but unusual.
- **WiFi coexistence**: nothing explicit beyond what NeoPixelBus provides. Long-standing glitch reports on large installations — historically the reason WLED-MM forked.
- **Maintenance**: very active (commit yesterday, 501 open issues — high but reflects scale).
- **Field notes**: by far the most widely tested LED firmware. Carries baggage as a full firmware, not a library — extracting the bus layer for embedding is non-trivial.

### WLED-MM — https://github.com/MoonModules/WLED-MM

- **HEAD**: `e66e478` (2026-05-23), default branch is `mdev`, 395 stars, EUPL-1.2.
- **Delta from upstream**: focused on large installations and HUB75. WLED-MM adds inline-fast-path pixel writes (`unGamma8_bus`, `lastBus`/`laststart`/`lastlen` caching in `bus_manager.cpp` around line 1081), heavy `IRAM_ATTR`/`__attribute__((hot))` annotations, and a substantially extended `BusHub75Matrix` with dirty-bit tracking. Adds board-specific pinout presets for MOONHUB, MatrixPortal S3, HD-WF1/WF2.
- **Notably absent**: WLED-MM does **not** integrate hpwit's I2SClocklessLedDriver or the virtual driver — its HUB75 path uses `ESP32-HUB75-MatrixPanel-DMA` directly; its WS281x path is still NeoPixelBus via PolyBus. The "more pins for big installs" benefit comes from the upstream LCD-parallel I2S, not from a swap-in driver.
- **Channel allocation tweaks**: `bus_wrapper.h:776-786` shifts RMT/I2S channel assignment to accommodate the I2S0 split. Adds `WLEDMM_SLOWPATH` / `WLEDMM_TWOPATH` build options.
- **Maintenance**: active, 102 open issues. Smaller maintainer pool than upstream; product owner is co-maintainer here.
- **Field notes**: this fork's value-add is empirical tuning for installations beyond ~5K LEDs, not a fundamentally different driver architecture. If MoonLight wants the WLED bus layer at all, the question is whether to take it from upstream or from MM.

### I2SClocklessLedDriver (hpwit) — https://github.com/hpwit/I2SClocklessLedDriver

- **HEAD**: `a736f5e` (2026-04-06), `main`, 72 stars, MIT.
- **Abstraction**: ESP32 I2S peripheral in parallel-LCD mode (classic ESP32, `i2sInit()` at lines 1093-1167); on S3 it uses **LCD_CAM + GDMA** despite the "I2S" name (`#ifdef CONFIG_IDF_TARGET_ESP32S3`, lines 47-87). One peripheral, many GPIOs.
- **Platforms**: ESP32 and ESP32-S3 only. **No S2, C3, P4** support in the source (no conditional blocks for those). Sweet spot: S3 with PSRAM.
- **Protocols**: WS2812/13/15 and SK6812 (RGB and RGBW). No APA102 / SPI strips. Up to 16 parallel strips per peripheral.
- **Threading**: ISR-driven DMA chain with `nbDmaBuffer + 2` buffers (default 6), `loadAndTranspose()` runs inside the ISR (`:1654`, `:1741`) interleaving transposition with transmission. FreeRTOS semaphores (`sem`, `semSync`, `semDisp`, `waitDisp`) coordinate caller-side waits. `showPixels(NO_WAIT, ...)` is non-blocking.
- **Hot reconfig**: `initled()` is the (re-)init path. Pins and per-strip lengths can be changed by calling `initled()` again; no public API for live pin migration mid-frame.
- **WiFi coexistence**: not explicitly addressed in source; in practice users pin the driver task to core 1 and run WiFi on core 0. The DMA-driven design means the WiFi stack stealing CPU has little effect on bit timing — this is hpwit's core architectural advantage.
- **Field notes**: the standard go-to when RMT runs out of channels. Excellent for fixed installs; less ergonomic for hot reconfiguration.

### I2SClocklessVirtualLedDriver (hpwit) — https://github.com/hpwit/I2SClocklessVirtualLedDriver

- **HEAD**: `e7b2f0e` (2024-11-20), `main`, 47 stars, MIT. **Less active than the non-virtual driver** — last release "Titus" Jan 2024 — but stable; the technique doesn't need churn.
- **Abstraction**: same I2S parallel-LCD trick as the non-virtual driver (S3 = LCD_CAM + GDMA; classic = I2S0 LCD mode). Innovation is the **per-physical-pin time-multiplexing** through external 74HC595 shift registers + 74HC245 level shifters. `NUM_VIRT_PINS` (default 7-8) parallel data lines × up to 8 strips per shifted output = **up to ~120 strips total** from one ESP32. Source: project README + `I2SClocklessVirtualLedDriver.h` GDMA setup around line 430.
- **Platforms**: ESP32 + ESP32-S3. Hpwit overclocks the S3 LCD clock up to ~1.125 MHz, giving ~40% bandwidth headroom over baseline 800 kHz WS2812 timing.
- **Protocols**: WS2812/13/15, SK6812. No SPI strips, no DMX.
- **Threading**: optional dedicated display task (`I2SClocklessVirtualLedDriver_dispTaskHandle`), pinnable to core via `enableShowPixelsOnCore(corenum)`. Same semaphore model as the non-virtual driver. WAIT / NO_WAIT / LOOP display modes.
- **Hot reconfig**: init-time pin assignment; the shift-register topology is physical, so "hot pin change" doesn't really apply — the wiring is the wiring.
- **WiFi coexistence**: same story as the non-virtual driver — pin the display task to one core, WiFi on the other. Field-proven on installations approaching 100K LEDs.
- **Field notes**: this is the heavy-iron option for installations beyond 30K LEDs. Requires PCB design / hand-wired shift registers; not plug-and-play. **Fork-and-vendor is a low-risk option** given the product owner's relationship with the author and the technique's stability.

### FastLED — https://github.com/FastLED/FastLED

- **HEAD**: `f7d4b0e` (2026-05-24), `master`, 7,399 stars, MIT.
- **Critical: HEAD diverges sharply from the September 2025 release (3.10.3).** Master has a **modular driver refactor** under `src/platforms/esp/32/drivers/` with subdirectories `rmt/`, `rmt_rx/`, `parlio/`, `lcd_cam/`, `lcd_spi/`, `i2s/`, `i2s_spi/`, `spi/`, `uart/`, `gpio_isr_rx/`, `ble/`. Each is an OO peripheral abstraction with "automatic initialization and fallback handling" and "thread-safe operations" per the in-tree README. The pre-overhaul code lived as monolithic `clockless_rmt_esp32.h` / `clockless_i2s_esp32.h`.
- **Runtime driver switching** is the headline new API: `FastLED.add<Bus B>()`, `Channel::create<Bus B>()` (templated, since ~May 2026); non-template overload also lands. Diagnostic: "one-shot affinity-miss" + normalised driver names. Source: commit log on `master` around 2026-05-11 to 2026-05-12.
- **Removed**: `FASTLED_DISABLE_LEGACY_DRIVER_REGISTRY` macro (legacy registry is now opt-out-by-default).
- **Platforms**: classic ESP32, S2, S3, C3, C6, P4 (parlio is the P4 backend). Teensy 4.x via ObjectFLED (default since 3.10.x). ARM M0/M0+ for UCS7604 RGBW (beta).
- **Protocols**: WS281x family, SK6812 RGBW, APA102, SK9822, HD107S, LPD8806, WS2801, UCS7604 (16-bit RGBW, beta), and ~20 others.
- **Parallel pins**: up to 8 via RMT, up to 16 via I80/LCD parallel (S3/P4), up to 50 on Teensy 4 via ObjectFLED.
- **Threading**: per-driver; the LCD/I80 path is DMA + GDMA on S3, zero CPU during transmit.
- **Hot reconfig**: the new runtime-driver API is explicitly designed for it. This is the largest architectural delta from the September release.
- **WiFi coexistence**: better than legacy FastLED because the LCD/parlio paths are DMA-driven. Historical RMT bugs (`taskYIELD()` during WiFi storms) are mitigated in the new path.
- **Field notes**: master is in flux; the API surface above is fresh as of May 2026 and could shift before the next tagged release. Anyone building on it should pin a commit SHA, not the version.

### NightDriverStrip — https://github.com/PlummersSoftwareLLC/NightDriverStrip

- **HEAD**: `53edb35` (2026-05-16), `main`, 1,551 stars, GPL-3.0.
- **Abstraction**: built on FastLED (the legacy monolithic ESP32 RMT/I2S path, not the master refactor — release-version-pinned in `platformio.ini`).
- **Platforms**: ESP32 (classic, S2, S3) on a variety of dev boards (M5Stick, Heltec, LilyGo, plain DevKit). HUB75 via custom integration.
- **Threading**: explicit multi-task design — render task pinned to core 1, networking and effects on core 0. The README's "core affinity" discipline is what makes the project recognisable. Documentation here is thin; the patterns live in the source.
- **WiFi coexistence**: addressed by core pinning and explicit FreeRTOS task priorities, not by disabling WiFi.
- **GPL-3.0** is the key gotcha — copyleft constraints make code reuse in MoonLight (under whatever final licence) a deal-breaker for direct copy. Read it for **patterns**, not lines.
- **Field notes**: the canonical example of "how to keep a multi-thousand-LED ESP32 install stable while WiFi is on". Worth studying, not vendoring.

### ESP-IDF `led_strip` — https://github.com/espressif/idf-extra-components/tree/master/led_strip

- **Abstraction**: official Espressif component. Two backends — RMT (RMT5 in IDF 5.x) and SPI clockless (single-data-pin via SPI MOSI rate-matching).
- **Platforms**: every IDF-supported ESP32 variant.
- **Protocols**: WS2812/SK6812 generic single-wire.
- **Hot reconfig**: handle-based, init/deinit per strip. Multi-strip via multiple handles; no built-in parallel multiplexing.
- **License**: Apache-2.0.
- **Field notes**: the lowest-common-denominator reference. Worth knowing for the SPI-MOSI trick alone (one-pin WS2812 with zero CPU). Not a contender for large installs.

### ESP32 driver summary

| Driver | Abstraction | Platforms | Protocols | Max LEDs envelope | Hot reconfig | Licence | Maintenance | One-line |
|---|---|---|---|---|---|---|---|---|
| WLED bus layer | NeoPixelBus (RMT + I2S) | ESP32 / S2 / S3 / C3 / C6 / P4 | WS281x, APA102, SK9822, TM1814, DMX, HUB75 | ~10K typical, more via I2S-parallel | add/removeAll, full re-init | EUPL-1.2 | very active | full firmware, hard to extract |
| WLED-MM bus layer | same + hot-path tuning + HUB75 | as upstream | as upstream | ~16K demonstrated on Olimex per WLED-MM users | as upstream | EUPL-1.2 | active | hardened upstream, no virtual driver |
| I2SClocklessLedDriver | ESP32 I2S-LCD / S3 LCD_CAM+GDMA | ESP32, S3 | WS281x, SK6812 | 16 pins × strip length; 8K-30K | init-time pin change | MIT | active (2026-04) | DMA, ISR, 16 parallel pins, no WiFi issues |
| I2SClocklessVirtualLedDriver | same + 74HC595 shift-reg | ESP32, S3 | WS281x, SK6812 | up to ~120 strips, 50K-100K+ field-proven | init-time | MIT | stable (2024-11) | the heavy-iron option |
| FastLED master | modular per-peripheral drivers | ESP32 / S2 / S3 / C3 / C6 / P4 | WS281x + ~20 others | 50 pins on Teensy; 16 via LCD on S3 | **runtime driver-switch API new** | MIT | flux on master, stable releases | major overhaul — read HEAD not Sep release |
| NightDriverStrip | FastLED (legacy path) | ESP32 family | WS281x, HUB75 | demos with several thousand | restart | GPL-3.0 | active | study the task-pinning patterns |
| ESP-IDF led_strip | RMT5 / SPI clockless | every ESP32 variant | WS281x | per-handle, modest | per-handle | Apache-2.0 | active | reference, not a contender |

## Teensy

Three options for Teensy 4.x (NXP i.MX RT1062). License notes per project; all permissive.

- **OctoWS2811** — https://github.com/PaulStoffregen/OctoWS2811 — Paul Stoffregen's reference. 8 parallel WS281x outputs via DMA on Teensy 3.x; on Teensy 4.x uses the `_imxrt.cpp` variant which exploits the i.MX RT's DMA + FlexIO. Stable but slow release cadence (last release March 2022). The historical Teensy reference.
- **ObjectFLED** — https://github.com/zackees/ObjectFLED — up to **40 parallel pins on Teensy 4.0, 55 on Teensy 4.1**. Uses OctoWS2811's DMA core underneath, layered to add per-object independent timing. Non-blocking show — returns in ~6% of transmission time. Teensy 4.x only; FastLED-compatible. This is FastLED's default Teensy backend since 3.10.x.
- **FastLED on Teensy** — same library, ObjectFLED backend by default. The master-branch refactor (above) consolidates Teensy paths but doesn't reshape them as drastically as the ESP32 layer.

Teensy is a strong fit when the LED count exceeds what an ESP32 can drive cleanly and Ethernet (built-in on 4.1) supplants WiFi entirely. No WiFi-coexistence problem, period.

## Raspberry Pi

Three patterns, all known but each with caveats in 2026.

- **rpi-ws281x** — https://github.com/jgarff/rpi_ws281x — BSD-2. The historical reference. Supports Pi 1-4 via PWM (2 channels), PCM (1 channel), or SPI MOSI (1 channel). Up to ~5,400 LEDs via PCM, ~2,700 per string via PWM. **Pi 5 (RP1 chip) is broken** — issues #554 (2025-04, build fails when `PAGE_SIZE != 16384`), #555 (2025-05, "HW revision not supported"), #564 (2025-09, 2 GB variant). Last release v1.0.0 in March 2023. Community-maintained; no active Pi 5 PR landed at HEAD read time.
- **Pi-via-SPI bit-bang** — using the Pi's SPI MOSI as a one-pin WS281x driver at ~3.2 MHz, encoding each WS281x bit as 3-4 SPI bits. Works on Pi 5 because SPI is still exposed via RP1. Single-pin, but reliable.
- **OPC server pattern** — Pi as a render node feeding LEDs via Open Pixel Control over USB to a downstream ESP32/Teensy, or via E1.31/ArtNet over Ethernet. Doesn't address the Pi's own GPIO, but is the most-deployed Pi pattern in 2026 for large installs.
- **Pi 5 / RP1 outlook** — RP1 has user-accessible PIO-like blocks, but no mainstream LED library wraps them yet. **Needs Stage-2 verification** if Pi 5 is in scope.

## Desktop

Product owner's question: "Is there any way to do something with GPIO pins? E.g. connect an ESP32 via USB, high-speed access via USB to its pins?" Honest answer:

- **USB-CDC to ESP32 bridge** — by far the most common pattern. ESP32 acts as a smart pixel pusher; the desktop sends RGB data over USB serial. ESP32-S3's native USB-Serial-JTAG bus sustains ~12 Mbps (1.5 MB/s); UART-bridge boards (CP210x, CH340) cap at ~1 Mbps. At 12 Mbps, 30K RGB LEDs at 50 FPS needs 4.5 MB/s — over the wire's limit, so this caps somewhere around 10-15K LEDs. **The ESP32 is the driver; the desktop is the renderer.**
- **OPC over USB-CDC** — same idea with a defined protocol on top. Open Pixel Control (Zestyping/openpixelcontrol) is widely supported by visualisers (Processing, TouchDesigner). Maintenance dormant — superseded by E1.31/DDP in professional lighting — but still works.
- **FT232H bit-bang** — FTDI's MPSSE mode runs at up to 30 MHz. Libraries (pyftdi, libFTDI) can produce WS281x-timed pulses directly from the desktop, no microcontroller. Realistic for ~500-2000 LEDs; not for the project's targets. Niche but it does exist.
- **USB-to-SPI dongles** — Bus Pirate / FT2232H / Cypress CY7C65211. Same envelope as FT232H, marginally easier API. Same scaling ceiling.
- **Native desktop GPIO** — parallel-port WS281x bit-bang was a thing in 2015. Effectively extinct in 2026.

**Direct answer to the question**: yes, "ESP32 over USB" is real, well-trodden, and the right model for desktop-driven installations up to ~10K LEDs. Beyond that, the desktop should output E1.31/DDP/ArtNet over Ethernet to one or more ESP32/Teensy nodes — the network protocols are designed for exactly this scale.

## Architectural primitives observed

**Output paths.** Five clusters across the field:

- *RMT* (ESP32 legacy + RMT5) — per-bit timing precision, but channel count is 4-8 depending on variant, and the IDF 5 RMT5 API is structurally different from RMT4. Good for ≤8 strips.
- *I2S parallel* (classic ESP32) — one peripheral, 8-16 GPIOs in lockstep via DMA. Pixel data must be *transposed* (interleaved across strips bit-by-bit) before DMA reads it — this is the work hpwit's driver does in its ISR.
- *LCD-CAM / I80* (ESP32-S3) — successor to I2S parallel; faster, cleaner DMA via GDMA. FastLED master's `lcd_cam` driver is here; hpwit's S3 path is also here.
- *parlio* (ESP32-P4) — new parallel I/O peripheral, ESP-IDF 5.3+ only. FastLED master has a backend; ecosystem otherwise sparse.
- *SPI MOSI* — encode each WS281x bit as 3-4 SPI bits at ~3 MHz. One pin, but DMA-driven and zero CPU. The Pi and ESP-IDF `led_strip` both use this.
- *FlexIO/DMA* (Teensy 4.x) — OctoWS2811's territory. Functionally equivalent to ESP32 I2S-parallel.

**Buffer strategies.** Single-buffer + busy-wait (small installs, RMT4) gives way to double/N-buffer + DMA chain at scale. hpwit uses `nbDmaBuffer + 2` (default 6) circular descriptors with the ISR doing on-the-fly transposition — this is the most sophisticated pattern in the field. NeoPixelBus and ESP-IDF `led_strip` use simpler double-buffering. Per-strip vs mega-buffer trade-off: per-strip is more flexible (independent strip lengths), one mega-buffer is faster to DMA. hpwit supports both via overloaded `initled()` with per-strip `sizes[]`.

**WiFi-coexistence patterns.** Three approaches:

1. *Core pinning* — driver on core 1, WiFi on core 0. NightDriverStrip is the canonical example; hpwit's drivers also support this via `enableShowPixelsOnCore()`.
2. *DMA-driven output* — once data is in DMA, WiFi can't steal it. This is the real reason I2S/LCD-CAM drivers don't flicker: it's not core affinity, it's that the CPU isn't in the timing-critical path at all.
3. *Disable WiFi during transmit* — never used in serious projects; defeats the point of a networked controller. Mentioned only to dismiss.

Most libraries (NeoPixelBus, FastLED legacy RMT, Adafruit_NeoPixel) don't address this and rely on the CPU staying responsive — which is exactly when WiFi storms cause visible glitches.

**Hot reconfiguration.** A genuine differentiator. WLED's `BusManager::add()`/`removeAll()` supports runtime bus mutation but at the cost of full reallocation. hpwit's drivers require an `initled()` re-init for pin/count change. **FastLED master's runtime-driver-switch API** (May 2026) is the only library where switching the entire driver (RMT ↔ LCD) at runtime is a first-class supported operation. For MoonLight, the requirement is GPIO/protocol/count via UI controls — that's "re-init on change" pattern, achievable with all of these but ergonomically cleanest with FastLED's new API.

## Research question — can the virtual driver be reused on Teensy / Pi?

The product owner's explicit question. Honest answer from reading the source:

The technique has **two components**: (a) parallel-clocked GPIO output via a peripheral that can drive N pins in lockstep from DMA-fed memory, and (b) external 74HC595 shift registers that demultiplex one clocked output into 8 strip signals via a clock/latch protocol. (b) is hardware — protocol-agnostic, reusable anywhere. (a) is platform-specific.

**Teensy 4.x (NXP i.MX RT1062)** — has **FlexIO**, a programmable parallel I/O peripheral with DMA support. FlexIO can do parallel-clocked output of 8-32 GPIOs with timer-driven clocking. The architecture matches what ESP32's I2S-LCD provides. **Conceptually portable**, but the rewrite is non-trivial — FlexIO's programming model (shifters + timers + pin muxing) doesn't map 1:1 to I2S DMA descriptors. The `loadAndTranspose()` ISR pattern needs adapting to FlexIO's interrupt model. **Estimate: 1-2 weeks of focused work**; existing OctoWS2811 i.mx_rt code is the reference for FlexIO + DMA, but it doesn't do the shift-register multiplexing. **Needs Stage-2 verification** to confirm FlexIO clock can reach the 800 kHz × 8-slot = 6.4 MHz effective rate needed.

**Raspberry Pi 4 (BCM2711)** — has PWM and SMI (Secondary Memory Interface) for parallel output, plus DMA. Less programmable than FlexIO; the timing precision is the question. **Likely portable but with effort**; not a clean fit.

**Raspberry Pi 5 (RP1)** — RP1 contains PIO blocks (the same architecture as RP2040). PIO is a near-perfect fit for the virtual driver pattern — programmable state machines with deterministic timing, DMA-fed FIFO, parallel pin output up to 32 GPIOs. **Conceptually the best target outside ESP32**, but no mainstream library has wrapped RP1 PIO for LEDs yet. **Needs Stage-2 verification** — RP1 PIO docs are sparse vs RP2040 PIO; userspace access path through the kernel driver is the unknown.

**Summary**: the technique generalises. ESP32 I2S-LCD and RP1 PIO are the natural fits; Teensy FlexIO is workable; BCM2711 is the weakest. The shift-register hardware is reusable across all platforms. A cross-platform virtual driver is a real engineering target, but each port is 1-3 weeks, not a weekend.

## Stage-2 candidates

For the product owner to pick 2-3 from:

1. **FastLED master — modular driver layer** — what we'd learn: whether the new runtime driver-switching API and per-peripheral driver subdirectories are mature enough to use directly, or if vendoring a snapshot makes more sense. Estimated cost: 1.5 days.
2. **I2SClocklessVirtualLedDriver — fork-and-vendor scope** — what we'd learn: minimal API surface to wrap as a MoonLight driver MoonModule; what config controls (pin map, brightness, gamma, virtual-pin count) need to be hot-reconfigurable; whether the init-time-only constraint is acceptable. Estimated cost: 1 day.
3. **WLED-MM bus layer extraction** — what we'd learn: whether the bus layer can be lifted out of WLED-MM and embedded in MoonLight cleanly (or if it's too entangled with WLED's segment/effect plumbing). The EUPL-1.2 licence interaction with MoonLight's licence is part of this. Estimated cost: 1 day.
4. **NeoPixelBus directly** — what we'd learn: bypass WLED entirely, use the underlying library as MoonLight's WS281x backend. NeoPixelBus is what every WLED bus class wraps anyway. Estimated cost: 0.5 day.
5. **Hybrid: hpwit non-virtual + FastLED for non-WS281x** — what we'd learn: covers the 1-30K WS281x case with hpwit's I2S-LCD driver (no shift registers needed) and falls back to FastLED for APA102/SK9822/HD107S. Estimated cost: 1 day.
6. **ObjectFLED + FastLED Teensy path** — what we'd learn: feasibility of Teensy 4.1 as a parallel target alongside ESP32, especially for installations where WiFi is replaced by 4.1's built-in Ethernet. Estimated cost: 1 day.

## Answers — product-owner direction (2026-05-25)

These were the open questions; the answers now drive the architecture below.

1. **Teensy 4.1 — near-term target.** The architecture must host a Teensy backend from day one, not as "later work". ObjectFLED (FlexIO + DMA) is the reference; FastLED-on-Teensy is the implementation path for the "use libraries" scenario.
2. **Licence — GPL-3.0 default; MIT under consideration.** Either way the WLED-MM bus-layer extraction is licence-compatible (EUPL-1.2 is compatible with GPL-3.0; MIT is compatible with both). Neither rules out any Stage-2 candidate. Decision deferred but stops being a blocker.
3. **Custom PCBs are acceptable.** The virtual-driver shape (74HC595 shift-register multiplex) is in-house already; the architecture must natively support `pinCount × stripsPerPin` topology, not bolt it on as a special case.
4. **Runtime driver switching is a hard requirement.** Confirmed: switch RMT ↔ LCD-CAM ↔ I2S ↔ virtual from the UI. FastLED master's new `Channel::create<Bus>()` proves the field is converging here. This is the architectural axis everything else hangs off.
5. **Raspberry Pi 5 — stretch goal.** RP1 PIO stays in the architecture's future-target list (the abstraction must not assume Xtensa or NXP). Doesn't drive Stage-1 sketch choices, but the boundary line should respect it.

## Reference architecture

The architecture splits at one place: **what the LED driver receives** vs **what the LED driver does**. Above the line is platform-independent; below the line is platform-dependent. Both Stage-2 scenarios (build vs. borrow) sit on the **same** above-the-line shape — they differ only in who provides the below-the-line implementations.

Below the line is **two orthogonal axes**, not a flat list of backends:

- **Backend axis** — which parallel-clocked peripheral feeds the wire (RMT, I2S-LCD, LCD-CAM, parlio, FlexIO, RP1 PIO, …).
- **Multiplex axis** — `None` (one peripheral pin = one strip) or `ShiftReg` (one peripheral pin → N strips via 74HC595 cascade; the "virtual driver" technique).

The virtual-driver technique is *not* a sibling backend alongside LCD-CAM — it's a **modifier on top of** any parallel-clocked backend. The shift-register protocol on the wire is identical regardless of which peripheral is driving the parallel pins underneath. hpwit's library uses I2S-LCD on classic ESP32 and LCD-CAM+GDMA on the S3, but the technique generalises: anywhere there's a parallel-clocked DMA-fed pin block (parlio on P4, FlexIO on Teensy, RP1 PIO on Pi 5), the multiplex can hang off it.

```text
                  hardware-independent (above the line)
+--------------------------------------------------------------------+
| Layouts → Layers → BlendMap → Drivers (container) → Layer buffer    |
|                                          OR                         |
|                                     outputBuffer_                   |
|                                          ↓                          |
|                              std::span<const uint8_t> + metadata    |
|                                          ↓                          |
|                                  LedDriver::push(span)              |
+----------------------------------|---------------------------------+
                  hardware-dependent (below the line)
                                   ↓
+--------------------------------------------------------------------+
| LedDriver = composition of a Backend + a Multiplex                  |
|                                                                    |
|                      Multiplex axis →                              |
|                None (direct)        ShiftReg (virtual)             |
|              ┌────────────────────┬────────────────────────────┐   |
|  Backend  Rmt│ RmtLedDriver       │ (RMT can't drive shift-reg │   |
|  axis        │ (ESP32 family,     │  cleanly: no parallel-clock │   |
|       ↓      │  ≤8 strips/chip)   │  pin block — N/A)           │   |
|              ├────────────────────┼────────────────────────────┤   |
|        I2sLcd│ I2sLcdLedDriver    │ I2sLcdVirtualLedDriver      │   |
|              │ (classic ESP32,    │ (classic ESP32, hpwit's     │   |
|              │  16 parallel pins) │  current real-world deploy)│   |
|              ├────────────────────┼────────────────────────────┤   |
|        LcdCam│ LcdCamLedDriver    │ LcdCamVirtualLedDriver      │   |
|              │ (S3, 16 pins,      │ (S3, hpwit's modern path;   │   |
|              │  GDMA)             │  S3-overclocked for headroom)│  |
|              ├────────────────────┼────────────────────────────┤   |
|        Parlio│ ParlioLedDriver    │ ParlioVirtualLedDriver      │   |
|              │ (P4, 16 pins, new) │ (P4 future port — needs     │   |
|              │                    │  Stage-2 verification)      │   |
|              ├────────────────────┼────────────────────────────┤   |
|         FlexIo│ FlexIoLedDriver    │ FlexIoVirtualLedDriver      │   |
|              │ (Teensy 4.x via    │ (Teensy future port —       │   |
|              │  ObjectFLED-like)  │  needs Stage-2 verification)│   |
|              ├────────────────────┼────────────────────────────┤   |
|         Rp1Pio│ Rp1PioLedDriver    │ Rp1PioVirtualLedDriver      │   |
|              │ (Pi 5 stretch)     │ (Pi 5 stretch + virtual)    │   |
|              ├────────────────────┼────────────────────────────┤   |
|       SpiMosi│ SpiMosiLedDriver   │ (single pin: no multiplex   │   |
|              │ (one pin, any chip,│  benefit — N/A)             │   |
|              │  small installs)   │                             │   |
|              └────────────────────┴────────────────────────────┘   |
|                                                                    |
| BareMetalBackend — escape hatch for exotic chips; same two-axis    |
| shape, just owns its register pokes instead of using a HAL.        |
|                                                                    |
| Each LedDriver instance owns: peripheral handle, DMA descriptors,  |
| transposition buffer, optional ShiftRegMultiplex member, ISR.      |
| None of this leaks above the line.                                 |
+--------------------------------------------------------------------+
```

The multiplex axis is **composition, not inheritance**: a `LcdCamLedDriver` instance has a `Multiplex multiplex_` member (defaulting to `Multiplex::none()`); setting it to `Multiplex::shiftReg(virtualPins, cascadeDepth)` enables the virtual driver mode at runtime. The peripheral backend stays the same — only the inner-loop transposition function changes, and only at topology-change time, not per-frame. This preserves the runtime driver-switch requirement (axis 1) *and* the runtime virtual-on-or-off switch (axis 2) without templates.

### The line itself — `LedDriver::push(std::span<const uint8_t>)`

The boundary is a single virtual call per frame per driver. Inside `push()`, the driver uses its own typed peripheral handle with no further virtual dispatch — hot-path-clean. The line is intentionally narrow: just bytes and metadata, no buffer ownership crossing it. The driver does not store the span beyond the call's scope (the caller's buffer may be reused for the next frame).

Metadata carried by the driver as bound MoonModule controls, **not** through `push()`:

- `pinMap[]` — which GPIO each strip lives on (or, for virtual driver: which shift-register output).
- `stripLengths[]` — per-strip light counts, sums to `bufferBytes / channelsPerLight`.
- `protocol` — WS2812 / WS2815 / SK6812 / APA102 / SK9822 / HD107S / etc.
- `colorOrder` — GRB / RGB / GRBW / etc.
- `gamma` (1.0-3.0).
- `globalBrightness` (0-255 uint8).
- *Backend-specific*: each backend may expose its own controls (RMT clock divider, LCD overclock factor, FlexIO timer divider). These live on the backend, not the base.

Per-frame: just the span + an implicit "size matches the bound metadata".

> **Superseded — `colorOrder` / `gamma` / `globalBrightness` shipped differently.** These three are now the **shared output correction** (`src/light/drivers/Correction.h`): the `Drivers` container owns one `Correction` (brightness LUT + a `lightPreset` covering channel order *and* RGBW) and hands each child a `const Correction*`, rather than each driver binding its own `colorOrder`/`gamma`/`brightness`. One source of truth across all physical drivers; ArtNet already uses it. The future LED driver consumes the same `Correction` — see [leddriver-analysis-top-down.md § 4.6](leddriver-analysis-top-down.md) and [MoonLight, drivers](../../explanation/architecture/moonlight.md#drivers). Gamma is not implemented yet (the LUT is brightness-only; gamma folds in later as a per-channel R/G/B split).

### Identity-mapping fast path preserved

The existing optimisation at `Drivers.h:90` — when `layer_->lut().hasLUT() == false`, drivers point at `layer_->buffer()` directly, no copy — survives unchanged. The architecture sketches the `outputBuffer_` path because it's the common case, but the identity case is the *fast* case:

```text
Identity case (no MappingLUT):
  Layer.buffer ────────► LedDriver::push(span)     (zero-copy)

Composed case (one or more Layers with non-identity LUT):
  Layer1.buffer ─┐
  Layer2.buffer ─┼─► BlendMap → Drivers.outputBuffer_ ─► LedDriver::push(span)
  ...           ─┘
```

The driver doesn't know which path produced the span. That's the point.

### Scenario A — Use existing libraries

Below the line is **FastLED master** (the modular driver subdirectories `src/platforms/esp/32/drivers/` and FastLED's Teensy ObjectFLED integration) for the backend axis, **plus a vendored fork of hpwit's library for the multiplex axis only**.

```text
+--------------------------------+--------------------------------------+
| MoonLight LedDriver (backend×Multiplex)|  External implementation       |
+--------------------------------+--------------------------------------+
| RmtLedDriver (None)            → FastLED::add<WS2812, RMT_5_WORKER>     |
| LcdCamLedDriver (None)         → FastLED::add<WS2812, LCD_I80_WORKER>   |
| FlexIoLedDriver (None) (Teensy)→ FastLED::add<WS2812, OBJECT_FLED>      |
| ParlioLedDriver (None) (P4)    → FastLED parlio backend                 |
|                                                                         |
| LcdCamLedDriver (ShiftReg)     → FastLED lcd_cam DMA path + our         |
|                                  ShiftRegMultiplex transposition.       |
|                                  hpwit's lib provides the transposition |
|                                  algorithm reference; we vendor that    |
|                                  function only, not the peripheral init |
|                                  (FastLED owns that).                   |
| I2sLcdLedDriver (ShiftReg)     → same shape on classic ESP32 (FastLED's |
|                                  legacy I2S backend hosts our multiplex)|
+--------------------------------+--------------------------------------+
```

Pros: ride FastLED's runtime driver-switch API and per-backend maturity (matches requirement 4). FastLED owns the peripheral churn (RMT4 → RMT5 → parlio API revisions); we own the multiplex transposition and the MoonLight-side wiring. Teensy support comes essentially free via ObjectFLED. The multiplex code we vendor is small (~200 lines from hpwit's lib — just the transposition + cascade-shift protocol, not the peripheral init). ~2-3 weeks to a working ESP32 + Teensy hybrid; +1 week for the multiplex layer on top of FastLED lcd_cam.

Cons: FastLED master is in flux (May 2026 surface could shift), so we pin a commit SHA, not a version. The multiplex-on-top-of-FastLED-lcd_cam path needs FastLED to expose its DMA buffer for us to write into — its current API doesn't do that cleanly, so we either bypass FastLED for that one specific backend (effectively becoming Scenario B for it) or upstream a PR to FastLED adding the hook. **This is the brittle seam of Scenario A.**

### Scenario B — Build everything ourselves

Below the line is **our own peripheral backends + our own multiplex layer**, all talking to ESP-IDF / NXP HALs (the floor is HAL-level, not register-level: `esp_lcd_panel_io`, `parlio_tx_unit`, `rmt_tx_channel`; NXP FlexIO HAL on Teensy; not register pokes — that's the `BareMetalBackend` escape hatch).

```text
+--------------------------------------+--------------------------------+
| MoonLight LedDriver (backend×Multiplex)|  ESP-IDF / NXP HAL             |
+--------------------------------------+--------------------------------+
| RmtLedDriver (None)                  → rmt_tx_channel + encoder        |
| I2sLcdLedDriver (None)               → legacy I2S in LCD mode + DMA    |
| I2sLcdLedDriver (ShiftReg)           → same + our ShiftRegMultiplex    |
| LcdCamLedDriver (None)               → esp_lcd_panel_io_i80 + GDMA     |
| LcdCamLedDriver (ShiftReg)           → same + our ShiftRegMultiplex    |
| ParlioLedDriver (None)               → parlio_tx_unit (IDF 5.3+)       |
| ParlioLedDriver (ShiftReg) (future)  → same + our ShiftRegMultiplex    |
| FlexIoLedDriver (None) (Teensy)      → NXP FlexIO HAL + eDMA           |
| FlexIoLedDriver (ShiftReg) (future)  → same + our ShiftRegMultiplex    |
| (Rp1PioLedDriver — stretch)          → /dev/pio + ioctl                |
| SpiMosiLedDriver (None) (single pin) → SPI MOSI bit-rate encoder       |
| BareMetalBackend (exotic)            → register pokes for one-offs     |
+--------------------------------------+--------------------------------+
```

Notice that **the ShiftReg row is the same class** across all the parallel-clocked backends. `ShiftRegMultiplex` is one ~300-line class parameterised by virtual-pin count and cascade depth, sharing the transposition algorithm. The peripheral backend doesn't know it's being multiplexed — its `pinCount` is the physical pin count; the multiplex layer expands that to `physicalPins × stripsPerPin` upstream of `push()`.

Pros: everything under our roof, no upstream churn surprises. The two-axis composition is *natural* in our tree because there's no library boundary to fight: the multiplex layer cleanly sees the backend's DMA buffer. Virtual driver as a first-class deployment target everywhere (S3 today, P4 / Teensy / Pi5 when those backends land). Hot-reconfigure semantics are exactly what we want. No upstream API stability dependency.

Cons: 2-4 weeks per backend (HAL-floor; longer for parlio because of its newness). 5 backends × ~3 weeks + multiplex layer + integration = ~4-5 months before parity with what FastLED + vendored multiplex would give us in 3-4 weeks. Carries ongoing maintenance cost for every IDF API churn. Teensy backend needs separate test hardware. **The cost is mostly in the backend axis, not the multiplex axis** — the multiplex layer is small and stable.

### Feasibility check — is the two-axis split actually reachable?

This section is **hope-and-gotchas**, written after one pass through hpwit's `I2SClocklessVirtualLedDriver/src/I2SClocklessVirtualLedDriver.h` (HEAD, ~3820 lines, 2026-05-25). The architectural picture above is the goal; what follows is what stands between us and it.

**What I found in hpwit's source:**

- **Two distinct code paths interleaved through one file.** Classic ESP32 uses I2S0 in LCD mode (`soc/i2s_reg.h`, direct register pokes into `(&I2S0)->lc_conf.val`). S3 uses LCD-CAM + GDMA (`soc/lcd_cam_struct.h`, `hal/gdma_types.h`, `esp_private/gdma.h`). 30 `#ifdef CONFIG_IDF_TARGET_ESP32S3` blocks across the file; the two paths share the `loadAndTranspose()` inner-loop algorithm but the peripheral init, DMA setup, and ISR are entirely separate per target.
- **The multiplex code is not factored out.** `NUM_VIRT_PINS` and `NBIS2SERIALPINS` are macros baked at compile time. The transposition function (`loadAndTranspose` at lines 3533 + 3713 — itself duplicated under different `I2S_MAPPING_MODE` paths) intermixes the shift-register cascade math with the peripheral's DMA buffer layout (`DMABuffersTampon[dmaBufferActive]->buffer`). The shift-register protocol and the peripheral's DMA descriptor format are entangled in the same memory layout calculations.
- **No P4 / parlio code.** Zero references to `parlio`, `P4`, or `esp32p4` in the source tree. The S3 path was added relatively recently (the GDMA+LCD_CAM code is the newest large addition); P4 is a similar amount of work, not yet done.

**What that means for the two-axis split:**

- **The classic-ESP32 ↔ S3 split as two backends** — already exists *de facto* in hpwit's code via the `#ifdef`, just not factored as separate classes. Extracting `I2sLcdBackend` (classic) and `LcdCamBackend` (S3) as siblings is a real refactor (a few days of careful surgery) but it's reading what's there, not inventing it. **Feasible.**
- **The shift-register multiplex as a reusable layer** — the harder one. The transposition function `loadAndTranspose` knows the DMA buffer's pixel-encoding layout (per-bit interleaving × WS2812 timing × the cascade-shift signalling). Pulling that into a backend-agnostic `ShiftRegMultiplex` class requires the backend to expose its DMA buffer's *encoding contract* (where bit N for output O lives in memory). Different peripherals encode parallel-clocked output differently — I2S-LCD has a specific FIFO layout; LCD-CAM's GDMA descriptors are different again; parlio uses a different bit-packing scheme (per its `parlio_bit_pack_order_t` enum). **The multiplex isn't peripheral-agnostic in the way I wrote earlier; it needs per-backend specialisation of its bit-layout math.** What stays shared is the algorithm shape (cascade-shift signal generation, latch protocol, transposition strategy); what changes per backend is the memory-write pattern.
- **ESP32-P4 + parlio support for the multiplex** — ESP-IDF's `parlio_tx_unit` exposes a 16-pin parallel-clocked DMA-fed output (`SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH = 16` per `esp32p4/include/soc/soc_caps.h:462`). Same architectural shape as LCD-CAM. **The technique conceptually fits**; the work is writing the parlio-specific transposition function that matches parlio's bit-packing layout. Estimated 1-2 weeks of focused implementation + bench-verification on real hardware. **Feasible but unproven.** No one has shipped this; we'd be first. Stage-2 verification required.

**Net honest read on the two-axis split:**

The composition I drew is the *architectural target* and is achievable. The reality is:

1. **`Backend × None`** (RMT, LCD-CAM direct, parlio direct, FlexIO direct) — all feasible; each is a standard peripheral wrapper.
2. **`I2sLcd × ShiftReg`** (classic ESP32 virtual) — hpwit's working code is the reference; lifting it into our architecture is a refactor not a port. **High confidence.**
3. **`LcdCam × ShiftReg`** (S3 virtual) — same, the S3 path in hpwit's lib is the reference. **High confidence.**
4. **`Parlio × ShiftReg`** (P4 virtual) — no existing reference; needs original engineering. The parlio API supports it in principle (DMA-fed parallel-clocked output); the multiplex transposition has to be re-derived for parlio's bit-pack layout. **Medium confidence, Stage-2 verification needed.**
5. **`FlexIo × ShiftReg`** (Teensy virtual) — likewise no existing reference; OctoWS2811 doesn't multiplex. Each port is a multi-week engineering project, not free composition. **Medium confidence.**
6. **`Rp1Pio × ShiftReg`** (Pi 5 virtual) — most speculative. RP1 PIO has the right shape but no LED-driving reference exists. **Low confidence, true research.**

So the architecture's promise — "the same `ShiftRegMultiplex` class works across backends" — is **aspirational**. What's realistic is "the same `ShiftRegMultiplex` *protocol* (cascade-shift signalling, transposition strategy) generalises, but each `Backend × ShiftReg` pair needs its own per-backend transposition implementation matching that backend's DMA layout". The class hierarchy can express it; the inner-loop code is per-pair work.

**Possible stoppers that Stage 2 needs to verify:**

- Does hpwit's algorithm survive being refactored out of one file into a Backend + Multiplex pair without losing performance? The current code's tight coupling is partly a performance optimisation (the transposition writes directly to known DMA-buffer slots). Splitting it adds an abstraction layer that we'd need to prove is zero-cost.
- Does parlio's `parlio_tx_unit_transmit` API support the cascade-shift signalling pattern? The hpwit S3 trick uses LCD-CAM in a slightly unusual way; whether parlio is flexible enough in the same direction is unproven.
- Does the bit-overlap technique (using one peripheral's clock edges to clock shift registers *and* deliver pixel data on other pins) work on parlio? The S3 LCD-CAM lets you commandeer the LCD output clock for this; parlio's clock generation is structurally similar but the pin-routing model differs.

These are the "hope vs. confirmed" gaps. Stage 2 deep-dive into option #2 (virtual driver vendor) is where they get resolved — and resolves them on real hardware, not by reading source.

### What does ESP-IDF give us if we build everything ourselves?

Short answer: **a lot of low-level peripheral plumbing, almost nothing LED-specific.** ESP-IDF treats LED driving as a community concern, not a first-party concern.

What IDF provides natively (relevant to LED driving):

- **`driver/rmt_tx.h`** (`esp_driver_rmt`) — the RMT TX peripheral. Generic per-bit pulse-train generator with timing precision. **Useful** for WS281x single-pin output up to 8 channels; programmable encoder support for any clockless protocol. RMT5 API (IDF 5.x) is what we'd target.
- **`driver/parlio_tx.h`** (`esp_driver_parlio`) — parallel I/O TX. Up to 16 GPIOs in lockstep, DMA-fed. **Useful** as the host for our `ParlioBackend` (None and ShiftReg). New (IDF 5.3+), P4-only today, API still stabilising.
- **`esp_lcd_panel_io_i80.h`** (`esp_lcd`) — i80 LCD interface, ESP32-S3. 16-pin parallel data + clock, DMA-fed via GDMA. **Useful** as the host for our `LcdCamBackend`. This is what hpwit's S3 code uses underneath, just exposed via the public IDF API rather than direct register pokes.
- **`driver/spi_master.h`** — DMA-fed SPI MOSI at MHz rates. **Useful** for the `SpiMosiBackend` (one-pin WS281x via bit-rate encoding, the same trick the official `led_strip` component uses for its non-RMT mode).
- **`hal/gpio_ll.h`** — low-level GPIO ops, IRAM-safe. **Useful** for the latch-pin signalling in our shift-register multiplex.
- **GDMA / `esp_private/gdma.h`** — the underlying DMA engine on S3/C-series/P4 chips. **Useful** for advanced backends that need to chain DMA descriptors manually (the way hpwit does on S3).

What IDF does **not** provide:

- **No LED-strip driver in core IDF.** No `esp_ws2812.h`, no built-in WS281x bit encoder, no APA102 helper. The `led_strip` community component (`espressif/idf-extra-components`, Apache-2.0) gives a basic single-strip handle on top of RMT or SPI MOSI, but it's a managed-component dependency we'd pull in, not built into IDF.
- **No transposition helpers.** If you want to drive 16 parallel WS281x strips through I2S-LCD or LCD-CAM, you write the bit-interleaving yourself. That's the work hpwit's lib does.
- **No multi-protocol abstraction.** "Drive WS2812 here, APA102 there, DMX over there" is your problem; IDF gives you the peripherals, not the protocol mapping.
- **No color-order / gamma / brightness layer.** Up to us.
- **No hot-reconfigure pattern.** Each peripheral driver's API is "create_unit → enable → transmit → delete"; toggling pin maps at runtime means delete + recreate, and the discipline of doing that without dropping a frame is on us.

**What that means for Scenario B effort estimate:**

Per backend, we're writing:

- ~200 lines of peripheral setup (config struct, handle ownership, GPIO routing)
- ~50-100 lines of DMA buffer management (descriptor chain, double-buffering)
- ~100-300 lines of encoding (WS2812 timing-bit pattern, APA102 SPI clocking, etc.)
- ~50-100 lines of ISR / completion-callback handling
- ~100 lines of hot-reconfigure logic
- ~50 lines of MoonModule control surface

Per backend: ~500-700 lines of new code. For ESP32 (RMT) + S3 (LCD-CAM) + P4 (parlio) + Teensy (FlexIO) that's 2000-2800 lines just for backend basics. The shift-reg multiplex layer is another ~300-500 lines on top, with per-backend transposition specialisations.

**IDF helps a lot at the peripheral layer; nothing at the LED-protocol layer.** That's the honest weight of Scenario B. The 2-4-weeks-per-backend estimate stands; it's not naïve, but it does require that the IDF peripheral APIs *don't have surprises*. RMT5 has had three breaking changes between IDF 5.0 and 5.4; parlio is still pre-stabilisation. Expect to pin an IDF version and re-validate on every bump.

### What we gain by building everything ourselves

Importing a library means you get a lot that wasn't built for your case. Specifically, what we'd give up by adopting FastLED master under the line:

- **The pixel buffer layout is FastLED's, not ours.** Their CRGB struct, their channel ordering decisions, their gamma/brightness application points. We bridge — every `push()` call has to translate from MoonLight's flat `std::span<const uint8_t>` (the existing `Buffer::data()` shape) into whatever FastLED's bus class expects. That bridge is a per-frame copy or a per-frame view rewrite; in either case it's hot-path overhead that disappears in Scenario B because we control both sides.
- **Hot-reconfigure is FastLED-paced.** Their new `Channel::create<Bus B>()` API is the closest the field has to runtime driver-switching, but it's young (May 2026), the API surface is in flux, and "switch RMT to LCD-CAM mid-flight without dropping a frame" is something we'd have to verify works the way we need — not assume. In Scenario B the contract is ours; if a frame drop on driver switch is unacceptable, we engineer around it.
- **No leverage on inner-loop optimisations specific to MoonLight's MappingLUT / identity-mapping fast path.** FastLED knows nothing about our identity-mapping optimisation (`!hasLUT()` → direct Layer-buffer push). To use FastLED's bus path we either pre-compose into FastLED's CRGB buffer (defeating the identity path) or write a thin wrapper that exposes our raw buffer (possible but adds a coupling point we don't control). Scenario B keeps the identity path zero-copy through to the wire.
- **The multiplex seam is brittle on FastLED.** This is the strongest finding from the feasibility check above. FastLED has no shift-register multiplex backend. To use FastLED's lcd_cam DMA path *with* our shift-register multiplex, we need FastLED to hand us its DMA buffer for direct writes — its current API doesn't, and either (a) we bypass FastLED for that backend (effectively Scenario B for the most MoonLight-defining backend), (b) we upstream a PR (slow, library-author dependent), or (c) we accept a per-frame copy from FastLED's buffer into our transposed buffer (hot-path cost).
- **No control over IDF version pinning.** FastLED master targets whatever IDF FastLED master targets. If our MoonLight IDF version pin disagrees with FastLED's, we either bump them both in lockstep or carry a divergent fork. In Scenario B we own the IDF compatibility matrix.
- **Carrying FastLED is binary size we don't fully use.** FastLED master ships drivers for ~20 protocols, ~6 ESP32 variants, Teensy, ARM M0/M0+, and AVR; gamma tables, palette helpers, FX layer, blur kernels. We use the bus driver and the bus driver only. Dead code elimination at link time helps, but FastLED's runtime driver-registry is the opposite of dead-code-eliminable (the whole point is that drivers are reachable from a string lookup). Realistic estimate: 80-150 KB of binary we'd carry without using.
- **Profile-guided tuning is harder.** MoonLight's hot path is unusual — most LED firmwares don't have a Layer/MappingLUT composition layer above the driver. Tuning the inner loop for MoonLight's specific access patterns is straightforward if we own the inner loop; in Scenario A it's a sequence of conversations with the library maintainer.

What we **lose by building everything ourselves**: speed-to-first-working-driver. Scenario A puts an ESP32 + Teensy hybrid in your hands in ~3 weeks. Scenario B is multi-month before parity, and during those months we own every IDF API surprise. That cost is real.

What we **gain by building everything ourselves**: every byte of the hot path is ours, the multiplex axis is symmetric across backends without library-author cooperation, the binary stays focused on MoonLight's actual needs, and the architecture stops being a negotiation between MoonLight's design and FastLED's. **The architecture becomes a contract we own**, not a translation layer between two contracts.

### Recommendation — walk Scenario B

Reading the whole document honestly: **build everything ourselves.** Reasons, in priority order:

1. **The multiplex axis is the MoonLight-defining value-add for the >30K-LED case, and it doesn't compose cleanly onto FastLED.** Scenario A's "ride FastLED + vendor hpwit for the multiplex" framing was the first thing I wrote, and the feasibility check made it visibly worse. The cleanest path for the virtual driver is Scenario B; that's our heaviest single dependency on Scenario A, and it's the spot where Scenario A is structurally weakest.
2. **Runtime driver switching as a first-class requirement aligns naturally with our own contract.** FastLED's API is moving toward this in May 2026 master, but it's young and we'd be downstream of their choices. Owning the contract means owning the semantics of "switch backend without dropping a frame" — the user-facing requirement.
3. **The identity-mapping fast path needs the driver to read directly from `layer_->buffer()`.** That's a MoonLight-specific optimisation that the rest of the architecture is built around (Drivers.h:90 already implements it for ArtNetSend / Preview). FastLED has no incentive to support it; we'd lose the optimisation or carry a coupling layer.
4. **ESP-IDF gives us the peripheral plumbing for free.** As the previous section showed, `parlio_tx_unit`, `esp_lcd_panel_io_i80`, `rmt_tx_channel` are all production-ready. The LED-specific layer on top is small (300-700 lines per backend); we're not writing a peripheral driver from scratch.
5. **License + binary-size tax shrink the surplus from Scenario A.** EUPL-1.2 / MIT / GPL-3.0 mixing is solvable but adds review burden; 80-150 KB of unused FastLED code is real on a 4 MB partition table.

**Costs accepted by this recommendation:**

- **Time to first working driver shifts from ~3 weeks to ~3 months.** This is the biggest concrete cost. Mitigated by ordering: ship `LcdCamLedDriver (None)` on ESP32-S3 first (highest-value single backend), then `LcdCamLedDriver (ShiftReg)` on the same chip (validates the multiplex layer against an in-house reference), then `RmtLedDriver (None)` for classic ESP32 fallback, then `FlexIoLedDriver (None)` for Teensy, then expand. By month 4 we have parity with what Scenario A would give in month 1; by month 6+ we have everything Scenario A could never give us (parlio multiplex, RP1 PIO if Pi 5 is ready, custom inner-loop optimisations).
- **Per-backend IDF API surface changes are our problem.** Mitigated by IDF-version pinning + a documented compatibility matrix. Plan-17 already established the v6.1-dev pin discipline; this extends it.
- **Teensy backend needs separate test hardware.** Mitigated by the fact that Teensy 4.1 was named as a near-term target; the hardware purchase is in scope regardless of which scenario.

**Suggested order of Stage-2 work** (informed by the recommendation):

1. **Spike: `LcdCamLedDriver (None)` on ESP32-S3 against the existing `Drivers::loop()` shape.** ~1 week. Goal: prove the `LedDriver::push(span)` contract works end-to-end on real hardware (16 parallel pins, 8 KB strip each, 50 FPS). If this spike fails, the rest of the recommendation fails too — so do it first, fastest signal.
2. **Refactor hpwit's S3 path into `LcdCamLedDriver (ShiftReg)` against the same contract.** ~2-3 weeks. This is the highest-information-density spike: validates the two-axis split, validates the multiplex transposition refactor, gives us a working virtual driver on our own architecture. **If this spike fails, the recommendation needs revising — the multiplex-can-be-factored-out claim is the single load-bearing architectural assumption.**
3. **`RmtLedDriver (None)`** for classic ESP32 small-install support. ~1 week (RMT5 is the simplest backend).
4. **Continue per the architecture diagram**, prioritising by user-visible value: FlexIo (Teensy) > Parlio (P4) > Rp1Pio (Pi 5 stretch).

**When to revisit this recommendation:**

If the spike at step 2 fails — i.e. the multiplex refactor turns out to be infeasible without ruining performance, or hpwit's S3 path turns out to be too register-poke-specific to extract cleanly — **we fall back to a hybrid**: Scenario A for the non-multiplex backends (RMT, LCD-CAM direct, FlexIO direct), Scenario B for the multiplex layer specifically, accepting that the multiplex bypass-FastLED-DMA path is the brittle seam. The architecture supports this; the diagram remains accurate; the recommendation is just "we shipped less of Scenario B than we wanted."

## Concrete DriverBase API sketch

The header that Stage 2 starts from (illustrative, not final). Lives at `src/light/drivers/LedDriver.h`:

```cpp
#pragma once

#include "light/drivers/DriverBase.h"
#include <span>
#include <cstdint>

namespace mm {

enum class LedProtocol : uint8_t {
    WS2812, WS2815, SK6812, SK6812RGBW,
    APA102, SK9822, HD107S, TM1814, P9813,
    Custom,  // backend-specific timing in its own controls
};

enum class ColorOrder : uint8_t {
    RGB, RBG, GRB, GBR, BRG, BGR,
    RGBW, GRBW, // …
};

// Per-strip descriptor — one of these per physical strip on the driver's
// peripheral. Stored as a bound MoonModule control array.
struct StripSpec {
    uint16_t lightCount;     // logical lights on this strip
    uint8_t  pin;            // GPIO or shift-register output index
    LedProtocol protocol;
    ColorOrder order;
};

// LedDriver — abstract base for hardware-specific LED-strip output.
//
// Inherits the existing DriverBase (src/light/drivers/Drivers.h) shape, so
// it composes with ArtNetSendDriver and PreviewDriver in the Drivers
// container without changes to the container. Lifecycle (setup / loop /
// teardown) is MoonModule's; the LED-specific contract is push() +
// onTopologyChange().
//
// Hot path: push() is called once per render frame per driver instance.
// Inside push(), the implementation uses its own typed members with no
// further virtual dispatch. push() must:
//   - not block longer than the per-driver budget (see performance section)
//   - not allocate
//   - not call any platform API that holds the WiFi stack
class LedDriver : public DriverBase {
public:
    // Per-frame entry point. `pixels` is the source buffer (either the
    // shared outputBuffer_ or a Layer's own buffer in the identity-mapping
    // fast path); the driver does NOT store this span past push() return.
    // Span length matches strips_ × channelsPerLight at all times — caller
    // (Drivers container) guarantees this; driver may assert in debug.
    virtual void push(std::span<const uint8_t> pixels) = 0;

    // Called when strip count, lengths, pin map, or protocol changes via
    // UI. Backends use this to (re-)allocate DMA buffers, re-bind GPIOs,
    // and recompute transposition tables. May block (it's not the render
    // loop). Returns false if the new topology can't be honoured (e.g.
    // RMT channel exhaustion, too many parallel pins for the peripheral).
    virtual bool onTopologyChange(std::span<const StripSpec> strips) = 0;

    // Diagnostic: name of the backend ("rmt", "lcd_cam", "virtual_i2s",
    // "flexio", …). Used by the UI's driver-switch dropdown.
    virtual const char* backendName() const = 0;

    // Optional: maximum strips/lights this backend can hold given the
    // current chip. Used to grey-out impossible UI configurations.
    virtual uint16_t maxStrips() const { return 16; }
    virtual uint16_t maxLightsPerStrip() const { return UINT16_MAX; }
};

}  // namespace mm
```

Notes on the shape:

- `std::span<const uint8_t>` for `pixels` — caller-owned, zero-copy, lifetime tied to the call. ESP-IDF allows `std::span` (C++20 is the project baseline).
- No `color_order` / `gamma` / `brightness` argument to `push()` — those are controls on the driver and applied during push by the implementation. Keeps the boundary narrow.
- `StripSpec.pin` is a uint8 because the virtual driver indexes shift-register outputs (0..63), not real GPIOs. Real-GPIO backends just use it as a GPIO number.
- `onTopologyChange()` returns bool so the UI can surface "this combination isn't possible on this chip" instead of silently truncating.
- No CRTP, no templates — straightforward virtual dispatch, one call per frame. Runtime driver switch is `delete oldDriver; oldDriver = makeDriverByName(...)` — trivially supported.

## Performance budget (16K LEDs × 50 FPS, ESP32-S3 + PSRAM)

| Component | Memory | Time per frame | Notes |
|---|---|---|---|
| Frame budget total | — | **20 ms** | 50 FPS = 20 ms wall clock per tick |
| Effect compute (per Layer) | n/a | 8-12 ms | Empirical: Metaballs at 16K = ~12 ms today |
| Layer buffer (one Layer, RGB) | 48 KB | n/a | 16384 × 3 |
| MappingLUT (identity case) | 0 KB | 0 ms | Direct hand-off to driver, the *fast* path |
| MappingLUT (sparse) | 32-64 KB | n/a | (lightCount+1) × sizeof(idx) + dest count × sizeof(idx) |
| BlendMap (composed) | n/a | 1-3 ms | One memcpy in identity case; per-light loop in LUT case |
| Driver `push()` budget | n/a | **3-5 ms** | What's left after effect + blend |
| DMA transmission (background) | per-backend | 16-20 ms | WS2812 timing × strips; runs *after* push returns |
| Output buffer (RGB → wire bytes) | 48-96 KB | n/a | Pre-encoded vs runtime-encoded |
| Free heap headroom on no-PSRAM ESP32 | ≥ 40 KB | n/a | Otherwise WiFi / Ethernet can't start |
| Free heap headroom on PSRAM S3 | ≥ 100 KB | n/a | More breathing room; multi-driver becomes viable |

Two real numbers worth noting:

- **DMA transmission is asynchronous to `push()`.** Wire-level WS2812 transmission at 800 kHz takes ~30 µs per LED × 16K = ~500 ms in serial; on parallel hardware (16 pins) it's ~30 ms. Both of these are larger than the frame budget — which is why double-buffering + DMA chains exist. The driver's `push()` returns immediately after handing the buffer to DMA; the next frame's `push()` blocks (or yields) until DMA is free.
- **`push()` budget shrinks fast at higher counts.** At 30K LEDs the effect compute alone is 20+ ms (linear in light count for most effects); the frame budget either grows (drop to 30 FPS) or moves to multi-core (effects on core 1, network on core 0). MoonLight does not pin tasks today — the render runs on whichever core the scheduler task lands on.

## Hot-path do-and-don't checklist

Specific to the LED-driver hot path (inside `push()` and anything it calls per-frame). General-purpose render-loop rules from `architecture.md` § Hot path discipline apply too.

### Don't
- `std::function`, `std::bind`, `std::variant`'s `std::visit` with non-trivial visitor — all heap-allocate or have hidden vtable surface.
- `new`, `malloc`, `push_back`, `std::string`, `std::vector` growth — anything that can hit the heap.
- Virtual calls inside per-light or per-byte inner loops. One virtual call per `push()` for `LedDriver::push()` itself is fine; per-byte virtual dispatch in transposition is not.
- `mutex.lock()`, `semaphore.take()` with non-zero timeout — block the render loop. `try_lock` returning fast is OK.
- `printf` / `ESP_LOGI` — they take a global lock and write to UART. Use `ESP_DRAM_LOGE` if absolutely needed; better, log from `loop1s()` not from `push()`.
- PSRAM for the *output staging buffer* that DMA reads — PSRAM is ~12 MB/s sequential vs SRAM's 80+ MB/s; DMA from PSRAM works but is slow.
- Float math per-light if integer suffices. The existing code is already integer-first; preserve that in any new backend.

### Do
- Bound the inner loop count statically where possible (`uint16_t` for strip lengths up to 65 535).
- Place ISR helpers in IRAM via `IRAM_ATTR` — IDF-level discipline.
- For DMA buffers on ESP32, prefer `heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)` to ensure SRAM, not PSRAM.
- Document each `IRAM_ATTR` placement; the iram budget is tight on the classic ESP32 (~70 KB total).
- Use `std::span` over pointer+size pairs where C++20 syntax allows — keeps the hot path readable without abandoning zero-cost. (Note: this is the opposite of what `platform.h` does today across the broader platform API — there it's pointer+size for consistency with C-style ESP-IDF APIs. Inside the LED driver layer where everything is C++, `std::span` is the right call.)
- For runtime driver switching: free the old backend (DMA stopped, peripheral released, ISR detached) before allocating the new one. Avoid the heap-fragmentation pattern that bit us in plan-18 (Drivers null-deref).

## Risks and unknowns

Specific to the LED-driver architecture; ranked by potential blast radius.

1. **RMT5 API churn.** ESP-IDF's RMT v5 (`driver/rmt_tx.h`) landed in IDF 5.0 and has had non-trivial revisions in 5.1, 5.2, 5.3, and 5.4 — encoder API, callback signatures, queue semantics. Our backend has to pin to one IDF version and re-validate on every IDF bump. FastLED's master takes the same pinning approach; we'd inherit the same risk in scenario A.
2. **parlio is too new.** `parlio_tx_unit` is IDF 5.3+ only, ESP32-P4 only, and the API surface has rough edges (especially around chained DMA and idle-state behaviour). Worth treating as "Stage-2 verification needed" rather than "ready to use".
3. **Virtual driver portability beyond ESP32.** The `loadAndTranspose()` ISR pattern hpwit uses is conceptually portable to FlexIO and RP1 PIO but neither has a published reference implementation. Each port is its own multi-week engineering project; cross-platform "free" doesn't exist here.
4. **LCD overclock non-portability.** hpwit overclocks the S3 LCD clock to ~1.125 MHz for WS2812 timing headroom. This is undocumented behaviour; Espressif could change it without warning. The non-overclocked path works; "overclock for headroom" is performance-only and falls back cleanly if removed.
5. **Hot-reconfigure during DMA in-flight.** What happens if the user changes `pinMap` via the UI while the *current* frame's DMA is mid-transmission? The architecture must define this: most likely "topology changes are taken into account on the next frame's `push()`; the in-flight frame completes on the old topology". `onTopologyChange()` blocks until DMA-quiescent before re-allocating peripheral resources.
6. **WiFi-coexistence empirically variable.** Different routers, different switches, different cable conditions all interact with the WiFi stack's CPU-burst patterns. Our two tracks (DMA-driven + core pinning) cover the known modes; there's an unknown unknown around very-large installs (50K+) that no public library has fully solved.
7. **Identity-mapping path subtlety.** The fast path requires the Layer's buffer layout to *exactly* match the wire layout (no gamma, no brightness, no color-order swap on the way out). If the driver wants to apply gamma/brightness in `push()`, the identity path is gone — we re-introduce the copy. The control-binding shape should make this trade-off explicit per backend. **Resolved for clockless LEDs:** a WS2812-class driver must encode RGB into a pulse pattern anyway, so it already allocates a separate DMA buffer — the `Correction` (brightness/reorder/white) fuses into that encode pass at no extra buffer cost. The true zero-copy-to-wire identity path only ever applied to byte-identical protocols (ArtNet/DMX, APA102-SPI), where there's no encode and correction is the only reason to copy. So "identity → no copy" is a property of the *protocol*, not the mapping.
8. **Build vs borrow lock-in.** Whichever scenario Stage 2 picks, switching later is expensive (months, not weeks). The hybrid escape hatch (Scenario A for some backends, Scenario B for others) reduces this but doesn't eliminate it.

## Stage-2 candidates (revised)

The original list still stands; the answers above narrow it:

1. **FastLED master — modular driver layer** (Scenario A backbone). Confirmed in scope.
2. **I2SClocklessVirtualLedDriver — fork-and-vendor** for the virtual-driver shape. Confirmed in scope (custom PCBs OK).
3. **WLED-MM bus layer extraction** (Scenario A alternative for ESP32-only). Confirmed: license compatible either way.
4. **NeoPixelBus directly** — superseded by 1+3; deprioritise unless 1 and 3 both fall through.
5. **Hybrid hpwit non-virtual + FastLED** — covers the 1-30K WS281x case without shift registers. Still a strong middle option.
6. **ObjectFLED + FastLED Teensy path** — confirmed in scope (Teensy 4.1 is near-term).

Pick 2-3 for Stage 2. **See the "Recommendation — walk Scenario B" section above** for which to pick under that recommendation (the answer is the two spikes at the head of "Suggested order of Stage-2 work" — `LcdCamLedDriver (None)` on S3 first as a contract-shape spike, then `LcdCamLedDriver (ShiftReg)` to validate the multiplex refactor against hpwit's existing S3 implementation as the reference).

If you reject the Scenario-B recommendation and want to evaluate Scenario A or a hybrid instead: **#1 (FastLED master) + #2 (virtual driver vendor) + #6 (Teensy via ObjectFLED)** is the trio that exposes the most surface for Stage-2 to characterise. Be aware that this is the trio that produces *Scenario A + a vendored multiplex bypass for the virtual driver*, which is the hybrid path the recommendation explicitly names as the fallback if the Scenario-B multiplex spike fails.
