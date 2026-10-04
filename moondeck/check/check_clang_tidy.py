#!/usr/bin/env python3
"""Run clang-tidy over the desktop build and print a readable findings report.

The config is [.clang-tidy](../../.clang-tidy) at the repo root — the same file clangd reads,
so the editor and this report never disagree. This script only adds what a raw `run-clang-tidy`
dump lacks: deduplication (a header included by N translation units is reported N times), a
per-check and per-file summary, and a filter down to our own files. Output goes to stdout —
MoonDeck shows it in the log pane, so a run answers on the spot instead of writing a file to
go open.

Not a gate. `.clang-tidy` sets `WarningsAsErrors: ''` while the initial findings are triaged,
so this reports and exits 0 unless `--fail-on-findings` says otherwise.

Usage:
  uv run moondeck/check/check_clang_tidy.py                  # full run, report to stdout
  uv run moondeck/check/check_clang_tidy.py --fail-on-findings
  uv run moondeck/check/check_clang_tidy.py --check bugprone-use-after-move
"""

import argparse
import collections
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# Third-party code that lives inside our tree. Findings here are upstream's to fix.
VENDORED = ("test/doctest.h", "src/platform/desktop/vendor/miniaudio.h")

# A diagnostic line: path:line:col: severity: message [check-name]
#
# The trailing bracket can hold MORE than the check name: with WarningsAsErrors set, clang-tidy
# emits `[clang-diagnostic-unused-variable,-warnings-as-errors]`. A pattern that allowed only
# [a-z.-] rejected the comma and silently dropped EVERY finding — turning the ratchet itself
# into a green-looking zero the moment it was switched on. Capture the whole bracket, then keep
# the first comma-separated name.
_DIAG = re.compile(r"^(?P<file>\S+?):(?P<line>\d+):(?P<col>\d+): "
                   r"(?P<sev>warning|error): (?P<msg>.*?) \[(?P<check>[^\]]+)\]$")


def _host_build_dir():
    """The build dir holding the compile_commands.json to analyze against.

    Two can exist: build/ (a plain `cmake --build build`) and build/<host>/ (what the build
    script writes). Pick whichever database is NEWEST rather than assuming, because a database
    from a configure that never actually built leaves generated headers missing — clang-tidy
    then reports `file not found` and SKIPS that file entirely. A skipped file looks identical
    to a clean one in the report, so the stale-database case must not win by default.
    """
    host = {"darwin": "macos", "linux": "linux", "win32": "windows"}.get(sys.platform, "macos")
    candidates = [d for d in (ROOT / "build" / host, ROOT / "build")
                  if (d / "compile_commands.json").exists()]
    if not candidates:
        return ROOT / "build" / host          # caller reports the missing-database error
    return max(candidates, key=lambda d: (d / "compile_commands.json").stat().st_mtime)


def _find_tool(name):
    """clang-tidy is often not on PATH on macOS (Xcode does not ship it). Prefer whatever is
    on PATH, then the usual Homebrew LLVM location, so this works without setup."""
    return shutil.which(name) or next(
        (p for p in (f"/opt/homebrew/opt/llvm/bin/{name}", f"/usr/local/opt/llvm/bin/{name}")
         if Path(p).exists()), None)


def _toolchain_args():
    """Extra flags so clang-tidy can find the standard library the DATABASE's compiler uses.

    The trap this exists for: CMake records whichever compiler it picked (on macOS that is
    Apple Clang at /usr/bin/c++), but clang-tidy is usually a *different* clang — Homebrew
    LLVM, or a distro package. A different clang does not know Apple Clang's SDK paths, so
    every file fails with `'cstdint' file not found`, clang-tidy abandons it, and the run
    reports almost nothing. **It looks like a clean codebase.** Measured here: 129 of 129
    files errored that way, hiding a real integer-division bug in BouncingBallsEffect.

    macOS needs `-isysroot` pointing at the active SDK. Linux and Windows do not: the system
    clang and gcc share include paths there, so no argument is needed and this returns empty.
    Deliberately NOT "require Homebrew clang" — that would make the check macOS-only.
    """
    if sys.platform != "darwin":
        return []
    sdk = subprocess.run(["xcrun", "--show-sdk-path"],
                         capture_output=True, text=True).stdout.strip()
    return [f"-isysroot{sdk}"] if sdk else []


def run(build_dir, check_filter=None, tu_regex=None):
    """Run clang-tidy across the compilation database; return parsed, deduplicated findings."""
    runner = _find_tool("run-clang-tidy")
    if not runner:
        print("run-clang-tidy not found. Install LLVM (brew install llvm) or add it to PATH.",
              file=sys.stderr)
        return None

    cmd = [runner, "-p", str(build_dir), "-quiet", "-j", str(os.cpu_count() or 4)]
    # `-extra-arg=VALUE`, never `-extra-arg VALUE`: run-clang-tidy silently ignores the
    # space-separated form, which produced an empty report that read as a clean tree.
    cmd += [f"-extra-arg={extra}" for extra in _toolchain_args()]
    if check_filter:
        # `=` form, for the same reason as -extra-arg above: run-clang-tidy silently ignores the
        # space-separated form, so `--check X` filtered nothing and reported 0 like a clean tree.
        cmd += [f"-checks=-*,{check_filter}"]

    # run-clang-tidy shells out to `clang-tidy` BY NAME, so it must be on PATH even though we
    # resolved the runner by absolute path. Without this it exits 0 having analyzed nothing —
    # a silent empty report that reads exactly like a clean tree. Put the runner's own
    # directory first so the pair always comes from one toolchain (a clang-tidy from a
    # different LLVM than the runner is the other way this goes quietly wrong).
    # A trailing positional restricts which TUs are PARSED, not just which findings are kept —
    # measured 3s vs 227s for one module. Only .cpp files can be named: a header is not a TU, so
    # a header-only module still needs the full parse and the report filter below.
    if tu_regex:
        cmd.append(tu_regex)

    env = dict(os.environ)
    env["PATH"] = f"{Path(runner).parent}{os.pathsep}{env.get('PATH', '')}"

    # clang-tidy exits non-zero when it finds anything; that is data, not failure.
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, env=env)
    out = proc.stdout

    # A run that analyzed nothing prints no per-file progress and yields no findings, which
    # is indistinguishable from a clean tree in the report. Treat it as an error instead:
    # a silent "0 findings" is the worst possible output for a check like this.
    if "clang-tidy" in proc.stderr and "not found" in proc.stderr:
        print(f"run-clang-tidy could not invoke clang-tidy:\n{proc.stderr.strip()}",
              file=sys.stderr)
        return None

    seen, rows = set(), []
    for line in out.splitlines():
        m = _DIAG.match(line.replace(str(ROOT) + "/", ""))
        if not m:
            continue
        d = m.groupdict()
        d["check"] = d["check"].split(",")[0]      # drop the `,-warnings-as-errors` suffix
        # Our code only: a TU drags in SDK and vendored headers we neither own nor fix.
        # test/doctest.h is vendored too — it sits under test/ but is upstream's single-header
        # release, so its findings (4 NewDeleteLeaks + a garbage-value read) are not ours to act
        # on and would never reach zero.
        if not d["file"].startswith(("src/", "test/")) or d["file"] in VENDORED:
            continue
        # A header analyzed via N translation units yields N identical diagnostics.
        key = (d["file"], d["line"], d["col"], d["check"])
        if key in seen:
            continue
        seen.add(key)
        rows.append(d)

    # A file that fails to compile is never analyzed, so widespread errors mean the report is
    # measuring nothing — the failure that hid a real bug here until it was chased down. Treat
    # "most files errored" as a broken run rather than a result, because the alternative is a
    # short, clean-looking report that is entirely fictional.
    errors = sum(1 for r in rows if r["check"] == "clang-diagnostic-error")
    if errors > 10:
        print(f"{errors} files failed to compile — clang-tidy analysed almost nothing.\n"
              f"Usually a toolchain mismatch: the compilation database was written by a\n"
              f"different compiler than clang-tidy is using. Rebuild the desktop target, or\n"
              f"check _toolchain_args() covers this platform.", file=sys.stderr)
        return None

    return sorted(rows, key=lambda r: (r["file"], int(r["line"])))


def render(rows, commit):
    """The findings as plain text, printed straight to the log.

    Deliberately NOT a file: a 2-3 minute run that answers with "wrote a file, go look" makes
    you open a second thing to learn what happened, and the file was gitignored anyway — it
    existed only to be read once. Space-aligned columns rather than Markdown tables, since the
    output pane is a plain-text log (same reasoning as check_code.py's table).
    """
    by_check = collections.Counter(r["check"] for r in rows)
    by_file = collections.Counter(r["file"] for r in rows)

    L = [f"clang-tidy · {datetime.now():%Y-%m-%d %H:%M} · commit {commit} · "
         f"{len(rows)} findings"]

    if not rows:
        # Kept deliberately: this is not an explanation but the check on the result — a zero
        # from a run that analyzed nothing looks identical to a clean tree.
        L += ["", "No findings. ✓",
              "Verify with a check that must fire: "
              "--check readability-magic-numbers"]
        return "\n".join(L)

    check_w = max(len(c) for c in by_check)
    L += ["", "BY CHECK", f"  {'COUNT':>5}  CHECK", f"  {'-' * 5}  {'-' * check_w}"]
    L += [f"  {n:>5}  {c}" for c, n in by_check.most_common()]

    file_w = min(max(len(f) for f in by_file), 60)
    L += ["", "WORST FILES", f"  {'COUNT':>5}  FILE", f"  {'-' * 5}  {'-' * file_w}"]
    L += [f"  {n:>5}  {f}" for f, n in by_file.most_common(15)]

    L += ["", "ALL FINDINGS"]
    current = None
    for r in rows:
        if r["file"] != current:
            current = r["file"]
            L += ["", f"  {current}"]
        L.append(f"    {r['line']:>6}  {r['msg']}  [{r['check']}]")
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--fail-on-findings", action="store_true",
                    help="Exit 1 if anything is found (for when this becomes a gate).")
    ap.add_argument("--check", help="Run one check only, e.g. bugprone-use-after-move.")
    ap.add_argument("--module", help="Report only findings in this module's source files.")
    args = ap.parse_args()

    build_dir = _host_build_dir()
    if not (build_dir / "compile_commands.json").exists():
        print(f"No compile_commands.json in {build_dir.relative_to(ROOT)} — "
              f"run `uv run moondeck/build/build_desktop.py` first.", file=sys.stderr)
        return 2

    only, tu_regex = None, None
    if args.module:
        # Resolver lives in check_clang_query (one owner) so every tool scopes a module the same.
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import check_clang_query
        only = check_clang_query.module_files(args.module)
        if not only:
            print(f"No source files for module '{args.module}'.", file=sys.stderr)
            return 2
        print(f"Filtered to {args.module}: {', '.join(only)}")
        # Parse only the TUs that actually reach this module's files. A header-only module has
        # no TU of its own, but it IS included by one or two — analyzing those beats parsing all
        # 15 (measured: 18s vs 263s for a header included only by main.cpp).
        tus = check_clang_query.including_tus(only, build_dir)
        all_tus = check_clang_query._source_tus(build_dir)
        if tus and len(tus) < len(all_tus):
            tu_regex = "|".join(re.escape(Path(f).name) for f in tus)
            print(f"Parsing {len(tus)} of {len(all_tus)} TUs: "
                  f"{', '.join(Path(f).name for f in tus)}")
        print()

    rows = run(build_dir, args.check, tu_regex)
    if rows is None:
        return 2
    if only:
        rows = [r for r in rows if r["file"] in only]

    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                            capture_output=True, text=True).stdout.strip()
    print(render(rows, commit))

    return 1 if (args.fail_on_findings and rows) else 0


if __name__ == "__main__":
    sys.exit(main())
