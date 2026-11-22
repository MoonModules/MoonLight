# Plan: one firmware per chip and PSRAM mode

Plan of 2026-11-21, a Restructure. **Goal: every board runs the right firmware, with every feature it has today and nothing slower, and no install path guesses.** What a board can tell is read from the board: its flash size sets the layout at install, its PSRAM is found at boot, the P4's WiFi coprocessor is used when it answers. A firmware exists only where the silicon forces it, plus one tier for small flash. The ESP32 firmwares go from 16 to 12.

The topic started from the IoTorero LS-3PS (an ESP32-PICO-D2, 4 MB, no PSRAM), which runs `esp32` while a firmware named `esp32-pico` exists for a different chip: the name said the package, where the build depends on memory.

## What exists

The 16 ESP32 firmwares, with their app images as built on the dates shown:

| Firmware | Flash layout | PSRAM | Radios | App image |
|---|---|---|---|---|
| `esp32` | 4 MB with MoonBase | off | WiFi + RMII Ethernet | 2327 KB (11-20) |
| `esp32-eth` | 4 MB with MoonBase | off | RMII Ethernet | 1899 KB (11-18) |
| `esp32-16mb` | 16 MB with MoonBase | off | WiFi + RMII Ethernet | 2210 KB (10-30) |
| `esp32-wrover` | 4 MB with MoonBase | quad | WiFi + RMII Ethernet | 2144 KB (10-30) |
| `esp32-pico` | 8 MB with MoonBase | quad | WiFi + RMII Ethernet | 2494 KB (11-19) |
| `esp32s3-zero` | 4 MB with MoonBase | quad | WiFi | 2075 KB (10-30) |
| `esp32s3-n8r2` | 8 MB with MoonBase | quad | WiFi | 2194 KB (10-30) |
| `esp32s3-n8r8` | 8 MB dual OTA | octal | WiFi + W5500 | 2300 KB (10-30) |
| `esp32s3-n16r8` | 16 MB dual OTA | octal | WiFi + W5500 | 2541 KB (11-20) |
| `esp32s3-n32r16` | 32 MB dual OTA, octal flash | octal | WiFi + W5500 | 2564 KB (11-20) |
| `esp32c3` | 4 MB with MoonBase | none | WiFi | 2440 KB (11-19) |
| `esp32p4rev1-eth`, `-eth-wifi`, `esp32p4rev3-eth`, `-eth-wifi` | 16 MB | on | Ethernet, optional C6 WiFi | 2554 to 2883 KB |
| `esp32s31` | 16 MB | on | WiFi 6 + Ethernet | 2777 KB (11-18) |

The facts about the build, the PSRAM mode, the slot, the update names and the installer's choice are in [firmware variants](../reference/hardware/firmware-variants.md), [PSRAM by revision](../reference/hardware/firmware-variants.md#psram-by-revision), [the build system](../reference/build-system.md#firmware-variants) and [MoonInstaller](../explanation/architecture/mooninstaller.md#one-manifest-a-writer-per-chip-family).

## End states

| | End state | Firmwares | Gains | Loses |
|---|---|---|---|---|
| A | Rename only, by Espressif's ordering code (`esp32-n8r2`) | 16 | Names say the memory | The PSRAM size in a name is a fact the build does not fix; a board on the wrong build still ignores its PSRAM |
| B | PSRAM found at boot, the classic builds split by revision, flash size and radios | 16 | Every board uses its PSRAM | As many firmwares as today, names that multiply by every axis |
| C | What the board can tell is read at run time; firmwares only where the silicon forces it, plus a tier for 4 MB | 12 | One rule, few firmwares, nothing guessed, nothing slower | The most work; every board carries the PSRAM code |

**The product owner picked C** on 2026-11-21.

**C is Tasmota's model**: one generic image per chip, PSRAM used when found, a recovery partition (Tasmota's `safeboot`, MoonLight's MoonBase), the P4 split by revision (`tasmota32p4`, `tasmota32p4r3`), a separate build carrying the PSRAM cache fix ("PSRAM with fix"), and named builds for a smaller feature set (`tasmota32-lite`). It goes further in two places: the layout follows the flash measured at install, where Tasmota's Partition Wizard resizes after install, and the octal S3 has a prebuilt image, where Tasmota's generic S3 is quad only.

**WLED-MM, the other prior art**: its nightly ships 34 binaries, multiplied by chip, flash, PSRAM mode, feature set (`_S`, `_M`), IDF line, `_eth` (Ethernet added beside WiFi), `_HUB75` and named boards (`athom_music_esp32_4MB_V4_M`, `esp32_pico_4MB_V4_S`), whose pins, microphone and limits are compile-time defines (`-D LEDPIN=2`, `-D I2S_SDPIN=25`). MoonLight keeps that board knowledge as data in `deviceModels.json`, applied as state at install, so a new board adds a catalog entry and the firmwares follow the silicon alone.

## The rule and the firmwares

Built as described in [firmware variants](../reference/hardware/firmware-variants.md#firmware-variants): what the board can tell is read at run time (flash size, PSRAM, the P4's coprocessor), a firmware exists per thing the silicon forces, `-lite` is the one tier for 4 MB, `-ethonly` is chosen by the user. The naming rule is [there](../reference/hardware/firmware-variants.md#firmware-variants). The old-to-new name table is in [MIGRATING](../reference/MIGRATING.md#the-firmwares-take-the-names-the-silicon-decides).

**Why `esp32-rev1-lite` carries the 4 MB tier on every layout**: with the SD card the workaround build is 7 KB over the 4 MB slot, and most revision 0 and 1 boards have 4 MB (the product owner, 2026-11-21). **Why `rev` and not Tasmota's `r3`**: R in Espressif's ordering codes is the PSRAM size (N8R2). **Why `-ethonly`**: WLED-MM's `_eth` means Ethernet added beside WiFi.

**New firmware has its own file names**: an installed device asks for `firmware-<name>-v<version>.bin`, and most classic devices are 4 MB boards on `esp32`, so the new firmwares publish as `moonlight-<name>-v<version>.bin` and the old names are served from the old-name table (step 7). Names in the two sets never collide.

## Steps

### 1. Measure `esp32-lite` and `esp32-rev1-lite`, with PSRAM by revision ✅

Both builds measured on the bench and passing the stop rule (same tick time, at least 64 KB free in the 4 MB slot, IRAM room). The rule, the sizes and the two defects only hardware showed: [PSRAM by revision](../reference/hardware/firmware-variants.md#psram-by-revision).

### 2. The P4 WiFi build on a board without a C6 🚧 (the radio switch's esp_hosted shutdown took the Olimex ESP32-P4-PC off the network at every boot on 2026-11-22, the board needing a cable; the shutdown is held back to a no-op until a serial log shows why, so GPIO 14 to 19 stay with the C6 link and the LED test waits for that fix)

Flash today's `esp32p4rev1-eth-wifi` on the Olimex ESP32-P4-PC (192.168.1.221, no C6) with an Ethernet-only device model: it boots, Ethernet works, and the C6's SDIO and reset pins see nothing (whether esp_hosted touches them at boot without a link is the hypothesis to test). Passing, the P4 has one build per revision line, Ethernet and the C6's WiFi when it answers; failing, the P4 keeps an `-ethonly` build per line (14 firmwares).

Open: whether the LED driver still drives GPIO 14 to 19 once esp_hosted has set them up as SDIO at boot (this board's LED pins; [the build system](../reference/build-system.md#the-p4s-radio-chip-image)). Done: it boots and runs on Ethernet at 26 fps with 336 KB internal heap free, where `esp32p4rev1-eth` leaves 376 KB.

### 3. One app across a family's layouts ✅

One app image runs with its full filesystem under each layout of its family, on classic, S3 octal and S3 quad boards: [firmware variants](../reference/hardware/firmware-variants.md#firmware-variants). The S3 quad 8 MB layout is covered by the octal runs both ways, since finding the partitions does not depend on the PSRAM mode.

### 4. Firmwares and layouts as two axes in the build 🚧 (built, see [the build system](../reference/build-system.md#firmware-variants) and [MoonBase and the partition layout](../reference/build-system.md#moonbase-and-the-partition-layout); missing: the P4 on the bench)

`build_esp32.py` defines the 12 firmwares and the layouts separately, MoonBase at every size, one bootloader per chip, `moonlight-` release assets, the P4's radio switch; the removed builds and fragments are listed under [What it removes](#what-it-removes). The octal-flash S3 on the 32 MB layout is verified: the migration image leaves the networks in NVS where the new filesystem overlaps it, and the device's backup restored the other files.

### 5. The installer picks from what it measures ✅

Layout by measured flash, firmware by measurement, a chosen device's firmware checked against it, the migration image picking the largest layout: [MoonInstaller](../explanation/architecture/mooninstaller.md#one-manifest-a-writer-per-chip-family).

### 6. Devices and the map know the new names 🚧 (built: the device's `hardware` facts in `/api/system` and on the map, the System card's `chipPackage`, the Firmware card and the map popup naming the firmware its facts name, MoonCloud charting firmware, revision and package; awaiting: the Firmware card and popup on a device once the installer site serves this release's rule)

Documented in [firmware variants](../reference/hardware/firmware-variants.md#firmware-variants).

### 7. Old names keep updating 🚧 (built: the old-name table, `OLD_NAMES` in release_stage.py, its `old-names` release step, and the device's `assetPrefix`; missing: the OTA runs on hardware, which need a release carrying both name sets)

The table is in [MIGRATING](../reference/MIGRATING.md#the-firmwares-take-the-names-the-silicon-decides). To verify on hardware: an OTA from each old name to the new one, by the Firmware page and by MQTT, on the boards of steps 1 to 3 and the IoTorero LS-3PS (`esp32` to `esp32-lite`). The table's removal is in the Backlog.

### 8. Catalog, docs and tests ✅

The catalog names the new firmwares, and the pages [firmware variants](../reference/hardware/firmware-variants.md), [the build system](../reference/build-system.md) and [MIGRATING](../reference/MIGRATING.md) describe them; the tests pin the PSRAM rule, the naming rule, the old-name table and the installer's choice.

## Backlog

- Remove the old-name table once MoonCloud stats show no device on an old name.
- An S3 board with a separate PSRAM chip, not in the package, has a PSRAM interface the installer cannot read; none is in the catalog.
- Fold `esp32s3-opi-oflash` into `esp32s3-opi`, the 32 MB table becoming one more layout and the `esp32s3oflash` MoonBase family going. ESP-IDF's flash mode auto-detect, on in `esp32s3-opi`, reads the efuse flash type and runs octal flash in OPI mode, and the write path checks the mode at runtime; the price is STR sampling where `CONFIG_ESPTOOLPY_OCT_FLASH` gives DTR, about half the flash read bandwidth on a cache miss, and IDF's help warns that some code still keys on the option. To measure first: `esp32s3-opi` booting, updating over the air and writing files on an octal board, and its fps against `esp32s3-opi-oflash`. The bench's octal board is the Waveshare (.127), on a USB cable for the test. Until then a WLED board on an octal-flash S3 is offered `shared-migrate-esp32s3`, the quad family's image (`devicepopup.js`, keyed by chip), which the fold settles too.
- Host device models in `deviceModels.json`: Raspberry Pi 5 and 4, NanoPi, and a generic macOS, Windows, Linux, Android and Docker, each with its platform's firmware, its install page and the `state` its hardware fixes (a Pi 5 with a HUB75 bonnet bringing the PIO driver's pins). The Device models page then groups by platform, the installer shows what it can flash, and a host's `deviceModel` is a catalog name, MoonWalk's `walkChoices` reading the platform from the entry instead of parsing a `macos-arm64` string.
- A peer on an old firmware name offered its successor in the map popup's install picker: it updates through the `firmware-<old name>` copy the release publishes (`channelUpdate`), and moving it to the successor by name needs `OLD_NAMES` in the registry the UI reads (`mooninstaller/firmwares.json`), one home with release_stage.py (CodeRabbit on #263).

## What it removes

Four firmwares (16 to 12, with step 2 passing), the `firmware-` names (`esp32`, `esp32-eth`, `esp32-16mb`, `esp32-wrover`, `esp32-pico`, `esp32s3-zero`, `esp32s3-n8r2`, `esp32s3-n8r8`, `esp32s3-n16r8`, `esp32s3-n32r16` and the P4's `-eth` and `-eth-wifi` pairs), four sdkconfig fragments, the PSRAM and flash sizes in firmware names, and the installer's offer of every firmware for a chip when no device model is chosen. It adds the layout axis in the build, the PSRAM startup hook, the `moonlight-` file names, and the old-name table, which is temporary.
