# Firmware variants and memory

What each firmware build contains, and what each class of device can run. Facts to look up; the reasoning behind them is in [the architecture](../../explanation/architecture/index.md).

## Firmware variants

Selected by `build_esp32.py --firmware <key>`, reported by `SystemModule.firmware`, and the contract target key in scenarios. A firmware exists per thing the silicon forces: the chip, the P4's revision line, the S3's PSRAM interface and octal flash, and the PSRAM cache workaround of a classic revision 0 or 1. Everything a board can tell about itself is read at run time: its flash size picks the partition layout, and the PSRAM it finds joins the heap. Each chip's firmware carries the Ethernet driver it can host (RMII EMAC for classic and P4, W5500 SPI for S3); which PHY and pins a device model uses is runtime configuration.

| Variant | Chip | For |
|---|---|---|
| `esp32` | classic, more than 4 MB flash | Every feature, WiFi and RMII Ethernet; PSRAM used on revision 3 and up |
| `esp32-lite` | classic, 4 MB flash | [The 4 MB tier](#the-4-mb-tier-lite); PSRAM used on revision 3 and up |
| `esp32-ethonly` | classic, any flash | Ethernet only: WiFi compiled out, so every feature fits 4 MB and more RAM stays free |
| `esp32-rev1-lite` | classic revision 0 and 1 with PSRAM | The PSRAM cache workaround, PSRAM used; the 4 MB tier on every layout |
| `esp32s3-qspi` | S3 with quad PSRAM, more than 4 MB flash | Every feature, WiFi and W5500 SPI Ethernet |
| `esp32s3-qspi-lite` | S3 with quad PSRAM, 4 MB flash | The 4 MB tier, WiFi |
| `esp32s3-opi` | S3 with octal PSRAM, 8 MB flash and up | Every feature, WiFi and W5500 SPI Ethernet |
| `esp32s3-opi-oflash` | S3 with octal PSRAM and octal flash (WROOM-2 N32R16V) | As `esp32s3-opi`; GPIO 35 to 37 belong to the octal flash and PSRAM |
| `esp32c3` | C3, 4 MB flash | WiFi |
| `esp32p4rev1`, `esp32p4rev3` | P4 by revision line, an image each | Ethernet, and WiFi over the on-board ESP32-C6; a board lacking a C6 switches the radio off on its WiFi card |
| `esp32s31` | S31 | WiFi 6 and gigabit Ethernet |

**The naming rule**: the chip, then the revision line where builds differ by it (`p4rev1`, `p4rev3`, `-rev1`), the PSRAM interface where the chip has more than one (`qspi`, `opi`), `-oflash` for octal flash, the tier `-lite`, and `-ethonly`. A plain name carries everything its family can do.

**The installer picks** the firmware from what it measures when no device model is chosen: chip, revision, flash size and, on an S3, the PSRAM in the package. A chosen device model's firmware is checked against the same measurement. A running device suggests the firmware its own facts name on its Firmware card.

**Release assets** are named `moonlight-<variant>-v<version>.bin`. A device from before these names asks for `firmware-<its old name>-v<version>.bin`, which every release also carries ([MIGRATING](../MIGRATING.md)).

**A device names its update** from its compiled-in firmware name, `firmware-<name>-v<version>.bin` for a device from before the `moonlight-` names, used by the Firmware page, MQTT and Home Assistant ([FirmwareUpdateModule.h](../../../src/core/system/FirmwareUpdateModule.h)). The Firmware page offers the firmwares compatible with the running one (`isCompatible` in [install-picker.js](../../../src/ui/install-picker.js)), and MoonBase offers the app's own name. An over-the-air install checks the image's project name and chip ([FirmwareImage.h](../../../src/core/util/FirmwareImage.h)) and not its firmware name, so a renamed image of the same chip installs. An app image runs under every layout of its family, since the flash size it sees comes from the bootloader header the layout is installed with.

### PSRAM by revision

A build fixes the PSRAM mode; ESP-IDF detects the size at boot. A classic ESP32 firmware starts PSRAM in two stages. The hardware stage finds the chip by the package's pins (`CONFIG_SPIRAM_BOOT_HW_INIT`, with `CONFIG_SPIRAM_IGNORE_NOTFOUND`, so a PICO-D2 that has no PSRAM reports none and boots on). The heap stage is MoonLight's own startup hook, registered right after ESP-IDF's (`CONFIG_SPIRAM_BOOT_INIT=n`), which calls `esp_psram_init()` and adds the PSRAM to the heap on revision 3 and up, and always in `esp32-rev1-lite`. The rule is a pure function of the revision and whether the build carries the workaround, pinned by a host test per case. A revision 0 or 1 board on `esp32-lite` finds its PSRAM and leaves it unused, and its Firmware page offers `esp32-rev1-lite`.

- **Revision 0 and 1 corrupt PSRAM accesses through the cache**, revision 3 fixed it. Classic builds support revision 0 and up (`CONFIG_ESP32_REV_MIN_0`), so `esp32-rev1-lite` carries `-mfix-esp32-psram-cache-issue`, which adds wait instructions after memory accesses in all code.
- **The 4 MB layout's app slot is 2496 KB**, and an installed device keeps its slot. App images (7.1.0-dev.170): `esp32` 2327 KB, `esp32-lite` 2336 KB, `esp32-rev1-lite` 2384 KB; IRAM use 63 %, 68 % and 72 %, since the PSRAM driver moves about 18 KB of code into IRAM.
- **Speed**: on an Olimex ESP32-Gateway the frame time is the same within 0.3 % across `esp32`, `esp32-lite`, `esp32-rev1-lite` and `esp32-ethonly` (111 fps, 8.9 ms); the Effects time moves about 15 % between runs of one build, so the workaround's cost does not show at that load. `esp32-ethonly` leaves 20 KB more heap free (145 KB against 125 KB).
- **A board that has no PSRAM boots on a PSRAM build** through two settings: `CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC`, since ESP-IDF defaults mbedTLS to PSRAM only with SPIRAM on and `psa_crypto_init` then fails; and the hook's `psramPadsFreedWhenAbsent`, since the probe reserves its pins before the ID read and keeps them when none answers, which cost a board its GPIO 16 and 17.

### The 4 MB tier (`-lite`)

A 4 MB board has one 2496 KB app slot beside MoonBase. What does not fit beside everything else stays out of the `-lite` firmwares, behind one build setting, `CONFIG_MM_LITE`:

- **The SD card** (the FatFs backend), on every chip.
- **USB MIDI host**, on the S3, and **the audio codecs** (ES8311, ES7210), on the S3 and the classic ESP32.

A device model that names one of these keeps working on `-lite`: a state document skips a type the firmware leaves out and names it.

## Scaling to available memory

| Device | Memory | Typical capability |
|---|---|---|
| ESP32 + OPI PSRAM | 2-8 MB | Many layers, 10K+ LEDs |
| ESP32, no PSRAM | ~320 KB internal | Full pipeline: double buffering, mapping, blending, parallelism. Proven up to 16K lights (128x128 measured live on an Olimex Gateway) |
| Teensy 4.x | 1 MB internal, no PSRAM | Comfortable headroom for several layers; DMA-based LED output (OctoWS2811). Ethernet built-in on 4.1 |
| Host / RPi | Abundant | No constraints |

## Degradation cascade

When memory is short the pipeline steps down, best to worst. Each step is observable via `lutSkipped()` and reported per module in `/api/system`.

1. **Full pipeline**: LUT plus driver output buffer. Modifiers applied, clean separation.
2. **Skip LUT and driver buffer**: modifiers not applied, forced 1:1 mapping. A LUT needs a driver buffer to map into, so the two go together.
3. **Reduce layer dimensions**: halve width and height until the buffer fits, minimum 8x8.
