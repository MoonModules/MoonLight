#!/usr/bin/env python3
"""Run the firmware under QEMU, a real ESP32, emulated, on the development machine.

Why this exists: MoonLive emits machine code, and a defect in it shows up as a board that resets.
Every other check we have compares emitted bytes to a model of what they should be, which cannot
catch a mistake the model shares. QEMU EXECUTES the code the way silicon does, including Xtensa's
register window, `entry`/`retw` and the exception path, so a fault happens here, on the host, in a
debugger, instead of on a bench with only a crash dump to read.

The emulated board is a full device: the REST API and the web UI work, so the same scripts, tests
and browser drive it exactly as they drive hardware.

    uv run moondeck/qemu/run_qemu.py                  # boot it, forward the UI to :8410
    uv run moondeck/qemu/run_qemu.py --gdb            # freeze at reset, wait for a debugger
    uv run moondeck/qemu/run_qemu.py --seconds 20     # run a while, print the log, exit

With --gdb, attach from another terminal:
    xtensa-esp32-elf-gdb build/esp32-qemu/projectMM.elf -ex 'target remote :1234'
"""

import argparse
import glob
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "build", "esp32-qemu")

# esptool lives in the ESP-IDF venv, not in the interpreter `uv run` starts. build_esp32.py already
# knows how to find the right one (there can be several IDF versions installed), so reuse that rather
# than re-deriving the path here.
sys.path.insert(0, os.path.join(ROOT, "moondeck", "build"))
from build_esp32 import find_idf_python  # noqa: E402


def qemu_binary() -> str:
    hits = glob.glob(os.path.expanduser(
        "~/.espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa"))
    if not hits:
        sys.exit("qemu-system-xtensa not found, install it with:\n"
                 "  python3 $IDF_PATH/tools/idf_tools.py install qemu-xtensa")
    return sorted(hits)[-1]


def merged_flash(force: bool) -> str:
    """The 4 MB flash image QEMU boots from: bootloader + partition table + app, in one file."""
    out = os.path.join(BUILD, "qemu-flash.bin")
    args = os.path.join(BUILD, "flash_args")
    if not os.path.exists(args):
        sys.exit(f"no build at {BUILD}, build it first:\n"
                 "  uv run moondeck/build/build_esp32.py --firmware qemu")
    # Freshness is judged against the APP BINARY, not `flash_args`. flash_args is written once when
    # CMake configures the build dir and never touched again, so it is always older than the image ,
    # which made this cache never expire: after a rebuild the emulator kept booting the PREVIOUS
    # app, and code that was plainly in the .bin appeared not to run at all.
    app = os.path.join(BUILD, "projectMM.bin")
    moonbase = os.path.join(os.path.dirname(BUILD), "moonbase-esp32", "projectMM-moonbase.bin")
    newest_input = max((os.path.getmtime(p) for p in (args, app, moonbase)
                        if os.path.exists(p)), default=0)
    if os.path.exists(out) and not force and os.path.getmtime(out) > newest_input:
        return out
    # find_idf_python returns the venv DIRECTORY; the interpreter is bin/python inside it.
    venv = find_idf_python()
    idf_py = os.path.join(str(venv), "bin", "python") if venv else ""
    if not idf_py or not os.path.exists(idf_py):
        sys.exit("no ESP-IDF Python env found, source export.sh, or install the IDF tools")
    # The qemu firmware carries MoonBase, and IDF's own flash_args stages the app at the factory
    # offset (MoonBase's slot): the same correction every flasher applies. moonbase_flash_files
    # is the one place that knows the corrected layout; the flat list it returns feeds merge_bin
    # directly instead of @flash_args.
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "build"))
    from build_esp32 import FIRMWARES, moonbase_flash_files
    if FIRMWARES["qemu"].get("moonbase"):
        from pathlib import Path
        writes = [str(x) for off, path in moonbase_flash_files("qemu", Path(BUILD))
                  for x in (off, path)]
    else:
        writes = [f"@{args}"]
    r = subprocess.run([idf_py, "-m", "esptool", "--chip", "esp32", "merge_bin",
                        "-o", out, "--fill-flash-size", "4MB"] + writes,
                       cwd=BUILD, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"merge_bin failed:\n{r.stderr[:800]}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gdb", action="store_true",
                    help="freeze the CPU at reset and wait for a debugger on :1234")
    ap.add_argument("--seconds", type=int, default=0,
                    help="run for N seconds, then exit (0 = run until interrupted)")
    ap.add_argument("--http-port", type=int, default=8410,
                    help="host port forwarded to the guest's HTTP server (default 8410). "
                         "Deliberately not 8080: a desktop MoonLight build listens there, and "
                         "QEMU fails to start rather than sharing the port.")
    ap.add_argument("--rebuild-image", action="store_true",
                    help="re-merge the flash image even if it looks current")
    ap.add_argument("--erase", action="store_true",
                    help="wipe the emulated flash (settings + scripts) before starting, the "
                         "equivalent of erase_flash_esp32.py on a real board. Use when a saved "
                         "config boot-loops the guest: a merged image keeps the DATA partitions, "
                         "so re-merging alone does NOT clear it.")
    ap.add_argument("--stop", action="store_true",
                    help="stop a running emulator instead of starting one")
    ap.add_argument("--background", action="store_true",
                    help="start it and return, instead of holding the terminal (MoonDeck uses this)")
    args = ap.parse_args()

    if args.stop:
        # pkill by the binary path, not by our own PID: MoonDeck starts the emulator in one task and
        # stops it from another, so there is no shared handle to keep.
        r = subprocess.run(["pkill", "-f", "qemu-system-xtensa"], capture_output=True)
        print("emulator stopped" if r.returncode == 0 else "no emulator was running")
        return 0

    if args.erase:
        # Deleting the image is the erase: merged_flash rebuilds it from the bootloader, partition
        # table and app alone, so every DATA partition (nvs, otadata, spiffs, where the module tree
        # and the .mlv scripts live) comes back blank. On a real board this is erase_flash_esp32.py;
        # here the whole "flash chip" is one file, so removing it is the same operation.
        img = os.path.join(BUILD, "qemu-flash.bin")
        if os.path.exists(img):
            os.remove(img)
            print("  erased  → emulated flash wiped (settings + scripts gone)")
        else:
            print("  erased  → no image to wipe")

    # Free the forward port FIRST. QEMU treats a failed hostfwd as fatal but still exits 0, so a
    # second emulator (or a leftover one) makes the new run die in under a second while the caller
    # reports success, and the UI simply never appears, with nothing in the log to say why. There is
    # only ever one emulator, so taking the port from a previous instance is always what was meant.
    holder = subprocess.run(["lsof", "-nP", f"-iTCP:{args.http_port}", "-sTCP:LISTEN", "-t"],
                            capture_output=True, text=True)
    for pid in holder.stdout.split():
        print(f"  freeing → port {args.http_port} was held by pid {pid}, stopping it")
        subprocess.run(["kill", pid], capture_output=True)
    if holder.stdout.strip():
        time.sleep(1)

    flash = merged_flash(args.rebuild_image or args.erase)
    cmd = [
        qemu_binary(), "-nographic", "-machine", "esp32",
        "-drive", f"file={flash},if=mtd,format=raw",
        # The emulated MAC, plus a host-port forward so the device's web UI opens in a browser.
        # `user` networking needs no privileges and no host interface, the guest gets a DHCP
        # address on QEMU's internal network and reaches the outside through the host.
        "-nic", f"user,model=open_eth,hostfwd=tcp::{args.http_port}-:80",
    ]
    if args.gdb:
        cmd += ["-s", "-S"]

    print(f"QEMU: {os.path.basename(flash)}")
    print(f"  web UI  → http://localhost:{args.http_port}/   (once the guest has a DHCP lease)")
    if args.gdb:
        print("  debugger→ frozen at reset, waiting on :1234")
        print(f"            xtensa-esp32-elf-gdb {BUILD}/projectMM.elf -ex 'target remote :1234'")
    print()

    if args.background:
        # Detached, with the serial log on disk: the caller gets its prompt back and can read the
        # boot output from build/esp32-qemu/qemu-serial.log while the guest keeps running.
        logf = os.path.join(BUILD, "qemu-serial.log")
        with open(logf, "wb") as f:
            subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, start_new_session=True)
        print(f"  serial  → {logf}")
        print("  stop it → uv run moondeck/qemu/run_qemu.py --stop")
        return 0

    try:
        if args.seconds:
            subprocess.run(cmd, timeout=args.seconds)
        else:
            subprocess.run(cmd)
    except subprocess.TimeoutExpired:
        pass
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
