#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial", "littlefs-python"]
# ///
"""Test the over-the-air migration from WLED to MoonLight on a USB-connected bench board, a power cut at every step.

Each round puts the board back in the state a WLED device ships in (WLED 0.15.0, its bootloader and its partition table), joins it to the WiFi over Improv, uploads a migration image through WLED's own update page, and checks that the board ends up answering as MoonBase.

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
import shutil
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
from build_esp32 import find_idf, idf_env   # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent

# What a WLED device of each chip ships with: the release app, the bootloader of the Arduino core WLED 0.15.0 builds that chip on, where the
# chip's ROM looks for that bootloader, the flash it runs (speed and size), WLED's table and its filesystem's span, erased so WLED starts afresh.
# The classic ESP32 builds on core 1.0.6.4 (ESP-IDF 3.3), the C3 and the S3 on 2.0.9; the S3's bootloader comes from WLED's web installer.
WLED_4MB = {"table": ROOT / "esp32" / "partitions" / "wled_4mb_1mb_fs.csv", "fs": ("0x310000", "0xf0000"), "flash_size": "4MB"}
# The bootloader's header always says DIO, as ESP-IDF writes it: the boot ROM reads the bootloader before anything sets the flash chip's quad-enable bit, and the bootloader switches to QIO itself.
BOOTLOADER_MODE = "dio"
WLED = {
    "esp32": {
        "app": "https://github.com/wled/WLED/releases/download/v0.15.0/WLED_0.15.0_ESP32.bin",
        "bootloader_bin": "https://raw.githubusercontent.com/Aircoookie/arduino-esp32/1.0.6.4/tools/sdk/bin/bootloader_dio_40m.bin",
        "bootloader_at": "0x1000",
        "flash_freq": "40m",
        "baud": "460800",   # a USB-serial bridge, which does not hold 921600 as the C3's own USB does
        **WLED_4MB,
    },
    "esp32c3": {
        "app": "https://github.com/wled/WLED/releases/download/v0.15.0/WLED_0.15.0_ESP32-C3.bin",
        "bootloader_elf": "https://raw.githubusercontent.com/espressif/arduino-esp32/2.0.9/tools/sdk/esp32c3/bin/bootloader_dio_80m.elf",
        "bootloader_at": "0x0",
        "flash_freq": "80m",
        "baud": "921600",
        **WLED_4MB,
    },
    # The 16 MB build with octal PSRAM, an N16R8 board's: its 16 MB table.
    "esp32s3": {
        "app": "https://github.com/wled/WLED/releases/download/v0.15.0/WLED_0.15.0_ESP32-S3_16MB_opi.bin",
        # WLED's web installer's own S3 bootloader for quad flash, whose flash size the write sets.
        "bootloader_bin": "https://install.wled.me/bin/boot/bootloaders/esp32-s3/bootloader_s3.bin",
        "bootloader_at": "0x0",
        "flash_freq": "80m",
        "baud": "921600",
        "table": ROOT / "esp32" / "partitions" / "wled_16mb_9mb_fs.csv",
        "fs": ("0x610000", "0x9e0000"),
        "flash_size": "16MB",
        # This board's radio browns out at full power, so WLED boots with its own TX power setting at 8.5 dBm (wifi_power_t 34), written into its config.
        "txpwr": 34,
    },
}
PAUSES = 5


def fail(msg: str) -> None:
    print(f"==> {msg}")
    sys.exit(2)


class Bench:
    def __init__(self, port: str, chip: str):
        self.port = port
        self.chip = chip
        self.ip = ""
        idf = find_idf()
        if not idf:
            fail("ESP-IDF not found: esptool and the partition generator come from it")
        self.idf = idf
        self.env = idf_env(idf)
        self.py = shutil.which("python", path=self.env.get("PATH", "")) or sys.executable
        self.cache = ROOT / "build" / "wled-migration-test" / chip

    def esptool(self, *args: str) -> None:
        r = subprocess.run([self.py, "-m", "esptool", "--chip", self.chip, *args],
                           env=self.env, capture_output=True, text=True)
        if r.returncode:
            print(r.stdout[-400:], r.stderr[-400:])
            fail(f"esptool {args[0]} failed")

    # WLED's three images, downloaded and converted once into build/wled-migration-test/<chip>/.
    def wled_files(self) -> tuple[Path, Path, Path]:
        self.cache.mkdir(parents=True, exist_ok=True)
        spec = WLED[self.chip]
        app, elf = self.cache / "wled.bin", self.cache / "bootloader.elf"
        boot, table = self.cache / "bootloader.bin", self.cache / "partitions.bin"
        # A core that publishes its bootloader as an image is taken as it is; one that publishes the ELF is converted.
        sources = [(app, spec["app"]), (boot, spec["bootloader_bin"]) if "bootloader_bin" in spec else (elf, spec["bootloader_elf"])]
        for path, url in sources:
            if not path.exists():
                print(f"==> downloading {url}")
                urllib.request.urlretrieve(url, path)
        if not boot.exists():
            self.esptool("elf2image", "--flash-mode", BOOTLOADER_MODE, "--flash-freq", spec["flash_freq"],
                         "--flash-size", spec["flash_size"], "-o", str(boot), str(elf))
        if not table.exists():
            gen = self.idf / "components" / "partition_table" / "gen_esp32part.py"
            if subprocess.run([self.py, str(gen), "--flash-size", spec["flash_size"], str(spec["table"]), str(table)], env=self.env).returncode:
                fail("the WLED partition table did not generate")
        return app, boot, table

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
        subprocess.run(["curl", "-s", "-m", "120", "-o", "/dev/null", "-F", f"update=@{image}",
                        f"http://{self.ip}/update"])

    # The board as a WLED device ships: WLED's images, an empty app choice and an empty filesystem, joined to the WiFi.
    # WLED's filesystem holding its own config, cfg.json and wsec.json: our network, and the TX power the board needs, so WLED boots on the network at that power.
    # The LittleFS settings are those of the Arduino core WLED's S3 build runs (2.0.9): 4 KB blocks, 128-byte reads and writes, 64-character names, on-disk version 2.0.
    def wled_config_image(self, txpwr: int) -> Path:
        from host_wifi import get_host_wifi
        ssid, password = get_host_wifi()
        if not ssid or not password:
            fail("no host WiFi credentials: run moondeck/build/host_wifi.py first")
        spec = WLED[self.chip]
        fs = littlefs.LittleFS(block_size=4096, block_count=int(spec["fs"][1], 16) // 4096, read_size=128, prog_size=128,
                               lookahead_size=128, cache_size=512, block_cycles=512, name_max=64, disk_version=0x00020000)
        network = {"ssid": ssid, "pskl": len(password), "psk": password, "ip": [0, 0, 0, 0], "gw": [0, 0, 0, 0], "sn": [255, 255, 255, 0]}
        with fs.open("/cfg.json", "w") as f:
            f.write(json.dumps({"rev": [1, 0], "nw": {"ins": [network]}, "wifi": {"txpwr": txpwr}}, separators=(",", ":")))   # compact, as WLED writes it
        with fs.open("/wsec.json", "w") as f:
            f.write(json.dumps({"nw": {"ins": [{"psk": password}]}}, separators=(",", ":")))
        image = self.cache / "wled-fs.bin"   # holds the WiFi password: build/ is ignored by git, and the next run overwrites it
        image.write_bytes(fs.context.buffer)
        return image

    def as_wled(self, files: tuple[Path, Path, Path], self_update: bool) -> None:
        app, boot, table = files
        spec = WLED[self.chip]
        config = [spec["fs"][0], str(self.wled_config_image(spec["txpwr"]))] if "txpwr" in spec else []
        self.esptool("--port", self.port, "-b", spec["baud"], "erase-region", "0xe000", "0x2000")
        self.esptool("--port", self.port, "-b", spec["baud"], "erase-region", *spec["fs"])
        self.esptool("--port", self.port, "-b", spec["baud"], "write-flash", "--flash-mode", BOOTLOADER_MODE,
                     "--flash-freq", spec["flash_freq"], "--flash-size", spec["flash_size"],
                     spec["bootloader_at"], str(boot), "0x8000", str(table), "0x10000", str(app), *config)
        time.sleep(3)
        wait = ["--wait-provisioned", "30"] if config else []   # WLED joins from the config written into it
        r = subprocess.run(["uv", "run", str(ROOT / "moondeck" / "build" / "improv_provision.py"),
                            "--port", self.port, *wait], cwd=ROOT, capture_output=True, text=True)
        url = next((line.split("provisioned:", 1)[1].strip() for line in r.stdout.splitlines()
                    if "provisioned:" in line), "")
        self.ip = url.removeprefix("http://").strip("/")
        if not self.ip:
            print(r.stdout[-300:])
            fail("WLED did not report an address over Improv")
        if self.wait_for("WLED", 40) != "WLED":
            fail(f"WLED is not answering at {self.ip}")
        try:   # what WLED runs with, as it saved it: its TX power among it
            with urllib.request.urlopen(f"http://{self.ip}/json/cfg", timeout=5) as r:
                print(f"==> WLED's TX power setting: {json.loads(r.read()).get('wifi', {}).get('txpwr')}")
        except Exception as e:
            print(f"==> WLED's config did not read: {e}")
        if self_update:   # WLED then runs from app1, so the image lands in app0, where MoonBase goes
            self.upload(app)
            time.sleep(5)
            if self.wait_for("WLED", 60) != "WLED":
                fail("WLED did not come back after its own update")

    # Upload `image`, reset in pause `cut` (0 = never), and say what the board answers as.
    def round(self, files, image: Path, cut: int, self_update: bool) -> tuple[str | None, list[str]]:
        self.as_wled(files, self_update)
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
    ap.add_argument("--chip", default="esp32c3", choices=sorted(WLED), help="The board's chip")
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
    files = bench.wled_files()
    results = []
    for r in rounds:
        if r == "ship":
            outcome, log = bench.round(files, shipped, 0, False)
            label = "shipped image, no cut"
        elif r == "move":
            outcome, log = bench.round(files, paused, 0, True)
            label = "move aside, no cut"
        elif r.isdigit() and 1 <= int(r) <= PAUSES:
            outcome, log = bench.round(files, paused, int(r), False)
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
