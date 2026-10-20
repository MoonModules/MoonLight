#!/usr/bin/env python3
"""Every check's last findings as SARIF 2.1.0, for GitHub code scanning: one file per check.

The findings record (moondeck/check/_findings.py) already has SARIF's shape, so this only spells it: a check is a run whose tool is the check, a rule a `reportingDescriptor` with its reason and its standards page, a finding a `result` with its rule, level, message and location. One file per check, each with its own `automationDetails.id`, because code scanning keys an upload by tool and category and refuses two runs that share both.

Run: uv run moondeck/ci/findings_sarif.py --out build/sarif
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))
import _findings  # noqa: E402

SCHEMA = "https://json.schemastore.org/sarif-2.1.0.json"
REPO = "https://github.com/MoonModules/MoonLight/blob/main/"


def to_sarif(doc: dict) -> dict:
    """One check's findings record as a SARIF log with one run."""
    rules = [{"id": rule, "shortDescription": {"text": meta.get("reason") or rule},
              **({"help": {"text": meta["fix"]}} if meta.get("fix") else {}),
              **({"helpUri": REPO + meta["standard"]} if meta.get("standard") else {})}
             for rule, meta in doc["rules"].items()]
    known = {r["id"] for r in rules}
    # A finding names a rule its check reports, or one the check learned from its tool (a clang-tidy check id): either way the run declares it.
    rules += [{"id": r, "shortDescription": {"text": r}} for r in sorted({f["rule"] for f in doc["findings"]} - known)]
    results = []
    for f in doc["findings"]:
        result = {"ruleId": f["rule"], "level": "error" if f.get("level") == "error" else "warning", "message": {"text": f.get("message") or f["rule"]}}
        if f.get("file"):
            location = {"artifactLocation": {"uri": f["file"]}}
            if f.get("line"):
                location["region"] = {"startLine": int(f["line"])}
            result["locations"] = [{"physicalLocation": location}]
        results.append(result)
    return {"$schema": SCHEMA, "version": "2.1.0",
            "runs": [{"tool": {"driver": {"name": doc["check"], "informationUri": REPO + f"moondeck/check/{doc['check']}.py", "rules": rules}},
                      "automationDetails": {"id": f"{doc['check']}/"}, "results": results}]}


def write(out: Path, directory: Path | None = None) -> list:
    """A SARIF file per check under `out`, from the findings `directory`; the files written."""
    out.mkdir(parents=True, exist_ok=True)
    written = []
    for path in sorted((directory or _findings.DIR).glob("*.json")):
        doc = json.loads(path.read_text(encoding="utf-8"))
        target = out / f"{doc['check']}.sarif"
        target.write_text(json.dumps(to_sarif(doc), ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        written.append(target)
    return written


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, required=True, help="the directory to write one .sarif per check into")
    args = ap.parse_args()
    written = write(args.out)
    print(f"findings_sarif: {len(written)} check(s) under {args.out}")
    return 0 if written else 1   # no findings record at all is a run that checked nothing, never a clean one


if __name__ == "__main__":
    sys.exit(main())
