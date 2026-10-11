#!/usr/bin/env python3
"""Build a Teensy 4.1 firmware into build/<firmware>/: MoonLight.elf, and MoonLight.hex for the loader.

`teensy41` drives the board's own Ethernet PHY, and `teensy41-w5500` a W5500 module for the 4.1-NE, which has no PHY.
Configures CMake on first use with the SDK setup_teensy.py installed, at the versions pinned there.
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from setup_teensy import gcc_bin, missing, sdk_root, teensyduino_define  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent
FIRMWARES = ("teensy41", "teensy41-w5500")
# What platform_teensy.cpp leaves the program of the 8 MB flash: LittleFS takes 1 MB and the reserved top 256 KB.
PROGRAM_FLASH_BYTES = (8192 - 1024 - 256) * 1024


def hex_path(firmware: str) -> Path:
    """Where a firmware's image lands, one build directory per firmware as on the ESP32."""
    return ROOT / "build" / firmware / "MoonLight.hex"


def add_firmware_arg(parser: argparse.ArgumentParser):
    """The one --firmware flag the build, flash and repo-health scripts share."""
    parser.add_argument("--firmware", choices=FIRMWARES, default=FIRMWARES[0],
                        help="teensy41: the board's Ethernet PHY; teensy41-w5500: a W5500 module (default: teensy41)")


def hex_image_bytes(path: Path) -> int:
    """The bytes an Intel HEX image writes to flash: the sum of its data records (type 00)."""
    total = 0
    for line in path.read_text().splitlines():
        if line.startswith(":") and line[7:9] == "00":
            total += int(line[1:3], 16)
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    # A release burns its channel and computed version in, as build_esp32.py does; a local build leaves both empty.
    parser.add_argument("--release", default="", help="the release channel tag, e.g. latest or v1.0.0")
    parser.add_argument("--version", default="", help="the computed semver (compute_version.py)")
    add_firmware_arg(parser)
    args = parser.parse_args()
    build_dir = hex_path(args.firmware).parent
    left = missing(loader=False)   # building needs no loader
    if left:
        sys.exit(f"Teensy SDK incomplete ({', '.join(left)}): run uv run moondeck/build/setup_teensy.py")
    r = subprocess.run(["cmake", "-S", str(ROOT / "teensy"), "-B", str(build_dir), "-G", "Ninja",
                        f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'teensy' / 'teensy41.cmake'}",
                        f"-DTEENSY_SDK={sdk_root()}", f"-DTEENSY_GCC_BIN={gcc_bin()}",
                        f"-DTEENSYDUINO={teensyduino_define()}",
                        f"-DMM_FIRMWARE={args.firmware}",
                        f"-DMM_RELEASE={args.release}", f"-DMM_VERSION={args.version}"], cwd=ROOT)
    if r.returncode != 0:
        sys.exit(r.returncode)
    sys.exit(subprocess.run(["cmake", "--build", str(build_dir)], cwd=ROOT).returncode)


if __name__ == "__main__":
    main()
