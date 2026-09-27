#!/usr/bin/env python3
"""Build the ESP32 target for a specific firmware variant.

"Firmware" here is the compiled binary variant (chip + radios/peripherals +
sdkconfig fragments) — separate from "board" (physical hardware: PCB, PHY,
USB-serial, PSRAM). See docs/explanation/architecture/index.md § Firmware vs board.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import compute_version   # sibling: the one place a version string is derived

ROOT = Path(__file__).resolve().parent.parent.parent
ESP32_DIR = ROOT / "esp32"

# Common ESP-IDF install locations
IDF_SEARCH_PATHS = [
    Path.home() / "esp" / "esp-idf",
    Path.home() / ".espressif" / "esp-idf",
    Path("/opt/esp-idf"),
]

# The ESP-IDF commit every target (classic ESP32, S3, P4, S31) has been
# validated against — the `v6.1-rc1` tag, on the earliest IDF line that
# carries the esp32s31 preview target. Kept here (not in setup_esp_idf.py) so
# the pre-build drift check below can share the constant — a stale local IDF is
# the single most common source of an "it built for me last week" ESP32 build
# failure, so the check runs on every build_esp32 invocation, not just when the
# user remembers to re-run setup_esp_idf.py. setup_esp_idf.py imports these
# two constants.
PINNED_IDF_COMMIT = "44f0c59f7c81a72a5868a52d5f6dfbbf88829704"
PINNED_IDF_VERSION = "v6.1-rc1"


def installed_idf_commit(idf_path: Path) -> str:
    """Return the git HEAD SHA of the installed IDF, or '' if unavailable."""
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(idf_path),
                           capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else ""
    except OSError:
        return ""


def check_idf_pin(idf_path: Path) -> None:
    """Abort the build if the installed IDF isn't at PINNED_IDF_COMMIT.

    Reason: silently building against a drifted IDF produces the classic
    "missing symbol" / "renamed cap macro" errors that look like *our* bug but
    are actually Espressif renaming an API between snapshots. Failing fast with
    the exact fix command turns 30-minute debugging into a 30-second
    re-checkout. Skip if HEAD can't be determined (not a git checkout, missing
    git, etc.) — no false positives.

    Deliberate escape hatch: a dev testing an upcoming IDF release (beta1 → RC,
    RC → GA) needs to build against a drifted IDF on purpose. Symmetric with
    setup_esp_idf.py's --no-checkout, that path is `--skip-idf-pin-check`
    threaded from main(); passed here as `bypass=True`.
    """
    installed = installed_idf_commit(idf_path)
    if not installed or installed == PINNED_IDF_COMMIT:
        return
    print(f"\nESP-IDF commit drift: installed {installed[:12]} != "
          f"pinned {PINNED_IDF_COMMIT[:12]} ({PINNED_IDF_VERSION}).", file=sys.stderr)
    print("The build was validated against the pinned commit; a drifted IDF is "
          "the most common source of ESP32 build failures that look like "
          "MoonLight bugs but are actually Espressif renaming a symbol.",
          file=sys.stderr)
    print("Fix: re-run `uv run moondeck/build/setup_esp_idf.py` (it will offer "
          "to check out the pinned commit + resync submodules + reinstall "
          "toolchains). See docs/how-to/building.md § ESP-IDF version for the "
          "manual command if you'd rather do it by hand.", file=sys.stderr)
    print("Or pass --skip-idf-pin-check to build anyway (deliberate migration "
          "to a newer IDF release; re-tests then update PINNED_IDF_COMMIT).",
          file=sys.stderr)
    sys.exit(2)


# Components to drop from an Ethernet-only build. ESP-IDF v6.x has no
# CONFIG_ESP_WIFI_ENABLED switch (the symbol is non-settable, forced y on
# WiFi-capable SoCs), so WiFi is removed via EXCLUDE_COMPONENTS instead.
# All consumers of these use *optional* requires, so excluding them links
# cleanly as long as our own code never references esp_wifi (it doesn't —
# the WiFi platform functions are #ifdef-stubbed in the eth-only build).
#
# NOTE: esp_phy is NOT excluded — it provides RF/clock init the ESP32 EMAC
# (Ethernet RMII) depends on. Excluding it leaves Ethernet stuck "started"
# with no link. Only the genuinely WiFi-side components are dropped.
#
# NOTE on the P4 co-processor components (esp_hosted / esp_wifi_remote / eppp_link):
# the `rules: target == esp32p4` gate in main/idf_component.yml pulls them for ANY
# esp32p4 build, including the WiFi-less esp32p4rev1-eth, because manifest rules can't
# see our eth-only flag. EXCLUDE_COMPONENTS does NOT drop them (the component
# manager resolves the managed dependency before the exclude applies). It's a
# *build-time* cost only: the linker dead-strips the unused code, so they add ~0
# bytes of flash to esp32p4rev1-eth (our coprocessorWifi() is the empty stub there, so
# no esp_hosted symbol is referenced — confirmed: their .text size is 0x0 in the
# .map). Left as-is rather than fought; see docs/work/future/.
ETH_ONLY_EXCLUDE = ["esp_wifi", "wpa_supplicant", "esp_coex"]

# Firmware catalogue. Each entry describes one shipping firmware variant.
# Keys combine chip name + feature flags + (for SKU-sensitive chips) module:
#   esp32           — ESP32 classic, WiFi + Ethernet (RMII; eth comes up only
#                     when a PHY is present, pins per board from deviceModels.json)
#   esp32-eth       — ESP32 classic, Ethernet only (WiFi compiled out — smaller)
#   esp32s3-n16r8   — ESP32-S3 DevKitC-1 with the N16R8 module
#                     (16 MB flash, 8 MB octal PSRAM). Other S3 SKUs (N8R2,
#                     N8R8, …) get their own key — the sdkconfig fragment
#                     encodes flash size + partition table + PSRAM mode,
#                     which differ per SKU.
# The Ethernet driver is compiled into each chip's firmware (RMII EMAC for
# classic/P4 via sdkconfig.defaults.eth, W5500 SPI for S3 via .eth-spi);
# which PHY/pins a given board uses is runtime config (deviceModels.json →
# NetworkModule → ethInit), so one binary per chip serves every board.
#
# `panel_cards`: True links PanelCardDriver (panel receiver cards over raw Ethernet). Opt-in
# because the cards need a gigabit link: the S31 has one, and the P4 is included to measure what
# 100 Mbit actually does. Absent = the driver is not compiled in.
#
# `ships`: True for variants the release matrix builds + publishes. A variant can
# exist here (buildable from the CLI) yet be held out of CI with ships=False.
# This dict is the SINGLE source of truth — generate_firmwares.py projects it to
# mooninstaller/firmwares.json, which the CI matrix, the ESP Web Tools manifest
# loops, and MoonDeck all read (check_firmwares.py guards the projection).
FIRMWARES: dict[str, dict] = {
    # Default classic ESP32: WiFi AND Ethernet in one binary. The RMII Ethernet
    # driver compiles in (the .eth fragment); whether Eth comes up, and on which
    # pins/PHY, is runtime config (deviceModels.json → NetworkModule → ethInit). A
    # WiFi-only board flashing this just gets WiFi — ethInit() no-ops when no PHY
    # responds, then the WiFi cascade takes over (no GPIO grab, no hang). This
    # replaces the old separate `esp32` (WiFi-only) + `esp32-eth-wifi` keys.
    "esp32": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.eth", "sdkconfig.defaults.moonbase-4mb"],
        "moonbase": True,   # 4 MB: factory MoonBase + one big app slot (see moonbase/)
        "eth_only": False,
        "description": "ESP32 classic — WiFi + Ethernet (RMII; per-board pins/PHY "
                       "from deviceModels.json, default LAN8720 pins).",
        "ships": True,
    },
    # The EMULATED board. Not silicon: QEMU has no radio and no Ethernet PHY, so this variant swaps
    # WiFi for the emulated OpenCores MAC (see sdkconfig.defaults.qemu). `eth_only` is True for the
    # same reason an Ethernet-only board sets it, there is no WiFi to cascade to.
    #
    # ships=False: nobody flashes this to a device. It exists so the whole firmware, MoonLive's
    # emitted machine code included, can be RUN and debugged on a development machine, with the REST
    # API and web UI reachable through a forwarded host port.
    "qemu": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.qemu", "sdkconfig.defaults.moonbase-4mb"],
        "moonbase": True,   # 4 MB: factory MoonBase + one big app slot (see moonbase/)
        "eth_only": True,
        "description": "ESP32 classic under QEMU, emulated Ethernet (openeth), no WiFi. "
                       "Run with moondeck/qemu/run_qemu.py, not flashed to hardware.",
        "ships": False,
        # Not silicon: keep it out of mooninstaller/firmwares.json entirely. `ships` already stops
        # the release pipeline building it; this stops it reaching the installer's list, whose
        # entries are things a user can flash to a board.
        "installable": False,
    },
    "esp32-16mb": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.16mb",
                      "sdkconfig.defaults.eth", "sdkconfig.defaults.moonbase-16mb"],
        "moonbase": True,   # MoonBase + ONE app slot; the freed 4 MB goes to the filesystem
        "eth_only": False,
        "description": "ESP32 classic with 16 MB flash — WiFi + Ethernet. Same silicon "
                       "as `esp32`; this variant uses the extra flash for a big app slot "
                       "+ an 11 MB filesystem (Serg boards, QuinLED Dig-Octa).",
        "ships": True,
    },
    "esp32-wrover": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.eth",
                      "sdkconfig.defaults.wrover", "sdkconfig.defaults.moonbase-4mb"],
        "moonbase": True,   # 4 MB: factory MoonBase + one big app slot (see moonbase/)
        "eth_only": False,
        "description": "ESP32-WROVER (classic ESP32, 4 MB flash + 4 MB quad PSRAM) — WiFi + "
                       "Ethernet. Same silicon as `esp32`; this variant enables PSRAM for "
                       "the larger buffers (big grids, preview) the WROVER's extra RAM allows.",
        "ships": True,
    },
    "esp32-pico": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.eth",
                      "sdkconfig.defaults.esp32-pico"],
        "moonbase": True,   # 8 MB: factory MoonBase + one app slot (see moonbase/)
        "eth_only": False,
        "description": "ESP32-PICO-V3-02 (classic ESP32 SiP: 8 MB embedded flash + 2 MB "
                       "embedded quad PSRAM). WiFi + Ethernet, same silicon as `esp32`; its "
                       "own variant because the flash is 8 MB where the base assumes 4 and "
                       "PSRAM is on (QuinLED Dig-Next-2).",
        "ships": True,
    },
    "esp32-eth": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.eth", "sdkconfig.defaults.moonbase-4mb"],
        "moonbase": True,   # 4 MB: factory MoonBase + one big app slot (see moonbase/)
        "eth_only": True,
        "description": "ESP32 classic — Ethernet only (WiFi compiled out; smaller "
                       "image, more RAM). Per-board pins/PHY from deviceModels.json. The "
                       "default `esp32` does WiFi+Ethernet — use this only to drop WiFi.",
        "ships": True,
    },
    "esp32s3-n16r8": {
        "chip": "esp32s3",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s3-n16r8",
                      "sdkconfig.defaults.eth-spi"],
        "eth_only": False,
        "description": "ESP32-S3 DevKitC-1 (N16R8: 16 MB flash, 8 MB octal PSRAM) — WiFi + "
                       "W5500 SPI Ethernet (external module, pins per board in deviceModels.json)",
        "ships": True,
        # W5500 over SPI is 100 Mbit, well under the gigabit these cards want, so a wall of
        # any size needs a gigabit switch between the S3 and the card to negotiate the link.
        # Enabled anyway: the S3 is the board most people already own, and a small panel is a
        # real way to try this before buying an S31.
        "panel_cards": True,
    },
    "esp32s3-n8r8": {
        "chip": "esp32s3",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s3-n8r8",
                      "sdkconfig.defaults.eth-spi"],
        "eth_only": False,
        "description": "ESP32-S3 (N8R8: 8 MB flash, 8 MB octal PSRAM) — WiFi + W5500 SPI "
                       "Ethernet. Half the flash of N16R8; the N16R8 binary overruns an "
                       "8 MB board, so N8R8 boards (LightCrafter etc.) need this variant.",
        "ships": True,
        # W5500 over SPI is 100 Mbit, well under the gigabit these cards want, so a wall of
        # any size needs a gigabit switch between the S3 and the card to negotiate the link.
        # Enabled anyway: the S3 is the board most people already own, and a small panel is a
        # real way to try this before buying an S31.
        "panel_cards": True,
    },
    "esp32s3-zero": {
        "chip": "esp32s3",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s3-zero",
                      "sdkconfig.defaults.moonbase-4mb"],
        "moonbase": True,   # 4 MB: factory MoonBase + one big app slot (see moonbase/)
        "eth_only": False,
        "description": "ESP32-S3-Zero (N4R2: 4 MB embedded flash, 2 MB embedded QUAD "
                       "PSRAM) - WiFi only, no Ethernet. Its own variant because neither "
                       "other S3 image can boot here: both assume 8/16 MB flash and set "
                       "SPIRAM_MODE_OCT, and octal mode fails PSRAM init on this board's "
                       "quad part. A thumbnail-sized board for small installations.",
        "ships": True,
        # No Ethernet fragment: the Zero breaks out no SPI header for a W5500, and its
        # appeal is the size, so a variant carrying an unusable PHY would only cost flash
        # on a part that has 48 KB to spare.
        "panel_cards": False,
    },
    "esp32p4rev1-eth": {
        "chip": "esp32p4",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32p4rev1-eth"],
        "eth_only": True,
        "description": "Waveshare ESP32-P4-NANO — Ethernet only (IP101 PHY), for P4 "
                       "revisions 0.x/1.x ONLY. The WiFi-less fallback; "
                       "esp32p4rev1-eth-wifi adds the C6 radio.",
        "ships": True,
        "panel_cards": True,
    },
    "esp32p4rev1-eth-wifi": {
        "chip": "esp32p4",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32p4rev1-eth",
                      "sdkconfig.defaults.esp32p4rev1-eth-wifi"],
        "eth_only": False,
        "description": "Waveshare ESP32-P4-NANO — Ethernet + WiFi via the on-board "
                       "ESP32-C6 over SDIO (esp_hosted), for P4 revisions 0.x/1.x "
                       "ONLY. Boots and associates as of IDF v6.1-rc1 with "
                       "CONFIG_PM_SLEEP_CLK_ICG_ENABLE=n, which sidesteps esp-idf "
                       "#18759 (sleep_clock_icg_startup_init failing ESP_ERR_NO_MEM "
                       "and aborting cpu_start).",
        # Was a crash-repro build for esp-idf #18759 and is now a working firmware:
        # bench-verified on a v1.3 P4 (associates, RSSI -52, serves the UI). #18759 is
        # NOT fixed upstream — CONFIG_PM_SLEEP_CLK_ICG_ENABLE=n (an option v6.1-rc1
        # added) skips the allocation that failed, at the cost of peripheral clock
        # gating during light sleep, which this device never enters.
        "ships": True,
        "panel_cards": True,
    },
    "esp32p4rev3-eth": {
        "chip": "esp32p4",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32p4rev1-eth",
                      "sdkconfig.defaults.esp32p4rev3"],
        "eth_only": True,
        "description": "⚠️ UNTESTED — Waveshare ESP32-P4-NANO, Ethernet only (IP101 "
                       "PHY), for P4 revisions 3.x (the CURRENT silicon). Identical to "
                       "esp32p4rev1-eth apart from the chip revision, which the two "
                       "generations cannot share. Published so someone with a v3 board "
                       "can test it: both bench boards are v1.3, so this image has "
                       "never been booted.",
        # The board fragment is REUSED rather than copied: the two images differ only in
        # CONFIG_ESP32P4_SELECTS_REV_LESS_V3 / REV_MIN, so duplicating the partition
        # table, flash size and EMAC config would be the same fact in two places, and
        # they would drift the first time the board config changed.
        "ships": True,
        "panel_cards": True,
    },
    "esp32p4rev3-eth-wifi": {
        "chip": "esp32p4",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32p4rev1-eth",
                      "sdkconfig.defaults.esp32p4rev1-eth-wifi",
                      "sdkconfig.defaults.esp32p4rev3"],
        "eth_only": False,
        "description": "⚠️ UNTESTED — Waveshare ESP32-P4-NANO, Ethernet + WiFi via the "
                       "on-board ESP32-C6 (esp_hosted), for P4 revisions 3.x (the "
                       "CURRENT silicon). The rev1 build of this image is bench-verified; "
                       "this one differs only in the chip revision and has never been "
                       "booted.",
        "ships": True,
        "panel_cards": True,
    },
    "esp32s31": {
        "chip": "esp32s31",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s31"],
        "eth_only": False,
        "description": "Espressif ESP32-S31 Function-CoreBoard-1 — WiFi 6 + 1 Gbps "
                       "Ethernet LED control (RISC-V, 16 MB flash, PSRAM). The on-chip "
                       "EMAC drives the board's YT8531 RGMII PHY; Ethernet is preferred "
                       "when a cable is present, WiFi otherwise. esp32s31 is a preview "
                       "target on the v6.1 IDF line.",
        "ships": True,
        "panel_cards": True,
    },
}

# IDF target → chip-family label. ONE source for the family vocabulary, shared by:
#   * the ESP Web Tools manifest (`chipFamily`, generate_manifest.py),
#   * the installer's detect-vs-board comparison (deviceModels.json `chip` uses these
#     same strings; install-orchestrator.js normalizes detected silicon to them).
# (firmwares.json does NOT store a per-variant family — it's derivable from `chip`;
# see generate_firmwares.py.)
# MoonLight aims to support every ESP32-family chip, so new SoCs are added HERE
# once (S2 / C3 / C6 / C5 / H2 / P4 variants) and every consumer follows.
TARGET_TO_FAMILY = {
    "esp32":    "ESP32",
    "esp32s3":  "ESP32-S3",
    "esp32s31": "ESP32-S31",
    "esp32p4":  "ESP32-P4",
}

# Chips IDF still marks "preview" — `idf.py set-target <chip>` refuses without an
# explicit `--preview` flag ("you have to append '--preview' to use any preview
# feature"). Drop a chip from this set once it graduates to a stable target.
PREVIEW_TARGETS = {"esp32s31"}

# Deprecated --profile values → firmware, kept one release for callers that
# still pass --profile. Remove once external tooling has migrated.
PROFILE_ALIASES = {
    "default": "esp32",
    "eth-only": "esp32-eth",
}


def find_idf() -> Path | None:
    """Find ESP-IDF installation. Checks IDF_PATH env, then common locations."""
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        p = Path(idf_path)
        if (p / "tools" / "idf.py").exists():
            return p

    for p in IDF_SEARCH_PATHS:
        if (p / "tools" / "idf.py").exists():
            return p
    return None


# Python venv layout differs between platforms — POSIX puts the interpreter in
# `<venv>/bin/python`; Windows in `<venv>/Scripts/python.exe`. Same for the
# toolchain `bin` dirs ESP-IDF unpacks under ~/.espressif/tools (POSIX) vs
# %USERPROFILE%\.espressif\tools\…\Scripts (Windows for some tools, `bin` for
# the GCC toolchains). The constants below capture the per-platform split.
_VENV_BIN = "Scripts" if sys.platform == "win32" else "bin"
_PYTHON_EXE = "python.exe" if sys.platform == "win32" else "python"


def find_idf_python(idf_path: Path | None = None) -> Path | None:
    """Find the ESP-IDF Python venv for the target IDF version.

    ESP-IDF names each venv `idf<major.minor>_py<X.Y>_env` (e.g.
    `idf6.1_py3.12_env`) — one per IDF version × Python version — exactly as
    `idf_tools.py` computes it. We select by matching the *target* IDF version,
    NOT by mtime: with two IDFs installed (e.g. a 5.5 alongside 6.1 for a
    version fallback), the most-recently-activated venv is the newest by mtime
    but belongs to whichever IDF was last sourced, so an mtime pick silently
    hands a 6.1 build the 5.5 venv (mismatched esptool → "requirements not
    satisfied"). Matching the version makes selection a function of what we're
    building, not what was last run.

    Among venvs for the right IDF version (there can be several Python-minor
    variants) the newest wins. Falls back to newest-overall only when the IDF
    version can't be determined or no versioned venv matches — preserving the
    single-IDF behaviour where mtime is unambiguous anyway.
    """
    venv_dir = Path.home() / ".espressif" / "python_env"
    if not venv_dir.exists():
        return None
    candidates = []
    for d in venv_dir.iterdir():
        if (d / _VENV_BIN / _PYTHON_EXE).exists():
            candidates.append((d.stat().st_mtime, d))
    if not candidates:
        return None

    if idf_path is not None:
        # idf_version() → "6.1.0"; the venv prefix uses only major.minor.
        prefix = "idf" + ".".join(idf_version(idf_path).split(".")[:2]) + "_"
        matched = [c for c in candidates if c[1].name.startswith(prefix)]
        if matched:
            matched.sort(reverse=True)
            return matched[0][1]

    candidates.sort(reverse=True)
    return candidates[0][1]


def idf_version(idf_path: Path) -> str:
    """Extract a clean semver from the IDF version string."""
    version_file = idf_path / "version.txt"
    if version_file.exists():
        raw = version_file.read_text(encoding="utf-8").strip()
        # Extract major.minor.patch from strings like "v6.1-dev-399-gd1b91b79b5"
        m = re.match(r"v?(\d+\.\d+)(?:\.(\d+))?", raw)
        if m:
            return f"{m.group(1)}.{m.group(2) or '0'}"
    return "5.4.0"  # safe fallback


def idf_env(idf_path: Path) -> dict:
    """Build the environment for running idf.py."""
    env = dict(os.environ)
    env["IDF_PATH"] = str(idf_path)
    env["ESP_IDF_VERSION"] = idf_version(idf_path)
    # ESP-IDF's Python tooling refuses to run on a non-UTF-8 locale. Windows
    # defaults to cp1252 (locale "English_Netherlands.1252" etc.), so idf.py
    # bails with "Support for Unicode is required". PYTHONUTF8=1 (PEP 540)
    # forces Python into UTF-8 mode regardless of the system locale, and
    # PYTHONIOENCODING covers the stdin/stdout/stderr streams.
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"

    venv_path = find_idf_python(idf_path)
    if venv_path:
        env["IDF_PYTHON_ENV_PATH"] = str(venv_path)

    # IDF's post-build gen_gdbinit.py reads ESP_ROM_ELF_DIR (export.sh sets it;
    # this hand-built env must too). The step only re-runs when its inputs
    # change, so the missing variable failed builds intermittently.
    rom_elfs = Path.home() / ".espressif" / "tools" / "esp-rom-elfs"
    if rom_elfs.exists():
        versions = sorted((d for d in rom_elfs.iterdir() if d.is_dir()), reverse=True)
        if versions:
            env["ESP_ROM_ELF_DIR"] = str(versions[0]) + os.sep

    # Build PATH: venv bin + IDF tools + toolchains + existing PATH
    extra_paths = []

    if venv_path:
        extra_paths.append(str(venv_path / _VENV_BIN))

    extra_paths.append(str(idf_path / "tools"))

    # Add toolchain paths from ~/.espressif/tools. Tool layout varies — POSIX
    # tools have `bin/` subdirs (xtensa-esp-elf, cmake), Windows tools often
    # don't (ninja, ccache, idf-exe ship the .exe at the version-dir root, or
    # inside a single product-named subdir). Add both: the version dir itself
    # (catches flat layouts) plus any nested `bin/` subdir (catches POSIX
    # layouts). Together this covers every tool IDF installs on either host.
    tools_dir = Path.home() / ".espressif" / "tools"
    if tools_dir.exists():
        for tool in tools_dir.iterdir():
            if tool.is_dir():
                for version_dir in sorted(tool.iterdir(), reverse=True):
                    extra_paths.append(str(version_dir))
                    bin_dirs = list(version_dir.rglob("bin"))
                    if bin_dirs:
                        extra_paths.append(str(bin_dirs[0]))
                    break

    env["PATH"] = os.pathsep.join(extra_paths + [env.get("PATH", "")])
    return env


def idf_cmd(idf_path: Path) -> list[str]:
    """Return the command to invoke idf.py via the venv Python.

    On Windows the entry point is `_idf_win_shim.py` instead of idf.py
    directly — the shim calls `locale.setlocale(LC_ALL, "en_US.UTF-8")`
    BEFORE idf.py runs, which is the only way to make IDF's locale check
    (`locale.getlocale()`) pass on Windows installs whose system locale
    is non-UTF-8 (e.g. Dutch / German / French). See the shim's docstring.
    """
    venv_path = find_idf_python(idf_path)
    python_exe = (str(venv_path / _VENV_BIN / _PYTHON_EXE)
                  if venv_path else "python")
    if sys.platform == "win32":
        shim = Path(__file__).resolve().parent / "_idf_win_shim.py"
        return [python_exe, str(shim)]
    return [python_exe, str(idf_path / "tools" / "idf.py")]


def firmware_cmake_args(firmware: str, release: str = "", version: str = "",
                        task_cpu_stats: bool = False) -> list[str]:
    """Extra -D cache args for the requested firmware.

    `release` is the release-channel tag (e.g. "latest", "v1.0.0") to burn
    into the binary as MM_RELEASE. Empty for local builds — SystemModule
    then shows the bare semver with no channel suffix.

    `version` overrides MM_VERSION with the pipeline-computed semver
    (compute_version.py): the core for a stable tag, `<core>-dev.<N>` for a
    moving `latest` build. Empty for local builds — build_info.h's #ifndef
    default (library.json) applies.
    """
    spec = FIRMWARES[firmware]
    frags = list(spec["fragments"])
    if task_cpu_stats:
        # Opt-in per-task CPU% in TasksModule: append the run-time-stats fragment (enables the
        # FreeRTOS counter, ~5% tick) and set the compile def that fills + shows the column. Off
        # unless --task-cpu-stats is passed — a profiling build, never the default.
        frags.append("sdkconfig.defaults.task-cpu-stats")
    fragments = ";".join(frags)
    args = [f"-DSDKCONFIG_DEFAULTS={fragments}"]
    if task_cpu_stats:
        args.append("-DMM_TASK_CPU_STATS=1")
    # Burn the firmware key into the binary so SystemModule can report it and
    # the OTA path can pick the matching release asset (every release ships
    # one .bin per firmware key — see release.yml).
    args.append(f'-DMM_FIRMWARE_NAME="{firmware}"')
    # Burn the release-channel tag too, when the build pipeline supplies one.
    # Same -D mechanism; empty default left to build_info.h's #ifndef so a
    # local build needs no flag.
    if release:
        args.append(f'-DMM_RELEASE="{release}"')
    # Same for the computed version — empty leaves build_info.h's library.json default.
    if version:
        args.append(f'-DMM_VERSION="{version}"')
    # And into the IMAGE's app descriptor, the struct IDF puts in every binary. Without it the
    # descriptor keeps IDF's `git describe` fallback, which drifts from what the device reports:
    # describe names the nearest tag, so a one-off tag left in the repo kept surfacing in built
    # images long after it stopped meaning anything. MoonBase carries the same string, so the app
    # can compare the two images by equality and say when its recovery image was built apart.
    args.append(f"-DPROJECT_VER={version or compute_version.compute('local', '')}")
    if spec["eth_only"]:
        # Drop the WiFi components from the link, and tell our code to compile
        # out the WiFi paths (MM_ETH_ONLY → esp32/main/CMakeLists.txt).
        args.append("-DEXCLUDE_COMPONENTS=" + ";".join(ETH_ONLY_EXCLUDE))
        args.append("-DMM_ETH_ONLY=1")
    # Firmwares that have no Ethernet driver at all (no EMAC sdkconfig and no
    # SPI-PHY sdkconfig) lack the headers platform_esp32.cpp's ethInit() needs,
    # so it won't compile — set MM_NO_ETH and the source provides stubs instead.
    # A variant "has Ethernet" when any of its sdkconfig fragments enables a PHY
    # driver — the RMII EMAC (CONFIG_ETH_USE_ESP32_EMAC, classic/P4/S31) or the
    # W5500 SPI PHY (CONFIG_ETH_USE_SPI_ETHERNET, S3). We read the fragment *files*
    # and check for the actual enabling line rather than pattern-matching the
    # filename: the S31 enables EMAC in `sdkconfig.defaults.esp32s31` (no ".eth" in
    # the name), which a filename heuristic would miss and silently stub eth out.
    # openeth is the third PHY driver: QEMU's emulated MAC. Without it here the qemu variant would
    # be treated as having no Ethernet at all, ethInit() would be stubbed to `return false`, and the
    # emulated board would come up with no IP stack, no REST API, no web UI.
    eth_symbols = {"CONFIG_ETH_USE_ESP32_EMAC=y", "CONFIG_ETH_USE_SPI_ETHERNET=y",
                   "CONFIG_ETH_USE_OPENETH=y"}

    def fragment_enables_eth(frag: str) -> bool:
        path = ESP32_DIR / frag
        if not path.exists():
            return False
        # Match a whole line, not a substring: a disabled symbol is written
        # "# CONFIG_ETH_USE_ESP32_EMAC is not set" (no "=y"), and a commented-out
        # "# CONFIG_...=y" would substring-match but is not actually enabled.
        return any(line.strip() in eth_symbols
                   for line in path.read_text(encoding="utf-8").splitlines())

    if not any(fragment_enables_eth(frag) for frag in spec["fragments"]):
        args.append("-DMM_NO_ETH=1")
    # Panel receiver cards over raw L2 (PanelCardDriver). Opt-in per firmware rather than per chip:
    # the cards want a gigabit link, and classic ESP32 / P4 / S31 all report an internal MAC while
    # only the S31 is gigabit. Firmwares that don't set it save ~2.8 KB of flash.
    if spec.get("panel_cards"):
        args.append("-DMM_PANEL_CARDS=1")
    return args


def resolve_firmware(args: argparse.Namespace) -> str:
    """Resolve the firmware name from --firmware or the deprecated --profile alias."""
    if args.firmware:
        if args.firmware not in FIRMWARES:
            valid = ", ".join(sorted(FIRMWARES))
            print(f"Unknown --firmware '{args.firmware}'. Choose one of: {valid}")
            sys.exit(2)
        return args.firmware

    if args.profile:
        alias = PROFILE_ALIASES.get(args.profile)
        if not alias:
            print(f"Unknown --profile '{args.profile}'. "
                  f"Use --firmware instead (one of: {', '.join(sorted(FIRMWARES))}).")
            sys.exit(2)
        print(f"--profile is deprecated; use --firmware {alias} instead.")
        return alias

    # No flag → keep the prior default behavior (WiFi-only ESP32 classic).
    return "esp32"


def stale_feature_cache(build_dir: Path, extra: list[str], chip: str) -> str | None:
    """Detect a build dir whose cached feature flags disagree with this firmware.

    CMake `-D` flags are written into CMakeCache.txt; *omitting* a flag on a
    later configure does NOT clear it. So if a firmware key's Ethernet-ness
    (MM_NO_ETH / MM_ETH_ONLY) changes while its build dir already exists, the
    stale cache value wins and the binary silently builds for the old feature
    set — e.g. the collapsed `esp32` (WiFi+Eth) reusing a pre-collapse
    WiFi-only dir kept MM_NO_ETH=1 and stubbed Ethernet out (no link, no LED).
    Erasing flash doesn't help: it's a compile-time define, not device state.

    Also catches a mismatched IDF_TARGET: an earlier `set-target` for this
    firmware may have partially succeeded (interrupted, or ran when the
    RISC-V toolchain was missing and only got as far as writing sdkconfig for
    the default `esp32` target), leaving the build dir with the wrong chip.
    The wrapper's "dir exists → skip set-target" fast-path then trusts that
    stale state and silently builds for the wrong chip — the P4 case that
    bit us in the beta1 bring-up. `chip` is the firmware's expected target
    (per FIRMWARES[firmware]["chip"]).

    Returns a human-readable reason string when the cache is stale (caller
    should clean + reconfigure), or None when it matches.
    """
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    text = cache.read_text(encoding="utf-8", errors="replace")
    # Chip / target mismatch — cheap check, do it first (a mismatch here means
    # every other cached flag is also for the wrong target, so no point
    # checking those). CMake writes `IDF_TARGET:STRING=esp32p4`; a fresh
    # cache without an IDF_TARGET line (partial set-target failure before
    # configure) also counts as stale.
    m = re.search(r"^IDF_TARGET:[^=]*=(.*)$", text, re.MULTILINE)
    cached_target = m.group(1).strip() if m else None
    if cached_target != chip:
        return (f"IDF_TARGET cached as {cached_target!r} but this firmware "
                f"wants {chip!r}")
    # The feature toggles whose presence/absence changes which code compiles.
    # The FRAGMENT LIST is a feature flag too: IDF generates sdkconfig from
    # SDKCONFIG_DEFAULTS only when the file is absent, so adding a fragment to a
    # firmware (the MoonBase partition table did this first) silently leaves an
    # existing dir on the OLD config. The cache still holds the LAST run's list at
    # this point, so a mismatch is detectable and means: wipe and reconfigure.
    wanted_frags = next((a.split("=", 1)[1] for a in extra
                         if a.startswith("-DSDKCONFIG_DEFAULTS=")), None)
    m = re.search(r"^SDKCONFIG_DEFAULTS:[^=]*=(.*)$", text, re.MULTILINE)
    cached_frags = m.group(1).strip() if m else None
    if wanted_frags and cached_frags and cached_frags != wanted_frags:
        return (f"SDKCONFIG_DEFAULTS cached as {cached_frags!r} but this firmware "
                f"wants {wanted_frags!r}")

    # And the one generated value dangerous enough to verify outright: the partition table. The
    # list comparison above cannot catch a dir poisoned BEFORE the rule existed (its cache already
    # matches), so read what the fragments want (last fragment naming a table wins, IDF's own
    # merge order) and compare against what the generated sdkconfig actually says.
    wanted_table = None
    if wanted_frags:
        resolved = table_from_fragments(wanted_frags.split(";"))
        wanted_table = str(resolved.relative_to(ESP32_DIR))
    gen = build_dir / "sdkconfig"
    if wanted_table and gen.exists():
        m2 = re.search(r'^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"',
                       gen.read_text(), re.MULTILINE)
        have_table = m2.group(1) if m2 else None
        if have_table != wanted_table:
            return (f"generated sdkconfig uses partition table {have_table!r} but the "
                    f"fragments want {wanted_table!r}")

    # For each, "wanted" = does this firmware pass the -D, "cached" = is it set
    # in the existing cache. A disagreement means a stale dir.
    # MM_TASK_CPU_STATS is here too: toggling --task-cpu-stats on an existing dir must wipe, or the
    # sdkconfig fragment (GENERATE_RUN_TIME_STATS) never re-seeds — CPU% would read all-0 with the flag
    # on, or leave a hidden ~5% tick tax with it off. Same stale-cache trap as MM_NO_ETH.
    for flag in ("MM_NO_ETH", "MM_ETH_ONLY", "MM_NO_WIFI", "MM_TASK_CPU_STATS", "MM_PANEL_CARDS"):
        wanted = any(a.startswith(f"-D{flag}") for a in extra)
        cached = f"{flag}:" in text  # CMake writes `MM_NO_ETH:UNINITIALIZED=1`
        if wanted != cached:
            return (f"{flag} {'set' if cached else 'unset'} in cache but "
                    f"firmware wants it {'set' if wanted else 'unset'}")
    # Value flags (not just present/absent): MM_VERSION / MM_RELEASE carry a string
    # that changes per build. CMake keeps the OLD cached value when the same dir is
    # reused, so a changed --version would silently build the stale version (it's a
    # compile-time define, like the feature flags above). Detect a value mismatch and
    # force a clean reconfigure so the binary never lies about its version.
    for flag in ("MM_VERSION", "MM_RELEASE"):
        wanted = next((a[len(f"-D{flag}="):] for a in extra
                       if a.startswith(f"-D{flag}=")), None)
        if wanted is None:
            continue  # not passed this build — leave the cache alone
        m = re.search(rf"^{flag}:[^=]*=(.*)$", text, re.MULTILINE)
        cached = m.group(1) if m else None
        if cached is not None and cached != wanted:
            return f"{flag} cached as {cached!r} but this build wants {wanted!r}"
    return None


def build_dir_for(firmware: str) -> Path:
    """Return the per-firmware build directory.

    Each firmware variant gets its own subdir of ``<ROOT>/build/`` so multiple
    variants can coexist on disk — switching firmwares no longer forces a
    clean rebuild. The ``esp32-`` prefix namespaces ESP32 firmware keys away
    from desktop targets (``build/macos/``, ``build/linux/``,
    ``build/windows/``) that share the same root. Common-patterns rationale:
    CMake / idf.py ``-B <dir>`` is the documented mechanism for parallel
    build dirs; the bespoke choice here is just the naming.
    """
    return ROOT / "build" / f"esp32-{firmware}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--env", help="ESP32 chip type (legacy; derived from --firmware)")
    parser.add_argument("--firmware", choices=sorted(FIRMWARES),
                        help="Firmware variant. One of: " + ", ".join(sorted(FIRMWARES)))
    parser.add_argument("--profile", choices=["default", "eth-only"],
                        help="Deprecated alias for --firmware. Use --firmware instead.")
    parser.add_argument("--release", default="",
                        help="Release-channel tag to burn into the binary as "
                             "MM_RELEASE (e.g. 'latest', 'v1.0.0'). Set by the "
                             "release workflow; omit for local builds.")
    parser.add_argument("--version", default="",
                        help="Override MM_VERSION with the pipeline-computed semver "
                             "(see compute_version.py): core for a stable tag, "
                             "'<core>-dev.<N>' for latest. Omit for local builds "
                             "(library.json applies).")
    parser.add_argument("--task-cpu-stats", action="store_true",
                        help="Enable per-task CPU%% in TasksModule (FreeRTOS run-time stats). "
                             "A profiling build only — costs ~5%% tick; off by default.")
    parser.add_argument("--skip-idf-pin-check", action="store_true",
                        help="Build against the currently-checked-out IDF even if "
                             "it differs from PINNED_IDF_COMMIT (for a dev "
                             "deliberately testing a newer IDF release before "
                             "the pin bumps). Symmetric with setup_esp_idf.py "
                             "--no-checkout. Use with care: a drifted IDF is the "
                             "most common source of ESP32 build failures.")
    args = parser.parse_args()

    firmware = resolve_firmware(args)
    chip = FIRMWARES[firmware]["chip"]
    # --env, if supplied, must agree with the firmware's chip
    if args.env and args.env != chip:
        print(f"--env {args.env} conflicts with --firmware {firmware} (chip: {chip}). "
              f"Drop --env or pass --firmware for a different chip.")
        sys.exit(2)

    if not ESP32_DIR.exists():
        print(f"ESP32 project directory not found: {ESP32_DIR}")
        sys.exit(1)

    idf_path = find_idf()
    if not idf_path:
        print("ESP-IDF not found. Install it or set IDF_PATH.")
        print("Searched: " + ", ".join(str(p) for p in IDF_SEARCH_PATHS))
        sys.exit(1)

    print(f"Using ESP-IDF at {idf_path}")
    # Fail-fast on a drifted local IDF before we sink a few minutes into a
    # build that would end in "SOC_FOO was renamed to SOC_BAR" or similar. See
    # check_idf_pin above for why this belongs in the build script, not just
    # setup_esp_idf.py. --skip-idf-pin-check bypasses the check entirely for a
    # dev deliberately testing an upcoming IDF release.
    if not args.skip_idf_pin_check:
        check_idf_pin(idf_path)
    env = idf_env(idf_path)
    cmd = idf_cmd(idf_path)

    build_dir = build_dir_for(firmware)
    # -B points idf.py at the per-firmware build dir. -DSDKCONFIG keeps each
    # firmware's sdkconfig inside its own build dir too — without this idf.py
    # writes `esp32/sdkconfig` at the project root, and switching firmwares
    # poisons it ("project sdkconfig was generated for target X, but
    # CMakeCache contains Y"). Per-build-dir sdkconfig is the IDF-supported
    # way to do parallel builds; CMake forwards the variable into the
    # build component manager. Absolute paths are necessary for SDKCONFIG
    # because CMake resolves it relative to the build dir, not the project.
    sdkconfig_path = build_dir / "sdkconfig"
    b_arg = [
        "-B", str(build_dir),
        "-DSDKCONFIG=" + str(sdkconfig_path),
    ]

    # First-time build for this firmware: idf.py needs `set-target` before
    # `build` so sdkconfig gets seeded from SDKCONFIG_DEFAULTS. On subsequent
    # builds the per-build-dir sdkconfig already has the chip pinned, so
    # set-target is skipped — switching to another firmware uses a different
    # build_dir entirely, so its sdkconfig is untouched.
    #
    # esp32p4rev1-eth-wifi needs no special handling: on IDF v6.1-rc1 a clean build and an
    # incremental rebuild through this wrapper both keep CONFIG_SLAVE_IDF_TARGET_ESP32C6=y
    # (esp_wifi_remote's slave target), so it builds like any other variant.
    extra = firmware_cmake_args(firmware, args.release, args.version,
                                task_cpu_stats=args.task_cpu_stats)

    # Guard against a build dir configured for a different feature set (a stale
    # MM_NO_ETH / MM_ETH_ONLY in CMakeCache that a plain reconfigure won't clear).
    # Wiping the dir forces the set-target path below, which seeds a clean cache.
    stale = stale_feature_cache(build_dir, extra, chip)
    if stale:
        print(f"Build dir {build_dir.relative_to(ROOT)} has a stale feature "
              f"cache ({stale}); removing it for a clean reconfigure.")
        shutil.rmtree(build_dir)

    if not build_dir.exists():
        print(f"Setting target to {chip} (firmware: {firmware}, build dir: "
              f"{build_dir.relative_to(ROOT)})...")
        # A preview chip (esp32s31 today) needs `--preview` on idf.py itself,
        # before the action, or set-target refuses it.
        preview = ["--preview"] if chip in PREVIEW_TARGETS else []
        r = subprocess.run(cmd + preview + b_arg + extra + ["set-target", chip],
                           cwd=ESP32_DIR, env=env)
        if r.returncode != 0:
            sys.exit(r.returncode)

    print(f"Building for {chip} (firmware: {firmware})...")
    r = subprocess.run(cmd + b_arg + extra + ["build"], cwd=ESP32_DIR, env=env)
    if r.returncode != 0:
        sys.exit(r.returncode)

    # Show flash/RAM usage summary
    subprocess.run(cmd + b_arg + ["size"], cwd=ESP32_DIR, env=env)

    if FIRMWARES[firmware].get("moonbase"):
        build_moonbase(cmd, env, chip, args.version)


# ---- MoonBase flash layout, shared by every consumer of the build output ----
# flash_esp32.py (serial flash), generate_manifest.py (web installer), preview_installer.py
# (release preview) and run_qemu.py (emulator image) all assemble a flash layout from IDF's
# flasher_args.json. On a MoonBase table that file is WRONG about the app: IDF stages the app
# binary at the first app partition (0x10000: the factory slot, MoonBase's home), because it
# knows nothing about the two-image scheme. These helpers are the one place that knows better.

def table_from_fragments(fragments) -> Path:
    """The partition CSV a fragment list selects (last fragment naming one wins, IDF's own
    merge order). The one resolver: moonbase_table_csv and stale_feature_cache both use it."""
    csv = ESP32_DIR / "partitions" / "esp32dev.csv"
    for frag in fragments:
        fp = ESP32_DIR / frag
        if not fp.exists():
            continue
        m = re.search(r'^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"',
                      fp.read_text(), re.MULTILINE)
        if m:
            csv = ESP32_DIR / m.group(1)
    return csv


def moonbase_table_csv(firmware: str) -> Path:
    return table_from_fragments(FIRMWARES[firmware]["fragments"])


def partition_offsets(csv_path: Path) -> dict:
    """SubType -> offset (hex string) for the rows a MoonBase layout needs: 'factory',
    'ota_0' and 'ota' (the otadata bookkeeping partition)."""
    import csv as _csv
    out = {}
    for row in _csv.reader(csv_path.read_text().splitlines()):
        if not row or row[0].strip().startswith("#") or len(row) < 5:
            continue
        subtype = row[2].strip()
        if subtype in ("factory", "ota_0", "ota"):
            out[subtype] = row[3].strip()
    return out


def otadata_slot0_bytes() -> bytes:
    """An otadata image with slot 0 (ota_0) selected, so a fresh full flash boots the app with
    MoonBase standing by (blank otadata boots the factory slot: MoonBase). Two 4 KB copies; the
    record is 32 bytes: uint32 seq, 24 bytes 0xFF, uint32 CRC over the seq alone
    (bootloader_common_ota_select_crc: crc32_le seeded UINT32_MAX == zlib.crc32(seq, 0xFFFFFFFF)).
    Byte-identical to what IDF's otatool writes for --slot 0, verified against a bench readback.
    """
    import struct, zlib
    seq = struct.pack("<I", 1)
    record = seq + b"\xff" * 24 + struct.pack("<I", zlib.crc32(seq, 0xFFFFFFFF))
    page = record + b"\xff" * (0x1000 - len(record))
    return page + b"\xff" * 0x1000


def moonbase_flash_files(firmware: str, build_dir: Path) -> list[tuple[str, Path]]:
    """The corrected (offset, file) write list for a MoonBase-table flash: IDF's flash_files with
    the app remapped to ota_0, the blank otadata replaced by the slot-0 image (written into the
    build dir), and MoonBase added at the factory slot."""
    import json as _json
    offs = partition_offsets(moonbase_table_csv(firmware))
    chip = FIRMWARES[firmware]["chip"]
    moonbase_bin = build_dir.parent / f"moonbase-{chip}" / "projectMM-moonbase.bin"
    if not all(k in offs for k in ("factory", "ota_0", "ota")) or not moonbase_bin.exists():
        raise FileNotFoundError(
            f"MoonBase layout needs factory/ota_0/otadata offsets and a built image "
            f"(run build_esp32.py first; missing: {moonbase_bin})")
    otadata = build_dir / "ota_data_slot0.bin"
    otadata.write_bytes(otadata_slot0_bytes())
    fa = _json.loads((build_dir / "flasher_args.json").read_text())
    writes: list[tuple[str, Path]] = []
    for off, rel in fa["flash_files"].items():
        name = Path(rel).name
        if name == "projectMM.bin":
            writes.append((offs["ota_0"], build_dir / rel))
        elif name == "ota_data_initial.bin":
            writes.append((offs["ota"], otadata))
        else:
            writes.append((off, build_dir / rel))
    writes.append((offs["factory"], moonbase_bin))
    return writes


def build_moonbase(cmd: list[str], env: dict, chip: str, version: str = "") -> None:
    """Build the MoonBase image for `chip` into build/moonbase-<chip>.

    MoonBase (moonbase/) is the second boot image the MoonBase variants carry in their factory
    partition: a small firmware whose job is installing the application, since a board with one
    app slot cannot rewrite the partition it is executing from. It is chip-specific but variant-
    agnostic, so every classic variant shares one build. Its size budget lives in
    moonbase/sdkconfig.defaults; the shared partition table keeps the two images provably agreed
    on where everything lives.

    `version` becomes PROJECT_VER, which IDF writes into the image's app descriptor. The app
    reads it back from the factory partition to report which MoonBase a device carries, so
    without it a device cannot say what it is running: a bench board that could not install
    firmware took a bisect of the git log to identify, because every MoonBase looked alike.
    A version is variant-independent, so passing it keeps one image per chip valid. Empty for a
    local build, where IDF falls back to `git describe`.
    """
    moonbase_dir = ROOT / "moonbase"
    build_dir = ROOT / "build" / f"moonbase-{chip}"
    b_arg = ["-B", str(build_dir), f"-DSDKCONFIG={build_dir}/sdkconfig"]
    if not version:
        # The app resolves this through build_info.h's #ifndef; MoonBase has no build_info.h, so
        # it resolves the same library.json default here rather than reporting a git-describe
        # string the app has no way to compare against.
        version = compute_version.compute("local", "")
    b_arg.append(f"-DPROJECT_VER={version}")
    # Same trap as stale_feature_cache: IDF generates sdkconfig from the defaults only when it is
    # absent, so an edited moonbase/sdkconfig.defaults silently changes nothing. One defaults file
    # here, so mtime is a sufficient staleness signal.
    gen = build_dir / "sdkconfig"
    defaults = moonbase_dir / "sdkconfig.defaults"
    if gen.exists() and defaults.stat().st_mtime > gen.stat().st_mtime:
        print(f"MoonBase build dir {build_dir.name} predates sdkconfig.defaults; "
              "removing it for a clean reconfigure.")
        shutil.rmtree(build_dir)
    if not build_dir.exists():
        print(f"Setting MoonBase target to {chip}...")
        r = subprocess.run(cmd + b_arg + ["set-target", chip], cwd=moonbase_dir, env=env)
        if r.returncode != 0:
            sys.exit(r.returncode)
    print(f"Building MoonBase for {chip}...")
    r = subprocess.run(cmd + b_arg + ["build"], cwd=moonbase_dir, env=env)
    if r.returncode != 0:
        sys.exit(r.returncode)
    binp = build_dir / "projectMM-moonbase.bin"
    if binp.exists():
        kb = binp.stat().st_size / 1024
        # Slot fit is printed by IDF itself ("Smallest app partition ... free"); repeating a
        # hardcoded slot size here would lie the day the table changes.
        print(f"MoonBase image: {kb:.0f} KB")


if __name__ == "__main__":
    main()
