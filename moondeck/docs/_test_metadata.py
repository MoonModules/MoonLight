"""Shared parsers for unit-test and scenario metadata.

Both `moondeck/docs/generate_test_docs.py` (writes markdown to disk) and
`moondeck/moondeck.py` (serves HTML views in the MoonDeck UI) read the same
`@module` / `@also` / per-TEST_CASE descriptions from unit tests and the same
`module` / `also` / per-step `description` JSON fields from scenarios. This
module owns that parsing so adding a new field — `@since`, a new step key —
takes one edit, not two.

Renderers live in the consumers; this module only extracts the structured data.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UNIT_DIR = ROOT / "test" / "unit"
SCENARIO_DIR = ROOT / "test" / "scenarios"

# Match `TEST_CASE("name")` or `TEST_CASE("name" * doctest::skip())`. Captures the name.
TEST_CASE_RE = re.compile(r'^TEST_CASE\s*\(\s*"([^"]+)"', re.MULTILINE)


def parse_unit_file(path: Path) -> dict:
    """Return {path, module, also, file_description, cases: [(name, description)]}.

    A case `description` is the single `//` comment line immediately above the
    TEST_CASE. Missing → None; callers decide how to fall back (the doc
    generator uses the TEST_CASE name itself, MoonDeck italicises it).
    """
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()

    module: str | None = None
    also: list[str] = []
    file_description_lines: list[str] = []
    for line in lines:
        s = line.strip()
        if not s.startswith("//"):
            # Header annotations only live in the leading `//` block.
            if s.startswith("#") or s.startswith("namespace") or s.startswith("TEST_"):
                break
            continue
        # `//` or `///`: a file lead is spelled with three slashes, and stripping exactly two
        # left `/ @module` behind, which matched nothing and put every file in "Uncategorized".
        body = s.lstrip("/").strip()
        if body.startswith("@module "):
            module = body[len("@module "):].strip()
        elif body.startswith("@also "):
            also = [m.strip() for m in body[len("@also "):].split(",") if m.strip()]
        elif body.startswith("@description "):
            file_description_lines.append(body[len("@description "):].strip())

    cases: list[tuple[str, str | None]] = []
    for m in TEST_CASE_RE.finditer(text):
        case_name = m.group(1)
        line_idx = text[:m.start()].count("\n")  # 0-based index of the TEST_CASE line
        # Walk back over consecutive `//` (non-`@`) lines so a multi-line
        # comment block above the TEST_CASE renders as one full description.
        # Previously only the last line was captured, truncating leading clauses.
        desc_lines: list[str] = []
        i = line_idx - 1
        while i >= 0:
            prev = lines[i].strip()
            if not prev.startswith("//") or prev.startswith("// @"):
                break
            desc_lines.append(prev[2:].strip())
            i -= 1
        desc_lines.reverse()
        desc: str | None = " ".join(desc_lines) if desc_lines else None
        cases.append((case_name, desc))

    return {
        "path": path,
        "module": module,
        "also": also,
        "file_description": " ".join(file_description_lines).strip() or None,
        "cases": cases,
    }


def parse_scenario_file(path: Path) -> dict:
    """Return top-level metadata + step list. Each step is a dict (not a tuple)
    carrying every field the generator surfaces: name, description, op, bounds,
    contract per target, observed per target. Older callers that only need the
    path or top-level keys are unaffected (new keys are additive)."""
    data = json.loads(path.read_text(encoding="utf-8"))
    steps = []
    for step in data.get("steps", []) or []:
        steps.append({
            "name": step.get("name", "?"),
            "description": step.get("description"),
            "op": step.get("op", ""),
            "measure": bool(step.get("measure")),
            "bounds": step.get("bounds") or {},
            "contract": step.get("contract") or {},
            "observed": step.get("observed") or {},
        })
    return {
        "path": path,
        "module": data.get("module"),
        "also": data.get("also", []) or [],
        "name": data.get("name", path.stem),
        "description": data.get("description", ""),
        "mode": data.get("mode", "construct"),
        "live_only": bool(data.get("live_only")),
        "steps": steps,
    }


def collect_unit_files() -> list[dict]:
    """All test/unit/**/unit_*.cpp, parsed and sorted by path."""
    return [parse_unit_file(p) for p in sorted(UNIT_DIR.rglob("unit_*.cpp"))]


def _nav_order() -> list[str]:
    """The nav order the device UI draws, read from the one place that defines it.

    A scenario suite that ran alphabetically told its story out of order, so the run follows the
    interface instead. The order lives in app.js NAV_GROUPS, and is read rather than restated:
    a second copy is a list that drifts the first time someone reorders the nav and looks here.
    A card app.js does not name sorts after these, so adding one is visible rather than silently last.
    """
    try:
        block = (ROOT / "src" / "ui" / "app.js").read_text().split("const NAV_GROUPS = [", 1)[1]
    except (OSError, IndexError):
        return []           # no nav to read: fall back to the alphabetical order below
    return re.findall(r'"([^"]+)"', block.split("];", 1)[0])


NAV_ORDER = _nav_order()


def _nav_rank(scenario: dict) -> tuple[int, str]:
    """Where a scenario sits in the nav, by the card its module belongs to."""
    name = scenario["path"].name
    for i, card in enumerate(NAV_ORDER):
        # The file is named for its card: scenario_<Card>_<what it proves>.json, where a card
        # with a space is written without one.
        if name.lower().startswith(f"scenario_{card.replace(' ', '').lower()}"):
            return (i, name)
    return (len(NAV_ORDER), name)


def collect_scenario_files() -> list[dict]:
    """All test/scenarios/**/scenario_*.json, parsed and ordered the way the nav lists the cards."""
    found = [parse_scenario_file(p) for p in sorted(SCENARIO_DIR.rglob("scenario_*.json"))]
    return sorted(found, key=_nav_rank)


def list_test_modules() -> list[str]:
    """Union of every module name a unit test or scenario primarily exercises
    (@module / module) OR mentions as a peer (@also / also). MoonDeck's module
    dropdown filters on this list — a module that only appears under `also`
    still needs to be selectable so the user can find every test that touches it.
    """
    modules: set[str] = set()
    for f in collect_unit_files():
        if f["module"]:
            modules.add(f["module"])
        modules.update(f.get("also") or [])
    for s in collect_scenario_files():
        if s["module"]:
            modules.add(s["module"])
        modules.update(s.get("also") or [])
    return sorted(modules)


def find_scenario_path(stem: str) -> Path | None:
    """Locate a scenario JSON anywhere under test/scenarios/ by file stem."""
    return next(SCENARIO_DIR.rglob(f"{stem}.json"), None)


def paths_for_module(module: str) -> list[Path]:
    """Scenario JSON paths whose @module or @also matches, sorted."""
    return sorted(
        s["path"] for s in collect_scenario_files()
        if s["module"] == module or module in s["also"]
    )


def cases_for_module(module: str) -> list[dict]:
    """Per-TEST_CASE rows for every unit file whose @module or @also matches.

    Each row: {file (str, ROOT-relative), name, desc, primary (bool)}.
    """
    rows: list[dict] = []
    for f in collect_unit_files():
        if f["module"] != module and module not in f["also"]:
            continue
        for name, desc in f["cases"]:
            rows.append({
                "file": f["path"].relative_to(ROOT).as_posix(),
                "name": name,
                "desc": desc,
                "primary": f["module"] == module,
            })
    return rows
