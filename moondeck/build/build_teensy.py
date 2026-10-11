#!/usr/bin/env python3
"""Build the Teensy 4.1 firmware into build/teensy41/: MoonLight.elf, and MoonLight.hex for the loader.

Configures CMake on first use with the SDK setup_teensy.py installed, at the versions pinned there.
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from setup_teensy import gcc_bin, missing, sdk_root, teensyduino_define  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_DIR = ROOT / "build" / "teensy41"
HEX = BUILD_DIR / "MoonLight.hex"
# What platform_teensy.cpp leaves the program of the 8 MB flash: LittleFS takes 1 MB and the reserved top 256 KB.
PROGRAM_FLASH_BYTES = (8192 - 1024 - 256) * 1024


def hex_image_bytes(path: Path) -> int:
    """The bytes an Intel HEX image writes to flash: the sum of its data records (type 00)."""
    total = 0
    for line in path.read_text().splitlines():
        if line.startswith(":") and line[7:9] == "00":
            total += int(line[1:3], 16)
    return total


def main():
    argparse.ArgumentParser(description=__doc__).parse_args()
    left = missing()
    if left:
        sys.exit(f"Teensy SDK incomplete ({', '.join(left)}): run uv run moondeck/build/setup_teensy.py")
    r = subprocess.run(["cmake", "-S", str(ROOT / "teensy"), "-B", str(BUILD_DIR), "-G", "Ninja",
                        f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'teensy' / 'teensy41.cmake'}",
                        f"-DTEENSY_SDK={sdk_root()}", f"-DTEENSY_GCC_BIN={gcc_bin()}",
                        f"-DTEENSYDUINO={teensyduino_define()}"], cwd=ROOT)
    if r.returncode != 0:
        sys.exit(r.returncode)
    sys.exit(subprocess.run(["cmake", "--build", str(BUILD_DIR)], cwd=ROOT).returncode)


if __name__ == "__main__":
    main()
