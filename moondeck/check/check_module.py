#!/usr/bin/env python3
"""Every static-analysis tool, on ONE module — the repo-wide reports turned around.

The other check scripts each run one tool over the whole repo: good for a sweep, wrong shape
when you are working on a single file and want to know what the tools say about *it*. This runs
the whole set against one module and prints them under one heading, so "is my module clean" is
one click rather than three runs and three filters.

Adds no analysis of its own — it invokes the same scripts with `--module`, so this report and
the repo-wide ones can never disagree about a finding.

Tools, in order (sequential by design: each already parallelises across cores internally, so
running them concurrently would just make them contend):

    clang-tidy    bug patterns, performance, portability
    clang-query   our own AST rules — RAM-costing arrays, heap allocation sites
    code          complexity and function length (lizard)
    footprint     bytes on the device, split flash / static RAM (needs a built ESP32 ELF)

A module is resolved to `src/**/<Module>.h` and `.cpp` (see check_clang_query.module_files) —
the same names the MoonDeck dropdown offers.

Usage:
  uv run moondeck/check/check_module.py --module Control
  uv run moondeck/check/check_module.py --module Layer --skip clang-tidy
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
HERE = Path(__file__).resolve().parent

sys.path.insert(0, str(HERE))
import check_clang_query  # noqa: E402  — the module→files resolver, one owner

# check_code with `--module` prints that module's findings without touching the report, which is the repo-wide artifact.
#
# clang-query keeps its 60-row cap. It used to run `--max-rows=0` on the same
# reasoning, but the comments rule made a single module's report unreadable — HttpServerModule
# alone prints 212 declarations, and a wall of rows is skimmed rather than read. The cap is per
# TABLE and always announces what it dropped, so the tail is one `--max-rows=0` away.
#
# `optional` marks a tool whose inputs may legitimately be absent — footprint reads an ESP32 ELF,
# and a desktop-only checkout has none. That is a SKIP, not a failure: failing the whole card
# because the firmware was not built would train people to ignore its exit code.
TOOLS = [
    ("clang-tidy", ["check_clang_tidy.py"], False),
    ("clang-query", ["check_clang_query.py"], False),
    ("-Wfunction-effects", ["check_nonblocking.py"], False),
    ("code", ["check_code.py"], False),
    ("CodeQL", ["check_codeql.py"], True),
    ("footprint", ["check_footprint.py"], True),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--module", required=True, help="Module name, e.g. Control or ParallelLedDriver.")
    ap.add_argument("--skip", action="append", default=[],
                    help="Skip a tool by name (repeatable): clang-tidy, clang-query, code, footprint.")
    args = ap.parse_args()

    files = check_clang_query.module_files(args.module)
    if not files:
        print(f"No source files for module '{args.module}' — looked for "
              f"src/**/{args.module}.h and src/**/{args.module}.cpp.", file=sys.stderr)
        return 2

    print(f"All tools on module: {args.module}")
    print(f"Files: {', '.join(files)}")
    print()

    failed = []
    for name, argv, optional in TOOLS:
        if name in args.skip:
            print(f"--- {name}: skipped ---\n")
            continue
        print(f"{'=' * 70}\n=== {name}\n{'=' * 70}")
        started = time.time()
        proc = subprocess.run(
            ["uv", "run", str(HERE / argv[0]), *argv[1:], "--module", args.module],
            cwd=ROOT, capture_output=True, text=True)
        # Each tool already prints a readable report; pass it through rather than re-format,
        # so this stays an orchestrator and the formatting has one owner per tool.
        sys.stdout.write(proc.stdout)
        if proc.returncode not in (0, 1):      # 1 = findings, which is a result not a failure
            sys.stderr.write(proc.stderr)
            # An optional tool says WHY it could not run (no ELF built, no toolchain). Echo that
            # onto STDOUT too: the card streams stdout, so a stderr-only reason reads as a tool
            # that produced nothing at all — a silent skip, which is what this must never be.
            if optional:
                print(f"  (not run: {proc.stderr.strip().splitlines()[0] if proc.stderr.strip() else 'no reason given'})")
            else:
                failed.append(name)
        print(f"[{name}: {time.time() - started:.0f}s]\n")

    if failed:
        print(f"Tools that could not run: {', '.join(failed)}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
