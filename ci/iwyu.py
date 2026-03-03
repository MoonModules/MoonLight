#!/usr/bin/env python3
"""
Run Include-What-You-Use (IWYU) on MoonLight source files.

Informational only — exits 0 regardless of findings so CI never fails.
Files that cannot compile without the ESP32 toolchain are skipped.

Usage:
    python3 ci/iwyu.py [--verbose]

Prerequisites (CI installs these):
    sudo apt-get install -y iwyu
    pio pkg install -e native   # fetches FastLED headers
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).parent.parent
SRC = ROOT / "src"
FASTLED_SRC = ROOT / ".pio/libdeps/esp32-s3/FastLED/src"
FASTLED_TESTS = ROOT / ".pio/libdeps/esp32-s3/FastLED/tests"

VERBOSE = "--verbose" in sys.argv

# Include flags matching [env:native] in platformio.ini
INCLUDE_FLAGS = [
    "-std=c++17",
    f"-I{SRC}/MoonBase",
    f"-I{SRC}/MoonLight",
    f"-I{SRC}",
    "-DPLATFORMIO=1",
    "-DFT_MOONBASE=1",
    "-DFT_MOONLIGHT=1",
    "-DFT_ENABLED(x)=x",
]
if FASTLED_SRC.exists():
    INCLUDE_FLAGS += [f"-I{FASTLED_SRC}", f"-I{FASTLED_TESTS}"]
else:
    print("NOTE: FastLED headers not found — run 'pio pkg install -e native' first")


def run_iwyu(path: Path) -> tuple[bool, str]:
    """Run IWYU on one file. Returns (compiled_ok, iwyu_suggestions)."""
    cmd = ["include-what-you-use", *INCLUDE_FLAGS, str(path)]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        # IWYU writes suggestions to stderr; stdout is unused
        # returncode 0  → compiled OK (may still have suggestions)
        # returncode ≠ 0 → compilation failed (ESP32-specific header missing, etc.)
        return result.returncode == 0, result.stderr
    except subprocess.TimeoutExpired:
        return False, "(timeout)"


files = sorted(list(SRC.rglob("*.h")) + list(SRC.rglob("*.cpp")))
print(f"IWYU: analyzing {len(files)} files under src/ ...")
print(f"      include flags: {' '.join(f for f in INCLUDE_FLAGS if f.startswith('-I'))}")
print()

analyzed, skipped, with_suggestions = 0, 0, 0

for f in files:
    try:
        ok, output = run_iwyu(f)
    except FileNotFoundError:
        print("ERROR: 'include-what-you-use' not found.")
        print("  Install: sudo apt-get install -y iwyu")
        sys.exit(0)

    if ok:
        analyzed += 1
        has_suggestions = "should add" in output or "should remove" in output
        if has_suggestions:
            with_suggestions += 1
            print(f"{'─' * 60}")
            print(f"  {f.relative_to(ROOT)}")
            print(output.strip())
            print()
        elif VERBOSE:
            print(f"  OK  {f.relative_to(ROOT)}")
    else:
        skipped += 1
        if VERBOSE:
            print(f"  SKIP (cannot compile without ESP32 toolchain): {f.relative_to(ROOT)}")

print(f"{'=' * 60}")
print(f"IWYU: {analyzed} analyzed, {skipped} skipped (ESP32/Arduino-specific),")
print(f"      {with_suggestions} file(s) have include suggestions")
print("(Informational only — CI will not fail on IWYU findings)")
sys.exit(0)
