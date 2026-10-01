#!/usr/bin/env -S uv run --script
"""Prose rules, enforced on ADDED lines through Vale.

The rules are stated in docs/contributing/documentation-standards.md and live as YAML under .vale/styles/,
one file per rule, so the standard and its check share one vocabulary. This script owns the one
thing Vale cannot: SCOPE. It walks the git diff and feeds Vale only what a change adds, because
the tree holds thousands of pre-existing violations and a whole-file gate would fail every commit
until a sweep larger than any change. Converting as files are touched reaches the same end state
without that.

An ERROR (em-dash, spelling, "e.g.") blocks; a warning or suggestion informs. Wired as a
write-time hook (hook_prose.py) so a fix happens while the sentence is still in mind, and again
at the commit gate. Without vale on PATH the check skips with a notice rather than failing.

TEMPORARY. This script and hook_prose.py exist only to keep the gate honest while the tree
holds inherited violations. Once `vale docs/ CLAUDE.md README.md` exits clean, delete both
and let .github/workflows/prose.yml check whole files.

    uv run moondeck/check/check_prose.py
"""

import json
import os
import re
import shutil
import subprocess
import tempfile
import sys
from pathlib import Path

# Files whose prose the standards govern. Not .json or .txt: generated or data.
# .mle/.mll/.mlm are MoonLive scripts: shipped, opened in the device's own editor, and read by
# every user who learns the language, so they are the most user-facing prose in the repo.
SUFFIXES = (".h", ".hpp", ".c", ".cpp", ".inc", ".md", ".py", ".js", ".css", ".html",
            ".mle", ".mll", ".mlm")

# Paths exempt, with the reason each earns it.
EXEMPT = (
    "docs/friend-repos/", # monthly digests OF OTHER PROJECTS, quoted from their sources
    "docs/work/past/",    # dated records: what was true at a moment, kept unrewritten
    "docs/reference/metrics/",      # generated
    "docs/reference/tests/",        # generated from test comments (fix the test, not the page)
    "docs/moonmodules/",  # partly generated technical pages
    "src/platform/desktop/vendor/",   # upstream single-header code (miniaudio): not our prose
    "src/ui/vendor/",                 # upstream browser code (Prism): not our prose either
    "moondeck/check/check_prose.py",  # the detector: its rule table spells the very patterns
    "docs/contributing/documentation-standards.md",  # the RULE: it must quote an em-dash and "analyse" to ban them
)

def added_lines(base):
    """Every line this branch or working tree ADDS, as (path, line text)."""
    diff = subprocess.run(
        ["git", "diff", base, "--unified=0"], capture_output=True, text=True
    ).stdout
    path, out = None, []
    for line in diff.split("\n"):
        if line.startswith("+++ b/"):
            path = line[6:]
        elif line.startswith("+") and not line.startswith("+++") and path:
            out.append((path, line[1:]))
    return out


ROOT = Path(__file__).resolve().parent.parent.parent
REPORT = ROOT / "docs" / "reference" / "metrics" / "prose.md"

# What the whole-tree report covers. The gate itself reads every suffix in SUFFIXES; these are the
# trees whose prose is OURS, which is the same question EXEMPT answers for the diff.
REPORT_ROOTS = ("src/", "moondeck/", "mooninstaller/", "test/", "docs/", "CLAUDE.md", "README.md")


def committed_counts():
    """Every rule's count in the COMMITTED report, or None when there is no baseline yet.

    Read from git rather than from the working tree, because `write_report` rewrites the file before the comparison happens.
    Same shape as check_docgen's ratchet, for the same reason: a number nobody compares against is a number that rises in silence.
    """
    rel = REPORT.relative_to(ROOT).as_posix()
    r = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        return None
    counts, in_rules = {}, False
    for line in r.stdout.split("\n"):
        # Only the `| rule | findings |` table: the worst-files table shares its column shape, and a
        # row from it would enter the baseline as a rule that can never appear in the new counts.
        if line.startswith("| rule |"):
            in_rules = True
            continue
        if in_rules and line.startswith("## "):
            break
        if in_rules and line.startswith("| `"):
            cells = [c.strip() for c in line.strip("|").split("|")]
            if len(cells) == 2 and cells[1].isdigit():
                counts[cells[0].strip("`")] = int(cells[1])
    return counts or None


def ratchet(by_rule: dict, total: int) -> list:
    """Rules whose count ROSE against the committed report, worst first.

    Per rule as well as per total, because a total hides one rule paying for another: an em-dash swept out of ten files while a new weasel word lands in twenty reads as progress.
    """
    was = committed_counts()
    if was is None:
        return []
    risen = [(rule, was.get(rule, 0), now) for rule, now in by_rule.items()
             if now > was.get(rule, 0)]
    old_total = sum(was.values())
    if total > old_total:
        risen.append(("(total)", old_total, total))
    return sorted(risen, key=lambda r: r[1] - r[2])


def write_report():
    """Count every finding in the tree, by rule, into docs/reference/metrics/prose.md.

    Whole files rather than a diff, so the number is the debt itself rather than what one change added.
    Vale is run once over the roots below; a per-file loop cost minutes for the same answer.
    """
    r = subprocess.run(["vale", "--output=JSON", "--no-exit", *REPORT_ROOTS],
                       capture_output=True, text=True, cwd=ROOT)
    # VALIDATE BEFORE PARSING. `--no-exit` makes Vale exit 0 on findings, so a non-zero status here
    # is a real failure (a bad config, a missing style) and its stdout is empty or partial. Treating
    # an empty stdout as `{}` wrote a report of ZERO findings and the ratchet read that as the debt
    # being swept, which is the silent-zero this file guards against everywhere else.
    if r.returncode != 0 or not r.stdout.strip():
        return None
    try:
        report = json.loads(r.stdout)
    except json.JSONDecodeError:
        # A broken run must not overwrite a good report, and must not read as one either: the
        # caller fails on None rather than skipping the comparison.
        return None
    if not isinstance(report, dict):
        return None

    by_rule, by_file, total = {}, {}, 0
    for path, alerts in report.items():
        rel = os.path.relpath(path, ROOT) if os.path.isabs(path) else path
        # The same exemptions as the gate: a generated page present on disk after a docs build counted its source's prose a second time.
        if rel.startswith(EXEMPT):
            continue
        if alerts:
            by_file[rel] = len(alerts)
        for a in alerts:
            by_rule[a.get("Check", "?")] = by_rule.get(a.get("Check", "?"), 0) + 1
            total += 1

    lines = [
        "# Prose report",
        "",
        "Every prose finding in the tree, by rule. Generated by",
        "[`moondeck/check/check_prose.py`](../../../moondeck/check/check_prose.py) on every run.",
        "**Do not edit by hand.**",
        "",
        "The gate is scoped to the lines a change ADDS, so a change is judged on its own prose.",
        "This number is the debt behind that gate, and it RATCHETS: a rise fails the check, the way",
        "docgen.md does. A comment is prose wherever it lives, so a `//` in the web interface and a",
        "`#` in a script are counted beside a `///` in a header.",
        "",
        f"**{total} finding(s)** across {len(by_file)} file(s).",
        "",
        "| rule | findings |",
        "|---|---|",
    ]
    for rule, n in sorted(by_rule.items(), key=lambda kv: -kv[1]):
        lines.append(f"| `{rule}` | {n} |")
    lines += ["", "## Worst files", "", "| file | findings |", "|---|---|"]
    for rel, n in sorted(by_file.items(), key=lambda kv: -kv[1])[:20]:
        lines.append(f"| `{rel}` | {n} |")
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return by_rule, total


def main():
    # Against main when on a branch, else the working tree: the check means "what am I adding".
    base = "main...HEAD" if len(sys.argv) < 2 else sys.argv[1]
    if subprocess.run(["git", "rev-parse", "--verify", "main"],
                      capture_output=True).returncode != 0:
        base = "HEAD"

    if shutil.which("vale") is None:
        print("Prose check: vale is not installed (brew install vale); skipping.")
        return 0

    # Group added lines per file, so Vale sees each file's additions as one document and its
    # per-file exemptions in .vale.ini apply. Line numbers reported are positions WITHIN the
    # added text, not the file; the quoted text is what identifies the line.
    by_path = {}
    for path, text in added_lines(base) + added_lines("HEAD"):
        if not path.endswith(SUFFIXES) or path.startswith(EXEMPT):
            continue
        by_path.setdefault(path, []).append(text)

    findings, errors = [], 0
    for path, lines in by_path.items():
        # Vale parses by extension; a .h or .py is fed as plain text so comment prose is checked
        # without a code-aware parser pretending the whole file is a program.
        ext = ".md" if path.endswith(".md") else ".txt"
        r = subprocess.run(["vale", "--output=JSON", "--no-exit", "--ext=" + ext,
                            "--path=" + path],
                           input="\n\n".join(dict.fromkeys(lines)), capture_output=True, text=True)
        try:
            report = json.loads(r.stdout or "{}")
        except json.JSONDecodeError:
            continue
        for alerts in report.values():
            for a in alerts:
                sev = a.get("Severity", "")
                if sev == "error":
                    errors += 1
                findings.append(f"{path}: {sev} {a.get('Check')}: {a.get('Message')}  "
                                f"[{a.get('Match', '')[:40]}]")

    # Pages the sweep has finished are checked WHOLE, not by diff: .vale.ini lists them with every
    # rule promoted to error, so any regression on such a page fails here before it reaches a PR.
    strict = [l.strip()[1:-1] for l in open(".vale.ini", encoding="utf-8")
              if l.startswith("[") and l.strip().endswith(".md]")]
    for path in strict:
        if not os.path.exists(path):
            continue
        r = subprocess.run(["vale", "--output=JSON", "--no-exit", path], capture_output=True, text=True)
        try:
            report = json.loads(r.stdout or "{}")
        except json.JSONDecodeError:
            continue
        for alerts in report.values():
            for a in alerts:
                sev = a.get("Severity", "")
                if sev == "error":
                    errors += 1
                findings.append(f"{path}: {sev} {a.get('Check')}: {a.get('Message')}  "
                                f"[{a.get('Match', '')[:40]}]  (whole file: this page is finished)")

    # CONTROL: a header reaches Vale through the tree-sitter View in .vale.ini, and a missing or
    # broken one prints `view 'CComments' not found` and EXITS 0. Every header then reports clean,
    # which is indistinguishable from a swept tree. So lint a fixture that MUST fail: if Vale finds
    # nothing in a line carrying an em-dash and a British spelling, the setup is broken rather than
    # the tree. A zero is only trustworthy once something that should fire, fires.
    # ONE PROBE PER VIEW, because each is a separate way to read nothing. The report counts every
    # language, and the ratchet below reads a fall as progress: a broken `javascript` view drops
    # about eleven hundred findings, which is a sweep to anything that only reads the number.
    # Written to a REAL FILE, never piped with `--ext`: that flag treats the input as plain text and
    # never consults the config's View at all, so a probe built on it stays green while the view it
    # claims to test is broken. Measured by breaking the view on purpose and watching it pass.
    #
    # One probe per language the report counts. Only the C family needs a View, but a `.js` or `.py`
    # reaching Vale at all depends on its own section of .vale.ini, and a section deleted by accident
    # drops eleven hundred findings that the ratchet below would read as a sweep.
    PROBES = (
        (".h",  "// A colour scheme \u2014 an em-dash.\nint x = 1;\n",  "the CComments view"),
        (".js", "// A colour scheme \u2014 an em-dash.\nconst x = 1;\n", "the [*.{js,mjs}] section"),
        (".py", "# A colour scheme \u2014 an em-dash.\nx = 1\n",         "the [*.py] section"),
    )
    probe_dir = tempfile.mkdtemp()
    for ext, fixture, view in PROBES:
        fixture_path = os.path.join(probe_dir, "probe" + ext)
        with open(fixture_path, "w", encoding="utf-8") as fh:
            fh.write(fixture)
        probe = subprocess.run(
            ["vale", "--output=JSON", "--no-exit", fixture_path],
            capture_output=True, text=True, cwd=ROOT)
        try:
            hits = sum(len(v) for v in json.loads(probe.stdout or "{}").values())
        except json.JSONDecodeError:
            hits = 0
        if hits < 2:
            print("Prose check: FAILED ITS OWN CONTROL.\n")
            print(f"  A {ext} fixture carrying an em-dash and a British spelling produced "
                  f"{hits} alert(s), expected 2.")
            print(f"  Vale is not reading {ext} comments: check {view} in .vale.ini.")
            print(f"  Every {ext} file reads as clean until this passes, and the report below "
                  f"would record that as a sweep.")
            return 1

    # THE STANDING DEBT, written where the other metrics live. The gate above is scoped to added
    # lines, which is what keeps a change honest about its own prose; it says nothing about what the
    # tree already carries. Those two numbers answer different questions and the second one had no
    # home, so 1104 findings in the scripts and the web interface were invisible until the views in
    # .vale.ini were written. A report nobody can see is a number nobody fixes.
    counted = write_report()
    # AND RATCHETED. Writing the number without comparing it lets the debt rise in silence, which is
    # the state this report was written to end: docgen.md has held its line this way for months.
    if counted is None:
        print("Prose check: the whole-tree report could not be produced, so the number in\n"
              "  docs/reference/metrics/prose.md is unverified. Check that `vale` runs.")
        return 1
    risen = ratchet(*counted)
    if risen:
        print("PROSE FINDINGS ROSE against the committed report. The list only shrinks:")
        for rule, was, now in risen:
            print(f"  {rule}: {was} -> {now}")
        print("  Fix them, or say in the commit why the rule itself changed.\n")

    findings = sorted(set(findings))
    if not findings:
        print("Prose check: clean in added lines.")
        # A clean diff is not a clean tree: the ratchet above still decides, or the report could
        # rise on a line this change rewrote without adding.
        return 1 if risen else 0

    print(f"Prose check: {len(findings)} finding(s) in ADDED lines.\n")
    for f in findings:
        print("  " + f)
    print("\nRules: docs/contributing/documentation-standards.md, enforced by .vale/styles/MoonLight/.")
    # Only an ERROR blocks; warnings and suggestions inform. A risen report blocks too: the number
    # in the tracked file is the promise, and it only falls.
    return 1 if (errors or risen) else 0


if __name__ == "__main__":
    sys.exit(main())
