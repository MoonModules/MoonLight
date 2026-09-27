# hpwit/I2SClocklessLedDriver: monthly activity digest

What landed on [hpwit/I2SClocklessLedDriver](https://github.com/hpwit/I2SClocklessLedDriver)'s `main` branch, month by month. External-context reference, a factual log of a friend repo's activity, not MoonLight's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](index.md).

The library: Yves Bazin's (hpwit) clockless-LED driver that clocks WS2812-class strips out over the ESP32's I2S/LCD peripheral with DMA, the parallel-output technique MoonLight's LED-driver analysis studies. Summarised via the GitHub commits API (all commits on `main`), filtering out merge commits, lint/format churn, and 🐰-review fixups. No versioned release is cut from `main` in this window (latest tag is 1.4), so months are kept whole.

> **Authorship note.** Most of the activity in this window is MoonLight's own, `ewowi` authored ~53 of the in-window commits, with the rest from the maintainer (Yves Bazin / hpwit) and a couple of others. The IDF 5.5 / arduino-less ESP-IDF / RGBCCT / >65K-LED work below is largely MoonLight upstreaming its driver needs into hpwit's library, then tracking the result here.

## August 2026

No activity. No commits on `main` in August 2026, and none on any other branch. The driver last changed on 2026-04-06, and the `esp32-p4-support` branch, which is the one carrying new hardware work, last moved on 2026-04-11.

No issues were opened or closed. No release; the most recent is v1.4 from April 2026.

_Checked: `repos/hpwit/I2SClocklessLedDriver/commits?sha=main` for 2026-08-01..2026-09-01 (0), and the same window on all 6 branches (0); releases published (none); issue search `repo:hpwit/I2SClocklessLedDriver is:issue created:2026-08-01..2026-08-31` and the same with `closed:` (0 results each)._

## July 2026

No user-facing activity: no commits merged to `main` (latest activity is April 6, 2026) and no notable issues. No branch saw commits either, the newest work anywhere is the `esp32-p4-support` branch, last touched April 11, 2026.

_Auditability: commits on `main` author-dated 2026-07-01..2026-07-31 = 0 (0 merged), and 0 on every other branch; issues created/closed/updated in July 2026 = 0; PRs created = 0. No versioned release published in July (latest tag `1.4`, 2026-04-06)._

## June 2026

No user-facing activity: no commits merged to `main` (latest activity is April 6, 2026) and no notable issues.

_Auditability: commits on `main` author-dated 2026-06-01..2026-06-30 = 0 (0 merged); issues created/closed/updated in June 2026 = 0. No versioned release published in June (latest tag `1.4`, 2026-04-06)._

## April 2026

*~17 commits on `main`, 2026-04-01 … 2026-04-30.*

- **Arduino-less ESP-IDF support hardening**: ML/ESP-IDF compile fixes, `esp_rom_delay_us`, `#ifndef HARDWARESPRITES` guard.
- Moved `gNbDmaBuffer` / `gNumStrips` into the driver class (less global state); CI + documentation set up; integer types tightened to `uintx_t`.

## March 2026

*~9 commits on `main`, 2026-03-01 … 2026-03-31.*

- **RGBCCT support** added; an ESP-IDF `CMakeLists.txt` + small-modifications pass for arduino-less ESP-IDF builds.
- Memory-management improvements; `setGamma` arguments switched to RGB order; semaphore-wait logging tuned.

## January 2026

*~4 commits on `main`, 2026-01-01 … 2026-01-31.*

- **>65K-LED support**: `total_leds` widened to 32 bits; added `extractWhiteFromRGB`.
- ESP32-D0: removed `ESP_INTR_FLAG_IRAM` from `esp_intr_alloc`; `deleteDriver` checks on the DMA tampon buffers.

## December 2025

*~6 commits on `main`, 2025-12-01 … 2025-12-31.*

- **White channel at a configurable offset** (not fixed at index 3); `driver->p_w` bugfix.

## November 2025

*~2 commits on `main`, 2025-11-01 … 2025-11-30.*

- ESP32-S3 I2S init timing tweak (`div_num/a/b`), small stop-delay; reverted an SK6812 clock-speed adjustment.

## October 2025

*~19 commits on `main`, 2025-10-01 … 2025-10-31.*

**New**

- **IDF 5.5 support**: version checks, dynamic DMA-buffer allocation (PSRAM-preferred), `NUM_STRIPS` as a global, `IRAM_ATTR` removed from forwards to compile warning-free.
- `updateDriver` / `deleteDriver` gained length/size and per-strip offset parameters; `initled` with custom color arrangement; `isVirtualDriver` flag; `setDelay()`.
- Split a `Driver.cpp` out of the header; added clang-format and removed the `COLOR_ORDER_` compiler directives.

**Fixed**

- `_gammab`-to-RGBW bugfix; `pins` array made `uint8_t` for compatibility with other libraries.
