# The shift-register (74HCT595) LED driver — fan-out mechanics, memory ceiling, module shape

> **Forward-looking research document — exception to the CLAUDE.md present-tense rule.** Researched 2026-07 against primary sources: hpwit's `I2SClocklessVirtualLedDriver` and `I2SClocklessLedDriver` read line-by-line at `main` HEAD, plus MoonLight's own shipped `ParallelLedDriver` / `ParallelSlots` code and measured ceilings. Every load-bearing number is cited in § 7. Input for a `/plan`, not an approved plan.

## 1. Verdict

**The shift-register driver is buildable on the shipped parallel drivers — but only on the S3, and only through the i80/LCD_CAM path.** The fan-out itself is cheap and generic; the memory is what decides the chips.

The one number everything hangs on: **the DMA buffer grows 8×**, exactly in step with the fan-out factor. This was the open question and it is now settled from source two ways (§ 4). The buffer growth is *not* avoidable by clever encoding — it is the direct consequence of serialising 8 strands through one physical pin.

The subtlety worth stating plainly, because it cuts both ways:

- **Adding strands is free.** 15 lanes and 48 lanes cost the *same* DMA buffer. Lanes ride the slot **width** (the physical pin count), and 48 lanes = 6 physical pins, still inside one 8-bit bus. **A 48×256 frame is no bigger than a 15×256 frame.**
- **The ×8 fan-out is not free.** It multiplies the slot **count** 8×, because each WS2812 bit must now clock 8 shift positions instead of 1.

So the frame is **~145 KB** for *both* targets (§ 5), and that single figure is the gate:

| Chip | Path | 15×256 (3,840) | 48×256 (12,288) | Why |
|---|---|---|---|---|
| **ESP32-S3** | i80 / LCD_CAM | ✅ | ✅ | DMA buffer allocates from **PSRAM**; already proven to 16,384 lights (~150 KB) on this exact path |
| **classic ESP32** | i80 / I2S | ❌ | ❌ | I2S DMA **cannot address PSRAM** — buffer is internal-only against a ~76 KB largest-block wall. ~145 KB does not fit, at *either* target |
| **ESP32-P4** | Parlio | ❌ | ❌ | **65,535-byte single-shot transfer cap** (hardware). ~145 KB is 2.2× over it |
| **ESP32-P4** | i80 / LCD_CAM | ✅ *(untested)* | ✅ *(untested)* | P4 has LCD_CAM too; the i80 driver is already registered on it. **This is the P4's viable route** |
| **any chip** | RMT | ❌ | ❌ | Structurally impossible (§ 6.2) |

**Blunt version: classic ESP32 cannot do the shift-register driver at any useful size, and Parlio cannot do it at all.** The PO's 48×256 target is an **S3 feature** (and probably a P4-over-i80 feature). If the plan assumes StarLight's numbers, note those were achieved on hpwit's **PSRAM-fed refill ring**: a different memory model MoonLight does not have and has deliberately [parked](led-driver-psram-ring-analysis.md).

**The good news, and it is genuinely good:** on the S3 this needs **no new memory model, no new peripheral, and no new driver class**. It is a fan-out *option on the drivers we already ship*, and the 48×256 floor lands inside a buffer size the S3 is measured to handle.

## 2. The hardware

The board this driver targets is **hpwit's expander board** (the physical article, not just his library). Its BOM for **48 strands**, confirmed off the board itself:

| part (as fitted) | role | count |
|---|---|---|
| **74HCT595N** (NXP) | shift register — the actual 1→8 fan-out | **6** (6 × 8 = 48) |
| **SN74HCT245N** (TI) | **octal** buffer / level-shifter (stores nothing): 3.3 V → 5 V, and supplies the drive current | **1** |

Plus **one shared CLOCK line** and **one shared LATCH line** across all six '595s (they clock in lockstep).

**The ×8 is the shift register's width** — each '595 has 8 parallel outputs. That is where the factor comes from; it is *not* an arbitrary tunable. (Matches the source: `NUM_VIRT_PINS` = 7, used everywhere as `NUM_VIRT_PINS + 1` = 8.)

**One '245 is exactly enough, and that is not a coincidence:** the signals needing the 3.3 → 5 V shift are **6 data lines + CLOCK + LATCH = 8**, which is precisely a '245's width. A '245 is a bus *transceiver*, so its direction pin (DIR) is strapped for a fixed A→B direction and OE tied active — it is used as a plain octal buffer here.

**The `T` in 74HC*T* is load-bearing.** At 5 V a **74HC** input needs V_IH ≥ 0.7 × Vcc = **3.5 V**, and the ESP32 only drives **3.3 V** — *below* threshold, so a HIGH is not guaranteed to read as a 1. It typically *seems* to work on the bench (a given chip may trip nearer 2.5 V at room temperature) and then fails with temperature, supply, or a new batch: **flaky/garbled strands, not dead ones** — the worst kind of fault to chase. **74HCT** has TTL-compatible inputs (V_IH = **2.0 V**), so 3.3 V is unambiguously a HIGH while the outputs still swing a full 5 V, which is what WS2812 wants. Same margin problem, and same family of fix, as the data-line level shifter in [led-signal-integrity.md](../../how-to/led-signal-integrity.md).

**Timing headroom is thin, and the buffer is why — know this before blaming the firmware.** The fitted '245 is **plain HCT** (t_pd ≈ 10–18 ns at 5 V), *not* the ~5 ns **A**HCT part. Shift mode clocks the bus at **26.67 MHz** — a **37.5 ns** period — so the buffer alone can consume roughly a third of the bit period in propagation delay.

**But do not pre-emptively panic (or pre-emptively buy).** hpwit runs **19.2 MHz** on this same hardware and it works; we ask for 39% more. The parts are not marginal by design, they are marginal by *headroom*.

**The clock is a DURATION, not a divider — and it is NOT a free sweep knob.** The bench result that fixed the first bench run: 20 MHz was picked "because it divides exactly" and gave 8 × 50 = **400 ns** slots, over the WS2812B **T0H max (~380 ns)** — the strands rendered scattered max-brightness pixels and washed-out white (zeros read as ones; [lessons.md](../past/lessons.md) #5). The in-spec band for a ×8 slot is **290–380 ns → pclk 21.1–27.6 MHz**, and **26.67 MHz (prescale 3 of the 80 MHz bus resolution) is the only exact divide inside it**. `esp_lcd` silently rounds an inexact rate *down* into a wrong waveform rather than erroring, so an inexact clock would fake a hardware fault.

**So do not "test" a buffer-timing hypothesis by lowering the clock** — a lower pclk makes the slot *longer*, pushing T0H further past 380 ns, and the symptom it produces is the same washed-out white. There is no lower exact divide in the band. If the buffer is genuinely suspected, the falsifiable test is the *part*: an **AHCT245** (~5 ns) is a drop-in that restores the margin, and the symptom either tracks it or the buffer is innocent.

This is the same "make the hardware hypothesis falsifiable before reaching for the soldering iron" discipline as the TX-power sweep in [led-signal-integrity.md](../../how-to/led-signal-integrity.md).

**Total GPIO cost = `physicalDataPins + 2`.** hpwit's headline "120 strips from 15 pins" is `NBIS2SERIALPINS = 15` data pins × 8 outputs = 120, **plus** the clock and latch pins — so 17 GPIOs in total, not 15. Worth stating because it changes the pin budget.

**Is the ×8 configurable?** Effectively no, and this matters for the control design. `NUM_VIRT_PINS` is `7` (`:119`), used everywhere as `NUM_VIRT_PINS + 1` = 8 — it is the shift depth, hard-wired to the '595's 8 outputs. A ×16 would mean cascading two '595s per pin, which doubles the buffer again (§ 5) and doubles the shift time per bit. **Treat ×8 as a hardware constant, not a user control.**

### 2.0 The pin map (reverse-engineered from the StarLight board)

Read off a working StarLight install on the 48-strand board (read-only; raw capture + full flash backup in `.snapshots/starlight-48pin-board/`). Its live-mapping script and the runtime `addPin` calls agree:

| role | GPIO |
|---|---|
| **Data (6)** | **9, 10, 12, 8, 18, 17** ← order matters; it decides which strand is which |
| **CLOCK** | **3** (this is our i80 `clockPin` / WR — the '595 shift clock) |
| **LATCH** | **46** |

6 pins × 8 outputs × 256 = **12,288 lights** on a 128×96 panel (8×6 grid of 16×16, serpentine, wired right-to-left).

**The one adaptation: StarLight uses SIX data pins; the i80 bus needs SEVEN.** The `esp_lcd` i80 bus width is a power of two, and the latch occupies one bus bit, so the data pins + latch must total exactly 8. StarLight's I2S driver has no such constraint (its script even notes *"for virtual driver, max 6 pins supported atm"*).

The fix is a **ghost lane**: add a spare GPIO as the 7th data pin. It drives no '595, so its 8 strands must never light — and they don't, for free: `ledsPerPin` is a **per-strand** list, and a single number broadcasts until the buffer runs out. With `"256"` and a 12,288-light buffer, the first 48 strands get 256 each and **the ghost pin's 8 land on 0 automatically** (verified against `assignCounts`). No hand-written list, no chance of a stray lane.

### 2.1 Loopback wiring — which wire feeds back into the S3

The driver's loopback self-test works with the expander fitted, and it is a **stronger** test than the direct-mode one: it verifies the whole chain — encode → i80 bus → shift register → latch → output — rather than only the ESP32 half. But **the jumper comes from a different place, and the wrong wire can damage the S3.**

**The wire: the first '595's Q7 output → a spare S3 GPIO (`loopbackRxPin`), through a level shifter.**

- **Why Q7 and not Q0.** The test pattern is driven on **strand 0**, which is data pin 0's register at shift position 0. A '595 shifts **MSB-first** — the bit clocked in *first* ends up on the *last* output — so strand 0 emerges on **Q7** (the last of the QA…QH row), not Q0. Wiring Q0 captures a different strand's data and fails every bit, which would look exactly like a firmware bug.
- **⚠ The '595 output swings to 5 V. An ESP32 GPIO is not 5 V tolerant.** Do **not** wire Q7 straight to a GPIO. Divide it: **1 kΩ from Q7 to the GPIO, 2 kΩ from that GPIO to GND** gives 5 V × 2/3 ≈ 3.3 V. (Any 5 V → 3.3 V shifter works; the divider is just the two-resistor version.)
- **Set `loopbackRxPin`** to that GPIO, and turn `loopbackTest` on. `loopbackTxPin` is **ignored** in pin expander mode — the strand is decided by which register output the jumper is on, not by which pin transmits.

**A mis-wired jumper reads as a bit FAIL, not "jumper not detected."** The GPIO continuity pre-check is deliberately **skipped** in pin expander mode: it drives the TX pin and expects the RX pin to follow directly, which is true of a bare jumper but false through a shift register (raising the serial input does not raise an output — that takes 8 clocks and a latch). So the bit-verify is the only proof the wire is right, and it is the better proof anyway.

## 3. The wire format

Two lines carry the fan-out, and they are handled *differently* — this is the detail that decides the buffer size, and it is easy to get wrong.

- **CLOCK is peripheral-driven, and costs zero DMA bytes.** `gpio_matrix_out(clock_pin, deviceClockIndex[I2S_DEVICE], ...)` on classic; `esp_rom_gpio_connect_out_signal(clock_pin, LCD_PCLK_IDX, ...)` on S3 (`:581`, `:594`). It is the peripheral's own **pixel clock output** — hardware toggles it once per bus word, automatically. **This is precisely our i80 driver's existing `clockPin` (WR).**
- **LATCH is a data lane, and it does cost DMA bytes.** `gpio_matrix_out(latch_pin, deviceBaseIndex[I2S_DEVICE] + NBIS2SERIALPINS + 8, ...)` (`:579`) routes it to a **regular I2S data line**, sitting immediately above the `NBIS2SERIALPINS` data pins. So the latch is **a bit inside every DMA bus word**, set by the transpose.

That is what the `+1` in the sizing expression is: **the latch row**, not a spare shift cycle.

**Per WS2812 bit, the DMA clocks out `3 slots × 8` = 24 bus words** (vs 3 in the direct driver). Within each of the 3 WS2812 thirds, the 8 strands' bits are shifted into the '595s one bus word at a time; the latch lane pulses to present them. The bus word's bit L is physical data pin L (as ours is), so the **existing `ParallelSlots.h` bit-plane transpose is the right primitive** — it just runs 8× per slot with a different lane-gather.

## 4. The buffer growth — settled from source

Two independent confirmations, because this number decides feasibility.

**(a) The sizing expression** (`I2SClocklessVirtualLedDriver.h:291`), verbatim:

```c
#define WS2812_DMA_DESCRIPTOR_BUFFER_MAX_SIZE ((NUM_VIRT_PINS + 1) * nb_components * 8 * 3 * 2 + _DMA_EXTENSTION * 4)
```

versus the **direct** driver's `nbComponents * 8 * 2 * 3`. Reading the factors: `nb_components` (3, RGB) × `8` (bits/byte) × `3` (slots/bit) × `2` (bytes per 16-bit bus word) — **identical in both**. The shift-register driver's *only* extra factor is `(NUM_VIRT_PINS + 1)` = **8**.

- direct: 3 × 8 × 3 × 2 = **144 B** per LED row
- his shift-register driver: **8** × 3 × 8 × 3 × 2 = **1,152 B** per LED row

**Exactly 8×.** The 3-slot-per-bit structure survives; the 8× rides on top of it.

**(b) The clock rate — the falsifiable tell.** Classic-ESP32 I2S, 80 MHz base, `f = 80 / (div_num + div_b/div_a)`:

| | `div_num` | `div_a` | `div_b` | Effective clock |
|---|---|---|---|---|
| **direct** (`I2SClocklessLedDriver.h:575-577`) | 33 | 3 | 1 | 80/(33⅓) = **2.40 MHz** |
| **his shift-register driver** (`I2SClocklessVirtualLedDriver.h:777-779`) | 3 | 6 | 7 | 80/(3⅙) = **19.20 MHz** |

**Ratio: exactly 8.00×.** And 19.2 appears *literally* in his source as a constant — `#define _BASE_BUFFER_TIMING (((NUM_VIRT_PINS + 1) * nb_components * 8 * 3 / 19.2) - 4)` (`:257`) — confirming 19.2 MHz is the intended shift-mode bus rate.

**The bus runs 8× faster and the buffer is 8× bigger, for the same WS2812 wire time.** That is the whole mechanism, and both readings agree. **The "buffer does not grow" hypothesis is refuted.**

## 5. The memory arithmetic

Our formula (`ParallelLedDriver::frameBytesFor`), with the fan-out factor added:

```text
frameBytes = lightsPerStrand × channels × 24 slots × slotBytes × outputsPerPin (1 or 8) + latchPad
```

Note **`lightsPerStrand`**, not total lights: the frame is set by the *longest lane*, and every lane clocks in parallel. That is why lane count is free.

For RGB (3 ch), 256 lights/lane, ×8 fan-out, 8-bit bus (`slotBytes` = 1), latch pad 864 B, 64-byte aligned:

| Target | Strands | Physical pins | DMA frame | ×2 (asyncTransmit) |
|---|---|---|---|---|
| **15 × 256** = 3,840 lights | 15 | 2 (+clk +latch) | **~145 KB** | ~290 KB |
| **48 × 256** = 12,288 lights | 48 | 6 (+clk +latch) | **~145 KB** | ~290 KB |

**Identical — the PO's two targets cost the same memory.** 48 lanes needs 6 physical pins, still an 8-bit bus.

Held against each chip's real ceiling:

- **classic ESP32** — buffer must be **internal** (`esp_lcd_i80_alloc_draw_buffer` rejects `MALLOC_CAP_SPIRAM` on the I2S backend; bench-confirmed). Largest free block ~76 KB. **~145 KB does not fit. Both targets fail.** Even 15×256 fails — the classic chip's problem is the ×8, not the lane count. Its direct ceiling is ~2,048 lights and the shift-register driver *lowers* the reachable count per byte, so there is no configuration that rescues it. Lifting this needs the parked PSRAM refill ring, which is a second, register-level classic driver.
- **ESP32-S3 (i80/LCD_CAM)** — buffer allocates **PSRAM-first** and the LCD_CAM GDMA bursts straight from external RAM. **Measured: 16,384 lights on this path (~150 KB, plus a second buffer for async).** ~145 KB is squarely inside that. **Both targets fit; async double-buffer fits too.**
- **ESP32-P4 (Parlio)** — dead on a **hardware** limit, not a memory one: Parlio's single-shot transfer caps at **65,535 bytes** (measured: 897 RGB lights/lane). ~145 KB is **2.2× over the cap**, and the frame fails to transmit. Separately, the P4's Parlio PSRAM allocation is measured to degrade to internal RAM anyway. **Parlio cannot host the shift-register driver** without the chunked-transfer backlog item.
- **ESP32-P4 (i80/LCD_CAM)** — the P4 *has* LCD_CAM and `I80LedDriver` is already registered on it. This route has no 65 KB cap and reaches PSRAM. **This is the P4's viable path**, and the bus clock it needs is bench-confirmed (§ 5.1). The frame size itself is untested on this chip at ~145 KB.

### 5.1 The bus clock — bench-confirmed on both LCD_CAM chips

The one platform unknown was whether `esp_lcd` grants the ~8× pixel clock, since we go through IDF's i80 layer rather than hpwit's raw registers. **Measured on real silicon, 2026-07-14 — it does, on both chips:**

```
W mm_i80: CLOCK SPIKE: request 20000000 Hz -> GRANTED (prescale 4 -> granted 20000000 Hz)
```

(`MoonLight-testbench-S3`, esp32s3-n16r8, and `MoonLight-testbench-P4`, esp32p4-eth. A throwaway IO device opened at the shift-mode rate before the real device claims the bus; probe reverted afterwards, both boards restored.)

**Ask for 26.67 MHz, not hpwit's 19.2 MHz.** `esp_lcd` derives an **integer** prescale from the bus resolution and *silently rounds down* — it only errors when the prescale is 0 or > `LCD_LL_PCLK_DIV_MAX` (64). So a bad clock is not an error, it is a wrong waveform, and the rate must be chosen to divide exactly:

| | source | bus resolution | prescale | granted |
|---|---|---|---|---|
| today (direct) | PLL160M | 80 MHz | 30 | 2.667 MHz (exact) |
| **pin expander mode** | PLL160M | 80 MHz | **3** | **26.67 MHz (exact, 300 ns slot)** |

Both S3 and P4 default to `LCD_CLK_SRC_PLL160M` with `LCD_PERIPH_CLOCK_PRE_SCALE = 2` → an 80 MHz bus resolution, and both cap the prescale at 64. hpwit's 19.2 MHz is an artifact of the **classic I2S fractional divider** (`div_num`/`div_a`/`div_b`), which LCD_CAM does not share. So the rate must be an exact divide AND land in the in-spec slot band (290–380 ns → 21.1–27.6 MHz): **26.67 MHz** (prescale 3) is the only one that does, giving a 300 ns slot — T0H 300 ns, T1H 600 ns, both comfortably inside spec. This is the same reasoning that already picked the exact `/30` for `kPclkHz`.

The P4 additionally exposes `LCD_CLK_SRC_APLL`, which could hit a true 21.33 MHz exactly if the 3-slot timing ever needs to be preserved to the nanosecond — an escape hatch, not needed for the recommendation.

**Sanity check against hpwit:** his 12,000-LED worked example runs on a classic ESP32 — because his **refill ring** holds only ~1,152 B × `__NB_DMA_BUFFER` in internal RAM and streams from a PSRAM framebuffer. Our whole-frame model materialises all ~145 KB at once. **Same fan-out, different memory model, and that is the entire reason classic works for him and not for us.** Do not read his light counts as reachable on our path.

## 6. Module shape

### 6.1 Recommendation: a `pinExpander` checkbox + `latchPin` on the existing parallel drivers

Three shapes were on the table. Recommending **(c)**: two controls — `pinExpander` (a checkbox: is the expander board fitted? shipped as `MoonLedDriver::pinExpander`; this analysis originally proposed the name `pinExpanderMode`) and `latchPin` (a GPIO) — with the ×8 as a hardware constant, not a user control. The '595's width is the chip's, not a setting, so there are exactly two wirings and a boolean says which.

- **(a) A separate `ShiftRegisterLedDriver` class** — rejected. It would duplicate the lane parsing, the window slicing, the correction LUT, the DMA double-buffer, the loopback self-test, the `wireUs` KPI: the ~250 lines `ParallelLedDriver` exists specifically to hold once. Adding a driver class to change *how bits are packed into an existing bus* is the sibling-class mistake.
- **(b) A submodule/child of an LED driver** — rejected. The fan-out is not a separable unit of behaviour with its own lifecycle; it is an *encoding mode* of the parent's DMA frame. A child module that reaches into its parent's DMA buffer is worse than a flag.
- **(c) Controls on the existing drivers** — **recommended.** The peripheral, the pins, the DMA path, and the encode are all *already there*; the pin expander mode changes the **encode function and the frame size**, which is exactly what `prepare()` already rebuilds live.

**Against the principles:**
- ***Default to subtraction*** — (c) adds two controls and one encode branch. (a) adds a class, a registration, a doc page, and a duplicate of everything the base owns.
- ***Common patterns first*** — a mode flag that changes a driver's output encoding is the ordinary shape. And the repo's own bottom-up analysis reached this conclusion independently: *"the shift-register driver is **not** a sibling backend — it's the ShiftReg multiplex applied on top of any parallel-clocked backend"*, and *"an expander variant is a configuration of an existing driver, not a new class."* Two prior passes and this one agree; that is a strong signal.
- ***Complexity lives in core*** — the transpose belongs in `ParallelSlots.h` (the shared encoder), so both drivers inherit it from one implementation.

**One caveat to weigh honestly:** the ×8 buffer growth means turning the checkbox on can *fail to allocate* where the direct config succeeded. That is fine and already handled — `busInit` is allocate-and-degrade and the driver idles with a status — but the **UI should make the memory consequence visible**, not silently fail. Worth a `frameBytes`-style read-only KPI next to the checkbox.

### 6.2 Genericity — and why RMT cannot

**The fan-out lives in the shared base, so i80 and Parlio both get it free.** The encode is peripheral-agnostic (a bus word is a bus word — the base's own doc says an i80 byte and a Parlio byte are identical), so `ParallelLedDriver::encodeRows` + a new `ParallelSlots.h` entry point covers both. Parlio can host it *architecturally* even though it cannot host it *usefully* (the 65 KB cap, § 5) — so write it generic, ship it on i80.

**RMT cannot do this, structurally.** RMT is **self-timed NRZ**: it emits one symbol stream per channel with per-symbol durations and **no clock line at all**. A 74HCT595 needs an external shift clock and a latch, and RMT has no way to emit a synchronous parallel bus word — its channels are independent, not lockstep lanes of one word. There is no encoding that rescues this; the shift register would have nothing to clock it. **Do not attempt it.** (The bottom-up analysis's driver matrix already marks the RMT × ShiftReg cell as impossible.)

## 7. What changes where

Minimal, and no new module:

| File | Change |
|---|---|
| `src/light/drivers/ParallelSlots.h` | **New encode entry point** for the shift-register wire format: 24 bus words/bit, the latch lane, the 8-position shift. Reuses `transposeLanes8x8` with a different lane gather. Host-testable, no platform include — pin it in `unit_ParallelSlots.cpp`. |
| `src/light/drivers/ParallelLedDriver.h` | Two controls (`pinExpander`, `latchPin`); `frameBytesFor` gains the ×8 factor; `encodeRows` branches to the shift encoder; `parseConfig` maps strands → physical pins (`ceil(lanes/8)`) and validates `latchPin` against the data pins + `clockPin`. Both `affectsPrepare` triggers. |
| `src/light/drivers/I80LedDriver.h` | Likely **nothing** — its `clockPin` (WR) already *is* the shift clock, which is a genuinely lucky fit. |
| `src/platform/esp32/platform_esp32_i80.cpp` | The bus clock must run **8× faster** in pin expander mode: **26.67 MHz** (prescale 3, exact, 300 ns slots) rather than the 2.667 MHz `kPclkHz`. Granted on both S3 and P4 — a `pclk_hz` the driver selects per mode, not a platform risk. |
| `docs/moonmodules/light/drivers.md` | Document the mode + the wiring + the memory cost. |

## 7.5 STATUS — shipped dormant; the transport bug is NOT yet understood (2026-07-14)

**The feature is in the tree but OFF by default** (`pinExpander` unchecked). Direct mode is proven unchanged across the bench boards it was regression-tested on: zero GDMA errors, all driving.

**Read this section as a list of things that are TRUE, and a list of things that were guessed and are FALSE.** Six hypotheses have now died on this bug, several of them written up here as if settled. The pattern is the lesson: each one explained the symptom, none survived a real test. Do not add a seventh theory before making a measurement that could refute it.

### What is TRUE (measured, reproducible)

1. **The encoder is correct.** A 74HCT595 simulator in `unit_ParallelSlots.cpp` replays the emitted bus words and asserts what the strand physically receives. LEDs light, content is visible.
2. **The failing mount always reports `need = pool + 1`.** The GDMA descriptor pool is sized by esp_lcd from `max_transfer_bytes` (= exactly one frame): `ceil(bytes/4032)` nodes. The mount asks for one more than that, at *every* frame size (38 vs 37 at 256 lights/strand; 29 vs 28 at 192). A constant, quantified off-by-one — the strongest clue we have. **Its cause is unknown.**
3. **`avail` sweeps 0..pool and never reaches `need`.** `gdma_link_mount_buffers` mounts every transfer from index 0 and counts only descriptors the DMA has released (`gdma_link.c:163-174`).
4. **`asyncTransmit` OFF is dramatically better** (PO observation, the single most useful data point so far — LEDs visibly clean vs. wild flicker). Not understood; **this is where the next investigation should start.**
5. **The P4 runs the identical 147 KB shift frame from PSRAM with zero errors and full wire time.** The S3 does not.
6. **The failure is silent to us:** `tx_color` returns `ESP_OK` (the enqueue succeeded); the mount fails later in the ISR, which discards the return value (`esp_lcd_panel_io_i80.c:895` — arguably an IDF bug worth reporting upstream).

### The internal-RAM / PSRAM cliff — found blind, by eye (PO, 2026-07-14)

The sharpest measurement of the whole investigation, and it was made **without knowing which memory the frame was in**: with pin expander mode preferring internal RAM, the PO swept `ledsPerPin` and reported

The `frame` column here is the **per-strand source-buffer size** (the internal-RAM copy the sweep was watching, at ~561 B/light), NOT the fully-encoded shift frame — the § "PHASE 2 DESIGN" table below counts the encoded 16-bit frame (~1,152 B/light) and so shows larger KB for the same light counts. Both are correct for what they measure; the cap analysis uses the **encoded-frame** figure.

| leds/pin | source buffer | verdict |
|---|---|---|
| 64 | 36 KB | smooth, flicker-free |
| **96** | **54 KB** | **smooth — the highest good value** |
| 128 | 72 KB | not good |
| 256 | 144 KB | wild flicker |

The board's largest free contiguous internal DMA block at the time: **~38 KB** (heap 217 KB free of 346 KB). So the good values are the ones whose frame lands (or nearly lands) in **internal RAM**, and the bad ones are exactly those that overflow to **PSRAM**. A blind visual sweep landing on the allocator's boundary is strong, independent confirmation: **a shift frame renders correctly from internal RAM and incorrectly from PSRAM, on the S3.**

Note the mismatch worth keeping in mind: the serial log still shows GDMA mount failures in states the PO calls visually clean. **The error count is therefore NOT a reliable success metric** — it was used as one for most of this investigation, which is part of why so many hypotheses "confirmed" and then died. Trust the strands.

**This caps the feature at roughly 96 leds/pin × 16 strands ≈ 1,500 lights** — *less* than direct mode already reaches, which is why internal-RAM-first is a labelled stopgap and not the destination. Getting PSRAM working (understanding the real mechanism, or streaming PSRAM through small internal chunks the way hpwit's ring and Espressif's RGB bounce buffers do) is what unlocks the large displays this feature exists for.

### What is FALSE (guessed, then refuted — do NOT re-derive)

| hypothesis | how it died |
|---|---|
| Pool too small → double `max_transfer_bytes` | **Tried. Made it WORSE** (189 failures, and it broke the previously-clean async-OFF path too). |
| Descriptors are never handed back (write-back disabled) | Refuted: `avail` climbs steadily to pool-size. |
| A frame-descriptor off-by-one we can fix by nudging `frameBytes_` | Refuted: a *fresh* bus mounts its full node count fine; only the repeating path degrades. |
| Silent S3 FIFO underrun | Built on the S3's *inability to report* underruns — absence of evidence, not evidence. The P4 (which CAN report) shows zero underruns while working. |
| The frame must live in internal RAM, not PSRAM | **Refuted twice.** The P4 works *from PSRAM*; the S3 fails *from internal RAM*. Also a dead end by design: internal DMA RAM is ~110 KB, which would cap a shift display *below* what direct mode already does — it saws off the branch the driver stands on. **PSRAM is what makes large displays possible; do not propose moving off it.** |
| PSRAM is too slow | The S3's **octal** PSRAM has ample bandwidth for 26.67 MB/s. Never plausible; should have been checked before assuming. |
| `trans_queue_depth = 1` | An early test "showing no change" was taken while accidentally in direct mode, so it proved nothing. Shift mode currently forces depth 1 anyway. |

### The likely mechanism (PO's hypothesis, 2026-07-14) — and why the fix belongs in the CORE, not here

**Whole-frame vs parts-of-a-frame.** Our path hands `esp_lcd` one gigantic PSRAM buffer and asks the GDMA to stream it, unassisted, at 26.67 MB/s from first byte to last — no CPU in the loop to cover a hiccup. hpwit's driver never does that: the DMA only ever reads **small internal-RAM buffers**, refilled by the CPU from data that may itself live in PSRAM. Same for Espressif's own RGB-LCD **bounce buffers**. The one place hpwit *does* DMA from PSRAM is the P4 Parlio path — **chunked** — which is consistent with our own result that a P4 runs the identical PSRAM whole-frame shift transfer perfectly while the S3 does not.

This explains both facts we could not otherwise account for: **`asyncTransmit` OFF is better** (two whole frames in flight against a descriptor pool esp_lcd sizes for one), and **the internal/PSRAM cliff** (a sustained PSRAM read has no slack; an internal one does).

**It is a hypothesis, not a diagnosis** — six well-fitting stories have already died on this bug. But it is *testable*, and testing it does not require leaving `esp_lcd`: stage the frame through a few internal-RAM chunks fed back-to-back.

**The fix belongs in the core, but the expander is not optional — it is the whole performance story.** The same staging mechanism lifts two bigger *unshifted* ceilings — **P4 Parlio's ~4,096-light contiguous-block wall** and the **classic ESP32's 2,048-light PSRAM-unreachable wall** — so it is tracked as a **core** item and should be **built and proven on the unshifted path first**, where the win is measurable on proven code. That is a sequencing rule about where to de-risk the mechanism.

It is **not** a claim that the expander is a nice-to-have. The WS2812 wire time is a physical constant (30 µs/light, serial per strand), so the only lever on frame rate is **lights per strand**: 16 direct lanes × 1024 = 16K lights is stuck at **33 fps**, while **48 strands × 256 = 12K at 130 fps**, which hpwit and the PO have *actually run* (StarLight). The expander is the only way to reach 48+ strands without spending 48+ GPIOs, and therefore the only route to 100 fps at this scale. The two mechanisms buy different things: **staging buys lights, the expander buys fps**, and they compound: the expander's own ~145 KB frame is precisely the one that fails from PSRAM today. See [backlog-light § Chunked transfer](backlog-light.md).

### PHASE 2 DESIGN — the encode-into-the-ring (2026-07-14, arithmetic done, not yet built)

**The cap is now understood exactly**, and the fix follows from it. The frame scales with *lights per strand* (all strands clock in parallel), and in pin expander mode the encoder emits **1,152 bytes per light** (3 ch × 8 bits × 3 slots × 8 shift-words × 2 bytes on a 16-bit bus):

The `encoded frame` column is the **fully-encoded shift frame** (~1,152 B/light on the 16-bit bus) — the number the DMA actually streams, and the one the cap depends on. (The § 7.6 sweep table above reports the smaller per-strand *source* buffer instead, hence its lower KB for the same light counts.)

| lights/strand | encoded frame | |
|---|---|---|
| 96 | 108 KB | fits internal DMA RAM (~110 KB) — **this is why 96 is the cap** |
| 128 | 144 KB | PSRAM only → stalls |
| 256 | 288 KB | PSRAM only → stalls |

**The DMA cannot read PSRAM at the expander's clock (53 MB/s), and neither can the CPU** — it is the same memory over the same bus, and the CPU is not faster at bulk reads than the DMA. So *any* design that keeps a big encoded frame in PSRAM is dead, whoever reads it. (This is why hpwit's frame buffer is a plain `calloc` — internal RAM. He does not solve the PSRAM problem; he never has it.)

**So: never materialise the encoded frame at all.** The DMA loops a small ring of *internal* buffers holding a few transposed lights; as each drains, the CPU encodes the next slice straight into it, reading from the **Layer buffer** — which is internal, and 24× smaller than the encoded output (3 bytes/light vs 1,152). PSRAM leaves the path entirely. hpwit, independently: *"you need to hack the interrupt to stop at every pixel frame instead of the full frame, which allows you to store only a buffer of transposed pixels."* Same design.

**~~The deadline is comfortable, and the expander is why.~~ REFUTED ON THE BENCH (2026-07-17) — the "~3 µs encode / 7× headroom" estimate was wrong by ~15×, and it is the single most expensive error in this document.** The DMA does take **21.6 µs** to drain one light. The measured encode is **46 µs/light** (S3, `-O2`, 1-LED ring, after the prefill fix below) — **2.1× OVER the wire, not 7× under it.** There is no headroom; the producer is slower than the consumer, which is the one condition under which a ring cannot stream. Every "it works at N lights" result before this was the whole-frame fallback, not the ring (see § 7.6).

**Do not re-derive the encode cost from an estimate — measure it.** The estimate was arrived at by counting operations on paper; the bench disagreed by more than an order of magnitude. Same failure mode as the tap-hoist phantom (§ 7.6).

**The refill must run in the EOF ISR, in IRAM** — this is the load-bearing constraint, and it is why hpwit uses a level-3 IRAM interrupt. The alternative (encode ahead from the render task, ISR only advances descriptors) must survive a WiFi preemption of 1–2 ms, which needs a ~72 KB ring — at which point the frame buffer is back and nothing was gained:

| ring | tolerates a preemption of |
|---|---|
| 4 × 4 lights (18 KB) | 259 µs |
| 8 × 8 lights (72 KB) | 1,210 µs |

**What it costs us.** The encoder + the correction LUT + the SWAR transpose all become ISR-reachable and must be `IRAM_ATTR`; a flash access or a cache miss in that path is an underrun, and an underrun is a visible glitch. That is precisely the fragility the whole-frame design was chosen to avoid (see [MoonI80Peripheral](../../moonmodules/light/moxygen/MoonI80Peripheral.md)), and it is the price of going past 96 lights/strand on an S3. **Both drivers keep shipping**: `I80LedDriver` (esp_lcd, capped, bulletproof) and `MoonI80LedDriver` (ours, uncapped, real-time).

**The seam**, keeping the platform boundary intact — the platform owns the ring/descriptors/ISR, the domain owns the encode:

```cpp
using MoonI80EncodeFn = void(*)(void* user, uint8_t* dst, uint32_t firstRow, uint32_t rowCount);
bool moonI80Ws2812InitRing(handle, …, rowBytes, totalRows, MoonI80EncodeFn, void* user);
```

`ParallelLedDriver::encodeRows()` is *already* a per-row loop (`for (row = 0; row < maxLaneLights_; row++)`), so slicing it to a row range is a contained change that leaves every existing test valid.

## 7.6 The 1-LED ring — what the first attempt proved (2026-07-16/17)

**The ring was built and it streams.** `kRingRows=1` / `kRingBufs=32`: RAM **147 KB → 18 KB, constant at any strand length** — 256, 512, 1024 all cost the same. That was the whole point of the geometry and it works. What it does *not* yet do is meet the deadline: **46 µs encode against a 21.6 µs wire**. Start the second attempt from these four facts, not from the § 5 arithmetic.

### The encode deadline is BUYABLE, not fixed — hpwit's `_DMA_EXTENSTION`

The most useful thing learned, and it reframes the § 5.1 clock question. From his header:

```c
#define _DMA_EXTENSTION 0                                              // default
#define _NB_BIT (_DMA_EXTENSTION * 2 + (NUM_VIRT_PINS + 1) * nb_components * 8 * 3)
#define _BUFFER_TIMING ((_NB_BIT / 19.2) - 4)                          // <-- his encode deadline
```

**The extension sits inside the numerator of the deadline.** Padding a DMA buffer with zeros lengthens its *wire time*, so the ISR gets more microseconds to encode the next one. It is legal because the WS2812 only latches on a ≥300 µs LOW — his README: *"if you wait less than 150us than the led will pass the new data like it was sent just after"*. Sub-150 µs of zeros is a **pause, not a reset**. Cost: frame delivery time, i.e. fps.

**So hpwit is over his own budget too, and says so** — *"mine takes 50 microseconds"* against a 30 µs slot. He does not live inside the deadline; he **moved** it. That is the second mechanism he has and we do not (the first: PLL240M → 19.2 MHz → a 30 µs slot vs our 26.67 MHz → 21.6 µs). **He is not faster than us. He has a bigger budget, twice over.**

The headroom menu is therefore three items, not two:

| lever | buys | costs |
|---|---|---|
| PLL240M (19.2 MHz) | +8.4 µs/light | a peripheral clock-tree change (architecture-decision-worthy) |
| `_DMA_EXTENSTION` | arbitrary, tunable | **RAM per DMA buffer** + fps |
| a faster encode | the real fix | engineering |

**Why the extension does not rescue us** (check before reaching for it): we pad at 26.67 MHz vs his 19.2, so ~2.3× more zero bytes per µs bought; closing 46 → 21.6 needs roughly 576 B → ~1,200 B/light; and the pad is **per DMA buffer**, which at `kRingRows=1` means per light. Our heap's largest free block is **73,728 B** and the ring already fragments it. **RAM is the wall we are already standing against — the knob that saves him is the one we can least afford.**

**Verdict: at 46 µs neither PLL240M (+8.4 µs) nor an affordable pad closes 2.1×. The encode remains the lever.** hpwit agrees, unprompted: *"that is why the first version was only able to do 5:1 and I spent most of the time optimising the code to be able to do 8:1."*

**Two things from his README that DO transfer:**
1. **`__BRIGHTNESS_BIT`** — brightness as a power of 2 (`#define __BRIGHTNESS_BIT 5` → max 32), which *"will drastically decrease the time of the buffer calculation"* by removing brightness arithmetic from the ISR. Our `Correction::apply` runs **16 lanes × 16 rows per slice inside the ISR**. Independent evidence it is worth measuring; cheap to try.
2. **He ships the instrument** — `_max_pixels_out_of_time` (counts lights that missed the deadline) and `_proposed_dma_extension` (auto-tunes the pad from the measured max). Same idea as our `ringDbg` `enc`/`gap`, but self-correcting. Worth copying the *idea*.

### Where the 46 µs goes — the measured decomposition

Cycle-counted on an S3 with the real encoder (not a model). **The transpose + emit is the target; nothing else is worth attacking first.**

| stage | cycles | µs | share |
|---|---:|---:|---:|
| `memset(wire_)` | 131 | 0.5 | 2% |
| correction, 16 lanes | 1510 | 6.3 | 19% |
| **transpose + emit** | **6280** | **26.2** | **79%** |
| cache msync | 95 | 0.4 | 1% |
| **accounted** | **~8000** | **~33** | |

A diagnostic encoder that paints a self-generated pattern — no source read, no correction, no memset, just the 192 data stores — runs at **19 µs/light**. That is the floor: **the data stores alone are ~9 µs and irreducible.**

**Ruled out on the bench — do not re-attempt these** (each was measured, each was null):

| hypothesis | test | result |
|---|---|---|
| PSRAM snapshot reads | `allocIsr`, retested at 1 light/buffer | 50 → 50 µs |
| cache msync per refill | split counter | 0.4 µs (1%) |
| flash-resident ISR code | `IRAM_ATTR` on the encode chain | no change |
| 16 KB instruction cache | 32 KB | no change |
| per-call setup overhead | 4 rows/buffer (¼ the calls) | 48.3 vs 46 µs |
| `planes[]` staging spill | fused emit | byte-identical ELF |

**The lesson that outlives the numbers:** six optimisation attempts were spent on the encode *before* anyone built the 19 µs diagnostic encoder — which then showed the wall was still wrong, proving speed was not the fault at all (it was the frame-close bug, since fixed). **Build the cheap floor test first when an artifact might be correctness OR speed.**

### Prefill once per FRAME, not per slice — a per-slice cost becomes a per-light cost

**The trap, and it is a shape, not a typo.** The data-only prefill (§ 7.5) lays constants **per slice** — correct and cheap at `kRingRows=16`, amortised over 16 lights. At **`kRingRows=1` the slice IS one light**, so it fired once per light: **384 constant stores + 192 data stores = 576 = exactly the whole-slot encoder it had replaced.** The 4.4×-on-host win was cancelled to zero, **silently — no test failed, the bytes were correct**. Only a decomposition found it.

**The fix** (hpwit's `putdefaultones`, our own seam): a `MoonI80PrefillFn` called from `startRingTransfer` lays each buffer's constants **once at frame arm**; the ISR refill writes **data only**. Measured: **enc 66 → 46 µs/light (1.43×)**, frameTime 12021 → 8913 µs (83 → 112 fps). Biggest single win of the attempt.

**The general rule for the next attempt: re-cost every per-slice operation after ANY geometry change — the per-light budget is the only denominator that matters.** A win measured at one `kRingRows` does not survive another for free.

### LATENT BUG carried into the next attempt — ragged strands

`ringPrefillTrampoline` uses **row 0's active mask for every buffer**. Correct for **uniform strands only**. With ragged strands (different lights per pin — which the PO has called a hard constraint for all drivers, since end users mix strips and panels) an exhausted strand's lane is driven **HIGH** instead of released. **The tests pass only because the mock's geometry is uniform and misses it.** Needs prefill-per-equal-mask-run. Documented at the seam in the header.

### The instrument lies — fix before trusting any fps claim

The module header reports the **tick** rate (252 fps) while `frameTime` reports the real frame (83 fps). Every fps number in this document predating 2026-07-17 should be read with that in mind.

### Where to start next

**The scatter is diagnosed, see § 7.6 and [backlog-light.md](backlog-light.md).** The ring is clean iff `ringBufs − nSlices ≥ ~2` (a producer/consumer headroom margin, bench-bisected on the wall 2026-07-18). "More buffers" cannot reach 48×256 (the headroom RAM is ~145 KB regardless of geometry, the whole-frame wall); the fix is a refill that structurally TRAILS the DMA read head (hpwit's model), so headroom holds at any `nSlices` at constant RAM.

**The loopback RX path CAPTURES and bit-verifies** (fixed 2026-07-15, `2873ec9d`: "captures it back off the strand, bit-verifies 2304/2304 bits, textbook 300/600 ns pulse widths"; the R14 bit-0 settling artifact was measured on a captured strand, independent proof). But the current loopback builds a PRIVATE frame and transmits it — it does NOT go through the render ring, so it proves the peripheral, not the pipeline, and cannot observe a ring-scatter. An **intrusive** mode — capture what the LIVE ring actually put on the wire (via `captureAndVerifyFrame`, already decoupled from the transmit) — is the closed-loop instrument the ring fix needs, so a machine can bit-verify the frame reached the LEDs intact instead of relying on the PO's eyes.

## 8. Open questions for the PO

1. **Is this an S3-only feature?** That is where the arithmetic lands. Are you OK shipping 48×256 as S3 (+ probably P4-over-i80), with classic explicitly unsupported? If classic at 12 K lights is a *requirement*, that is the trigger to un-park the PSRAM refill ring — a much bigger piece of work, and it should be decided before a plan, not during.
2. ~~Does the 8× bus clock survive our stack?~~ **Answered — granted on both S3 and P4, and the rate is settled at 26.67 MHz** (the only exact divide inside the in-spec slot band; the first attempt at 20 MHz produced 400 ns slots and washed the strands white).
3. **Is the shift-register board built/sourced?** The ×8 is a property of the *hardware*; the driver can't be verified without it. Which board, and how many physical pins does it break out?
4. **Do you want ×16 (cascaded '595s)?** It doubles the buffer again (~290 KB) and the shift time. Recommend **no** for now — ×8 hits 48×256 already.
5. **`asyncTransmit` with a ~145 KB frame** wants ~290 KB of PSRAM. Fine on an N16R8, but confirm the target board's PSRAM budget.

## 9. Sources

**hpwit (primary — source read at `main` HEAD, 2026-07):**
- [`I2SClocklessVirtualLedDriver.h`](https://github.com/hpwit/I2SClocklessVirtualLedDriver/blob/main/src/I2SClocklessVirtualLedDriver.h) — `:119` `NUM_VIRT_PINS 7`; `:122` `NBIS2SERIALPINS`; `:257` `_BASE_BUFFER_TIMING … / 19.2`; `:291` `WS2812_DMA_DESCRIPTOR_BUFFER_MAX_SIZE = (NUM_VIRT_PINS+1) * nb_components * 8 * 3 * 2 + …`; `:567` `setPins(Pins, clock_pin, latch_pin)`; `:579` latch → I2S **data** lane; `:581`/`:594` clock → `deviceClockIndex` / `LCD_PCLK_IDX` (**peripheral clock**); `:777-779` `clkm_div_num=3, div_a=6, div_b=7` → **19.2 MHz**. Also (read 2026-07-17, § 7.6): `_DMA_EXTENSTION` (default 0) inside `_NB_BIT`, hence inside `_BUFFER_TIMING` — **the deadline is a tunable, not a constant**; `WS2812_DMA_DESCRIPTOR_BUFFER_MAX_SIZE (576*2)` and `__NB_DMA_BUFFER 10` on the S3 branch; `__BRIGHTNESS_BIT` (default 8) → `__HARDWARE_BRIGHTNESS`; `_max_pixels_out_of_time` + `_proposed_dma_extension` (his deadline-miss counter + auto-tuner).
- [`I2SClocklessVirtualLedDriver` README](https://github.com/hpwit/I2SClocklessVirtualLedDriver) — chapters *"Increase the buffer length"* (the <150 µs pause rule: *"if you wait less than 150us than the led will pass the new data like it was sent just after"*), *"Reduce buffer calculation time"* (`__BRIGHTNESS_BIT`), *"Artifacts due to interrupts"* (`__NB_DMA_BUFFER`).
- **hpwit direct (Discord, 2026-07-17)** — *"the first version was only able to do 5:1 and I spent most of the time optimising the code to be able to do 8:1"*; *"mine takes 50 microseconds"* (against his own 30 µs slot); and the standing offer: *"If I can look at the code I could give you some hints."*
- [`I2SClocklessLedDriver.h`](https://github.com/hpwit/I2SClocklessLedDriver/blob/main/src/I2SClocklessLedDriver.h) — `:575-577` `clkm_div_num=33, div_a=3, div_b=1` → **2.4 MHz**; the 144 B direct buffer.

**MoonLight's own code + measurements:**
- `src/light/drivers/ParallelLedDriver.h` — `frameBytesFor`, the CRTP base, `asyncTransmit`.
- `src/light/drivers/ParallelSlots.h` — the 3-slot wire contract + SWAR transpose.
- `src/platform/esp32/platform_esp32_i80.cpp` — PSRAM-first on LCD_CAM, internal-only on classic I2S (`SOC_LCDCAM_I80_LCD_SUPPORTED` gate).
- `src/platform/esp32/platform_esp32_parlio.cpp` — the PSRAM→internal degrade.
- [performance.md § Multi-pin LED driving](../../reference/performance.md) — Parlio **65,535 B/lane** single-shot cap (897 RGB lights/lane); S3 i80 **16,384 lights** on PSRAM.
- [led-driver-psram-ring-analysis.md](led-driver-psram-ring-analysis.md): the classic ~2,048 ceiling; the parked refill ring; the shift-register driver's 12,288 floor.
- [leddriver-analysis-bottom-up.md](leddriver-analysis-bottom-up.md) — "the multiplex is a configuration of a parallel-clocked backend, not a sibling driver class"; the RMT × ShiftReg impossibility.
