#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Monitor the ESP32 serial output, decoding panic backtraces. Saves to esp32/monitor.log.

A crash prints `Backtrace: 0x4038456d:0x3fcae310 …` — raw addresses that say nothing on their own.
Pass ``--firmware`` and each one is annotated with its function, file and line, so a panic names the
faulting source line in the monitor instead of starting an addr2line session. Same idea as
PlatformIO's ``esp32_exception_decoder`` monitor filter; here it is the toolchain's own addr2line
against the ELF the firmware was built from.
"""

import argparse
import glob
import os
import re
import serial
import subprocess
import sys
import time
from contextlib import suppress
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
LOG_FILE = ROOT / "esp32" / "monitor.log"

# A panic line: "Backtrace: 0xPC:0xSP 0xPC:0xSP …" (Xtensa) — the PCs are what we resolve.
BACKTRACE_RE = re.compile(r"Backtrace:((?:\s*0x[0-9a-fA-F]+:0x[0-9a-fA-F]+)+)")
# A bare "PC      : 0x…" register-dump line names the faulting instruction directly.
PC_RE = re.compile(r"^(PC|EXCVADDR)\s*:\s*(0x[0-9a-fA-F]+)")
# The device announces which firmware it is running; used to catch a stale ELF.
SHA_RE = re.compile(r"ELF file SHA256:\s*([0-9a-f]+)")


def find_addr2line(firmware: str):
    """The addr2line for this firmware's ISA, and the ELF to resolve against.

    Returns (tool, elf) or (None, None) when either is missing — decoding is then skipped and the
    monitor still runs, because a missing toolchain must not cost you the serial output.
    """
    elf = ROOT / "build" / f"esp32-{firmware}" / APP_ELF
    if not elf.exists():
        return None, None
    # P4 is RISC-V; every other supported target is Xtensa.
    pattern = ("riscv32-esp-elf/bin/riscv32-esp-elf-addr2line" if "p4" in firmware
               else "xtensa-esp-elf/bin/xtensa-esp32*-elf-addr2line")
    hits = glob.glob(os.path.expanduser(f"~/.espressif/tools/*/*/{pattern}"))
    return (sorted(hits)[-1], str(elf)) if hits else (None, None)


def decode(tool: str, elf: str, addrs: list[str]) -> list[str]:
    """Resolve addresses to `function at file:line`, one line each. Empty on any failure."""
    try:
        out = subprocess.run([tool, "-pfiaC", "-e", elf, *addrs],
                             capture_output=True, text=True, timeout=20)
        return [line for line in out.stdout.splitlines() if line.strip()]
    except Exception:
        return []

# Shared moondeck.json + logLevel-toggle helpers (one level up, reachable from check/ and run/).
sys.path.insert(0, str(ROOT / "moondeck"))
from _moondeck_config import active_device_ips, raised_log_level, LOG_INFO  # noqa: E402
sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from build_esp32 import APP_ELF  # noqa: E402

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="Serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--firmware", help="Firmware variant whose ELF decodes panic backtraces "
                                           "(e.g. esp32s3-n16r8). Without it, addresses print raw.")
    args = parser.parse_args()

    tool = elf = elf_sha = None
    # Cleared for the rest of the session once the device reports a firmware SHA that is not this
    # ELF's. Starts true: until the device says otherwise, the requested ELF is the best answer.
    sha_ok = True
    if args.firmware:
        tool, elf = find_addr2line(args.firmware)
        if tool:
            import hashlib
            # The ELF can vanish or turn unreadable between find_addr2line and here (a rebuild
            # mid-monitor). Decoding is the accessory; the serial output is the point — so a
            # failure here drops to raw addresses rather than ending the session.
            try:
                elf_sha = hashlib.sha256(Path(elf).read_bytes()).hexdigest()
                print(f"Decoding backtraces against build/esp32-{args.firmware}/{APP_ELF} "
                      f"({elf_sha[:9]})")
            except OSError as e:
                tool = elf = elf_sha = None
                print(f"Cannot read the ELF for {args.firmware} ({e}) — addresses print raw.")
        if not tool:
            print(f"No ELF or toolchain for {args.firmware} — addresses print raw.")

    # Raise the device(s) to Info so the tick line shows while monitoring; restore each device's ORIGINAL
    # level on exit (monitor failure and normal Ctrl+C alike), so a device the user set to Debug/Error
    # keeps its choice. Best-effort — an un-networked device is skipped (and already logs at Info for its
    # first 60 s anyway).
    print(f"Monitoring {args.port} at {args.baud} baud...")
    print(f"Log saved to {LOG_FILE}")
    print("Press Ctrl+C (or Stop in MoonDeck) to stop.\n")
    sys.stdout.flush()

    # OPEN THE PORT FIRST. raised_log_level contacts every device in moondeck.json over HTTP at a
    # 3 s timeout each — with a dozen registered and most powered off, that is half a minute of
    # blocking before a single byte is read, and the boot output you were monitoring FOR is already
    # gone. The log level is a nicety; the serial stream is the point.
    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}")
        sys.exit(1)

    with raised_log_level(active_device_ips(), LOG_INFO):
        with open(LOG_FILE, "w") as log:
            try:
                while True:
                    # A USB-CDC port on an S3/P4 is provided BY the firmware, so it disappears and
                    # re-enumerates on every reboot — exactly when the log matters most. Reconnect
                    # instead of dying: a crash-loop used to end the monitor with a traceback, losing
                    # the panic that caused it. Ctrl+C still stops, because that raises separately.
                    try:
                        raw = ser.readline()
                    except (serial.SerialException, OSError):
                        note = "  -- device disconnected (reboot?), waiting for it to come back --"
                        print(note)
                        log.write(note + "\n")
                        log.flush()
                        sys.stdout.flush()
                        with suppress(Exception):
                            ser.close()
                        ser = None
                        while ser is None:
                            time.sleep(0.3)
                            with suppress(Exception):
                                ser = serial.Serial(args.port, args.baud, timeout=1)
                        note = "  -- reconnected --"
                        print(note)
                        log.write(note + "\n")
                        log.flush()
                        sys.stdout.flush()
                        continue
                    line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    if line:
                        print(line)
                        log.write(line + "\n")
                        # Annotate a panic in place: the decoded frames follow the raw line, so the
                        # log keeps the original AND the reading, and a crash names its source line.
                        if tool:
                            # The device prints the SHA of the firmware it is running. If that is not
                            # the ELF we decode against, every resolved line would be from a different
                            # build — confidently wrong, which is worse than raw addresses.
                            sha = SHA_RE.match(line)
                            if sha and elf_sha and not elf_sha.startswith(sha.group(1)):
                                sha_ok = False
                                warn = (f"  !! running firmware {sha.group(1)} != this ELF "
                                        f"{elf_sha[:9]} — reflash to decode; addresses print raw")
                                print(warn)
                                log.write(warn + "\n")
                            addrs = []
                            m = BACKTRACE_RE.search(line)
                            if m:
                                addrs = [p.split(":")[0] for p in m.group(1).split()]
                                if "CORRUPTED" in line:
                                    note = ("  !! stack chain corrupted — frames past the first are "
                                            "not real; the fault is a stack overflow or a wild jump")
                                    print(note)
                                    log.write(note + "\n")
                            else:
                                pc = PC_RE.match(line)
                                if pc and pc.group(1) == "PC":
                                    addrs = [pc.group(2)]
                            # Only decode against the ELF the device is actually running. A
                            # mismatch resolves every address in a different build's layout, which
                            # reads as fact and sends you to the wrong file.
                            for i, d in enumerate(decode(tool, elf, addrs) if sha_ok else []):
                                out = f"  #{i} {d}"
                                print(out)
                                log.write(out + "\n")
                        sys.stdout.flush()
                        log.flush()
            except KeyboardInterrupt:
                pass
            finally:
                # `ser` is None while the reconnect loop waits for a rebooting board to come back,
                # which is exactly when Ctrl+C is likely — closing unconditionally would end the
                # session on an AttributeError traceback instead of the clean line below.
                if ser is not None:
                    ser.close()
                print(f"\nStopped. Full log: {LOG_FILE}")

if __name__ == "__main__":
    main()
