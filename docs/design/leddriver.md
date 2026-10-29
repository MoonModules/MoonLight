# LED drivers: the designs ahead

What shipped and where it lives: [Choosing and wiring an LED driver](../reference/hardware/led-drivers.md) for the drivers and their limits, [MoonLight, drivers](../explanation/architecture/moonlight.md#drivers) for the pipeline, [What bounds a parallel driver](../explanation/architecture/moonlight.md#what-bounds-a-parallel-driver) and [performance.md](../reference/performance.md#multi-pin-led-driving-all-three-peripherals-128128-grid) for the measurements, and the headers under `src/light/drivers/` and `src/platform/esp32/` for each peripheral's design. This file holds the research behind four things that are not built, each with its backlog item.

## LED output from a Raspberry Pi's own pins

Backlog: [LED output from a Raspberry Pi's own pins](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086230). The prerequisite is a `src/platform/rpi/` backend; a Pi runs the desktop build and reaches LEDs through the network drivers.

**Pi 4 and earlier (BCM2711 and before).** rpi_ws281x (BSD-2, <https://github.com/jgarff/rpi_ws281x>) drives the header by DMA through PWM (2 channels), PCM (1) or SPI (1), up to about 2,700 lights a string on PWM and 5,400 on PCM. It is the reference for that route; its last release is v1.0.0 of March 2023.

**Pi 5 (BCM2712 with the RP1 southbridge).** GPIO, PWM, DMA, SPI and I2C sit behind RP1, so every driver that mapped BCM283x registers from userspace stops working (rpi_ws281x issues #528, #554, #555, #564). Three routes exist:

- **SPI MOSI**, one lane per bus, each WS2812 bit encoded in SPI bits at 2.4 MHz: the rpi5-ws2812 package (<https://github.com/niklasr22/rpi5-ws2812>). The one path every Pi shares, and the one to start with.
- **PWM through a kernel module**, rpi_ws281x's Pi 5 branch (<https://github.com/jgarff/rpi_ws281x/wiki/Raspberry-Pi-5-Support>): `rp1_ws281x_pwm.ko` plus a device-tree overlay, one channel working, concurrent channels not.
- **RP1's PIO**, the forward path. RP1 carries one PIO block of the RP2040's design, 4 state machines and 32 instruction slots. Its registers are not reachable over the PCIe link, only the state machines' FIFOs through DMA, so userspace goes through the kernel's `rp1-pio` driver, which piolib wraps (<https://www.raspberrypi.com/news/piolib-a-userspace-library-for-pio-control/>, BSD-3). WS2812 is piolib's own example (<https://github.com/raspberrypi/utils/blob/master/piolib/examples/ws2812.c>), one lane. Parallel lanes would feed pre-encoded slots through a FIFO by DMA, the shape `GpioDmaPeripheral` uses on the Teensy, and a '595 expander would hang off the same slots; nobody has shipped either on RP1.

**HUB75 on a Pi.** On a Pi 1 to 4, hzeller's rpi-rgb-led-matrix (<https://github.com/hzeller/rpi-rgb-led-matrix>) writes the GPIO registers directly, and its options catalog the panel variety a backend has to cover: multiplexing schemes, row-address types, driver-chip init sequences such as FM6126A, chained and parallel panels, PWM depth, a pixel mapper. On a Pi 5, Adafruit's PioMatter (<https://docs.circuitpython.org/projects/piomatter/en/latest/adafruit_raspberry_pi5_piomatter.html>) drives up to 3 panels through the PIO, alpha. Both are study material: MoonLight writes its own from the chip datasheets, with credit.

**Not a route.** WiringPi (a community fork since its author stopped) and libgpiod make a call per pin change, far too slow and jittery for WS2812's 150 ns edges or a HUB75 refresh; they serve buttons and relays.

## The classic ESP32 past 2,048 lights

Backlog: [Classic-ESP32 shift-register ring on raw I2S](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287080365).

### Verdict

**Yes: PSRAM can lift the classic-ESP32 ceiling, and the mechanism is a memory-model change, not a shift-register feature.** Our 2048-light wall is a direct consequence of the *whole-frame* DMA model: `esp_lcd`'s i80 backend materializes every light's WS2812 waveform in one contiguous DMA-capable block before the transfer starts, and on the classic ESP32 that block **cannot be PSRAM**: the IDF source rejects it outright (`esp_lcd_panel_io_i2s.c`: `ESP_RETURN_ON_FALSE((caps & MALLOC_CAP_SPIRAM) == 0, NULL, TAG, "external memory is not supported")`), because the classic chip's I2S DMA physically cannot address the external-RAM window. So the DMA buffer scales linearly with the light count against a ~76 KB internal wall.

The ring shipped as the `MoonI80` peripheral beside `i80`: [Choosing and wiring an LED driver](../reference/hardware/led-drivers.md#which-one).

**A scoping fact that shapes the work: the ring is not available off the shelf.** This is not a matter of adopting a library. The mainstream ESP32 LED libraries use the same **whole-frame DMA** we do and therefore share our internal-RAM ceiling; the refill-ring model is effectively unique to hpwit's drivers, which is why § 2 studies **his source** rather than surveying alternatives. Two consequences: (1) building it is genuinely new capability for a classic-ESP32 LED controller, not catching up; (2) per [*Industry standards, our own code*](../CLAUDE.md#principles), we study his implementation hard and then **write our own against our architecture**: carry the idea forward, not the code.

### The PSRAM question

#### Two memory models

**Whole-frame (MoonLight, via `esp_lcd` i80).** The DMA buffer *is* the frame: every light's expanded WS2812 waveform, 3 slots per bit, one byte (8-lane) or two (16-lane) per slot, materialized in one contiguous DMA-capable block, handed to the peripheral in a single chained transfer. The CPU is out of the loop for the whole frame, which is why this model is **underrun-immune by construction**: nothing remains to refill, so no ISR storm can starve it. The price is linear scaling:

```text
internalBytes(whole-frame) = maxLaneLights × channels × 3 slots × slotBytes
```

**Refill ring (hpwit, under the raw I2S registers).** The DMA buffer holds only a small rolling window: a ring of `nbDmaBuffer` buffers, each **one pixel-row of slots** wide. On each buffer-EOF the I2S ISR calls `loadAndTranspose()`, which reads the next pixel row from the source array and SWAR-transposes it into the buffer the DMA is about to reach. Internal RAM:

```text
internalBytes(ring) = (nbDmaBuffer + 2) × (nbComponents × 8 bits × 3 slots × 2 bytes/word)
                    = (nbDmaBuffer + 2) × 144 B   [RGB]
```

**The formula has no LED-count term.** That is the decoupling, and it is the entire point.

**Crucially, "ring buffer" names two different things and only one solves the ceiling.** The distinction is the whole decision, and it is easy to lose:

| | **(A) Refill ring** | **(B) Ping-pong / double buffer** |
|---|---|---|
| What's in a buffer | one pixel row of slots | a **whole frame** |
| Who fills it, when | the **DMA-completion ISR**, mid-transfer | the CPU, **before** the transfer |
| Source array touched by | **CPU only** → **PSRAM-eligible** | DMA → must be internal |
| Internal RAM vs LED count | **constant** | **scales linearly** |
| Buys you | **lifts the memory ceiling** | hides wire time behind DMA |
| In our tree | *(this proposal)* | **`asyncTransmit`** (already shipped) |

We already have (B). It is genuinely valuable, it is what took the S3 from ~13 to ~34 fps at 16 K, but **it does not and cannot lift the memory ceiling**, which is exactly why halving the DMA buffer by turning `asyncTransmit` off didn't help the WROVER.

#### What hpwit's source says

Read directly from [`src/I2SClocklessLedDriver.h`](https://github.com/hpwit/I2SClocklessLedDriver/blob/main/src/I2SClocklessLedDriver.h) / `.cpp` (branch `main`):

- **A ring of linked `lldesc_t` descriptors.** `struct I2SClocklessLedDriverDMABuffer { lldesc_t descriptor; uint8_t* buffer; };`. `initDMABuffers()` allocates `nbDmaBuffer + 2` buffers of `nbComponents * 8 * 2 * 3` bytes each (**144 B RGB / 192 B RGBW**) plus one 4×-size terminator. Chained `descriptor.qe.stqe_next = &next->descriptor`, last wrapping to `[0]`: a genuine ring.
- **`uint8_t nbDmaBuffer = 6;`**: default 6, and **runtime-tunable** via `updateDriver(..., uint8_t dmaBuffer, ...)` → `nbDmaBuffer = dmaBuffer;`. (This is why the product owner's StarLight-era setting of **75** was reachable: it's a plain count, not a compile-time constant.)
- **The ISR refills and transposes.** `static void IRAM_ATTR interruptHandler(void*)`: on `I2S_OUT_EOF_INT` it increments `ledToDisplay` and calls `loadAndTranspose()`, which reads `driver->leds + ledToDisplay * nbComponents` per strip, applies brightness/gamma maps, and SWAR-transposes (`transpose16x1Noinline2`) into `dmaBuffersTampon[dmaBufferActive]->buffer`. **One buffer = one LED slot, so the ISR fires per LED (~30 µs at 800 kHz).**
- **Ring buffers are internal; the framebuffer is the caller's.** Ring: `heap_caps_malloc(bytes, MALLOC_CAP_DMA)`. Framebuffer: `initled(uint8_t* leds, ...)` takes a raw pointer the driver only ever reads **with the CPU**. Nothing DMA-touches it → **a PSRAM pointer works by construction.**
- **The ISR is deliberately arranged to be allowed to read PSRAM.** `esp_intr_alloc(interruptSource, ESP_INTR_FLAG_INTRDISABLED | ESP_INTR_FLAG_LEVEL3, ...)` with the in-code comment that `ESP_INTR_FLAG_IRAM` was *"removed to avoid Cache Disabled but Cached Memory Region Accessed"*, i.e. the refill ISR is explicitly permitted to touch cache-mapped (PSRAM) memory. That is the design decision that makes the PSRAM framebuffer legal, stated in hpwit's own source.
- **The whole-frame mode exists too, and is the one that scales.** `FULL_DMA_BUFFER` allocates `numLedPerStrip + 2` buffers, and is precisely the mode behind [issue #11, DMA mode taking 60 KB of RAM for a single 300-LED strip](https://github.com/hpwit/I2SClocklessLedDriver/issues/11). **hpwit's library contains both models, and the scaling complaint lands on the whole-frame one, the same wall we hit.**

**Honest caveat:** hpwit never writes "put `leds[]` in PSRAM" in so many words *for the classic chip* (the explicit PSRAM-framebuffer statements are in the S3 variants). The classic conclusion is drawn from construction, CPU-only reads from a caller-supplied pointer, in a non-IRAM Level-3 ISR, which is sound but is **inference from source, not a vendor promise.** The bench spike in § 5 is what turns it into a measured fact.

#### The memory math, concretely (8 lanes, RGB)

Whole-frame, `slotBytes = 1` (8-lane bus), 3 channels, 3 slots/bit, our i80 model:

| Lights/lane | Total (8 lanes) | Whole-frame DMA buffer | Fits ~76 KB internal? |
|---|---:|---:|---|
| 256 | 2,048 | ~18 KB | ✅ **measured working** |
| 512 | 4,096 | ~37 KB | ❌ **measured fail** (`i80 bus init failed`): the real wall sits below the arithmetic: ~76 KB is the *largest free block*, and `asyncTransmit` wants two, plus IDF descriptor overhead |
| 1024 | 8,192 | ~74 KB | ❌ |
| 2048 | 16,384 | ~147 KB | ❌ |

Ring, same 8 lanes, RGB (hpwit's own sizing, 144 B/buffer):

| `nbDmaBuffer` | Ring internal RAM | Lights its memory allows |
|---|---:|---|
| 6 (default) | (6+2) × 144 = **1,152 B** | **any**: 2 K, 16 K, 130 K; the count does not appear in the formula, so wire time and ISR headroom bind first |
| 32 | (32+2) × 144 ≈ **4.9 KB** | **any** |
| 75 (StarLight-era) | (75+2) × 144 ≈ **11 KB** | **any** |

**Read those against each other.** The whole-frame model needs 147 KB of a ~76 KB budget to reach 16 K lights, so it cannot. The ring reaches 16 K, or 130 K, on **1.6–11 KB**, and even a 75-buffer cushion costs ~11 KB. The frame itself moves to PSRAM, where 16 K lights × 3 B = 48 KB is a rounding error against 4 MB.

#### What the ring gives the WROVER, and what binds next

With the internal-RAM wall gone, the binding constraints become, in order:

1. **Wire time, the hard physical floor.** WS2812 is ~30 µs per light per lane (24 bits × 1.25 µs). At 8 lanes × 2048/lane = 16,384 lights: 2048 × 30 µs = **~61 ms → ~16 fps**, before any CPU cost. A protocol constant no driver escapes; it is why "more LEDs" past a point is a *distribution* problem.
2. **Encode CPU in interrupt context, the ring's true price.** Our whole-frame model transposes once with a SWAR kernel on the main path out of internal RAM (measured **~7.6 µs/light** on classic). The ring does comparable work **inside an ISR, per LED, reading cache-mediated PSRAM**. hpwit fires that ISR once per LED (~30 µs apart): so the refill must reliably complete within the wire time of one pixel. This is the number to measure first.
3. **PSRAM bandwidth / cache stalls**, the same story from the other side: an ISR that stalls on a cache miss is an ISR that is late, which is item 4.
4. **The underrun cushion** (§ 2.5).

**Honest bottom line for a 4 MB-PSRAM WROVER with a ring driver:** the **memory ceiling disappears** (the frame is kilobytes against megabytes) and the practical ceiling becomes **wire time and ISR headroom**: comfortably into the **8–16 K** range at single-digit-to-low-teens fps. That is a **4–8× lift over our measured 2048**, and the right framing is *"the ceiling stops being about RAM."* **This is not theory:** a prior firmware built on hpwit's ring (the product owner's own MoonLight / WLED-MM lineage) raises its light boundary to **130 K on PSRAM against 4096 without it**: a ~32× gap between the PSRAM and non-PSRAM configuration of the *same* firmware, which is the empirical shape of ring-vs-whole-frame.

#### The cost: underrun, and why `nbDmaBuffer` is tunable

The ring's weakness is the exact mirror of whole-frame's strength. Every buffer completion needs the ISR to refill *before* the DMA consumes the next one. If a higher-priority ISR (WiFi), or a flash-cache-off window, or a PSRAM stall delays it, the DMA runs off the end of valid data and **the strip glitches**: WS2812 latches on a gap of only ~150 µs.

`nbDmaBuffer` **is** the cushion: it buys `nbDmaBuffer × (one pixel's wire time)` of slack. hpwit says so directly in the shift-register driver's README, *"Sometimes interrupts can disturb the pixel buffer calculations hence making some artifacts. A solution … is to calculate several buffers in advance. By default we have 2 dma buffers. this can be increased"*, and notes the ceiling case: pushing it to `NUM_LEDS_PER_STRIP` is the full frame, "but it requires too much memory" (i.e. it degenerates back into our model). Mitigations visible in his source: a deeper ring, `_DMA_EXTENSTION` idle padding, **core pinning** (`enableShowPixelsOnCore()`), and **Level-3 interrupt priority** so the refill preempts ordinary WiFi ISRs.

This is not a niche worry: every ESP32 LED library that streams rather than whole-frames fights it, and they all expose the same remedy: a **tunable count of DMA buffers**, raised when interrupt load (typically WiFi) causes flicker. So the cushion depth is a **recognized, standard tuning parameter, not a bespoke hack**: which matters under [*Common patterns first*](../CLAUDE.md#principles): if we build the ring, `nbDmaBuffer` is the knob users already expect.

### Sources

**hpwit (primary, source read line-by-line):**
- [I2SClocklessLedDriver](https://github.com/hpwit/I2SClocklessLedDriver) · [`src/I2SClocklessLedDriver.h`](https://github.com/hpwit/I2SClocklessLedDriver/blob/main/src/I2SClocklessLedDriver.h): `lldesc_t` ring; `nbDmaBuffer = 6` default, runtime-tunable via `updateDriver()`; `initDMABuffers()` → `nbDmaBuffer + 2` buffers of `nbComponents * 8 * 2 * 3` B (144 RGB / 192 RGBW) + 4× terminator; `IRAM_ATTR interruptHandler()` → `loadAndTranspose()` reading `driver->leds`; ring buffers `heap_caps_malloc(..., MALLOC_CAP_DMA)`; `initled(uint8_t* leds, ...)` caller-supplied framebuffer; `esp_intr_alloc(..., ESP_INTR_FLAG_INTRDISABLED | ESP_INTR_FLAG_LEVEL3, ...)` with `ESP_INTR_FLAG_IRAM` "removed to avoid Cache Disabled but Cached Memory Region Accessed"; optional `FULL_DMA_BUFFER` whole-frame mode.
- [issue #11, DMA mode taking 60 KB of RAM for a single 300-LED strip](https://github.com/hpwit/I2SClocklessLedDriver/issues/11): the whole-frame mode's scaling complaint.
- [PR #47, `__NB_DMA_BUFFER` as var & PSRAM-prefer](https://github.com/hpwit/I2SClocklessLedDriver/pull/47).
- [I2SClocklessVirtualLedDriver](https://github.com/hpwit/I2SClocklessVirtualLedDriver): "8 strips out of one single pin … 8x15=120 strips"; 74HC595 per virtual pin + 74HC245 level shifter; `__NB_DMA_BUFFER` default 2, buffers `(NUM_VIRT_PINS+1) * nb_components * 8 * 3 * 2` ≈ 1152 B; "Artifacts due to interrupts" → "calculate several buffers in advance"; `enableShowPixelsOnCore()`; 12,000-LED worked example; 75 → 129 fps second-core figures.
- [I2SClockLessLedDriveresp32s3](https://github.com/hpwit/I2SClockLessLedDriveresp32s3): the S3 lineage (PSRAM buffers; S3 EDMA can DMA from PSRAM).

- [The predecessor MoonLight](https://github.com/ewowi/MoonLight) / [WLED-MM](https://github.com/MoonModules/WLED-MM): a prior firmware riding hpwit's ring on PSRAM: LEDs pre-allocated in PSRAM, light boundary **130 K** (vs **4096** non-PSRAM). The empirical demonstration that the ring's ceiling lift is real.

**Espressif (the load-bearing hardware constraint):**
- [`esp_lcd/i80/esp_lcd_panel_io_i2s.c`](https://github.com/espressif/esp-idf/blob/master/components/esp_lcd/i80/esp_lcd_panel_io_i2s.c): `ESP_RETURN_ON_FALSE((caps & MALLOC_CAP_SPIRAM) == 0, NULL, TAG, "external memory is not supported");`
- [Support for External RAM, ESP-IDF (ESP32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/external-ram.html): external RAM "cannot be used as a place to store DMA transaction descriptors or as a buffer for a DMA transfer to read from or write into"; prescribed workaround = internal DMA-able buffer + copy. **Confirms: no IDF API feeds PSRAM into classic-ESP32 I2S DMA.**
- [Support for External RAM, ESP-IDF (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/external-ram.html): the S3 *does* have hardware DMA-to-PSRAM (descriptors still internal): why our S3 i80 reaches 16 K and classic cannot.

**MoonLight's own measurements:**
- [performance.md § Multi-pin LED driving](../reference/performance.md#multi-pin-led-driving-all-three-peripherals-128128-grid): classic i80 2048-light ceiling + `esp_lcd_i80_alloc_draw_buffer` rejecting `MALLOC_CAP_SPIRAM`; S3 16,384 @ ~34 fps; P4 Parlio 4096, 139 fps @ 1024; the `multicore` +44 % table.
- [backlog/drivers.md](https://github.com/users/MoonModules/projects/5?filterQuery=area%3ADrivers): the superseded chunk-streaming-ring decision; the shift-register driver's **48 × 256 = 12,288** acceptance floor.

## The pin expander on the other peripherals

The 74HCT595 expander ships on `i80` and `MoonI80`, the LCD_CAM peripheral of the S3, P4 and S31 ([Wiring a 74HCT595 pin expander](../reference/hardware/led-drivers.md#wiring-a-74hct595-pin-expander)). What it needs from a peripheral: a shift clock on a pin, the latch as one bit of every bus word, and eight bus words per slot, so the DMA frame grows eightfold. RMT has no clock line and never qualifies. The other two parallel peripherals could:

- **Parlio (P4).** `ParlioPeripheral::supportsPinExpander()` is false. The transmit unit generates its pixel clock itself, and `platform_esp32_parlio.cpp` leaves `clk_out_gpio_num` unconnected because WS2812 ignores it; connected, it is the '595's shift clock. Whether Parlio's clock output and bit packing carry the expander's protocol is a bench question, and the eightfold frame meets the single-transaction ceiling the [chunked transfer](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287080935) lifts. Backlog: [A '595 expander on Parlio](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287079383).
- **GPIO-DMA (Teensy 4.1).** `GpioDmaPeripheral::supportsPinExpander()` is false. The port write carries no clock of its own, so the shift clock has to come from the writes themselves, at a DMA rate still to measure. FlexIO clocks by itself, and [the Teensy plan](../work/Plan-20261010%20-%20Teensy%204.1%20(3%20of%206).md) set it aside because no 8 contiguous FlexIO pins stay free beside the W5500. TriantaduoWS2811 (<https://github.com/wramsdell/TriantaduoWS2811>) is the prior art: 32 strands from 3 Teensy pins through FlexIO clocking '595s at zero CPU cost. Backlog: [A '595 expander on the Teensy's GPIO-DMA bus](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287078012).
- **RP1 PIO (Pi 5).** Research grade, above.

The expander multiplies slots, not pins, so each peripheral's frame or ring grows eightfold too: the memory question arrives with the clock question.

## The sigrok cross-check

Backlog: [sigrok/fx2lafw cross-check + MoonDeck LED driver test script](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287130440).

The loopback test trusts the chip's own clock, and on an ESP32 it runs on the chip whose WiFi causes the flicker it looks for. An analyzer with its own clock crosses it: where the two disagree there is a real bug. The reference device is a NanoDLA v1.3 (CY7C68013A, fx2lafw firmware, 24 MS/s on 8 channels, 10 to 15 dollars); any board sigrok's `fx2lafw` driver enumerates works the same. 24 MS/s is 41 ns a sample, enough to tell a 350 ns pulse from a 700 ns one and marginal for a timing-margin test (320 against 380 ns). A DSLogic Plus (16 channels at 100 MS/s) is the upgrade when a flicker reproduces that 24 MS/s cannot resolve, and the scripts carry over unchanged.

Wiring: the analyzer's GND to a board GND over the shortest jumper, and channel D0 (labeled CH0) to the data GPIO, beside the loopback jumper. The analyzer is high impedance and loads nothing; 3.3 V GPIO into a 3.3 V, 5 V tolerant input needs no shifter or resistor. Never power the board from the analyzer's rail, and give a real strand its own 5 V with its ground on the same common ground.

```bash
sigrok-cli --scan                      # one fx2lafw entry
sigrok-cli -d fx2lafw -c samplerate=24m --samples 100000 \
           -P ws2812:data=D0 -A ws2812=rgb
```

The decoder (libsigrokdecode's `ws2812`) prints one line per pixel in transmit order, to compare against the bytes the firmware sent. At 24 MS/s, 100,000 samples is about 4 ms, a 30-light frame with its latch; a longer strand raises `--samples` or triggers on the first edge. Gotchas in the order they come: no common ground reads as random toggling; the wrong channel; a sample rate left at the driver's lower default, where a 350 ns pulse is three samples and zeros read as ones (always pass `samplerate=24m`); and when no frame decodes, PulseView on the same capture shows the missing idle gap or the stuck bit by eye.

The real flicker test is a sustained capture of seconds with WiFi associated and a packet flood, every frame decoded for a slipped byte or a short latch gap.
