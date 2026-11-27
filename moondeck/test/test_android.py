#!/usr/bin/env python3
"""Run the unit tests on an attached Android device (a phone, a tablet, a Meta Quest) over adb.

Strips the ``mm_tests`` build_android.py made, pushes it to ``/data/local/tmp``, and runs it there
with its temp and data folders beside it, since an adb shell has no /tmp and no home.

A process adb shell starts lacks the app identity AAudio needs, so it runs as a machine with no sound
device (platform_host_audio.cpp); the audio tests feed their own samples.
"""

import argparse
import shlex
import subprocess
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from build_android import BUILD_DIR, ndk_tool  # noqa: E402

DEVICE_DIR = "/data/local/tmp/moonlight-tests"

# The repo files the tests read from disk, pushed under DEVICE_DIR/repo, where test/unit/source_tree.h looks on a device.
# Built firmware images stay on the computer: their tests return early on a device, as on a tree that has built none.
SOURCE_FILES = {
    "moonlive": "script sweeps, catalog, motion and particle examples, Xtensa codegen",
    "fonts/6x8.wbf": "unit_TextEffect.cpp, the 6x8 golden",
    "esp32/partitions": "unit_PartitionTables.cpp",
    "moonbase/main/moonbase_main.cpp": "unit_MoonBaseContract.cpp",
    "src/core/system/HttpServerModule.cpp": "unit_MoonBaseContract.cpp",
    "src/ui/app.js": "unit_MoonBaseContract.cpp",
    "src/core/system/FirmwareUpdateModule.h": "unit_MoonBaseContract.cpp",
    "src/platform/esp32/platform_esp32_ota.cpp": "unit_MoonBaseContract.cpp",
    "src/core/moonlive/MoonLiveCompiler.cpp": "unit_MoonLiveScripts.cpp, the status-line length",
    "src/core/moonlive/MoonLiveCompiler.h": "unit_MoonLiveScripts.cpp, the status-line length",
    "docs/moonmodules/light/moonlive.md": "unit_MoonLiveScripts.cpp, the docs examples",
    "docs/tutorials/first-script.md": "unit_MoonLiveScripts.cpp, the docs examples",
}


def adb(*args, **kw):
    return subprocess.run(["adb", *args], **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("doctest_args", nargs="*", help="passed to mm_tests, such as -tc='*MoonLive*'")
    args, doctest_flags = ap.parse_known_args()   # doctest's own flags, such as -tc=, pass through as they are

    tests = ROOT / BUILD_DIR / "test" / "mm_tests"
    if not tests.exists():
        sys.exit(f"No {tests.relative_to(ROOT)}: run `uv run moondeck/build/build_android.py --tests` first.")
    devices = [line for line in adb("devices", capture_output=True, text=True).stdout.splitlines()[1:] if line.strip()]
    if not any(line.endswith("\tdevice") for line in devices):
        sys.exit("No authorized Android device: connect it with USB debugging on and accept the prompt on its screen.\n"
                 + "\n".join(devices))

    stripped = tests.with_name("mm_tests.stripped")
    subprocess.run([str(ndk_tool("llvm-strip")), "-o", str(stripped), str(tests)], check=True)
    adb("shell", f"rm -rf {DEVICE_DIR}; mkdir -p {DEVICE_DIR}/tmp {DEVICE_DIR}/data", check=True)
    adb("push", str(stripped), f"{DEVICE_DIR}/mm_tests", check=True, stdout=subprocess.DEVNULL)
    adb("shell", f"chmod 755 {DEVICE_DIR}/mm_tests", check=True)
    for rel in SOURCE_FILES:
        remote = f"{DEVICE_DIR}/repo/{PurePosixPath(rel).parent}"   # the device's path, POSIX whatever the computer is
        adb("shell", f"mkdir -p {remote}", check=True)
        adb("push", str(ROOT / rel), f"{remote}/", check=True, stdout=subprocess.DEVNULL)
    quoted = shlex.join(doctest_flags + args.doctest_args)   # a filter may hold an apostrophe, as test names do
    run = (f"cd {DEVICE_DIR} && TMPDIR={DEVICE_DIR}/tmp MM_DATA_DIR={DEVICE_DIR}/data HOME={DEVICE_DIR}/data "
           f"./mm_tests {quoted}; echo \"exit $?\"")
    out = adb("shell", run, capture_output=True, text=True).stdout
    print(out)
    last = out.strip().splitlines()[-1] if out.strip() else "exit 1"
    sys.exit(int(last.split()[-1]) if last.startswith("exit") else 1)


if __name__ == "__main__":
    main()
