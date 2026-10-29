#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# ///
"""Build + run the unit suite under AddressSanitizer, ThreadSanitizer or RealtimeSanitizer, as CI does: `--kind <k> --filter ''` runs one lane over the whole suite.

WHY THIS EXISTS: the render/encode multicore split put a second thread beside code that had been
single-threaded for its whole life — a shared output buffer handed between cores, an atomic stop
flag, a try-lock over the WebSocket sender, and a child array that can be reallocated while the
worker walks it. Hand-written tests prove the races we THOUGHT of; the sanitizers find the ones we
didn't. ASan catches use-after-free (a driver deleted while core 1 is inside its tick()); TSan
catches data races (two cores touching one field without synchronization).

macOS toolchain reality (verified 2026-07-13, macOS 26.5 / arm64, with hello-world reproducers —
these are runtime bugs, NOT anything in MoonLight):
  * **Apple clang's ASan HANGS** during its own startup, inside `dyld_shared_cache_iterate_text`;
    a hello-world never reaches main(). So Apple's toolchain is unusable and this script refuses it.
  * **Homebrew LLVM's ASan WORKS.** That is why the script requires it — a difference between
    "runs" and "hangs forever", not a preference.
  * **TSan SEGFAULTS (139) on hello-world under BOTH toolchains.** Thread-race detection is
    therefore unavailable on macOS/arm64 at present; `--kind thread` reports SKIPPED rather than a
    misleading FAIL. Run TSan on Linux (CI, or a container) — that is the only place it works.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import _findings  # noqa: E402

RULES = {
    "address sanitizer": {"reason": "The unit suite fails under AddressSanitizer: a use-after-free or memory error.", "fix": "Run `check_sanitizers.py --kind address` and read the report.", "standard": "docs/contributing/coding-standards.md"},
    "thread sanitizer": {"reason": "The unit suite fails under ThreadSanitizer: a data race between threads.", "fix": "Run `check_sanitizers.py --kind thread` on Linux and read the report.", "standard": "docs/contributing/coding-standards.md"},
}
RULE_OF = {"address": "address sanitizer", "thread": "thread sanitizer"}

# The one toolchain whose sanitizer runtime works on macOS (see the module docstring).
BREW_LLVM = Path("/opt/homebrew/opt/llvm/bin")


def compiler() -> tuple[str, str]:
    """(cc, cxx): Homebrew LLVM on macOS, CC and CXX or the system compiler elsewhere."""
    if sys.platform == "darwin":
        cxx = BREW_LLVM / "clang++"
        if not cxx.exists():
            sys.exit(
                "macOS needs Homebrew LLVM for sanitizers (Apple's ASan runtime hangs at startup).\n"
                "  brew install llvm"
            )
        return str(BREW_LLVM / "clang"), str(cxx)
    # The system compiler, as CMake picks it; an empty variable, as a CI matrix leaves one, means unset.
    return os.environ.get("CC") or "cc", os.environ.get("CXX") or "c++"


def runtime_works(kind: str, cxx: str) -> bool:
    """Does this sanitizer's RUNTIME work on this host at all?

    Compile+run a hello-world first. A broken runtime (Apple ASan hangs; TSan segfaults on
    macOS/arm64) would otherwise surface as a mystery FAIL of the whole suite and send the next
    person hunting a race that isn't there — which is exactly what happened before this check
    existed. A gate you can't trust is worse than no gate.
    """
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "probe.cpp"
        src.write_text("int main() { return 0; }\n")
        exe = Path(tmp) / "probe"
        try:
            subprocess.run([cxx, f"-fsanitize={kind}", "-g", str(src), "-o", str(exe)],
                           check=True, capture_output=True)
            r = subprocess.run([str(exe)], capture_output=True, timeout=20)
            return r.returncode == 0
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            return False   # timeout = the runtime hung; non-zero = it crashed


def run_one(kind: str, filt: str | None, junit: Path | None = None) -> bool:
    """Build + run mm_tests under one sanitizer. Returns True on a clean run."""
    cc, cxx = compiler()
    if not runtime_works(kind, cxx):
        print(f"\n=== {kind.upper()} ===")
        # Skip ONLY the one runtime we have positively identified as broken-by-toolchain: TSan on
        # macOS/arm64 (segfaults on hello-world under both Apple and Homebrew clang). Anything else —
        # notably ANY probe failure on Linux, where CI runs — is a real problem with the toolchain or
        # the build, and must FAIL. A gate that turns every unexpected failure into a green skip is
        # worse than no gate: it would have quietly stopped guarding the multicore code.
        if kind == "thread" and sys.platform == "darwin":
            print("  SKIPPED: TSan's runtime is broken on macOS/arm64 (hello-world segfaults; a")
            print("  toolchain bug, not a finding). `act -j sanitizers` runs CI's Linux lane locally.")
            return True
        print(f"  FAIL — the {kind} runtime does not run a hello-world on this host.")
        print("  That is a broken toolchain or build, not a skippable condition. Investigate.")
        return False
    build = ROOT / "build" / f"san-{kind}"
    flags = f"-fsanitize={kind} -fno-omit-frame-pointer -g"

    print(f"\n=== {kind.upper()} ===")
    subprocess.run(
        ["cmake", "-S", str(ROOT), "-B", str(build), "-DCMAKE_BUILD_TYPE=Debug",
         f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}",
         f"-DCMAKE_CXX_FLAGS={flags}", f"-DCMAKE_EXE_LINKER_FLAGS=-fsanitize={kind}"],
        check=True, stdout=subprocess.DEVNULL,
    )
    subprocess.run(["cmake", "--build", str(build), "--target", "mm_tests", f"-j{os.cpu_count() or 8}"],
                   check=True, stdout=subprocess.DEVNULL)

    env = dict(os.environ)
    # Leak detection is a separate concern from the races/UAF this gate is for, and it fires on
    # third-party static init. Keep the signal on what we're actually hunting.
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    env["TSAN_OPTIONS"] = "halt_on_error=0"   # every race of the run reported, the run still failing: halting at the first hid the next one behind each fix
    # RTSan reports and does not halt: the render path's known blocking calls are counted by check_code and backlogged, so halting would fail every run; the log is the signal.
    env["RTSAN_OPTIONS"] = "halt_on_error=0"

    cmd = [str(build / "test" / "mm_tests")]
    if filt:
        cmd.append(f"-tc={filt}")
    if junit:
        junit.parent.mkdir(parents=True, exist_ok=True)
        cmd += ["--reporters=junit", f"--out={junit}"]
    proc = subprocess.run(cmd, env=env)
    ok = proc.returncode == 0
    print(f"  {kind}: {'PASS' if ok else 'FAIL'}")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    # `realtime` is the runtime half of the hot-path check: MM_NONBLOCKING marks the tick methods, -Wfunction-effects proves what it can at compile time, and RTSan catches what reaches blocking through virtual dispatch. It needs Clang 20, so CI names clang-20 through CC and CXX.
    ap.add_argument("--kind", choices=["address", "thread", "realtime", "both"], default="both")
    # The concurrency suites are the point of this gate; the full suite is available but slower.
    ap.add_argument("--filter", default="render-split*,worker seam*,TryLock*",
                    help="doctest -tc filter; pass '' for the whole suite")
    ap.add_argument("--junit", type=Path, help="write the unit results as JUnit here, for CI's annotations")
    args = ap.parse_args()

    if not shutil.which("cmake"):
        sys.exit("cmake not found")

    kinds = ["address", "thread"] if args.kind == "both" else [args.kind]
    # Run BOTH, then aggregate — `all(generator)` short-circuits, so an ASan failure would skip TSan
    # entirely and hide a race behind a use-after-free. They are independent signals; report both.
    results = [run_one(k, args.filter or None, args.junit) for k in kinds]
    ok = all(results)
    if not args.filter:   # a filtered run is part of the suite, so only the whole one is the record
        _findings.write("check_sanitizers", RULES,
                        [{"rule": RULE_OF[k], "message": f"mm_tests fails under the {k} sanitizer", "level": "error"}
                         for k, passed in zip(kinds, results) if not passed and k in RULE_OF])
    print("\nSanitizers:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
