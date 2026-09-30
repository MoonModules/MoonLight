#!/usr/bin/env python3
"""Build the desktop target into a host-named build directory.

``build/macos/`` on macOS arm64, ``build/linux/`` on Linux, ``build/windows/``
on Windows — so a single machine could (in principle) build multiple host
flavours without one wiping the other, and so the layout matches the
ESP32 side (``build/esp32-<board>/``, one dir per target).

Builds ONE target, not the whole project: the firmware binary (``MoonLight``) by
default, or the test binaries (``--tests`` → ``mm_tests`` + ``mm_scenarios``).
Keeping them separate means the "just give me the binary" build doesn't wait on
the ~130-file test suite (and vice-versa).
"""

import argparse
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def host_build_dir() -> str:
    """Map platform.system() to the desktop build-dir name.

    Returns a slash-string (not Path) so the cmake -B argument stays
    portable across shells.
    """
    sys_name = platform.system()
    return {
        "Darwin":  "build/macos",
        "Linux":   "build/linux",
        "Windows": "build/windows",
    }.get(sys_name, f"build/{sys_name.lower()}")


# The g++ names a real GCC install goes by, newest first. ONE list: _gates.py's GCC-build trigger
# reads it too, to decide whether that gate applies at all, and a second copy there would silently
# disagree the first time a version is added.
GCC_CANDIDATES = ("g++-16", "g++-15", "g++-14", "g++-13")


def desktop_binary(build_dir=None):
    """The built desktop executable inside `build_dir` (default: this host's), or None.

    Where it lands depends on the generator, not the platform: a single-config generator (Ninja,
    Makefiles) writes it at the build root, a multi-config one (Visual Studio, Xcode) puts it under
    a per-config subdirectory. So all four spellings are real, and NEWEST WINS rather than a fixed
    priority: a machine that has built with both generators holds two of them, and any fixed order
    reports whichever the order happened to name instead of what was last built.

    ONE definition, because the copies had already drifted: collect_kpi.py looked for bare, .exe,
    Release/.exe while repo_health.py looked for Release/.exe, .exe, bare, so a single
    `collect_kpi --commit` could take `binary_kb` from one file and `flash.desktop` from another.
    """
    root = Path(build_dir) if build_dir else ROOT / host_build_dir()
    found = [p for p in (root / "MoonLight", root / "MoonLight.exe",
                         root / "Release" / "MoonLight", root / "Release" / "MoonLight.exe")
             if p.exists()]
    return max(found, key=lambda p: p.stat().st_mtime) if found else None


def gcc_pair():
    """(cc, cxx) for a real GCC, or exit with how to get one.

    WHY THIS MODE EXISTS: CI compiles with GCC, every local gate compiles with clang, and the two
    disagree — GCC emits -Wstringop-truncation / -Wformat-truncation / -Wformat-zero-length /
    -Wunused-function where clang stays silent, and clang leaks standard headers transitively where
    GCC does not. With -Werror a hard rule, every divergence is a CI failure you cannot see locally.
    That cost four push-and-discover cycles once; this flag is so it costs zero.
    """
    for cxx in GCC_CANDIDATES:
        if shutil.which(cxx):
            return cxx.replace("g++", "gcc"), cxx
    sys.exit("no GCC found — `brew install gcc` (macOS) or `apt install g++` (Linux).\n"
             "Note /usr/bin/g++ on macOS is clang in disguise; it will NOT catch these.")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gcc", action="store_true",
                    help="build with GCC instead of the default compiler — the toolchain CI uses. "
                         "Catches the warnings clang does not emit.")
    ap.add_argument("--no-jit", action="store_true",
                    help="build as a desktop WITHOUT a MoonLive backend (forces "
                         "MM_MOONLIVE_HAS_HOST_JIT=0). Every x86-64 desktop is one: Windows, Linux "
                         "and Intel macOS ship no backend, so a test that presumes a compile "
                         "succeeds passes on an arm64 bench and fails only on CI. This runs the "
                         "suite the way those hosts see it.")
    ap.add_argument("--tests", action="store_true",
                    help="compile the test binaries (mm_tests + mm_scenarios) instead of the firmware. "
                         "The default build makes only MoonLight; the ~130 test units are a separate, "
                         "slower compile, run afterwards by test_desktop.py / run_scenario.py.")
    args = ap.parse_args()

    bdir = host_build_dir()
    is_windows = platform.system() == "Windows"
    extra = []
    build_type = "Release"
    if args.gcc:
        cc, cxx = gcc_pair()
        bdir = "build/gcc"          # its own dir — do not clobber the clang build's cache
        # DEBUG, because that is what CI's job builds. Release turns on more inlining, which lets GCC
        # prove more about buffer sizes and emit ~17 further -Wformat-truncation/-Wstringop-* warnings
        # that CI never sees — real, but a separate cleanup, not a merge gate. Match CI, not more.
        build_type = "Debug"
        extra = [f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}"]
        print(f"Using GCC ({cxx}) in Debug — the exact toolchain + build type CI uses.")
    if args.no_jit:
        # Its own build dir, like --gcc: the macro changes which TEST_CASEs compile at all, so
        # sharing a cache with the normal build would mean rebuilding the world on every switch.
        bdir = "build/nojit"
        extra += ["-DMM_MOONLIVE_NO_HOST_JIT=ON"]
        print("Forcing MM_MOONLIVE_HAS_HOST_JIT=0 — the x86-64 desktop's view.")
    what = "test binaries" if args.tests else "desktop target"
    print(f"Building {what} into {bdir}/ ...")
    # CMAKE_BUILD_TYPE is honoured by single-config generators (Ninja, Make).
    # Visual Studio is multi-config and ignores it — we pass --config Release at
    # build time below. Setting CMAKE_BUILD_TYPE on multi-config is harmless.
    r = subprocess.run(
        ["cmake", "-B", bdir, f"-DCMAKE_BUILD_TYPE={build_type}", *extra],
        cwd=ROOT,
    )
    if r.returncode != 0:
        sys.exit(r.returncode)

    # Build a SPECIFIC target, never the whole "all" — the firmware and the ~130 test translation units are
    # separate concerns. Default builds only MoonLight (the "give me the desktop binary" path stays
    # fast; a header edit no longer drags the test suite through the compiler). `--tests` builds the test
    # binaries instead, which test_desktop.py runs (mm_tests) and run_scenario.py runs (mm_scenarios).
    targets = ["mm_tests", "mm_scenarios"] if args.tests else ["MoonLight"]
    build_cmd = ["cmake", "--build", bdir, "--target", *targets]
    if is_windows:
        build_cmd += ["--config", "Release"]
    r = subprocess.run(build_cmd, cwd=ROOT)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
