#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# ///
"""Mirror the GitHub Project's items to build/project.json, so an agent searches the board offline as it searches the docs.

The mirror is an artifact, not source: `build/` is gitignored, and the Project stays the one copy.
The Project is private to its owner's account, so the read uses that account's stored `gh` token when one exists, leaving the active account alone.
"""

import argparse
import datetime
import json
import os
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_OUT = ROOT / "build" / "project.json"
FIELDS = ("status", "area", "module", "kind", "trigger", "platform", "stage", "process")


def owner_env(owner: str) -> dict:
    """The environment for `gh`, carrying the owner's token when `gh` holds one for that account."""
    env = dict(os.environ)
    r = subprocess.run(["gh", "auth", "token", "--user", owner], capture_output=True, text=True)
    if r.returncode == 0 and r.stdout.strip():
        env["GH_TOKEN"] = r.stdout.strip()
    return env


def fetch(owner: str, number: int) -> dict:
    """The Project's items as `gh project item-list` reports them."""
    r = subprocess.run(
        ["gh", "project", "item-list", str(number), "--owner", owner, "--format", "json", "--limit", "5000"],
        cwd=ROOT, capture_output=True, text=True, env=owner_env(owner),
    )
    if r.returncode != 0:
        sys.exit(f"project_mirror: gh could not read project {number} of {owner}: {r.stderr.strip()}")
    return json.loads(r.stdout)


def normalize(raw: dict) -> list[dict]:
    """One flat record per item, its fields always present and empty when unset, ordered by area then title so a re-run on an unchanged board writes the same file."""
    items = []
    for it in raw.get("items", []):
        content = it.get("content") or {}
        rec = {
            "id": it.get("id", ""),
            "type": content.get("type", ""),
            "title": it.get("title") or content.get("title", ""),
            "number": content.get("number"),
            "url": content.get("url", ""),
        }
        for f in FIELDS:
            rec[f] = it.get(f) or ""
        rec["body"] = content.get("body", "")
        items.append(rec)
    return sorted(items, key=lambda r: (r["area"], r["title"].lower()))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--owner", default="MoonModules")
    ap.add_argument("--number", type=int, default=5)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    items = normalize(fetch(args.owner, args.number))
    doc = {
        "project": f"https://github.com/users/{args.owner}/projects/{args.number}",
        "generated": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "items": items,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n")

    by_status = Counter(r["status"] or "(no status)" for r in items)
    print(f"{len(items)} items to {args.out.relative_to(ROOT) if args.out.is_relative_to(ROOT) else args.out}")
    for status, n in sorted(by_status.items()):
        print(f"  {status}: {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
