#!/usr/bin/env python3
"""Flash a built ESP32 firmware to a device.

Reads the app image in ``build/esp32-<firmware>/``. The per-firmware build dir
(written by ``build_esp32.py``) makes "which firmware am I flashing" an
on-disk fact rather than an in-memory marker — switching firmwares is a
``--firmware`` change, not a clean-rebuild.

Prints the artifact size + age before flashing so a stale build (one
from yesterday vs an edit five minutes ago) is visible in the log.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ESP32_DIR = ROOT / "esp32"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_esp32 import find_idf, idf_env, idf_cmd, FIRMWARES, build_dir_for, APP_BIN


CATALOG = ROOT / "mooninstaller" / "deviceModels.json"
# MoonDeck / CLI flashing defaults FAST: this path is the DIY bench, where the operator
# knows their board and the USB bridge is almost always a modern one that sustains 921600
# (~2x faster). A board with a flaky bridge opts DOWN via its catalog `flashBaud` (e.g. a
# CH340 clone that stalls at 921600). This is the opposite of the web installer, which
# defaults to the safe 460800 because it serves unknown walk-up hardware (see
# install-orchestrator.js) — same catalog, different audience, different default.
DEFAULT_FLASH_BAUD = 921600


def _catalog_flash_baud(firmware: str, device_model: str | None = None) -> int:
    """The flash baud to use, from the deviceModel catalog.

    A deviceModel pins its own baud with a `flashBaud` field — down for a flaky bridge
    (a CH340 clone at 460800) or up/explicit for a verified one (the S31 at 921600).

    Resolution:
      - When the EXACT `device_model` is known (MoonDeck maps it from the port's device):
        that entry's `flashBaud` if it sets one, else DEFAULT_FLASH_BAUD. Keying on the
        exact board stops a per-model opt-down leaking to firmware-siblings — the LOLIN's
        460800 must not slow the Dig-Uno, which shares the `esp32` firmware but has a fine
        bridge and no flashBaud of its own (so it gets the fast default, not the LOLIN's).
      - When no device_model is given (a plain --firmware flash): the LOWEST `flashBaud`
        among models sharing this firmware, so an opt-down still protects a board flashed
        without a known model; else DEFAULT_FLASH_BAUD.
    An unreadable catalog always yields DEFAULT_FLASH_BAUD.
    """
    import json
    try:
        catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return DEFAULT_FLASH_BAUD
    if device_model:
        for e in catalog:
            if isinstance(e, dict) and e.get("name") == device_model:
                b = e.get("flashBaud")
                return b if isinstance(b, int) else DEFAULT_FLASH_BAUD
        # device_model given but not in the catalog — fall through to firmware/default.
    bauds = {
        e["flashBaud"] for e in catalog
        if isinstance(e, dict) and firmware in (e.get("firmwares") or [])
        and isinstance(e.get("flashBaud"), int)
    }
    return min(bauds) if bauds else DEFAULT_FLASH_BAUD


def _fmt_age(seconds: float) -> str:
    """Compact human-readable age (5s, 12m, 3h, 2d)."""
    s = int(seconds)
    if s < 60:    return f"{s}s"
    if s < 3600:  return f"{s // 60}m"
    if s < 86400: return f"{s // 3600}h"
    return f"{s // 86400}d"


def _moonbase_flash_cmd(build_dir, firmware: str, port: str, baud: int, env: dict) -> list[str]:
    """The explicit write list for a MoonBase-layout flash, moonbase_flash_files() is the one
    place that knows the corrected layout (app remapped to ota_0, slot-0 otadata so the fresh
    flash boots the app, MoonBase at factory). idf.py flash cannot be used here: IDF's own
    flash_args stages the app at the factory offset."""
    from build_esp32 import moonbase_flash_files
    try:
        writes = moonbase_flash_files(firmware, build_dir)
    except FileNotFoundError as e:
        print(f"MoonBase flash: {e}")
        sys.exit(1)
    chip_m = re.search(r'CONFIG_IDF_TARGET="([^"]+)"', (build_dir / "sdkconfig").read_text())
    if not chip_m:
        print(f"MoonBase flash: no CONFIG_IDF_TARGET in {build_dir / 'sdkconfig'}; rebuild first.")
        sys.exit(1)
    py = shutil.which("python", path=env.get("PATH", "")) or sys.executable
    cmd = [py, "-m", "esptool", "--chip", chip_m.group(1), "--port", port, "--baud", str(baud),
           "write_flash"]
    for off, path in writes:
        cmd += [off, str(path)]
    return cmd

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", required=True, help="Serial port")
    parser.add_argument("--firmware", required=True, choices=sorted(FIRMWARES),
                        help="Firmware variant to flash. The build for this "
                             "firmware must exist at build/esp32-<firmware>/ — "
                             "i.e. you must have run Build with the same "
                             "--firmware first.")
    parser.add_argument("--baud", type=int, default=None,
                        help="esptool flash baud rate. When omitted, defaults to 921600 "
                             "(~2x faster — the DIY-bench assumption is a modern bridge), "
                             "unless the deviceModel's catalog 'flashBaud' pins it lower "
                             "for a flaky bridge (a CH340 clone that stalls at 921600 with "
                             "'chip stopped responding'). Pass --baud to override either. "
                             "(The web installer defaults to 460800 instead — it serves "
                             "unknown walk-up hardware.)")
    parser.add_argument("--device-model", dest="device_model", default=None,
                        help="The deviceModel of the board being flashed (MoonDeck passes "
                             "it, mapped from the port's device). Lets the baud resolve by "
                             "the EXACT board so a per-model flashBaud opt-down doesn't leak "
                             "to firmware-siblings. Optional — omitted flashes resolve by "
                             "firmware then the fast default.")
    args = parser.parse_args()

    # Default fast (921600) for the DIY bench; a deviceModel with a flaky bridge pins its
    # own `flashBaud` in the catalog to slow down. An explicit --baud always wins; else the
    # catalog value (exact deviceModel first, then lowest across firmware-siblings) applies;
    # else the fast default. Data-driven per-board, like the rest of deviceModels.json.
    baud = args.baud if args.baud is not None \
        else _catalog_flash_baud(args.firmware, args.device_model)

    if not ESP32_DIR.exists():
        print(f"ESP32 project directory not found: {ESP32_DIR}")
        sys.exit(1)

    build_dir = build_dir_for(args.firmware)
    image = build_dir / APP_BIN

    if not image.exists():
        print(f"ERROR: no build for {args.firmware!r} at "
              f"{build_dir.relative_to(ROOT)}/.")
        print(f"       Run Build with --firmware {args.firmware} first, then "
              f"Flash again.")
        sys.exit(2)

    size_kb = image.stat().st_size // 1024
    age = _fmt_age(time.time() - image.stat().st_mtime)
    print(f"==> flashing {args.firmware} build ({size_kb} KB, built {age} ago) "
          f"to {args.port}")

    idf_path = find_idf()
    if not idf_path:
        print("ESP-IDF not found. Install it or set IDF_PATH.")
        sys.exit(1)

    env = idf_env(idf_path)
    cmd = idf_cmd(idf_path)
    # -B + -DSDKCONFIG mirror build_esp32.py so idf.py flash reads the
    # per-firmware sdkconfig (the chip target lives in there). Without
    # -DSDKCONFIG, idf.py reads esp32/sdkconfig at the project root,
    # which may belong to a different firmware.
    b_arg = [
        "-B", str(build_dir),
        "-DSDKCONFIG=" + str(build_dir / "sdkconfig"),
    ]

    # -b sets the esptool flash baud (idf.py's own default is also 460800).
    # --baud 921600 matches the web installer for ~2x speed, but isn't the
    # default because some USB bridges can't sustain it (see --baud help).
    # Tee the output: esptool prints the board's efuse MAC during its connect
    # phase ("MAC: xx:xx:..."), which we parse to key the flash breadcrumb by the
    # board's stable identity — so MoonDeck records last_port on the exact device,
    # even when two boards share a firmware. Streaming keeps the live progress on
    # the console; we just also scan it.
    print(f"==> flash baud: {baud}")
    mac = ""
    # A MoonBase variant cannot use `idf.py flash`: with a factory + ota_0 table, IDF stages the
    # application at the FACTORY offset (0x10000), which is MoonBase's slot and too small for it.
    # The parts are placed at explicit offsets instead, the same shape the mooninstaller manifest
    # uses, with each offset read from the built partition table rather than hardcoded.
    if FIRMWARES.get(args.firmware, {}).get("moonbase"):
        flash_cmd = _moonbase_flash_cmd(build_dir, args.firmware, args.port, baud, env)
    else:
        flash_cmd = cmd + b_arg + ["flash", "-p", args.port, "-b", str(baud)]
    proc = subprocess.Popen(flash_cmd,
                            cwd=ESP32_DIR, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in proc.stdout:
        sys.stdout.write(line)          # keep the live flash output visible
        if not mac:
            m = re.search(r"MAC:\s*([0-9A-Fa-f:]{17})", line)
            if m:
                mac = m.group(1).upper()
    proc.wait()
    if proc.returncode == 0:
        _record_flash_event(args.port, args.firmware, mac)
    sys.exit(proc.returncode)


def _record_flash_event(port: str, firmware: str, mac: str) -> None:
    """Record the just-flashed serial port against the exact device, keyed by `mac`
    (the board's efuse MAC parsed from esptool's flash output — the stable identity,
    unambiguous when two boards share a firmware).

    Two writes, because two callers consume this:
    - `moondeck.json` `last_port` is set DIRECTLY here, so a CLI flash (no MoonDeck
      GUI running) still records the port. Without this, `last_port` only ever got
      written by MoonDeck's discover/refresh, so a board flashed purely from the CLI
      (the agent path) never gained a `last_port` and every later flash had to
      re-probe every serial port to find it.
    - the `moondeck/.last_flash.json` breadcrumb is still dropped for MoonDeck's
      discover/refresh, which additionally records the drift-immune `usbSerial` and
      strips the port from stale holders (see moondeck.py `_link_last_flash`)."""
    import json, time
    _set_last_port_in_catalog(mac, port)
    marker = ROOT / "moondeck" / ".last_flash.json"
    marker.write_text(json.dumps({
        "port": port,
        "firmware": firmware,
        "mac": mac,
        "ts": time.time(),
    }))


def _set_last_port_in_catalog(mac: str, port: str) -> None:
    """Set `last_port` on the moondeck.json device with this MAC, and strip the port
    from any OTHER device that still carries it (a physical port maps to exactly one
    board at a time; boards get swapped on the same USB port). No MAC, or no matching
    device, is a no-op — the breadcrumb path still covers the MoonDeck-GUI case.

    Matching goes through MoonDeck's `_mac_matches`, not plain equality: esptool
    reports the board's raw EFUSE MAC, but an S31 device REPORTS the EUI-64-truncated
    form of it (FF:FE inserted after the OUI, cut to 6 bytes) — the same alias the
    breadcrumb path already tolerates. A plain compare would silently never link the
    S31's port (the exact regression `_mac_matches` exists for)."""
    import json
    if not mac:
        return
    sys.path.insert(0, str(ROOT / "moondeck"))
    from moondeck import _mac_matches
    catalog = ROOT / "moondeck" / "moondeck.json"
    try:
        data = json.loads(catalog.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return
    changed = False
    for network in data.get("networks", []):
        for device in network.get("devices", []):
            same = _mac_matches(mac, device.get("mac", "") or "")
            if same:
                if device.get("last_port") != port:
                    device["last_port"] = port
                    changed = True
            elif device.get("last_port") == port:   # stale link on a swapped-out board
                device.pop("last_port", None)
                changed = True
    if changed:
        catalog.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
