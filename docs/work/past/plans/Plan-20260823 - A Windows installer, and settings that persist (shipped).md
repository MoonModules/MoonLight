# Plan, A Windows installer, and settings that persist

## Context

Two defects found on the Windows bench, both hit by the same user in the same session.

**Settings do not persist.** The log fills with `FilesystemModule: write failed for /.config/NetworkModule.json`, one line per save, and every setting is lost on restart.

The root cause is [platform_desktop.cpp:495](src/platform/desktop/platform_desktop.cpp#L495):

```cpp
std::filesystem::path fsRoot_{"build"};
```

A **relative** path, resolved against the process working directory. That is a developer-layout assumption shipped in the product. A double-clicked exe has no writable `build\` beside it, and even where the directory is writable the settings become per-folder: move the exe, re-extract the zip, or launch from elsewhere and they are gone. It also explains a stray instance seen on this bench that came up with no PanelCard driver at all: it simply could not read `build/.config/Drivers.json` from its working directory.

Two things then hide the failure rather than reporting it. `FilesystemModule::setup()` ([FilesystemModule.cpp:39](src/core/FilesystemModule.cpp#L39)) discards the result of `fsMkdir(CONFIG_DIR)`, and desktop `fsMount()` returns `true` unconditionally, so the "mount failed, persistence disabled" path can never fire on desktop. The result is a stream of downstream write errors instead of one clear statement at startup that the location is unusable.

**Windows is the only platform with no install experience.** `moondeck/ci/package_desktop.py` builds a signed `.app` with an icon for macOS and a `.deb` with an icon and menu entry for Linux. Windows gets a bare `.exe` and a `README.txt` in a zip: no icon, no Start-menu entry, no uninstaller.

Outcome: a Windows user installs MoonLight the way a macOS user does, and their settings survive both a restart and an update.

## Approach

### Part 1: settings that persist

Default the desktop filesystem root to a per-user data directory, created at mount time:

| Platform | Data directory |
|---|---|
| Windows | `%LOCALAPPDATA%\MoonLight` |
| macOS | `~/Library/Application Support/MoonLight` |
| Linux | `$XDG_DATA_HOME/MoonLight`, else `~/.local/share/MoonLight` |

**A repo checkout keeps using `build/`**, detected by `CMakeLists.txt` AND `moondeck/` both being present in the working directory (`CMakeLists.txt` alone is true in the root of every CMake project). This is the non-destructive half of the change: the dev workflow, every gate script, and the existing tests are untouched, and nobody's local config silently relocates. `MM_DATA_DIR` overrides both cases, for tests and for anyone who wants the data somewhere specific.

**Make the failure loud and singular.** Desktop `fsMount()` creates the root and returns `false` when it cannot, so `FilesystemModule::setup()`'s existing "persistence disabled" branch does its job. Check the `fsMkdir(CONFIG_DIR)` result there too. One error at startup naming the directory, instead of one per save forever.

Reuse what exists rather than adding seams: `fsMkdir` ([platform_desktop.cpp:526](src/platform/desktop/platform_desktop.cpp#L526)) already wraps `create_directories`, `fsSetRoot` already exists for the override, and `toFsPath` already rejects paths escaping the root.

### Part 2: the installer

**NSIS**, producing `MoonLight-windows-x64-vX.Y.Z-setup.exe` beside the existing zip, which stays for portable use.

- **Program** installs to `%LOCALAPPDATA%\Programs\MoonLight`, per-user, so no elevation prompt.
- **Start-menu shortcut** with the icon, plus an uninstaller and an Add/Remove Programs entry.
- **Icon**: generate a `.ico` from the existing `web-installer/favicon.png` (the macOS path already derives its `.icns` from the same source), and embed it in the exe through a Windows `.rc` resource so the binary carries its icon even outside the installer.
- **Skip when `makensis` is absent on a dev machine, fail outright under CI**, matching the pattern the `.deb` path already uses for a missing `dpkg-deb`. The asymmetry is the point: the release uploads with `fail_on_unmatched_files`, so a silent skip there would fail the whole release, ESP32 firmware and all, with an error naming a glob rather than the absent tool.
- **Close a running instance before overwriting.** A running `MoonLight.exe` holds a lock on the file, which is exactly how three build attempts failed during this session's bench work. The installer must detect and stop it, or the upgrade fails with a file-in-use error.

**Settings survive an update by construction**, because the program and the data live in separate directories:

```text
%LOCALAPPDATA%\Programs\MoonLight\    program   - replaced on update, removed on uninstall
%LOCALAPPDATA%\MoonLight\.config\     settings  - never touched by the installer
```

The uninstaller removes the program only and leaves settings in place, which is standard Windows behavior and means a reinstall or an upgrade finds the user's configuration exactly where it was.

Unsigned, so SmartScreen will warn on first run, the same trade-off already documented for the bare exe and for macOS Gatekeeper. Code signing is out of scope here.

## Files

- **`src/platform/desktop/platform_desktop.cpp`**, the data-directory resolution and the `MM_DATA_DIR` / checkout-detection rules; `fsMount()` creates the root and reports failure.
- **`src/core/FilesystemModule.cpp`**, check the `fsMkdir(CONFIG_DIR)` result in `setup()` and `loadAll()`; report once, naming the directory.
- **`CMakeLists.txt`**, a `WIN32`-guarded `.rc` resource so the exe carries the icon.
- **`moondeck/ci/package_desktop.py`**, `.ico` generation from `web-installer/favicon.png`, the NSIS script, and the installer build with its skip-if-absent guard.
- **`.github/workflows/release.yml`**, publish the installer alongside the zip from the existing `build-windows` job.
- **`docs/how-to/building.md`, `README.md`**, the Windows install path, and where settings live per platform.

## Verification

1. **Unit tests** (`test/unit/core/`, alongside the existing `unit_FilesystemModule_persistence.cpp`): outside a checkout the default root is the per-user directory; inside one it stays `build`; `MM_DATA_DIR` wins over both; an unwritable root fails the mount rather than reporting a write error per save.
2. `ctest --test-dir build/windows -C Release`, note `build_desktop.py --tests` is required first, or ctest silently runs a stale binary.
3. **The bug, end to end**: run the exe from a directory with no `build\`, change a setting, restart, confirm it persists and that no `write failed` line appears.
4. **The installer**: install, launch from the Start menu, change a setting, then install again over the top and confirm the setting survives the upgrade. Uninstall and confirm settings remain.
5. **No regression for the other platforms**: macOS and Linux packaging still build, and a repo checkout still writes to `build/.config`.

## Risks

- **The macOS `.app` has the same latent bug**: its launcher runs `open -a Terminal`, which starts in `$HOME`, so its root is `~/build/.config`. If that works today it works by accident. This change fixes it as a side effect, but anyone relying on the accidental location will find their settings in the new directory. Worth a line in the release notes; no automatic migration, per the chosen option.
- **NSIS on `windows-latest`** is confirmed present (3.10) in the runner image manifest, so no setup step is needed. The bench used 3.12; the script uses nothing version-specific, but the two are not identical, so the first release run is still worth a glance.
