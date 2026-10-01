#!/usr/bin/env python3
"""Clean one or all ESP32 per-firmware build directories.

Each firmware has its own ``build/esp32-<firmware>/`` (managed by ``build_esp32.py``), and a firmware that carries MoonBase also builds ``build/moonbase-<chip>/``, shared by every firmware for that chip.
This script removes one firmware's directories, or every ``build/esp32-*/`` and ``build/moonbase-*/`` plus a stale legacy ``esp32/build/`` if present.

The MoonBase directory goes with its firmware because a clean is for a cache that no longer matches the tree, a moved checkout or a changed toolchain, and the MoonBase cache goes stale in the same moment.
Left behind, it fails the next build of that firmware with the old path in its cache.
"""

import argparse
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ESP32_DIR = ROOT / "esp32"
BUILD_ROOT = ROOT / "build"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_esp32 import FIRMWARES, build_dir_for


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--firmware", choices=sorted(FIRMWARES),
                   help="Remove build/esp32-<firmware>/ for the named firmware, and its "
                        "chip's build/moonbase-<chip>/ when the firmware carries MoonBase.")
    g.add_argument("--all", action="store_true",
                   help="Remove every build/esp32-*/ and build/moonbase-*/ directory, "
                        "plus the legacy esp32/build/ dir if it still exists.")
    args = ap.parse_args()

    targets: list[Path] = []
    if args.firmware:
        targets.append(build_dir_for(args.firmware))
        spec = FIRMWARES[args.firmware]
        if spec.get("moonbase"):
            targets.append(BUILD_ROOT / f"moonbase-{spec['chip']}")
    else:
        if BUILD_ROOT.exists():
            targets.extend(sorted(BUILD_ROOT.glob("esp32-*")))
            targets.extend(sorted(BUILD_ROOT.glob("moonbase-*")))
        # Sweep the legacy single-dir layout on its way out.
        legacy = ESP32_DIR / "build"
        if legacy.exists():
            targets.append(legacy)

    if not targets:
        print("Nothing to clean.")
        return

    for path in targets:
        if path.exists():
            shutil.rmtree(path)
            print(f"  removed {path.relative_to(ROOT)}")
        else:
            print(f"  (skip {path.relative_to(ROOT)}: already gone)")


if __name__ == "__main__":
    main()
