# Funkelfetisch/MoonLight: monthly activity digest

What landed on [Funkelfetisch/MoonLight](https://github.com/Funkelfetisch/MoonLight), month by month. External-context reference, a factual log of a friend repo's activity, not MoonLight's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](index.md).

This is a **fork of this project** building a commercial product on it: **HELIO**, a physical "infinity sphere" lamp (a warm-white 3000 K RGBW strip inside a clear acrylic shell). The fork's own plan documents describe matching a browser preview to "the transmitted LED frame, warm-white output, clear acrylic, internal reflections, and optional wall bounce", and its firmware carries a `helio1` sdkconfig variant, a curated preset player, and a branded update channel. The README is unchanged from ours, so this is MoonLight plus a product layer rather than a rebrand.

**Branch note: the work is not on the default branch.** `main` tracks our upstream and has not moved since 2026-07-09; every change lives in named branches, so each month below carries a **Branches** line for what moved on them. The repository has no issues and publishes no releases.

## August 2026

- **HELIO product layer, stabilised.** One large commit ("Stabilize HELIO scenes, previews, output, and onboarding", 147 files vs our `main`) covering scene playback, the browser preview, LED output and first-run onboarding for the sphere product.
- A browser-side optical simulation of the lamp (`helio-preview.js`, `heliotrace.js`, ~2,000 lines) renders internal reflections and an optional wall bounce, so the dashboard preview resembles the physical object rather than a flat grid.
- A **Pixelblaze pattern interpreter** (`PixelblazeCompatEffect.h`, ~1,300 lines) runs Pixelblaze-style patterns on the device.
- An **ambient-light service** (BH1750 over I2C) publishes a brightness target that drivers consume, for automatic brightness.
- A **curated WLED preset player** keeps source effect IDs, names, speed/intensity values and playlist order as data, rendered through this project's own palette and light-buffer primitives.

- **Branches:** only `codex/helio-private-wip` moved (2026-08-20). The other seven have been dormant since July.

_Checked: commits on `main` for 2026-08-01..2026-09-01 (0; `main` last moved 2026-07-09); commits on all 9 branches for the same window, filtered to fork-authored work (1: `a649bd47` on `codex/helio-private-wip`, 2026-08-20, 139 files; the branch's other August commits are upstream `MoonModules/projectMM` carry-forward); no commit on any branch between 2026-08-21 and 2026-09-01; releases published (none); issue search `repo:Funkelfetisch/MoonLight is:issue created:2026-08-01..2026-08-31` and the same with `closed:` (0 results each, the repository has no issue tracker activity)._

## July 2026

Six feature branches opened, none merged to the fork's `main`, alongside a carry-forward of our own `next-iteration`. Each is a self-contained proposal against this project rather than product work:

- **Automatic firmware updates**, the device polls a JSON manifest, checks the advertised version, chip family and flash offset against its own build info, and starts an OTA with SHA-256 and expected-size verification. Manifests over plain HTTP or without a declared size are refused.
- **BLE WiFi provisioning**, credentials over Bluetooth using Espressif's `wifi_provisioning` component, gated to run only while the device is in access-point fallback, in its own firmware variant (it costs roughly 320 KB of flash).
- **WiFi reconnect handling**, a debounce before applying typed credentials, and repeated station retries before falling back to an access point.
- **RMT LED output over DMA**, enables the DMA backend for WS2812 transmission on chips that have it, with a completion callback instead of a blocking wait, so a delayed interrupt under network load cannot stretch a bit cell into visible flashing.
- **RGBW colour correction**, a wider set of channel-order presets, and an explicit white channel taken from the source when the layer carries one.
- **Frame pacing**, a target-frame-rate cap on the scheduler, work-time metrics, and a periodic-tick sweep spread across ticks rather than run in one burst.

- **Branches:** `codex/upstream-auto-update-manifest` (07-11), `codex/upstream-network-sta-reconnect` (07-10), `codex/upstream-rmt-rgbw-performance` (07-10), `codex/performance-frame-pacing` (07-11), `codex/universal-ble-provisioning` (07-13), `codex/helio-private-wip` (07-10, initial WIP), `next-iteration` (07-10, a carry-forward of our upstream branch). `main` last moved 2026-07-09.

_Checked: commits on `main` for author-date 2026-07-01..2026-08-01 (2, both upstream carry-forward); commits on all branches vs `MoonModules/projectMM@main` for the same window (11); releases published (none); issue search `repo:Funkelfetisch/MoonLight is:issue created:2026-07-01..2026-07-31` and the same with `closed:` (0 results each)._

## June 2026

- **Gyro / IMU input**, an MPU6050 accelerometer and gyroscope read over I2C, publishing angular rate plus pitch and roll as read-only values, with a desktop simulation so the interface shows live numbers without the hardware attached.

- **Branches:** `feature/gyro_module` (06-05). No other branch activity.

_Checked: commits on all branches vs `MoonModules/projectMM@main` for author-date 2026-06-01..2026-07-01 (1); releases published (none); issue search `repo:Funkelfetisch/MoonLight is:issue created:2026-06-01..2026-06-30` and the same with `closed:` (0 results each)._
