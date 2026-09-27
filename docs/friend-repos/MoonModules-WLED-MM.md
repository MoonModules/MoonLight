# WLED-MM: monthly activity digest

What landed on [WLED-MM](https://github.com/MoonModules/WLED-MM)'s `mdev` (default) branch, month by month. External-context reference, a factual log of a friend repo's releases, not MoonLight's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](index.md). Months are split at versioned-release boundaries (the rolling `nightly` tag is not a release).

## August 2026

*Summarised from 1 commit on `mdev` (no versioned release cut this month; the rolling `nightly` prerelease republished on 2026-08-13).*

Dormant month. The single commit is a build-configuration fix with no user-visible change: the audio-reactive usermod is pinned by its full commit hash, because the pioarduino build tooling fails on short hashes. Nightly builds therefore keep resolving the audio-reactive dependency correctly.

_Auditability: 1 commit on `mdev`, author-date 2026-08-01..2026-08-31 (f2d32c9c, "use long hash in AR_lib_deps"), via `gh api repos/MoonModules/WLED-MM/commits?sha=mdev&since=2026-08-01T00:00:00Z&until=2026-09-01T00:00:00Z`. Issues checked: `search/issues?q=repo:MoonModules/WLED-MM+is:issue+created:2026-08-01..2026-08-31` (0) and `closed:2026-08-01..2026-08-31` (0), no issues opened or closed all month. Releases checked (`repos/MoonModules/WLED-MM/releases`): only the rolling `nightly` prerelease (2026-08-13), which is not a versioned release, so no month split._

## July 2026

*Summarised from 2 commits on `mdev`, both 2026-07-01 (no versioned release cut this month; the `nightly` prerelease republished on 2026-07-02 packages June's work).*

Near-dormant month, a single small change and a build-number bump.

- The instance list now shows which repo (WLED-MM or upstream WLED) each discovered instance runs, and reports the right release for upstream WLED instances.

*Auditability: 2 commits on `mdev`, author-date 2026-07-01..2026-07-31 (range 12b0238 … 7c55f91; 7c55f91 is a build-number bump, omitted as not user-facing). Issues checked: `repo:MoonModules/WLED-MM is:issue created:2026-07-01..2026-07-31` (0) and `closed:2026-07-01..2026-07-31` (0), and `updated:2026-07-01..2026-07-31` (0), no issues opened, closed, or even commented on all month. (The June window returns 2 on the same query form, so the zeros are real rather than a mistyped repo name: the search API needs `MoonModules/WLED-MM`, not `MoonModules/WLED`.)*

## June 2026

*Summarised from 38 commits on `mdev`, 2026-06-01 … 2026-06-25 (no versioned release cut this month, so the month is not split; the `nightly` prerelease is not a release).*

**New**
- Waveshare ESP32-S3 Matrix Driver board profile added.
- Audio-reactive is now an out-of-tree usermod (pulled in as a dependency) rather than baked into the tree, no user-facing change to how it works, but a cleaner build.

**Fixed**
- Output settings no longer revert to defaults after a reboot (regression in 14.7.2 nightly, reported on ESP32-D0WDQ5 and ESP32-S3; issue #367).
- LEDs no longer flicker on startup and during GIF playback.
- Custom palettes reload immediately after you upload a palette file (previously needed a manual refresh; relates to long-standing issue #105).
- Fixed `esp32dev_compat` firmware build failing (issue #371) and a broken `-D WLEDMM` build flag.
- ArtiFX (ARTI effect engine) robustness: call-stack bounds checks, safer string/lexer handling, and fixes for glitches seen only in speed-optimised builds (relates to issue #295); ARTI status now shown in the Info panel.
- Upstream compatibility: accepts `I2CSDAPIN`/`I2CSCLPIN` as alternatives to the older I2C pin defines; fixed the arduinoFFT dependency; brown-out restart info now checked on both cores.

*Auditability: 38 commits on `mdev`, author-date 2026-06-01..2026-06-30 (range 84669c3 … 70fe1b8; several are CHANGELOG/version-bump/CodeRabbit-config/internal-refactor commits, omitted as not user-facing). Issues checked: created 2026-06-01..2026-06-30 and closed 2026-06-01..2026-06-30, 4 relevant surfaced (#367, #371 fixed this month; #105, #295 long-standing, closed/addressed this month).*

## May 2026

*Summarised from 34 first-parent commits on `mdev`, 2026-05-01 … 2026-05-30.*

- **Ethernet board support:** added QuinLED v4 Ethernet profiles, a legacy Olimex ETH-Gateway option, and fixed the KIT-VE PHY address. **Breaking:** a duplicate Ethernet option was removed, re-select your board if you used the previous Olimex-ESP32-Gateway entry.
- Audio-reactive auto-disables during DDP / DMX / Art-Net input (avoids the two fighting over the LEDs).
- Persistent on-screen error display when a restart is needed (errors no longer scroll away unseen); Improv and MQTT input hardened against malformed data.
- Web UI accessibility improvements.

## April 2026

*Summarised from 47 first-parent commits on `mdev`, 2026-04-01 … 2026-04-30.*

- **DDP input** hardened, rejects malformed / unsupported / "control" packets, relaxed header checks for compatibility.
- Robustness: recovers gracefully from an empty `{}` config file (`cfg.json` / `wsec.json`) instead of misbehaving; steadier serial on ESP32; ESP8266 build fixes.
- Otherwise a documentation / AI-contributor-guideline month (little user-facing).

## March 2026

*Summarised from 43 first-parent commits on `mdev`, 2026-03-01 … 2026-03-31.*

**New / effects**

- New **Color Clouds** smooth effect; Flow effect fixed so the start/end of a segment flows correctly; Spots effect fixes.
- ESP-NOW remote gains 3 more buttons.
- Info page shows total LEDs, PSRAM size, GitHub repo, with restyling.

**Fixed**

- APA102 crash on classic ESP32 with PSRAM; a segment-index misalignment bug; ESP8266 flickering during file writes; default mic-level method changed from "floating" to "freeze".

## February 2026

*Summarised from 42 first-parent commits on `mdev`, 2026-02-01 … 2026-02-28.*

- **Memory / "Heap too low" work:** moved the WS-LED preview buffer into PSRAM, PSRAM-aware allocation, reduced JSON buffers on S3-without-PSRAM, fewer out-of-memory failures on tight boards.
- **Board support:** ESP32-S3 QSPI build, builds without the HUB75 driver (4 MB / 16 MB variants), better handling of ESP32 PICO-D2/V3 and D0WDR2-V3 (frees GPIO17), startup serial now prints HUB75 pins + full chip revision.
- Audio: disabled broken I2S 16-bit sampling; fixed Ethernet errors when using I2S audio.
- Spots effect fixes; fixed a short black-out when a playlist advances.

## January 2026 (post-v14.7.1)

*Summarised from 48 first-parent commits on `mdev`, 2026-01-13 (after v14.7.1) … 2026-01-31.*

- **Animartrix** overhauled, optional gamma correction, always paints in 2D, big math speedups, dependency upgrade, and several bugfixes (segment-option changes now respected).
- **New ESP32 node types** for ESP-NOW (WizMote data); Philips Hue robustness; PixelForge GIF tool gains image rotation.
- **Fixed:** DMX-output now rate-limited to prevent watchdog resets; "relay does not turn on" sporadic issue; stack-smashing crash risk from `notify()`; better 2D preview color accuracy and PS Fireworks trails.
- New V4 build environments incl. `esp32_16MB_V4_M_eth` (16 MB ESP32 with Ethernet); IR re-enabled for the Athom Music build.

## January 2026 (up to v14.7.1)

*Summarised from 32 first-parent commits on `mdev`, 2026-01-01 … 2026-01-13, released as **v14.7.1**.*

- **Release v14.7.1.**
- Random per-LED colors via the JSON API (`"col":["r","r","r"]`); manual/dual auto-white modes work with palettes; segment-palette functions inlined for speed.
- PixelForge effect adjustments; GIFs no longer blurred by default; the Colors column stays visible when toggling the GFX button.
- **Fixed:** preset/file-access glitches, status-LED stops blinking; clearer "Connection to light failed!" message; Info page shows the GitHub repo, build flags, and status-LED pin.

## December 2025

*Summarised from 48 first-parent commits on `mdev`, 2025-12-01 … 2025-12-31.*

**New / effects**

- Several new and improved effects: **Shimmer** (new), **Color Clouds** groundwork, fixed "Flow Stripe" + palette support, refactored Android FX, DistortionWave update, Twinklecat reverse option, Soap effect ~3× faster, general effect-math speedups up to 3×.
- More segment data / no effect-data limits on PSRAM boards; Perlin noise now uses FastLED's `inoise16`.

**Fixed**

- ledmap parser robustness (no longer discards `ledmap.json` too early); FX checkmark sync; 2D matrix-generator preview update; a UI TypeError with a custom palette selected; random trail flickering.

## November 2025

*Summarised from 100 first-parent commits on `mdev`, 2025-11-01 … 2025-11-30.*

**New**

- **DDP over WebSocket** added; improved 1D support for GIF images with a blur option; image-effect flicker fixer for WS2812b on RMT.
- UCS chipset and bus handling improvements; faster `map()`, inline gamma correction, HUB75 / 2D `drawPixel` speedups.
- Audio-reactive UDP sync improved.

**Board / build**

- Large reorganisation of the MoonModules build environments: larger program partition for Ethernet builds, 32 MB-flash (WROOM-2) and Adafruit board partition files, moved C3/S2 builds to the Tasmota platform, revived a "legacy V3" ESP32 build, fixed S3-without-PSRAM builds. ESP8266 GIF player removed (too little RAM).

**Fixed**

- Reboot loop on V3 "legacy" builds; AP-not-showing (default channel → 6); device-fingerprint / deviceId crashes; buffer-bounds and concurrency precautions; LED glitches during file writing.

## October 2025

*Summarised from 28 first-parent commits on `mdev`, 2025-10-01 … 2025-10-31.*

**New / effects**

- ParticleFX better defaults for 64-px-height matrices and a framebuffer memory-calc fix; HUB75 drops color-temperature correction for performance.
- WLED-MM-specific error effect instead of the plain orange flash on effect-memory failure; low-brightness gradient "jumpyness" fixed.

**Fixed**

- Random corruption of `presets.json`; IR JSON decode buffer overrun (#272, rotary usermod); PSRAM caching bug. Updated AsyncTCP / AsyncWebServer (3.4.7).

## September 2025

*Summarised from 3 first-parent commits on `mdev`, 2025-09-01 … 2025-09-30.*

- Quiet month on `mdev`, build instructions, npm `ci` for dependencies, and GitHub Copilot contributor instructions. Nothing user-facing.
