#!/usr/bin/env python3
"""Generate mooninstaller/firmwares.json from the FIRMWARES dict.

The firmware-variant list was hand-copied across the CI matrix, the two ESP Web
Tools manifest loops, MoonDeck, and the docs: six copies that drifted (MoonDeck's
even dropped the P4 variants). This script projects the single source of truth,
the `FIRMWARES` registry in firmwares.py, into one machine-readable file every
consumer reads instead:

  * release.yml: the build matrix + both manifest loops select `.ships`,
  * MoonDeck: its firmware picker,
  * docs/how-to/building.md: points here instead of restating the list.

check_firmwares.py guards the committed file against drift from FIRMWARES.

"Firmware" here is the compiled binary variant, separate from "board" (the
physical hardware). See docs/explanation/architecture/index.md § Firmware vs board.

Inputs:
  --out <path>   firmwares.json destination (mooninstaller/firmwares.json).
"""

import argparse
import json
import sys
from pathlib import Path

# firmwares.py joins each platform's slice into the one registry; import it so this script never holds a second hand-maintained list.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from firmwares import FIRMWARES  # noqa: E402


def build_doc() -> dict:
    """Project FIRMWARES into the firmwares.json document.

    `chip` is the platform's build target: the IDF target on an ESP32 ("esp32s3"), the board on a Teensy ("teensy41"). The chip-FAMILY label ("ESP32-S3")
    is deliberately NOT stored here: it's derivable from `chip` via firmwares.py's
    TARGET_TO_FAMILY, and no firmwares.json consumer needs it: the ESP Web Tools
    manifest gets `chipFamily` from that map at generation time, and the installer's
    runtime family check uses deviceModels.json + esptool detection, not this file. Adding
    it would be redundant data ahead of a consumer. `fragments` is build-internal
    and likewise omitted. check_firmwares.py imports this so the generator and
    checker can't disagree.
    """
    return {
        "firmwares": [
            {
                "name": name,
                "platform": spec["platform"],
                "chip": spec["chip"],
                "eth_only": spec["eth_only"],
                "ships": spec["ships"],
                "description": spec["description"],
            }
            # FLASHABLE variants only. This file feeds the WEB INSTALLER, the list of things a
            # user puts on a board, so a variant that is not silicon has no place in it. The QEMU
            # variant runs an emulator on a development machine and is driven from MoonDeck, never
            # from the installer; listing it made the installer's naming rules have to describe
            # something that will never appear in its UI.
            for name, spec in FIRMWARES.items() if spec.get("installable", True)
        ]
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", required=True, type=Path,
                        help="firmwares.json destination")
    args = parser.parse_args()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    # ensure_ascii=False keeps the em-dashes in descriptions literal (readable
    # diffs), matching the hand-authored deviceModels.json sibling.
    doc = build_doc()
    # encoding pinned for the same reason ensure_ascii=False is set: this writes non-ASCII, and
    # an unnamed encoding falls back to the platform default, which on Windows is cp1252 and
    # raises UnicodeEncodeError. The MoonLive catalog generator failed this way in CI.
    args.out.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    # Count what was WRITTEN, not len(FIRMWARES): build_doc drops non-installable
    # variants, so the raw dict size contradicts check_firmwares.py's count.
    print(f"generate_firmwares: wrote {args.out} ({len(doc['firmwares'])} variants)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
