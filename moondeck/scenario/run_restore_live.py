#!/usr/bin/env python3
"""Restore a backup onto a running device the way the File Manager's Restore does, and prove every value came back.

It runs the UI's own `restoreBundle` (src/ui/migrate.js) in Node against the device, so the migration, the file writes
and the report are the button's, without a browser. Then it reads each config file back from the device and compares it
with the migrated bundle, value by value; a value MIGRATING renames on the device itself, and what a device reports
rather than stores, are left out. It fails when the device restarted meanwhile, naming its boot reason.

`--readers N` keeps N other clients reading the device for the whole restore, as a second browser or a script does.
With the serial monitor attached, that is the setup that caught a panic after a restore on the bench.

Exit codes: 0 every value back and the report clean, 1 a failure, 2 no Node or no device.

Run:  uv run moondeck/scenario/run_restore_live.py --bundle test/fixtures/backup-flat-2026-09-30.json [--host IP[:port]] [--readers N]
"""

import argparse
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="localhost:8080", help="the device, as IP or IP:port (default the desktop app)")
    ap.add_argument("--bundle", required=True, help="a backup bundle, as the File Manager's Backup writes it")
    ap.add_argument("--readers", type=int, default=0, help="other clients reading the device during the restore")
    args = ap.parse_args()
    if shutil.which("node") is None:
        print("run_restore_live: Node is not installed.")
        return 2
    try:
        urllib.request.urlopen(f"http://{args.host}/api/system", timeout=5).read()
    except OSError as e:
        print(f"run_restore_live: no device answers at {args.host} ({e}).")
        return 2
    driver = ROOT / "moondeck" / "scenario" / "restore_backup.mjs"
    return subprocess.run(["node", str(driver), args.host, str(Path(args.bundle).resolve()), str(args.readers)]).returncode


if __name__ == "__main__":
    sys.exit(main())
