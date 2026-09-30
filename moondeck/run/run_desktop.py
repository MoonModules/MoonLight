#!/usr/bin/env python3
"""Run the desktop executable as a detached background process.

Same shape as flashing ESP32: this script does the launch and exits cleanly.
The app keeps running until the user (or another `run_desktop` invocation)
stops it. A re-run kills any prior instance first, so "Run" is idempotent —
press it twice in a row and you get one fresh app, not two.

Why detached: if we waited (or `os.execv`d), MoonDeck would track this
process's PID and terminate it as soon as another script was launched, which
is exactly the behaviour the user asked us to drop. Detaching releases the
PID and lets the app outlive this script (mirroring how flashing an ESP32
leaves the device running independently).
"""

import argparse
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def _host_build_dir() -> Path:
    """Per-host build dir, matching build_desktop.py / package_desktop.py."""
    return ROOT / "build" / {
        "Darwin":  "macos",
        "Linux":   "linux",
        "Windows": "windows",
    }.get(platform.system(), platform.system().lower())


# MSVC multi-config places the binary under <build>/Release/MoonLight.exe;
# every other generator drops it directly under <build>/MoonLight. Pick the
# first that exists at lookup time so the file works on every host.
def _resolve_executable() -> Path:
    bdir = _host_build_dir()
    candidates = [
        bdir / "MoonLight.exe",
        bdir / "Release" / "MoonLight.exe",
        bdir / "MoonLight",
        # A plain `cmake --build build` writes here rather than into the per-host dir, so this path
        # is often the NEWER binary. Both are considered and the freshest wins below: picking the
        # first that merely exists served a stale build whose changes appeared to be no-ops.
        ROOT / "build" / "MoonLight",
        ROOT / "build" / "MoonLight.exe",   # the same root-build case on Windows
    ]
    # Only this host's artefact shape is a candidate: a stale MoonLight.exe left in a shared
    # checkout must never be picked on macOS/Linux (and vice versa), however new it is.
    want_exe = platform.system() == "Windows"
    # Read each candidate's mtime in the SAME step that proves it exists: a rebuild running
    # alongside this script can replace a binary between an exists() check and a later stat(),
    # which would raise FileNotFoundError from inside max().
    stamped = []
    for c in candidates:
        if (c.suffix == ".exe") != want_exe:
            continue
        try:
            stamped.append((c.stat().st_mtime, c))
        except (FileNotFoundError, OSError):
            continue   # vanished or unreadable: simply not a candidate
    if stamped:
        return max(stamped)[1]
    # Return the most-likely candidate so the error message points somewhere
    # informative if the binary genuinely isn't there.
    return bdir / ("MoonLight.exe" if sys.platform == "win32" else "MoonLight")


EXECUTABLE = _resolve_executable()


def _is_running() -> bool:
    """True iff an instance of the executable is currently running."""
    name = EXECUTABLE.name
    if sys.platform == "win32":
        # EXECUTABLE already carries the .exe suffix on Windows (see the top of
        # this file); appending it a second time would produce "MoonLight.exe.exe"
        # and tasklist would match nothing.
        r = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {name}"],
            capture_output=True, text=True)
        return name in r.stdout
    r = subprocess.run(["pgrep", "-f", str(EXECUTABLE)], capture_output=True)
    return r.returncode == 0


def _kill_running():
    """Terminate every running instance. Idempotent."""
    if sys.platform == "win32":
        subprocess.run(["taskkill", "/F", "/IM", EXECUTABLE.name],
                       capture_output=True)
    else:
        # Match the full path to avoid hitting unrelated programs called "MoonLight".
        subprocess.run(["pkill", "-f", str(EXECUTABLE)], capture_output=True)


def main():
    ap = argparse.ArgumentParser(description="Run the desktop build in the background.")
    # Ports below 1024 need root, so the default stays 8080. Port 80 exists for Home Assistant's
    # WLED integration, which hardcodes port 80 and offers no way to specify another
    # (`sudo uv run moondeck/run/run_desktop.py --port 80`).
    ap.add_argument("--port", type=int, default=None,
                    help="HTTP port (default 8080; 80 needs root, for the Home Assistant WLED integration)")
    args = ap.parse_args()
    if args.port is not None and not (1 <= args.port <= 65535):
        print(f"--port must be 1..65535, got {args.port}")
        sys.exit(1)

    if not EXECUTABLE.exists():
        print(f"Executable not found: {EXECUTABLE}")
        print("Run build_desktop.py first.")
        sys.exit(1)

    if _is_running():
        print(f"Existing {EXECUTABLE.name} found — stopping it first.")
        _kill_running()
        # Give the OS a moment to release sockets the app was holding (HTTP :8080).
        for _ in range(20):
            if not _is_running():
                break
            time.sleep(0.05)

    print(f"Launching {EXECUTABLE} (detached)...")
    sys.stdout.flush()

    # Detach: own session, stdio disconnected from this script, output redirected
    # to a log file the user can tail. The app survives this script exiting and
    # is unaffected by MoonDeck terminating other scripts.
    log_path = _host_build_dir() / "MoonLight.log"
    log_fp = open(log_path, "wb")  # closed when this script exits; the child keeps the fd

    popen_kwargs = dict(
        stdin=subprocess.DEVNULL,
        stdout=log_fp,
        stderr=subprocess.STDOUT,
        cwd=str(ROOT),
        close_fds=True,
    )
    if sys.platform == "win32":
        popen_kwargs["creationflags"] = (
            subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
        )
    else:
        popen_kwargs["start_new_session"] = True  # own session, immune to our SIGTERM

    cmd = [str(EXECUTABLE)]
    if args.port is not None:
        cmd += ["--port", str(args.port)]
    proc = subprocess.Popen(cmd, **popen_kwargs)
    print(f"PID {proc.pid} — log: {log_path}")
    print("Press the Run button again to restart; the app keeps running otherwise.")


if __name__ == "__main__":
    main()
