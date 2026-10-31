# Build system

How the source tree maps onto CMake, where each build writes, and the facts a build depends on: the firmware variants and the pinned ESP-IDF. The steps to build are [MoonWalk's contributor processes](../moonwalk.md#desktop-build-test); this is what to look up.

CMake is the sole build system. The source tree is shared across every platform, but build entry points are separate because ESP-IDF wraps CMake with its own conventions (`idf_component_register()` instead of `add_library()`).

```text
CMakeLists.txt                          ← standard CMake: desktop / RPi + tests
src/
  main.cpp                              ← shared pipeline wiring (mm_main), platform-neutral
  platform/
    desktop/
      main_desktop.cpp                  ← desktop entry point: int main() + SIGINT and SIGTERM
      platform_config.h                 ← desktop platform constants
    esp32/
      platform_config.h                 ← ESP32 platform constants (reads sdkconfig)
esp32/
  CMakeLists.txt                        ← ESP-IDF project root (thin wrapper)
  main/
    CMakeLists.txt                      ← idf_component_register() pointing at src/
    main.cpp                            ← ESP32 entry point: app_main() + NVS init
  sdkconfig.defaults                    ← board-specific defaults
```

The shared `src/main.cpp` defines `mm_main(keepRunning, httpPort)`, the full pipeline wiring. Each platform provides a thin entry point that does platform-specific init (SIGINT and SIGTERM on desktop, NVS on ESP32; the network starts in the platform layer) then calls `mm_main()`.

The project is a small set of CMake libraries. A core library is platform-independent, a platform library is selected at configure time, and the application target links both and provides the entry point. Further decomposition (effects, networking, drivers as separate libraries) happens when the codebase is large enough to justify it.

Which script runs which build: [MoonDeck.md](../../moondeck/MoonDeck.md). Why the tooling is ours rather than PlatformIO: [Why we write our own code](../explanation/why-we-write-our-own.md).

## Build directories

Each host writes into its own build directory: `build/macos/`, `build/linux/` or `build/windows/`. Each ESP32 firmware writes into `build/esp32-<firmware>/`. One directory per target means nothing clobbers another on a machine that builds several. Switching firmware is a different `--firmware` argument with no clean rebuild, and a rebuild of the same firmware stays incremental. Each firmware's directory takes about 100 MB; `moondeck/build/clean_esp32.py --firmware <name>` or `--all` removes them.

A firmware key that changes its feature set would otherwise keep its old `MM_NO_ETH` or `MM_ETH_ONLY` in `CMakeCache.txt`. CMake writes a `-D` flag to the cache, and omitting it later does not clear it. `build_esp32.py` compares the cached feature flags with what the firmware wants and wipes the directory for a clean configure on a mismatch, printing the reason.

On Windows the default generator is Visual Studio multi-config, so `MoonLight.exe` lands at `build/windows/Release/MoonLight.exe` and `mm_scenarios.exe` under `build/windows/test/Release/`. `build_desktop.py` and `run_scenario.py` look in both the `Release/` folder and the build root, so Ninja works as well.

## The compiler cache

Every build uses [ccache](https://ccache.dev) when it is installed, and builds as before when it is not. A compile whose source, headers and flags match an earlier one is read back from the cache. That covers a rebuild after a clean, the same file in another ESP32 firmware, and a branch switched back to. ESP-IDF turns on its own ccache switch from `esp32/CMakeLists.txt`, and the desktop and Teensy builds set it as the compiler launcher. ESP-IDF's installer brings ccache on Windows; the Windows desktop build, on the Visual Studio generator, runs without it. `ccache -s` shows how often it hits. CI keeps one cache per build job through `.github/actions/ccache`. The version, git hash and date change with every commit, so `build_info.cpp` alone holds them, and a new commit recompiles only what it changed.

## Where the desktop keeps its settings

A source checkout writes to `build/fs/`, its config under `build/fs/.config/`, recognized by `CMakeLists.txt` and `moondeck/` both being in the working directory. A development tree stays self-contained and ignored by git, and the File Manager shows what a board shows rather than build output. An installed or unzipped binary writes to the per-user data folder its OS provides, listed under [Install MoonLight](../moonwalk.md#install). `MM_DATA_DIR` overrides both, which is how the tests keep their files inside the build tree.

A shipped binary starts from a download folder or a Start-menu shortcut, where a path relative to the working directory is unwritable or belongs to that folder. The root is created when the filesystem mounts, and a location that cannot be written fails the mount and is reported once, rather than failing every save.

## Generated before compiling

CMake runs these when their sources change; the generated files are ignored by git and rebuilt on every clean build. Both are defined in the root `CMakeLists.txt` (desktop) and `esp32/main/CMakeLists.txt` (ESP32).

| Step | Source | Generated |
|---|---|---|
| `build_info_gen` | `library.json` and the git hash | `src/core/util/build_id.h` |
| `ui_embed` | every file in `src/ui`, bundled and minified by `moondeck/build/bundle_ui.py` with a pinned esbuild it fetches into `build/tools` | `src/ui/ui_embedded.h` |

## Firmware variants

The canonical list is the firmware registry, [`moondeck/build/firmwares.py`](../../moondeck/build/firmwares.py). It joins the `FIRMWARES` dicts in [`build_esp32.py`](../../moondeck/build/build_esp32.py) and [`build_teensy.py`](../../moondeck/build/build_teensy.py) and tags each row with its `platform`; an ESP32 row also keeps its sdkconfig `fragments`. Its machine-readable projection is [`mooninstaller/firmwares.json`](../../mooninstaller/firmwares.json), written by `generate_firmwares.py` and guarded by `check_firmwares.py`. The release matrix, the manifests, the Teensy builds and MoonDeck all read it. A row with `ships: false` builds from the command line but stays out of the release. ("Firmware" is the compiled binary; the physical product is a deviceModel, see [the three words](../explanation/architecture/mooninstaller.md#the-three-words).)

A key combines the chip, its feature flags and, for a chip whose modules differ, the module. Each ESP32-S3 module has its own key, since the sdkconfig fragment encodes flash size, partition table and PSRAM mode. An `n16r8` binary on an N8R2 module misaligns the partition table or fails PSRAM init. A new module is a new key; there is no generic `esp32s3`. `--profile default` and `--profile eth-only` still map to `--firmware esp32` and `--firmware esp32-eth`.

ESP-IDF v6 has no `CONFIG_ESP_WIFI_ENABLED` switch, so a firmware without WiFi excludes it through `EXCLUDE_COMPONENTS` plus `MM_NO_WIFI`, set when `MM_ETH_ONLY=1` in `esp32/main/CMakeLists.txt`. `esp32-eth` takes that path; `esp32` keeps both stacks and lets `NetworkModule` try Ethernet first and fall back to WiFi.

The Ethernet PHY and its pins are runtime settings. Each firmware carries the drivers its chip can host: RMII EMAC on the classic and the P4, W5500 over SPI on the S3. `deviceModels.json` supplies a board's PHY and pins at provisioning. The classic default is the common LAN8720 wiring: reset GPIO 5, MDIO address 0, clock GPIO 17, as on the Olimex ESP32-Gateway. A board with the same PHY on other pins needs another `deviceModels.json` entry, not another build.

## MoonBase and the partition layout

Some variants carry [MoonBase](../explanation/architecture/moonbase.md), the second boot image: `esp32`, `esp32-eth`, `esp32-wrover`, `esp32-pico`, `esp32-16mb`, `esp32s3-zero`, `esp32s3-n8r2`, `esp32c3`, and `qemu`, which is emulated. On those, `flash_esp32.py` writes the whole layout in one pass: the app in the large `ota_0` slot, MoonBase in `factory`, and an otadata that boots the app. A network update writes the app and never the partition table, so a device on an older table adopts a new one only through a full serial flash. On the 4 MB classic that also moves the filesystem, and the device comes back unprovisioned.

## ESP-IDF version

**Pinned to `v6.1`**, commit `fff9895c`, the GA tag. `setup_esp_idf.py` holds it in `PINNED_IDF_VERSION`, warns when the installed tree differs, and offers to check the pin out. A stray `git pull` or a fresh clone on a newer commit then converges back; `--no-checkout` keeps it a warning. CI builds against the matching `espressif/idf` Docker tag. The firmware uses v6 APIs (`esp_eth_phy_new_generic`, the component manager for mDNS, the RMT v2, parlio and LCD drivers), and C++20 needs GCC 12 or newer.

**Why v6.1.** v6.1 (GA 2026-08-25) carries driver fixes for the newer chips (P4 parlio, RMT v2 everywhere) and is the first line with the `esp32s31` target. The pin is a signed tag rather than the moving `release/v6.1` branch, so a pull cannot shift it under a build. A GA tag also starts the 30-month support window, which a pre-release never has. Moving the pin is a deliberate re-test pass: the move to GA was validated on all four chip families with the full device scenario set (2026-09-29).

**v6.0 is the floor.** v6.0 stable is the fallback if the v6.1 line misbehaves, so the firmware and the tooling stay buildable on it. They use no IDF API, component, Kconfig symbol or tool that v6.0 lacks. An exception needs the product owner's explicit decision, recorded where it is introduced (the module spec, a comment at the dependency, the commit body), with what the fallback loses. Two stand, each for one target and tracked in the [backlog](https://github.com/users/MoonModules/projects/5). **P4 WiFi over the C6 co-processor** needs `esp_wifi_remote` and esp-hosted, managed components outside v6.0. The **ESP32-S31** exists only from v6.1, so it has no v6.0 to fall back to.

**Where the change was.** v5 to v6.0 was the large step. The legacy peripheral drivers, RMT among them, were removed, so the LED drivers use the RMT v2, parlio and `esp_lcd` APIs. picolibc replaced newlib, warnings became errors by default, and `CONFIG_ESP_WIFI_ENABLED` went, hence the `EXCLUDE_COMPONENTS` path above. v6.1 is an ordinary minor on that baseline.

**Support.** Each stable ESP-IDF release is supported for 30 months from GA, a service period of frequent bugfix releases and then a maintenance period of security and severe fixes only. Check for a newer one on the [releases page](https://github.com/espressif/esp-idf/releases), or with `git -C ~/esp/esp-idf fetch --tags && git -C ~/esp/esp-idf tag -l 'v6.*'`. The installed tree is `git -C ~/esp/esp-idf describe --tags`, and the upstream [ROADMAP.md](https://github.com/espressif/esp-idf/blob/master/ROADMAP.md) has each minor's dates. Moving the pin is [Bump the ESP-IDF pin](../moonwalk.md#bump-idf).

### The v6 ecosystem

Anything the IDF makes the recognized way to build, install, provision or ship is the path to take rather than a bespoke one. It is adopted step by step, each its own commit and hardware re-test, and only once it clears the v6.0 floor. None may leak an IDF assumption into shared `src/` or the desktop and Teensy builds. The plan per item is in [backlog/platform](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085832).

| Change | Where MoonLight stands |
|---|---|
| **EIM**, the ESP-IDF Installation Manager, the new default installer | `setup_esp_idf.py` drives the older `install.sh` and `install.bat`. |
| **PSA Crypto** replacing the legacy mbedTLS APIs | No direct exposure: OTA uses `esp_https_ota` and `esp_crt_bundle_attach`, which wrap crypto. |
| **`network_provisioning`**, Espressif's provisioning over BLE and SoftAP with phone apps | Provisioning is [Improv](../moonmodules/core/system.md#improv-provisioning) from the browser or a serial command. The phone-app flow is a coverage gap rather than a duplicate. |
| **CMake Build System v2**, a preview in v6.0 and v6.1 | The v1 `idf.py` build; the component is a thin `idf_component_register()`, so the migration is small. |
| **The built-in MCP server** (`idf.py mcp-server`) | Not used: agents and people go through MoonDeck's scripts, which add the pin check, the per-firmware directories and the measurements. |

## Source

[CMakeLists.txt](https://github.com/MoonModules/MoonLight/blob/main/CMakeLists.txt) is the desktop and test build; [esp32/CMakeLists.txt](https://github.com/MoonModules/MoonLight/blob/main/esp32/CMakeLists.txt) is the ESP-IDF project root.
