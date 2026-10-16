#!/usr/bin/env python3
"""Generate docs/reference/tests/unit-tests.md and docs/reference/tests/scenario-tests.md from the source of truth.

Unit tests: walks test/unit/ recursively for unit_*.cpp, extracts `// @module <Name>`, optional
`// @also A, B`, and a single `//` description line above each `TEST_CASE("...")`.

Scenarios: walks test/scenarios/scenario_*.json, reads top-level `module`,
`also`, `name`, `description`, and per-step `description`.

Both outputs are grouped by primary `@module` / `module`. The script is the
single owner of the generated files, running it idempotently produces the
same bytes (verified by --check).

Usage:
    uv run moondeck/docs/generate_test_docs.py             # writes both files
    uv run moondeck/docs/generate_test_docs.py --unit      # unit-tests.md only
    uv run moondeck/docs/generate_test_docs.py --scenario  # scenario-tests.md only
    uv run moondeck/docs/generate_test_docs.py --check     # exit 1 if regen would change files
"""

import argparse
import sys
from collections import defaultdict
from pathlib import Path

from _test_metadata import (
    ROOT,
    collect_scenario_files,
    collect_unit_files,
)

OUT_DIR = ROOT / "docs" / "reference" / "tests"
UNIT_OUT = OUT_DIR / "unit-tests.md"
SCENARIO_OUT = OUT_DIR / "scenario-tests.md"


def render_unit_tests(files: list[dict]) -> str:
    by_module: dict[str, list[dict]] = defaultdict(list)
    for f in files:
        by_module[f["module"] or "Uncategorized"].append(f)
    for mods in by_module.values():
        mods.sort(key=lambda f: f["path"].name)

    lines: list[str] = []
    lines.append("# Unit Tests")
    lines.append("")
    lines.append(
        "Auto-generated from `test/unit/{core,light}/unit_*.cpp` by `moondeck/docs/generate_test_docs.py`. "
        "**Do not edit by hand**: update the source file's `@module` / `@also` "
        "and per-TEST_CASE `//` descriptions instead, then regenerate."
    )
    lines.append("")
    lines.append(
        "Unit tests are the fastest tier in the [test strategy](../testing.md): "
        "they run the production code in-process with doctest, no platform, no network. "
        "Each section below covers one module."
    )
    lines.append("")

    for module in sorted(by_module.keys()):
        lines.append(f"## {module}")
        lines.append("")
        for f in by_module[module]:
            rel = f["path"].relative_to(ROOT).as_posix()
            if f["file_description"]:
                lines.append(f"`{rel}`: {f['file_description']}")
            else:
                lines.append(f"`{rel}`")
            if f["also"]:
                lines.append(f"*Also touches: {', '.join(f['also'])}.*")
            lines.append("")
            for name, desc in f["cases"]:
                bullet = desc if desc else f"_{name}_"
                lines.append(f"- {bullet}")
            lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def _fmt_us(us: int) -> str:
    """Pretty-print microseconds with a friendly unit (us / ms / s)."""
    if us >= 1_000_000:
        return f"{us / 1_000_000:.2f}s"
    if us >= 1_000:
        return f"{us / 1_000:.1f}ms"
    return f"{us}µs"


# What a cell shows when there is nothing to show. An em-dash is the typographic
# convention for "no value" in a table, and the coding standards exempt a literal one in
# a UI string from the no-em-dashes prose rule. Named once so the six cell renderers agree
# and so the character appears in exactly one place.
NO_VALUE = "\u2014"


def _fps_from_us(us: int) -> str:
    """Convert tick_us → frames-per-second string (no decimals for ≥100 FPS,
    one decimal below; for headline display). Shared core of both
    `_fps_floor_from_contract` (single scalar → '≥ N FPS') and
    `_fps_range_from_observed_range` ([min, max] tick → 'lo-hi FPS')."""
    if us <= 0:
        return NO_VALUE
    fps = 1_000_000 / us
    if fps >= 100:
        return f"{int(round(fps)):,}"
    return f"{fps:.1f}"


def _fmt_heap(bytes_: int) -> str:
    """Pretty-print free-heap bytes (KB once over 1024). 0 = unlimited (desktop)."""
    if bytes_ == 0:
        return "unlimited"
    if bytes_ >= 1024:
        return f"{bytes_ / 1024:.0f}KB"
    return f"{bytes_}B"


def _fps_floor_from_contract(tick_us) -> str:
    """Contract tick ceiling → FPS floor. We store ticks (the assertion unit)
    but render FPS as the headline number (project convention; see README §
    Performance)."""
    if tick_us in (None, 0):
        return NO_VALUE
    return f"≥ {_fps_from_us(int(tick_us))}"


def _fps_range_from_observed_range(v) -> str:
    """Observed tick → FPS range, inverted (slow tick = low FPS). Collapses when the
    formatted endpoints would render the same. Returns the missing-value dash when there is
    nothing to show.

    Reads the current observation shape (a dict of statistics over a rolling sample
    window, moondeck/scenario/_observed.py) and renders p50 to p95: the typical cost and
    the tail. NOT min to max, which spans one lucky run to the worst excursion ever
    recorded and reads as a much wider performance envelope than the code actually has.
    The older [min, max] list is still accepted so a file written before the change
    renders rather than raising."""
    if v is None:
        return NO_VALUE
    pair = _observed_pair(v)
    if pair is None:
        return NO_VALUE
    lo_us, hi_us = pair
    # BOTH endpoints must convert: _fps_from_us returns the missing-value dash for a
    # non-positive tick, and pairing that with a real number renders a range with a dash on one
    # end, which reads as a measurement rather than as absent data.
    if lo_us <= 0 or hi_us <= 0:
        return NO_VALUE
    # Higher FPS comes from the LOWER tick, so the pair inverts.
    hi_fps, lo_fps = _fps_from_us(lo_us), _fps_from_us(hi_us)
    return lo_fps if lo_fps == hi_fps else f"{lo_fps}-{hi_fps}"


def _heap_contract_cell(v) -> str:
    """Contract heap/block floor → '≥ N KB'. None → '—'. 0 → 'unlimited'
    (the desktop platform reports free_heap=0 / max_alloc_block=0 to mean
    "no meaningful ceiling"; rendering them as missing was misleading)."""
    if v is None:
        return NO_VALUE
    if v == 0:
        return "unlimited"
    return f"≥ {_fmt_heap(int(v))}"


def _observed_pair(v):
    """Any observation shape → the (low, high) pair the cell renderers display, or None.

    ONE place knows the stored shape. Observations are a dict of statistics over a
    rolling sample window (moondeck/scenario/_observed.py); a file written before that
    change holds a [min, max] list, and an older one a bare scalar. Each renderer used to
    re-derive this, so the shape change broke them one at a time.

    For the dict, the pair is p50 to p95: the typical value and its tail. Min to max
    would span one lucky run to the worst excursion ever recorded, which reads as a far
    wider envelope than the code actually has. n=0 (never measured on this target, e.g.
    another OS) returns None, so the cell shows a dash rather than a fabricated zero.
    """
    if v is None:
        return None
    if isinstance(v, dict):
        if not v.get("n"):
            return None
        return int(v.get("p50", 0)), int(v.get("p95", 0))
    if isinstance(v, list) and len(v) == 2:
        return int(v[0]), int(v[1])
    if isinstance(v, (int, float)):
        return int(v), int(v)
    return None


def _heap_observed_cell(v) -> str:
    """Observed heap/block as 'N KB' or 'N-M KB'; the missing-value dash when absent; and
    'unlimited' for 0, matching the contract cell (desktop reports 0 to mean "no
    meaningful value", which should display as unlimited rather than be dropped)."""
    pair = _observed_pair(v)
    if pair is None:
        return NO_VALUE
    lo, hi = pair
    if lo == 0 and hi == 0:
        return "unlimited"
    return _fmt_heap(lo) if lo == hi else _fmt_heap_range([lo, hi])


def _format_perf_table(step: dict) -> list[str]:
    """Build a markdown table for a step's contract + observed data, one row
    per board. Returns [] when neither contract nor observed has anything
    measurable; otherwise returns the table lines plus any per-board audit
    footer (set_by / reason / observed-at) below."""
    contract = step.get("contract") or {}
    observed = step.get("observed") or {}
    boards = sorted(set(contract.keys()) | set(observed.keys()))
    if not boards:
        return []

    lines: list[str] = []
    lines.append("**Performance** (contract / observed), tick stored, FPS shown:")
    lines.append("")
    lines.append("| Board | FPS | heap | block |")
    lines.append("|---|---|---|---|")
    for b in boards:
        c = contract.get(b) or {}
        o = observed.get(b) or {}
        fps = f"{_fps_floor_from_contract(c.get('tick_us'))} / {_fps_range_from_observed_range(o.get('tick_us'))}"
        heap = f"{_heap_contract_cell(c.get('free_heap'))} / {_heap_observed_cell(o.get('free_heap'))}"
        block = f"{_heap_contract_cell(c.get('max_alloc_block'))} / {_heap_observed_cell(o.get('max_alloc_block'))}"
        lines.append(f"| `{b}` | {fps} | {heap} | {block} |")
    lines.append("")

    # Audit footer: contract origin + observation timestamps, only when present.
    audit: list[str] = []
    for b in boards:
        c = contract.get(b) or {}
        o = observed.get(b) or {}
        bits: list[str] = []
        if c.get("set_by") or c.get("reason"):
            sb = c.get("set_by") or "?"
            rs = f' "{c["reason"]}"' if c.get("reason") else ""
            bits.append(f"contract set {sb}{rs}")
        at = o.get("last_updated", o.get("at"))
        if at:
            bits.append(f"observed {_fmt_at_range(at)}")
        if bits:
            audit.append(f"- `{b}`: {' · '.join(bits)}")
    if audit:
        lines.extend(audit)
        lines.append("")
    return lines


def _fmt_us_range(v) -> str:
    """Pretty-print a [min, max] tick range. Collapses when the *formatted*
    endpoints would render identically (e.g. 84,500µs and 84,520µs both round
    to "85µs" at our resolution, showing them as a range adds noise)."""
    if isinstance(v, list) and len(v) == 2:
        lo, hi = int(v[0]), int(v[1])
        lo_s, hi_s = _fmt_us(lo), _fmt_us(hi)
        lo_fps, hi_fps = _fps_from_us(lo), _fps_from_us(hi)
        if lo_s == hi_s and lo_fps == hi_fps:
            return f"{lo_s} ({lo_fps} FPS)"
        return f"{lo_s}-{hi_s} ({hi_fps}-{lo_fps} FPS)"
    return f"{_fmt_us(int(v))} ({_fps_from_us(int(v))} FPS)"


def _fmt_heap_range(v) -> str:
    """Pretty-print a [min, max] heap/block range. Collapses when the formatted
    endpoints would render identically (KB rounding hides sub-KB drift)."""
    if isinstance(v, list) and len(v) == 2:
        lo, hi = int(v[0]), int(v[1])
        lo_s, hi_s = _fmt_heap(lo), _fmt_heap(hi)
        if lo_s == hi_s:
            return lo_s
        return f"{lo_s}-{hi_s}"
    return _fmt_heap(int(v))


def _fmt_at_range(at) -> str:
    """The date this observation last took a sample (`last_updated`). The older `at` key
    and its two-element [first_seen, last_updated] form are still accepted so a file
    written before the change renders; only the last element is shown, the first having
    described samples that had long aged out of the window."""
    if isinstance(at, list) and len(at) == 2:
        return str(at[1])
    return str(at)


def render_scenarios(files: list[dict]) -> str:
    by_module: dict[str, list[dict]] = defaultdict(list)
    for f in files:
        by_module[f["module"] or "Uncategorized"].append(f)
    for scens in by_module.values():
        scens.sort(key=lambda f: f["path"].name)

    lines: list[str] = []
    lines.append("# Scenario Tests")
    lines.append("")
    lines.append(
        "Auto-generated from `test/scenarios/{core,light}/scenario_*.json` by "
        "`moondeck/docs/generate_test_docs.py`. **Do not edit by hand**: "
        "update the JSON file's top-level fields and per-step `description` "
        "/ `contract` / `observed` instead, then regenerate."
    )
    lines.append("")
    lines.append(
        "Scenario tests are the integration tier in the [test strategy](../testing.md): "
        "each one is a JSON script that drives the full pipeline (desktop or live ESP32) "
        "and captures tick / heap per step against per-target contracts. "
        "Run them with `moondeck/scenario/run_scenario.py` (desktop) or "
        "`moondeck/scenario/run_live_scenario.py` (live device). "
        "See [testing.md § Performance contracts](../testing.md#performance-contracts-contracttarget) "
        "for the contract semantics."
    )
    lines.append("")

    for module in sorted(by_module.keys()):
        lines.append(f"## {module}")
        lines.append("")
        for f in by_module[module]:
            rel = f["path"].relative_to(ROOT).as_posix()
            lines.append(f"### {f['name']}")
            lines.append("")
            lines.append(f"`{rel}`: {f['description']}")
            lines.append("")
            # Top-level scenario flags worth surfacing.
            meta_bits: list[str] = [f"**Mode**: `{f['mode']}`"]
            if f["live_only"]:
                meta_bits.append("**live-only** (skipped in-process)")
            if f["also"]:
                meta_bits.append(f"**Also touches**: {', '.join(f['also'])}")
            lines.append(" · ".join(meta_bits))
            lines.append("")
            # Per-step expansion. Only measured steps get a `####` heading;
            # un-measured prep steps (set_control without `measure: true`)
            # collapse to a "Setup:" bullet list under the next measured step,
            # since they carry no contract/observed data of their own and
            # rendering each one as a heading bloats the page without signal.
            prep_buffer: list[dict] = []
            for step in f["steps"]:
                if not step["measure"]:
                    prep_buffer.append(step)
                    continue
                # Flush: render this measured step with any preceding prep.
                name = step["name"]
                op = step["op"]
                lines.append(f"#### `{name}` ({op})  📏")
                lines.append("")
                if step["description"]:
                    lines.append(step["description"])
                    lines.append("")
                if prep_buffer:
                    lines.append("**Setup** (preceding non-measured steps):")
                    for p in prep_buffer:
                        bits = [f"`{p['name']}` ({p['op']})"]
                        if p["description"]:
                            bits.append(p["description"])
                        lines.append(f"- {': '.join(bits)}")
                    lines.append("")
                    prep_buffer = []
                lines.extend(_format_perf_table(step))
            # Trailing prep steps after the last measurement (rare) get their
            # own collapsed bullet list under a "Trailing setup" header.
            if prep_buffer:
                lines.append("#### Trailing setup (no measurement after)")
                lines.append("")
                for p in prep_buffer:
                    bits = [f"`{p['name']}` ({p['op']})"]
                    if p["description"]:
                        bits.append(p["description"])
                    lines.append(f"- {': '.join(bits)}")
                lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def write_or_check(path: Path, content: str, check_only: bool) -> bool:
    """Return True if the file matches `content` (or was written successfully)."""
    if check_only:
        if not path.exists():
            print(f"  MISSING  {path.relative_to(ROOT)}")
            return False
        if path.read_text() != content:
            print(f"  CHANGED  {path.relative_to(ROOT)}")
            return False
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    print(f"  WROTE    {path.relative_to(ROOT)}")
    return True


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--unit", action="store_true", help="Write only unit-tests.md")
    p.add_argument("--scenario", action="store_true", help="Write only scenario-tests.md")
    p.add_argument("--check", action="store_true",
                   help="Exit 1 if regeneration would change content on disk")
    args = p.parse_args()

    do_unit = args.unit or not args.scenario
    do_scenario = args.scenario or not args.unit

    ok = True
    if do_unit:
        content = render_unit_tests(collect_unit_files())
        if not write_or_check(UNIT_OUT, content, args.check):
            ok = False
    if do_scenario:
        content = render_scenarios(collect_scenario_files())
        if not write_or_check(SCENARIO_OUT, content, args.check):
            ok = False

    if args.check and not ok:
        print("Run `uv run moondeck/docs/generate_test_docs.py` to regenerate.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
