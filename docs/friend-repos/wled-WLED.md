# WLED (upstream): monthly activity digest

What landed on [wled/WLED](https://github.com/wled/WLED)'s `main` branch, month by month. External-context reference, a factual log of a friend repo's releases, not MoonLight's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](index.md).

Months are **not** split at release dates: upstream WLED cuts releases from separate release branches (`0_15`, `16_x`), so the version tags aren't on `main`, `main` is the development trunk that feeds future releases. Each month notes which release shipped, as context.

## August 2026

A quieter, consolidation month on `main` after July's V5 platform switch: the toolchain moved forward again, drawing and segment bugs got fixed, and a set of long-open field reports were finally closed. No versioned release was published in August, so the month is not split.

**New**
- Trunk builds move to ESP-IDF 5.5.4, and the ESP32-C5 target now builds on the same platform as the rest (its NTP workaround is gone, so time settings behave normally there).
- The Waveshare ESP32-S3 HUB75 build gains the SHTC3 v2 temperature and humidity sensor usermod.
- Effects can now load palettes from their own code, so an effect can pick or cycle palettes itself.

**Fixed**
- Circle drawing is more accurate: outlines no longer come out slightly flat, and filled circles are rounder.
- Renaming a segment while effects are running no longer risks a crash on dual-core ESP32.
- If the device runs out of memory at startup, it now creates a small default segment instead of showing "no segments" with most controls disabled.
- Pac-Man is hardened against drawing outside the strip, and ESP8266 gets a smaller DDP send packet so streaming does not fail, plus about 2.5 KB of flash back by dropping unusable GIF code.
- ESP32 chip revision is reported correctly again on the V5 platform.

**Fixed (reported by users)**
- FW1906 strips no longer light the cool-white and warm-white channels on certain solid colors (#5812).
- The effects list no longer loads incomplete or broken over a slow connection (#5813).
- Waveshare ESP32-S3-RGB-Matrix HUB75 boards: boot failures (#5776) and swapped green and blue channels (#5815) both resolved.
- Long-running strip flicker and strobe reports from the 0.15 line were closed out, including the LEDs-flashing-every-10-30-seconds thread (#4805), DRGB realtime strobing (#5512), and the effect jumping to "Copy Segment" (#5506). Also closed: white flash at full brightness before the boot preset loads (#5468), a permanently locked OTA on QuinLED Dig-Uno (#5158), and Gledopto Ethernet dropouts (#5431).

**Watching**
- Adding a second LED output crashes on 17.0.0-dev; confirmed and marked major (#5770).
- Two proposals are gauging community interest: PPP-over-serial as a network transport, turning the USB cable into a full network link (#5811), and a DDP compression extension for low-bandwidth links (#5810), the busiest thread of the month.
- An RFC proposes rewriting the settings web UI to be schema-driven, generated from the code rather than hand-written HTML (#5792).
- A WLED-MM backport is proposed upstream: switching DDP, E1.31 and Art-Net output to AsyncUDP for better streaming performance (#5816).
- A BSSID typed with colons or dashes is silently parsed wrong, so access-point pinning never matches (#5797).

_Auditability: 52 commits on `main` with author-date 2026-08-01..2026-08-31 (range aa98fe4 ... c472e41), via `gh api repos/wled/WLED/commits?sha=main&since=2026-08-01T00:00:00Z&until=2026-09-01T00:00:00Z`. Issues via `search/issues` for `repo:wled/WLED+is:issue+created:2026-08-01..2026-08-31` (19 opened) and `repo:wled/WLED+is:issue+closed:2026-08-01..2026-08-31` (14 closed); only user-facing ones surfaced. No versioned release published in August 2026 (`repos/wled/WLED/releases`), so no month split. Internal refactors (the `netmindz` global-state encapsulation series, ~15 commits), CI changes, docs and dependency bumps are omitted._

## July 2026

The month `main` switched to the **V5** platform: WLED's trunk moved from the ESP-IDF 4.4 / arduino-esp32 v2 build to ESP-IDF 5.3 / arduino-esp32 v3, and the long-running `V5` branch became the development trunk (merged July 19). Maintainers warned publicly that `main` would be unstable for a while, and the web UI now shows a "development build" banner. v16.0.1 shipped July 7 from a release branch, so the month is not split.

**New**
- Trunk builds move to ESP-IDF 5.3 / arduino-esp32 v3, opening the door to the newer chips (ESP32-C5, C6 and P4 build targets ride along).
- ESP-NOW now uses WLED's own code instead of the QuickESPNow library, faster, and roughly 10 KB more free memory on ESP32 (1.5 KB on ESP8266).
- New `esp32_eth_V4` build for Ethernet boards; ESP32-C6 boards get 4 MB and 8 MB builds.
- Nightly builds renamed, and the web UI warns when you're running a development build.
- Audio-reactive now compiles on all the newer chips.

**Fixed**
- ESP32-C3 and S3 no longer boot-loop on the new platform (device-ID and audio-reactive builds rescued).
- Usermod settings: non-pin dropdowns no longer reserve GPIO pins, which was blocking pin choices elsewhere.
- Several DMX-input crashes and a robustness fix in its configuration; a possible array overrun in the Improv response.
- ESP8266 minimum build shrank by 1.5 KB; the unmaintained `WLED_SAVE_RAM` build option was removed.

**Watching**
- The loudest thread of the month is #5746, asking the project to do unstable work on a `dev` branch rather than breaking `main`.
- Ethernet users want the WiFi access point to switch off entirely once the wired link is up (#5762).
- Open v16 field reports: Animated Staircase segments switching instead of fading (#5731), APA102 on GPIO19 misbehaving (#5728), HUB75 colour-order options hidden and mostly unimplemented (#5723), and a request for current-limited LED support in the brightness limiter (#5715).

_Auditability: 40 first-parent commits on `main` with author-date 2026-07-01..2026-07-31 (56 including merged sub-commits). Issues via `search/issues` for `repo:wled/WLED+is:issue+created:2026-07-01..2026-07-31` (20 opened) and `closed:2026-07-01..2026-07-31` (15 closed); only user-facing ones surfaced. v16.0.1 (2026-07-07) is not an ancestor of `main` (GitHub compare reports `main` and `v16.0.1` diverged), so no month split._

## June 2026

Post-16.0 stabilisation month: no new version tag (v16.0.0 shipped 2026-05-03 off a release branch, so the month is not split), just a steady stream of bugfixes and small additions landing on `main`.

**New**
- HUB75 matrix panels: added a "Seengreat" pinout, plus fixes for 4-scan and chained-panel setups.
- Renamed the "CW" LED type to "CCT" for clarity when configuring CCT white strips.
- V5-C6 boards are now covered by the V5 build.

**Fixed**
- "Rainbow" and other color-wheel effects no longer mis-drive the white channel on RGBW strips.
- Restored the pre-16.0 look of several effects that had changed appearance after the 16.0 upgrade (also fixes the DJ Light intensity regression).
- HUB75: restored missing pixel trails on some 2D effects (Black Hole, Lissajous, Spaceships).
- Fixed a crash when creating a 2D setup larger than the actual number of LEDs.
- Fixed LED glitches on long strips with ESP32-C3.
- Nightlight: brightness now applies correctly, including small transition steps, and no longer resets when set to max brightness.
- Color no longer jumps when you change it mid-transition; gamma is now applied correctly during realtime/live-data override.
- Improved boot behaviour for boot presets, and "Reset segments" now respects "Make a segment for each output".
- Fixed a ledmap parser reading past the end of the map, an analog-button reading fix, and a pixel-buffer refresh after changing matrix dimensions.
- Better brownout detection and extended error codes aligned with WLED-MM.

**Watching**
- Discussion opened on switching from plain gamma to an sRGB transfer function for better low-brightness accuracy (#5707), and on improving the Nodes/Instances page (#5711), no shipped outcome yet.
- Several v16.0 field reports still open: multi-controller sync losing color (#5705), UDP sync failing in AP mode (#5709), and OTA-update trouble on some boards (#5682, #5702).

_Auditability: 43 commits on `main` with author-date 2026-06-01..2026-06-30 (`repos/wled/WLED/commits?sha=main`, first-line view; a few older-dated cherry-picks appear in-range and were excluded as non-June). Issues via `search/issues` for repo:wled/WLED created:2026-06-01..2026-06-30 (18 opened) and closed:2026-06-01..2026-06-30 (25 closed); only user-facing ones surfaced. No versioned release published in June (v16.0.0 was 2026-05-03), so no month split._

## May 2026

*Summarised from 72 first-parent commits on `main`, 2026-05-01 … 2026-05-31. (Trunk after the **v16.0.0** release on May 3; v16 is codenamed "Kagayaki".)*

**New / boards**

- HUB75: FM6124 driver for 4-scan panels and the 64×64 limit removed on PSRAM boards; new ESP32-S3-without-PSRAM build environments; option to build against the Espressif framework instead of Tasmota.

**Fixed / hardened**

- Effects: restored palette wrap in `color_wheel()` (a regression since 0.15.x), Twinkle fixes, Dissolve "Complete" same-color-as-background fix, gravity audio-reactive top-LED fix.
- Audio-reactive auto-suspends in realtime modes (but stays on with "use main segment only").
- DDP and all realtime protocols: relaxed-but-safer header acceptance + bounds checks; Improv/UDP parsing hardened; `/reset` auth clarified.
- Auto-migration for legacy sunrise/sunset config; animated-staircase inverted-PIR support.

## April 2026

*Summarised from 67 first-parent commits on `main`, 2026-04-01 … 2026-04-30. (v16.0.0-beta was tagged April 11.)*

**New / effects**

- Game of Life fix; FPS bump via a fast path in `blendSegment`; better packet queuing/pacing for custom-palette live preview.
- PixelForge palette/tools list moved into the repo; fxdata serialized without ArduinoJSON (smaller/faster).

**Fixed**

- Critical Candle-FX bug + Flow-FX integer issue; segment inputs no longer restrict trailing strips; iOS blending-style list filter; DDP flag-bit masking for compatibility; robustness rewinding file pointers before writes.
- (Otherwise a heavy AI-contributor-guideline / docs month.)

## March 2026

*Summarised from 65 first-parent commits on `main`, 2026-03-01 … 2026-03-31. (v0.15.4 shipped March 14 from the 0_15 release branch; trunk version bumped to 17.0.0-dev.)*

**New / effects**

- **Full FastLED replacement** merged (#4615), WLED's own color/math instead of the FastLED dependency.
- Many new user_fx effects: Spinning Wheel, Color Clouds, Lava Lamp, Magma, Ants, Morse Code, Comet (fire particle system), a slow >4-hour transition FX, Tetris line-clear flash.
- Scrolling-text FX gains custom fonts + international UTF-8; stencil blending mode; ESP32-C3 audio-reactive (DSP FFT + integer math); more macro/timer slots; longer max playlist duration.
- OTA update page restyled (auto-sets download URL from `info.repo`); clearer UI tool icons.

**Fixed**

- Segment-index misalignment; hostname/DNS cleanup; hue preservation in color fade; array-bounds on short WS payloads; DDP rejects unsupported/non-display packets.

## February 2026

*Summarised from 35 first-parent commits on `main`, 2026-02-01 … 2026-02-28.*

**New**

- **Version scheme changed to Major.minor** (dropped the leading "0."), heading toward v16; bumped to 16.0.0-alpha.
- New **Pin Info** page (used/available pins overview); UI settings readability improvements.
- Improved bus handling, free choice of bus driver in any order, better memory calculations; gamma lower-limit removed (enables inverse gamma correction, applied to segment brightness too).
- Extended CCT blending (exclusive blend, color-jump fix); full WiFi scan with BSSID apply; new ESP32-S3 8MB QSPI build; experimental ESP32-C5/C6 in the node list.

**Fixed**

- LED animations briefly pausing at bootup (ESP32); boot-up WiFi pause with extended scanning; removed dangerous mutex macros in the bus manager; Flow-FX flow at segment start/end.

## January 2026

*Summarised from 38 first-parent commits on `main`, 2026-01-01 … 2026-01-31.*

**New**

- **New custom-palettes editor** (#5010); WPA-Enterprise WiFi support; random per-LED colors via JSON API; option to save unmodified presets to autosave; PixelForge GIF image rotation.
- Removed the MAX_LEDS_PER_BUS limit for virtual buses; new ESP32 node types; JSON validation + minify on file upload in the UI.

**Fixed**

- Relay not turning on at boot; GPIO0 always grabbed by a button; gamma correction on a fresh install; Ethernet static-IP ignored; HUB75 improvements; config exceeding the LED limit.

## December 2025

*Summarised from 39 first-parent commits on `main`, 2025-12-01 … 2025-12-31. (v0.15.3 shipped December 4 from the 0_15 release branch.)*

**New / effects**

- **PacMan effect** and the new **"WLEDPixelForge"** image & scrolling-text interface (#4982); dynamic LED-type dropdown; improved 2D particle collisions; sequential UI-resource loading.
- Usermod Temperature uses full 12-bit precision; "peek" shows gaps; removed legacy EEPROM support.

**Fixed**

- FX checkmark sync; UI TypeError with a custom palette; rotary-encoder palette-count off-by-one; particle-collision binning; segment overflow.

## November 2025

*Summarised from 46 first-parent commits on `main`, 2025-11-01 … 2025-11-30. (v0.15.2-beta1 → **v0.15.2** shipped Nov 9 / Nov 29 from the 0_15 release branch.)*

**New**

- **New file editor** (#4956) with ctrl+S, toasts, efficient ledmap reading, 0-byte-file handling.
- **DDP over WebSocket**; improved 1D GIF support with blur option; variable button count up to 32; Dissolve "Complete" mode (always fades fully).
- Aurora FX speedups; better PSRAM-MB usage reporting; bootloader offsets for C3/S3 + variable bootloader sizes per MCU; Adafruit board partitions.

**Fixed**

- Stale UI after firmware updates; AP-not-showing (default channel tweaks); device-fingerprint crashes; `millis()`-rollover robustness in wait logic; OTA update for C3 from 0.15; ESP8266 low-heap and DMA fixes.

## October 2025

*Summarised from 9 first-parent commits on `main`, 2025-10-01 … 2025-10-31.*

- Quiet month on trunk. **DDP over WebSocket** groundwork (shared WS connection in common.js); Twinkle blank-area fix; low-brightness gradient "jumpyness" fix; bootloop-tracker safety check; GIF-player inactive-segment + copy-FX bugfixes.

## September 2025

*Summarised from 41 first-parent commits on `main`, 2025-09-01 … 2025-09-30.*

**New / effects**

- **Shimmer FX** added; "unrestricted" number of custom palettes; center-bin selection for 2D GEQ; Twinklecat reverse option; speed optimisations + `restoreColorLossy` fix.
- Heap-memory and PSRAM handling improvements; HUB75 AC fixes.

**Fixed**

- Tri Fade FX; custom-palette color picker; Colortwinkles; LED buffer-size calculation; UDP name-sync rework; crash debug output added.
