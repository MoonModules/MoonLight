#!/usr/bin/env python3
"""Run scenario tests. Replays scenario JSON files via the in-process runner.

Filters compose:
  --name <stem>     run that one scenario file (the JSON stem, e.g. scenario_Layer_base_pipeline)
  --module <Name>   run every scenario whose top-level `module` (or `also`) matches
  --name + --module the named scenario must also match the module (otherwise refused)
  (neither)         run every scenario the runner discovers

--update-contract   renegotiate the per-step performance contract. After each
--reason "..."      scenario, parse its MEASURE lines and write the observed
                    tick_us / free_heap into contract[<host-target>] along with
                    set_by + reason. This is the "I want to change the promise"
                    path, not a routine baseline refresh. Both flags required
                    together; symmetric with run_live_scenario.py.
"""

import argparse
import datetime
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
# Reuse the shared test-metadata parser so scenario discovery stays in one place.
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
import _test_metadata as test_meta  # noqa: E402
sys.path.insert(0, str(ROOT / "moondeck" / "scenario"))
import _observed  # noqa: E402

_HOST = {"darwin": "macos", "win32": "windows"}.get(sys.platform, "linux")


def _resolve_runner() -> Path:
    """Find mm_scenarios. MSVC multi-config drops it in test/Release/; Ninja and
    single-config generators drop it in test/. Check both so the script works
    with either layout."""
    base = ROOT / "build" / _HOST / "test" / "mm_scenarios"
    suffix = ".exe" if sys.platform == "win32" else ""
    candidates = [base.with_suffix(suffix)]
    if sys.platform == "win32":
        candidates.insert(0, base.parent / "Release" / f"mm_scenarios{suffix}")
    for c in candidates:
        if c.exists():
            return c
    return candidates[-1]  # fall through with the simplest path for the error message


RUNNER = _resolve_runner()


# What feeds mm_scenarios. Same question check_esp32_built.py asks of a firmware image, for
# the same reason: a binary older than its sources reports on code that is no longer there.
#
# `src/` plus test/scenario_runner.cpp ONLY — that is the whole of what the target compiles
# (`add_executable(mm_scenarios scenario_runner.cpp)` + mm_core/mm_platform). Watching all of
# test/ made every unit-test edit report the runner stale, and rebuilding did not clear it
# because CMake correctly relinks nothing: a false alarm that trains people to ignore the guard.
_RUNNER_SOURCE_DIRS = ("src",)
# src/platform/esp32 is in the tree but NOT in this target: the desktop runner links mm_platform's
# desktop half, so an ESP32 edit relinks nothing and rebuilding can never clear the warning. Left in
# scope it wedges the gate permanently, which is the same false alarm the note above is about.
_RUNNER_SKIP_DIRS = ("src/platform/esp32",)
_RUNNER_SOURCE_FILES = ("test/scenario_runner.cpp",)
_RUNNER_SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp"}
_RUNNER_SKIP_PARTS = {"build", "__pycache__", ".git"}
# GENERATED sources are re-emitted (identical content, fresh mtime) by every ESP32 build, so a
# timestamp comparison calls the runner stale whenever a firmware was built more recently —
# rebuilding clears it only until the next firmware build. They are excluded because their mtime
# carries no information about whether the runner is out of date; a real edit reaches them through
# their INPUT (src/ui/app.js), which is watched.
# build_info.h belongs here for the same reason and a sharper one: it embeds `git status
# --porcelain` as a `+` dirty-suffix, so its CONTENT changes the moment the tree goes dirty. A gate
# run writes scenario baselines and repo-health metrics, which dirties the tree, which flips the
# suffix, which makes every binary look stale on the NEXT run. A build id is not code, so it cannot
# make the runner "report on code that is no longer there", which is what this guard is for.
_RUNNER_GENERATED = {"src/ui/ui_embedded.h", "src/core/util/build_info.h"}


def _stale_runner_reason() -> str:
    """The newest source file NEWER than the runner binary, or "" when it is fresh.

    This exists because a stale runner is INVISIBLE: it runs, it prints PASSED, and every
    assertion is against code that has since changed. It cost ten days of three MoonLive
    scenarios silently asserting nothing — `set_control` steps naming a control the modules no
    longer had, reported as applied by a binary built before the rename.

    Freshness is measured against the SOURCES, never the clock: a wall-clock rule passes a
    binary that predates an edit made minutes ago, which is the exact trap this catches.

    The trap is easy to fall into because two build directories exist: `cmake --build build`
    (what a developer types) writes build/, while this script runs build/<host>/ — the tree
    build_desktop.py produces. Rebuilding the wrong one leaves the runner untouched and no
    error anywhere.
    """
    if not RUNNER.exists():
        return "missing"
    binary_mtime = RUNNER.stat().st_mtime
    newest, newest_path = 0.0, None
    candidates = []
    for d in _RUNNER_SOURCE_DIRS:
        # Skip-parts are matched against the path RELATIVE to ROOT: `f.parts` is absolute, so a
        # checkout living under a directory called "build" (or ".git") would match every file and
        # skip the whole scan — the guard would then pass on any binary, silently.
        candidates.extend(f for f in (ROOT / d).rglob("*")
                          if f.is_file() and f.suffix in _RUNNER_SOURCE_SUFFIXES
                          and not (_RUNNER_SKIP_PARTS & set(f.relative_to(ROOT).parts))
                          and not f.relative_to(ROOT).as_posix().startswith(_RUNNER_SKIP_DIRS)
                          and f.relative_to(ROOT).as_posix() not in _RUNNER_GENERATED)
    candidates.extend(ROOT / f for f in _RUNNER_SOURCE_FILES if (ROOT / f).is_file())
    for f in candidates:
        m = f.stat().st_mtime
        if m > newest:
            newest, newest_path = m, f
    if newest > binary_mtime and newest_path is not None:
        mins = (newest - binary_mtime) / 60.0
        return (f"{newest_path.relative_to(ROOT)} is {mins:.0f} min newer than the runner")
    return ""


def _require_fresh_runner() -> None:
    """Refuse to run against a missing or stale binary, naming the rebuild command."""
    reason = _stale_runner_reason()
    if not reason:
        return
    rel = RUNNER.relative_to(ROOT) if RUNNER.is_relative_to(ROOT) else RUNNER
    if reason == "missing":
        print(f"Scenario runner not built: {rel}")
    else:
        print(f"Scenario runner is STALE: {rel}")
        print(f"  {reason}")
        print("  It would report on code that is no longer there.")
    print("  rebuild: uv run moondeck/build/build_desktop.py --tests")
    sys.exit(1)

# Format emitted by scenario_runner.cpp's measure block:
#   MEASURE <step-name>: tick=Nus FPS=N lights=N heap=N (step: ±N) block=N
# `<step-name>` may contain hyphens and underscores. heap is the absolute free
# heap after the measurement (what observed.free_heap consumes); the (step: ±N)
# fragment is a human-readable delta for diagnostics — not captured here.
# Both heap and block are 0 on desktop where the platform stubs return
# "unlimited".
_MEASURE_RE = re.compile(
    r"^\s*MEASURE\s+(?P<name>\S+):\s+tick=(?P<tick>\d+)us\s+FPS=\d+\s+lights=\d+"
    r"\s+heap=(?P<heap>\d+).*?\bblock=(?P<block>\d+)"
)


def _host_target() -> str:
    """Same shape run_live_scenario.py's _detect_target falls back to on desktop."""
    return {"Darwin": "desktop-macos", "Linux": "desktop-linux", "Windows": "desktop-windows"}.get(
        platform.system(), "desktop-unknown"
    )


# What mm_scenarios returns for a scenario that did not run: test/scenario_runner.cpp's kSkipped.
# A skip is neither a pass nor a failure, and counting it as either is how a suite that stopped
# testing reads as green.
SKIPPED = 2


def _run_one(path: Path, update_contract: bool, update_reason: str | None,
             no_write: bool = False) -> int:
    """Run one scenario. Always parses MEASURE lines and writes
    observed.<target> blocks back into the scenario JSON (every run produces a
    drift record). With --update-contract, also rewrites the contract.

    Symmetric with the live runner's behaviour — observations persist always,
    contracts only when renegotiated."""
    # `skip_on` is the runner's rule, not this wrapper's: mm_scenarios reads the same field and
    # returns SKIPPED for it. A copy here returned 0 instead, so a skipped scenario was counted as a
    # pass and the binary's own skip path was never reached.
    # Capture + tee: stream to stdout while collecting MEASURE lines.
    # Pin the runner's filesystem root into the build tree. The runner performs real writes, and
    # its default root is the OS per-user data directory unless the working directory happens to be
    # a checkout. Relying on cwd for that would put a test one wrong directory away from
    # overwriting a developer's own installed-MoonLight settings.
    env = {**os.environ, "MM_DATA_DIR": str(ROOT / "build" / "scenario-fs")}
    proc = subprocess.Popen([str(RUNNER), str(path)], cwd=ROOT, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1)
    observations: dict[str, dict] = {}  # step-name → {tick_us, free_heap, max_alloc_block}
    for line in proc.stdout:
        sys.stdout.write(line)
        m = _MEASURE_RE.match(line)
        if m:
            # heap is absolute free-heap after measurement (the value the
            # observed/contract blocks consume directly). On desktop
            # freeHeap() returns 0 (unlimited) which the runner treats as
            # "no heap assertion". max_alloc_block is also absolute.
            observations[m["name"]] = {
                "tick_us": int(m["tick"]),
                "free_heap": int(m["heap"]),
                "max_alloc_block": int(m["block"]),
            }
    proc.wait()
    if proc.returncode != 0:
        # Scenario failed or skipped — don't persist observations either way: a failing run would
        # record garbage as the latest reading, and a skipped one produced no measurement at all.
        return proc.returncode

    if not observations:
        return 0

    # Explicit utf-8: scenario descriptions contain non-ASCII (→, ×, µ). On
    # Windows the default encoding is cp1252 and would mojibake those.
    with open(path, encoding="utf-8") as f:
        scenario = json.load(f)
    target = _host_target()
    today = datetime.date.today().isoformat()
    touched_observed = 0
    touched_contract = 0
    for step in scenario.get("steps", []):
        name = step.get("name")
        if name not in observations:
            continue
        # observed.<target> keeps a rolling window of samples per scalar and reports
        # p50/p95/min/max/n over it: the median is what the step normally costs and p95
        # is its tail, neither of which a single contended run can move far. When
        # --update-contract was passed, reset to the current single point (the window
        # described the PREVIOUS contract). See moondeck/scenario/_observed.py.
        existing_obs = step.get("observed", {}).get(target)
        if update_contract:
            new_obs = _observed.reset(observations[name], today)
            obs_changed = True
        else:
            new_obs, obs_changed = _observed.widen(existing_obs, observations[name], today)
        if obs_changed:
            step.setdefault("observed", {})[target] = new_obs
            touched_observed += 1

        # Only renegotiate the contract when --update-contract was passed. The
        # max_alloc_block field is *not* added unconditionally — it's an opt-in
        # floor that only a few scenarios assert (where LUT-fit is part of the
        # workload). When the existing contract already opts in, refresh the
        # value from the current run (consistent with tick_us / free_heap);
        # tolerances are user-set knobs and stay as-is.
        if update_contract:
            existing = step.get("contract", {}).get(target, {})
            new_block = {
                "tick_us": observations[name]["tick_us"],
                "free_heap": observations[name]["free_heap"],
                "set_by": today,
                "reason": update_reason or existing.get("reason", "updated"),
            }
            for k in ("tick_tolerance_pct", "heap_tolerance_pct", "tolerance_us"):
                if k in existing:
                    new_block[k] = existing[k]
            # max_alloc_block: opt-in (only carry it over if the existing
            # contract had it), but refresh the value rather than copying
            # the stale one. observations[name] always carries this key
            # because the scenario runner emits it; if it ever doesn't,
            # fall back to the existing value so we don't drop the opt-in.
            if "max_alloc_block" in existing:
                new_block["max_alloc_block"] = observations[name].get(
                    "max_alloc_block", existing["max_alloc_block"])
            step.setdefault("contract", {})[target] = new_block
            touched_contract += 1

    # --no-write: report the drift, change nothing. A gate must leave the tree exactly as it
    # found it, or the run invalidates its own result.
    if no_write:
        if touched_observed or touched_contract:
            print(f"  (drift in {path.name}: observed[{target}] x {touched_observed}"
                  f"{f', contract x {touched_contract}' if touched_contract else ''};"
                  f" run without --no-write to record it)")
        return 0

    if touched_observed or touched_contract:
        _observed.save_scenario(path, scenario)
        what = []
        if touched_observed:
            what.append(f"observed[{target}] × {touched_observed}")
        if touched_contract:
            what.append(f"contract[{target}] × {touched_contract}")
        print(f"  WROTE  {path.name} ({', '.join(what)})")
    return 0


def _run_many(paths, args) -> int:
    """Run every scenario and print the same pass/fail/skip summary mm_scenarios prints."""
    codes = [_run_one(p, args.update_contract, args.reason, args.no_write) for p in paths]
    skipped = sum(1 for c in codes if c == SKIPPED)
    failed = sum(1 for c in codes if c not in (0, SKIPPED))
    print(f"=== {len(codes)} scenario(s), {len(codes) - failed - skipped} passed, "
          f"{failed} failed, {skipped} skipped ===")
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", default=None,
                        help="Scenario name (file stem). Runs all if omitted.")
    parser.add_argument("--module", default=None,
                        help="Module filter. Runs only scenarios that match.")
    parser.add_argument("--no-write", action="store_true",
                        help="run and report, but do not write observations back into the "
                             "scenario JSON. What the GATES use: a gate that writes dirties the "
                             "tree it just checked, which makes it look like it needs running "
                             "again, and puts an observation diff in every commit. Run without "
                             "the flag to refresh the recorded numbers.")
    parser.add_argument("--update-contract", action="store_true",
                        help=("Renegotiate the per-step performance contract: write "
                              "observed tick/heap into contract[<host-target>] and "
                              "stamp set_by + reason. Requires --reason."))
    parser.add_argument("--reason", default=None,
                        help=("Why the contract is being renegotiated (required with "
                              "--update-contract). Examples: 'tighter Layer LUT copy', "
                              "'accepted DMX driver overhead'."))
    args = parser.parse_args()

    if args.update_contract and not args.reason:
        parser.error("--update-contract requires --reason "
                     "(e.g. --reason 'tightened after Layer optimization')")

    # Missing OR stale: both mean the results would not describe the code on disk.
    _require_fresh_runner()

    module_filter = args.module if (args.module and args.module.lower() != "all") else None

    # Resolve the scenario set.
    if args.name:
        scenario_file = test_meta.find_scenario_path(args.name)
        if not scenario_file:
            print(f"Scenario not found: {args.name}.json under {test_meta.SCENARIO_DIR}")
            sys.exit(1)
        if module_filter and scenario_file not in test_meta.paths_for_module(module_filter):
            print(f"Scenario {args.name} does not match module {module_filter}.")
            sys.exit(1)
        sys.exit(_run_one(scenario_file, args.update_contract, args.reason, args.no_write))

    if module_filter:
        paths = test_meta.paths_for_module(module_filter)
        if not paths:
            print(f"No scenarios found for module: {module_filter}")
            sys.exit(1)
        print(f"Module filter: {module_filter} ({len(paths)} scenario(s))")
        sys.exit(_run_many(paths, args))

    # Run all scenarios. We iterate per-file (instead of letting the C++ runner
    # auto-discover) because _run_one captures MEASURE lines and writes
    # observed.<target> blocks back into each scenario JSON on every run.
    paths = sorted((ROOT / "test" / "scenarios").rglob("scenario_*.json"))
    sys.exit(_run_many(paths, args))


if __name__ == "__main__":
    main()
