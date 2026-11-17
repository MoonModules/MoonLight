# Plan: the host platform

Plan of 2026-11-16, a Refactor, approved 2026-11-16 (name: host; the firmware ids change). The platform that runs MoonLight as a program on an operating system, macOS, Windows, Linux and Android, is called **host**, the textbook embedded term for the machine with an operating system against the microcontroller target. "Desktop" named it while it ran on computers only; since Android it misnames a phone and a headset. Each word gets one meaning: **host** for the platform, the build and its code; **desktop** only where it means a computer to a user ("the desktop app"); and the network sense of "host" moves to words of its own, so `src/platform/host/` and a board's own address never share a name.

## What exists

- **The platform layer** is `src/platform/desktop/`, 23 files: `platform_desktop*.cpp`, `main_desktop.cpp`, `main_android.cpp`, the MoonLive host backends. Its paths appear 50 times in 24 files, its file names 26 times in 17 files, and CMake's `MM_DESKTOP_OS` and `MM_DESKTOP_CPU` 16 times in one file.
- **"Host" in the build sense exists already**: `host_update.h`, `host_mdns.h`, `host_address.h`, `moonlive_asm_host.h`, `hostPlatform()`, update mode `Host`, `HostAssembler`, `MM_MOONLIVE_HAS_HOST_JIT`, `HostBus`, `HostRmt`, `kHostIsAndroid`, and "Host:" on a host's Network card. These keep their names: they are the platform's word already.
- **"Host" in the network sense** is MoonLight's own in four places, listed under [Renames](#renames-that-free-the-word); in standards' terms it stays, as below.
- **Firmware ids** `desktop-<os>-<arch>`, as `desktop-macos-arm64` and `desktop-android-arm64`: 336 times in 41 files (`MM_FIRMWARE_NAME`, `firmwares.py`, `firmwares.json`, the installer, the Firmware card's update matching, docs, tests). A running program finds its update by this id.
- **MoonDeck scripts** `build_desktop.py`, `run_desktop.py`, `test_desktop.py` and `ci/package_desktop.py`: 156 times in 51 files, with their cards, MoonDeck.md sections and release.yml steps. `test_host.py` already exists, running the Python and JS tests: 47 times in 19 files.
- **Process and rules**: CLAUDE.md's "Desktop first, always" (5 times in 4 files) and MoonWalk's `desktop-build-test` process, linked 17 times from 11 files.
- **The word "desktop"** appears about 1,900 times in 330 files in all; after the names above, the rest is prose, a share of which means a computer and stays.
- **No card** on the Project names this, so the plan absorbs none.

## End state

One end state, picked: **host** for the platform, the firmware ids changed, the network sense renamed. Rejected: **native**, the name PlatformIO and Zephyr give such a build, a tool's term rather than the textbook pair, and on Android a word for compiled code; **keeping the old firmware ids**, which would leave `desktop-android-arm64` naming a phone for good.

## Renames that free the word

"Host" keeps the platform meaning alone. Where MoonLight's own names use it in the network sense, they move:

| Now | Becomes | Why | Reach |
|---|---|---|---|
| `platform::hostIp()`, `hostIf_`, `findHostIf`, `setTestHostNetwork` | `deviceIp()`, `deviceIf_`, `findDeviceIf`, `setTestDeviceNetwork` | the device's own address, on a board as on a host | 44 times, 11 files |
| `struct Host`, `HostList.h`, `parseHostList`, `hostCount_`, `hosts_`, `hostsError_`, `setHostsField`, `kMaxHosts` | `struct Receiver`, `ReceiverList.h`, `parseReceiverList`, `receiverCount_`, … | the devices a service sends to: a receiver in sACN's and Art-Net's terms | 49 times, 13 files |
| the `hosts` and `lightsPerHost` controls (Network Send, the audio sync's send, OSC's feedback), `knownHostsText` | `receivers`, `lightsPerReceiver`, `knownReceiversText` | user-facing: a control name in a saved config and the API, so a MIGRATING entry | 3 docs files, the services, the UI |
| `HostResolver`, `resolveHost`, `resolveHostStep`, `refreshNamedHosts` | `NameResolver`, `resolveName`, `resolveNameStep`, `refreshNamedReceivers` | it resolves names to addresses, as a DNS resolver does | 80 times, 16 files |
| `controlsHost` (app.js) | `controlsBox` | a DOM container, which the UI calls a box elsewhere | 17 times, 1 file |
| `ScriptHost` (a test fixture) | `ScriptModule` | the module running a script | 6 times, 1 file |

**Kept, being a standard's own word**: `hostname` and `setHostname`, `sanitizeHostname`, `kMaxHostName` (RFC 1123); the HTTP `Host:` header; `localhost`; USB host (`hasUsbMidiHost`, `uvc_host_*`), the role a board takes on the USB bus; ESP-IDF's `SPI2_HOST` and `sdmmc_host_t`, a peripheral's controller; ESP-Hosted (`coprocessorHostVersion`, `host_wifi`), Espressif's product. Words that merely contain the letters (`ghost`, `hostile`) are no concern.

## Steps

| # | Step | Status | Reach | Effort |
|---|---|---|---|---|
| 1 | **Free the word**: the renames above, with a MIGRATING entry for the `hosts` and `lightsPerHost` controls (a restore renames them). | ✅ | about 60 files | Done (2026-11-17): the network sense is `Receiver`, `ReceiverList.h`, `receivers` and `lightsPerReceiver`, with a MIGRATING entry and a Restore rename. |
| 2 | **The platform layer**: `src/platform/desktop/` to `src/platform/host/` (`git mv`, history kept), `platform_desktop*.cpp` to `platform_host*.cpp`, `main_desktop.cpp` to `main_host.cpp`, `MM_DESKTOP_OS` and `MM_DESKTOP_CPU` to `MM_HOST_OS` and `MM_HOST_CPU`; every include and CMake line follows. | ⏳ | 23 files moved, about 40 edited | an hour: the builds on every target prove it |
| 3 | **Firmware ids** `desktop-<os>-<arch>` to `host-<os>-<arch>`: `MM_FIRMWARE_NAME`, `firmwares.py` and the regenerated `firmwares.json`, the installer, the Firmware card's matching, tests. A MIGRATING entry: a host on an earlier build takes the first update by hand, since its own id names no release asset any more. | ⏳ | 41 files, 336 places | half a session: user-visible, so each match is read, not replaced blind |
| 4 | **MoonDeck**: `test_host.py` to `test_tooling.py` first, its "Host Tests" card to "Tooling Tests"; then `build_desktop.py`, `run_desktop.py`, `test_desktop.py` and `ci/package_desktop.py` to `*_host.py`; the Desktop tab to **Host**, beside ESP32 and Teensy, with its eight cards (`"tab": "host"`); MoonDeck.md and release.yml follow. | ⏳ | about 60 files | an hour |
| 5 | **Process and rules**: CLAUDE.md's "Host first, always"; MoonWalk's `desktop-build-test` to `host-build-test` with its 17 links; the steps that name the scripts. | ⏳ | about 15 files | half an hour; CLAUDE.md is the product owner's to approve line by line |
| 6 | **Docs**: each remaining "desktop" read and sorted: the platform becomes host, a computer stays desktop. The README and the tagline keep "macOS, Windows, Linux and Android". | ⏳ | up to 300 files | the largest share: a session, since every line is a judgment |
| 7 | **Checks and tests**: the files named after desktop (`test_firmwares_desktop.py` and others), check_taglines, the scenario and KPI scripts reading the build folder. | ⏳ | about 20 files | an hour |
| 8 | **Verify**: every commit check, the desktop, Android, ESP32 and Teensy builds, the release workflow on a branch run, the installer listing every host package, and a host updating from its Firmware card across the id change. | ⏳ | | half a session, the update across the id change on a real install |

**In all**: about 400 files, most of them a one-word change. For the agent, about three sessions of work; for the product owner, the review of one large and mostly mechanical diff, the line-by-line read of the CLAUDE.md change, and one hand update of a host across the id change. One topic on its own branch, `next-iteration` (#255).

## Open questions

- **When** (decided 2026-11-17): on `next-iteration`, the branch of #255, once #254 is merged and main merged into it, so the rename starts from the encoder's move.
