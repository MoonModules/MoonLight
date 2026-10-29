#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml", "jsonschema"]
# ///
"""Run a MoonWalk process's script steps from the terminal, as MoonDeck runs them from its page: `run_process.py ci-on-pr` runs locally what CI runs.

A step that calls a subprocess stands for that subprocess's steps. By default only the steps the change triggers run, and a step whose last pass still holds is skipped; `--every` runs every step and `--rerun` ignores a holding pass. Each run is recorded as MoonDeck records it, so the map marks it.
"""

import argparse
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
import gen_moonwalk  # noqa: E402
import walk_status   # noqa: E402


def plan(steps: list[dict], rerun: bool) -> list[tuple[dict, str]]:
    """Each step with what happens to it: `run`, or a skip and its reason. A step's `triggered` already counts `--every`."""
    out = []
    for s in steps:
        if not s["triggered"]:
            out.append((s, "skip: the change does not touch its paths"))
        elif s["fresh"] and not rerun:
            out.append((s, "skip: its last pass still holds"))
        else:
            out.append((s, "run"))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("process", help="the process id, such as ci-on-pr or commit")
    ap.add_argument("--every", action="store_true", help="run every step, as a release does, not only those the change triggers")
    ap.add_argument("--rerun", action="store_true", help="run a step even when its last pass still holds")
    ap.add_argument("--base", help="judge the change against where the branch left this ref, as a merge does")
    args = ap.parse_args()

    model = gen_moonwalk.build()
    if args.process not in model["processes"]:
        sys.exit(f"run_process: no process {args.process!r}")
    steps = walk_status.status(model, walk_status.read_runs(walk_status.WALK_RUNS), walk_status.read_runs(walk_status.DURATIONS),
                               base=args.base, every=args.every).get(args.process, [])
    if not steps:
        sys.exit(f"run_process: {args.process} runs no script")

    failed = 0
    lines = []
    for s, what in plan(steps, args.rerun):
        label = f"{s['process']}/{s['id']}"
        if what != "run":
            lines.append(f"SKIP {label}: {what[6:]}")
            continue
        print(f"\n=== {label}: uv run {s['script']} {s['args']}".rstrip(), flush=True)
        started = time.monotonic()
        passed = subprocess.run(["uv", "run", s["script"], *shlex.split(s["args"])], cwd=ROOT).returncode == 0
        key = walk_status.run_key(s["script"], s["args"])
        walk_status.record_finished(walk_status.WALK_RUNS, key, passed)
        if passed:
            walk_status.record_duration(walk_status.DURATIONS, key, time.monotonic() - started)
        failed += not passed
        lines.append(f"{'PASS' if passed else 'FAIL'} {label}")
    print("\n" + "\n".join(lines))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
