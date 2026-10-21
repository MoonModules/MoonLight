#!/usr/bin/env python3
"""Test results and check findings on a GitHub pull request, through workflow commands: an `::error` annotation on each failure and a table in the job summary.

Workflow commands are what a job prints, so they need no token: a pull request from a fork, whose token is read-only, gets the same annotations as ours. Each failing test is annotated where its failure points; a check's error-level findings are annotated in the files the pull request changes, since those are the ones it answers for.

Run:
  uv run moondeck/ci/github_annotations.py --junit results/*.xml
  uv run moondeck/ci/github_annotations.py --findings --changed changed.txt
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))
import _findings  # noqa: E402

LOCATION = re.compile(r"([\w./-]+\.(?:cpp|h|inc|py|mjs|js)):(\d+)")


def _escape(text: str) -> str:
    """A workflow command's message, with the characters its syntax reserves encoded."""
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def _prop(text: str) -> str:
    """A workflow command's property value, which also reserves the comma and the colon that separate the properties."""
    return _escape(text).replace(",", "%2C").replace(":", "%3A")


def _rel(path: str) -> str:
    return os.path.relpath(path, ROOT) if os.path.isabs(path) else path


def junit_failures(path: Path) -> tuple[list, dict]:
    """A JUnit file's failing cases, each as (file, line, test, message), and its counts."""
    counts = {"passed": 0, "failed": 0, "skipped": 0}
    failures = []
    for case in ET.parse(path).getroot().iter("testcase"):
        bad = case.find("failure") if case.find("failure") is not None else case.find("error")
        if case.find("skipped") is not None:
            counts["skipped"] += 1
            continue
        if bad is None:
            counts["passed"] += 1
            continue
        counts["failed"] += 1
        text = " ".join(filter(None, [bad.get("message"), bad.text]))
        where = LOCATION.search(text)
        file = _rel(case.get("file") or (where.group(1) if where else case.get("classname", "")))
        line = case.get("line") or (where.group(2) if where else "")
        failures.append((file, line, case.get("name", ""), text.strip()[:2000]))
    return failures, counts


def annotation(file: str, line, title: str, message: str) -> str:
    where = ",".join(filter(None, [f"file={_prop(file)}" if file else "", f"line={line}" if line else "", f"title={_prop(title)}"]))
    return f"::error {where}::{_escape(message)}"


def junit(paths: list, out=sys.stdout) -> list:
    """Annotate every failing test and return the summary rows, one per JUnit file."""
    rows = []
    for path in paths:
        failures, counts = junit_failures(Path(path))
        for file, line, test, message in failures:
            print(annotation(file, line, test, message), file=out)
        rows.append((Path(path).name, counts))
    return rows


def findings(changed: set, out=sys.stdout, directory: Path | None = None) -> list:
    """Annotate each check's error-level findings in the changed files and return a summary row per check."""
    rows = []
    for check, f in sorted(_findings.read_all(directory, metrics=directory).items()):
        if f.get("committed"):
            continue   # a check that did not run here has counts, no findings to annotate
        for entry in f["files"]:
            if entry["file"] in changed:
                for e in entry.get("errors", []):
                    print(annotation(entry["file"], e.get("line"), check, e["message"]), file=out)
        rows.append((check, {"errors": f["errors"], "warnings": f["warnings"]}))
    return rows


def summary(title: str, rows: list) -> str:
    """The rows as a Markdown table for the job summary."""
    if not rows:
        return ""
    keys = list(rows[0][1])
    lines = [f"### {title}", "", "| | " + " | ".join(keys) + " |", "|---|" + "---:|" * len(keys)]
    lines += [f"| {name} | " + " | ".join(str(c[k]) for k in keys) + " |" for name, c in rows]
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--junit", nargs="*", default=None, help="JUnit files whose failing tests to annotate")
    ap.add_argument("--findings", action="store_true", help="annotate the checks' error-level findings in the changed files")
    ap.add_argument("--changed", type=Path, help="a file listing the changed paths, one per line")
    args = ap.parse_args()
    text = ""
    if args.junit is not None:
        text += summary("Tests", junit([p for p in args.junit if Path(p).exists()]))
    if args.findings:
        changed = set(args.changed.read_text(encoding="utf-8").split()) if args.changed and args.changed.exists() else set()
        text += summary("Findings", findings(changed))
    if text and os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as fh:
            fh.write(text)
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
