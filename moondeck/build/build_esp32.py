#!/usr/bin/env python3
"""Build the ESP32 target for a specific firmware variant.

"Firmware" here is the compiled binary variant (chip + radios/peripherals +
sdkconfig fragments); separate from "board" (physical hardware: PCB, PHY,
USB-serial, PSRAM). See docs/explanation/architecture/index.md § Firmware vs board.
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

import compute_version   # sibling: the one place a version string is derived
import sdkconfig_fragments   # sibling: a changed fragment regenerates a build dir's sdkconfig
from moonbase_images import MOONBASE_BIN, build_migrate, build_moonbase   # sibling: the images that carry MoonBase
from coprocessor_image import build_coprocessor   # sibling: the radio chip firmware a WiFi co-processor variant carries
from idf_env import IDF_SEARCH_PATHS, check_idf_pin, find_idf, idf_env, idf_cmd   # sibling: finding and running ESP-IDF

ROOT = Path(__file__).resolve().parent.parent.parent
ESP32_DIR = ROOT / "esp32"

# What ESP-IDF names each build's output, which is the `project()` name in esp32/CMakeLists.txt; MoonBase's, MOONBASE_BIN, lives with its builder in moonbase_images.py and is imported above. It is also stamped into the image descriptor a shipped device compares before it installs anything, so a rename is decided in src/core/util/FirmwareImage.h first. Every script reading a build imports these.
APP_BIN = "MoonLight.bin"
# The first word of every release app image name, `moonlight-<firmware>-v<version>.<ext>`, as the device asks for it (kReleaseAssetPrefix in src/core/system/FirmwareUpdateModule.h).
APP_ASSET_PREFIX = "moonlight"
APP_ELF = "MoonLight.elf"

# Components to drop from an Ethernet-only build. ESP-IDF v6.x has no
# CONFIG_ESP_WIFI_ENABLED switch (the symbol is non-settable, forced y on
# WiFi-capable SoCs), so WiFi is removed via EXCLUDE_COMPONENTS instead.
# All consumers of these use *optional* requires, so excluding them links
# cleanly while our own code references no esp_wifi symbol (the WiFi
# platform functions are #ifdef-stubbed in the eth-only build).
#
# NOTE: esp_phy is NOT excluded; it provides RF/clock init the ESP32 EMAC
# (Ethernet RMII) depends on. Excluding it leaves Ethernet stuck "started"
# with no link. Only the genuinely WiFi-side components are dropped.
ETH_ONLY_EXCLUDE = ["esp_wifi", "wpa_supplicant", "esp_coex"]

# Firmware catalogue. Each entry describes one firmware: a firmware per thing the silicon forces (the chip, the
# P4's revision line, the S3's PSRAM interface and octal flash, the classic cache workaround), a `-lite` tier
# for 4 MB where a family's full build does not fit (CONFIG_MM_LITE), and `-ethonly`, the user's choice.
# The naming rule and why: docs/reference/hardware/firmware-variants.md.
# The Ethernet driver is compiled into each chip's firmware (RMII EMAC for
# classic/P4 via sdkconfig.defaults.eth, W5500 SPI for S3 via .eth-spi);
# which PHY/pins a given board uses is runtime config (deviceModels.json →
# NetworkModule → ethInit), so one binary per chip serves every board.
#
# `layouts`: the flash sizes in MB the firmware installs into, each with its table in LAYOUTS; the fragments
# carry the smallest, since the flash size the app sees comes from the bootloader header, not the app's build.
#
# `panel_cards`: True links PanelCardDriver (panel receiver cards over raw Ethernet). Opt-in
# because the cards need a gigabit link: the S31 has one, and the P4 is included to measure what
# 100 Mbit actually does. Absent = the driver is not compiled in.
#
# `ships`: True for variants the release matrix builds + publishes. A variant can
# exist here (buildable from the CLI) yet be held out of CI with ships=False.
# This dict is the SINGLE source of truth; generate_firmwares.py projects it to
# mooninstaller/firmwares.json, which the CI matrix, the ESP Web Tools manifest
# loops, and MoonDeck all read (check_firmwares.py guards the projection).
FIRMWARES: dict[str, dict] = {
    # The classic ESP32 (docs/reference/hardware/firmware-variants.md): one image per tier for every revision and module. PSRAM is found at boot on
    # every board and joins the heap where psramHeapSafe allows (revision 3 and up, or a build with the cache workaround).
    # WiFi and Ethernet in one binary: the RMII driver compiles in (the .eth fragment), and whether Ethernet comes up, on which
    # pins and PHY, is runtime config (deviceModels.json -> NetworkModule -> ethInit); a board with no PHY falls to the WiFi cascade.
    "esp32": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.eth", "sdkconfig.defaults.psram-classic",
                      "sdkconfig.defaults.moonbase-8mb"],
        "layouts": [8, 16],
        "eth_only": False,
        "description": "ESP32 classic with more than 4 MB flash, every revision: WiFi + Ethernet, every "
                       "feature; PSRAM used on revision 3 and up.",
        "ships": True,
    },
    "esp32-lite": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.eth", "sdkconfig.defaults.psram-classic",
                      "sdkconfig.defaults.lite", "sdkconfig.defaults.moonbase-4mb"],
        "layouts": [4],
        "eth_only": False,
        "description": "ESP32 classic with 4 MB flash, every revision: WiFi + Ethernet, the 4 MB tier "
                       "(no SD card); PSRAM used on revision 3 and up.",
        "ships": True,
    },
    "esp32-ethonly": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.eth", "sdkconfig.defaults.psram-classic",
                      "sdkconfig.defaults.moonbase-4mb"],
        "layouts": [4, 8, 16],
        "eth_only": True,
        "description": "ESP32 classic, every revision: Ethernet only (WiFi compiled out, so every feature "
                       "fits 4 MB and more RAM stays free). Per-board pins/PHY from deviceModels.json.",
        "ships": True,
    },
    # The tier on every layout: with the SD card the workaround's code is 7 KB over the 4 MB slot, and most revision 0 and 1 boards have 4 MB.
    "esp32-rev1-lite": {
        "chip": "esp32",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.eth", "sdkconfig.defaults.psram-classic",
                      "sdkconfig.defaults.psram-workaround", "sdkconfig.defaults.lite", "sdkconfig.defaults.moonbase-4mb"],
        "layouts": [4, 8, 16],
        "eth_only": False,
        "description": "ESP32 classic revision 0 and 1 with PSRAM (the older WROVER modules): WiFi + "
                       "Ethernet, the PSRAM cache workaround, PSRAM used, the 4 MB tier (no SD card).",
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
        "layouts": [4],
        "eth_only": True,
        "description": "ESP32 classic under QEMU, emulated Ethernet (openeth), no WiFi. "
                       "Run with moondeck/qemu/run_qemu.py, not flashed to hardware.",
        "ships": False,
        # Not silicon: keep it out of mooninstaller/firmwares.json entirely. `ships` already stops
        # the release pipeline building it; this stops it reaching the installer's list, whose
        # entries are things a user can flash to a board.
        "installable": False,
    },
    # The S3, one image per PSRAM interface (quad R2 modules, octal R8/R16V) and octal flash: an octal-mode
    # image fails PSRAM init on a quad part, and an octal-flash board needs its flash mode in the bootloader.
    # W5500 SPI Ethernet in the full images, its pins per board in deviceModels.json. `panel_cards`: W5500
    # over SPI is 100 Mbit, under the gigabit the cards want, so a wall needs a gigabit switch between the
    # S3 and the card; enabled because the S3 is the board most people already own.
    "esp32s3-qspi": {
        "chip": "esp32s3",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s3-quad", "sdkconfig.defaults.eth-spi",
                      "sdkconfig.defaults.moonbase-8mb"],
        "layouts": [8, 16],
        "eth_only": False,
        "description": "ESP32-S3 with quad PSRAM and more than 4 MB flash (N8R2, such as the Adafruit "
                       "MatrixPortal S3): WiFi + W5500 SPI Ethernet, every feature.",
        "ships": True,
        "panel_cards": True,
    },
    "esp32s3-qspi-lite": {
        "chip": "esp32s3",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s3-quad", "sdkconfig.defaults.lite",
                      "sdkconfig.defaults.moonbase-4mb"],
        "layouts": [4],
        "eth_only": False,
        "description": "ESP32-S3 with quad PSRAM and 4 MB flash (N4R2, such as the ESP32-S3-Zero): WiFi, "
                       "the 4 MB tier.",
        "ships": True,
        # No Ethernet fragment: the 4 MB boards break out no SPI header for a W5500, and the slot has no room for an unusable PHY.
        "panel_cards": False,
    },
    "esp32s3-opi": {
        "chip": "esp32s3",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s3-octal", "sdkconfig.defaults.eth-spi",
                      "sdkconfig.defaults.moonbase-8mb"],
        "layouts": [8, 16],
        "eth_only": False,
        "description": "ESP32-S3 with octal PSRAM, 8 MB flash and up (N8R8, N16R8): WiFi + W5500 SPI "
                       "Ethernet, every feature.",
        "ships": True,
        "panel_cards": True,
    },
    "esp32s3-opi-oflash": {
        "chip": "esp32s3",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s3-octal", "sdkconfig.defaults.esp32s3-oflash",
                      "sdkconfig.defaults.eth-spi"],
        "layouts": [32],
        "eth_only": False,
        "description": "ESP32-S3 with octal PSRAM and octal flash (WROOM-2 N32R16V): WiFi + W5500 SPI "
                       "Ethernet, every feature.",
        "ships": True,
        "panel_cards": True,
    },
    "esp32c3": {
        "chip": "esp32c3",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32c3",
                      "sdkconfig.defaults.moonbase-4mb"],
        "layouts": [4],
        "eth_only": False,
        "description": "ESP32-C3 (4 MB flash, no PSRAM) - WiFi only, one RISC-V core at 160 MHz.",
        "ships": True,
        "panel_cards": False,
    },
    # The P4 by revision line, since each revision line's image boots on its own line alone: Ethernet (IP101/RMII, per-board pins
    # from deviceModels.json) and WiFi over the on-board ESP32-C6 when it answers. A board with no C6 switches the radio off on its
    # WiFi card, which keeps the network cascade off WiFi.
    "esp32p4rev1": {
        "chip": "esp32p4",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32p4", "sdkconfig.defaults.esp32p4-wifi",
                      "sdkconfig.defaults.moonbase-16mb"],
        "layouts": [16],
        "eth_only": False,
        "coprocessor": "esp32c6",   # the radio chip, whose ESP-Hosted firmware is built beside the app (coprocessor_image.py)
        "description": "ESP32-P4 revisions 0.x and 1.x: Ethernet, and WiFi over the on-board ESP32-C6 where the board has one.",
        # CONFIG_PM_SLEEP_CLK_ICG_ENABLE=n in the WiFi fragment skips the allocation that aborted cpu_start, at the cost of peripheral clock gating during light sleep, a mode this device stays out of.
        "ships": True,
        "panel_cards": True,
    },
    "esp32p4rev3": {
        "chip": "esp32p4",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32p4", "sdkconfig.defaults.esp32p4-wifi",
                      "sdkconfig.defaults.esp32p4rev3", "sdkconfig.defaults.moonbase-16mb"],
        "layouts": [16],
        "eth_only": False,
        "coprocessor": "esp32c6",
        "description": "ESP32-P4 revisions 3.x, the current silicon: Ethernet, and WiFi over the on-board ESP32-C6 where the board has one. "
                       "Differs from esp32p4rev1 only in the chip revision; no revision 3 board is on the bench.",
        "ships": True,
        "panel_cards": True,
    },
    "esp32s31": {
        "chip": "esp32s31",
        "fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s31", "sdkconfig.defaults.moonbase-16mb"],
        "layouts": [16],
        "eth_only": False,
        "description": "Espressif ESP32-S31 Function-CoreBoard-1: WiFi 6 + 1 Gbps "
                       "Ethernet LED control (RISC-V, 16 MB flash, PSRAM). The on-chip "
                       "EMAC drives the board's YT8531 RGMII PHY; Ethernet is preferred "
                       "when a cable is present, WiFi otherwise. esp32s31 is a preview "
                       "target on the v6.1 IDF line.",
        "ships": True,
        "panel_cards": True,
    },
}

# The partition table per chip and flash size, shared by every firmware of the chip: MoonBase plus one app slot,
# the rest files, at every size (the product owner, 2026-11-21); the octal-flash S3's MoonBase is built for octal flash.
LAYOUTS: dict[str, dict[int, str]] = {
    "esp32":    {4: "partitions/4mb_moonbase.csv", 8: "partitions/8mb_moonbase.csv", 16: "partitions/16mb_moonbase.csv"},
    "esp32s3":  {4: "partitions/4mb_moonbase.csv", 8: "partitions/8mb_moonbase.csv", 16: "partitions/16mb_moonbase.csv",
                 32: "partitions/32mb_moonbase.csv"},
    "esp32c3":  {4: "partitions/4mb_moonbase.csv"},
    "esp32p4":  {16: "partitions/16mb_moonbase.csv"},
    "esp32s31": {16: "partitions/16mb_moonbase.csv"},
}

# IDF target → chip-family label. ONE source for the family vocabulary, shared by:
#   * the ESP Web Tools manifest (`chipFamily`, generate_manifest.py),
#   * the installer's detect-vs-board comparison (deviceModels.json `chip` uses these
#     same strings; install-orchestrator.js normalizes detected silicon to them).
# (firmwares.json does NOT store a per-variant family; it's derivable from `chip`;
# see generate_firmwares.py.)
# MoonLight aims to support every ESP32-family chip, so new SoCs are added HERE
# once (S2 / C3 / C6 / C5 / H2 / P4 variants) and every consumer follows.
TARGET_TO_FAMILY = {
    "esp32":    "ESP32",
    "esp32s3":  "ESP32-S3",
    "esp32s31": "ESP32-S31",
    "esp32p4":  "ESP32-P4",
    "esp32c3":  "ESP32-C3",
}

# Chips IDF still marks "preview"; `idf.py set-target <chip>` refuses without an
# explicit `--preview` flag ("you have to append '--preview' to use any preview
# feature"). Drop a chip from this set once it graduates to a stable target.
PREVIEW_TARGETS = {"esp32s31"}

# Deprecated --profile values → firmware, kept one release for callers that
# still pass --profile. Remove once external tooling has migrated.
PROFILE_ALIASES = {
    "default": "esp32",
    "eth-only": "esp32-ethonly",
}


def firmware_cmake_args(firmware: str, release: str = "", version: str = "") -> list[str]:
    """Extra -D cache args for the requested firmware.

    `release` is the release-channel tag (e.g. "latest", "v1.0.0") to burn
    into the binary as MM_RELEASE. Empty for local builds; SystemModule
    then shows the bare semver with no channel suffix.

    `version` overrides MM_VERSION with the pipeline-computed semver
    (compute_version.py): the core for a stable tag, `<next>-dev.<N>` for a
    moving `latest` build. Empty for local builds; build_info.cpp's #ifndef
    default (library.json) applies.
    """
    spec = FIRMWARES[firmware]
    fragments = ";".join(spec["fragments"])
    args = [f"-DSDKCONFIG_DEFAULTS={fragments}"]
    # Burn the firmware key into the binary so SystemModule can report it and
    # the OTA path can pick the matching release asset (every release ships
    # one .bin per firmware key; see release.yml).
    args.append(f"-DMM_FIRMWARE_NAME={firmware}")   # bare: CMake adds the quotes, so idf.py's cache comparison matches
    args.append(f"-DMM_MOONBASE_FAMILY={migration_family(firmware)}")   # the MoonBase image the board takes, which its Firmware card names
    # Burn the release-channel tag too, when the build pipeline supplies one.
    # Same -D mechanism; empty default left to build_info.cpp's #ifndef so a
    # local build needs no flag.
    if release:
        args.append(f"-DMM_RELEASE={release}")
    # Same for the computed version; empty leaves build_info.cpp's library.json default.
    if version:
        args.append(f"-DMM_VERSION={version}")
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
    # so it won't compile; set MM_NO_ETH and the source provides stubs instead.
    # A variant "has Ethernet" when any of its sdkconfig fragments enables a PHY
    # driver; the RMII EMAC (CONFIG_ETH_USE_ESP32_EMAC, classic/P4/S31) or the
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
    set; e.g. the collapsed `esp32` (WiFi+Eth) reusing a pre-collapse
    WiFi-only dir kept MM_NO_ETH=1 and stubbed Ethernet out (no link, no LED).
    Erasing flash doesn't help: it's a compile-time define, not device state.

    Also catches a mismatched IDF_TARGET: an earlier `set-target` for this
    firmware may have partially succeeded (interrupted, or ran when the
    RISC-V toolchain was missing and only got as far as writing sdkconfig for
    the default `esp32` target), leaving the build dir with the wrong chip.
    The wrapper's "dir exists → skip set-target" fast-path then trusts that
    stale state and silently builds for the wrong chip; the P4 case that
    bit us in the beta1 bring-up. `chip` is the firmware's expected target
    (per FIRMWARES[firmware]["chip"]).

    Returns a human-readable reason string when the cache is stale (caller
    should clean + reconfigure), or None when it matches.
    """
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    text = cache.read_text(encoding="utf-8", errors="replace")
    # Chip / target mismatch; cheap check, do it first (a mismatch here means
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
        # as_posix, not str: IDF writes this path into the generated sdkconfig with forward
        # slashes on every platform. str() spells it with backslashes on Windows, so the two
        # never compared equal there and the dir was judged stale on EVERY build: a forced
        # clean reconfigure each time, 14 minutes where an incremental build wanted seconds.
        wanted_table = resolved.relative_to(ESP32_DIR).as_posix()
    gen = build_dir / "sdkconfig"
    if wanted_table and gen.exists():
        m2 = re.search(r'^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"',
                       gen.read_text(encoding="utf-8"), re.MULTILINE)
        have_table = m2.group(1) if m2 else None
        if have_table != wanted_table:
            return (f"generated sdkconfig uses partition table {have_table!r} but the "
                    f"fragments want {wanted_table!r}")

    # For each, "wanted" = does this firmware pass the -D, "cached" = is it set
    # in the existing cache. A disagreement means a stale dir.
    for flag in ("MM_NO_ETH", "MM_ETH_ONLY", "MM_NO_WIFI", "MM_PANEL_CARDS"):
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
        m = re.search(rf"^{flag}:[^=]*=(.*)$", text, re.MULTILINE)
        cached = m.group(1) if m else None
        # Not passed means not wanted: a plain build after a versioned one would otherwise keep reporting that version.
        if cached != wanted:
            return f"{flag} cached as {cached!r} but this build wants {wanted!r}"
    return None


def fragment_paths(extra: list[str]) -> list[Path]:
    """The sdkconfig fragments a build passes in SDKCONFIG_DEFAULTS, in their merge order."""
    frags = next((a.split("=", 1)[1] for a in extra if a.startswith("-DSDKCONFIG_DEFAULTS=")), "")
    return [ESP32_DIR / name for name in frags.split(";") if name]


def config_seed_paths(extra: list[str]) -> list[Path]:
    """What a build's sdkconfig is generated from: its fragments and the project's own Kconfig, whose defaults (`default y if ...`) seed every option a fragment leaves unset."""
    return fragment_paths(extra) + [ESP32_DIR / "main" / "Kconfig.projbuild"]


def build_dir_for(firmware: str) -> Path:
    """Return the per-firmware build directory.

    Each firmware variant gets its own subdir of ``<ROOT>/build/`` so multiple
    variants can coexist on disk; switching firmwares no longer forces a
    clean rebuild. The ``esp32-`` prefix namespaces ESP32 firmware keys away
    from host targets (``build/macos/``, ``build/linux/``,
    ``build/windows/``) that share the same root. Common-patterns rationale:
    CMake / idf.py ``-B <dir>`` is the documented mechanism for parallel
    build dirs; the bespoke choice here is just the naming.
    """
    return ROOT / "build" / f"esp32-{firmware}"


def idf_build(firmware: str, cmd: list[str], env: dict, args, action: str = "build") -> None:
    """Configure build/esp32-<firmware> and run one idf.py action in it: `build` for the firmware, `bootloader` for the bootloader a migration image carries."""
    chip = FIRMWARES[firmware]["chip"]
    build_dir = build_dir_for(firmware)
    # Each firmware keeps its sdkconfig in its own build dir (-DSDKCONFIG, absolute since CMake resolves it from the build dir), ESP-IDF's way to parallel builds:
    # a shared esp32/sdkconfig would carry one firmware's target into the next. A new build dir takes `set-target` first, which seeds it from the defaults.
    b_arg = ["-B", str(build_dir), "-DSDKCONFIG=" + str(build_dir / "sdkconfig")]
    extra = firmware_cmake_args(firmware, args.release, args.version)

    # A build dir configured for another feature set (a stale MM_NO_ETH / MM_ETH_ONLY a reconfigure keeps) is wiped, so set-target below seeds a clean cache.
    stale = stale_feature_cache(build_dir, extra, chip)
    if stale:
        print(f"Build dir {build_dir.relative_to(ROOT)} has a stale feature "
              f"cache ({stale}); removing it for a clean reconfigure.")
        shutil.rmtree(build_dir)

    # A chip in PREVIEW_TARGETS needs `--preview` on idf.py itself, before the action, or set-target and reconfigure refuse it.
    preview = ["--preview"] if chip in PREVIEW_TARGETS else []
    digest = sdkconfig_fragments.refresh(cmd + preview + b_arg + extra, build_dir,
                                         config_seed_paths(extra), ESP32_DIR, env)

    if not build_dir.exists():
        print(f"Setting target to {chip} (firmware: {firmware}, build dir: "
              f"{build_dir.relative_to(ROOT)})...")
        r = subprocess.run(cmd + preview + b_arg + extra + ["set-target", chip],
                           cwd=ESP32_DIR, env=env)
        if r.returncode != 0:
            sys.exit(r.returncode)

    print(f"Building {'for' if action == 'build' else 'the ' + action + ' for'} {chip} (firmware: {firmware})...")
    r = subprocess.run(cmd + b_arg + extra + [action], cwd=ESP32_DIR, env=env)
    if r.returncode != 0:
        sys.exit(r.returncode)
    sdkconfig_fragments.record(build_dir, digest)


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
                             "'<next>-dev.<N>' for latest. Omit for local builds "
                             "(library.json applies).")
    parser.add_argument("--migrate-pause", type=int, default=0, metavar="SECONDS",
                        help="Also build a rehearsal migration image that pauses this long after each step, "
                             "into build/migrate-<family>-pause, so a power cut can be tested at each point.")
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

    idf_build(firmware, cmd, env, args)
    build_dir = build_dir_for(firmware)

    # A bootloader and a table per flash size the firmware installs into, which the release carries and the installer picks by the flash it measures.
    for size in FIRMWARES[firmware]["layouts"]:
        layout_images(firmware, size, cmd[0], env)

    # The flash/RAM summary, read from the linker map: `idf.py size` builds again first, and the build stamp would relink the image a second time.
    subprocess.run([cmd[0], "-m", "esp_idf_size", str(build_dir / "MoonLight.map")], cwd=ESP32_DIR, env=env)

    # The family's first firmware builds its migration image, which carries MoonBase and, beside each table, a bootloader of the family sized to that table.
    family = migration_family(firmware)
    builds_migration = firmware == family_members(family)[0]
    preview = ["--preview"] if chip in PREVIEW_TARGETS else []
    build_moonbase(cmd + preview, env, chip, args.version, family)
    if FIRMWARES[firmware].get("coprocessor"):
        build_coprocessor(cmd, env, FIRMWARES[firmware]["coprocessor"])
    if builds_migration:
        entries = migration_tables(family)
        for donor in dict.fromkeys(d for _, d, _ in entries if d != firmware):
            idf_build(donor, cmd, env, args, "bootloader")
        pairs = [(csv, sized_bootloader(d, size, cmd[0], env)) for csv, d, size in entries]
        build_migrate(cmd + preview, env, chip, family, pairs)
        if args.migrate_pause:
            build_migrate(cmd + preview, env, chip, family, pairs, args.migrate_pause)


# ---- MoonBase flash layout, shared by every consumer of the build output ----
# flash_esp32.py (serial flash), generate_manifest.py (web installer), preview_installer.py
# (release preview) and run_qemu.py (emulator image) all assemble a flash layout from IDF's
# flasher_args.json. On a MoonBase table that file is WRONG about the app: IDF stages the app
# binary at the first app partition (0x10000: the factory slot, MoonBase's home), because it
# knows nothing about the two-image scheme. These helpers are the one place that knows better.

def fragment_value(fragments, key: str) -> str | None:
    """The value a fragment list gives `key` (`CONFIG_...`), the last fragment naming it winning, IDF's own merge order; None when none does."""
    value = None
    for frag in fragments:
        fp = ESP32_DIR / frag
        if fp.exists():
            m = re.search(rf'^{key}=(.*)$', fp.read_text(encoding="utf-8"), re.MULTILINE)
            if m:
                value = m.group(1).strip().strip('"')
    return value


def migration_family(firmware: str) -> str:
    """The boards one MoonBase and one migration image serve: the firmware's chip, on the P4 its revision line, since an image for revision 3 and one for below it exclude each other, and octal flash, which a quad-flash image cannot run from."""
    spec = FIRMWARES[firmware]
    return (spec["chip"] + ("rev3" if fragment_value(spec["fragments"], "CONFIG_ESP32P4_REV_MIN_300") == "y" else "")
            + ("oflash" if fragment_value(spec["fragments"], "CONFIG_ESPTOOLPY_OCT_FLASH") == "y" else ""))


def family_members(family: str) -> list[str]:
    """The shipping firmwares of a migration family, in FIRMWARES order."""
    return [k for k, v in FIRMWARES.items() if v.get("ships") and migration_family(k) == family]


def layouts_markdown() -> str:
    """The partition sizes per layout as the Markdown table build-system.md carries, from LAYOUTS and the CSVs, so the page cannot drift from them (test_build_esp32_migration checks it)."""
    import csv as _csv
    tables: dict[str, list[str]] = {}
    for chip, sizes in LAYOUTS.items():
        for size, csv_name in sizes.items():
            users = [fw for fw, spec in FIRMWARES.items() if spec["chip"] == chip and size in spec["layouts"] and spec.get("ships")]
            tables.setdefault(csv_name, []).extend(f"`{fw}`" for fw in users if f"`{fw}`" not in tables.get(csv_name, []))
    rows = ["| Layout | MoonBase | App | Files | Firmwares |", "|--|--|--|--|--|"]
    for csv_name in sorted(tables, key=lambda n: int(Path(n).name.split("mb")[0])):
        parts = {r[0].strip(): int(r[4].strip(), 0) // 1024 for r in _csv.reader((ESP32_DIR / csv_name).read_text(encoding="utf-8").splitlines())
                 if r and not r[0].strip().startswith("#") and len(r) >= 5}
        files = parts.get("littlefs", 0)
        rows.append(f"| `{Path(csv_name).name}` | {parts.get('moonbase', 0)} KB | {parts.get('app', 0)} KB | {files} KB | {', '.join(tables[csv_name])} |")
    return "\n".join(rows)


def layout_table(firmware: str, size: int) -> Path:
    """The partition table `firmware` installs with on `size` MB of flash (LAYOUTS)."""
    return ESP32_DIR / LAYOUTS[FIRMWARES[firmware]["chip"]][size]


def migration_tables(family: str) -> list[tuple[Path, str, int]]:
    """What the family's migration image carries: one table per flash size and flash type (quad or octal), each with the firmware whose bootloader goes beside it and the size that bootloader's header takes."""
    chosen: dict[tuple[int, bool], tuple[Path, str, int]] = {}
    for fw in family_members(family):
        octal = fragment_value(FIRMWARES[fw]["fragments"], "CONFIG_ESPTOOLPY_OCT_FLASH") == "y"
        for size in FIRMWARES[fw]["layouts"]:
            chosen.setdefault((size, octal), (layout_table(fw, size), fw, size))
    return [chosen[k] for k in sorted(chosen)]


def sized_bootloader(firmware: str, size: int, python: str, env: dict) -> Path:
    """The firmware's bootloader with `size` MB in its header, as esptool writes it with --flash-size (`merge-bin` patches the header and its digest): one bootloader per chip serves every layout."""
    import json as _json
    build_dir = build_dir_for(firmware)
    offset = _json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))["bootloader"]["offset"]
    out = build_dir / "bootloader" / f"bootloader-{size}mb.bin"
    subprocess.run([python, "-m", "esptool", "--chip", FIRMWARES[firmware]["chip"], "merge-bin",
                    "--flash-size", f"{size}MB", "--target-offset", offset, "-o", str(out),
                    offset, str(build_dir / "bootloader" / "bootloader.bin")],
                   cwd=ESP32_DIR, env=env, check=True)
    return out


def table_from_fragments(fragments) -> Path:
    """The partition CSV a fragment list selects (last fragment naming one wins, IDF's own
    merge order). The one resolver: moonbase_table_csv and stale_feature_cache both use it."""
    name = fragment_value(fragments, "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME")
    return ESP32_DIR / (name or "partitions/esp32dev.csv")


def moonbase_table_csv(firmware: str) -> Path:
    return table_from_fragments(FIRMWARES[firmware]["fragments"])


def partition_offsets(csv_path: Path) -> dict:
    """SubType -> offset (hex string) for the rows a MoonBase layout needs: 'factory',
    'ota_0' and 'ota' (the otadata bookkeeping partition)."""
    import csv as _csv
    out = {}
    for row in _csv.reader(csv_path.read_text(encoding="utf-8").splitlines()):
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


def layout_table_image(firmware: str, size: int, python: str, env: dict) -> Path:
    """The partition-table image `firmware` installs with on `size` MB of flash, the table LAYOUTS names, made by ESP-IDF's own gen_esp32part beside the build's own."""
    table = build_dir_for(firmware) / "partition_table" / f"partition-table-{size}mb.bin"
    subprocess.run([python, str(Path(env["IDF_PATH"]) / "components" / "partition_table" / "gen_esp32part.py"),
                    "--flash-size", f"{size}MB", str(layout_table(firmware, size)), str(table)],
                   cwd=ESP32_DIR, env=env, check=True, capture_output=True)
    return table


def layout_images(firmware: str, size: int, python: str, env: dict) -> tuple[Path, Path]:
    """The bootloader and partition-table images `firmware` installs with on `size` MB of flash."""
    return sized_bootloader(firmware, size, python, env), layout_table_image(firmware, size, python, env)


def moonbase_flash_files(firmware: str, build_dir: Path, layout: tuple[int, Path, Path] | None = None) -> list[tuple[str, Path]]:
    """The corrected (offset, file) write list for a MoonBase-table flash: IDF's flash_files with
    the app remapped to ota_0, the blank otadata replaced by the slot-0 image (written into the
    build dir), and MoonBase added at the factory slot. `layout` (size, bootloader, table), from
    layout_images, swaps in another flash size's layout; with none given, the one the build carries."""
    import json as _json
    offs = partition_offsets(layout_table(firmware, layout[0]) if layout else moonbase_table_csv(firmware))
    moonbase_bin = build_dir.parent / f"moonbase-{migration_family(firmware)}" / MOONBASE_BIN
    if not all(k in offs for k in ("factory", "ota_0", "ota")) or not moonbase_bin.exists():
        raise FileNotFoundError(
            f"MoonBase layout needs factory/ota_0/otadata offsets and a built image "
            f"(run build_esp32.py first; missing: {moonbase_bin})")
    otadata = build_dir / "ota_data_slot0.bin"
    otadata.write_bytes(otadata_slot0_bytes())
    fa = _json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    writes: list[tuple[str, Path]] = []
    for off, rel in fa["flash_files"].items():
        name = Path(rel).name
        if name == APP_BIN:
            writes.append((offs["ota_0"], build_dir / rel))
        elif name == "ota_data_initial.bin":
            writes.append((offs["ota"], otadata))
        elif layout and name == "bootloader.bin":
            writes.append((off, layout[1]))
        elif layout and name == "partition-table.bin":
            writes.append((off, layout[2]))
        else:
            writes.append((off, build_dir / rel))
    writes.append((offs["factory"], moonbase_bin))
    return writes


if __name__ == "__main__":
    main()
