#!/usr/bin/env python3
# /// script
# dependencies = ["pillow"]
# ///
"""Build the host target for Android arm64 into ``build/android-arm64/``, with the NDK.

Android is the fourth host OS: the NDK's CMake toolchain compiles the same tree as Linux, and
CMake names the program ``host-android-arm64``. Builds ONE thing: the program (``MoonLight``)
by default, the unit tests (``--tests`` → ``mm_tests``), which test_android.py runs on a device, or
the app (``--apk``): ``libmoonlight.so`` and the launcher icon, packed by Gradle into
``build/android-arm64/MoonLight.apk``.
"""

import argparse
import base64
import contextlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_DIR = "build/android-arm64"
# Android 9, the floor the plan picked: old enough for a 2018 tablet, and raising it later is free.
API_LEVEL = 28
# The NDK this build is validated with, taken over whichever a machine or a CI runner has besides.
NDK_VERSION = "27.2.12479018"


def sdk_root() -> Path:
    """The Android SDK: ANDROID_HOME, which CI's runners set, else Android Studio's default folder."""
    return Path(os.environ.get("ANDROID_HOME") or Path.home() / "Library" / "Android" / "sdk")


def ndk_root() -> Path | None:
    """The NDK: ANDROID_NDK_HOME when set, else the pinned version under the SDK, else the newest there."""
    if os.environ.get("ANDROID_NDK_HOME"):
        return Path(os.environ["ANDROID_NDK_HOME"])
    pinned = sdk_root() / "ndk" / NDK_VERSION
    if pinned.is_dir():
        return pinned
    versions = sorted((sdk_root() / "ndk").glob("*"), key=lambda p: [int(x) for x in p.name.split(".") if x.isdigit()])
    return versions[-1] if versions else None


def install_ndk() -> Path | None:
    """The pinned NDK through the SDK's sdkmanager, for a machine or a runner whose SDK lacks it."""
    sdkmanager = sdk_root() / "cmdline-tools" / "latest" / "bin" / "sdkmanager"
    if not sdkmanager.exists():
        return None
    subprocess.run([str(sdkmanager), f"ndk;{NDK_VERSION}"], input="y\n" * 8, text=True, stdout=subprocess.DEVNULL)
    return ndk_root()


@contextlib.contextmanager
def release_keystore():
    """The release key's file for the build's duration: MM_ANDROID_KEYSTORE when set, else MM_ANDROID_KEYSTORE_BASE64 (CI's secret)
    unpacked into a private temporary folder that goes with the build, so no copy of the key stays on disk; None for a debug-signed build."""
    if os.environ.get("MM_ANDROID_KEYSTORE"):
        yield os.environ["MM_ANDROID_KEYSTORE"]
        return
    encoded = os.environ.get("MM_ANDROID_KEYSTORE_BASE64")
    if not encoded:
        yield None
        return
    with tempfile.TemporaryDirectory() as folder:   # created readable by its owner alone
        path = Path(folder) / "release.keystore"
        path.write_bytes(base64.b64decode(encoded))
        yield str(path)


def ndk_tool(name: str) -> Path:
    """A tool of the NDK's LLVM toolchain, such as llvm-strip."""
    prebuilt = next((ndk_root() / "toolchains" / "llvm" / "prebuilt").iterdir())
    return prebuilt / "bin" / name


# The launcher icon's sizes, one per screen density, made from the installer's favicon like the macOS and Windows icons.
ICON_SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}


def make_icons(res: Path):
    """The launcher icon at each density, as Android reads it from res/mipmap-<density>/."""
    from PIL import Image
    source = Image.open(ROOT / "mooninstaller" / "favicon.png").convert("RGBA")
    for density, size in ICON_SIZES.items():
        folder = res / f"mipmap-{density}"
        folder.mkdir(parents=True, exist_ok=True)
        source.resize((size, size), Image.LANCZOS).save(folder / "ic_launcher.png")


def version_code(version: str) -> int:
    """Android's version code, which only rises: 7.1.0-dev.120 is 70100120 and 7.1.0 is 70100999, so a release ranks above its own dev builds and below the next minor's."""
    core, _, pre = version.partition("-")
    major, minor, patch = (int(x) for x in (core.split(".") + ["0", "0"])[:3])
    build = int(pre.rsplit(".", 1)[-1]) if pre and pre.rsplit(".", 1)[-1].isdigit() else 999
    return ((major * 100 + minor) * 100 + patch) * 1000 + min(build, 999)


def build_apk(version: str | None) -> int:
    """Gradle packs the library and the icon with the Kotlin shell (android/), signed with the release key where MM_ANDROID_KEYSTORE names it, debug-signed otherwise."""
    make_icons(ROOT / BUILD_DIR / "res")
    env = dict(os.environ)
    env.setdefault("ANDROID_HOME", str(sdk_root()))
    if not env.get("JAVA_HOME") and Path("/opt/homebrew/opt/openjdk@17").exists():
        env["JAVA_HOME"] = "/opt/homebrew/opt/openjdk@17"   # Homebrew's JDK, which macOS does not put on the path
    gradlew = ROOT / "android" / ("gradlew.bat" if os.name == "nt" else "gradlew")
    # A local build carries the version a local host build does, so its version code ranks with CI's and installs over one.
    from compute_version import compute
    app_version = version or compute("local")
    props = [f"-PmmVersion={app_version}", f"-PmmVersionCode={version_code(app_version)}"]
    with release_keystore() as keystore:
        if keystore:
            env["MM_ANDROID_KEYSTORE"] = keystore   # what android/app/build.gradle.kts signs with
        r = subprocess.run([str(gradlew), "-p", str(ROOT / "android"), "assembleRelease", "-q", *props], env=env)
    if r.returncode != 0:
        return r.returncode
    apk = ROOT / "android" / "app" / "build" / "outputs" / "apk" / "release" / "app-release.apk"
    out = ROOT / BUILD_DIR / "MoonLight.apk"
    shutil.copy2(apk, out)
    print(f"Built {out.relative_to(ROOT)}")
    if version:   # the release asset, named as the host packages are
        (ROOT / "dist").mkdir(exist_ok=True)
        shutil.copy2(apk, ROOT / "dist" / f"MoonLight-android-arm64-v{version}.apk")
        # The same package as the program an Android device installs over itself, named as every host's own program is, which the Firmware card and the map look for.
        shutil.copy2(apk, ROOT / "dist" / f"firmware-host-android-arm64-v{version}.bin")
        print(f"Staged dist/MoonLight-android-arm64-v{version}.apk and dist/firmware-host-android-arm64-v{version}.bin")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tests", action="store_true", help="build the unit tests (mm_tests) instead of the program")
    ap.add_argument("--apk", action="store_true", help="build the app: libmoonlight.so packed by Gradle into MoonLight.apk")
    ap.add_argument("--version", help="the release version, which the program and the app carry, and which stages dist/MoonLight-android-arm64-v<version>.apk")
    args = ap.parse_args()

    ndk = ndk_root() or install_ndk()
    if not ndk:
        sys.exit(f"No Android NDK found: install the SDK's command-line tools and `sdkmanager \"ndk;{NDK_VERSION}\"`, or set ANDROID_NDK_HOME.")
    what = "the unit tests" if args.tests else "the app" if args.apk else "MoonLight"
    print(f"Building {what} for Android arm64 into {BUILD_DIR}/ with NDK {ndk.name} ...")
    configure = ["cmake", "-B", BUILD_DIR, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 f"-DCMAKE_TOOLCHAIN_FILE={ndk / 'build' / 'cmake' / 'android.toolchain.cmake'}",
                 "-DANDROID_ABI=arm64-v8a", f"-DANDROID_PLATFORM=android-{API_LEVEL}"]
    # Always set, so the program reports the version the app carries: a version left in CMake's cache by an earlier --version build stays otherwise.
    from compute_version import compute
    configure.append(f"-DMM_VERSION={args.version or compute('local')}")   # bare: CMakeLists.txt adds the quotes
    r = subprocess.run(configure, cwd=ROOT)
    if r.returncode != 0:
        sys.exit(r.returncode)
    target = "mm_tests" if args.tests else "moonlight" if args.apk else "MoonLight"
    r = subprocess.run(["cmake", "--build", BUILD_DIR, "--target", target], cwd=ROOT)
    if r.returncode != 0 or not args.apk:
        sys.exit(r.returncode)
    sys.exit(build_apk(args.version))


if __name__ == "__main__":
    main()
