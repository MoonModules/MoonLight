#!/usr/bin/env python3
"""Restore a backup onto a running device the way the File Manager's Restore does, and prove every value came back.

It runs the UI's own `restoreBundle` (src/ui/migrate.js) in Node against the device, so the migration, the file writes
and the report are the button's, without a browser. Then it reads each config file back from the device and compares it
with the migrated bundle, value by value; a value MIGRATING renames on the device itself, and what a device reports
rather than stores, are left out. It fails when the device restarted meanwhile, naming its boot reason.

`--readers N` keeps N other clients reading the device for the whole restore, as a second browser or a script does.
With the serial monitor attached, that is the setup that caught a panic after a restore on the bench.

`--launch` starts this host's desktop build on an empty data folder and its own port, restores onto it and stops it, printing
the app's log when the restore fails: the CI job, the same on any machine, beside a desktop app already running on 8080.

Exit codes: 0 every value back and the report clean, 1 a failure, 2 no Node or no device.

Run:  uv run moondeck/scenario/run_restore_live.py --bundle test/fixtures/backup-flat-2026-09-30.json [--host IP[:port]] [--readers N]
      uv run moondeck/scenario/run_restore_live.py --launch --bundle test/fixtures/backup-flat-2026-09-30.json --readers 2
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from build_desktop import desktop_binary  # noqa: E402 (one definition of where the binary lands)

LAUNCH_PORT = 8091   # beside a desktop app already running on 8080


def answers(host: str, seconds: float) -> bool:
    """Whether the device answers /api/system within `seconds`."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            urllib.request.urlopen(f"http://{host}/api/system", timeout=5).read()
            return True
        except OSError:
            if time.monotonic() >= deadline:
                return False
            time.sleep(1)


def restore(host: str, bundle: str, readers: int) -> int:
    driver = ROOT / "moondeck" / "scenario" / "restore_backup.mjs"
    return subprocess.run(["node", str(driver), host, str(Path(bundle).resolve()), str(readers)]).returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="localhost:8080", help="the device, as IP or IP:port (default the desktop app)")
    ap.add_argument("--bundle", required=True, help="a backup bundle, as the File Manager's Backup writes it")
    ap.add_argument("--readers", type=int, default=0, help="other clients reading the device during the restore")
    ap.add_argument("--launch", action="store_true", help="start this host's desktop build on an empty data folder, restore onto it, stop it")
    args = ap.parse_args()
    if shutil.which("node") is None:
        print("run_restore_live: Node is not installed.")
        return 2
    if not args.launch:
        if not answers(args.host, 0):
            print(f"run_restore_live: no device answers at {args.host}.")
            return 2
        return restore(args.host, args.bundle, args.readers)
    app = desktop_binary()
    if app is None:
        print("run_restore_live: no desktop build; run uv run moondeck/build/build_desktop.py first.")
        return 2
    with tempfile.TemporaryDirectory() as data:
        log = Path(data) / "app.log"
        with open(log, "w") as out:
            proc = subprocess.Popen([str(app), "--no-browser", "--port", str(LAUNCH_PORT)], stdout=out, stderr=subprocess.STDOUT,
                                    env={**os.environ, "MM_DATA_DIR": str(Path(data) / "fs")})
        try:
            host = f"localhost:{LAUNCH_PORT}"
            rc = restore(host, args.bundle, args.readers) if answers(host, 30) else 2
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        if rc != 0:
            print("\n".join(log.read_text(errors="replace").splitlines()[-50:]))
        return rc


if __name__ == "__main__":
    sys.exit(main())
