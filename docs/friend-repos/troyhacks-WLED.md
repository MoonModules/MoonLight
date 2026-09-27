# troyhacks/WLED: monthly activity digest

What landed on [troyhacks/WLED](https://github.com/troyhacks/WLED)'s `mdev` branch, month by month. External-context reference, a factual log of a friend repo's activity, not MoonLight's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](index.md).

This is a personal fork of [MoonModules/WLED-MM](https://github.com/MoonModules/WLED-MM) (the `mdev` branch), so much of the `mdev` traffic is merges from and alignments with the MM and upstream WLED lines; the bullets below pick out what a *user* of this fork would notice. Summarised via the GitHub commits API (all commits on `mdev`, not first-parent merges), filtering out build-number bumps, merge commits, typo/comment churn, and pure refactors. No versioned release is cut from this branch (it tracks `mdev` and ships nightly `mdev` builds), so months are kept whole.

**Branch note, the experiments live off `mdev`.** troyhacks branches heavily: `mdev` is the merge/alignment stream, but the distinctive work happens in named experimental branches (HDMI output, ESP32-P4, W5500 Ethernet, hardware-panel ports, voice control, a pure-IDFv5 port, a new settings subsystem). Those are *experiments*, not necessarily destined for `mdev`, so each month below carries a separate **Experimental branches** line for what moved on them, the frontier of what this fork is probing.

## August 2026

No independent work on `mdev`: the 2 commits in the window are an alignment with the MoonModules line (a merge of `MoonModules:mdev` on 2026-08-24, bringing in the audio-reactive dependency pin plus the ARTI scripting robustness work and the Waveshare S3 Matrix Driver board profile that had accumulated upstream since June), and the MM commit itself. Nothing user-facing originates in this fork on `mdev`. No versioned release was published, so the month is kept whole.

- **Experimental branches:** `P4_experimental` is the active frontier, with 20 commits in August. The Pro DJ Link integration gained a strobe effect and can now switch playlists the way AutoMusic does, and its effect shuffling no longer washes everything to white. Art-Net output was reworked so custom pixel remapping applies to it, large pixel counts were fixed, and the custom mapping table now saves only the part actually in use and shows unmapped entries as `-1` rather than a large number. On ESP32-P4, external SD card audio input over I2S now reads correctly under IDF v5.

_Checked: commits on `mdev` for author-date 2026-08-01..2026-08-31 (2: b537e0c9 merge, f2d32c9c inherited from MoonModules/WLED-MM), via `gh api repos/troyhacks/WLED/commits?sha=mdev&since=2026-08-01T00:00:00Z&until=2026-09-01T00:00:00Z`. All 28 branches were scanned for August activity; only `P4_experimental` moved (20 commits, 2026-08-04 ... 2026-08-29). Releases published in August 2026: none (`repos/troyhacks/WLED/releases`), so no month split. Issue search `repo:troyhacks/WLED+is:issue+created:2026-08-01..2026-08-31` and `closed:2026-08-01..2026-08-31` both return 0, the issue tracker is disabled on this fork._

## July 2026

No user-facing activity: no commits were merged to `mdev` in July 2026 (the branch's most recent commit is still dated 2026-05-20), and no versioned release was published. The repository's issue tracker is disabled, so no issues were opened or closed.

- **Experimental branches:** nothing moved in July either, the most-recently-touched branch, `P4_experimental` (ESP32-P4), had no July commit either, and neither did any other branch.

_Checked: merged commits on `mdev` for author-date 2026-07-01..2026-08-01 (0 commits); commits on `P4_experimental` for the same window (0); releases published in July 2026 (none); issue search `repo:troyhacks/WLED is:issue created:2026-07-01..2026-07-31` and `closed:2026-07-01..2026-07-31` (0 results, issues disabled on this fork)._

## June 2026

No user-facing activity: no commits were merged to `mdev` in June 2026 (the branch's most recent commit is dated 2026-05-20), and no versioned release was published. The repository's issue tracker is disabled, so no issues were opened or closed.

_Checked: merged commits on `mdev` for author-date 2026-06-01..2026-06-30 (0 commits); releases published in June 2026 (none); issue search `repo:troyhacks/WLED is:issue created:2026-06-01..2026-06-30` and `closed:2026-06-01..2026-06-30` (0 results, issues disabled on this fork)._

## May 2026

*~18 commits on `mdev`, 2026-05-01 … 2026-05-31.*

- Quieter month on `mdev`, alignment with upstream/MM, smaller effect and build-flag fixes; the heavy lifting was in the Nov–Jan window.
- **Experimental branches:** `P4_experimental` (ESP32-P4) and `M5Stack_Core_S3_Display` both saw work, the two most-recently-touched branches in the repo.

## April 2026

*~53 commits on `mdev`, 2026-04-01 … 2026-04-30.*

- Continued PixelForge (image + scrolling-text interface) refinements and effect/UI fixes.
- Ongoing alignment with upstream WLED and WLED-MM busmanager / segment paths.
- **Experimental branches:** `Olimex_HDMI_Output` (HDMI video output on Olimex hardware) and `T-Display-P4_Experimental` (ESP32-P4 board bring-up).

## March 2026

*~46 commits on `mdev`, 2026-03-01 … 2026-03-31.*

- Effect and 2D-matrix fixes; build-target and partition adjustments.
- More upstream-compatibility alignment in the segment / bus drawing code.
- **Experimental branches:** `Pure_IDFv5_Port` (a from-scratch ESP-IDF v5 port, no Arduino) and `New-Settings-Subsystem` (settings rework with usermod auto-detection) both started.

## February 2026

*~59 commits on `mdev`, 2026-02-01 … 2026-02-28.*

- Audio-reactive receive path hardening continued (sequence checks, packet handling).
- Effect tuning and PixelForge follow-ups.
- **Experimental branches:** `mdev+W5500` (W5500 SPI-Ethernet support layered onto `mdev`).

## January 2026

*~138 commits on `mdev`, 2026-01-01 … 2026-01-31.*

**New**

- **PixelForge** image/GIF tooling gained image rotation; WLED-MM-specific adjustments.
- **RMTHI** (high-speed RMT LED output) now works on ESP32-S2 and S3; new 16 MB ESP32-with-Ethernet build target.
- Random colors via the JSON API (`"col":["r","r","r"]`); Animartrix optional gamma correction + math-optimization speedups.
- Nightly-build automation: automatic version stamping, cleaner release notes, "Nightly mdev Build" titling.

**Fixed**

- DMX output rate-limiting to prevent a watchdog reset; ESPDMX and Philips Hue robustness improvements.
- Audio-reactive UDP: automatic packet drop + improved format/sequence detection; user option to purge the audio queue.
- Always allow the serial console on S3/C3/C6; fix Hub75 removal breaking Hub75 builds.

**Experimental branches**

- `W5500_Support` (W5500 SPI-Ethernet, S3 Ethernet range moved up) and `DF2301Q_Voice_Control` (on-device voice control with hot-plug retry) both active.

## December 2025

*~97 commits on `mdev`, 2025-12-01 … 2025-12-31.*

**New**

- **WLEDPixelForge**: a new image and scrolling-text interface (`pxmagic`), with 1D GIF support, blur option, and version-14.x adaptations.
- Effect math sped up (up to ~3× faster); inlined hot-path color/segment functions; more segment/effect data allowed on PSRAM boards.
- DDP-over-websockets / DDP-over-WS stability; E1.31 kill switch; `dnrgbw` realtime mode.

**Fixed / hardened**

- Large **preset/ledmap robustness pass**: fixed `presets.json` corruption (mutex protection on the write path), ledmap-parser robustness, reduced UI freeze when updating presets.
- Extensive **mutex / critical-section redesign** across segment and `bus.show` paths, fixes for semaphore leaks, "giving a semaphore never taken", and realtime-lock race conditions.

## November 2025

*~166 commits on `mdev`, 2025-11-01 … 2025-11-30.*

**New**

- **Full Codepage-437 / high-ASCII text support** (UTF-8→UTF-16 decoder) for scrolling text and GIFs.
- Improved 1D GIF support (blur option, bugfixes); `WLEDMM_FASTPATH` enabled for all ESP32 builds; device-ID + version-reporting features.
- WLED-MM branding for the update message box; various Help/README link updates.

**Fixed / hardened**

- HUB75 speedups and DMA-cleanup ordering; 2D drawPixel optimizations; JMap use-after-free fixes.
- USB-mode handling reworked for CDC-on-boot boards (fixes serial breakage and stale-UI-after-update issues).
- ESP8266: dropped the GIF player (too much RAM), reverted to a known-good async webserver.

**Experimental branches**

- A burst of hardware-panel and video-output branches: `ESP32-P4-86-Panel-ETH-2RO` (P4 86-size panel with Ethernet), `HDMI-Experiment` (HDMI output + "maybe faster Art-Net"), and `WaveShare_10.1_Panel` (WaveShare 10.1″ panel).

## October 2025

*~32 commits on `mdev`, 2025-10-01 … 2025-10-31.*

- **DDP-over-websockets** added; HUB75 skips color-temperature correction for performance.
- `setPixelColor` / `getPixelColor` hot-path optimizations (cached-bus path, `colorKtoRGB` fix, IRAM placement); particle-FX framebuffer memory-calc fix.
- Bugfixes: preset-corruption prevention, IR-JSON buffer-overrun, low-brightness gradient smoothness.

## September 2025

*~15 commits on `mdev`, 2025-09-01 … 2025-09-30.*

- New **Shimmer** effect; reverse-checkmark option for Twinklecat.
- Build process learns to extract the GitHub repo/version into the firmware; 2D-matrix-generator preview fix; AutoPlaylist race-condition fix.
