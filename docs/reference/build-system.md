# Build system

How the source tree maps onto CMake, where each build writes, and the facts a build depends on: the firmware variants and the pinned ESP-IDF. The steps to build are [MoonWalk's contributor processes](../moonwalk.md#host-build-test); this is what to look up.

CMake is the sole build system. The source tree is shared across every platform, but build entry points are separate because ESP-IDF wraps CMake with its own conventions (`idf_component_register()` instead of `add_library()`).

```text
CMakeLists.txt                          ← standard CMake: host / RPi + tests
src/
  main.cpp                              ← shared pipeline wiring (mm_main), platform-neutral
  platform/
    host/
      main_host.cpp                  ← host entry point: int main() + SIGINT and SIGTERM
      platform_config.h                 ← host platform constants
    esp32/
      platform_config.h                 ← ESP32 platform constants (reads sdkconfig)
esp32/
  CMakeLists.txt                        ← ESP-IDF project root (thin wrapper)
  main/
    CMakeLists.txt                      ← idf_component_register() pointing at src/
    main.cpp                            ← ESP32 entry point: app_main() + NVS init
  sdkconfig.defaults                    ← board-specific defaults
```

The shared `src/main.cpp` defines `mm_main(keepRunning, httpPort)`, the full pipeline wiring. Each platform provides a thin entry point that does platform-specific init (SIGINT and SIGTERM on the host, NVS on ESP32; the network starts in the platform layer) then calls `mm_main()`.

The project is a small set of CMake libraries. A core library is platform-independent, a platform library is selected at configure time, and the application target links both and provides the entry point. Further decomposition (effects, networking, drivers as separate libraries) happens when the codebase is large enough to justify it.

Which script runs which build: [MoonDeck.md](../../moondeck/MoonDeck.md). Why the tooling is ours rather than PlatformIO: [Why we write our own code](../explanation/why-we-write-our-own.md).

## Build directories

Each host writes into its own build directory: `build/macos/`, `build/linux/` or `build/windows/`. Each ESP32 firmware writes into `build/esp32-<firmware>/`. One directory per target keeps each build's files its own on a machine that builds several. Switching firmware is a different `--firmware` argument with no clean rebuild, and a rebuild of the same firmware stays incremental. Each firmware's directory takes about 100 MB; `moondeck/build/clean_esp32.py --firmware <name>` or `--all` removes them.

A firmware key that changes its feature set would otherwise keep its old `MM_NO_ETH` or `MM_ETH_ONLY` in `CMakeCache.txt`. CMake writes a `-D` flag to the cache, and omitting it later does not clear it. `build_esp32.py` compares the cached feature flags with what the firmware wants and wipes the directory for a clean configure on a mismatch, printing the reason.

On Windows the compiler is clang-cl, Visual Studio's Clang toolset, which `build_host.py` and `package_host.py` select with `-T ClangCL`: every host then compiles with Clang or GCC, and the per-light loops vectorize on Windows as they do elsewhere. CMake refuses a build directory configured with another toolset, so `build_host.py` removes one left by an MSVC build and says so. The generator is Visual Studio multi-config, so `MoonLight.exe` lands at `build/windows/Release/MoonLight.exe` and `mm_scenarios.exe` under `build/windows/test/Release/`. `build_host.py` and `run_scenario.py` look in both the `Release/` folder and the build root, so Ninja works as well; a `CMAKE_GENERATOR` naming it gets no toolset and takes its compiler from `CC` and `CXX`.

## The compiler cache

Every build uses [ccache](https://ccache.dev) when it is installed, and builds as before when it is not. A compile whose source, headers and flags match an earlier one is read back from the cache. That covers a rebuild after a clean, the same file in another ESP32 firmware, and a branch switched back to. ESP-IDF turns on its own ccache switch from `esp32/CMakeLists.txt`, and the host and Teensy builds set it as the compiler launcher. ESP-IDF's installer brings ccache on Windows; the Windows host build, on the Visual Studio generator, runs uncached. `ccache -s` shows how often it hits. CI keeps one cache per build job through `.github/actions/ccache`. The version, git hash and date change with every commit, so `build_info.cpp` alone holds them, and a new commit recompiles only what it changed.

## Where the host keeps its settings

A source checkout writes to `build/fs/`, its config under `build/fs/.config/`, recognized by `CMakeLists.txt` and `moondeck/` both being in the working directory. A development tree stays self-contained and ignored by git, and the File Manager shows what a board shows rather than build output. An installed or unzipped binary writes to the per-user data folder its OS provides, listed under [Install MoonLight](../moonwalk.md#install). `MM_DATA_DIR` overrides both, which is how the tests keep their files inside the build tree.

A shipped binary starts from a download folder or a Start-menu shortcut, where a path relative to the working directory is unwritable or belongs to that folder. The root is created when the filesystem mounts, and a location that cannot be written fails the mount and is reported once, rather than failing every save.

## Generated before compiling

CMake runs these when their sources change; the generated files are ignored by git and rebuilt on every clean build. Both are defined in the root `CMakeLists.txt` (host) and `esp32/main/CMakeLists.txt` (ESP32).

| Step | Source | Generated |
|---|---|---|
| `build_info_gen` | `library.json` and the git hash | `src/core/util/build_id.h` |
| `ui_embed` | every file in `src/ui`, bundled and minified by `moondeck/build/bundle_ui.py` with a pinned esbuild it fetches into `build/tools` | `src/ui/ui_embedded.h` |

## Firmware variants

The canonical list is the firmware registry, [`moondeck/build/firmwares.py`](../../moondeck/build/firmwares.py). It joins the `FIRMWARES` dicts in [`build_esp32.py`](../../moondeck/build/build_esp32.py) and [`build_teensy.py`](../../moondeck/build/build_teensy.py) and tags each row with its `platform`; an ESP32 row also keeps its sdkconfig `fragments`. Its machine-readable projection is [`mooninstaller/firmwares.json`](../../mooninstaller/firmwares.json), written by `generate_firmwares.py` and guarded by `check_firmwares.py`. The release matrix, the manifests, the Teensy builds and MoonDeck all read it. A row with `ships: false` builds from the command line but stays out of the release. ("Firmware" is the compiled binary; the physical product is a deviceModel, see [the three words](../explanation/architecture/mooninstaller.md#the-three-words).)

Firmwares and partition layouts are two axes. A firmware exists per thing the silicon forces, as [firmware variants](hardware/firmware-variants.md) lists them; a layout exists per chip and flash size (`LAYOUTS` in `build_esp32.py`). Each firmware names the flash sizes it installs into (`layouts`), and its fragments carry the smallest, since the flash size the app sees comes from the bootloader header. `flash_esp32.py`, the web installer and the migration image all measure the board's flash and write that size's table and bootloader, the bootloader's header and digest made at build time by esptool `merge-bin`. The 4 MB tier is one Kconfig symbol, `CONFIG_MM_LITE`, which each feature that does not fit defaults off under. `--profile default` and `--profile eth-only` map to `--firmware esp32` and `--firmware esp32-ethonly`.

ESP-IDF v6 has no `CONFIG_ESP_WIFI_ENABLED` switch, so an Ethernet-only firmware excludes WiFi through `EXCLUDE_COMPONENTS` plus `MM_NO_WIFI`, set when `MM_ETH_ONLY=1` in `esp32/main/CMakeLists.txt`. `esp32-ethonly` takes that path; `esp32` keeps both stacks and lets `NetworkModule` try Ethernet first and fall back to WiFi.

The Ethernet PHY and its pins are runtime settings. Each firmware carries the drivers its chip can host: RMII EMAC on the classic and the P4, W5500 over SPI on the S3. `deviceModels.json` supplies a board's PHY and pins at provisioning. The classic default is the common LAN8720 wiring: reset GPIO 5, MDIO address 0, clock GPIO 17, as on the Olimex ESP32-Gateway. A board with the same PHY on other pins needs another `deviceModels.json` entry, not another build.

## The P4's radio chip image

A P4 with WiFi carries its radio on an ESP32-C6 running ESP-Hosted, whose firmware must match the host's ESP-Hosted release on major.minor. [`coprocessor_image.py`](../../moondeck/build/coprocessor_image.py) builds the C6 image from the `slave/` project inside the same `espressif/esp_hosted` component the P4 firmware links, so one pin in [`esp32/main/idf_component.yml`](../../esp32/main/idf_component.yml) moves both chips and a release's two images match by construction. A firmware row naming `coprocessor: esp32c6` builds it after the app, into `build/coprocessor-esp32c6/shared-c6-slave-<version>.bin`, and the release ships it beside the firmwares under that name. It is a release asset rather than part of the P4 image, since embedding about 1.3 MB would cost every P4 update a megabyte for a chip that rarely changes. The Firmware card's WiFi coproc tab installs it ([Firmware](../moonmodules/core/system.md#firmware-update)). One P4 build per revision line serves boards that have a C6 and boards that lack one: the link starts only when WiFi is asked for, on its own task, so a C6 that does not answer falls back after about 20 s, and the WiFi card's `radio` switch keeps a board that lacks one off WiFi. ESP-Hosted starts its SDIO link in a boot constructor, so GPIO 14 to 19 are set up as SDIO at every boot, whatever the device model says.

## MoonBase and the partition layout

Every variant carries [MoonBase](../explanation/architecture/moonbase.md), the second boot image, at every flash size: MoonBase in `factory`, one app slot in `ota_0`, and the rest of the flash for files. MoonBase is built once per family, the chip, the P4's revision line and octal flash (`esp32`, `esp32p4rev3`, `esp32s3oflash`), and the device names its family on its Firmware card's MoonBase tab. `flash_esp32.py` writes the whole layout in one pass: the app, MoonBase, and an otadata that boots the app.

<!-- layouts:begin (generated by layouts_markdown() in moondeck/build/build_esp32.py) -->
| Layout | MoonBase | App | Files | Firmwares |
|--|--|--|--|--|
| `4mb_moonbase.csv` | 896 KB | 2496 KB | 548 KB | `esp32-lite`, `esp32-ethonly`, `esp32-rev1-lite`, `esp32s3-qspi-lite`, `esp32c3` |
| `8mb_moonbase.csv` | 896 KB | 3072 KB | 4096 KB | `esp32`, `esp32-ethonly`, `esp32-rev1-lite`, `esp32s3-qspi`, `esp32s3-opi` |
| `16mb_moonbase.csv` | 896 KB | 4096 KB | 11264 KB | `esp32`, `esp32-ethonly`, `esp32-rev1-lite`, `esp32s3-qspi`, `esp32s3-opi`, `esp32p4rev1`, `esp32p4rev3`, `esp32s31` |
| `32mb_moonbase.csv` | 896 KB | 4096 KB | 27648 KB | `esp32s3-opi-oflash` |
<!-- layouts:end -->

A network update writes the app alone and leaves the partition table as it is. A board on an older table moves to its MoonBase layout by a full serial flash, or over the air through the migration image: back up its files on its own page, upload the image as an update, install the app through MoonBase, and restore the files. The image carries the networks, by NVS where the new filesystem overlaps the image as it runs.

## ESP-IDF version

**Pinned to `v6.1.1`**, commit `051e4c4e`, a bugfix release on the v6.1 line. `setup_esp_idf.py` holds it in `PINNED_IDF_VERSION`, warns when the installed tree differs, and offers to check the pin out. A stray `git pull` or a fresh clone on a newer commit then converges back; `--no-checkout` keeps it a warning. CI builds against the matching `espressif/idf` Docker tag. The firmware uses v6 APIs (`esp_eth_phy_new_generic`, the component manager for mDNS, the RMT v2, parlio and LCD drivers), and C++20 needs GCC 12 or newer.

**Why v6.1.** v6.1 (GA 2026-08-25) carries driver fixes for the newer chips (P4 parlio, RMT v2 everywhere) and is the first line with the `esp32s31` target. The pin is a signed tag rather than the moving `release/v6.1` branch, so a pull cannot shift it under a build. Only a GA tag starts the 30-month support window. Moving the pin is a deliberate re-test pass: v6.1 GA was validated on all four chip families with the full device scenario set (2026-09-29), and the bugfix release v6.1.1 on a classic ESP32 with MoonBase (its app and its MoonBase installed over WiFi), an S3 installed over WiFi and a P4 installed over Ethernet and restarted five times (2026-11-16).

**One pinned version.** The firmware and the tooling build with the pinned release alone, as a lockfile pins a dependency, and use whatever it offers. Falling back means moving the pin back, the same re-test pass as moving it forward. The S3 and P4 builds need v6.1.1 for MoonI80's GDMA and LCD calls, the S31 needs the v6.1 line, and P4 WiFi takes `esp_wifi_remote` and esp-hosted as managed components.

**Where the change was.** v5 to v6.0 was the large step. The legacy peripheral drivers, RMT among them, were removed, so the LED drivers use the RMT v2, parlio and `esp_lcd` APIs. picolibc replaced newlib, warnings became errors by default, and `CONFIG_ESP_WIFI_ENABLED` went, hence the `EXCLUDE_COMPONENTS` path above. v6.1 is an ordinary minor on that baseline.

**Support.** Each stable ESP-IDF release is supported for 30 months from GA, a service period of frequent bugfix releases and then a maintenance period of security and severe fixes only. Check for a newer one on the [releases page](https://github.com/espressif/esp-idf/releases), or with `git -C ~/esp/esp-idf fetch --tags && git -C ~/esp/esp-idf tag -l 'v6.*'`. The installed tree is `git -C ~/esp/esp-idf describe --tags`, and the upstream [ROADMAP.md](https://github.com/espressif/esp-idf/blob/master/ROADMAP.md) has each minor's dates. Moving the pin is [Bump the ESP-IDF pin](../moonwalk.md#bump-idf).

### The v6 ecosystem

Anything the IDF makes the recognized way to build, install, provision or ship is the path to take rather than a bespoke one. It is adopted step by step, each its own commit and hardware re-test, on the pinned release. None may leak an IDF assumption into shared `src/` or the host and Teensy builds. The plan per item is in [backlog/platform](https://github.com/MoonModules/MoonLight/issues/220).

| Change | Where MoonLight stands |
|---|---|
| **EIM**, the ESP-IDF Installation Manager, the new default installer | `setup_esp_idf.py` drives the older `install.sh` and `install.bat`. |
| **PSA Crypto** replacing the legacy mbedTLS APIs | No direct exposure: OTA uses `esp_https_ota` and `esp_crt_bundle_attach`, which wrap crypto. |
| **`network_provisioning`**, Espressif's provisioning over BLE and SoftAP with phone apps | Provisioning is [Improv](../moonmodules/core/system.md#improv-provisioning) from the browser or a serial command. The phone-app flow is a coverage gap rather than a duplicate. |
| **CMake Build System v2**, a preview in v6.0 and v6.1 | The v1 `idf.py` build; the component is a thin `idf_component_register()`, so the migration is small. |
| **The built-in MCP server** (`idf.py mcp-server`) | Not used: agents and people go through MoonDeck's scripts, which add the pin check, the per-firmware directories and the measurements. |

## Source

[CMakeLists.txt](https://github.com/MoonModules/MoonLight/blob/main/CMakeLists.txt) is the host and test build; [esp32/CMakeLists.txt](https://github.com/MoonModules/MoonLight/blob/main/esp32/CMakeLists.txt) is the ESP-IDF project root.
