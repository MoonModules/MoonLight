#!/usr/bin/env python3
"""Run the host test suites: the Python ones, the JS ones, or both.

These are the tests that live outside the C++ binary, and they cover what it cannot reach: the
cross-language contracts (the Improv frame's wire format, WLED's /json shape), the MoonDeck scripts
themselves, the browser code in src/ui, and the claim that every shipped MoonLive script is valid
C++. The commit gate and CI both run them; this is the same command with a card in front of it.

  uv run moondeck/test/test_host.py            # both suites
  uv run moondeck/test/test_host.py --python   # just Python
  uv run moondeck/test/test_host.py --js       # just JS
  uv run moondeck/test/test_host.py --ui       # just the UI run (needs a running device)
  uv run moondeck/test/test_host.py --python --junit-dir results   # as CI runs it, the results where its annotations read them

The Python deps ride in a PEP-723 block per test file, so they are named here rather than
installed: `uv run --with` resolves them per run and leaves no environment behind.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
import test_runs  # noqa: E402

# Named here rather than in a requirements file: each is a test's own dependency (pyserial for the flash tests, wled for the /json shim, markdown for the docs checks, requests for the MoonTube recorder, libclang for the help generator, pyyaml and jsonschema for MoonWalk's generator, jsonschema for the device catalog too), and `uv run --with` is how every other script in MoonDeck reaches one.
PY_DEPS = ("pytest", "pyserial", "markdown", "wled", "requests", "libclang", "pyyaml", "jsonschema")

# The UI suite's own deps. Separate because it is the only lane that drives a browser
# against a RUNNING device: pytest-playwright is Playwright's documented Python runner
# (per-test browser isolation, auto-retrying expect), and it is dead weight on a bench
# that only runs the pure suites.
UI_DEPS = ("pytest", "pytest-playwright", "requests")


def run(cmd, label):
    print(f"\n=== {label} ===", flush=True)
    r = subprocess.run(cmd, cwd=ROOT, check=False)   # the caller collects the code; a raise would skip the other suite
    return r.returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    # Mutually exclusive: each flag means "ONLY this suite", so passing both asks for two
    # contradictory things. Neither flag still means both suites, which is the common case.
    only = ap.add_mutually_exclusive_group()
    only.add_argument("--python", action="store_true", help="run only the Python suite")
    only.add_argument("--js", action="store_true", help="run only the JS suite")
    only.add_argument("--ui", action="store_true", help="run only the UI suite (needs a device)")
    ap.add_argument("--junit-dir", type=Path, default=ROOT / "build" / "moondeck-logs",
                    help="where the JUnit results go; CI points it where its annotations step reads them")
    ap.add_argument("--clip", help="with --ui, perform only the run files whose name holds this, such as 16-restore-a-backup")
    args = ap.parse_args()
    args.junit_dir = args.junit_dir.resolve()   # the caller's folder, since the runs below start in the repo
    if args.clip and not args.ui:
        ap.error("--clip narrows the UI suite: pass --ui with it")
    # The UI lane is OPT-IN: it needs a MoonLight listening, so a bare run stays the
    # pure suites that work on any machine. It skips rather than fails when asked for
    # without a device, which is what the tests themselves decide.
    both = not (args.python or args.js or args.ui)

    rc = 0
    if both or args.python:
        cmd = ["uv", "run"]
        for d in PY_DEPS:
            cmd += ["--with", d]
        junit = args.junit_dir / "junit-python.xml"   # the results beside the console, to be kept as runs
        junit.parent.mkdir(parents=True, exist_ok=True)
        junit.unlink(missing_ok=True)   # a run that writes none records none, rather than the last one's results as this tree's
        cmd += ["pytest", "test/python", "-q", f"--junitxml={junit}"]
        rc |= run(cmd, "Python (test/python)")
        if junit.exists():
            test_runs.record(test_runs.pytest_runs(junit))

    if both or args.js:
        # node, not uv: the JS suite is the browser code's own runner. Reported rather than failed
        # when node is absent, because a Python-only bench is a normal setup and a missing runtime
        # is "this does not apply here", which is what SKIP means.
        if shutil.which("node") is None:
            print("\n=== JS (test/js) ===\nSKIP: node is not on PATH", flush=True)
        else:
            # Expanded HERE, not by node and not by a shell. The call passes a list with no
            # shell=True, so a literal `test/js/**/*.test.mjs` would reach node and depend on its
            # own glob support, which older runtimes lack. Resolving the paths in Python makes the
            # command work on any node, and names the files it ran.
            tests = sorted(str(f.relative_to(ROOT)) for f in (ROOT / "test/js").rglob("*.test.mjs"))
            if not tests:
                print("\n=== JS (test/js) ===\nSKIP: no test files found", flush=True)
            else:
                junit = args.junit_dir / "junit-js.xml"
                junit.parent.mkdir(parents=True, exist_ok=True)
                junit.unlink(missing_ok=True)
                console = "spec" if sys.stdout.isatty() else "tap"   # what node picks on its own, named now that a second reporter writes the results
                rc |= run(["node", "--test", f"--test-reporter={console}", "--test-reporter-destination=stdout",
                           "--test-reporter=junit", f"--test-reporter-destination={junit}", *tests], "JS (test/js)")
                if junit.exists():
                    import gen_tests
                    test_runs.record(test_runs.node_runs(junit, gen_tests.js_titles()))

    if args.ui:
        cmd = ["uv", "run"]
        for d in UI_DEPS:
            cmd += ["--with", d]
        cmd += ["pytest", "moontube", "-q"] + (["-k", args.clip] if args.clip else [])
        rc |= run(cmd, "UI (moontube)")

    print("\nDONE" if rc == 0 else "\nFAILED", flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
