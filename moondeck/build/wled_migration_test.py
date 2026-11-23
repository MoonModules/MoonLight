#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial", "littlefs-python"]
# ///
"""Test the over-the-air migration from WLED to MoonLight on a USB-connected bench board, a power cut at every step.

Each round installs the latest WLED as the web installer does (install_other_firmware.py: its bootloader, its partition table, an empty app choice and filesystem), joins it to the WiFi over Improv, uploads a migration image through WLED's own update page, and checks that the board ends up answering as MoonBase.

Rounds, in the order given to --rounds:
  ship  the shipped image, no cut
  move  the rehearsal image after a WLED self-update, so it lands where MoonBase goes and moves itself aside
  1..5  the rehearsal image, reset in that pause (after: the network read, MoonBase, the filesystem, the table, the app choice)

A cut during the table write itself is the one that needs a cable, which is why the image pauses after it rather than during it.

Run before a release that changes moonbase/migrate/; it rewrites the whole flash of the board on --port.

Usage:
  uv run moondeck/build/build_esp32.py --firmware esp32c3 --migrate-pause 10
  uv run moondeck/build/wled_migration_test.py --port /dev/cu.usbmodemXXXX
  uv run moondeck/build/build_esp32.py --firmware esp32 --migrate-pause 10
  uv run moondeck/build/wled_migration_test.py --chip esp32 --port /dev/cu.usbserial-XXXX

Exit codes: 0 = every round ends in MoonBase, 1 = a round did not, 2 = environment failure.
"""

import argparse
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

import json

import littlefs
import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import install_other_firmware as other   # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent

# The latest WLED build each test board takes, from the web installer's catalog; the S3 board is an N16R8, octal PSRAM.
WLED_BUILD = {"esp32": "ESP32", "esp32c3": "ESP32-C3", "esp32s3": "ESP32-S3_16MB_opi"}
# The S3 board's radio browns out at full power, so WLED boots with its own TX power setting at 8.5 dBm (wifi_power_t 34), written into its config.
TXPWR = {"esp32s3": 34}
# A USB-serial bridge, which does not hold 921600 as the C3's and the S3's own USB does.
BAUD = {"esp32": "460800"}
PAUSES = 5


def fail(msg: str) -> None:
    print(f"==> {msg}")
    sys.exit(2)


class Bench:
    def __init__(self, port: str, chip: str):
        self.port = port
        self.chip = chip
        self.ip = ""
        self.cache = ROOT / "build" / "wled-migration-test" / chip
        self.baud = BAUD.get(chip, "921600")

    # The latest WLED for this board: what install_other_firmware.py writes, and where WLED's table puts its filesystem.
    def wled(self) -> dict:
        found = other.plan(self.port, "WLED", WLED_BUILD[self.chip])
        if found.get("error"):
            fail(found["error"])
        table = next(p["file"] for p in found["parts"] if p["offset"] == 0x8000).read_bytes()
        for i in range(0, len(table), 32):   # 32-byte entries: magic, type, subtype, offset, size, label
            entry = table[i:i + 32]
            if entry[:2] != b"\xaa\x50":
                break
            if entry[2] == 1 and entry[3] in (0x82, 0x83):   # data: spiffs or littlefs
                found["fs"] = (int.from_bytes(entry[4:8], "little"), int.from_bytes(entry[8:12], "little"))
                return found
        fail("WLED's partition table names no filesystem")

    def page(self) -> str | None:
        try:
            with urllib.request.urlopen(f"http://{self.ip}/json/info", timeout=2) as r:
                if b'"ver"' in r.read(200):
                    return "WLED"
        except Exception:
            pass
        try:
            with urllib.request.urlopen(f"http://{self.ip}/", timeout=2) as r:
                text = r.read(400).decode("utf-8", "replace")
        except Exception:
            return None
        for name in ("MoonBase", "MoonLight", "WLED"):
            if name in text:
                return name
        return "other"

    def wait_for(self, name: str, secs: float) -> str | None:
        end = time.time() + secs
        while time.time() < end:
            if self.page() == name:
                return name
            time.sleep(1)
        return self.page()

    def upload(self, image: Path) -> None:
        # The fields the device UI's map sends, skipValidation ahead of the file, which WLED 16 needs to take an image that is not its own.
        subprocess.run(["curl", "-s", "-m", "120", "-o", "/dev/null", "-F", "skipValidation=1", "-F", f"update=@{image}",
                        f"http://{self.ip}/update"])

    # WLED's filesystem holding its own config, cfg.json and wsec.json: our network, and the TX power the board needs, so WLED boots on the network at that power.
    # The LittleFS settings of the Arduino core WLED's S3 build ran at 0.15 (2.0.9): 4 KB blocks, 128-byte reads and writes, 64-character names, on-disk
    # version 2.0, which a later core's littlefs mounts too, since littlefs reads every older minor version of its disk format.
    def wled_config_image(self, txpwr: int, fs_size: int) -> Path:
        from host_wifi import get_host_wifi
        ssid, password = get_host_wifi()
        if not ssid or not password:
            fail("no host WiFi credentials: run moondeck/build/host_wifi.py first")
        self.cache.mkdir(parents=True, exist_ok=True)
        fs = littlefs.LittleFS(block_size=4096, block_count=fs_size // 4096, read_size=128, prog_size=128,
                               lookahead_size=128, cache_size=512, block_cycles=512, name_max=64, disk_version=0x00020000)
        network = {"ssid": ssid, "pskl": len(password), "psk": password, "ip": [0, 0, 0, 0], "gw": [0, 0, 0, 0], "sn": [255, 255, 255, 0]}
        with fs.open("/cfg.json", "w") as f:
            f.write(json.dumps({"rev": [1, 0], "nw": {"ins": [network]}, "wifi": {"txpwr": txpwr}}, separators=(",", ":")))   # compact, as WLED writes it
        with fs.open("/wsec.json", "w") as f:
            f.write(json.dumps({"nw": {"ins": [{"psk": password}]}}, separators=(",", ":")))
        image = self.cache / "wled-fs.bin"   # holds the WiFi password: build/ is ignored by git, and the next run overwrites it
        image.write_bytes(fs.context.buffer)
        return image

    def as_wled(self, wled: dict, self_update: bool) -> None:
        fs_start, fs_size = wled["fs"]
        config = [(hex(fs_start), self.wled_config_image(TXPWR[self.chip], fs_size))] if self.chip in TXPWR else []
        other.flash(self.port, wled, erase=[("0xe000", "0x2000"), (hex(fs_start), hex(fs_size))], extra=config, baud=self.baud)
        self.ip = (other.provision(self.port, 30 if config else 0) or "").removeprefix("http://").strip("/")   # WLED joins from the config written into it
        if not self.ip:
            fail("WLED did not report an address over Improv")
        if self.wait_for("WLED", 40) != "WLED":
            fail(f"WLED is not answering at {self.ip}")
        try:   # what WLED runs with, as it saved it: its TX power among it
            with urllib.request.urlopen(f"http://{self.ip}/json/cfg", timeout=5) as r:
                print(f"==> WLED's TX power setting: {json.loads(r.read()).get('wifi', {}).get('txpwr')}")
        except Exception as e:
            print(f"==> WLED's config did not read: {e}")
        if self_update:   # WLED then runs from app1, so the image lands in app0, where MoonBase goes
            self.upload(next(p["file"] for p in wled["parts"] if p["offset"] == 0x10000))
            time.sleep(5)
            if self.wait_for("WLED", 60) != "WLED":
                fail("WLED did not come back after its own update")

    # Upload `image`, reset in pause `cut` (0 = never), and say what the board answers as.
    def round(self, wled: dict, image: Path, cut: int, self_update: bool) -> tuple[str | None, list[str]]:
        self.as_wled(wled, self_update)
        s = serial.Serial()
        s.port, s.baudrate, s.timeout, s.dtr, s.rts = self.port, 115200, 0.2, False, False
        s.open()
        time.sleep(1)
        if self.wait_for("WLED", 40) != "WLED":
            fail("WLED went away when the port opened")
        log: list[str] = []
        threading.Thread(target=self.upload, args=(image,), daemon=True).start()
        pauses, reset_at, buf = 0, None, b""
        end = time.time() + 120
        while time.time() < end:
            buf += s.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip()
                if "migrate" in line or "rst:" in line:
                    log.append(line)
                if "pausing" in line and reset_at is None:
                    pauses += 1
                    if pauses == cut:
                        s.rts = True
                        time.sleep(0.1)
                        s.rts = False
                        reset_at = time.time()
                        log.append(f"--- reset in pause {cut} ---")
            if reset_at and time.time() - reset_at > 75:
                break
        s.close()
        return self.wait_for("MoonBase", 30), log


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", required=True, help="Serial port of the bench board")
    ap.add_argument("--chip", default="esp32c3", choices=sorted(WLED_BUILD), help="The board's chip")
    ap.add_argument("--rounds", default="ship,move,1,2,3,4,5",
                    help="Comma-separated rounds: ship, move, or a pause number 1-5")
    args = ap.parse_args()

    shipped = ROOT / "build" / f"migrate-{args.chip}" / "MoonLight-migrate.bin"
    paused = ROOT / "build" / f"migrate-{args.chip}-pause" / "MoonLight-migrate.bin"
    rounds = [r.strip() for r in args.rounds.split(",") if r.strip()]
    if any(r != "ship" for r in rounds) and not paused.exists():
        fail(f"no rehearsal image: run build_esp32.py --firmware {args.chip} --migrate-pause 10 first")
    if "ship" in rounds and not shipped.exists():
        fail(f"no shipped image: run build_esp32.py --firmware {args.chip} first")

    bench = Bench(args.port, args.chip)
    wled = bench.wled()
    results = []
    for r in rounds:
        if r == "ship":
            outcome, log = bench.round(wled, shipped, 0, False)
            label = "shipped image, no cut"
        elif r == "move":
            outcome, log = bench.round(wled, paused, 0, True)
            label = "move aside, no cut"
        elif r.isdigit() and 1 <= int(r) <= PAUSES:
            outcome, log = bench.round(wled, paused, int(r), False)
            label = f"reset in pause {r}"
        else:
            fail(f"unknown round {r!r}")
        ok = outcome == "MoonBase"
        results.append(ok)
        print(f"{'PASS' if ok else 'FAIL'} {label}: the board answers as {outcome}")
        for line in log:
            print(f"     {line}")
        sys.stdout.flush()
    print(f"\n{sum(results)} of {len(results)} rounds end in MoonBase")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
