"""The test runs a machine keeps: each test's last result, by the derived id the registry names it with.

A runner that writes JUnit XML is read into a run: the tree it saw, a status and the seconds. The records live under `build/moondeck-logs/` beside MoonDeck's other run records and are never committed, since a result belongs to the machine that ran it; CI publishes its own. The test page shows them where the file exists.
"""

from __future__ import annotations

import sys
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import walk_status  # noqa: E402

ROOT = walk_status.ROOT
RUNS = ROOT / "build" / "moondeck-logs" / "test-runs.json"


def split_doctest_output(text: str) -> tuple[str, str]:
    """doctest's `--out` takes every reporter's output into one file: the console text and the JUnit document, which this tells apart."""
    start, end = text.find("<testsuites"), text.rfind("</testsuites>")
    if start < 0 or end < 0:
        return text, ""
    xml = text[start:end + len("</testsuites>")]
    return (text[:start] + text[end + len("</testsuites>"):]).replace('<?xml version="1.0" encoding="UTF-8"?>\n', ""), xml


def follow_console(path: Path, stop, out) -> None:
    """Print the console text doctest writes into its out file as the file grows, until `stop` is set and the file is read to its end. The JUnit document, which comes last, is never printed."""
    pos, pending, seen_xml = 0, "", False
    while True:
        finished = stop.is_set()
        if path.exists():
            with path.open(encoding="utf-8", errors="replace") as f:
                f.seek(pos)
                chunk = f.read()
                pos = f.tell()
            text = (pending + chunk).replace('<?xml version="1.0" encoding="UTF-8"?>\n', "")
            pending = ""
            if not seen_xml:
                cut = text.find("<testsuites")
                if cut >= 0:
                    text, seen_xml = text[:cut], True
                elif "<" in text[-12:]:   # a marker may be split across two reads
                    cut = text.rindex("<")
                    text, pending = text[:cut], text[cut:]
                out.write(text)
                out.flush()
        if finished:
            return
        time.sleep(0.1)


def doctest_runs(xml: str, root: Path = ROOT) -> dict:
    """The runs in doctest's JUnit document, by id: `unit:<file>::<case>`, the file made relative to the repository so the id is the same on every machine."""
    runs = {}
    if not xml:
        return runs
    for case in ET.fromstring(xml).iter("testcase"):
        file = Path(case.get("classname", ""))
        try:
            file = file.relative_to(root)
        except ValueError:
            pass   # a path outside the repository keeps its own spelling
        status = "fail" if case.find("failure") is not None or case.find("error") is not None else "skip" if case.find("skipped") is not None else "pass"
        runs[f"unit:{file.as_posix()}::{case.get('name')}"] = {"status": status, "seconds": float(case.get("time") or 0)}
    return runs


def _status(case) -> str:
    return "fail" if case.find("failure") is not None or case.find("error") is not None else "skip" if case.find("skipped") is not None else "pass"


def pytest_runs(xml: Path) -> dict:
    """The runs in pytest's JUnit file, by id: `python:<file>::<test>`, the dotted classname turned back into the file's path."""
    runs = {}
    for case in ET.parse(xml).getroot().iter("testcase"):
        file = case.get("classname", "").replace(".", "/") + ".py"
        runs[f"python:{file}::{case.get('name')}"] = {"status": _status(case), "seconds": float(case.get("time") or 0)}
    return runs


def node_runs(xml: Path, titles: dict) -> dict:
    """The runs in node's JUnit file, which names a test by its title alone: `titles` maps each title to the ids that carry it (from the registry), so a run lands on its test, and on each of them when two files share a title."""
    runs = {}
    for case in ET.parse(xml).getroot().iter("testcase"):
        for test_id in titles.get(case.get("name"), []):
            runs[test_id] = {"status": _status(case), "seconds": float(case.get("time") or 0)}
    return runs


def scenario_runs(results: list) -> dict:
    """A scenario run's results, `(kind, name, status, seconds)` each, by id: `scenario:<name>` or, for one that only runs on a device, `live:<name>`."""
    return {f"{kind}:{name}": {"status": status, "seconds": round(seconds, 3)} for kind, name, status, seconds in results}


def check_runs(runs_file: Path | None = None, durations_file: Path | None = None) -> dict:
    """MoonDeck's record of each check script's last run, by id `check:<script>`: its pass, and the longest of its recent seconds, for a script run without arguments."""
    recorded = walk_status.read_runs(runs_file or walk_status.ROOT / "build" / "moondeck-logs" / "walk-runs.json")
    durations = walk_status.read_runs(durations_file or walk_status.ROOT / "build" / "moondeck-logs" / "durations.json")
    out = {}
    for key, r in recorded.items():
        if key.startswith("moondeck/check/check_") and " " not in key and "passed" in r:
            seconds = walk_status.longest(durations, key)
            out[f"check:{Path(key).stem}"] = {"status": "pass" if r["passed"] else "fail", "seconds": seconds or 0, "tree": r.get("tree")}
    return out


def record(runs: dict, path: Path = RUNS, tree: str | None = None) -> None:
    """Keep these runs with the tree they saw and when, replacing each test's last one."""
    tree = tree or walk_status.tree_now()
    at = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    walk_status._update(path, lambda data: data.update({i: {**r, "tree": tree, "at": at} for i, r in runs.items()}))


def published(path: Path = RUNS, tree: str | None = None, known: set | None = None, checks: bool = True) -> dict:
    """The runs as the page shows them: each with whether it ran on the working tree as it is now, so an older pass reads as older. A subcase's run (`case/sub`, as doctest names it) counts for the registered test it belongs to: it fails if any fails."""
    runs = {**walk_status.read_runs(path), **(check_runs() if checks else {})}
    if not runs:
        return {}
    tree = tree or walk_status.tree_now()
    out: dict = {}
    for i, r in sorted(runs.items()):
        if "status" not in r:
            continue
        if known is not None and i not in known:
            parent = next((k for k in known if i.startswith(k + "/")), None)
            if parent is None:
                continue
            i = parent
        entry = {"status": r["status"], "seconds": r.get("seconds"), "at": r.get("at"), "current": r.get("tree") == tree}
        if i in out:
            was = out[i]
            entry = {"status": "fail" if "fail" in (was["status"], r["status"]) else was["status"], "seconds": (was["seconds"] or 0) + (r.get("seconds") or 0),
                     "at": max(filter(None, [was["at"], entry["at"]]), default=None), "current": was["current"] and entry["current"]}
        out[i] = entry
    return out
