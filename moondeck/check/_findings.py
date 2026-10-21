"""The findings every check reports, in one shape: the rule, where it fired and what it said.

A check keeps its own ratchet; this is the record each of them writes, so the test page can show a check with its rules and
what it last found, whatever the check is: the page is the report. A rule carries its reason and the standards page that
holds it, in the shape of SARIF's `reportingDescriptor`; a finding is a SARIF `result`: a rule id, a level, a message, a
file and a line. The record lives under the build folder with the other run records and is never committed.
"""

from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _ratchet  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent
DIR = ROOT / "build" / "moondeck-logs" / "findings"


def is_real(report: Path) -> bool:
    """Whether `report` is the repository's own counts file, so a run that writes elsewhere (a test) keeps the real findings as they were."""
    return report.resolve().parent == (ROOT / "docs" / "reference" / "metrics").resolve()


def write(check: str, rules: dict, findings: list, directory: Path | None = None) -> Path:
    """Keep what `check` last found. `rules` maps a rule id to its `reason`, its `fix` and its `standard` (a page and an anchor); `findings` are dicts with `rule`, `message` and optionally `file`, `line`, `value` and `level`."""
    directory = directory or DIR
    counts = Counter(f["rule"] for f in findings)
    doc = {"check": check,
           "rules": {r: {**meta, "count": counts.get(r, 0)} for r, meta in rules.items()},
           "findings": [{"level": "warning", **f} for f in findings]}
    path = directory / f"{check}.json"
    _ratchet.write_json(path, doc)
    return path


def page_counts(directory: Path | None = None) -> dict:
    """The counts a ratcheted check keeps with the tree (docs/reference/metrics/<name>.json for check_<name>), by check name: what a page built where the check never ran shows, rules and totals without files."""
    out = {}
    for path in sorted((directory or ROOT / "docs" / "reference" / "metrics").glob("*.json")):
        check = f"check_{path.stem}"
        if not (ROOT / "moondeck" / "check" / f"{check}.py").exists():
            continue
        try:
            counts = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        out[check] = {"rules": {r: {"count": n} for r, n in counts.items() if not r.startswith("(")}, "total": counts.get("(total)", 0), "files": [], "committed": True}
    return out


def read_all(directory: Path | None = None, metrics: Path | None = None) -> dict:
    """Every check's last findings by check name, as a page shows them: its rules with counts, and every file with its count and its errors in full. A ratcheted check that never ran here shows the counts committed in `metrics`, the repository's own when not given."""
    out = page_counts(metrics)
    for path in sorted((directory or DIR).glob("*.json")):
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        found = doc.get("findings", [])
        by_file = Counter(f.get("file") for f in found if f.get("file"))
        errors: dict = {}
        for f in found:
            if f.get("level") == "error" and f.get("file"):
                errors.setdefault(f["file"], []).append({k: f[k] for k in ("line", "message") if k in f})
        levels = Counter(f.get("level") for f in found)
        out[doc["check"]] = {"rules": doc["rules"], "total": len(found), "errors": levels.get("error", 0), "warnings": levels.get("warning", 0),
                             "files": [{"file": f, "count": n, **({"errors": errors[f]} if f in errors else {})} for f, n in by_file.most_common()]}
    return out
