#!/usr/bin/env python3
"""Build the host target into a host-named build directory.

``build/macos/`` on macOS arm64, ``build/linux/`` on Linux, ``build/windows/``
on Windows — so a single machine could (in principle) build multiple host
flavours without one wiping the other, and so the layout matches the
ESP32 side (``build/esp32-<board>/``, one dir per target).

Builds ONE target, not the whole project: the firmware binary (``MoonLight``) by
default, or the test binaries (``--tests`` → ``mm_tests``, ``mm_scenarios`` and ``mm_tempo``).
Keeping them separate means the "just give me the binary" build doesn't wait on
the ~130-file test suite (and vice-versa).
"""

import argparse
import platform
import re
import shutil
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def host_build_dir() -> str:
    """Map platform.system() to the host build-dir name.

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


def host_binary(build_dir=None):
    """The built host executable inside `build_dir` (default: this host's), or None.

    Where it lands depends on the generator, not the platform: a single-config generator (Ninja,
    Makefiles) writes it at the build root, a multi-config one (Visual Studio, Xcode) puts it under
    a per-config subdirectory. So all four spellings are real, and NEWEST WINS rather than a fixed
    priority: a machine that has built with both generators holds two of them, and any fixed order
    reports whichever the order happened to name instead of what was last built.

    ONE definition, because the copies had already drifted: collect_kpi.py looked for bare, .exe,
    Release/.exe while repo_health.py looked for Release/.exe, .exe, bare, so a single
    `collect_kpi --commit` could take `binary_kb` from one file and the `flash` figure from another.
    """
    root = Path(build_dir) if build_dir else ROOT / host_build_dir()
    found = [p for p in (root / "MoonLight", root / "MoonLight.exe",
                         root / "Release" / "MoonLight", root / "Release" / "MoonLight.exe")
             if p.exists()]
    return max(found, key=lambda p: p.stat().st_mtime) if found else None


def test_binary(name, build_dir=None):
    """A host binary of the test tree (mm_bench, mm_tempo) inside `build_dir` (default: this host's): at test/ for a single-config generator, test/Release/ for a multi-config one, and the first of those when neither exists yet."""
    root = Path(build_dir) if build_dir else ROOT / host_build_dir()
    exe = f"{name}.exe" if platform.system() == "Windows" else name
    for candidate in (root / "test" / exe, root / "test" / "Release" / exe):
        if candidate.exists():
            return candidate
    return root / "test" / exe


TOOLSET = "ClangCL"   # Visual Studio's Clang toolset


def toolset_args():
    """The CMake arguments naming this host's compiler toolset: clang-cl on Windows, so every host compiles with Clang or GCC and the hot loops vectorize alike; none elsewhere.

    A toolset belongs to the Visual Studio generators, so a `CMAKE_GENERATOR` naming another, such as Ninja, gets none and takes its compiler from `CC` and `CXX`."""
    generator = os.environ.get("CMAKE_GENERATOR", "")
    if platform.system() != "Windows" or (generator and not generator.startswith("Visual Studio")):
        return []
    return ["-T", TOOLSET]


def drop_other_toolset(bdir):
    """Remove a build directory configured with another toolset, which CMake refuses to reconfigure, and say so; a directory on this one, or on no toolset, stays."""
    cache = ROOT / bdir / "CMakeCache.txt"
    if not toolset_args() or not cache.exists():
        return
    was = re.search(r"^CMAKE_GENERATOR_TOOLSET:INTERNAL=(.*)$", cache.read_text(encoding="utf-8", errors="replace"), re.M)
    if was and was.group(1) != TOOLSET:
        print(f"{bdir} was configured with {was.group(1) or 'MSVC'}, and CMake keeps a directory's toolset: removing it for a clean {TOOLSET} configure.")
        shutil.rmtree(ROOT / bdir)


def build_target(name, quiet=False):
    """Configure this host's tree as Release and build the one target `name`, as the bench scripts do; the exit code of the step that failed, or 0."""
    bdir = str(ROOT / host_build_dir())
    drop_other_toolset(host_build_dir())
    out = subprocess.DEVNULL if quiet else None
    for cmd in (["cmake", "-B", bdir, "-DCMAKE_BUILD_TYPE=Release", *toolset_args()],
                ["cmake", "--build", bdir, "--target", name, "--parallel", str(build_jobs())] + (["--config", "Release"] if platform.system() == "Windows" else [])):
        rc = subprocess.run(cmd, cwd=ROOT, stdout=out).returncode
        if rc != 0:
            return rc
    return 0


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


def lower_priority(by: int = 10) -> None:
    """Run this process `by` steps nicer, where the system has priorities; one already at the lowest stays there, as a caller that niced it
    (the gate runner's `nice -n 10`) leaves it, since macOS refuses a step past the lowest with EPERM rather than clamping."""
    if hasattr(os, "nice"):
        try:
            os.nice(by)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gcc", action="store_true",
                    help="build with GCC instead of the default compiler — the toolchain CI uses. "
                         "Catches the warnings clang does not emit.")
    ap.add_argument("--no-jit", action="store_true",
                    help="build as a host WITHOUT a MoonLive backend (forces "
                         "MM_MOONLIVE_HAS_HOST_JIT=0). Every x86-64 host is one: Windows, Linux "
                         "and Intel macOS ship no backend, so a test that presumes a compile "
                         "succeeds passes on an arm64 bench and fails only on CI. This runs the "
                         "suite the way those hosts see it.")
    ap.add_argument("--tests", action="store_true",
                    help="compile the test binaries (mm_tests, mm_scenarios, mm_tempo) instead of the firmware. "
                         "The default build makes only MoonLight; the ~130 test units are a separate, "
                         "slower compile, run afterwards by test_host.py / run_scenario.py.")
    ap.add_argument("--configure", action="store_true",
                    help="configure the tree and generate build_id.h, without compiling: what check_code and "
                         "check_docgen read, as CI's findings job runs them")
    ap.add_argument("--build-dir", metavar="DIR",
                    help="build into DIR instead of this host's tree: a container build of the same checkout, "
                         "whose CMake cache records another source path, keeps its own")
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
        print("Forcing MM_MOONLIVE_HAS_HOST_JIT=0: the x86-64 host's view.")
    if args.build_dir:
        bdir = args.build_dir
    what = "test binaries" if args.tests else "host target"
    print(f"Configuring {bdir}/ and generating build_id.h ..." if args.configure else f"Building {what} into {bdir}/ ...")
    # CMAKE_BUILD_TYPE is honored by single-config generators (Ninja, Make).
    # Visual Studio is multi-config and ignores it — we pass --config Release at
    # build time below. Setting CMAKE_BUILD_TYPE on multi-config is harmless.
    drop_other_toolset(bdir)
    r = subprocess.run(
        ["cmake", "-B", bdir, f"-DCMAKE_BUILD_TYPE={build_type}", *toolset_args(), *extra],
        cwd=ROOT,
    )
    if r.returncode != 0:
        sys.exit(r.returncode)
    if args.configure:
        sys.exit(subprocess.run([sys.executable, str(ROOT / "moondeck" / "build" / "generate_build_info.py")], cwd=ROOT).returncode)

    # Build a SPECIFIC target, never the whole "all": the firmware and the ~130 test translation units are separate concerns, so the host binary builds alone and a header edit leaves the test suite out of the compile.
    # `--tests` builds the test binaries instead: mm_tests (test_host.py), mm_scenarios (run_scenario.py) and mm_tempo (the tempo harness's end-to-end test).
    targets = ["mm_tests", "mm_scenarios", "mm_tempo"] if args.tests else ["MoonLight"]
    lower_priority()   # a build yields to MoonLight, the editor and the browser, whoever starts it; the compilers inherit it
    build_cmd = ["cmake", "--build", bdir, "--target", *targets, "--parallel", str(build_jobs())]
    if is_windows:
        build_cmd += ["--config", "Release"]
    r = subprocess.run(build_cmd, cwd=ROOT)
    sys.exit(r.returncode)


def build_jobs() -> int:
    """Compile jobs at once, for every build a script starts: CMAKE_BUILD_PARALLEL_LEVEL when set, every core on CI, where the build runs alone, else the cores less two and one job per GB above 4 GB of memory, so MoonLight, the editor and the browser keep their cores and memory stays in RAM."""
    level = os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL", "")
    if level.isdigit() and int(level) > 0:
        return int(level)
    if os.environ.get("CI"):
        return os.cpu_count() or 2
    by_cores = (os.cpu_count() or 2) - 2
    return max(1, min(by_cores, memory_gb() - 4))


def memory_gb() -> int:
    """This machine's memory in whole GB, or a large number where it cannot be read, leaving the cores to bound the jobs."""
    try:
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") // (1024 ** 3)
    except (ValueError, OSError, AttributeError):
        return 1 << 10


if __name__ == "__main__":
    main()
