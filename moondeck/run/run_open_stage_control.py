#!/usr/bin/env python3
"""Launch Open Stage Control with MoonLight's session, ready to use.

Open Stage Control's settings panel wants a send address, a listen port, a session file and a custom
module before it is useful, and getting one wrong is silent. All four are command-line options, so
this script is the whole configuration:

    uv run moondeck/run/run_open_stage_control.py                 # device on this machine
    uv run moondeck/run/run_open_stage_control.py --host 192.168.1.42

It does not install Open Stage Control (a free download from openstagecontrol.ammd.net). macOS
quarantines the unsigned app, so the first launch needs a right-click Open, once.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SESSION = ROOT / "docs" / "reference" / "examples" / "open-stage-control.json"

# Where the app lands, per platform. A PATH lookup comes first, so a package install or a custom
# location needs no entry here; these are the defaults each platform's own installer uses.
def candidates() -> list[Path]:
    home = Path.home()
    if sys.platform == "darwin":
        return [Path("/Applications/open-stage-control.app/Contents/MacOS/open-stage-control"),
                home / "Applications/open-stage-control.app/Contents/MacOS/open-stage-control"]
    if os.name == "nt":
        # The Windows build is an installed or portable directory holding the .exe.
        progs = [Path(os.environ.get("LOCALAPPDATA", "")) / "Programs",
                 Path(os.environ.get("PROGRAMFILES", "C:/Program Files"))]
        return [p / "open-stage-control" / "open-stage-control.exe" for p in progs if str(p)]
    # Linux: a package, or the unpacked AppImage/tarball people usually drop in ~/.
    return [Path("/usr/bin/open-stage-control"), Path("/usr/local/bin/open-stage-control"),
            Path("/opt/open-stage-control/open-stage-control"),
            home / "open-stage-control/open-stage-control",
            home / ".local/bin/open-stage-control"]


def find_app() -> str | None:
    found = shutil.which("open-stage-control")
    if found:
        return found
    for c in candidates():
        if c.exists():
            return str(c)
    return None


def entry_script(app: Path) -> Path | None:
    """The app's own index.js, when the binary is an Electron shim that needs it named.

    On macOS the launcher binary hands unrecognized arguments to node, so `--port` is "bad option"
    until index.js is passed first. The Windows and Linux builds lay the same file out differently,
    and a package install may have no bundled copy at all, in which case the binary takes its own
    options and this returns None.
    """
    for rel in ("../Resources/app/index.js",          # macOS .app bundle
                "resources/app/index.js",             # Windows / Linux unpacked Electron
                "../lib/open-stage-control/resources/app/index.js"):   # some Linux packages
        p = (app.parent / rel).resolve()
        if p.exists():
            return p
    return None


def port(value: str) -> int:
    """A TCP/UDP port. Argparse's `type=int` accepts 99999 and 0, which reach Open Stage
    Control as a bind failure buried in its own output; reject them where we can say why."""
    n = int(value)
    if not 1 <= n <= 65535:
        raise argparse.ArgumentTypeError(f"{n} is not a port (1..65535)")
    return n


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1",
                    help="the device's address (default: this machine)")
    ap.add_argument("--port", type=port, default=9000,
                    help="the device's OSC port, its `port` control (default 9000)")
    ap.add_argument("--listen", type=port, default=9001,
                    help="where WE listen, the device's `feedbackPort` control (default 9001)")
    ap.add_argument("--ui-port", type=port, default=8088,
                    help="the Open Stage Control web UI (default 8088; 8080 is MoonLight's)")
    ap.add_argument("--app", help="path to the open-stage-control binary, if it is not found")
    ap.add_argument("--gui", action="store_true",
                    help="also open the desktop window (default: server only, use a browser)")
    args = ap.parse_args()

    app = args.app or find_app()
    if not app:
        print("Open Stage Control not found. Install it from https://openstagecontrol.ammd.net/\n"
              "or pass --app /path/to/open-stage-control", file=sys.stderr)
        return 1
    if not SESSION.exists():
        print(f"session missing: {SESSION}", file=sys.stderr)
        return 1

    # No custom module: the session's own `autosync` widget sends /mm/hello from its
    # onCreate, which fires on every browser load AND refresh. A server-side module only
    # sees the server start, so it could not cover a refresh at all.
    env = dict(os.environ)
    entry = entry_script(Path(app).resolve())
    cmd = [app] + ([str(entry)] if entry else []) + [
           "--send", f"{args.host}:{args.port}",
           "--osc-port", str(args.listen),
           "--port", str(args.ui_port),
           "--load", str(SESSION)]
    # Server-only by default: the surface is meant for the phone or tablet in the room, and an
    # Electron window on the build machine costs a few hundred MB to show the same page.
    if not args.gui:
        cmd.append("--no-gui")

    print(f"MoonLight at {args.host}:{args.port}, feedback to us on {args.listen}")
    print(f"surface at http://127.0.0.1:{args.ui_port}")
    print(f"  {' '.join(cmd)}\n")
    print("On the device, the OSC module needs: listen on, feedback on, "
          f"feedbackPort {args.listen}.")
    try:
        return subprocess.call(cmd, env=env)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
