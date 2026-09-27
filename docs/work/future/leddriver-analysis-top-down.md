# LED driver — top-down analysis

A second-attempt design study, deliberately inverted: instead of cataloguing the LED-driver libraries that exist and choosing among them, start from the end goal — *driving WS2812-class LEDs from a GPIO pin* — and reason about what each supported MCU can actually do, how to defend the timing against the rest of the system (WiFi, the OS, the cache), and how to *prove* the output is correct without trusting the code.

The output of this study is the next-iteration plan: a generic driver architecture, the per-platform implementation that wires into it, the testing strategy that verifies it, and a "hello world" path that starts with WS2812 on ESP32 classic and walks out to Teensy 4.1 and Raspberry Pi 5.

The earlier study at [leddriver-analysis-bottom-up.md](leddriver-analysis-bottom-up.md) is intentionally not consulted while writing this one; a short reconciliation section at the bottom compares the two after the fact.

## TL;DR

- WS2812 is a strict-timing 1-wire NRZ protocol at 800 kHz. Every working driver does the same thing at the bottom: a DMA engine clocks out a pre-encoded bit pattern with sub-150 ns edge accuracy, while the CPU stays out of the timing loop.
- The peripheral that hosts that DMA engine is what changes per device: RMT on ESP32, I2S/LCD_CAM/PARLIO on the newer ESPs, FlexIO on Teensy 4.x, PIO (via RP1) on Raspberry Pi 5.
- The WiFi-glitch problem is, almost without exception, an ISR-latency problem: a refill ISR that misses a deadline by &gt; ~5 µs corrupts one bit and shifts every downstream pixel. The standard fix is to take the CPU out of the refill entirely (DMA-only ping-pong) and pin everything else to the other core.
- Architecture: one tiny abstract `LedDriver` interface; per-platform implementations live in `src/platform/<target>/` and are picked by `if constexpr (platform::…)` switches at compile time. No `#ifdef` outside `src/platform/`.
- Testing: on-board GPIO loopback is the cheap CI workhorse (RMT-RX on ESP32, FlexPWM-capture on Teensy). A $12 fx2lafw clone (the project uses a NanoDLA v1.3) + sigrok-cli adds an independent-clock cross-check. A $149 DSLogic Plus is the right "later" upgrade when timing-margin tests start to matter.
- Hello-world order: ESP32 classic → ESP32-S3 → ESP32-P4 → Teensy 4.x → Raspberry Pi 5 (via PIO/RP1 — and seriously consider just bridging to an MCU on the Pi 5).

## 1. The protocol, in detail

WS2812 (and the SK6812 RGBW variant) is a 1-wire NRZ protocol clocked at 800 kHz: every bit is a 1.25 µs cell with a high-then-low pulse whose duty cycle encodes 0 or 1. There is no clock line. Receivers latch on idle-low &gt; 50 µs (original WS2812) or &gt; 280 µs (current WS2812B silicon).

### 1.1 Datasheet numbers and real-silicon tolerances

| Chip       | T0H    | T0L    | T1H    | T1L    | Period  | Reset (current silicon) |
| ---------- | ------ | ------ | ------ | ------ | ------- | ----------------------- |
| WS2812B    | 0.40 µs | 0.85 µs | 0.80 µs | 0.45 µs | 1.25 µs | &gt; 280 µs                  |
| WS2812 (orig.) | 0.35 µs | 0.80 µs | 0.70 µs | 0.60 µs | 1.25 µs | &gt; 50 µs                   |
| SK6812 RGB | 0.30 µs | 0.90 µs | 0.60 µs | 0.60 µs | 1.20 µs | &gt; 80 µs                   |
| SK6812 RGBW | 0.30 µs | 0.90 µs | 0.60 µs | 0.60 µs | 1.20 µs | &gt; 80 µs                   |

Sources: [WS2812B datasheet](https://cdn-shop.adafruit.com/datasheets/WS2812B.pdf), [WS2812 datasheet](https://cdn-shop.adafruit.com/datasheets/WS2812.pdf), [SK6812 datasheet](https://cdn-shop.adafruit.com/product-files/1138/SK6812+LED+datasheet+.pdf), [SK6812 RGBW datasheet](https://cdn-shop.adafruit.com/product-files/2757/p2757_SK6812RGBW_REV01.pdf).

Tim "cpldcpu" Böscke's [reverse engineering of the real WS2812 silicon](https://cpldcpu.com/2014/01/14/light_ws2812-library-v2-0-part-i-understanding-the-ws2812/) shows that the actual decode threshold sits around 600 ns of high time — far more forgiving than the datasheet implies. That is why every library you can name uses slightly different numbers and they all work. The numbers *we* use should be:

- **Bit timing**: target T0H = 350 ns, T1H = 700 ns, period = 1250 ns, with a ±150 ns budget. These satisfy WS2812, WS2812B, and SK6812 simultaneously.
- **Reset / latch**: ≥ 300 µs idle low. This covers WS2812B current silicon, WS2813, and SK6812. The 50 µs value from the older datasheet is dead — [WorldSemi silently changed it ca. 2017–2018](https://tomverbeure.github.io/2019/10/13/WS2812B_Reset_Old_and_New.html) and code that assumed 50 µs randomly fails on new strips.

### 1.2 Bit order, byte order, frame order

- **Pixel order**: GRB (G7..G0, R7..R0, B7..B0), MSB first within each byte. SK6812 RGBW: GRBW, 32 bits, MSB first. Every other chip-specific quirk (BGR, RBG) is a transposition the driver applies on the *logical* color before encoding.
- **Frame order**: the first 24/32 bits go to the *first* pixel (the one closest to the controller). Subsequent pixels read the data after a per-pixel shift register inside the previous chip — there is no addressing.

### 1.3 Refresh-rate math

Per-pixel time = bits/pixel × 1.25 µs. Frame time = N × per-pixel + reset.

| N pixels (WS2812B) | Frame (µs)      | Max FPS |
| ------------------ | --------------- | ------- |
| 100                | 3000 + 300 = 3300 | 303     |
| 300                | 9000 + 300 = 9300 | 107     |
| 1000               | 30000 + 300 = 30300 | 33     |
| 4096               | 122880 + 300     | 8.1    |

Driving *K* strands in parallel from one peripheral does not multiply wall-clock time — the per-frame ceiling is set by the longest strand. The DMA *buffer* scales with K. SK6812 RGBW costs ~33% more time per frame than WS2812 at equal pixel counts because of the extra white byte.

This refresh ceiling is a property of the LED protocol, not of any MCU. No driver, however clever, breaks 33 FPS at 1000 pixels per strand — that is *the* number that drives every parallel-strand design decision below.

## 2. Per-platform peripheral inventory

### 2.1 ESP32 classic (Xtensa LX6, dual-core, with WiFi/BT)

| Peripheral             | Status                                                                                                                                                                                                                                                                                                                       |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| RMT v1 / v2            | 8 channels, 64 symbols per memory block, no DMA. Each channel timing-independent. The historical FastLED `clockless_rmt_esp32` driver (Sam Guyer's) does manual ping-pong refill at the half-buffer interrupt — that explicit double-buffering is what makes RMT v1 *more* WiFi-resilient than v2 ([FastLED #2082](https://github.com/FastLED/FastLED/issues/2082)). |
| I2S0 in LCD mode       | Up to 16 parallel chains via the LCD sub-mode of I2S0; this is what hpwit's [I2SClocklessLedDriver](https://github.com/hpwit/I2SClocklessLedDriver) uses. DMA-driven, ping-pong buffers, CPU out of the loop. The hard cost is WiFi coexistence — both peripherals contend for the SPI/DMA bus arbiter. Mitigations covered below. |
| SPI                    | Right peripheral for APA102 / SK9822 (clocked, two-wire). Not for WS2812 unless you tolerate 4× bit-stuffing (NeoPixelBus does this as a fallback).                                                                                                                                                                          |
| LCD_CAM, PARLIO        | Absent.                                                                                                                                                                                                                                                                                                                      |
| MCPWM, PCNT            | Never used in production for LEDs. Ignore.                                                                                                                                                                                                                                                                                  |

**Recommendation**: RMT (v1 API where possible) for 1–8 chains, mixed chipsets, with WiFi on. I2S parallel for 9–16 uniform-timing chains when you can tame WiFi.

### 2.2 ESP32-S3 (Xtensa LX7, vector instructions, USB-OTG)

| Peripheral | Status                                                                                                                                                                                                                                                                                                                |
| ---------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| RMT v2     | 4 TX + 4 RX channels, 48 symbols/block, **DMA optional** (`with_dma = true`). On a typical board with USB-CDC console, you have room for one DMA channel plus one non-DMA channel.                                                                                                                                  |
| I2S        | Still works for parallel LED output (16 lanes), but the S3's I2S no longer contains the LCD sub-module — that's been promoted to its own peripheral. The legacy I2S-parallel drivers still compile and run.                                                                                                          |
| LCD_CAM    | **Present, and this is the right peripheral for parallel LEDs on S3.** Originally a HUB75 / 8-bit-parallel-display peripheral; hpwit and Adafruit (Phil Burgess) [discovered](https://blog.adafruit.com/2022/06/14/esp32uesday-hacking-the-esp32-s3-lcd-peripheral/) it can be repurposed for 8- or 16-lane WS2812 output. It has its own GDMA channel and does not share the legacy DMA arbiter with WiFi. |
| SPI        | Same role as classic ESP32 — APA102 / SK9822.                                                                                                                                                                                                                                                                          |

**Origin of the "LCD_CAM discovery" the brief refers to**: the Adafruit "ESP32uesday" posts in June 2022 ([1](https://blog.adafruit.com/2022/06/14/esp32uesday-hacking-the-esp32-s3-lcd-peripheral/), [2](https://blog.adafruit.com/2022/06/21/esp32uesday-more-s3-lcd-peripheral-hacking-with-code/)) introduced the technique for HUB75 RGB matrices. hpwit ([I2SClockLessLedDriveresp32s3](https://github.com/hpwit/I2SClockLessLedDriveresp32s3)) carried it across to addressable WS2812-class chains shortly after. FastLED's WIP `clockless_lcd_*_esp32.h` is the public-library equivalent ([#1779](https://github.com/FastLED/FastLED/issues/1779), [#2113](https://github.com/FastLED/FastLED/issues/2113)).

**Recommendation**: LCD_CAM (I80 or RGB mode) for ≥ 4 parallel chains, especially with WiFi active. RMT v2 with DMA for ≤ 4 chains and mixed chipsets.

### 2.3 ESP32-P4 (RISC-V, no built-in WiFi)

| Peripheral | Status                                                                                                                                                                                                                                                                              |
| ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| RMT v2     | 4 TX + 4 RX, 48 sym/block, DMA standard. Without WiFi contention, RMT5 finally behaves as advertised.                                                                                                                                                                                |
| PARLIO TX  | **The headline P4 peripheral for LEDs.** Dedicated parallel TX block, 8 or 16 bits wide, hardware bit timing, GDMA. FastLED ships an alpha PARLIO driver ([#2095](https://github.com/FastLED/FastLED/issues/2095), [#2178](https://github.com/FastLED/FastLED/issues/2178)). Auto-detection is the current sore spot; force the driver via build flag for now. |
| LCD_CAM    | Also present on P4. FastLED's LCD driver targets P4 alongside S3 ([#2113](https://github.com/FastLED/FastLED/issues/2113)) but community testing is the bottleneck because P4 hardware is still scarce.                                                                              |
| SPI        | Adds Octal SPI (8 data lines) — useful for big APA102 banks. Not for WS2812.                                                                                                                                                                                                          |

**WiFi coexistence**: a non-issue on P4. If WiFi is needed it's via an ESP32-C6 over SDIO, fully decoupled.

**Recommendation**: PARLIO for any parallel layout, RMT5+DMA for ≤ 4 mixed-chipset chains.

### 2.4 Teensy 4.x (i.MX RT1062, ARM Cortex-M7, no WiFi)

The "trivially great" platform. The relevant peripheral is **FlexIO** — a configurable shifter/timer/state-machine fabric (essentially a smaller PIO before PIO was cool), DMA-fed, runs at the i.MX bus clock.

| Library                  | Use                                                                                                                                                                                              |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| [OctoWS2811](https://www.pjrc.com/teensy/td_libs_OctoWS2811.html) | Paul Stoffregen's reference, FlexIO + DMA. The Teensy-3 "8 channel" limit is gone on Teensy 4 — supports an arbitrary number of digital pins, zero CPU cost during refresh.                       |
| [TriantaduoWS2811](https://github.com/wramsdell/TriantaduoWS2811) | 32 channels from 3 Teensy pins via FlexIO clocking external 74HC595 shift registers, still zero CPU cost. Headline number: ~1.065 M LED updates/second across 32 channels. |
| WS2812Serial             | UART-based, blocking-free, single pin. "Best for &lt;600 LEDs." The fallback when you genuinely just want one strip and don't care about parallelism.                                              |

Teensy 4.1 has **1 MB tightly-coupled RAM** and supports up to **16 MB external PSRAM** via under-board solder pads, accessed via the `EXTMEM` attribute. PSRAM is the natural home for large frame buffers; DMA can read from it. No WiFi means no peripheral-vs-radio contention.

**Recommendation**: OctoWS2811 for ≤ 8 strands; Triantaduo/Duotrigesimal for 16–32 parallel; WS2812Serial for one-strip toys. PSRAM via `EXTMEM` for anything past a few hundred KB of frame buffer.

### 2.5 Raspberry Pi 5 (BCM2712 + RP1 southbridge)

Pi 5 deliberately moved GPIO, PWM, DMA, SPI, I2C off the SoC onto the in-house **RP1** chip. That broke every WS2812 driver that worked by mapping BCM283x PWM+DMA registers directly from userspace — [jgarff/rpi_ws281x #528](https://github.com/jgarff/rpi_ws281x/issues/528) tracks the fallout.

State of the art on Pi 5 in 2026:

| Path                                                                                                                                                                                            | Status                                                                                                                                                                                                                                                                |
| ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [rpi_ws281x Pi 5 branch](https://github.com/jgarff/rpi_ws281x/wiki/Raspberry-Pi-5-Support) (jgarff)                                                                                              | Experimental: custom kernel module `rp1_ws281x_pwm.ko` + device-tree overlay. Single-channel works, concurrent channels not yet.                                                                                                                                       |
| [PIOLib](https://www.raspberrypi.com/news/piolib-a-userspace-library-for-pio-control/) + [Adafruit_Blinka_Raspberry_Pi5_Neopixel](https://github.com/adafruit/Adafruit_Blinka_Raspberry_Pi5_Neopixel) | **The forward-looking path.** RP1 contains PIO blocks like the RP2040; PIOLib is a userspace ioctl wrapper over an `rp1-pio` kernel driver. WS2812 is the canonical PIO demo and lives at [`utils/piolib/examples/ws2812.c`](https://github.com/raspberrypi/utils/blob/master/piolib/examples/ws2812.c). |
| [niklasr22/rpi5-ws2812](https://github.com/niklasr22/rpi5-ws2812)                                                                                                                                | Bit-bang WS2812 timing through SPI MOSI at 2.4 MHz. Single channel, no kernel module needed, well-trodden hack.                                                                                                                                                       |
| Bridge to an MCU                                                                                                                                                                                | Use the Pi 5 as the brains (HTTP, preview, scenes) and hand the strand timing to an ESP32 / Teensy / Pico over USB or SPI. **Cleanest path today.**                                                                                                                  |

**Recommendation**: in MoonLight, treat the Pi 5 as a desktop-class platform that delegates strand timing to an attached MCU. If single-strand-from-Pi-5 ever becomes a requirement, wrap Adafruit's PIO library behind a `WS2812Rp1Driver` and accept the single-strand limit.

## 3. WiFi vs the driver — the failure that defines ESP32 driver design

The defining failure mode on ESP32 is: it works perfectly at boot, then it flickers — wrong colors, shifted pixels — the moment WiFi associates. This is not RF interference. Measured root cause ([FastLED #2082](https://github.com/FastLED/FastLED/issues/2082)): an ISR that should fire every ~35 µs is delayed **40–50 µs under light WiFi load** — i.e. the deadline is missed outright. One missed deadline corrupts one bit, which shifts every downstream pixel of that frame, until the next reset.

Contributing factors, ranked:

1. **WiFi task priority on core 0.** WiFi stack runs at a priority above user tasks and cannot be preempted. If the LED render task is also on core 0, it gets starved.
2. **Flash-cache stalls.** Any ISR or hot function not marked `IRAM_ATTR` stalls during cache-disabled windows (NVS writes, OTA, WiFi calibration). The interrupt must also be registered with `ESP_INTR_FLAG_IRAM`.
3. **Dynamic frequency scaling.** If the CPU drops from 160 MHz to 40 MHz, the APB divider feeding RMT/I2S can't keep up. Force `pm_config.min_freq_mhz = 80`.
4. **RF coupling** — the *inverse* problem, where I2S parallel clock lines next to the WiFi front-end degrade WiFi RX. Rare, board-layout-dependent.
5. **DMA bus contention** — rarely the dominant factor. The AHB matrix arbitrates per-peripheral.

**Mitigations checklist**, in priority order — these are what every production driver does:

- Pin the LED render task to **core 1**, leave WiFi tasks on core 0 (`xTaskCreatePinnedToCore(..., 1)`).
- `WiFi.setSleep(false)` — modem sleep introduces periodic ~100 ms blackouts.
- Mark every ISR and hot function `IRAM_ATTR`; register interrupts with `ESP_INTR_FLAG_IRAM`; constants in `DRAM_ATTR`.
- Raise interrupt priority (level 3 via `esp_intr_alloc`).
- Set `pm_config.min_freq_mhz` ≥ 80.
- Prefer DMA-fed peripherals (RMT-with-DMA, I2S, LCD_CAM, PARLIO) so the CPU is out of the timing loop.

Diagnostic signature for WiFi-induced corruption (vs power or signal-integrity faults): first frames after boot look perfect; symptoms appear only after WiFi associates; pixels are shifted/stuck *from a position outward* (one bit slipped, everything downstream inherits it); color flashes correlate with traffic bursts. Compare against power-fault signature: brownout, whole-strip dim, first-pixel corruption.

For MoonLight, this means: **the LED driver task lives on core 1** (the quiet core, away from the WiFi stack and its interrupts) and **the effects / network path lives on core 0** (it can tolerate latency spikes — a late effect frame is invisible, a late driver bit is a corrupted pixel). This is the inverse of the WLED render-on-core-1 pattern, applied to a different observation about which task has the harder deadline; see § 7.2 for the full rationale and the MoonLight-specific per-module core-affinity story.

## 4. Generic driver architecture

A driver does five things, in order:

1. **Configure** peripheral timing for a chipset variant (WS2812B vs SK6812 RGBW vs APA102) at setup.
2. **Correct + encode** the logical `Buffer` into a peripheral-native bit pattern. The source buffer is linear RGB; the driver applies the shared **output correction** (brightness, channel reorder, RGBW white — see § 4.6) per light, then encodes the corrected, wire-ordered bytes into the bit pattern. Correction and encode fuse into one pass.
3. **Start** the DMA / peripheral, point it at the encoded buffer.
4. **Wait** for completion (or signal completion via callback / semaphore).
5. **Enforce** the inter-frame reset gap (≥ 300 µs).

That's it. The five steps are the same on every MCU; what changes is *who* does step 2 (CPU vs DMA encoder) and what the peripheral in step 3 looks like.

### 4.1 The interface

```cpp
// src/light/drivers/LedDriverBase.h
namespace mm {

struct LedFrame {
    std::span<const uint8_t> bytes;  // logical RGB (3 ch); driver applies Correction (brightness,
                                     // reorder to GRB/…, RGBW white) per light during encode
    uint16_t lightCount;             // pixels (not bytes)
    uint8_t channelsPerLight;        // source channels (3 = RGB today); output channels come
                                     // from the Correction's light-preset (3 or 4)
};

struct LedDriverConfig {
    uint32_t t0h_ns = 350;
    uint32_t t1h_ns = 700;
    uint32_t period_ns = 1250;
    uint32_t reset_us = 300;
    uint8_t  invert = 0;             // for level-shifters that swap polarity
};

class LedDriverBase {
public:
    virtual ~LedDriverBase() = default;
    virtual bool begin(const LedDriverConfig&) = 0;
    virtual void show(const LedFrame&) = 0;     // synchronous: returns after DMA done + reset
    virtual void end() = 0;
};

}
```

`show()` is synchronous because the per-frame budget is small enough that there is nothing useful to do on the same core while a frame is in flight. The caller decides whether to call `show()` from the render task (simple) or from a dedicated task pinned to a different core than the network stack (the WiFi mitigation pattern). The driver itself doesn't pin tasks — that's a wiring decision in `main.cpp`.

### 4.2 Per-platform implementations

```
src/platform/esp32/      Esp32RmtWs2812Driver       // 1-8 chains, mixed chipsets, WiFi-safe
src/platform/esp32/      Esp32I2sParallelDriver     // 9-16 chains, uniform timing
src/platform/esp32s3/    Esp32S3LcdCamDriver        // 8/16 chains, DMA, post-WiFi-clean
src/platform/esp32p4/    Esp32P4ParlioDriver        // 8/16 chains, hardware-clocked, no WiFi
src/platform/teensy/     TeensyFlexIoDriver         // OctoWS2811-equivalent on FlexIO
src/platform/rpi5/       Rpi5PioDriver              // PIOLib via Adafruit Pi5 NeoPixel
src/platform/desktop/    DesktopNullDriver          // writes frames to a file/socket for preview
```

Each platform header is gated by `if constexpr (platform::isEsp32)` etc. at the call site — no `#ifdef` outside `src/platform/`. The selection logic lives in one `main.cpp` factory that returns a `std::unique_ptr<LedDriverBase>` chosen by `platform_config.h` flags.

### 4.3 The encoding step (step 2 above)

For RMT, the encoded form is an array of `(level, duration)` symbols — one per bit, with the duration set from the chipset's T0H/T0L/T1H/T1L. RMT v2 also supports a *callback encoder* that produces symbols on-the-fly, but for a fixed chipset the symbol cost is small (~2 KB for 1000 WS2812 pixels with one strand) and pre-encoding is simpler.

For I2S parallel / LCD_CAM / PARLIO, the encoded form is a *transposed* frame: the buffer is bit-sliced so that one DMA "tick" emits one bit-of-the-same-pixel across all K lanes. Concretely, for K=16 lanes:

```
DMA word at sample N = ((lane0_bitN) << 0) | ((lane1_bitN) << 1) | ... | ((lane15_bitN) << 15)
```

Each WS2812 bit becomes 3 sub-bits at 2.4 MHz (high → high-or-low depending on 1/0 → low), producing the per-bit pulse pattern. This is the same encoding hpwit's I2S driver and `jgarff/rpi_ws281x` use.

The transpose costs O(N×24) per frame on the CPU. On ESP32 classic that's ~50 µs for 1000 pixels — well inside the frame budget. On S3/P4 it's cheaper (vector instructions on LX7 / hardware barrel shifter on RISC-V). Doing the transpose on the *render side* (writing logical pixels into a pre-transposed shape) is faster but couples the buffer layout to the driver — keep it in the driver until profiling proves otherwise.

### 4.4 What goes in shared code vs platform code

Shared, in `src/light/`:

- `LedDriverBase` interface.
- `Buffer` (already exists), `MappingLUT` (already exists).
- Chipset descriptors (WS2812B, SK6812 RGB, SK6812 RGBW, APA102) — pure data: GRB order, channels, default timing.
- Output correction — already exists: `Correction` (`src/light/drivers/Correction.h`), owned by the `Drivers` container, applies brightness / channel reorder / RGBW white per light. The LED driver applies it in step 2 (§ 4.6), the same `const Correction*` ArtNet already uses. Gamma / dither fold into the same LUT later.

Platform-specific, in `src/platform/<target>/`:

- Peripheral setup (clock, DMA channel, pins, interrupt registration).
- Encoder for that peripheral's bit format.
- ISR / DMA callback.

### 4.5 Shift-register expanders

Putting 74HC595 (or similar) on the data lines is **not** a new driver layer — it's a hardware multiplexer that lets one peripheral-controlled clock + data drive N strands. The driver still emits the same bit pattern; the shift register fans it out in space rather than time. The Teensy TriantaduoWS2811 design is the canonical example: 3 Teensy pins (clock, data, latch) drive 32 strands via external '595s.

In the architecture above, an expander variant is a configuration of an existing driver (e.g. `TeensyFlexIoDriver` with `expanderChains = 32`), not a new class. The transpose step gets a wider stride; everything else is unchanged.

For MoonLight this is **third-priority** behind WS2812 and SK6812: design the interface so it could host it (the K-lane abstraction is already general enough), but don't ship it day one.

### 4.6 Output correction (already shipped)

The brightness / channel-reorder / RGBW-white stage is **not** specific to LED drivers and is **already implemented** — ArtNet uses it today. It is a per-driver stage shared by every *physical* driver, with the shared state owned by the `Drivers` container:

- `Correction` (`src/light/drivers/Correction.h`) holds a 256-entry brightness LUT, a channel-order table (the `lightPreset`: RGB / GRB / … / RGBW / GRBW), an output-channel count (3 or 4), and a derive-white flag. `Correction::apply(src, out)` transforms one logical RGB light into the wire form: brightness via LUT, then reorder, then (for RGBW presets) `W = min(r,g,b)` of the brightness-scaled channels.
- `Drivers` exposes `brightness` and `lightPreset` controls and rebuilds the LUT on the cheap `onUpdate` tier (no pipeline rebuild — the slider stays fluent). It hands each child a `const Correction*` via `DriverBase::setCorrection`; Preview ignores it (shows the raw logical buffer).
- An LED driver consumes the *same* `const Correction*`. In the fused single-pass case (§ 4.3, identity / shuffle, no blend) it calls `correction_->apply(...)` per light and encodes the result straight into the DMA buffer — F/G/H and the WS2812 encode are one pass, never a second sweep over encoded bytes.

So the LED driver does **not** re-invent brightness/reorder/white — it reuses the shipped `Correction`. Gamma and white-balance fold into the same LUT later as a per-channel R/G/B split (the field is `briLut`, not `gammaLut`, so that's a fill change, not a rename). See [MoonLight, drivers](../../explanation/architecture/moonlight.md#drivers) for the cross-driver picture.

## 5. Testing architecture

Goal: prove the driver produced the right bits at the right times, without trusting the code that produced them. The methods, ranked by what to actually do:

### 5.1 Software unit tests of the encoding pipeline (table stakes)

In `test/test_led_encoder.cpp`: feed a known logical `Buffer` into the encoder, assert the resulting bit pattern (GRB order, MSB first, T0H/T1H pulse widths in *symbol counts*). This catches color-order bugs, gamma bugs, off-by-one strip length, RGBW-vs-RGB confusion. Runs in CI on every push, zero hardware.

### 5.2 On-board GPIO loopback (the cheap CI workhorse — pick this)

The ESP32 RMT peripheral is a *transceiver*: the RX side captures pulse durations into 15-bit `(level, duration)` symbols at up to 12.5 ns resolution. Wire `GPIO_TX → GPIO_RX` with a jumper, run the driver, decode the RX symbol stream back into bytes.

- Cost: $0 (a jumper).
- Resolution: 12.5 ns native, 100 ns is plenty for WS2812.
- Coverage: bit-level + per-edge timing margin.
- Headless / CI: runs entirely on-device, host harness reads pass/fail over USB.
- Setup: write the RX-symbol → byte decoder. ESP-IDF ships an [RMTLoopback example](https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/RMT/RMTLoopback/RMTLoopback.ino) to fork.

On Teensy 4, FlexPWM dual-input capture gives 6.7 ns resolution; Paul Stoffregen's [WS2812Capture](https://github.com/PaulStoffregen/WS2812Capture) library is purpose-built for exactly this — it captures pulse widths via DMA and decodes bytes per byte. Use it directly.

This is the **primary CI signal-verification method** for the project. It runs on the device under test, on every push, with zero added hardware.

### 5.3 fx2lafw + sigrok-cli (the $12 independent cross-check)

A $10–15 fx2lafw clone gives 24 MS/s on 8 channels, sigrok-compatible. The project's reference device is the **NanoDLA v1.3** (CY7C68013A-based, 24 MHz / 8 channels, fx2lafw firmware), but any board the sigrok `fx2lafw` driver enumerates works the same way. The libsigrokdecode tree has a WS2812 protocol decoder ([logic-ws2812](https://github.com/dustin/logic-ws2812)) that emits RGB triples; [sigrok-cli](https://sigrok.org/wiki/Sigrok-cli) runs it headless:

```bash
sigrok-cli -d fx2lafw -c samplerate=24m --samples 100000 \
           -P ws2812:data=D0 -A ws2812=rgb
```

Why bother when 5.2 exists: the loopback method trusts the *MCU's own clock*. An independent analyzer with its own clock crosses the verification — if loopback and analyzer disagree, you have a real bug. 24 MS/s = 41 ns/sample, enough to *correctness*-check WS2812 (350 vs 700 ns) but marginal for *timing-margin* tests (320 vs 380 ns).

#### 5.3.1 Wiring the NanoDLA v1.3 to an ESP32 and capturing one frame

**Wiring** (any fx2lafw clone follows the same pattern; only the labels differ):

- **Common ground.** Tie the analyzer's `GND` pin to an ESP32 `GND` pin. Without this the analyzer reads garbage — every other concern is downstream of this one. Use the shortest jumper you have.
- **Signal.** Tie analyzer channel `D0` (NanoDLA labels them `CH0..CH7`) to the ESP32 GPIO that drives WS2812 data. For the on-board loopback rig from §5.2 that's the `GPIO_TX` side of the loopback jumper — keep the loopback in place and tap the same line with a second jumper into `D0`. The analyzer is high-impedance; it doesn't load the line.
- **Do NOT power the ESP32 from the analyzer's 5 V / 3.3 V rail.** The CY7C68013A's regulator can't carry an ESP32 under WiFi load. Power the ESP32 over its own USB cable; the analyzer's USB is data-only as far as the ESP32 is concerned.
- **Strand power.** If a real WS2812 strand is on the line too, give it its own 5 V supply with the supply ground also tied to the common ground above. The analyzer taps the signal; it does not drive the strand.

**Voltage levels.** ESP32 GPIO is 3.3 V CMOS. fx2lafw boards specify 3.3 V logic (5 V tolerant inputs). 3.3 V → 3.3 V is a direct connection — no level shifter, no series resistor. Skip every "is it safe?" worry; this is the well-understood case.

**Sample-rate vs strand length.** At 24 MS/s, 8 channels (the NanoDLA's max), the host-side buffer fills at ~24 MB/s. A 30-LED strand is one full frame ≈ 0.9 ms wire-time + reset — well inside one sigrok-cli `--samples` capture window. For larger strands either bump `--samples` proportionally or trigger on the first edge so you only capture the active wire-time.

**Capture + decode (single frame, one strand):**

```bash
# 1. Plug in: NanoDLA via USB, ESP32 via its own USB cable, jumpers as above.
# 2. Confirm sigrok sees the device. Should print one fx2lafw entry.
sigrok-cli --scan

# 3. Capture ~4 ms at 24 MS/s — enough for any reasonable WS2812 frame
#    plus the 50–280 µs idle that latches it.
sigrok-cli -d fx2lafw -c samplerate=24m --samples 100000 \
           -P ws2812:data=D0 -A ws2812=rgb

# 4. The output is one line per pixel, with RGB triples in transmit
#    order. Compare against the byte stream the firmware claims to have
#    sent: that's the cross-check the doc above describes.
```

**Common gotchas, in the order you'll hit them:**

1. **No common ground** — capture looks like random toggling. Re-seat the GND jumper.
2. **Wrong channel** — `-P ws2812:data=D0` vs the channel you actually wired. Re-check the NanoDLA label; CH0 = D0 in the sigrok driver.
3. **Sample rate too low** — at 8 MS/s a 350 ns "0" pulse becomes 3 samples and the decoder will start mistaking 0s for 1s. Always pass `samplerate=24m` explicitly; the driver's default is lower.
4. **PulseView for debugging.** When `sigrok-cli` says "no frames decoded," open the same capture in [PulseView](https://sigrok.org/wiki/PulseView) and look at the raw waveform — usually it's an idle-low gap missing or a stuck bit, both obvious by eye.

### 5.4 DSLogic Plus ($149) — the "later" upgrade

[DSLogic Plus](https://www.dreamsourcelab.com/shop/logic-analyzer/dslogic-plus/): 16 channels @ 100 MS/s (10 ns/sample). sigrok-compatible — the CI scripts from 5.3 reuse without change. 16 channels means you can capture multiple parallel strands at once. The right purchase when a timing-margin bug appears that 24 MS/s can't see.

### 5.5 Saleae, oscilloscope, camera, photodiode

Skip for CI:

- **Saleae Logic 8 / Pro 8** — gold standard, but the [Logic 2 Automation gRPC API](https://saleae.github.io/logic2-automation/) is the only feature that justifies the price over DSLogic for a CI rig, and that only at high test volume.
- **Oscilloscope** (Rigol DS1054Z and similar) — useful for development, fragile for CI (screen-scraping or vendor-specific scripting).
- **Webcam + OpenCV** — end-to-end ("a real LED lit up"), but rolling shutter, exposure noise, and refresh-vs-frame-rate beat make it low-SNR.
- **Photodiode array** — niche; interesting only when a *driver → LED* (vs driver-output) bug is suspected.

### 5.6 Recommended layered test suite

| Layer | Method | Cost | What it catches | When |
| ----- | ------ | ---- | --------------- | ---- |
| Unit  | Encoder tests, host-side | $0 | Encoding bugs, color order, RGBW, off-by-one | Every push, CI |
| Module | Loopback (RMT-RX / FlexPWM-capture) | $0 | Wrong bits emitted, wrong timing on real silicon | Every device-test push |
| Scenario | NanoDLA v1.3 (or any fx2lafw clone) + sigrok-cli | $12 | Cross-check vs MCU clock; reset gap; bus traffic | Per release |
| Margin   | DSLogic Plus + sigrok-cli | $149 | Sub-50 ns timing drift, jitter under WiFi load   | When a specific failure motivates it |

Layers 1–2 ship day one. Layer 3 ships when the first non-trivial driver lands. Layer 4 is reactive — buy it when you have a bug that 24 MS/s can't see.

## 6. Hello-world plan

The goal of hello-world is: end-to-end, one solid-color frame on a real WS2812 strip, with the test rig confirming it was correct, on each platform in turn.

### 6.1 ESP32 classic — first

1. Promote `src/platform/esp32/Esp32RmtWs2812Driver.{h,cpp}` (RMT v1 path; one channel; one strand of 100 WS2812B; pin 4).
2. Encoder: `LedFrame.bytes` → `rmt_item32_t[]` with `T0H/T0L/T1H/T1L` from `LedDriverConfig`.
3. `show()`: configure RMT TX, point at item buffer, start, wait for done, sleep ≥ 300 µs for reset.
4. Wiring in `main.cpp`: pick `Esp32RmtWs2812Driver` via `if constexpr (platform::isEsp32 && !platform::isEsp32S3 && !platform::isEsp32P4)`.
5. Pin the render task to core 1, WiFi/HTTP to core 0.
6. Verification: jumper GPIO 4 → GPIO 5, run RMT-RX decoder, assert decoded byte stream matches what was sent. Plus fx2lafw on the wire as cross-check.

This is the first commit. Everything that follows builds on this.

### 6.2 ESP32-S3 — second

Add `Esp32S3LcdCamDriver` with 8 parallel chains via LCD_CAM (I80 mode). Reuse the encoder logic; the difference is the transposed bit-slice and the GDMA setup. Verification: same loopback (RMT-RX still exists on S3) plus fx2lafw on lane 0.

### 6.3 ESP32-P4 — third

Add `Esp32P4ParlioDriver`. Sane API surface (Espressif provides PARLIO TX driver in ESP-IDF), no WiFi to defeat. The encoded form is the same transposed bit-slice as LCD_CAM, but PARLIO is a cleaner peripheral and the GDMA story is mature. Watch for [#2178](https://github.com/FastLED/FastLED/issues/2178) — auto-detection regression in FastLED; we're not using FastLED, so we don't inherit it, but it's a marker that the IDF API has been moving.

### 6.4 Teensy 4.1 — fourth

Add `TeensyFlexIoDriver`. Easiest of the lot because OctoWS2811 is mature and well-documented. Either link OctoWS2811 directly or re-implement the FlexIO setup; given that the project is C++20 / CMake and OctoWS2811 is Arduino-flavoured, a thin re-implementation is probably cleaner. Verification: `WS2812Capture` library handles loopback directly.

### 6.5 Raspberry Pi 5 — last, and probably as a bridge

Treat the Pi 5 as a desktop platform. Two paths:

- **Bridge** (recommended day one): connect a Pico or ESP32 over USB-serial; the Pi 5 sends frames; the MCU drives strands. Re-uses any of the drivers above. The Pi 5 implementation is just a serial protocol on top of `DesktopNullDriver`.
- **Direct** (only if needed): wrap Adafruit's PIO library behind `Rpi5PioDriver`, accept single-strand limit.

### 6.6 Acceptance bar for each step

Each platform reaches hello-world when:

1. A 100-pixel solid-color test frame renders on a real strip.
2. The on-board loopback test (5.2) passes on real hardware.
3. The fx2lafw cross-check (5.3) passes — same bytes, same reset gap.
4. The fps/jitter KPI line (per [collect_kpi.py](../../../moondeck/check/collect_kpi.py)) is captured and stored.
5. (ESP32 only) the test still passes with WiFi associated and a packet flood running.

## 7. Product-owner decisions

These were the open questions raised by §1–§6; each is now either settled or explicitly deferred with the shape it will take when revisited.

### 7.1 Network protocol — settled

Live LED data over **DDP / ArtNet / E1.31 / WebSocket**. No other protocols in scope for now.

### 7.2 Core affinity — settled, with a generalisation

Default direction is **inverted from §3 of this doc**: the LED driver task runs on **core 1**, the effects / network task runs on **core 0**. The reasoning is the same as §3 (WiFi-stack-and-radio interrupts land on core 0), but applied to a different observation about which side suffers more from disruption:

- If the **driver task is interrupted**, the LED signal corrupts → visible flicker. The driver must be on the quiet core.
- If the **effects task is interrupted**, the next frame is a few ms late → invisible at 30+ FPS. Effects can tolerate core 0.

That is the inverse of what §3 of this doc proposes (which followed the WLED pattern of network-on-core-0, render-on-core-1 — same conclusion, applied to the *render* loop rather than the *driver* loop). In MoonLight the driver and the render-of-effects are separate tasks, so the choice is about which one sits on the quiet core. The driver wins because its deadline is per-bit, not per-frame.

This is also how MoonLight (v2 → MoonLight's predecessor) is wired today: it works, the precedent matters.

**Generalisation**: a single fixed default is ESP-classic-and-S3 sensible. P4 has no built-in WiFi so the choice is moot there. **Because MoonLight already supports per-module core affinity** (the wider plan is to expose this as a control), the driver and effects task affinities become MoonModule-level controls with the defaults above. Other devices (Teensy, Pi 5) ignore the control.

Practical wiring rule for hello-world: in `main.cpp`, pin the LED driver task to core 1 and the effects/render task to core 0 on classic ESP32 + S3; leave it unset on P4 / Teensy / Pi 5. Make the values overridable via the existing core-affinity control.

**Single-purpose-device assumption**: in practice a device runs *either* live LEDs (network-driven art frames) *or* GPIO LEDs (effects engine driving local strands), not both heavy duty at once. The architecture must let either side own the quiet core; for hello-world we pick GPIO-LED-driver-on-core-1 as the default because the LED-driver-task path is what carries hard-realtime requirements regardless of which side is generating the frames.

### 7.3 Color depth at the wire — flexible-from-day-one, default 8-bit

No single answer; the design must be flexible enough to add the other paths later. The architecture supports three modes, with **default 8-bit** for the first commit:

| Mode | Layer buffers | BlendMap | Driver input | Wire | Notes |
| ---- | ------------- | -------- | ------------ | ---- | ----- |
| 8-bit (default) | 8-bit | 8-bit | 8-bit | 8-bit | Simplest, smallest RAM, WS2812 native. The starting point. |
| 8-bit + driver-side dither | 8-bit | 8-bit | 8-bit | 8-bit with temporal dither applied in the driver | Cheaper than full 16-bit; gradient quality between modes 1 and 3. WLED-style. |
| 16-bit pipeline (incl. dither) | 16-bit | 16-bit | 16-bit | 8-bit (driver downsamples + dithers) | Doubles RAM; best gradient quality; required for 16-bit-native LEDs (UCS7604, HD108) when those are added. |

Per-driver configurable; each driver instance declares which mode it operates in via a bound control. The shared upstream pipeline (effects, BlendMap) reads the driver's declared input width and produces buffers accordingly — i.e. switching the driver to 16-bit mode rebuilds the Layer buffers as 16-bit, not just the driver's encoding step.

For the first commit: 8-bit only. Mode 2 and Mode 3 are stubbed in the interface (e.g. `LedDriverBase::inputBitsPerChannel()` returning 8, with the override hook for later modes) but not implemented. This is the "stub code now, full implementation later" pattern from §7.5.

### 7.4 Frame buffering policy / DMA buffers — auto-from-heap, with override

Both single-buffer (write-then-show) and double/N-buffer (render-while-show) need to work; the framework decides per driver at setup based on available DMA-capable RAM. Concretely:

- The driver declares its per-buffer DMA size at setup (computed from strip count × pixels × encoding overhead).
- At `begin()`, the driver allocates `N` buffers where `N = clamp(min_for_target_fps, max(2, available_dma_ram / per_buffer_size), platform_ceiling)`.
- The auto-computed `N` is **exposed as a read-only diagnostic field on the UI**, and **can be overridden** by a bound control (default = `auto`, otherwise = explicit integer).

Per-platform expected ranges (the numbers you cited from MoonLight observation):

- Classic ESP32, no PSRAM: ~7 buffers.
- ESP32-S3, no PSRAM: ~30 buffers.
- ESP32-S3 + PSRAM: ~75 buffers.

These are the *anti-flicker* values — fewer buffers and the refill ISR can't tolerate a WiFi-induced latency spike. The auto-derive logic uses these as upper bounds; the override is the safety valve when the auto-derive is wrong for a given install.

### 7.5 Shift-register expander — yes, with stubs now, full implementation later

Confirmed in scope. The architecture must accommodate `pinCount × stripsPerPin` topology from day one. Concretely:

- `LedDriverBase` exposes `maxStrips()` / `maxLightsPerStrip()` (per the bottom-up doc's API sketch).
- A `ShiftRegMultiplex` configuration object is part of `LedDriverConfig` from the first commit (defaulting to `none()`), even when no driver actually implements it yet.
- Each parallel-clocked driver (`I2sLcdLedDriver`, `Esp32S3LcdCamDriver`, `Esp32P4ParlioDriver`, `TeensyFlexIoDriver`) has stub code that *rejects* a non-`none()` multiplex config with a clear "not implemented yet" return — not a crash, not silent acceptance.
- Full implementation of the multiplex layer is its own later plan, scheduled after the first non-multiplex driver lands and passes the testing layers (§5).

This is the pattern the bottom-up doc calls "Backend × Multiplex" — confirmed as the architectural target.

### 7.6 Pi 5 — accept "Pi 5 + attached MCU", defer firmware-shape decision

Confirmed: MoonLight does **not** commit to driving WS2812 directly from the Pi 5's own GPIO pins. Bridge to an MCU.

The shape of the firmware that runs on the bridged MCU is **deferred** until Pi 5 work actually starts. Two paths are on the table, both viable, both with trade-offs worth weighing at decision time rather than now:

#### Path A — Stripped MoonLight on the bridge MCU

The bridge MCU runs a thin MoonLight build: no UI, no effects engine, no full network listener — just the `Drivers` + `LedDriver` pipeline plus a USB-serial input parser that pushes incoming RGB byte frames into the driver. Estimate: 50–150 KB build target.

Pros:
- Code reuse with the main MoonLight tree. Driver fixes flow into the bridge automatically.
- A bridge MCU is a regular MoonLight build with three modules disabled — no second codebase.
- The bridge can locally expose status/heartbeat/diagnostics via the existing MoonModule control surface.

Cons:
- Larger image. Slower to boot. More to flash.
- Couples the bridge to MoonLight's release cadence and IDF version pin — bridge upgrades become MoonLight upgrades.

#### Path B — Dedicated minimal firmware

Standalone firmware, no MoonLight code reuse: `read N RGB bytes from USB serial → push to a vendored LedDriver implementation → repeat`. Estimate: under 30 KB.

Pros:
- Tiny, fast to boot, easy to flash from any laptop.
- Independent release cadence; the bridge stays stable while MoonLight iterates.
- No build-system overhead — single-file possibility.

Cons:
- Driver code diverges from MoonLight's tree over time unless we vendor + sync deliberately.
- No diagnostic surface beyond what the protocol carries.
- Two codebases to maintain in the longer run.

**Decision deferred** until the Pi 5 bridge becomes a real task. When it does, the deciding factor is likely: how often will the bridge's driver be touched? If rarely, Path B. If it tracks MoonLight's main-line driver work, Path A.

For *this* analysis it's enough to note: the `LedDriver` interface designed in §4 is the same in both paths — both reuse it. The choice between A and B is a *build target* decision, not an *interface* decision.

### 7.7 Carry-over from this doc's TL;DR

These were not in the original "open questions" list but are decisions that need to stay visible:

- **Hello-world platform order remains ESP32 classic → S3 → P4 → Teensy → Pi 5 bridge.**
- **CI signal-verification stack remains layered**: software unit tests, RMT-RX (or equivalent) loopback, fx2lafw cross-check, DSLogic Plus when timing-margin testing becomes necessary.

## 8. Reconciliation with the bottom-up analysis

Read [leddriver-analysis-bottom-up.md](leddriver-analysis-bottom-up.md) at the end, after this analysis was complete. Below are the points of strong agreement, where the two analyses diverge, and where the bottom-up document is meaningfully deeper than this one.

### 8.1 Strong agreement

- **Peripherals per chip.** Same list, same rankings. Classic ESP32 → RMT + I2S-LCD; S3 → LCD_CAM + GDMA; P4 → PARLIO + LCD_CAM; Teensy → FlexIO; Pi 5 → RP1 PIO. Both analyses identify the same "what to drive WS2812 from" answer per platform.
- **WiFi-glitch root cause and fix.** Both converge on the same triple: ISR latency / cache stalls / CPU starvation, mitigated by core pinning + DMA-only data path + IRAM discipline. The bottom-up doc states "the real reason I2S/LCD-CAM drivers don't flicker is that the CPU isn't in the timing-critical path at all" — exactly the same finding as this doc's WiFi section.
- **Pi 5 strategy.** Both reach the same conclusion: bridge to an MCU; PIO-via-RP1 exists but is research-grade today. The bottom-up doc adds the SPI MOSI bit-bang option ([rpi5-ws2812](https://github.com/niklasr22/rpi5-ws2812)) which works through RP1 because SPI is still exposed there.
- **Shift-register expanders as a value-add, not a separate backend.** The bottom-up doc names this explicitly as the "Multiplex axis" — a modifier on top of any parallel-clocked backend, not a sibling of LCD_CAM. This doc reached the same shape ("an expander variant is a configuration of an existing driver, not a new class") but the bottom-up doc has the cleaner vocabulary.
- **Hot path discipline.** Both arrive at the same do/don't list: no allocations, `IRAM_ATTR`, integer math, DMA-from-SRAM-not-PSRAM, no `printf` in `push()`.
- **WS2812 protocol fundamentals.** Same 800 kHz, same per-pixel time math, same ≥ 300 µs reset.

### 8.2 Genuine divergences

- **Build-vs-borrow recommendation.** The bottom-up doc recommends **Scenario B — build everything ourselves**, with five reasons in priority order (multiplex axis, runtime driver switching, identity-mapping fast path, IDF gives plumbing, license/binary-size tax). **This top-down doc does not make a build-vs-borrow recommendation.** It implicitly assumes we write our own drivers on top of ESP-IDF / NXP HALs, never even considering FastLED-as-dependency, because the top-down approach starts from "what is the simplest direct path from buffer to wire" and library overhead drops out.
  Reconciled: the bottom-up doc's recommendation stands. This top-down doc is consistent with Scenario B — same direction, more concrete implementation detail, less analysis of the choice itself.

- **Recommended Stage-2 spike order.** The bottom-up doc starts with **LCD_CAM on S3** (highest-value single backend). This doc proposes starting with **RMT on classic ESP32** (smallest, cheapest, most-likely-to-work first contract). Reconciled: starting with classic-ESP32-RMT is the right "hello world" — the goal there is to prove the `LedDriver` interface and the loopback test harness end-to-end on the simplest peripheral. Once that exists, the LCD_CAM-on-S3 spike from the bottom-up doc becomes the *second* commit and the contract is already known to fit.

- **Buffering policy.** This doc originally treated double-buffering as an open question. The bottom-up doc had already done the math (§ "Performance budget") and concluded: double-buffer is required at any non-trivial light count because DMA transmission dominates the frame budget. **Now resolved** in § 7.4 — buffer count is auto-derived from heap budget with explicit override, expected ranges 7 (classic ESP32) / 30 (S3 no PSRAM) / 75 (S3 + PSRAM). Both docs land in the same place.

- **Identity-mapping fast path.** The bottom-up doc treats this as a load-bearing architectural constraint (see `Drivers.h:90`, "drivers point at `layer_->buffer()` directly, no copy"). This top-down doc mentions zero-copy in passing but doesn't carry it as a constraint. Reconciled: it *is* a constraint. The `LedDriverBase::show(LedFrame&)` interface above already supports it — `LedFrame::bytes` is a `std::span<const uint8_t>` — but the wiring in `main.cpp` has to be explicit about handing the Layer's buffer pointer to the driver in the identity case rather than copying through `outputBuffer_`.

### 8.3 Where the bottom-up doc goes meaningfully deeper

- **API surface for the driver.** The bottom-up doc's `LedDriver` interface in § "Concrete DriverBase API sketch" (§ 477-549) is fully specified: `push(span)` + `onTopologyChange(strips)` + `backendName()` + `maxStrips()` / `maxLightsPerStrip()` for UI greying. This is more concrete than my `LedDriverBase::begin/show/end` and should be the starting point for the actual header.
- **Performance budget table.** The 16K × 50 FPS budget breakdown (§ 559-578) is the kind of math this top-down doc gestures at but does not actually do. It shows `push()` has 3-5 ms; DMA transmission runs 16-20 ms in the background; effects are 8-12 ms; the frame budget is 20 ms total. That table should be carried forward into the actual implementation plan.
- **Library landscape characterisation.** WLED, WLED-MM, NightDriverStrip, FastLED master vs release, NeoPixelBus, ESP-IDF `led_strip` — the bottom-up doc has read each one at a specific HEAD commit and characterised the abstractions, license, maintenance, and gotchas. Even if we walk Scenario B, that landscape map is the documentation we point at when someone asks "why didn't you just use WLED?"
- **Risks and unknowns.** The eight-item risk list (§ 601-612) names specific failure modes — RMT5 API churn, parlio newness, shift-register driver portability, LCD overclock non-portability, hot-reconfigure-during-DMA, WiFi-coexistence empirical variability, identity-mapping path subtlety, build-vs-borrow lock-in. These are real and we will hit them. This top-down doc's § 7 "Open questions" is shallower.
- **Hpwit's source read.** The bottom-up doc has actually read `I2SClocklessVirtualLedDriver.h` (3820 lines, HEAD 2026-05-25) and located the specific entanglements (30 `#ifdef CONFIG_IDF_TARGET_ESP32S3` blocks, duplicated `loadAndTranspose` under different `I2S_MAPPING_MODE` paths, multiplex math intertwined with DMA buffer layout). This is the kind of detail that only comes from reading the source, and it changes the cost estimate for the multiplex-as-reusable-layer claim. Both docs accept the same conclusion ("multiplex code needs per-backend specialisation, not free composition"), but the bottom-up doc has the receipts.

### 8.4 Net effect on the next plan

The two analyses are compatible — neither contradicts the other on any load-bearing decision. The right next plan reads the top-down doc for the architectural shape, the protocol math, the platform-by-platform peripheral choice, and the testing strategy; and reads the bottom-up doc for the build-vs-borrow recommendation, the concrete `LedDriver` interface, the performance budget, and the library landscape map. Hello-world is RMT-on-classic-ESP32 → LCD_CAM-on-S3 → PARLIO-on-P4 → FlexIO-on-Teensy → Pi-5-via-bridge. Both docs agree.

