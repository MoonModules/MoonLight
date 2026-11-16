#!/usr/bin/env python3
"""Install the app build_android.py --apk made on the attached Android device and open it.

``adb install -r`` keeps the app's settings, as installing a release's APK over it does, then the
app starts and runs MoonLight; its log is ``adb logcat -s MoonLight``.
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from build_android import BUILD_DIR  # noqa: E402

PACKAGE = "org.moonmodules.moonlight"


def main():
    apk = ROOT / BUILD_DIR / "MoonLight.apk"
    if not apk.exists():
        sys.exit(f"No {apk.relative_to(ROOT)}: run `uv run moondeck/build/build_android.py --apk` first.")
    devices = subprocess.run(["adb", "devices"], capture_output=True, text=True).stdout.splitlines()[1:]
    if not any(line.endswith("\tdevice") for line in devices):
        sys.exit("No authorized Android device: connect it with USB debugging on and accept the prompt on its screen.")
    # -d also installs over a newer version code, as a build from this checkout over a CI release is
    install = subprocess.run(["adb", "install", "-r", "-d", str(apk)], capture_output=True, text=True)
    print(install.stdout.strip())
    if install.returncode != 0:
        if "UPDATE_INCOMPATIBLE" in install.stdout + install.stderr:
            sys.exit(f"The device holds MoonLight signed with another key, such as a release: uninstall it first (back up its settings), "
                     f"`adb uninstall {PACKAGE}`, then run this again.")
        sys.exit(install.stderr.strip() or install.returncode)
    for cmd in (["adb", "shell", "am", "force-stop", PACKAGE],
                ["adb", "shell", "am", "start", "-n", f"{PACKAGE}/.MainActivity"]):
        r = subprocess.run(cmd)
        if r.returncode != 0:
            sys.exit(r.returncode)
    print("MoonLight is open on the device; its log: adb logcat -s MoonLight")


if __name__ == "__main__":
    main()
