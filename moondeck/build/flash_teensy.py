#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Flash a built firmware's MoonLight.hex onto a Teensy 4.1 over USB with teensy_loader_cli.

A running MoonLight is asked into its bootloader through its USB serial port, so no button press is needed.
A board without a running firmware waits for its program button.
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial
from serial.tools import list_ports

sys.path.insert(0, str(Path(__file__).resolve().parent))
from setup_teensy import loader_path  # noqa: E402
from build_teensy import add_firmware_arg, hex_path  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent
PJRC_VID = 0x16C0
# PJRC's convention: the core reboots into its bootloader when the USB serial port is set to 134 baud.
BOOTLOADER_BAUD = 134
LOADER_TIMEOUT_S = 60


def teensy_port():
    """The first Teensy USB serial port, or None."""
    return next((p.device for p in list_ports.comports() if p.vid == PJRC_VID), None)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="the Teensy's serial port; found by its USB vendor id when omitted")
    add_firmware_arg(ap)
    args = ap.parse_args()
    image = hex_path(args.firmware)
    if not image.exists():
        sys.exit(f"No {image.relative_to(ROOT)}: run uv run moondeck/build/build_teensy.py --firmware {args.firmware}")
    if not loader_path().exists():
        sys.exit("No teensy_loader_cli: run uv run moondeck/build/setup_teensy.py")
    print(f"Flashing {args.firmware} ({image.relative_to(ROOT)})", flush=True)
    port = args.port or teensy_port()
    if port:
        print(f"Asking the Teensy on {port} into its bootloader ...", flush=True)
        try:
            serial.Serial(port, BOOTLOADER_BAUD).close()
        except serial.SerialException as e:
            print(f"  {e}; press the program button instead", flush=True)
        time.sleep(0.5)
    else:
        print("No running Teensy found: press its program button", flush=True)
    try:
        r = subprocess.run([str(loader_path()), "--mcu=TEENSY41", "-w", "-v", str(image)], timeout=LOADER_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        r = None
    if r is None or r.returncode != 0:
        sys.exit("The loader did not reach the Teensy's bootloader: check the USB cable carries data,"
                 " then press the program button and flash again.")


if __name__ == "__main__":
    main()
