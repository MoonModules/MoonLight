"""The findings every check reports, in one shape: the rule, where it fired and what it said.

A check keeps its own report and its own ratchet; this is the record each of them also writes, so the test page can show a
check with its rules and what it last found, whatever the check is. A rule carries its reason and the standards page that
holds it, in the shape of SARIF's `reportingDescriptor`; a finding is a SARIF `result`: a rule id, a level, a message, a
file and a line. The record lives under the build folder with the other run records and is never committed.
"""

from __future__ import annotations

import json
import os
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DIR = ROOT / "build" / "moondeck-logs" / "findings"


def is_real(report: Path) -> bool:
    """Whether `report` is the repository's own report, so a run that writes elsewhere (a test) keeps the real findings as they were."""
    return report.resolve().parent == (ROOT / "docs" / "reference" / "metrics").resolve()


def write(check: str, rules: dict, findings: list, directory: Path | None = None) -> Path:
    """Keep what `check` last found. `rules` maps a rule id to its `reason`, its `fix` and its `standard` (a page and an anchor); `findings` are dicts with `rule`, `message` and optionally `file`, `line`, `value` and `level`."""
    directory = directory or DIR
    counts = Counter(f["rule"] for f in findings)
    doc = {"check": check,
           "rules": {r: {**meta, "count": counts.get(r, 0)} for r, meta in rules.items()},
           "findings": [{"level": "warning", **f} for f in findings]}
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{check}.json"
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(doc, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    os.replace(tmp, path)
    return path


def read_all(directory: Path | None = None) -> dict:
    """Every check's last findings by check name, as a page shows them: its rules with counts and its worst files, not every finding."""
    out = {}
    for path in sorted((directory or DIR).glob("*.json")):
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        by_file = Counter(f.get("file") for f in doc.get("findings", []) if f.get("file"))
        levels = Counter(f.get("level") for f in doc.get("findings", []))
        out[doc["check"]] = {"rules": doc["rules"], "total": len(doc["findings"]), "errors": levels.get("error", 0), "warnings": levels.get("warning", 0), "files": [{"file": f, "count": n} for f, n in by_file.most_common(8)]}
    return out
