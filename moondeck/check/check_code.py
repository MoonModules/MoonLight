#!/usr/bin/env python3
"""The code report: one page of counts that only fall, in the shape docgen.md and prose.md have.

Each rule is a textbook measure with a known tool: cyclomatic complexity, function length, nesting depth and parameter count from lizard, duplicated blocks from jscpd, file length and the two architecture boundaries counted here, and the blocking calls the render path reaches from Clang's `-Wfunction-effects` through check_nonblocking's build, a clean rebuild of the desktop that costs minutes. A finding is listed per file, so a touched file clears its own rows and leaves the list. The committed docs/reference/metrics/code.md is the number to beat, per rule and in total.

Lizard tokenizes rather than parses, which is what lets it run in a second with no build; the cost is a mangled name on some template-dense bodies. Counting per FILE rather than whitelisting per function name is what makes that harmless: a file's count is right whatever lizard calls the function.

Usage:
  uv run moondeck/check/check_code.py                   # rewrite code.md, exit 1 if any count rose
  uv run moondeck/check/check_code.py --module Control  # the findings in one module's files, no report
  uv run moondeck/check/check_code.py --account         # this change's own account against the last commit, for its message
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

HOT_PATH = "blocking call on the render path"
PLATFORM = "platform code outside src/platform"
CORE_LIGHT = "light include in core"
RULES = {"complex function": f"cyclomatic complexity > {MAX_CCN}",
         "long function": f"> {MAX_NLOC} lines of code",
         "deeply nested": f"control flow nested deeper than {MAX_NESTING}",
         "long parameter list": f"> {MAX_PARAMS} parameters",
         "duplicated block": f">= {MIN_CLONE_LINES} lines also found elsewhere",
         "large file": f"> {MAX_FILE_LINES} lines",
         HOT_PATH: "a call from tick, tick20ms or tick1s that can block or allocate",
         PLATFORM: "a vendor header or a platform #ifdef outside src/platform",
         CORE_LIGHT: "a src/core file including a light/ header"}
LIMITS = {"complex function": MAX_CCN, "long function": MAX_NLOC, "deeply nested": MAX_NESTING,
          "long parameter list": MAX_PARAMS, "duplicated block": MIN_CLONE_LINES, "large file": MAX_FILE_LINES, HOT_PATH: 1,
          PLATFORM: 1, CORE_LIGHT: 1}
# The catalog move for each smell (Fowler, Refactoring), so a finding says how it is solved; the table is coding-standards § From a finding to a fix.
FIXES = {"complex function": "a chain on one value becomes a table; a switch on a state becomes one method per state",
         "long function": "Extract Function: a named step a reader can skip",
         "deeply nested": "a guard becomes an early return; an inner loop becomes a function",
         "long parameter list": "Introduce Parameter Object: the parameters that travel together become one struct",
         "duplicated block": "Extract Function, or Pull Up Method into the shared base: one home for the block",
         "large file": "Extract Class, Move Function: a module per concern",
         HOT_PATH: "Move the work off the render thread (a worker, a cached value, a deferred apply), or annotate a callee that cannot block",
         PLATFORM: "Move it behind a function in src/platform, and branch on platform_config.h with if constexpr",
         CORE_LIGHT: "Dependency Inversion: core declares the interface, and the light domain implements and registers it"}


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


def functions_over(funcs) -> int:
    """How many functions break at least one limit, each counted once however many it breaks: the KPI and repo health read this one number."""
    return len({(f["file"], f["start"]) for f in funcs if findings([f])})


def owned_files() -> list:
    """The code files we own, tracked by git and matched by FILE_GLOBS, less the vendored ones."""
    # A git that does not answer raises: an empty list would read as a tree with nothing to measure.
    # `:(glob)` makes `**` match zero directories too, so src/main.cpp counts as src/**/*.cpp does in a shell.
    proc = subprocess.run(["git", "ls-files", *(":(glob)" + g for g in FILE_GLOBS)], cwd=ROOT, capture_output=True, text=True, timeout=60, check=True)
    # The working tree is what is measured, so a tracked file deleted on disk counts for nothing.
    return [rel for rel in proc.stdout.split() if not rel.startswith(FILE_EXCLUDE) and (ROOT / rel).exists()]


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
    """A clone counts under both files it is in, so either file's edit clears its row; plus the share and the count of duplicated lines, for the headline."""
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
    total = report.get("statistics", {}).get("total", {})
    return rows, (float(total.get("percentage", 0.0)), int(total.get("duplicatedLines", 0)))


def large_files() -> list:
    """Every owned code file over the line limit, as (file, rule, name, lines) rows like a function's."""
    rows = []
    for rel in owned_files():
        lines = sum(1 for _ in open(ROOT / rel, encoding="utf-8", errors="replace"))
        if lines > MAX_FILE_LINES:
            rows.append((rel, "large file", rel.rsplit("/", 1)[-1], lines))
    return rows


# A vendor SDK header, or a preprocessor branch on the target, which only src/platform may hold.
_PLATFORM_RE = re.compile(r'#include\s*[<"](?:esp_|freertos/|driver/|hal/|soc/|SDL|wiringPi|pigpio)'
                          r'|#(?:if|elif)(?:n?def\s+|\s+defined\s*\(\s*)(?:ESP_PLATFORM|CONFIG_IDF|__APPLE__|__linux__|_WIN32)')
_LIGHT_INCLUDE_RE = re.compile(r'#include\s*[<"]light/')


def boundary_findings(rel: str, lines) -> list:
    """The architecture's two boundaries in one firmware file: platform code stays in src/platform, and core includes nothing of the light domain."""
    if not rel.startswith("src/") or rel.startswith(("src/platform/", "src/ui/")) or not rel.endswith((".h", ".cpp")):
        return []
    rows = []
    for n, line in enumerate(lines, 1):
        code = line.split("//")[0].strip()
        if _PLATFORM_RE.search(code):
            rows.append((rel, PLATFORM, f"line {n}: {code}", 1))
        if rel.startswith("src/core/") and _LIGHT_INCLUDE_RE.search(line):
            rows.append((rel, CORE_LIGHT, f"line {n}: {code}", 1))
    return rows


def boundary_rows() -> list:
    """Every boundary crossing among the files we own."""
    rows = []
    for rel in owned_files():
        with open(ROOT / rel, encoding="utf-8", errors="replace") as f:
            rows += boundary_findings(rel, f)
    return rows


class BuildMissing(Exception):
    """The desktop build the hot path is read from is absent or does not compile."""


def hotpath_rows() -> list | None:
    """One row per render-path call site that can block or allocate, a float conversion at a formatTo site included.

    None when this host's compiler cannot measure it (no Clang 20 -Wfunction-effects), which skips the rule; BuildMissing when the build is absent or fails, which fails the run rather than reading as a clean tree.
    """
    import check_nonblocking as nb
    build_dir = nb.check_clang_tidy._host_build_dir()
    if not (build_dir / "CMakeCache.txt").exists():
        raise BuildMissing(f"No build in {build_dir.relative_to(ROOT)}: run `uv run moondeck/build/build_desktop.py` first.")
    if not nb.function_effects_enabled(build_dir):
        print("The desktop build carries no -Wfunction-effects (it needs Clang 20+), so the hot path cannot be measured here.", file=sys.stderr)
        return None
    out = nb.build_output(build_dir)
    if out is None:
        raise BuildMissing("The desktop build failed, so the hot path cannot be read.")
    rows = [(r["file"], HOT_PATH, f"{r['callee']} in {r['fn'] or '?'}", 1) for r in nb.collect(out)]
    for site in nb.float_conversions_on_the_hot_path():
        file, line = site.rsplit(":", 1)
        rows.append((file, HOT_PATH, f"a float conversion at formatTo, line {line}", 1))
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
    return None if text is None else counts_in(text)


def counts_in(text: str) -> dict | None:
    """A report's counts, per rule and in total, from its `## By rule` table and headline."""
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


def duplicated_lines_in(text: str) -> int | None:
    """The duplicated lines a report's headline records, or None for a report written without the clone pass."""
    m = re.search(r"of all lines are duplicated, (\d+) lines", text)
    return int(m.group(1)) if m else None


def changed_lines() -> dict:
    """Lines added and removed under src/ and test/ since the last commit, staged or not, new files included."""
    out = {"src": [0, 0], "test": [0, 0]}
    diff = subprocess.run(["git", "diff", "HEAD", "--numstat", "--", "src", "test"], cwd=ROOT,
                          capture_output=True, text=True, timeout=60, check=True).stdout
    for line in diff.splitlines():
        added, removed, path = line.split("\t", 2)
        if added != "-":   # a binary file has no lines
            out[path.split("/")[0]][0] += int(added)
            out[path.split("/")[0]][1] += int(removed)
    new = subprocess.run(["git", "ls-files", "--others", "--exclude-standard", "--", "src", "test"], cwd=ROOT,
                         capture_output=True, text=True, timeout=60, check=True).stdout
    for rel in new.split():
        out[rel.split("/")[0]][0] += sum(1 for _ in open(ROOT / rel, encoding="utf-8", errors="replace"))
    return out


def account_line(lines: dict, base: dict | None, now: dict | None, dup_base: int | None, dup_now: int | None) -> str:
    """One line for a commit message: what it added and removed, and what it did to the code findings and duplication."""
    parts = [f"{area} +{a}/-{r} lines" for area, (a, r) in lines.items()]
    if base and now and base != now:
        was, total = base.get("(total)", 0), now.get("(total)", 0)
        moved = [f"{rule} {now.get(rule, 0) - base.get(rule, 0):+d}" for rule in sorted(set(base) | set(now))
                 if rule != "(total)" and now.get(rule, 0) != base.get(rule, 0)]
        parts.append(f"code findings {was} -> {total}" + (f" ({', '.join(moved)})" if moved else ""))
    if dup_base is not None and dup_now is not None:
        parts.append(f"duplicated lines {dup_base} -> {dup_now} ({dup_now - dup_base:+d})")
    return "Commit: " + " | ".join(parts)


def account() -> str:
    """This change's account against the last commit, reading code.md as the last run of this check wrote it."""
    before = committed(REPORT)
    after = REPORT.read_text(encoding="utf-8") if REPORT.exists() else None
    return account_line(changed_lines(),
                        counts_in(before) if before else None, counts_in(after) if after else None,
                        duplicated_lines_in(before) if before else None, duplicated_lines_in(after) if after else None)


def write_report(rows, duplicated=None) -> None:
    """The current state as a tracked page, so its git history is the trend."""
    by_rule = Counter(rule for _, rule, _, _ in rows)
    by_file = defaultdict(list)
    for file, rule, name, value in rows:
        by_file[file].append((rule, name, value))
    by_area = Counter(area(file) for file, _, _, _ in rows)
    out = ["# Code", "",
           "Generated by [`moondeck/check/check_code.py`](../../../moondeck/check/check_code.py) on every run. **Do not edit by hand.**", "",
           "Every function, block and file over a limit the [coding standards](../../contributing/coding-standards.md#static-checks) set. Current state only: the trend is this file's git history. The list only shrinks, per rule and in total, and a touched file clears its own rows.", "",
           f"**{len(rows)} finding(s)** across {len(by_file)} file(s)."
           + (f" {duplicated[0]:.2f}% of all lines are duplicated, {duplicated[1]} lines." if duplicated is not None else ""), "",
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
    ap.add_argument("--account", action="store_true", help="Print this change's account against the last commit, without measuring.")
    args = ap.parse_args()
    if args.account:
        print(account())
        return 0

    funcs = measure()
    if funcs is None:
        return 2
    # Clones are found repo-wide, so the module view leaves them to the full report rather than pay for that pass per module.
    report = None if args.module else clones()
    if report is None and not args.module:
        return 2
    dup_rows, duplicated = clone_rows(report) if report else ([], None)
    rows = findings(funcs) + dup_rows + large_files() + boundary_rows()
    # The hot path is a full rebuild, so the module view leaves it to the full report as it does clones; check_nonblocking --module is its detailed view.
    try:
        hot = None if args.module else hotpath_rows()
    except BuildMissing as e:
        print(e, file=sys.stderr)
        return 2
    if hot is not None:
        rows += hot

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

    base = committed_counts()
    if hot is None:
        # A host without Clang 20's -Wfunction-effects cannot count the hot path: every other rule still ratchets, and the report keeps the last full count rather than losing that rule.
        print(f"SKIP {HOT_PATH}: not measurable on this host (it needs Clang 20+), so code.md is left as it is.")
        base = {k: v for k, v in (base or {}).items() if k not in (HOT_PATH, "(total)")} or None
        now = {k: v for k, v in counts(rows).items() if k != "(total)"}
    else:
        # The report is the artifact, written on every run: stdout scrolls away, the file is what the reviewer reads.
        write_report(rows, duplicated)
        now = counts(rows)
    rose = risen(base, now)
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
