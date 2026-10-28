#!/usr/bin/env python3
"""Generate an ESP Web Tools manifest for one firmware variant.

ESP Web Tools (https://esphome.github.io/esp-web-tools/) flashes ESP32
firmware from the browser using Web Serial. It reads a JSON manifest that
lists which `.bin` parts to write at which offsets, keyed by chip family.
The installer reads the same shape for every platform and picks its writer by `chipFamily`: a Teensy firmware is one part, the flat image at offset 0, and takes no --flasher-args.

Schema reference: https://esphome.github.io/esp-web-tools/#manifest

The release pipeline calls this once per firmware variant. Each variant gets
its own manifest because:
  * the firmware bundle differs per variant (sdkconfig fragments → different
    bootloader and partition table),
  * the chip family differs (ESP32 vs ESP32-S3), and so does the bootloader
    offset on flash (0x1000 vs 0x0; a wrong offset bricks visibly).

"Firmware" here is the compiled binary variant, separate from "board" (the
physical hardware). See docs/explanation/architecture/index.md § Firmware vs board.

We don't hardcode the offsets. ESP-IDF writes them into
`build/flasher_args.json` for the exact chip it just built. The CI stage
copies that file alongside the bins, and we parse it here.

Inputs:
  --firmware <key>        firmware variant key (matches the firmware
                            filename prefix).
  --version <ver>         release version, e.g. "1.0.0".
  --release-url <url>     base URL to the GitHub release assets.
  --flasher-args <path>   esp32/build/flasher_args.json from the build (ESP32 only).
  --out <path>            manifest JSON destination.
"""

import argparse
import json
import sys
from pathlib import Path

# The firmware registry is the one list of variants and their chips, on every platform.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_esp32 import APP_BIN  # noqa: E402
from firmwares import FIRMWARES, chip_family  # noqa: E402

# ESP-IDF writes a number of `<name>.bin` references in flasher_args.json,
# each keyed by hex offset. We map IDF's output names to the firmware-bundle
# filenames the release publishes (the same names release.yml stages from
# esp32/build/).
#
# App, bootloader and partition table are per-firmware ({prefix}): firmwares of one flash
# size still differ in layout (a MoonBase table puts the app at 0xF0000, a dual-OTA table at
# 0x10000). Only ota_data_initial.bin, a fixed 0xFF region identical for ALL firmwares, is
# shared (uploaded once, every firmware's manifest points at the shared name).
PART_NAME_MAP = {
    APP_BIN: "{prefix}.bin",
    "bootloader.bin": "{prefix}-bootloader.bin",
    "partition-table.bin": "{prefix}-partition-table.bin",
    "ota_data_initial.bin": "shared-ota-data.bin",
}

# firmware → chip family, from the registry's one family vocabulary; ESP Web Tools accepts
# the ESP32 strings, https://github.com/espressif/esptool-js/blob/main/src/esploader.ts
CHIP_FAMILIES = {f: chip_family(f) for f in FIRMWARES}


def parts_from_flasher_args(flasher_args: dict, prefix: str) -> list[dict]:
    """Convert IDF's flasher_args.json `flash_files` map into Web Tools parts.

    `prefix` names the per-firmware parts; the shared part ignores it.
    """
    flash_files = flasher_args.get("flash_files", {})
    parts: list[dict] = []
    for offset_hex, bin_name in flash_files.items():
        # Some entries are full paths (e.g. "bootloader/bootloader.bin"); we
        # match on basename so the lookup table stays firmware-neutral.
        basename = Path(bin_name).name
        template = PART_NAME_MAP.get(basename)
        if not template:
            print(f"generate_manifest: skipping unknown part '{bin_name}'")
            continue
        offset = int(offset_hex, 16) if isinstance(offset_hex, str) else int(offset_hex)
        parts.append({
            "path": template.format(prefix=prefix),
            "offset": offset,
        })
    # Web Tools doesn't require a specific order, but sorted-by-offset is the
    # human-readable convention every other project uses.
    parts.sort(key=lambda p: p["offset"])
    return parts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", required=True, choices=sorted(CHIP_FAMILIES))
    parser.add_argument("--version", required=True)
    parser.add_argument("--release-url", required=True,
                        help="Base URL where the .bin assets live (no trailing slash).")
    parser.add_argument("--flasher-args", type=Path, help="ESP32 only: IDF's flasher_args.json.")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    # Per-firmware parts: firmware-<firmware>-v<version>{,-bootloader}.bin.
    prefix = f"firmware-{args.firmware}-v{args.version}"
    base_url = args.release_url.rstrip("/")

    if FIRMWARES[args.firmware]["platform"] == "esp32":
        if not args.flasher_args:
            print("generate_manifest: an ESP32 firmware needs --flasher-args")
            return 2
        parts = esp32_parts(args, prefix)
        if parts is None:
            return 2
        if not parts:
            print(f"generate_manifest: no recognized parts in {args.flasher_args}")
            return 1
    else:
        parts = [{"path": f"{prefix}.bin", "offset": 0}]   # the flat image, which starts at the flash base

    # ESP Web Tools resolves the per-part `path` relative to the manifest URL,
    # so absolute URLs are the simplest robust shape: the manifest stays
    # correct whether it's served from Pages, a release page, or a mirror.
    for p in parts:
        p["path"] = f"{base_url}/{p['path']}"

    manifest = {
        "name": "MoonLight",
        "version": args.version,
        "home_assistant_domain": "MoonLight",
        # Wipe-first on flash. Right default while config schemas are
        # unstable (avoids stale-state bugs across versions). Revisit when
        # the schema is locked.
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": CHIP_FAMILIES[args.firmware],
                "parts": parts,
            }
        ],
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"generate_manifest: wrote {args.out} ({len(parts)} parts, "
          f"{CHIP_FAMILIES[args.firmware]})")
    return 0


def esp32_parts(args, prefix: str) -> list[dict] | None:
    """An ESP32 firmware's parts, from IDF's flasher_args.json; None when that file is unusable."""
    try:
        flasher_args = json.loads(args.flasher_args.read_text())
    except FileNotFoundError:
        print(f"generate_manifest: {args.flasher_args} not found")
        return None
    except json.JSONDecodeError as e:
        print(f"generate_manifest: {args.flasher_args} not valid JSON: {e}")
        return None

    parts = parts_from_flasher_args(flasher_args, prefix)

    # MoonBase firmwares: flasher_args stages the app at the first app partition: the factory
    # slot, MoonBase's home: because IDF knows nothing about the two-image scheme. Remap the app
    # to ota_0, swap the blank ota-data for the slot-0 image (a blank one would boot MoonBase on
    # first start), and add MoonBase at the factory slot. Offsets come from the same partition CSV
    # the firmware builds with (moonbase_table_csv), so manifest and table cannot disagree.
    # release.yml stages the two shared-moonbase assets these paths point at.
    from build_esp32 import moonbase_table_csv, partition_offsets
    spec = FIRMWARES[args.firmware]
    if spec.get("moonbase"):
        offs = partition_offsets(moonbase_table_csv(args.firmware))
        for part in parts:
            if part["path"] == f"{prefix}.bin":
                part["offset"] = int(offs["ota_0"], 16)
            elif part["path"] == "shared-ota-data.bin":
                part["path"] = "shared-ota-data-slot0.bin"
        parts.append({"path": f"shared-moonbase-{spec['chip']}.bin",
                      "offset": int(offs["factory"], 16)})
        parts.sort(key=lambda p: p["offset"])

    return parts


if __name__ == "__main__":
    sys.exit(main())
