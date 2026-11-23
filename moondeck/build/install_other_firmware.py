#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Install another project's firmware on a board over USB: WLED, WLED-MM, Tasmota, ESPHome or Troy's WLED, the web installer's
other-firmware list, then give it the WiFi over Improv.

The files come from the installer's own rules (mooninstaller/other-firmwares.json, other-firmwares.js and manifest.js, run by
other_firmware_parts.mjs), so the bench and the web installer write the same files at the same offsets. esptool writes them,
its --flash-size re-flagging a bootloader built for another size, and improv_provision.py sends the host's WiFi.

Usage:
  uv run moondeck/build/install_other_firmware.py --port /dev/cu.usbserial-XXXX --firmware WLED --build ESP32
  uv run moondeck/build/install_other_firmware.py --port /dev/cu.usbserial-XXXX --firmware Tasmota
  uv run moondeck/build/install_other_firmware.py --port /dev/cu.usbserial-XXXX --firmware WLED --list

Exit codes: 0 = installed and online, 1 = installed with no address over Improv (Troy's Ethernet build has no Improv),
2 = stopped before writing.
"""

import argparse
import json
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
CACHE = ROOT / "build" / "other-firmware"
ESPTOOL = ["uvx", "--from", "esptool>=5", "esptool"]


def fail(msg: str) -> None:
    print(f"==> {msg}")
    sys.exit(2)


def esptool(port: str, *args: str, baud: str = "460800") -> str:
    r = subprocess.run([*ESPTOOL, "--port", port, "-b", baud, *args], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-400:], r.stderr[-400:])
        fail(f"esptool {args[0]} failed")
    return r.stdout


def board(port: str) -> dict:
    """The board as esptool reports it, the shape the installer's factsFromEsptool reads."""
    out = esptool(port, "flash-id")
    field = lambda name: (re.search(rf"^{name}:\s*(.+)$", out, re.M) or [None, ""])[1].strip()
    return {"description": field("Chip type") or field("Chip is"), "features": [f.strip() for f in field("Features").split(",") if f.strip()],
            "flashSize": field("Detected flash size")}


def plan(port: str, firmware: str, build: str | None, download: bool = True) -> dict:
    """What to write: the name, version and parts the installer's rules give for this board, each part downloaded into build/other-firmware/
    unless `download` is off."""
    facts = board(port)
    r = subprocess.run(["node", str(ROOT / "moondeck" / "build" / "other_firmware_parts.mjs")],
                       input=json.dumps({"firmware": firmware, "build": build, **facts}), capture_output=True, text=True)
    if r.returncode:
        fail(f"other_firmware_parts.mjs failed: {r.stderr[-400:]}")
    found = json.loads(r.stdout)
    found["board"] = facts
    if found.get("error") or not download:
        return found
    CACHE.mkdir(parents=True, exist_ok=True)
    for part in found["parts"]:
        part["file"] = CACHE / part["url"].rsplit("/", 1)[-1]
        # Each install takes the project's latest files; a named client, since ESPHome's host refuses Python's default one (HTTP 403).
        request = urllib.request.Request(part["url"], headers={"User-Agent": "MoonDeck install_other_firmware"})
        with urllib.request.urlopen(request, timeout=60) as r:
            part["file"].write_bytes(r.read())
    return found


def flash(port: str, found: dict, erase: list[tuple[str, str]] = (), extra: list[tuple[str, Path]] = (), baud: str = "460800") -> None:
    """Erase `erase` regions, then write the parts and any `extra` (offset, file) in one esptool run."""
    for start, size in erase:
        esptool(port, "erase-region", start, size, baud=baud)
    sizes = {p["flashSize"] for p in found["parts"] if p.get("flashSize")}
    files = [arg for p in found["parts"] for arg in (hex(p["offset"]), str(p["file"]))] + [arg for off, f in extra for arg in (off, str(f))]
    esptool(port, "write-flash", *(["--flash-size", sizes.pop()] if sizes else []), *files, baud=baud)


def provision(port: str, wait_provisioned: float = 0) -> str | None:
    """The device's address once Improv gave it the host's WiFi ("" when it joined and named none, as ESPHome's first-use
    firmware does), or None when the firmware did not answer."""
    time.sleep(2)
    wait = ["--wait-provisioned", str(wait_provisioned)] if wait_provisioned else []
    # A firmware that kept its WiFi reports provisioned while it rejoins after the flash, and names its address once it has one.
    for _ in range(4):
        r = subprocess.run(["uv", "run", str(ROOT / "moondeck" / "build" / "improv_provision.py"), "--port", port, *wait],
                           cwd=ROOT, capture_output=True, text=True)
        url = next((line.split("provisioned:", 1)[1].strip() for line in r.stdout.splitlines() if "provisioned:" in line), None)
        if url or "PROVISIONED already" not in r.stdout:
            break
        time.sleep(5)
    if url is None:
        print((r.stdout + r.stderr)[-600:])
    return url


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", required=True, help="Serial port of the board")
    ap.add_argument("--firmware", required=True, help="An other-firmwares.json entry: WLED, WLED-MM, Tasmota, ESPHome, Troy")
    ap.add_argument("--build", help="The build, for an entry with several for this board (--list names them)")
    ap.add_argument("--list", action="store_true", help="Name the builds for this board and write nothing")
    ap.add_argument("--no-wifi", action="store_true", help="Skip the Improv step")
    ap.add_argument("--baud", default="460800", help="esptool's baud rate (460800 holds on a USB-serial bridge)")
    args = ap.parse_args()

    found = plan(args.port, args.firmware, args.build, download=not args.list)
    if found.get("error") or args.list:
        print(f"==> {found.get('error') or found['name'] + ' ' + found['version']}")
        for choice in found.get("choices", []):
            print(f"    {choice}")
        return 0 if args.list else 2
    print(f"==> installing {found['name']} {found['version']} on {found['board']['description']}, {found['board']['flashSize']}")
    flash(args.port, found, baud=args.baud)
    if args.no_wifi:
        return 0
    url = provision(args.port)
    if url is None:
        print("==> written; it did not answer Improv on this port: with WiFi saved it joins on its own, "
              "otherwise it opens its own access point")
        return 1
    print(f"==> online at {url}" if url else "==> joined the WiFi; the firmware names no address")
    return 0


if __name__ == "__main__":
    sys.exit(main())
