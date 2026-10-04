#!/usr/bin/env python3
"""The code report: one page of counts that only fall, in the shape docgen.md and prose.md have.

Each rule is a textbook measure with a known tool: cyclomatic complexity, function length, nesting depth and parameter count from lizard, duplicated blocks from jscpd, and file length counted here. A finding is listed per file, so a touched file clears its own rows and leaves the list. The committed docs/reference/metrics/code.md is the number to beat, per rule and in total.

Lizard tokenizes rather than parses, which is what lets it run in a second with no build; the cost is a mangled name on some template-dense bodies. Counting per FILE rather than whitelisting per function name is what makes that harmless: a file's count is right whatever lizard calls the function.

Usage:
  uv run moondeck/check/check_code.py                   # rewrite code.md, exit 1 if any count rose
  uv run moondeck/check/check_code.py --module Control  # the findings in one module's files, no report
"""

import argparse
import csv
import io
import os
import re
import json
import shutil
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _ratchet import committed, risen  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent
REPORT = ROOT / "docs" / "reference" / "metrics" / "code.md"

# McCabe's cyclomatic complexity and the body's non-comment lines, as lizard counts them; nesting and file length at SonarQube's defaults. clang-tidy's readability-function-* checks stay off, so each has one owner.
MAX_CCN = 10
MAX_NLOC = 60
MAX_NESTING = 3
MAX_PARAMS = 7
MAX_FILE_LINES = 1000
# jscpd's defaults: a clone is this many lines sharing this many tokens, so renamed variables still match and a changed structure does not.
MIN_CLONE_LINES = 5
MIN_CLONE_TOKENS = 50

# Firmware C++ only: src/ui is JavaScript served to the browser, and the vendor header is upstream's. -ENS adds the nesting depth column.
LIZARD_ARGS = ["src/", "-l", "cpp", "-x", "src/ui/*", "-x", "src/platform/desktop/vendor/*", "-ENS"]
# Every code file we own, which the file-length and duplication rules both read; vendored and generated files are not ours to shrink.
FILE_GLOBS = ("src/**/*.h", "src/**/*.cpp", "src/**/*.js", "moondeck/**/*.py", "mooninstaller/*.js", "test/**/*.cpp", "test/**/*.py", "test/**/*.mjs")
FILE_EXCLUDE = ("src/platform/desktop/vendor/", "src/ui/vendor/", "test/doctest.h")

RULES = {"complex function": f"cyclomatic complexity > {MAX_CCN}",
         "long function": f"> {MAX_NLOC} lines of code",
         "deeply nested": f"control flow nested deeper than {MAX_NESTING}",
         "long parameter list": f"> {MAX_PARAMS} parameters",
         "duplicated block": f">= {MIN_CLONE_LINES} lines also found elsewhere",
         "large file": f"> {MAX_FILE_LINES} lines"}
LIMITS = {"complex function": MAX_CCN, "long function": MAX_NLOC, "deeply nested": MAX_NESTING,
          "long parameter list": MAX_PARAMS, "duplicated block": MIN_CLONE_LINES, "large file": MAX_FILE_LINES}
# The catalog move for each smell (Fowler, Refactoring), so a finding says how it is solved; the table is coding-standards § From a finding to a fix.
FIXES = {"complex function": "a chain on one value becomes a table; a switch on a state becomes one method per state",
         "long function": "Extract Function: a named step a reader can skip",
         "deeply nested": "a guard becomes an early return; an inner loop becomes a function",
         "long parameter list": "Introduce Parameter Object: the parameters that travel together become one struct",
         "duplicated block": "Extract Function, or Pull Up Method into the shared base: one home for the block",
         "large file": "Extract Class, Move Function: a module per concern"}


def measure(extra=None):
    """Every function lizard measured, as dicts; None when lizard produced nothing, which is a broken run, never a clean one.

    `--csv` for a real format rather than a column-aligned table, and `-W /dev/null` so no whitelist lizard finds on its own filters the measurement.
    """
    cmd = ["uv", "run", "--with", "lizard", "python3", "-m", "lizard",
           *LIZARD_ARGS, "--csv", "-W", os.devnull, *(extra or [])]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=300)
    out = []
    for r in csv.reader(io.StringIO(proc.stdout)):
        # nloc,ccn,token,param,length,location,file,name,long_name,start,end,nesting
        if len(r) < 12:
            continue
        try:
            out.append({"nloc": int(r[0]), "ccn": int(r[1]), "params": int(r[3]), "file": r[6], "name": r[7], "start": int(r[9]), "nesting": int(r[11])})
        except ValueError:
            continue
    if not out:
        print("lizard produced no parseable output (uv run --with lizard)", file=sys.stderr)
        print(proc.stderr.strip()[:500], file=sys.stderr)
        return None
    return out


def findings(funcs) -> list:
    """Every (file, rule, function, value) a function earns, one row per rule it breaks."""
    rows = []
    for f in funcs:
        if f["ccn"] > MAX_CCN:
            rows.append((f["file"], "complex function", f["name"], f["ccn"]))
        if f["nloc"] > MAX_NLOC:
            rows.append((f["file"], "long function", f["name"], f["nloc"]))
        if f["nesting"] > MAX_NESTING:
            rows.append((f["file"], "deeply nested", f["name"], f["nesting"]))
        if f["params"] > MAX_PARAMS:
            rows.append((f["file"], "long parameter list", f["name"], f["params"]))
    return rows


def owned_files() -> list:
    """The code files we own, tracked by git and matched by FILE_GLOBS, less the vendored ones."""
    # A git that does not answer raises: an empty list would read as a tree with nothing to measure.
    proc = subprocess.run(["git", "ls-files", *FILE_GLOBS], cwd=ROOT, capture_output=True, text=True, timeout=60, check=True)
    return [rel for rel in proc.stdout.split() if not rel.startswith(FILE_EXCLUDE)]


def clones():
    """Every clone jscpd finds among the files we own, as the parsed report, or None when the run produced nothing, which is a broken run rather than a clean tree.

    jscpd comes through npx, the dependency the docs build already has for moxygen.
    """
    npx = shutil.which("npx")
    if not npx:
        print("jscpd needs npx (node), which the docs build needs as well", file=sys.stderr)
        return None
    with tempfile.TemporaryDirectory() as td:
        proc = subprocess.run([npx, "--yes", "jscpd@4", *owned_files(),
                               "--min-lines", str(MIN_CLONE_LINES), "--min-tokens", str(MIN_CLONE_TOKENS),
                               "--reporters", "json", "--output", td, "--silent"],
                              cwd=ROOT, capture_output=True, text=True, timeout=600)
        report = Path(td) / "jscpd-report.json"
        if not report.exists():
            print("jscpd wrote no report", file=sys.stderr)
            print(proc.stderr.strip()[:500], file=sys.stderr)
            return None
        return json.loads(report.read_text(encoding="utf-8"))


def clone_rows(report) -> tuple:
    """A clone counts under both files it is in, so either file's edit clears its row; plus the share of duplicated lines, for the headline."""
    rows = []
    for d in report.get("duplicates", []):
        a, b = d["firstFile"], d["secondFile"]
        rel = lambda f: os.path.relpath(f["name"], ROOT) if os.path.isabs(f["name"]) else f["name"]
        lines = int(d.get("lines", 0))
        # From the start and the length: jscpd's end field is wrong on some second files, while its line count holds.
        start = lambda f: int(f.get("startLoc", {}).get("line", f.get("start", 0)))
        for here, there in ((a, b), (b, a)):
            rows.append((rel(here), "duplicated block",
                         f"lines {start(here)}-{start(here) + lines - 1}, also in {rel(there)}:{start(there)}", lines))
    pct = report.get("statistics", {}).get("total", {}).get("percentage", 0.0)
    return rows, float(pct)


def large_files() -> list:
    """Every owned code file over the line limit, as (file, rule, name, lines) rows like a function's."""
    rows = []
    for rel in owned_files():
        lines = sum(1 for _ in open(ROOT / rel, encoding="utf-8", errors="replace"))
        if lines > MAX_FILE_LINES:
            rows.append((rel, "large file", rel.rsplit("/", 1)[-1], lines))
    return rows


def area(path: str) -> str:
    """The area a file counts under, the second directory level under src/."""
    parts = path.split("/")
    return parts[1] if len(parts) > 2 and parts[0] == "src" else parts[0]


def counts(rows) -> dict:
    """The counts the ratchet compares: one per rule plus the total."""
    now = Counter(rule for _, rule, _, _ in rows)
    now["(total)"] = len(rows)
    return dict(now)


def committed_counts() -> dict | None:
    """The committed report's counts, parsed from its `## By rule` table and headline."""
    text = committed(REPORT)
    if text is None:
        return None
    out, in_rules = {}, False
    for line in text.split("\n"):
        if line.startswith("## "):
            in_rules = line.strip() == "## By rule"
        m = re.match(r"^\| (.+?) \| (\d+) \|$", line.strip())
        if in_rules and m and m.group(1) != "Rule":
            out[m.group(1)] = int(m.group(2))
        m = re.match(r"^\*\*(\d+) finding\(s\)\*\*", line.strip())
        if m:
            out["(total)"] = int(m.group(1))
    return out or None


def write_report(rows, duplicated_pct=None) -> None:
    """The current state as a tracked page, so its git history is the trend."""
    by_rule = Counter(rule for _, rule, _, _ in rows)
    by_file = defaultdict(list)
    for file, rule, name, value in rows:
        by_file[file].append((rule, name, value))
    by_area = Counter(area(file) for file, _, _, _ in rows)
    out = ["# Code", "",
           "Generated by [`moondeck/check/check_code.py`](../../../moondeck/check/check_code.py) on every run. **Do not edit by hand.**", "",
           "Every function over a limit the [coding standards](../../contributing/coding-standards.md#static-checks) set. Current state only: the trend is this file's git history. The list only shrinks, per rule and in total, and a touched file clears its own rows.", "",
           f"**{len(rows)} finding(s)** across {len(by_file)} file(s)."
           + (f" {duplicated_pct:.2f}% of all lines are duplicated." if duplicated_pct is not None else ""), "",
           "## By rule", "", "| Rule | Findings |", "|---|---:|"]
    for rule in sorted(by_rule, key=lambda r: (-by_rule[r], r)):
        out.append(f"| {rule} | {by_rule[rule]} |")
    out += ["", "## How a finding is solved", "",
            "Each rule is a code smell with a named move from the refactoring catalog, the table in [coding standards, from a finding to a fix](../../contributing/coding-standards.md#from-a-finding-to-a-fix). The commit names the move.", ""]
    for rule in RULES:
        out.append(f"- **{rule}** ({RULES[rule]}): {FIXES[rule]}.")
    out += ["", "## By area", "", "| Area | Findings |", "|---|---:|"]
    for a in sorted(by_area, key=lambda a: (-by_area[a], a)):
        out.append(f"| `{a}` | {by_area[a]} |")
    out += ["", "## By file", "", "The next function worth simplifying is at the top of each file's list.", "",
            "| File | Findings | Worst |", "|---|---:|---|"]
    for file in sorted(by_file, key=lambda f: (-len(by_file[f]), f)):
        worst = max(by_file[file], key=lambda r: r[2] / LIMITS[r[0]])
        out.append(f"| `{file}` | {len(by_file[file])} | `{worst[1].removeprefix('mm::')}`, {worst[0]} {worst[2]} |")
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text("\n".join(out) + "\n", encoding="utf-8")


def _table(rows) -> str:
    """Findings as text, worst first, for a module run or a rise."""
    def over(r):
        return r[3] / LIMITS[r[1]]
    lines = []
    for file, rule, name, value in sorted(rows, key=lambda r: -over(r)):
        lines.append(f"  {value:>4}  {rule:<17} {name.removeprefix('mm::'):<40} {file}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--module", help="Report only this module's source files, without touching the report.")
    args = ap.parse_args()

    funcs = measure()
    if funcs is None:
        return 2
    # Clones are found repo-wide, so the module view leaves them to the full report rather than pay for that pass per module.
    report = None if args.module else clones()
    if report is None and not args.module:
        return 2
    dup_rows, duplicated_pct = clone_rows(report) if report else ([], None)
    rows = findings(funcs) + dup_rows + large_files()

    if args.module:
        import check_clang_query
        only = check_clang_query.module_files(args.module)
        if not only:
            print(f"No source files for module '{args.module}'.", file=sys.stderr)
            return 2
        rows = [r for r in rows if r[0] in only]
        print(f"{args.module}: {len(rows)} finding(s) in {', '.join(only)}")
        print(_table(rows))
        return 0

    # The report is the artifact, written on every run: stdout scrolls away, the file is what the reviewer reads.
    base = committed_counts()
    write_report(rows, duplicated_pct)
    rose = risen(base, counts(rows))
    print(f"Code check: {len(rows)} finding(s), {', '.join(f'{RULES[r]}: {n}' for r, n in Counter(x[1] for x in rows).items())}.")
    if rose:
        print("\nROSE against the committed report. These only shrink:")
        for rule, was, now in rose:
            print(f"  {rule}: {was} -> {now}")
        print("  Simplify what this change touched, or say in the commit why the rule itself changed.")
    print("\nThe move per rule: " + "; ".join(f"{r}: {FIXES[r]}" for r in RULES) + ".\nRules: docs/contributing/coding-standards.md § From a finding to a fix.")
    return 1 if rose else 0


if __name__ == "__main__":
    sys.exit(main())
