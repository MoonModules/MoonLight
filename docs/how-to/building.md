# Building, running, flashing

How to get the system running on a desktop, an ESP32, a Teensy, or a Raspberry Pi. Design rationale for the choices below lives in [the architecture](../explanation/architecture/index.md); coding conventions in [coding-standards.md](../contributing/coding-standards.md); what is tested in [testing.md](../reference/testing.md).

## MoonDeck — the dev console

Everything that builds, flashes, runs, tests, monitors, or checks the project — for every target — lives as a script under `moondeck/`. The full per-script reference is [moondeck/MoonDeck.md](../moondeck/MoonDeck.md).

The scripts have two front ends with the same code and arguments:

- **CLI** — `uv run moondeck/<group>/<name>.py`. What agents use; what CI uses. Composes with shell, captures exit codes, parses output.
- **MoonDeck** — `uv run moondeck/moondeck.py`, then open `http://localhost:8420`. A browser dev console wrapping the same scripts: status dots, run/stop toggles for long-running processes, grouped tabs, output panes. The human control deck.

Use whichever fits. Neither path is "more official" than the other; the scripts are the source of truth and the front ends are interfaces. New work adds a script first; both interfaces follow.

**Why our own scripts, not PlatformIO:** the ESP32 build is ESP-IDF-native — MoonLight tracks IDF pre-releases against a pinned commit for chips like the P4 and S31, a level of version control PlatformIO's packaged platforms don't offer, and the hot-path drivers (LCD_CAM, Parlio, GDMA) use the vendor APIs first-class rather than through an Arduino-core abstraction. The tooling surface is also far wider than compile-upload-monitor: desktop builds, unit and scenario runs, spec and boundary checks, KPI collection, the web installer, provisioning, multi-board bench orchestration. A wrapper toolchain would cover one of those tasks and still need all the scripts around it; one script per task, two front ends, keeps humans, agents, and CI on the identical path.

MoonDeck has three tabs:

- **Desktop** — build, run, test. Fast iteration.
- **ESP32** — chip type and USB port selection. Build, flash, monitor.
- **Live** — device discovery and monitoring against running devices on the network.

Script definitions and configuration live in `moondeck/moondeck_config.json` (committed). Script documentation lives in `moondeck/MoonDeck.md`, one section per script. Runtime state (selected devices, ports) persists in `moondeck/moondeck.json` (gitignored).

## Desktop / Raspberry Pi

Desktop and RPi both build with the root `CMakeLists.txt`. RPi can cross-compile against the same tree or build natively on the device — same source.

```sh
uv run moondeck/build/build_desktop.py        # build
uv run moondeck/run/run_desktop.py            # run as detached background process
uv run moondeck/test/test_desktop.py          # unit tests
```

Or use MoonDeck's Desktop tab for the same operations with a status dot per card. The desktop run detaches and outlives the launching script — the same model as flashing an ESP32, where the device runs independently afterwards.

![MoonDeck Desktop tab](../assets/ui/moondeck_desktop.png)

Each host writes into its own build dir: `build/macos/`, `build/linux/`, `build/windows/`. The per-host layout mirrors the ESP32 side's `build/esp32-<board>/` shape — one directory per target, no cross-target clobbering on a multi-host dev machine.

### Where the desktop keeps its settings

A **source checkout writes to `build/fs/`** (its config under `build/fs/.config/`), recognized by `CMakeLists.txt` and `moondeck/` both being in the working directory, so a development tree stays self-contained and gitignored. The device's filesystem is that one subdirectory rather than the whole build tree, so the File Manager shows what a board shows instead of build output. Anywhere else, an installed or unzipped binary writes to the OS per-user data directory:

| Platform | Directory |
|---|---|
| Windows | `%LOCALAPPDATA%\projectMM` |
| macOS | `~/Library/Application Support/projectMM` |
| Linux | `$XDG_DATA_HOME/projectMM`, else `~/.local/share/projectMM` |

`MM_DATA_DIR` overrides both, which is how the test suite pins its root into the build tree rather than touching a developer's real settings.

The distinction matters because a shipped binary is launched from a download folder or a Start-menu shortcut, where a path relative to the working directory is either unwritable or belongs to that folder rather than to the user. The root is created when the filesystem mounts, and a location that cannot be written to fails the mount and is reported once, rather than surfacing as a failed save on every change.

### Editor setup (clangd)

Diagnostics appear **as you type**, from the same [`.clang-tidy`](../.clang-tidy) config CI uses, so a finding shows up while the code is still in your head, not ten minutes later in a pipeline.

Once per machine: install the **clangd** extension (`llvm-vs-code-extensions.vscode-clangd`) and **disable Microsoft's C/C++ IntelliSense**, running both produces duplicated and contradictory diagnostics. Nothing else to configure: [`.clangd`](../.clangd) at the repo root points at the compilation database, and `CMAKE_EXPORT_COMPILE_COMMANDS` (set in `CMakeLists.txt`) means any normal build refreshes it.

Two things worth knowing:

- **If every file reports `'cstdint' file not found`**: the build directory was configured
  with a different compiler than clangd is. `.clangd`'s `--query-driver` handles the usual cases; if a new toolchain appears, add it there. This failure is loud and total, real diagnostics disappear behind it, so it is worth recognizing on sight.
- **clangd runs a subset of the CI check set**: skipping checks it considers slow (>10%
  AST-build cost). That is deliberate and means the same config file is safe to share: CI remains the authority.

### Packaging

`uv run moondeck/ci/package_desktop.py` builds and packages for the host it runs on: a `.dmg` with a `.app` on macOS, a `.tar.gz` plus a `.deb` on Linux, and a `.zip` plus an NSIS `-setup.exe` on Windows. The Windows installer puts the program in `%LOCALAPPDATA%\Programs\MoonLight` with a Start-menu shortcut and an uninstaller; it needs no elevation, and it never touches the settings directory, so an upgrade keeps the user's configuration.

Both the Windows icon and the macOS `.icns` derive from `mooninstaller/favicon.png`, so the mark has one source. The `.ico` is generated during the CMake build (`moondeck/ci/make_ico.py`, which pulls Pillow on demand through uv) and embedded in the executable, so the binary carries its icon whether it was installed or just unzipped.

Each packager skips its platform-specific format when the tool is missing (`dpkg-deb`, `makensis`) on a dev machine, and fails outright under CI, where a missing artifact would otherwise fail the release with an error naming a glob rather than the absent tool.

### Prerequisites

Every host needs [uv](https://docs.astral.sh/uv/), CMake 3.20+, and a C++20 compiler.

- **macOS:** `xcode-select --install` for Clang, `brew install cmake uv`.
- **Linux:** distro packages for `cmake`, GCC 12+ / Clang 15+, and `uv` from astral's installer.
- **Windows:** Visual Studio 2022 Build Tools with the **MSVC v143** workload and **Windows 11 SDK**, plus CMake. Quickest install (run in an elevated terminal):

  ```powershell
  winget install Kitware.CMake
  winget install Microsoft.VisualStudio.2022.BuildTools --override "--passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
  ```

  Build and test from a **Developer PowerShell for VS 2022** (Start Menu → "x64 Native Tools…") so `cl.exe` and the SDK paths are on `PATH`. The default CMake generator on Windows is Visual Studio multi-config, so `projectMM.exe` lands at `build/windows/Release/projectMM.exe` and `mm_scenarios.exe` at `build/windows/test/Release/`. `build_desktop.py` and `run_scenario.py` look in both the `Release/` subdir and the build root, so Ninja (single-config) also works if preferred.

### Docker

The desktop build runs in a container, which is the whole system without an ESP32: same effect pipeline, same web UI, same driver stack, driving real fixtures over Art-Net, DDP and E1.31.

```sh
docker compose up -d      # then open http://localhost:8081/
docker compose logs -f
docker compose down       # stops it; the volume, and your config, survive
```

Or from the published image, one per release:

```sh
docker run -d --name projectmm -p 8081:8080 -v projectmm:/data \
  ghcr.io/moonmodules/projectmm:latest --no-browser
```

`--no-browser` because a container has no browser to open: without it the start prints a line saying it could not open one, which is noise rather than a failure. The Compose service passes it already.

`:latest` follows the rolling prerelease, the same build the installer page offers; a version tag such as `:5.0.0` pins one. Images are published by the release workflow from the same `.deb` that release ships, so the image and the binary are the same build.

**Upgrading preserves everything.** The image holds only the binary and all state lives in the
volume, so an upgrade keeps settings, presets, scripts and the device's identity. Which command depends on which source the compose file names: `docker compose pull && docker compose up -d` for a published `image:`, and `docker compose build --pull && docker compose up -d` for the shipped `build: .`, where `pull` alone would fetch nothing and `up` would rebuild from the checkout. Only `docker compose down -v` wipes it, and only a mounted volume is preserved at all: a bare `docker run` with no `-v` loses its state when the container goes.

| | |
|---|---|
| **Config** | `/data/MoonLight/.config/` in the volume, `XDG_DATA_HOME=/data` |
| **Identity** | `/data/MoonLight/.config/identity`, generated on first run |
| **Logs** | stdout, so `docker logs` |
| **UI** | container port 8080; the compose file publishes it on 8081 so it never fights a native install |
| **Output** | Art-Net UDP 6454, DDP 4048, E1.31 5568, all outbound |
| **Capabilities** | none; it binds its port as an ordinary process |

**When host networking is needed.** Unicast output to a fixture works over ordinary bridge
networking. Discovery and the broadcast or multicast output modes do not cross a bridge, so those want `network_mode: host` (or an L2 CNI on Kubernetes). With host networking there is no port mapping, so pass `--port 8081` in `command:` to stay clear of anything already on 8080.

**Several instances** run side by side with no conflict: each container has its own port space, so
they all listen on 8080 internally with different published ports, and each generates its own identity so they are distinguishable on the network. CPU is the practical limit rather than memory (measured at ~5 MB and about one core each, since the desktop build renders as fast as it is allowed); cap it with `cpus:` in compose when running a fleet.

**amd64 only** for now: the release ships no arm64 Linux binary. On an Apple-silicon Mac or an ARM
server the compose file's `platform: linux/amd64` runs it under emulation, which works but is slower than native.

## ESP32

The ESP32 target uses ESP-IDF directly, not the Arduino framework.

**Tested IDF version:** **v6.1-rc1** (commit `44f0c59f`). CI builds against the `v6.1-rc1` Docker tag and local builds should match (clone command below). The why, the alternatives, and how to check for a newer one are in [ESP-IDF version](#esp-idf-version) below.

### Prerequisites

You need [uv](https://docs.astral.sh/uv/) (Python launcher), CMake 3.20+, and a C++20 compiler. Clone ESP-IDF (~2 GB) into the expected location for your OS — the build scripts search this path first via `Path.home() / "esp" / "esp-idf"`:

**macOS / Linux:**

```sh
git clone --depth 1 --branch v6.1-rc1 https://github.com/espressif/esp-idf.git ~/esp/esp-idf
```

**Windows** (PowerShell — run once with admin to enable long paths if you haven't already):

```powershell
# IDF and its tooling have deeply nested paths; without longpaths the clone
# trips MAX_PATH (260 chars) inside the v6.1-rc1 tree.
git config --global core.longpaths true
git clone --depth 1 --branch v6.1-rc1 https://github.com/espressif/esp-idf.git "$env:USERPROFILE\esp\esp-idf"
```

Then run the one-time Python environment setup — either open MoonDeck (`uv run moondeck/moondeck.py`), go to the ESP32 tab, and click **Setup ESP-IDF**, or run it directly:

```sh
uv run moondeck/build/setup_esp_idf.py                                 # one-time
uv run moondeck/build/build_esp32.py --firmware esp32                  # WiFi-only
uv run moondeck/build/flash_esp32.py --firmware esp32 --port /dev/tty.usbserial-XXXX
uv run moondeck/run/monitor_esp32.py --port /dev/tty.usbserial-XXXX
```

On the variants that opt into it (`esp32`, `esp32-16mb`, `esp32-wrover`, `esp32-eth`, `esp32s3-zero`, and `qemu`, which is emulated rather than installable) the build also produces
**MoonBase**, the second boot image ([MoonBase](../explanation/architecture/moonbase.md)),
and `flash_esp32.py` writes the corrected layout in one pass: app in the big `ota_0` slot, MoonBase in `factory`, and an otadata that boots the app directly. A device on the older dual-OTA table adopts this layout only through such a full serial flash, OTA never rewrites the partition table.

`setup_esp_idf.py` runs the upstream installer for the host: `install.sh` on macOS/Linux, `install.bat` on Windows. Both create the same `~/.espressif/python_env/...` venv and download the same toolchains (~1.5 GB more) — only the wrapper differs. The Windows installer needs roughly 5 minutes on a fast link. It also offers to move a drifted checkout onto the pinned commit (see [ESP-IDF version](#esp-idf-version)); pass `--no-checkout` to keep it warn-only.

**Building for the ESP32-S31** (a RISC-V *preview* target in v6.1) needs its toolchain fetched once — the default install only pulls the classic-`esp32` toolchains:

```sh
(cd ~/esp/esp-idf && ./install.sh esp32s31)   # one-time, adds the S31 RISC-V toolchain
```

Flash the S31 from the web installer or with the CLI (`flash_esp32.py --firmware esp32s31 --port <port>`), whichever suits. Browser flashing works from esptool-js 0.7.0, which identifies the chip by its id rather than by a ROM magic value the S31 shares with the classic ESP32.

On Windows, the `--port` argument is a `COM*` name (e.g. `COM3`) instead of `/dev/tty.usbserial-XXXX`. MoonDeck's port picker enumerates `COM*` automatically.

The ESP32 tab in MoonDeck wraps the same steps as cards (Setup → Firmware → Build → Port → Flash → Run). The Network bar at the top is the same one shown on the Live tab — it remembers which serial port and WiFi credentials belong to the current LAN, so moving the laptop between networks doesn't require re-picking.

![MoonDeck ESP32 tab](../assets/ui/moondeck_esp32.png)

### Windows: USB-serial drivers

Windows ships no drivers for the two USB-serial chips almost every ESP32 dev board uses (WCH CH340/CH341, Silicon Labs CP2102/CP2102N). macOS and Linux do — so a board that Just Works on your Mac may show up on Windows as an `Unknown` device with no `COM*` port allocated at all, in which case both MoonDeck's port dropdown and the web installer's Chrome Web Serial picker come up **empty**. This isn't a MoonLight bug; it's the OS.

**How to tell what you're dealing with:**

- **MoonDeck ESP32 tab** — click Refresh; no `COM*` in the dropdown after plugging in a board.
- **Chrome web installer** — the "Select a serial port" browser dialog says *No serial ports available*.
- **Device Manager** — the board shows under *Other devices* (yellow triangle) with the chip name (e.g. *CP2102N USB to UART Bridge Controller*) and status *Error*.
- **PowerShell** — `Get-PnpDevice | Where-Object InstanceId -match "VID_10C4|VID_1A86"` shows the device with `Status: Error` and empty `SERIALCOMM` registry (`Get-ItemProperty "HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM"`).

**Which driver you need**, by the vendor ID (VID) in the InstanceId:

| VID | Chip | Common boards | Driver |
|---|---|---|---|
| `1A86` | WCH CH340 / CH341 | LOLIN D32, cheap NodeMCU-ESP32 clones, some Olimex | wch.cn official CH341SER (Windows Update usually pulls it — if not, try `pnputil /scan-devices` from an elevated PowerShell first). |
| `10C4` | Silicon Labs CP2102 / CP2102N | ESP32-S3 DevKitC, ESP32-S31 CoreBoard, many newer dev kits | Silicon Labs Universal driver — [zip download](https://www.silabs.com/documents/public/software/CP210x_Universal_Windows_Driver.zip). Windows Update rarely has this one, so grab it directly. |
| `0403` | FTDI FT2232 / FT230X | Some Olimex Gateways, older ESP32-WROVER-KIT | Windows Update usually installs FTDI VCP automatically. |

**Fastest headless install** (works for both CH340 and CP210x — Silicon Labs' zip contains an `.inf` `pnputil` can install directly):

```powershell
# 1) Download + extract the Silicon Labs Universal driver (CP210x).
$dst = "$env:TEMP\cp210x_driver"
Invoke-WebRequest "https://www.silabs.com/documents/public/software/CP210x_Universal_Windows_Driver.zip" `
                  -OutFile "$dst.zip"
Expand-Archive -Path "$dst.zip" -DestinationPath $dst -Force

# 2) Install (elevated — accept the UAC prompt).
Start-Process powershell -Verb RunAs -ArgumentList `
    "pnputil /add-driver $dst\silabser.inf /install"
```

After the driver installs and Windows finishes binding (a few seconds), the boards appear as `Silicon Labs CP210x USB to UART Bridge (COM*)` with `Status: OK`. Both MoonDeck's port list (stateless registry read, no need to restart the server) and the web installer's Chrome picker populate immediately.

**Related Windows serial gotchas — same class of issue, same page:**

- **Stale "ghost" COM ports** from previous plug attempts (visible in Device Manager under *Show hidden devices*) reserve entries in the `ComDB` registry and can block fresh COM allocations even after replug. Clean them with `pnputil /remove-device "USB\VID_…"` (also elevated) — then replug.
- **Wedged CH340 driver from an earlier install** (`Status: Unknown` even though the driver is installed) — same pattern: `pnputil /delete-driver oem*.inf /uninstall /force` for the wch.cn driver, replug, let Windows Update supply a fresh one.
- **Cable / port** — some phone-charging cables carry only VBUS + GND (no data lines) and enumerate as briefly-then-vanish. Try a known-good cable and a rear USB-2 port before spending more time on drivers.

### ESP-IDF version

**Pinned to `v6.1-rc1`** (commit `44f0c59f`, a signed pre-release tag). `setup_esp_idf.py` holds the exact commit in `PINNED_IDF_VERSION`, warns loudly when the installed tree differs, and by default offers to check the pin out so a stray `git pull` or a fresh shallow clone landing on a newer commit converges back rather than silently building against the wrong tree (`--no-checkout` keeps it warn-only). Minimum is ESP-IDF v5.1 (C++20 needs GCC 12+); the project uses v6.x APIs (`esp_eth_phy_new_generic`, the component manager for mDNS, the modern RMT/parlio/LCD drivers) so v5.x would need adjustments.

**Why a v6.1 pre-release and not a stable tag.** The v6.x line is: **v6.0 is the current stable** (GA 2026-02-27); **v6.1 is pre-release** (beta1 2026-06-24, rc1 2026-08-14, GA to follow). We pin the `v6.1-rc1` *tag* (a fixed, signed pre-release, not the rolling `release/v6.1` branch) because it carries driver fixes for the newer SoCs (P4 parlio, RMT v2 on every chip) **and is on the earliest IDF line that carries the `esp32s31` preview target**, and because v6.0 vs v6.1 is a small delta. Riding the betas toward GA means breakage from the v6.1 delta surfaces incrementally, not all at once at the GA re-pin. The trade-off is honest: a pre-release gets **no support guarantee**, which is why the pin is a fixed tag, not a floating branch. The clean inflection point is **v6.1 GA**: re-pin to the `v6.1` tag then, which starts the 30-month support clock (see below). Each pin move (beta1 → RC → GA) is a deliberate re-test pass, not a routine pull. Tracked in [backlog](../work/future/index.md).

**v6.0 is the floor — don't depend on anything newer than it.** Because **v6.0 stable is our fallback** if the v6.1 line proves troublesome, the firmware and build tooling must stay buildable on v6.0. The rule is generic: **use no IDF API, component, Kconfig symbol, or tool that isn't present in v6.0.** A feature that exists only on the v6.1-dev branch (or arrives in a later minor) is off-limits until v6.0 is no longer the fallback. When adopting anything new from the IDF, confirm it shipped in v6.0 first (check the v6.0 docs / release notes, not `latest`); if it's v6.1-only, it waits.

**Explicit exceptions are allowed.** The floor is a default, not an absolute. A feature may step below it (depend on something not in v6.0) when the product owner decides so *explicitly* and the reason is documented at the point it's introduced: in the module spec, a code comment at the dependency, and the commit body. The bar is a conscious, recorded decision, not a silent drift: a floor you can consciously waive with a stated reason stays honest, whereas a rule quietly violated does not. Each such exception also narrows the v6.0 fallback (that target now needs the newer dependency too), so it states what the fallback loses. The known exception today is **P4 WiFi over the C6 co-processor**, which needs `esp_wifi_remote` / esp-hosted (a managed component outside mainline v6.0); it is an accepted, documented exception, scoped to the P4 target, tracked in the [backlog](../work/future/index.md).

**v6.0 vs v6.1, and where the real change was.** The earthquake was **v5.x → v6.0**, not v6.0 → v6.1:

- **v6.0** (vs v5.x): the legacy peripheral drivers were **removed entirely** (ADC, DAC, I2S, Timer, PCNT, MCPWM, **RMT**, temp sensor), which is why the LED drivers use the modern RMT v2 / parlio / `esp_lcd` APIs (rationale at [RmtLedDriver.md](../moonmodules/light/moxygen/RmtLedDriver.md)); **picolibc** replaced newlib as the default C library; **warnings-as-errors** became the default (matches our own `-Werror`); the `CONFIG_ESP_WIFI_ENABLED` switch was dropped (forced on for WiFi SoCs, hence the `EXCLUDE_COMPONENTS` path documented under [Firmware variants](#firmware-variants)); plus the new install manager (EIM), a built-in MCP server, CMake Build System v2 (preview), `wifi_provisioning` → `network_provisioning`, PSA Crypto, and new chips (C5/C61 full, H21/H4 preview).
- **v6.1** (vs v6.0): an ordinary minor — bugfixes, more chip maturity, incremental features on the v6.0 baseline. No second mass-removal. Because it is still beta, its feature set isn't frozen until RC1.

**Support / EOL policy.** Each *stable* ESP-IDF release is supported for **30 months** from its GA date, split into a Service period (frequent bugfix releases, occasional regulatory features) and a Maintenance period (security and high-severity fixes only). Pre-release and dev snapshots get none of this. So pinning to a GA tag (v6.0 today, or v6.1 after 2026-07-31) is what buys the support window; riding `v6.1-dev` does not.

**How to check for a newer version.**

- **Latest stable + all tags:** the [releases page](https://github.com/espressif/esp-idf/releases), or from a clone: `git -C ~/esp/esp-idf fetch --tags && git -C ~/esp/esp-idf tag -l 'v6.*'`.
- **What our tree currently is:** `cat ~/esp/esp-idf/version.txt`, or `git -C ~/esp/esp-idf describe --tags`. `setup_esp_idf.py` prints this and flags drift from the pin.
- **The release schedule + EOL dates:** the upstream [`ROADMAP.md`](https://github.com/espressif/esp-idf/blob/master/ROADMAP.md) (beta/RC/GA dates per minor, and when each older minor reaches end-of-life).

Moving to a different release is never automatic: bump `PINNED_IDF_COMMIT` / `PINNED_IDF_VERSION` in `setup_esp_idf.py` (and the `esp_idf_version` Docker tag + cache key in `.github/workflows/release.yml`), re-clone or check out the new tag, then run the full ESP32 build + hardware re-test pass before committing the bump.

#### Adopting the v6.x ecosystem changes

v6.0 introduced ecosystem-level changes beyond the API surface. The stance, under [§ Principles → Industry standards](../CLAUDE.md#principles), is to **embrace these as the ESP32 standard** — if the IDF makes something the recognised way to build, install, provision, or ship, that's the path we want, not a bespoke one we maintain alone. We adopt them **step by step** (each its own commit + hardware re-test) rather than all at once, and only after they clear the **v6.0-floor rule** above, but the default is *yes, adopt*, with the burden on *why not* — not the reverse.

Two guardrails bound the "embrace everything" stance:

- **Platform-generic stays intact.** These are ESP32-specific gains; none may regress Teensy or the desktop (macOS / Windows / Linux) paths, which don't use ESP-IDF at all. An IDF feature is adopted *inside* the ESP32 platform layer / build tooling, never by leaking an IDF assumption into shared `src/` or the desktop build. If embracing a v6.x feature would touch a cross-platform seam, that seam stays abstracted (the existing platform-boundary rule).
- **The v6.0 floor.** Adopt only what's in v6.0 (see the rule above), so the v6.0 fallback keeps working.

Where we are on each. The adoption plan and per-item triggers are filed in [backlog-core § Adopting the v6.x ecosystem changes](../work/future/backlog-core.md).

| Change | Where we are now |
|---|---|
| **EIM** (ESP-IDF Installation Manager) — the new default, cross-platform installer; Espressif says `install.sh` / `idf_tools.py` are "no longer needed" | `setup_esp_idf.py` drives the legacy `install.sh` / `install.bat`. Works, but is the *old* documented path. |
| **PSA Crypto** — legacy mbedTLS crypto APIs deprecated in favour of the PSA API | No direct exposure: we never call mbedTLS ourselves; OTA uses `esp_https_ota` + `esp_crt_bundle_attach` ([platform_esp32_ota.cpp](../src/platform/esp32/platform_esp32_ota.cpp)), which wrap crypto internally. |
| **`network_provisioning`** — Espressif's Unified Provisioning subsystem, renamed from `wifi_provisioning` in v6.0. Transports: **BLE (GATT)** + **Wi-Fi SoftAP**. Clients: official iOS/Android apps for both, plus `esp_prov` (a Python CLI on Linux/macOS/Windows). Transport-agnostic but ships no web/serial client. | We provision over [Improv](../src/core/system/ImprovProvisioningModule.h) — serial (USB) + BLE, driven from the **browser** (ESP Web Tools) or a serial CLI, which covers mooninstaller / no-app onboarding. The IDF-native **phone-app + SoftAP** flow is a coverage gap rather than a duplicate: the two standards meet only on BLE and own different front-ends. |
| **CMake Build System v2** — the named successor to the current build system; technical preview in v6.0/6.1, has its own migration guide | Standard `idf.py` build (v1). Our component is a thin `idf_component_register()` wrapper, so the migration surface is small. |
| **Built-in MCP server** (`idf.py mcp-server`) — lets an AI assistant drive build/flash/monitor/debug directly | Not used. Agents and humans both go through the `moondeck/<group>/*.py` layer (the uniform interface in [moondeck/MoonDeck.md](../moondeck/MoonDeck.md)), which wraps pin-drift checks, per-firmware build dirs, and KPI collection. |

The general rule: **anything already in v6.0 we adopt proactively** (it clears the floor), while **preview / not-yet-in-v6.0 features wait** until they are stable *and* in our floor. Each adoption is its own commit with its own hardware re-test, and none may regress the Teensy / desktop paths.

### Firmware variants

`build_esp32.py --firmware` selects one of the shipping variants. The key combines chip name + feature flags + (for SKU-sensitive chips) module. ("Firmware" here is the compiled binary; the physical product (deviceModel) is a separate concept: see [MoonInstaller, firmware vs deviceModel vs board](../explanation/architecture/mooninstaller.md#the-three-words).) `build_esp32.py --help` lists the full set.

The canonical list is the **`FIRMWARES` dict** in [`moondeck/build/build_esp32.py`](../moondeck/build/build_esp32.py) — the single source of truth, carrying each variant's `chip`, sdkconfig `fragments`, `eth_only`, `ships`, and `description`. Its machine-readable projection is [`mooninstaller/firmwares.json`](../mooninstaller/firmwares.json) (generated by `generate_firmwares.py`, drift-guarded by `check_firmwares.py`), which the CI release matrix, the ESP Web Tools manifest loops, and MoonDeck all read — so the list lives in exactly one place. `esp32p4rev1-eth-wifi` has `ships: false` (its C6-slave Kconfig isn't reproducible in CI yet), so it builds from the CLI but stays out of the release matrix.

ESP-IDF v6.x has no `CONFIG_ESP_WIFI_ENABLED` switch (the symbol is forced on for WiFi-capable SoCs), so dropping WiFi at compile time happens via `EXCLUDE_COMPONENTS` plus `MM_NO_WIFI` (set when `MM_ETH_ONLY=1`, applied in `esp32/main/CMakeLists.txt`). The `esp32-eth` variant takes this path; the default `esp32` keeps both stacks compiled in and uses the runtime cascade in `NetworkModule` (Ethernet first, WiFi fallback when no PHY responds).

Each firmware has its own build dir at `build/esp32-<firmware>/`, so all variants can coexist on disk. `build_esp32.py` points `idf.py -B` at the per-firmware dir; switching firmwares is just a different `--firmware` argument, no clean rebuild penalty. Same-firmware rebuilds stay incremental, as before. Disk usage scales with the number of firmwares built (≈100 MB each), and a future rename would orphan the old dir — clean with `moondeck/build/clean_esp32.py --firmware <name>` or `--all`.

If a firmware *key* changes its feature set (e.g. the classic `esp32` collapse turned a WiFi-only key into WiFi+Ethernet), its existing build dir would otherwise keep the old `MM_NO_ETH` / `MM_ETH_ONLY` in `CMakeCache.txt` — CMake `-D` flags are written to the cache, and omitting one on a later configure does *not* clear it, so the stale value would silently build the old feature set (Ethernet stubbed out, no link, no LED, and a flash erase wouldn't help because it's a compile-time define). `build_esp32.py` guards this: before reusing a dir it compares the cached feature flags to what the firmware wants and wipes the dir for a clean reconfigure on a mismatch (printing the reason). Same-feature rebuilds are untouched, so the incremental fast path is preserved.

Each ESP32-S3 SKU has its own firmware key because the sdkconfig fragment encodes flash size, partition table, and PSRAM mode — flashing an `n16r8` binary onto a different module (e.g. N8R2) either misaligns the partition table (boot loop) or fails PSRAM init. New SKUs become new keys (e.g. `esp32s3-n8r8`); there is no generic `esp32s3` shortcut.

The Ethernet PHY type and pin map are runtime config, not baked into the build: each firmware carries the driver(s) its chip can host (RMII EMAC for classic/P4, W5500 SPI for S3), and `deviceModels.json` supplies the per-board PHY/pins (pushed into NetworkModule's eth controls at provision). The classic chip default is the common LAN8720 RMII wiring (reset GPIO 5, MDIO addr 0, clock GPIO 17 — e.g. the Olimex ESP32-Gateway), so a board with the same PHY but a different pinout (e.g. WT32-ETH01 with reset on GPIO 16) just needs a different `deviceModels.json` entry — no rebuild.

`--profile` is accepted one release for migration: `--profile default` → `--firmware esp32`, `--profile eth-only` → `--firmware esp32-eth`.

### Flashing a running device over the network

A board already on the network is updated over HTTP, with no cable. Which route to use depends on whether the variant carries [MoonBase](../explanation/architecture/moonbase.md): a board cannot rewrite the partition it is executing from, so on a MoonBase variant the app hands over to MoonBase and MoonBase does the writing.

**On a MoonBase variant** (`esp32`, `esp32-16mb`, `esp32s3-zero`, and any variant `build_esp32.py`
builds MoonBase alongside), it is two requests:

```sh
# 1. the app reboots into MoonBase, with nothing staged. Back in ~4 seconds.
curl -X POST http://<device>/api/firmware/moonbase

# 2. MoonBase writes the app slot and reboots into it
curl --http1.1 -H "Expect:" --data-binary @build/esp32-<firmware>/projectMM.bin \
     http://<device>/api/firmware/upload
```

The second request ends with **no HTTP status** (curl reports 000): the device reboots into the new image as the write completes, so the socket closes before a response arrives. That is success, not failure. Confirm by reading the build back:

```sh
curl -s http://<device>/api/modules/Firmware   # the `build` control names the commit and date
```

MoonBase serves the same route names as the application, so a page driving an update keeps calling the same paths after the handover. It also installs unattended from a URL, which is what the UI's update button uses: `POST /api/firmware/url` with the URL as the body. `POST /api/firmware/boot-app` returns to the application without installing anything, and only boots an image that validates.

**Without MoonBase**, the application takes the image directly on the same route,
`POST /api/firmware/upload`. It is one of only two streaming routes (`/api/file` is the other), so the body may exceed the request buffer; every other route rejects an oversized body with 413.

Two failure modes are worth recognizing, because both look like something else:

- **413 from `/api/firmware/upload`** on a MoonBase variant means the request reached the
  APPLICATION rather than MoonBase, and the app rejected an oversized body on a route it does not stream. The device did not reboot into MoonBase, or booted back before the upload. Check with `GET /moonbase`, which MoonBase answers and the app 404s.
- **`{"error":"incomplete request body"}`** from `/api/firmware/upload` means the body did not
  arrive within the read window. Send with `--http1.1 -H "Expect:"` so the transfer starts immediately instead of waiting for a `100 Continue` the device does not send.

**A partition-table change needs a cable.** OTA writes the app, never the table, so a device on an
older layout adopts a new one only through a full serial flash (see the note under [Firmware variants](#firmware-variants)). On the 4 MB classic that migration also moves the filesystem, so the device comes back unprovisioned.


## Teensy

Teensy 4.x is in the supported target list. Buffers and pipeline configuration scale to 1 MB of internal RAM; OctoWS2811 gives excellent DMA-based LED output. Ethernet is built in on Teensy 4.1 and optional on 4.0.

Build flow is via the root `CMakeLists.txt` with a Teensy toolchain file. The platform layer for Teensy is added when the first hardware target is wired in.

## Pre-compilation steps

CMake runs these automatically before compilation when their source files change:

| Step | Source | Generated | Trigger |
|------|--------|-----------|---------|
| `build_info_gen` | `library.json` | `src/core/util/build_info.h` | `library.json` changes |
| `ui_embed` | `src/ui/index.html`, `app.js`, `style.css`, `preview3d.js`, `install-picker.js`, logo | `src/ui/ui_embedded.h` | any UI file changes |

Both are defined in the root `CMakeLists.txt` (desktop) and `esp32/main/CMakeLists.txt` (ESP32). Generated files are gitignored — rebuilt on every clean build.

## After it's running

The system serves the web UI from its embedded HTTP server. Open `http://<device-ip>/` in a browser; on desktop that's typically `http://localhost:8080/`. From the UI you can change effects and modifiers, configure controls, see the 3D preview. The settings persist across reboot.

To run the test suite or any of the checks (platform boundary, specs, KPI), see the MoonDeck reference linked above. The release-readiness gates that wrap these into a checklist live in [CLAUDE.md § The Process](../CLAUDE.md#the-process).
