#!/usr/bin/env python3
"""Check that mooninstaller/firmwares.json and install-picker.js's DESKTOP_BUILDS are in sync with the FIRMWARES dict.

firmwares.json is a generated projection of build_esp32.py's FIRMWARES (the single
source of truth), read by the CI release matrix, the ESP Web Tools manifest loops,
and MoonDeck. This guard regenerates the projection in-memory and fails if the
committed file drifted — so editing FIRMWARES without regenerating is caught at
commit time, not at release. The catalog counterpart to check_devices.py.

Exit 1 on drift (with the regenerate command), mirroring check_devices.py.
"""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
COMMITTED = ROOT / "mooninstaller" / "firmwares.json"

# Reuse the generator's projection so the checker and generator can't disagree.
sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from generate_firmwares import PICKER, build_doc, picker_source  # noqa: E402
sys.path.insert(0, str(ROOT / "moondeck" / "check"))
import _findings  # noqa: E402

REL = COMMITTED.relative_to(ROOT).as_posix()
RULES = {
    "unreadable list": {"reason": "firmwares.json cannot be read or parsed, so it cannot be compared.", "fix": "Regenerate it with generate_firmwares.py."},
    "firmware list drift": {"reason": "firmwares.json is a projection of FIRMWARES and differs from it; the release matrix and the manifests read it.", "fix": "uv run moondeck/build/generate_firmwares.py --out mooninstaller/firmwares.json"},
}


def main():
    expected = build_doc()
    n = len(expected["firmwares"])

    try:
        actual = json.loads(COMMITTED.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        _findings.write("check_firmwares", RULES, [{"rule": "unreadable list", "message": str(e), "file": REL, "level": "error"}])
        print(f"Firmware check: cannot read {COMMITTED.relative_to(ROOT)}: {e}")
        sys.exit(1)

    # Compare parsed dicts, not bytes — formatting (ensure_ascii, trailing
    # newline) can't cause spurious drift; only the data matters.
    if actual != expected:
        _findings.write("check_firmwares", RULES, [{"rule": "firmware list drift", "message": f"{REL} differs from the {n} variants of FIRMWARES", "file": REL, "level": "error"}])
        print(f"Firmware check: {n} variants, DRIFT")
        print("  mooninstaller/firmwares.json is stale — regenerate with:")
        print("  uv run moondeck/build/generate_firmwares.py --out mooninstaller/firmwares.json")
        sys.exit(1)

    picker = PICKER.read_text(encoding="utf-8")
    if picker != picker_source(picker, expected):
        _findings.write("check_firmwares", RULES, [{"rule": "firmware list drift", "message": "install-picker.js DESKTOP_BUILDS differs from the desktop rows of FIRMWARES", "file": PICKER.relative_to(ROOT).as_posix(), "level": "error"}])
        print(f"Firmware check: {n} variants, DRIFT in install-picker.js")
        print("  regenerate with: uv run moondeck/build/generate_firmwares.py --out mooninstaller/firmwares.json")
        sys.exit(1)

    _findings.write("check_firmwares", RULES, [])
    print(f"Firmware check: {n} variants, 0 issue(s)")
    print("Firmware list valid.")


if __name__ == "__main__":
    main()
