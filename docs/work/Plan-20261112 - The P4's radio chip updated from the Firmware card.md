# Plan: the P4's radio chip updated from the Firmware card

An ESP32-P4 with WiFi carries its radio on a companion ESP32-C6, which runs its own ESP-Hosted firmware over SDIO. That firmware must match the host's ESP-Hosted release on major.minor, or remote calls time out. This plan lets a user see the C6's firmware and update it from the Firmware card, over the link the two chips already share.

Absorbs the card "ESP32-P4 WiFi through the C6 co-processor", deleted once absorbed, its text in git through the backlog mirror: steps 1 to 3 update the C6 from the Firmware card, steps 4 to 7 are what the card still held once the C6 ran a matching release.

## The design basis

- **One version pin drives both chips.** The C6 image is built from the `slave/` project inside the same `espressif/esp_hosted` component the P4 firmware links, so a release's two images match by construction. Moving the pin in [esp32/main/idf_component.yml](../../esp32/main/idf_component.yml) moves both.
- **A release asset, not an embedded image.** The C6 image, about 1.3 MB, ships beside the firmwares as `shared-c6-slave-<esp_hosted version>.bin`. Embedding it would cost every P4 update a megabyte for a chip that rarely changes.
- **The existing update path, one more target.** `platform::OtaTarget` gains `Coprocessor` beside `App` and `MoonBase`: the URL fetch and the upload stream into it as they stream into the others, and `otaBegin`, `otaWrite` and `otaEnd` call ESP-Hosted's `esp_hosted_slave_ota_begin`, `_write`, `_end` and `_activate`.
- **One home for the C6's identity.** The status line moves from the System card (`wifiCoproc`) to a WiFi coproc tab on the Firmware card, beside MoonBase's tab.

## What exists

- **The status line** ([coprocessor_status.h](../../src/platform/coprocessor_status.h)): link not started, link down, the reported version, or too old to report; on the System card as `wifiCoproc`, pinned by [unit_coprocessor_status.cpp](../../test/unit/core/unit_coprocessor_status.cpp).
- **The Firmware card's tabs** ([FirmwareUpdateModule.h](../../src/core/system/FirmwareUpdateModule.h)): `App` and `MoonBase` through the `image` select, with install from a release, a URL or a file.
- **Proven on the bench (2026-11-12):** .139's C6 went from 0.0.0 to 2.12.13 through Espressif's `host_performs_slave_ota` example, 1.28 MB in 12.6 s over SDIO, the link up in 3.5 s afterwards instead of 10.9 s. A 0.0.0 C6 accepts begin, write and end; activate needs 2.6.0 or newer.

## Steps

### 1. The C6 image in every release 🧪 the asset on the next pre-release

- **Build:** the release workflow builds the `slave/` project of the managed `esp_hosted` component for `esp32c6`, after a P4 firmware build has fetched the component, and stages it as `shared-c6-slave-<version>.bin`, the version read from the component's `idf_component.yml`.
- **Locally:** a MoonDeck script builds the same image, so a bench board updates before a release has one.
- **Verified by:** the asset on a pre-release, its app descriptor naming `network_adapter` at the host's version.
- **Built (2026-11-12).** [coprocessor_image.py](../../moondeck/build/coprocessor_image.py) builds a copy of the fetched component's `slave/` into `build/coprocessor-esp32c6/shared-c6-slave-<version>.bin`, named so the release's `shared-*.bin` upload carries it; the two P4 WiFi variants name `coprocessor: esp32c6`, so `build_esp32.py` builds it after the app, and `release_stage.py` ships it. Built locally: `c6-slave-2.12.13.bin`, 1252 KB.

### 2. The C6 as an update target ✅

- **Platform:** `OtaTarget::Coprocessor`. Its begin starts the link where it was never started (`esp_hosted_connect_to_slave()`, since only `esp_wifi_init` starts it and Ethernet never calls that) and never reconnects a live link, which resets the C6 and fails. It checks the image's descriptor names `network_adapter`, refusing another chip's image before a byte is sent. Its end activates where the C6 is 2.6.0 or newer, and restarts the board so both chips resync.
- **Off the render thread:** starting the link waits out up to 20 s on a C6 that never answers, and each chunk is a remote call of about 10 ms. Both run in the task the URL fetch already uses; how an upload reaches that task is decided in the step.
- **Desktop:** a stand-in target the tests drive, as the MoonBase target has one.
- **Tests:** an image for another chip is refused; the link starts only when it was never started; a refused write abandons the image and the card says why.
- **Built (2026-11-12).** `OtaTarget::Coprocessor` in [platform_esp32_ota.cpp](../../src/platform/esp32/platform_esp32_ota.cpp), writing in 1500-byte pieces; the upload needs the link up, the URL install starts it on its task and restarts after. `coprocessorRejection` in [FirmwareImage.h](../../src/core/util/FirmwareImage.h) refuses another chip, another project, or another ESP-Hosted major.minor, pinned in unit_FirmwareImage. One URL handler for MoonBase and the C6. A desktop stand-in target was not needed: the rules it would drive are pure and tested directly.

### 3. The WiFi coproc tab on the Firmware card ✅

- **The tab:** on every build with a radio chip, Ethernet included, since the update itself starts the link. It shows the status line, the host's ESP-Hosted version, and install from the release's C6 asset or from a file.
- **The offer:** when the C6's major.minor differs from the host's, or it is too old to report, the tab warns, as MoonBase's tab does on a version mismatch.
- **The move:** the line leaves the System card; a MIGRATING entry names the new place.
- **Verified by:** .132, whose C6 has never connected on the bench, updated from the tab over Ethernet; .139 re-sent the same image.
- **Built (2026-11-12).** The Firmware card's tabs follow the device's `image` options; the WiFi coproc tab, named C6 when first built, shows `version`, `needs` and `connect`, warns on a settled mismatch, and installs from the release, a URL or a file. On .132 (WiFi): tabs App and C6, the warning, `C6 too old to report`, `ESP-Hosted 2.12.13`.
- **Verified (2026-11-12)** by the product owner on .132: `c6-slave-2.12.13.bin` installed from a file on the tab, the device restarted, the tab reads the new version and the warning is gone. Install from the release waits on step 1's asset.

### 4. A dead co-processor link does not abort the board ⏳

- **The defect:** on a board whose C6 never completes the handshake (the MHC-WLED P4 shield, chip rev v1.0, 2026-09-08), the WiFi cascade walks into `esp_wifi_init`, which waits out esp_hosted's slave-ready loop, up to about 20 s with a slave reset every 10 s, on the render thread and past the 5 s watchdog: a reboot every ~22 s.
- **Ours, twice:** the cascade does not consult the link, and the forwarded calls run on the render thread. The signal exists now: `coprocessorLinkUp()`, esp_hosted's transport state. A gate tried in September failed on its two signals, the TRANSPORT_UP event, posted before the firmware's event loop exists, and the version query.
- **Meanwhile:** such a board runs the eth-only image, or its C6 gets ESP-Hosted over its own UART, since an update over the link needs the link.
- **Verified by:** the shield on eth-wifi without Ethernet, running and saying the WiFi coproc does not answer instead of rebooting.

### 5. Association on every boot ⏳

- **The question:** the intermittent association on IDF v6.1 (2026-10-12) predates the C6 update; one boot on each NANO is not every boot.
- **Verified by:** ten cold boots of a NANO with C6 2.12.13 on WiFi only, each associating.

### 6. Teardown and re-init on a co-processor board ⏳

- **The path:** `wifiStaStop()` calls `esp_wifi_deinit()`, and the next start runs `esp_wifi_init()`, which reconnects the transport. ESP-Hosted 2.12.13 skips the slave reset on a live link, where older releases paid one; a user on another codebase fixed the mirror symptom by never tearing the stack down on a co-processor board.
- **First:** test STA retry and the access-point fallback on a NANO with C6 2.12.13, then change only what fails.

### 7. The WiFi tail ⏳

- **Measured (2026-11-12, 64 lights):** 2 of 90 fetches past 100 ms (106 and 186 ms) over WiFi, where native-radio boards had none; the link's traffic costs the render loop about 0.4 ms a frame (454 fps on Ethernet, 384 on WiFi).
- **Stop rule:** the tail matches a native-radio board's within the run-to-run spread, or a measurement names the cause and its fix costs less than the 0.4 ms it buys back.

## Backlog

- **Recovery over the C6's own UART,** for a C6 whose link never comes up: a hardware path, documented rather than built.

## What it removes

- The status line on the System card, which moves.
- The card's flashing steps through an ESP-Prog for a C6 whose link is up, which the tab replaces.
