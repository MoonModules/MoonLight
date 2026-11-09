# Funkelfetisch/MoonLight: monthly activity digest

What landed on [Funkelfetisch/projectMM](https://github.com/Funkelfetisch/projectMM) (earlier named Funkelfetisch/MoonLight), month by month. External-context reference, a factual log of a friend repo's activity, not MoonLight's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [index.md](index.md).

This is a **fork of this project** building commercial products on it: **HELIO**, a physical "infinity sphere" lamp (a warm-white 3000 K RGBW strip inside a clear acrylic shell), and since November 2026 **NEBULITE**, a line of LED garments (a Croptop and a Jacket on the ESP32-S3, 2021-era ESP32 jackets and bags, and a HELIO necklace) controlled from a phone over BLE, with battery controllers, signed firmware updates over BLE and a migration path from WLED. The firmware carries three product variants (`helio1`, `nebulite-s3`, `nebulite-esp32`), built with one flag that leaves out the modules no product uses, a product dashboard at `/` beside our generic editor at `/modules`, and a `src/helio/` product directory. The README is unchanged from ours, so this is MoonLight plus a product layer rather than a rebrand.

**Branch note: the work is not on the default branch.** `main` tracks our upstream and has not moved since 2026-07-09. Since November 2026 every change lives on one branch, `nebulite-helio-7`, cut from our `main` of 2026-10-28; the earlier `codex/*` branches are deleted from GitHub, their commits archived on a private remote the author names `nebulite-firmware`. The repository has issues disabled and publishes no releases.

## November 2026 (to date)

Seventy-one commits on one new branch, `nebulite-helio-7`, all between 2026-11-03 and 2026-11-09, adding NEBULITE beside HELIO.

- **The whole product layer re-based onto our tree as one commit** (414 files, about 60,000 added lines, 2026-11-03): BLE control independent of WiFi, a battery service, a BH1750 light sensor, a preset store, a product dashboard, twelve WLED-faithful NEBULITE effects (Blink, Firefly, Flow Stripe, Noisefire, Palette, Plasmoid, Rainbow, Sine, Snowflakes, Solid, Washing, Waterfall), `src/helio/` and 41 bench scripts.
- **BLE control, third protocol revision:** an MTU of 517 so a request fits one write, replies as notifications, list operations that stream every page from one request, a timer that retries a reply the stack had no buffers for, and an ambient-light operation for the phone. Each controller advertises its own name; an unnamed garment is `NEBULITE-` plus four MAC digits.
- **Firmware updates over BLE, signed by the publisher:** a P-256 key compiled in, claims (target, size, security version, image SHA-256) and a signature checked before the update slot opens, the image streamed on its own characteristic and acknowledged per window; a signing script prints the release manifest the phone app bundles.
- **WiFi setup from the phone over BLE**, credentials only over an encrypted link, and a handset remote; garments boot with WiFi off.
- **A 2025 WLED S3 controller moves to MoonLight without USB:** a migration image WLED's own update page accepts saves WLED's networks, takes the signed MoonLight image from the phone in chunks, rewrites the partition table and starts MoonLight, resuming after a cut. A 2021 classic ESP32 image ends on a short packet so the old BLE updater finishes installing it.
- **Fixes found on the boards:** a failed `new (std::nothrow)` aborted the ESP32 instead of returning null, now replaced with `malloc`; control descriptors shrink to 40 bytes with flags as bits, about 9 KB back on the classic ESP32; the HTTP listener retries a refused port and follows a port change live; the product dashboard loads three to four times faster.
- **Product firmware leaves out what no product uses:** one build flag drops MoonCloud, Devices, Hue, the SD card, HUB75 and the parallel LED outputs, `helio1` from 2.81 MB to 2.63 MB.
- **The S3 garment's battery controller stays on** through an update's restart, a reboot and a crash, its power pins held by the pad hold; each product's boot pins print at boot and are pinned by tests, and an onboard LED shows whether a phone or a network reaches the controller.
- **The browser preview as the lights emit it:** a frame over a slow BLE link finishes, and each light shows only the channels its outputs carry, so RGB strips show no white.
- **Smaller generic touches:** brightness shown as a percentage while the value stays 0 to 255, the Network card saying whether the local name answers and why not, `mdns` pinned to 1.14, a clean checkout stamping a clean build id, a restart keeping the chosen scene.
- **A branded NEBULITE dashboard:** the app's palette from the first paint, animation cards that play by themselves, Static and Music groups, garment illustrations; the import of historical WLED looks was removed.
- **Process:** the docs gate taken from 835 errors after the port to 0, eight MIGRATING entries, seven plans under `docs/work/present/`. Every commit carries an agent co-author trailer; 20 of the 71 commit messages report a test on hardware.
- **PR #158 closed:** the author closed [MoonModules/MoonLight#158](https://github.com/MoonModules/MoonLight/pull/158), the BLE transport, on 2026-11-04 without merging; the fork's BLE plan records lighting control without mandatory pairing as the product's choice.
- **Branches:** `nebulite-helio-7` is new (2026-11-03, head `289d1931` on 2026-11-09), based on our `main` at `04472e0d` with 71 commits on top and 66 behind. The `codex/*` branches are deleted, `main` stays at 2026-07-09 and `feature/gyro_module` at 2026-06-05.

_Checked: after `git clone` and fetch, commits on `origin/main` for 2026-11-01..2026-11-09 (0); commits on all 3 remote branches vs `MoonModules/MoonLight@main` for the same window (71, all fork-authored, all on `nebulite-helio-7`; 0 fork commits for 2026-10-28..2026-10-31); three-dot diff against our `main` 486 files, +69,492/-1,032; releases published (none); issues disabled (`has_issues: false`), so no issue search._

## October 2026

One fork commit, on a new branch cut from our `main` late in the month.

- **An optional BLE control transport** (`codex/moonlight-ble-transport`, 2026-10-25, 399 added lines across ten files): an encrypted control channel for a controller outside WiFi range.
- The peripheral is a native NimBLE one with GATT attributes for identity, state and commands, and pairing with persistent bonds.
- A bounded 512-byte line reassembler applies backpressure and recovers after rejected input, and the exchange with the scheduler is non-blocking.
- The caller owns startup, identity and request dispatch. Desktop, Teensy and BLE-disabled ESP32 builds share one stub, and shipping firmware defaults are unchanged.
- Provenance: the commit message says the hardware test ran on the HELIO prototype before the transport was extracted, and that Codex wrote the contribution at the author's request.
- Status: offered to this project the same evening as [MoonModules/MoonLight#158](https://github.com/MoonModules/MoonLight/pull/158), open at month end.
- **Branches:** `codex/moonlight-ble-transport` is new (2026-10-25), based on our `main` at `abcdaec9` with one commit on top. Nothing else moved: `codex/helio-private-wip` stays at 2026-08-21 and `main` at 2026-07-09.

_Checked: after `git fetch --all --tags`, commits on `origin/main` for 2026-10-01..2026-11-01 (0); commits on all 10 remote branches for the same window (182, all on `codex/moonlight-ble-transport`: 181 upstream `MoonModules/MoonLight` carry-forward, 1 fork-authored, `f5a244a9`); releases published (none); issue search `repo:Funkelfetisch/projectMM is:issue created:2026-10-01..2026-10-31` and the same with `closed:` (0 results each; the repository has issues disabled)._

## September 2026

Dormant month: no commit on any branch.

- **Branches:** none moved. The most recent activity anywhere is `codex/helio-private-wip` on 2026-08-21, and `main` last moved 2026-07-09.

_Checked: after `git fetch --all --tags`, commits on `origin/main` and on all 9 remote branches for 2026-09-01..2026-10-01 (0); releases published (none); issue search `repo:Funkelfetisch/projectMM is:issue created:2026-09-01..2026-09-30` and the same with `closed:` (0 results each). The repository now answers to `Funkelfetisch/projectMM`, and `Funkelfetisch/MoonLight` no longer resolves through the API._

## August 2026

- **HELIO product layer, stabilised.** One large commit ("Stabilize HELIO scenes, previews, output, and onboarding", 147 files vs our `main`) covering scene playback, the browser preview, LED output and first-run onboarding for the sphere product.
- A browser-side optical simulation of the lamp (`helio-preview.js`, `heliotrace.js`, ~2,000 lines) renders internal reflections and an optional wall bounce, so the dashboard preview resembles the physical object rather than a flat grid.
- A **Pixelblaze pattern interpreter** (`PixelblazeCompatEffect.h`, ~1,300 lines) runs Pixelblaze-style patterns on the device.
- An **ambient-light service** (BH1750 over I2C) publishes a brightness target that drivers consume, for automatic brightness.
- A **curated WLED preset player** keeps source effect IDs, names, speed/intensity values and playlist order as data, rendered through this project's own palette and light-buffer primitives.

- **Branches:** only `codex/helio-private-wip` moved (2026-08-20). The other seven have been dormant since July.

_Checked: commits on `main` for 2026-08-01..2026-09-01 (0; `main` last moved 2026-07-09); commits on all 9 branches for the same window, filtered to fork-authored work (1: `a649bd47` on `codex/helio-private-wip`, 2026-08-20, 139 files; the branch's other August commits are upstream `MoonModules/MoonLight` carry-forward); no commit on any branch between 2026-08-21 and 2026-09-01; releases published (none); issue search `repo:Funkelfetisch/MoonLight is:issue created:2026-08-01..2026-08-31` and the same with `closed:` (0 results each, the repository has no issue tracker activity)._

## July 2026

Six feature branches opened, none merged to the fork's `main`, alongside a carry-forward of our own `next-iteration`. Each is a self-contained proposal against this project rather than product work:

- **Automatic firmware updates**, the device polls a JSON manifest, checks the advertised version, chip family and flash offset against its own build info, and starts an OTA with SHA-256 and expected-size verification. Manifests over plain HTTP or without a declared size are refused.
- **BLE WiFi provisioning**, credentials over Bluetooth using Espressif's `wifi_provisioning` component, gated to run only while the device is in access-point fallback, in its own firmware variant (it costs roughly 320 KB of flash).
- **WiFi reconnect handling**, a debounce before applying typed credentials, and repeated station retries before falling back to an access point.
- **RMT LED output over DMA**, enables the DMA backend for WS2812 transmission on chips that have it, with a completion callback instead of a blocking wait, so a delayed interrupt under network load cannot stretch a bit cell into visible flashing.
- **RGBW color correction**, a wider set of channel-order presets, and an explicit white channel taken from the source when the layer carries one.
- **Frame pacing**, a target-frame-rate cap on the scheduler, work-time metrics, and a periodic-tick sweep spread across ticks rather than run in one burst.

- **Branches:** `codex/upstream-auto-update-manifest` (07-11), `codex/upstream-network-sta-reconnect` (07-10), `codex/upstream-rmt-rgbw-performance` (07-10), `codex/performance-frame-pacing` (07-11), `codex/universal-ble-provisioning` (07-13), `codex/helio-private-wip` (07-10, initial WIP), `next-iteration` (07-10, a carry-forward of our upstream branch). `main` last moved 2026-07-09.

_Checked: commits on `main` for author-date 2026-07-01..2026-08-01 (2, both upstream carry-forward); commits on all branches vs `MoonModules/MoonLight@main` for the same window (11); releases published (none); issue search `repo:Funkelfetisch/MoonLight is:issue created:2026-07-01..2026-07-31` and the same with `closed:` (0 results each)._

## June 2026

- **Gyro / IMU input**, an MPU6050 accelerometer and gyroscope read over I2C, publishing angular rate plus pitch and roll as read-only values, with a desktop simulation so the interface shows live numbers without the hardware attached.

- **Branches:** `feature/gyro_module` (06-05). No other branch activity.

_Checked: commits on all branches vs `MoonModules/MoonLight@main` for author-date 2026-06-01..2026-07-01 (1); releases published (none); issue search `repo:Funkelfetisch/MoonLight is:issue created:2026-06-01..2026-06-30` and the same with `closed:` (0 results each)._
