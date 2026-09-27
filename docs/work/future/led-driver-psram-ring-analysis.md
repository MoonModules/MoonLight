# The classic-ESP32 PSRAM ceiling — the refill-ring memory model

> **Forward-looking research document — exception to the CLAUDE.md present-tense rule.** One question: **can PSRAM lift the classic-ESP32 LED ceiling MoonLight measured at 2048 lights?** Researched 2026-07 against primary sources — hpwit's driver source read line-by-line, plus Espressif's own docs. Every load-bearing claim is cited in § 4.
>
> **Status: PARKED FINDING — not a queued task.** The answer is **yes, via a refill ring** — but that is a **second classic-ESP32 driver** (it cannot be a flag on the shipped i80 driver), and **nothing currently planned needs it**: the shift-register driver builds on the existing i80/Parlio base, S3/P4-first, where the DMA already reaches PSRAM. **§ 3 holds the verdict and the trigger that would un-park it.** Read § 3 before treating anything here as a plan.

## 1. Verdict

**Yes — PSRAM can lift the classic-ESP32 ceiling, and the mechanism is a memory-model change, not a shift-register feature.** Our 2048-light wall is a direct consequence of the *whole-frame* DMA model: `esp_lcd`'s i80 backend materialises every light's WS2812 waveform in one contiguous DMA-capable block before the transfer starts, and on the classic ESP32 that block **cannot be PSRAM** — the IDF source rejects it outright (`esp_lcd_panel_io_i2s.c`: `ESP_RETURN_ON_FALSE((caps & MALLOC_CAP_SPIRAM) == 0, NULL, TAG, "external memory is not supported")`), because the classic chip's I2S DMA physically cannot address the external-RAM window. So the DMA buffer scales linearly with the light count against a ~76 KB internal wall.

hpwit's driver uses a **streaming refill ring** instead: a handful of tiny DMA buffers (144 bytes each for RGB — *one LED slot across all 16 lanes*), refilled on the fly by the I2S completion ISR, which transposes the next pixel row out of the source color array as it goes. **The DMA never touches the source array — the CPU does, inside the ISR** — so the source array is under no DMA constraint and **can live in PSRAM**. Internal RAM becomes **constant (~1.2 KB at the default `nbDmaBuffer = 6`), independent of the light count**. That is the whole ballgame: the ring **decouples LED count from internal RAM**, for **any** driver built on it, expander or not.

**The ring must be a second classic driver, not a flag on the existing one.** Our i80 driver goes through IDF's `esp_lcd`, which *owns* the DMA and only does whole-frame. **No IDF API can feed PSRAM into classic-ESP32 I2S DMA** — Espressif's own workaround *is* the ring ("add a buffer in internal memory that is DMA'able, and use memcpy to move data from/to that buffer"). So a ring driver means going under `esp_lcd` to the raw I2S registers, exactly as hpwit does, and it lands **alongside** the i80 driver as a user choice: **i80 = simple, WiFi-underrun-immune, ~2 K lights; ring = PSRAM-fed, many-K lights, needs the `nbDmaBuffer` flicker cushion.**

**A scoping fact that shapes the work: the ring is not available off the shelf.** This is not a matter of adopting a library. The mainstream ESP32 LED libraries use the same **whole-frame DMA** we do and therefore share our internal-RAM ceiling; the refill-ring model is effectively unique to hpwit's drivers, which is why § 2 studies **his source** rather than surveying alternatives. Two consequences: (1) building it is genuinely new capability for a classic-ESP32 LED controller, not catching up; (2) per [*Industry standards, our own code*](../../CLAUDE.md#principles), we study his implementation hard and then **write our own against our architecture** — carry the idea forward, not the code.

## 2. The PSRAM question

### 2.1 Two memory models

**Whole-frame (MoonLight today, via `esp_lcd` i80).** The DMA buffer *is* the frame: every light's expanded WS2812 waveform — 3 slots per bit, one byte (8-lane) or two (16-lane) per slot — materialised in one contiguous DMA-capable block, handed to the peripheral in a single chained transfer. The CPU is out of the loop for the whole frame, which is why this model is **underrun-immune by construction**: nothing remains to refill, so no ISR storm can starve it. The price is linear scaling:

```text
internalBytes(whole-frame) = maxLaneLights × channels × 3 slots × slotBytes
```

**Refill ring (hpwit, under the raw I2S registers).** The DMA buffer holds only a small rolling window: a ring of `nbDmaBuffer` buffers, each **one pixel-row of slots** wide. On each buffer-EOF the I2S ISR calls `loadAndTranspose()`, which reads the next pixel row from the source array and SWAR-transposes it into the buffer the DMA is about to reach. Internal RAM:

```text
internalBytes(ring) = (nbDmaBuffer + 2) × (nbComponents × 8 bits × 3 slots × 2 bytes/word)
                    = (nbDmaBuffer + 2) × 144 B   [RGB]
```

— **no LED-count term.** That is the decoupling, and it is the entire point.

**Crucially, "ring buffer" names two different things and only one solves the ceiling.** The distinction is the whole decision, and it is easy to lose:

| | **(A) Refill ring** | **(B) Ping-pong / double buffer** |
|---|---|---|
| What's in a buffer | one pixel row of slots | a **whole frame** |
| Who fills it, when | the **DMA-completion ISR**, mid-transfer | the CPU, **before** the transfer |
| Source array touched by | **CPU only** → **PSRAM-eligible** | DMA → must be internal |
| Internal RAM vs LED count | **constant** | **scales linearly** |
| Buys you | **lifts the memory ceiling** | hides wire time behind DMA |
| In our tree | *(this proposal)* | **`asyncTransmit`** (already shipped) |

We already have (B). It is genuinely valuable — it is what took the S3 from ~13 to ~34 fps at 16 K — but **it does not and cannot lift the memory ceiling**, which is exactly why halving the DMA buffer by turning `asyncTransmit` off didn't help the WROVER.

### 2.2 What hpwit's source actually says

Read directly from [`src/I2SClocklessLedDriver.h`](https://github.com/hpwit/I2SClocklessLedDriver/blob/main/src/I2SClocklessLedDriver.h) / `.cpp` (branch `main`):

- **A ring of linked `lldesc_t` descriptors.** `struct I2SClocklessLedDriverDMABuffer { lldesc_t descriptor; uint8_t* buffer; };`. `initDMABuffers()` allocates `nbDmaBuffer + 2` buffers of `nbComponents * 8 * 2 * 3` bytes each (**144 B RGB / 192 B RGBW**) plus one 4×-size terminator. Chained `descriptor.qe.stqe_next = &next->descriptor`, last wrapping to `[0]` — a genuine ring.
- **`uint8_t nbDmaBuffer = 6;`** — default 6, and **runtime-tunable** via `updateDriver(..., uint8_t dmaBuffer, ...)` → `nbDmaBuffer = dmaBuffer;`. (This is why the product owner's StarLight-era setting of **75** was reachable: it's a plain count, not a compile-time constant.)
- **The ISR refills and transposes.** `static void IRAM_ATTR interruptHandler(void*)`: on `I2S_OUT_EOF_INT` it increments `ledToDisplay` and calls `loadAndTranspose()`, which reads `driver->leds + ledToDisplay * nbComponents` per strip, applies brightness/gamma maps, and SWAR-transposes (`transpose16x1Noinline2`) into `dmaBuffersTampon[dmaBufferActive]->buffer`. **One buffer = one LED slot, so the ISR fires per LED (~30 µs at 800 kHz).**
- **Ring buffers are internal; the framebuffer is the caller's.** Ring: `heap_caps_malloc(bytes, MALLOC_CAP_DMA)`. Framebuffer: `initled(uint8_t* leds, ...)` takes a raw pointer the driver only ever reads **with the CPU**. Nothing DMA-touches it → **a PSRAM pointer works by construction.**
- **The ISR is deliberately arranged to be allowed to read PSRAM.** `esp_intr_alloc(interruptSource, ESP_INTR_FLAG_INTRDISABLED | ESP_INTR_FLAG_LEVEL3, ...)` with the in-code comment that `ESP_INTR_FLAG_IRAM` was *"removed to avoid Cache Disabled but Cached Memory Region Accessed"* — i.e. the refill ISR is explicitly permitted to touch cache-mapped (PSRAM) memory. That is the design decision that makes the PSRAM framebuffer legal, stated in hpwit's own source.
- **The whole-frame mode exists too, and is the one that scales.** `FULL_DMA_BUFFER` allocates `numLedPerStrip + 2` buffers — and is precisely the mode behind [issue #11, "DMA mode takes up 60k of ram just for a single 300 led strip"](https://github.com/hpwit/I2SClocklessLedDriver/issues/11). **hpwit's library contains both models, and the scaling complaint lands on the whole-frame one — the same wall we hit.**

**Honest caveat:** hpwit never writes "put `leds[]` in PSRAM" in so many words *for the classic chip* (the explicit PSRAM-framebuffer statements are in the S3 variants). The classic conclusion is drawn from construction — CPU-only reads from a caller-supplied pointer, in a non-IRAM Level-3 ISR — which is sound but is **inference from source, not a vendor promise.** The bench spike in § 5 is what turns it into a measured fact.

### 2.3 The memory math, concretely (8 lanes, RGB)

Whole-frame, `slotBytes = 1` (8-lane bus), 3 channels, 3 slots/bit — our i80 model:

| Lights/lane | Total (8 lanes) | Whole-frame DMA buffer | Fits ~76 KB internal? |
|---|---:|---:|---|
| 256 | 2,048 | ~18 KB | ✅ **measured working** |
| 512 | 4,096 | ~37 KB | ❌ **measured fail** (`i80 bus init failed`) — the real wall sits below the arithmetic: ~76 KB is the *largest free block*, and `asyncTransmit` wants two, plus IDF descriptor overhead |
| 1024 | 8,192 | ~74 KB | ❌ |
| 2048 | 16,384 | ~147 KB | ❌ |

Ring, same 8 lanes, RGB (hpwit's own sizing, 144 B/buffer):

| `nbDmaBuffer` | Ring internal RAM | Lights supported |
|---|---:|---|
| 6 (default) | (6+2) × 144 = **1,152 B** | **any** — 2 K, 16 K, 130 K; the count does not appear in the formula |
| 32 | (32+2) × 144 ≈ **4.9 KB** | **any** |
| 75 (StarLight-era) | (75+2) × 144 ≈ **11 KB** | **any** |

**Read those against each other.** The whole-frame model needs 147 KB of a ~76 KB budget to reach 16 K lights, so it cannot. The ring reaches 16 K — or 130 K — on **1.6–11 KB**, and even a *very* deep 75-buffer cushion costs ~11 KB. The frame itself moves to PSRAM, where 16 K lights × 3 B = 48 KB is a rounding error against 4 MB.

### 2.4 What the ring gives the WROVER, and what binds next

With the internal-RAM wall gone, the binding constraints become, in order:

1. **Wire time — the hard physical floor.** WS2812 is ~30 µs per light per lane (24 bits × 1.25 µs). At 8 lanes × 2048/lane = 16,384 lights: 2048 × 30 µs = **~61 ms → ~16 fps**, before any CPU cost. A protocol constant no driver escapes; it is why "more LEDs" past a point is a *distribution* problem.
2. **Encode CPU in interrupt context — the ring's true price.** Our whole-frame model transposes once with a SWAR kernel on the main path out of internal RAM (measured **~7.6 µs/light** on classic). The ring does comparable work **inside an ISR, per LED, reading cache-mediated PSRAM**. hpwit fires that ISR once per LED (~30 µs apart) — so the refill must reliably complete within the wire time of one pixel. This is the number to measure first.
3. **PSRAM bandwidth / cache stalls** — the same story from the other side: an ISR that stalls on a cache miss is an ISR that is late, which is item 4.
4. **The underrun cushion** (§ 2.5).

**Honest bottom line for a 4 MB-PSRAM WROVER with a ring driver:** the **memory ceiling disappears** (the frame is kilobytes against megabytes) and the practical ceiling becomes **wire time and ISR headroom** — comfortably into the **8–16 K** range at single-digit-to-low-teens fps. That is a **4–8× lift over our measured 2048**, and the right framing is *"the ceiling stops being about RAM."* **This is not theory:** a prior firmware built on hpwit's ring (the product owner's own MoonLight / WLED-MM lineage) raises its light boundary to **130 K on PSRAM against 4096 without it** — a ~32× gap between the PSRAM and non-PSRAM configuration of the *same* firmware, which is the empirical shape of ring-vs-whole-frame.

### 2.5 The cost: underrun, and why `nbDmaBuffer` is tunable

The ring's weakness is the exact mirror of whole-frame's strength. Every buffer completion needs the ISR to refill *before* the DMA consumes the next one. If a higher-priority ISR (WiFi), or a flash-cache-off window, or a PSRAM stall delays it, the DMA runs off the end of valid data and **the strip glitches** — WS2812 latches on a gap of only ~150 µs.

`nbDmaBuffer` **is** the cushion: it buys `nbDmaBuffer × (one pixel's wire time)` of slack. hpwit says so directly in the shift-register driver's README — *"Sometimes interrupts can disturb the pixel buffer calculations hence making some artifacts. A solution … is to calculate several buffers in advance. By default we have 2 dma buffers. this can be increased"* — and notes the ceiling case: pushing it to `NUM_LEDS_PER_STRIP` is the full frame, "but it requires too much memory" (i.e. it degenerates back into our model). Mitigations visible in his source: a deeper ring, `_DMA_EXTENSTION` idle padding, **core pinning** (`enableShowPixelsOnCore()`), and **Level-3 interrupt priority** so the refill preempts ordinary WiFi ISRs.

This is not a niche worry — every ESP32 LED library that streams rather than whole-frames fights it, and they all expose the same remedy: a **tunable count of DMA buffers**, raised when interrupt load (typically WiFi) causes flicker. So the cushion depth is a **recognised, standard tuning parameter, not a bespoke hack** — which matters under [*Common patterns first*](../../CLAUDE.md#principles): if we build the ring, `nbDmaBuffer` is the knob users already expect.

**The trade, plainly:** our i80 driver's underrun-immunity is a real, defensible property we would be giving up on the ring path — which is exactly why the ring should be an *additional* driver, not a replacement. A 2 K-light install on a WiFi-busy board is *better served* by i80. A 16 K-light install cannot use it at all.

### 2.6 The shift-register (74HC595) driver — and why the ring is NOT its prerequisite

**In hpwit's lineage the shift-register driver does inherit the ring's PSRAM property.** It multiplies lanes by shifting each ESP32 pin's byte out through a 74HC595 (one per data pin, a 74HC245 level-shifting LATCH/CLOCK): **"8 strips out of one single pin … for 15 esp32 pins which gives you 8x15=120 strips."** That multiplication happens *downstream on the wire*; upstream it is the **same I2S ring, the same ISR-side transpose, the same caller-supplied framebuffer** (`__NB_DMA_BUFFER`, default 2, buffers of `(NUM_VIRT_PINS+1) * nb_components * 8 * 3 * 2` ≈ 1152 B).

**But that does not make the ring a prerequisite for *our* shift-register driver — the chip does.** Our backlog sets the shift-register driver's acceptance floor at **48 × 256 = 12,288 lights**. Under the whole-frame model that floor is:

| Chip | Whole-frame DMA buffer for 48 strands | Reachable today? |
|---|---|---|
| **Classic ESP32** | must be **internal** RAM (I2S DMA cannot address PSRAM) → past the ~76 KB wall | ❌ **needs the ring** |
| **ESP32-S3 / P4** | `esp_lcd` allocates it **from PSRAM** (EDMA reaches external RAM in hardware) | ✅ **no ring needed** |

So the honest conclusion — and the correction to an earlier draft of this document, which asserted a blanket prerequisite — is: **the whole-frame model cannot meet the shift-register driver's floor on the classic chip, and comfortably meets it on the S3 and P4.** The S3 i80 driver *already* drives 16,384 lights on a PSRAM DMA buffer (measured, SE16). A shift-register driver built on the **existing i80/Parlio base**, targeting **S3/P4 first**, is therefore **not memory-bound** and needs no new memory model. The classic chip's ~2,048-light whole-frame ceiling becomes a **documented limit of that chip**, not a blocker for the feature.

This also resolves — rather than reopens — the loop in our own backlog. `backlog-light.md` records that the chunk-streaming-ring design was **superseded** by the `esp_lcd` whole-frame path, on the reasoning that whole-frame is underrun-immune and reuses the shared `ParallelLedDriver` base. That decision **stands**: it bought immunity at the price of a *classic-chip* ceiling, and the chips that matter for a 12 K-light virtual install already reach PSRAM. The ring is what would lift the **classic** chip specifically — see § 5 for the trigger.

## 3. Verdict: a parked finding, not a next step

**This is a finding, not a plan. Do not build the ring driver yet.** The analysis above stands — the mechanism is real, the memory math is right, and it is genuinely new capability for a classic-ESP32 controller. But three things scope it *out* of the near-term order, and this section exists to say so plainly rather than let a research document read as an approved roadmap item.

**1. It is a whole second classic driver, and it cuts against what just shipped.** `esp_lcd` **owns the DMA** and only does whole-frame, so the ring **cannot be a flag** on `I80LedDriver` (§ 2.1). It means going under `esp_lcd` to the raw classic I2S registers — a second, register-level, ISR-driven, underrun-*prone* classic path standing next to the i80 driver we just hardened and measured across three chips. Forking the classic path on the strength of an analysis, right after proving the simpler path works, is the wrong trade of risk for capability.

**2. It is NOT a prerequisite for the shift-register driver — that was this document's own error.** An earlier draft of this section claimed the shift-register driver's 12,288-light floor forces the ring first. That is true **only on the classic ESP32**. On the **S3 and P4** the `esp_lcd`/LCD_CAM backend already draws its DMA buffer **from PSRAM** (hardware EDMA — § 3, and the measured 16,384 lights on the SE16), so a shift-register driver built on the existing i80/Parlio base is **not memory-bound there at all**. The correct scoping is therefore: build the shift-register driver **on the drivers we have, S3/P4-first**, and treat the classic chip's ~2,048-light whole-frame ceiling as **a documented limit of that chip**, not a defect blocking the feature. Step 3's shift-register-driver bullet already says "write our own against the shared slots encoder" — that instinct was right; this document was wrong to contradict it.

**3. Nothing currently planned requires it.** With the shift-register driver targeting S3/P4, no roadmap item is blocked by the classic memory model. The ring buys the classic chip a 4–8× light-count lift and a competitive win — genuinely valuable, but *optional*, and optional work does not preempt work that is on the critical path.

**The trigger that would un-park it (name it, so this isn't an indefinite maybe):** **classic-ESP32 above ~2,048 lights becomes a shipping requirement** — a board, a user, or a shift-register install that must run on classic silicon at high light counts. Until then this document is the finished homework, not a queued task: when the trigger fires, § 2's memory math, § 3's competitive position, and § 5's bench spike are ready to go without re-deriving anything.

**Related but distinct — don't conflate.** Two other levers raise the classic chip's *lane* count without touching the memory model: Step 3's **parallel-I2S driver** (hpwit `I2SClocklessLedDriver` lineage — the classic I2S peripheral can clock well past our current 8 lanes) and the **virtual/shift-register fan-out**. Those are lane-count work on the existing whole-frame model. The ring is *memory-model* work. They are independent; either can ship without the other.

### 3.1 If it is ever built: the shape, and the one thing to measure first

**It must be a second driver, alongside i80** — a scoping conclusion, not a preference (see reason 1 above). The two would coexist and the user picks:

| | `I80LedDriver` (shipped) | ring driver (parked) |
|---|---|---|
| DMA model | whole-frame, `esp_lcd` | **refill ring**, raw I2S |
| Framebuffer | internal only | **PSRAM** |
| Ceiling | ~2 K lights | not RAM-bound (wire/CPU-bound) |
| Underrun | **immune by construction** | needs `nbDmaBuffer` cushion |
| Complexity | thin, shares `ParallelLedDriver` | ISR-side transpose, register-level |

Keeping i80 is **not** legacy baggage: for a ≤2 K install on a WiFi-busy board, underrun-immunity is a real advantage the ring cannot offer. Two drivers, two honest trade-offs, user picks — and the driver docs should say exactly that, rather than presenting the ring as a strict upgrade.

**The one caution to pin before the first line of code.** The ring moves the transpose *into an ISR* and its source *into PSRAM*. Our classic encode is ~7.6 µs/light with a SWAR transpose on the main path out of internal RAM; the ring must do comparable work in interrupt context, per LED (~30 µs budget), reading cache-mediated PSRAM. **That ISR cost is the ring's true price and the thing most likely to surprise.** Budget a deep cushion from the start — `nbDmaBuffer` of 32–75 costs only ~5–11 KB (§ 2.3); the cushion is cheap and the flicker is not. Note hpwit's two enabling tricks, both of which we'd want: **Level-3 interrupt priority** (so the refill preempts WiFi) and **omitting `ESP_INTR_FLAG_IRAM`** (so the ISR may legally touch cache-mapped PSRAM).

**When the trigger fires, the first move is a bench spike — not driver code.** On the WROVER, stand up a minimal raw-I2S ring: `nbDmaBuffer` × 144 B `MALLOC_CAP_DMA` buffers, a `MALLOC_CAP_SPIRAM` source array, an ISR that transposes one pixel row per completion, driving **8 × 1024 = 8192 lights** — 4× past our measured wall — on the existing bench pins. That single spike answers all three open questions at once:

1. **Does internal RAM stay flat?** (It should read ~1.6–11 KB regardless of light count — the claim of § 2.3.)
2. **What does the ISR transpose actually cost per row out of PSRAM?** (The § 2.4 unknown.)
3. **What `nbDmaBuffer` depth holds a clean strip with WiFi associated and hammering?** (The § 2.5 trade, measured rather than assumed.)

**If that spike lights 8192 lights cleanly, the design is proven and the rest is integration** behind the existing `ParallelLedDriver` seam. If it flickers at every cushion depth, we learn it on a bench rig instead of inside a driver refactor. Either way the spike, not the argument, settles it — and it also converts § 2.2's one honest inference (PSRAM framebuffer on *classic*, which hpwit documents only for the S3) into a measured fact on our own hardware.

## 4. Sources

**hpwit (primary — source read line-by-line):**
- [I2SClocklessLedDriver](https://github.com/hpwit/I2SClocklessLedDriver) · [`src/I2SClocklessLedDriver.h`](https://github.com/hpwit/I2SClocklessLedDriver/blob/main/src/I2SClocklessLedDriver.h) — `lldesc_t` ring; `nbDmaBuffer = 6` default, runtime-tunable via `updateDriver()`; `initDMABuffers()` → `nbDmaBuffer + 2` buffers of `nbComponents * 8 * 2 * 3` B (144 RGB / 192 RGBW) + 4× terminator; `IRAM_ATTR interruptHandler()` → `loadAndTranspose()` reading `driver->leds`; ring buffers `heap_caps_malloc(..., MALLOC_CAP_DMA)`; `initled(uint8_t* leds, ...)` caller-supplied framebuffer; `esp_intr_alloc(..., ESP_INTR_FLAG_INTRDISABLED | ESP_INTR_FLAG_LEVEL3, ...)` with `ESP_INTR_FLAG_IRAM` "removed to avoid Cache Disabled but Cached Memory Region Accessed"; optional `FULL_DMA_BUFFER` whole-frame mode.
- [issue #11 — "DMA mode takes up 60k of ram just for a single 300 led strip"](https://github.com/hpwit/I2SClocklessLedDriver/issues/11) — the whole-frame mode's scaling complaint.
- [PR #47 — `__NB_DMA_BUFFER` as var & PSRAM-prefer](https://github.com/hpwit/I2SClocklessLedDriver/pull/47).
- [I2SClocklessVirtualLedDriver](https://github.com/hpwit/I2SClocklessVirtualLedDriver) — "8 strips out of one single pin … 8x15=120 strips"; 74HC595 per virtual pin + 74HC245 level shifter; `__NB_DMA_BUFFER` default 2, buffers `(NUM_VIRT_PINS+1) * nb_components * 8 * 3 * 2` ≈ 1152 B; "Artifacts due to interrupts" → "calculate several buffers in advance"; `enableShowPixelsOnCore()`; 12,000-LED worked example; 75 → 129 fps second-core figures.
- [I2SClockLessLedDriveresp32s3](https://github.com/hpwit/I2SClockLessLedDriveresp32s3) — the S3 lineage (PSRAM buffers; S3 EDMA can DMA from PSRAM).

- [MoonLight](https://moonmodules.org/projectMM/) / [WLED-MM](https://github.com/MoonModules/WLED-MM) — a prior firmware riding hpwit's ring on PSRAM: LEDs pre-allocated in PSRAM, light boundary **130 K** (vs **4096** non-PSRAM). The empirical demonstration that the ring's ceiling lift is real.

**Espressif (the load-bearing hardware constraint):**
- [`esp_lcd/i80/esp_lcd_panel_io_i2s.c`](https://github.com/espressif/esp-idf/blob/master/components/esp_lcd/i80/esp_lcd_panel_io_i2s.c) — `ESP_RETURN_ON_FALSE((caps & MALLOC_CAP_SPIRAM) == 0, NULL, TAG, "external memory is not supported");`
- [Support for External RAM — ESP-IDF (ESP32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/external-ram.html) — external RAM "cannot be used as a place to store DMA transaction descriptors or as a buffer for a DMA transfer to read from or write into"; prescribed workaround = internal DMA-able buffer + copy. **Confirms: no IDF API feeds PSRAM into classic-ESP32 I2S DMA.**
- [Support for External RAM — ESP-IDF (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/external-ram.html) — the S3 *does* have hardware DMA-to-PSRAM (descriptors still internal): why our S3 i80 reaches 16 K and classic cannot.

**MoonLight's own measurements:**
- [performance.md § Multi-pin LED driving](../../reference/performance.md#multi-pin-led-driving-all-three-peripherals-128128-grid) — classic i80 2048-light ceiling + `esp_lcd_i80_alloc_draw_buffer` rejecting `MALLOC_CAP_SPIRAM`; S3 16,384 @ ~34 fps; P4 Parlio 4096, 139 fps @ 1024; the `multicore` +44 % table.
- [backlog-light.md](backlog-light.md) — the superseded chunk-streaming-ring decision; the shift-register driver's **48 × 256 = 12,288** acceptance floor.
