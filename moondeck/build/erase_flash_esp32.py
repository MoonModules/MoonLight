#!/usr/bin/env python3
"""Erase the entire ESP32 flash. Useful when on-device state (e.g. /.config persistence)
is wedged and needs a fresh start — a module that crashes at boot is rebuilt from that state on
every boot, so an app reflash alone cannot break the loop.

Runs esptool directly rather than `idf.py erase-flash`. Erasing needs only a chip and a port, but
idf.py additionally validates the build directory, so it aborted on an unrelated python-env mismatch
("... is currently active while the project was configured with ...") and left the flash untouched.
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ESP32_DIR = ROOT / "esp32"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from idf_env import find_idf, idf_env, find_idf_python

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="Serial port")
    parser.add_argument("--firmware", help="Firmware variant, to name the chip. Omitted: detect.")
    args = parser.parse_args()

    if not ESP32_DIR.exists():
        print(f"ESP32 project directory not found: {ESP32_DIR}")
        sys.exit(1)

    idf_path = find_idf()
    if not idf_path:
        print("ESP-IDF not found. Install it or set IDF_PATH.")
        sys.exit(1)

    env = idf_env(idf_path)   # esptool comes from the IDF env; no idf.py wrapper needed

    fw = args.firmware or ""
    chip = "esp32s3" if "s3" in fw else "esp32p4" if "p4" in fw else "esp32" if fw else "auto"

    print(f"Erasing flash on {args.port} (chip: {chip})...")
    sys.stdout.flush()
    # esptool lives in the IDF's own venv, not in whatever interpreter is running this script.
    # find_idf_python returns the venv DIRECTORY; the interpreter is bin/python inside it
    # (Scripts/python.exe on Windows).
    venv = find_idf_python(idf_path)
    python = sys.executable
    if venv:
        for rel in ("bin/python", "Scripts/python.exe"):
            cand = Path(venv) / rel
            if cand.exists():
                python = str(cand)
                break
    r = subprocess.run([python, "-m", "esptool", "--chip", chip,
                        "--port", args.port, "erase_flash"],
                       cwd=ESP32_DIR, env=env)
    # Say which happened. Printing "Erasing..." and exiting non-zero read as success, so a failed
    # erase looked done and the next flash went onto un-erased state.
    print("Flash erased." if r.returncode == 0 else
          f"ERASE FAILED (exit {r.returncode}) - the flash was NOT erased.")
    sys.exit(r.returncode)

if __name__ == "__main__":
    main()
