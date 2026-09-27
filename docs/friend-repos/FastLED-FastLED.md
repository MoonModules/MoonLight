# FastLED: monthly activity digest

What landed on [FastLED](https://github.com/FastLED/FastLED)'s main branch, month by month. External-context reference (like the v1/v2/MoonLight inventories), a factual log of a friend repo's releases, not MoonLight's own history or roadmap. Newest month on top. The reusable prompt that generates these digests lives in [README.md](index.md).

## August 2026

No release cut this month (3.10.4, 2026-06-16, remains the published release, with the 3.10.5 tag on master unreleased), so the month is not split. Three threads dominate: filling in long-standing chipset and board gaps from very old issues, bringing the Raspberry Pi Pico 2 W online with WiFi and Bluetooth, and replacing the MP3 decoder.

**New**
- On-chip flash storage works at last: sketches can read and write files on an ESP32 without an SD card, through a new `fl::getEmbeddedFs()`.
- Raspberry Pi Pico 2 W gains WiFi (CYW43) and Bluetooth LE, plus a device-to-device over-the-air update flow.
- ESP32 sketches can now pick which SPI bus (SPI2 or SPI3) each strip uses, so two clocked strips can run on separate buses.
- New chipsets: WS2818, LC8816E (RGBW), MY9221 (12-channel, as used on Grove chainable RGB modules), TM1812 in five-channel RGBWW mode, and HD107S as an alias.
- New platforms: Chipintelli CI13XX (RISC-V), and Teknic ClearCore / SAME53 boards.
- Non-addressable analog RGB LEDs get a real driver: `addLeds<ANALOG, RED_PIN, GREEN_PIN, BLUE_PIN>()` with 500 Hz PWM, and brightness, color correction, and temperature applied like any other strip.
- Optional Oklab color blending (`fl::blend_oklab`) for smoother, more natural-looking fades than plain channel blending.
- Pixel buffers can be saved and loaded as JSON, with a worked ESP32 flash-storage example.
- ESP8266 sketches can override the clockless wait time with `FASTLED_ESP8266_CLOCKLESS_WAIT_TIME`.
- The MoodRing example is split out on its own and reworked so kick, snare, and downbeat each do something visually distinct instead of all just changing speed.
- MP3 playback switches to the minimp3 decoder and now decodes in fixed point by default, which is faster on chips without a floating-point unit. The old Helix decoder is gone, along with its license restrictions.

**Fixed**
- `FastLED.clear()` and `clearData()` only cleared the first strip of a parallel controller, which also made power limiting under-count. Open since 2020.
- `rgb2hsv_approximate()` no longer overflows and returns wrong hues on some colors. Open since 2020.
- ESP32: software SPI clock toggling no longer stomps on other pins changing at the same time.
- ESP32: strip reset timing now follows each chipset's own datasheet value rather than a fixed one.
- ESP32: selecting the legacy RMT4 backend on ESP-IDF 5 now gives one clear error instead of a build failure, and C2/C3/S2 fall back to bit-bang for clocked chipsets instead of failing.
- ESP8266: Arduino `D0`-`D8` pin constants are no longer remapped twice, and example default pins moved off pins that conflict with serial.
- SAMD51 builds, broken since 3.10.4, compile again; SAMD boards use native SERCOM for hardware SPI; the Adafruit QT Py M0 onboard NeoPixel pin and the Metro M4 board alias are recognized.
- Seeed XIAO nRF52840 Plus/Sense Plus builds work under the Mbed core.
- Teensy: builds fixed across the range (3.x `F_CPU_ACTUAL`, Teensy 4 SD card SPI, parallel output line masks), and Teensy 3.x/LC now emit a warning that the hardware is end-of-life and unvalidated.
- AVR: ATmega644A hardware SPI pins added, and examples no longer default to the serial TX pin.
- `FastLED.wait()` with no arguments now waits until output actually finishes instead of timing out early.
- Video playback no longer crashes on zero-sized frames.
- GS1903 strips get their own timing instead of borrowing WS2812's.
- Browser/WASM preview: asset loading is bounded and verified, so a stalled download no longer hangs the sketch forever.

**Watching**
- A proposed license change from MIT to a new "FastLED Reciprocal License 1.0" is under discussion (#4046, #4047, #4048). Nothing has changed yet: master is still MIT. Under the proposal, shipping a product with modified FastLED would require publishing those modifications.
- A large "profiled color pipeline" design is being planned in ten tracked phases (#4032 through #4044), aiming at datasheet-accurate color, gamut mapping, and float-free fixed-point output.
- The open report that `show()`'s refresh throttle takes an unconditional deep yield (#3762) is still unresolved.

_Auditability: 195 first-parent commits on `master` with author-date 2026-08-01..2026-08-31, via `git log --first-parent --since=2026-08-01 --until=2026-09-01 origin/master`. Issues via `gh api "search/issues?q=repo:FastLED/FastLED+is:issue+created:2026-08-01..2026-08-31&per_page=100"` (102 opened) and the same with `closed:2026-08-01..2026-08-31` (187 closed). The great majority of both are the project's own phase, meta, and CI trackers, so only user-facing ones are surfaced above. No versioned release was published in August (latest published release remains 3.10.4 from June; the `3.10.5` tag is an ancestor of master but has no GitHub release), so no month split._

## July 2026

No release cut this month (3.10.4, 2026-06-16, remains the latest), so the month is not split. Two big threads: finishing the Raspberry Pi Pico driver family, and cutting the ESP32 platform loose from the Arduino core.

**New**
- Raspberry Pi Pico / RP2040: automatic parallel PIO output finally works for real, 2/4/8 strips driven from one PIO program, with a single-lane fallback for mixed layouts.
- Raspberry Pi Pico gains fixed-function SPI+DMA drivers, a UART DMA driver, and a public hardware-SPI routing API.
- WS2814 RGBW strips are now a first-class chipset with datasheet timing.
- Classic ESP32 gains a second I2S bank, up to 32 parallel strip outputs, plus a second UART output lane.
- FastLED can be built as a plain ESP-IDF project with no Arduino core at all: IDF's own time, serial, SPI, LEDC and heap calls are now the default on ESP32, with Arduino only as an opt-in fallback.
- Classic ESP32 also gains an I2S-based signal capture backend (reading WS2812 data in), alongside the existing RMT and LPC845 capture paths.
- LPC845 now defaults to its UART DMA output path.
- Screenmaps can describe EL wire and EL panel shapes; a new HydroPack example drives two EL panels from a microphone beat detector.
- `fl::printf`/`snprintf` accept a generic `{}` placeholder.

**Fixed**
- ESP8266: `addLeds()` no longer watchdog-resets when GPIO12 (D6) is used with P9813.
- `rgb2hsv_approximate()` no longer turns orange into green; CHSV values now compare by field instead of by their RGB rendering.
- TM1829 timing (FLIP + wait time) restored after a refactor dropped it.
- SK9822/APA102 on the classic `addLeds` path now emit correct all-ones end clocks.
- ESP32 I2S clock divider no longer silently truncates, which could produce wrong strip timing.
- `m0clockless` brightness scaling was broken and always output zero.
- Teensy 4.x SPI drivers no longer depend on the Arduino `SPI` library; Renesas boards no longer pull in an I2S header they don't have.
- WASM/browser preview: microphone capture recovers after the user cancels access, and the default renderer works again.

**Watching**
- Report that RGBW output has been broken since 3.10.3 (#3622, closed) fed the month's RGBW colorimetry cleanups.
- An open thread (#3762) blames an unconditional deep yield in `show()`'s refresh throttle for a long-standing frame-timing regression, no fix shipped yet.
- A port to the WCH CH32V003 (48 MHz, 2 KB RAM) is proposed (#3755).

_Auditability: 212 first-parent commits on `master` with author-date 2026-07-01..2026-07-31. Issues via `search/issues` for `repo:FastLED/FastLED+is:issue+created:2026-07-01..2026-07-31` (110 opened) and `closed:2026-07-01..2026-07-31` (106 closed); the great majority are the project's own phase/meta bring-up trackers for RP2040, LPC845 and the classic-ESP32 I2S driver, plus CI and linter work, only the user-facing ones are surfaced above. No versioned release published in July, so no month split._

## June 2026 (up to 3.10.4)

Released **3.10.4** (2026-06-16), cut from `master`.

**New**
- STM32: Arduino UNO Q board support.
- New NXP LPC8xx family drivers land (LPC804 PLU, LPC845 bit-bang + PWM/DMA-to-GPIO, LPC11xx), early bring-up, bench-validated.
- Unified `fl::Watchdog` API with real hardware implementations across platforms (ESP32, Teensy 4, AVR, Apollo3, RP2040, STM32) plus a non-allocating reset/crash-classification helper.
- Wave simulation: opt-in 9-point isotropic Laplacian (smoother 2D waves), exposed as a UI toggle in several example sketches.
- ScreenMap gains a v2 schema (auto-detected), and a new `.fled` container format for video/screenmaps that `FxSdCard` can load.
- FFT now auto-detects ESP-DSP on ESP32 (no opt-in macro needed).

**Fixed**
- RGBW colorimetric path reworked: native LED gamut + D65 as the default source, improved strict/boosted solvers, corrected dual- and 3-channel solving.
- ESP32: reliable streaming for SPI strips over ~680 LEDs; ESP32-C6 routes Serial to HWCDC with non-blocking writes and rejects USB-serial pins for LED output.
- Teensy Audio selection fixed on low-memory boards; nRF52 now honors configured SPI data rates.
- LuminescentGrand example: corrected serpentine column wiring/orientation.

## June 2026 (post-3.10.4)

**New**
- Teensy 4.x LED driver bring-up: ObjectFLED and FlexIO parallel output engines plus a new LPUART-based WS2812 driver (inverted-TX + eDMA), and FlexPWM-based RX capture.
- New WS2812-style RX capture path on classic ESP32 (RMT4) and LPC845 (SCT+DMA).
- ARM Cortex-M DSP-extension SIMD backend wired up for Teensy 3.x/4.x (faster scale/blend on those chips).

**Fixed**
- SM16824E chipset timing corrected to match the datasheet.
- RGBW gamut configuration is now kept per strip.
- Fixed a memory leak in the chunked `fl::deque` container.

Note: the bulk of June's ~481 commits were internal (LPC bring-up scaffolding, the AutoResearch hardware-test harness, a Python→Rust C++ linter migration, RPC/JSON size-and-speed tuning, and test-file splits) and are not user-visible. Notable issue traffic was dominated by the Teensy driver and LPC845 bring-up trackers; a user-reported "multi strip problem" (#3340) was triaged and closed, and a deque refactor briefly broke a macOS-arm64 audio unit test (#3286, fixed same month).

_Auditability: 481 commits with author-date in 2026-06-01..2026-06-30 on `master` (first-parent/merged view); split at the 3.10.4 release (published 2026-06-16, an ancestor of `master`). Issues reviewed via `search/issues` for `created:2026-06-01..2026-06-30` and `closed:2026-06-01..2026-06-30` (~50 each); user-facing ones folded in above, the remainder were internal bring-up/CI/linter trackers._

## May 2026

*Summarised from 150 first-parent commits on `master`, 2026-05-01 … 2026-05-31.*

**New**

- New **Channels API** for managing multiple LED drivers at once, a `fl::Bus` type, `FastLED.add<Bus>(...)`, `fl::enableAllDrivers()`, and `FastLED.setExclusiveDriver(...)`, with a diagnostic that warns when a strip's driver doesn't match its bus. (The month's biggest effort.)
- **RGBW / RGBWW color**: proper colorimetric RGB→RGBW conversion with a lookup table, color-temperature (CCT) control, and an RGB+CCT mode.
- ESP32-P4 gains a SIMD (PIE) acceleration backend for faster pixel processing.

**Faster**

- Big speedups to the ESP32-P4 **PARLIO** parallel driver, encoding and transmission now overlap, and a chipset-aware encode path is ~5× faster than before.
- ESP32-P4 "Wave8" output ~1.2–2× faster via new transpose and lookup-table paths.

**Hardware & build**

- Builds cleanly on **ESP-IDF 6**, and can now be used as a standalone ESP-IDF component.
- ESP32 OTA support fixed (adds the required update/mDNS dependencies).
- Fixes for several boards: nRF52 Xiao BLE Sense and other nRF52 variants, ESP8266 / STM32 / ESP32-C3 / Teensy 4 / UNO R4 WiFi build issues, and ESP32-S3 LCD-clockless ISR safety.

**Fixed**

- RGBW driver no longer kept a dangling pointer to its color profile (could crash or corrupt output).
- AVR boards no longer try to compile RGBWW examples that overflow their memory.

## April 2026

*Summarised from 237 first-parent commits on `master`, 2026-04-01 … 2026-04-30.*

**New**

- **Audio "silence gate"** across the audio-reactive features, tempo, spectral metrics, and the Vibe effect now fade out cleanly when the input goes quiet instead of reacting to noise.
- Audio FFT can run on the ESP-DSP hardware backend (faster spectrum analysis on ESP32).
- ESP32-S3 LCD driver gains ISR-driven chunked DMA output (smoother large-strip output); coroutine tasks can be pinned to a chosen core.
- RMT receive (reading signals in) gains DMA streaming; a long-strip SPI bug (#2254) fixed.

**Fixed**

- ESP32-S3 LCD-clockless GPIO crash; SPI bus ownership/buffer-reuse issues when switching drivers.
- Board fixes: Teensy 4.1 pins 40–54, Arduino Due (sam3x8e), ESP8266 register-name clash, Digispark ATtiny85.

## March 2026

*Summarised from 444 first-parent commits on `master`, 2026-03-01 … 2026-03-31.*

- Mostly an internal stability and build-correctness month (sanitizer fixes, WASM build speed, IWYU/PCH hygiene), little user-facing.
- **Fixed:** AVR builds (replaced defaulted `noexcept` with explicit implementations in container types); ESP32-C3/C5 and Teensy build breakages; printf/Arduino-compatibility shim; an audio-path bug.

## February 2026

*Summarised from 516 first-parent commits on `master`, 2026-02-01 … 2026-02-28.*

- **Fixed (broad platform-stability month):** AVR math bugs (left-shifts wider than AVR's 16-bit int; `PROGMEM` LUT reads), ESP32-C6 dual-mode async/sync SPI, ESP32 WROOM + mbedTLS, ESP8266, Teensy LC, UART compiler issues, RGBW mode, and the UCS chipset preamble.
- HTTP-server / loopback networking pieces stabilised; OTA validation fixed.

## January 2026

*Summarised from 582 first-parent commits on `master`, 2026-01-01 … 2026-01-31.*

- A heavy **stability-hardening month**, most of the work was fixing memory and initialization bugs surfaced by sanitizers (ASan/LSan/UBSan): use-after-free, memory leaks, static-initialization-order issues, shared-pointer errors.
- **Fixed (user-visible):** a crash in power management; RP2350 system defines not being included; an ISR error where an int was read as a bool; i2s/LCD-CAM.

## December 2025

*Summarised from 392 first-parent commits on `master`, 2025-12-01 … 2025-12-31.*

**New**

- **PARLIO driver maturation** (ESP32 parallel output): streaming support, up to 16 lanes, larger per-channel LED counts, background DMA buffer worker, and a low-level hardware abstraction layer, plus many alignment/timing fixes (the "1-bit shift" buffer-boundary bug).
- **Validation / proof-of-life framework**: hardware-in-the-loop validation, an ESP32 watchdog (with a USB-disconnect fix), and a result banner.
- New `Potentiometer` class (hysteresis + calibration); 16-bit PWM pin support (`setPwm16`); per-channel gain on the HD108 chipset; `Serial` gains `printf`.
- Signal **receive (RX)** gains raw edge-time capture and a safe sketch-halt.

## November 2025

*Summarised from 528 first-parent commits on `master`, 2025-11-01 … 2025-11-30.*

**New**

- **Channels / ChannelBusManager foundation**: a unified, priority-based driver manager with fallback, centralized SPI driver registration, and a new ChannelEngine-based SPI driver + RMT4 driver for ESP32 IDF 4.x. (Start of the multi-driver architecture that continues through to May.)
- **PARLIO** gains a runtime-configurable multi-channel driver with auto-select (and dropped ESP32-S3, which uses LCD instead).
- New **UCS7604** controller; a generic clockless waveform generator; video playback support.
- **Audio-reactive** effects expand, downbeat-darkness effect, AnimartrixRing audio reactivity, configurable UI audio.
- WASM web preview moves to dedicated worker threads (drops Asyncify) with incremental/PCH build speedups.
- Experimental RISC-V interrupt support.

## October 2025

*Summarised from 1,185 first-parent commits on `master`, 2025-10-01 … 2025-10-31.*

**New**

- **RP2040 automatic parallel output** using the standard FastLED API.
- Per-platform ESP32 clockless controllers + configurable ESP32/ESP8266 timing; nanosecond timing support for ARM K66/KL26; SPI chipset controllers split into their own headers.
- Fallback **OTA** implementation for ESP-IDF < 4.0.
- PARLIO strategic buffer-breaking at color boundaries.
- 8-bit math optimised for ATtiny; math template/float overloads; a beat-detection `AudioProcessor` facade.
- New "advanced effects" and "LED cookbook" documentation chapters.

## September 2025 (post-3.10.3)

*Summarised from 162 first-parent commits on `master`, 2025-09-21 … 2025-09-30 (after the 3.10.3 release on the 20th).*

**New**

- **WASM web-preview overhaul**: Three.js-based tile rendering, instanced LED rendering, SharedArrayBuffer zero-copy frames, a background-worker async controller, and an improved video recorder (native `captureStream`, 60 FPS, better MP4 compatibility).
- New hardware-accelerated **ezWS2812** GPIO + SPI drivers for Silicon Labs MGM240 / EFR32MG24 (MG24) boards.
- **Codec support**: progressive JPEG decoding (4 ms time budget), and metadata parsing for GIF/JPEG/MPEG1.
- Bilinear interpolation for upscaling effects; `FxNoiseRing` low-memory mode.
- README/wiring guidance for high-parallel LED setups (incl. ObjectFLED parallel capacity).

