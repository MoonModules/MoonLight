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
import sys

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
    probe = subprocess.run(
        ["vale", "--output=JSON", "--no-exit", "--ext=.h", "--path=probe.h"],
        input="// A colour scheme \u2014 an em-dash.\nint x = 1;\n",
        capture_output=True, text=True)
    try:
        hits = sum(len(v) for v in json.loads(probe.stdout or "{}").values())
    except json.JSONDecodeError:
        hits = 0
    if hits < 2:
        print("Prose check: FAILED ITS OWN CONTROL.\n")
        print("  A fixture carrying an em-dash and a British spelling produced "
              f"{hits} alert(s), expected 2.")
        print("  Vale is not reading comments: check the View at "
              ".vale/styles/config/views/CComments.yml and the [src/**] section of .vale.ini.")
        print("  Every header reads as clean until this passes.")
        return 1

    findings = sorted(set(findings))
    if not findings:
        print("Prose check: clean in added lines.")
        return 0

    print(f"Prose check: {len(findings)} finding(s) in ADDED lines.\n")
    for f in findings:
        print("  " + f)
    print("\nRules: docs/contributing/documentation-standards.md, enforced by .vale/styles/MoonLight/.")
    # Only an ERROR blocks; warnings and suggestions inform.
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
