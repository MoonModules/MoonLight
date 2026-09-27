#!/usr/bin/env python3
"""Generate an ESP Web Tools manifest for one firmware variant.

ESP Web Tools (https://esphome.github.io/esp-web-tools/) flashes ESP32
firmware from the browser using Web Serial. It reads a JSON manifest that
lists which `.bin` parts to write at which offsets, keyed by chip family.

Schema reference: https://esphome.github.io/esp-web-tools/#manifest

The release pipeline calls this once per firmware variant. Each variant gets
its own manifest because:
  * the firmware bundle differs per variant (sdkconfig fragments → different
    bootloader and partition table),
  * the chip family differs (ESP32 vs ESP32-S3), and so does the bootloader
    offset on flash (0x1000 vs 0x0 — wrong offset bricks visibly).

"Firmware" here is the compiled binary variant — separate from "board" (the
physical hardware). See docs/explanation/architecture/index.md § Firmware vs board.

We don't hardcode the offsets. ESP-IDF writes them into
`build/flasher_args.json` for the exact chip it just built. The CI stage
copies that file alongside the bins, and we parse it here.

Inputs:
  --firmware <key>        — firmware variant key (matches the firmware
                            filename prefix).
  --version <ver>         — release version, e.g. "1.0.0".
  --release-url <url>     — base URL to the GitHub release assets.
  --flasher-args <path>   — esp32/build/flasher_args.json from the build.
  --out <path>            — manifest JSON destination.
"""

import argparse
import json
import sys
from pathlib import Path

# build_esp32.py is the single source of truth for which firmware variants exist
# and which IDF target each builds (`FIRMWARES[f]["chip"]`). Import it so this
# script never holds a second hand-maintained variant list — same cross-script
# import collect_kpi.py already uses.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_esp32 import FIRMWARES, TARGET_TO_FAMILY  # noqa: E402

# ESP-IDF writes a number of `<name>.bin` references in flasher_args.json,
# each keyed by hex offset. We map IDF's output names to the firmware-bundle
# filenames the release publishes (the same names release.yml stages from
# esp32/build/).
#
# Two of the four parts are byte-identical across firmwares and so are shared
# (uploaded once, every firmware's manifest points at the shared name):
#   * ota_data_initial.bin — a fixed 0xFF region, identical for ALL firmwares.
#   * partition-table.bin  — identical within a flash-size GROUP (4/8/16 MB), so
#     it is named per group (`partition-table-<size>.bin`). The {size} field is
#     filled from flasher_args' flash_settings.flash_size (see resolve below).
# App + bootloader stay per-firmware ({prefix}), as both differ per variant.
PART_NAME_MAP = {
    "projectMM.bin": "{prefix}.bin",
    "bootloader.bin": "{prefix}-bootloader.bin",
    "partition-table.bin": "partition-table-{size}.bin",
    "ota_data_initial.bin": "shared-ota-data.bin",
}

# firmware → ESP Web Tools chip family. TARGET_TO_FAMILY lives in build_esp32 (the
# one family-vocabulary source, shared with firmwares.json); derived per firmware
# so adding a variant to FIRMWARES flows here automatically. ESP Web Tools accepts
# these family strings — https://github.com/espressif/esptool-js/blob/main/src/esploader.ts
CHIP_FAMILIES = {f: TARGET_TO_FAMILY[spec["chip"]] for f, spec in FIRMWARES.items()}


def parts_from_flasher_args(flasher_args: dict, prefix: str, size: str) -> list[dict]:
    """Convert IDF's flasher_args.json `flash_files` map into Web Tools parts.

    `prefix` names the per-firmware parts (app + bootloader); `size` (e.g. "4mb")
    names the shared partition-table group. `{prefix}` / `{size}` in the
    PART_NAME_MAP templates are filled here — the shared parts ignore `prefix`.
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
            "path": template.format(prefix=prefix, size=size),
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
    parser.add_argument("--flasher-args", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    try:
        flasher_args = json.loads(args.flasher_args.read_text())
    except FileNotFoundError:
        print(f"generate_manifest: {args.flasher_args} not found")
        return 2
    except json.JSONDecodeError as e:
        print(f"generate_manifest: {args.flasher_args} not valid JSON: {e}")
        return 2

    # Per-firmware parts: firmware-<firmware>-v<version>{,-bootloader}.bin.
    prefix = f"firmware-{args.firmware}-v{args.version}"
    base_url = args.release_url.rstrip("/")

    # Flash-size group for the shared partition-table name (partition-table-<size>.bin).
    # IDF records it in flasher_args; lowercased so "16MB" → "16mb".
    size = str(flasher_args.get("flash_settings", {}).get("flash_size", "")).lower()
    if not size:
        print(f"generate_manifest: no flash_settings.flash_size in {args.flasher_args}")
        return 2

    parts = parts_from_flasher_args(flasher_args, prefix, size)
    if not parts:
        print(f"generate_manifest: no recognized parts in {args.flasher_args}")
        return 1

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

    # ESP Web Tools resolves the per-part `path` relative to the manifest URL,
    # so absolute URLs are the simplest robust shape — the manifest stays
    # correct whether it's served from Pages, a release page, or a mirror.
    for p in parts:
        p["path"] = f"{base_url}/{p['path']}"

    manifest = {
        "name": "MoonLight",
        "version": args.version,
        "home_assistant_domain": "projectMM",
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


if __name__ == "__main__":
    sys.exit(main())
