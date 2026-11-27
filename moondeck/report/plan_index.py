#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# ///
"""Write docs/work/index.md: every plan with its steps counted by their markers, so a plan's name stays its date and topic.

A plan's progress lives in its step markers, 🚧 in progress, 🧪 built with its test open, ✅ done and ⏳ not started; a dropped step
(❌, ⏸) leaves the total. The index is generated from them, as the backlog mirror is from the Project's cards, so the counts cannot
drift from the steps. `--check` renders without writing and fails when the index differs from what the plans render.

A plan's steps are the first structure in its steps section that carries a marker: step headings, numbered items, phase lines
(`✅ **Phase 1: ...**`), or the rows of a step table. Only that structure counts, so the numbered checks under a phase or a heading
stay out of the total.
"""
import argparse
import re
import sys
from pathlib import Path
from urllib.parse import quote

ROOT = Path(__file__).resolve().parent.parent.parent
WORK = ROOT / "docs" / "work"
INDEX = WORK / "index.md"

MARKERS = ("🚧", "🧪", "✅", "⏳")
DROPPED = ("❌", "⏸")
SECTION = re.compile(r"^## (Steps|The steps|Phases)\b")
HEADING = re.compile(r"^(#{2,4}) (?:Step |Phase )?\d+[a-z]?[.:]\s")
FENCE = re.compile(r"^\s*(```|~~~)")
NUMBERED = re.compile(r"^\d+\. ")
PHASE = re.compile(r"^(?:✅|🚧|🧪|⏳|❌|⏸) \*\*(?:Phase|Step) ")
TABLE_ROW = re.compile(r"^\| *\d+[a-z]? *\|")


def marker_of(text: str) -> str:
    """The step's status: its first marker, a dropped mark, or not started when it carries none."""
    hit = re.search("|".join(MARKERS + DROPPED), text)
    return hit.group(0) if hit else "⏳"


def outside_fences(lines: list[str]) -> list[str]:
    """The lines outside fenced code blocks, so an example inside one counts as no step."""
    kept, fence = [], None
    for line in lines:
        m = FENCE.match(line)
        if m and (fence is None or m.group(1) == fence):
            fence = None if fence else m.group(1)
            continue
        if fence is None:
            kept.append(line)
    return kept


def steps_section(lines: list[str]) -> list[str]:
    """The lines from the steps heading to the next level-2 heading, or the whole plan when it has no such heading, outside fenced code."""
    lines = outside_fences(lines)
    start = next((i for i, l in enumerate(lines) if SECTION.match(l)), None)
    if start is None:
        return lines
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    return lines[start + 1:end]


def kind_of(line: str) -> str | None:
    """Which step structure a line is, if any."""
    if PHASE.match(line):
        return "phase"
    if m := HEADING.match(line):
        return f"heading{len(m.group(1))}"   # by depth, so a nested heading under a step is no peer of it
    if TABLE_ROW.match(line):
        return "table"
    if NUMBERED.match(line):
        return "numbered"
    return None


def count_steps(text: str) -> dict[str, int]:
    """In progress, testing, done and not started, over the plan's top-level steps."""
    lines = steps_section(text.split("\n"))
    candidates = [(kind_of(l), l) for l in lines if kind_of(l)]
    marked = next((k for k, l in candidates if re.search("|".join(MARKERS + DROPPED), l) or k == "table"), None)
    counts = dict.fromkeys(MARKERS, 0)
    for kind, line in candidates:
        if kind != marked:
            continue
        status = marker_of(line.split("|", 3)[3] if kind == "table" and line.count("|") >= 4 else line)
        if status in counts:
            counts[status] += 1
    return counts


def render() -> str:
    rows = []
    for plan in sorted(WORK.glob("Plan-*.md")):
        c = count_steps(plan.read_text(encoding="utf-8"))
        total = sum(c.values())
        title = plan.stem.split(" - ", 1)[1] if " - " in plan.stem else plan.stem
        link = quote(plan.name, safe="")
        rows.append(f"| [{title}]({link}) | {plan.stem[5:13]} | {c['🚧']} | {c['🧪']} | {c['✅']} | {c['⏳']} | {total} |")
    head = ("# Plans\n\n"
            "The plans being built, each counted from its own step markers by `moondeck/report/plan_index.py`: 🚧 in progress, "
            "🧪 built with its test open, ✅ done, ⏳ not started. A dropped step (❌, ⏸) leaves the total. The script writes "
            "this page; rerun it when a plan's steps change.\n\n"
            "| Plan | Started | 🚧 | 🧪 | ✅ | ⏳ | Steps |\n|--|--|--|--|--|--|--|\n")
    return head + "\n".join(rows) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="fail when docs/work/index.md differs from what the plans render")
    args = ap.parse_args()
    page = render()
    if args.check:
        current = INDEX.read_text(encoding="utf-8") if INDEX.exists() else ""
        if current != page:
            print("FAIL: docs/work/index.md differs from the plans' step markers; run moondeck/report/plan_index.py")
            return 1
        print(f"PASS: docs/work/index.md matches {page.count(chr(10)) - 6} plans")
        return 0
    INDEX.write_text(page, encoding="utf-8", newline="\n")
    print(f"wrote {INDEX.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
