#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Monitor a Teensy 4.1's USB serial output, saved to build/teensy41/monitor.log.

Waits for the port to appear, so it runs straight after flash_teensy.py and catches the boot lines:
the firmware waits up to two seconds for a monitor before it starts.
Opening a Teensy's port does not reset it.
"""

import argparse
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "build"))
from flash_teensy import teensy_port  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent
LOG = ROOT / "build" / "teensy41" / "monitor.log"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="the Teensy's serial port; found by its USB vendor id when omitted")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long; 0 runs until interrupted")
    ap.add_argument("--wait", type=float, default=30, help="how long to wait for the port to appear")
    args = ap.parse_args()

    deadline = time.time() + args.wait
    conn = None
    while conn is None:
        port = args.port or teensy_port()
        try:
            conn = serial.Serial(port, 115200, timeout=0.2) if port else None
        except serial.SerialException:
            conn = None
        if conn is None:
            if time.time() > deadline:
                sys.exit("No Teensy serial port appeared")
            time.sleep(0.05)
    print(f"Monitoring {conn.port}, saving to {LOG.relative_to(ROOT)}", flush=True)

    LOG.parent.mkdir(parents=True, exist_ok=True)
    stop = time.time() + args.seconds if args.seconds else None
    with open(LOG, "w", encoding="utf-8") as log:
        try:
            while stop is None or time.time() < stop:
                data = conn.read(4096)
                if data:
                    text = data.decode("utf-8", "replace")
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    log.write(text)
                    log.flush()   # what MoonDeck showed survives a stop before the buffer fills
        except (KeyboardInterrupt, serial.SerialException):
            pass


if __name__ == "__main__":
    main()
