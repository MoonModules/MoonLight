#!/usr/bin/env python3
"""Mechanical projectMM -> MoonLight rename sweep (dry-run by default).

The one-shot transition tool for [docs/work/future/rename-to-moonlight.md] Phase 1
step 4. Replaces the product-name token everywhere it is the *current* name,
while leaving the references that must NOT change: the predecessor "MoonLight"
prose/links (already repointed to ewowi/MoonLight), the MoonLive scripting
engine (a different name), and the docs/history era record (which keeps saying
projectMM by the present-tense history exception).

Run it NOW against today's tree to harden the exclude list and review the diff,
but DRY-RUN ONLY until the switch — the externally-visible identifiers (binary
name, mDNS prefix, repo URL, library.json name) must flip *at* the switch, lined
up with the first release under the new repo (see the plan's Phase 2/3).

ORDERING: the sweep also rewrites the repo URL (MoonModules/projectMM ->
MoonModules/projectMM) and the docs host (moonmodules.org/projectMM). Per the
plan that is correct ONLY when run *after* the repo rename (Phase 3.2): the URL
becomes MoonModules/projectMM, which only resolves once this repo holds that
name. So --apply belongs in Phase 3.3, after 3.2 — not before.

  uv run moondeck/repo_rename/rename_to_moonlight.py            # dry-run: list every hit, change nothing
  uv run moondeck/repo_rename/rename_to_moonlight.py --apply    # WRITE the changes (switch-day only)

Why a script and not a bare `sed`: the value here is not clever per-form logic
(a plain token replace is correct for every form — repo URL, host path,
binary basename, product name, deviceName slug all just contain the token), it
is the EXCLUDE LIST + the dry-run review + the binary/.git guards. Verified
against today's tree: `projectMM` is never a substring of another token, and the
rename touches neither `MoonLight` nor `MoonLive`, so those are safe by
construction — the only deliberate handling is the capitalised enum token
`ProjectMM` and the path exclusions below.

Exit 1 if a dry-run finds hits (so CI / a pre-switch check can assert "still
references to flip"); exit 0 when clean (post-switch, nothing left).
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# One token replacement. The longer forms (MoonModules/projectMM, projectMM.bin,
# moonmodules.org/projectMM) fall out of replacing the token inside them, so no
# per-form rules are needed. `ProjectMM` had its own entry for the DevType enum,
# which was renamed ahead of the sweep, so the capitalised form no longer occurs.
REPLACEMENTS = [
    ("projectMM", "MoonLight"),   # the product name in every form
]

# Repointing the predecessor's citations to ewowi/MoonLight is NOT a rule here, though it belongs to
# the same rename. It is a one-time migration, already applied, where this sweep is repeatable: a
# rule for it would rewrite the MoonModules/projectMM this sweep itself produces from
# MoonModules/projectMM, so a second run would silently aim our own URLs at the predecessor.

# The file list comes from `git ls-files` (tracked files only), so build output
# (build/, esp32/build/ — both gitignored) and other artifacts never enter the
# sweep, without maintaining a brittle path blocklist. Only these directory
# prefixes are then *additionally* excluded for content reasons:
EXCLUDE_DIRS = [
    "docs/history",              # records the projectMM ERA by name — present-tense
                                  #   history exception (CLAUDE.md § Principles)
]
# The dry run's own log, committed beside the script so a change in what the sweep reaches is
# a diff rather than a number somebody remembers. Written on a dry run only: --apply is the
# switch-day action, and its result is the commit itself.
REPORT = ROOT / "moondeck" / "repo_rename" / "rename_to_moonlight.md"

EXCLUDE_FILES = [
    # This plan describes BOTH names and the move between them; rewriting it would
    # corrupt its meaning ("the predecessor at MoonModules/projectMM vacates…").
    "docs/work/future/rename-to-moonlight.md",
    # The rename script itself (it names the tokens it replaces).
    "moondeck/repo_rename/rename_to_moonlight.py",
    # The readiness check names the old token in every line it protects.
    "moondeck/repo_rename/check_rename_ready.py",
    # And its report, which lists them back.
    "moondeck/repo_rename/check_rename_ready.md",
    # And this script's own log, which lists every hit back.
    "moondeck/repo_rename/rename_to_moonlight.md",
]

# File extensions the sweep considers (text formats that carry the name). Binary
# assets (.png/.jpg/.bin) are skipped both by extension and by the text-read
# guard below.
INCLUDE_SUFFIXES = {
    ".md", ".py", ".h", ".cpp", ".js", ".json", ".html", ".css",
    ".txt", ".csv", ".cmake", ".yml", ".yaml", ".ini",
}
# Files with no suffix that still matter (e.g. CMakeLists.txt handled via suffix;
# add bare names here if needed). CMakeLists.txt has a .txt-like role but no
# suffix match, so include it explicitly by name.
INCLUDE_NAMES = {"CMakeLists.txt"}


def is_excluded(rel: str) -> bool:
    for d in EXCLUDE_DIRS:
        if rel == d or rel.startswith(d + "/"):
            return True
    return rel in EXCLUDE_FILES


def wanted(path: Path) -> bool:
    if path.suffix in INCLUDE_SUFFIXES:
        return True
    return path.name in INCLUDE_NAMES


def iter_files():
    # Tracked files only — `git ls-files` respects .gitignore, so build output
    # (build/, esp32/build/) is never swept and we don't maintain a blocklist.
    # Caveat: gitignored files are therefore NOT covered — notably the private
    # bench registry moondeck/moondeck.json, whose "board" names must be
    # hand-updated at switch-time (see the plan's Phase 1 step 5).
    out = subprocess.run(
        ["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout
    for rel in sorted(out.splitlines()):
        if not rel or is_excluded(rel):
            continue
        path = ROOT / rel
        if not path.is_file() or not wanted(path):
            continue
        yield path


# A line carrying this marker keeps the old name. The sweep is a blanket replace, so the few places that must NOT change say so beside themselves rather than in a list here that drifts from the code. Each was found by rehearsing the sweep, and each fails silently rather than as a build error, which is why it is marked.
KEEP_MARKER = "rename-keep"

# Lines that must keep the old name where a marker cannot ride along.
KEEP_SUBSTRINGS = [
    # A v5 device's recovery image is stamped projectMM-moonbase, so renaming the check makes the new firmware reject the image already in flash, leaving no recovery path. It changes when a MoonBase built under the new name ships.
    "projectMM-moonbase",
    # The predecessor repository, which keeps its own name.
    "ewowi/MoonLight",
    # The backup bookmarklet targets OLD firmware, whose Restore accepts only this value, and it sits inside a template literal where a trailing marker cannot go.
    "projectMM-config-backup",
]

# Where an existing installation's own data is addressed by a path or key carrying the product name, so a rewrite reads an empty location rather than failing: @see the rename plan's migration section.
# These are listed here rather than marked in place because each is a path built from segments, where a marker would sit on the wrong line.
KEEP_PATH_KEYS = [
    # A desktop user's config, presets and scripts live under a directory named after the product, and nothing carries them across: renaming this boots into an empty profile with no error.
    'std::filesystem::path(base) / "projectMM"',
    'Library" / "Application Support" / "projectMM"',
    'std::filesystem::path(xdg) / "projectMM"',
    '".local" / "share" / "projectMM"',
    # The web installer's saved device list, which is the user's own bookmark set.
    '"projectMM.devices.v1"',
]


def keeps_old_name(line: str) -> bool:
    """True where a line opts out of the sweep, by marker or by known content."""
    if KEEP_MARKER in line:
        return True
    if any(k in line for k in KEEP_PATH_KEYS):
        return True
    return any(k in line for k in KEEP_SUBSTRINGS)


def count_hits(text: str) -> int:
    return sum(
        sum(line.count(old) for old, _ in REPLACEMENTS)
        for line in text.split("\n")
        if not keeps_old_name(line)
    )


def apply_replacements(text: str) -> str:
    out = []
    for line in text.split("\n"):
        if not keeps_old_name(line):
            for old, new in REPLACEMENTS:
                line = line.replace(old, new)
        out.append(line)
    return "\n".join(out)


def write_report(total: int, touched: int, per_file: list[tuple[str, int]]) -> None:
    """The dry run as a file git can diff: what the sweep reaches, by area and by file."""
    areas: dict[str, int] = {}
    for rel, hits in per_file:
        area = str(Path(rel).parent)
        areas[area] = areas.get(area, 0) + hits

    out = [
        "# Rename sweep",
        "",
        "Generated by [`moondeck/repo_rename/rename_to_moonlight.py`](rename_to_moonlight.py) "
        "on every dry run. **Do not edit by hand.**",
        "",
        "Everywhere the product name still appears as the CURRENT name, which is what the "
        "switch-day sweep rewrites.",
        "",
        "Committed so the reach is reviewable before the day. A file appearing here that should "
        "keep the old name needs a `rename-keep` marker, and the readiness check asserts the "
        "ones already found.",
        "",
        f"**{total} hits across {touched} files.**",
        "",
        "## By area",
        "",
        "| Hits | Area |",
        "|---:|---|",
    ]
    for area, n in sorted(areas.items(), key=lambda kv: -kv[1]):
        out.append(f"| {n} | `{area}` |")
    out += ["", "## The twenty widest files", "", "| Hits | File |", "|---:|---|"]
    for rel, n in sorted(per_file, key=lambda kv: -kv[1])[:20]:
        out.append(f"| {n} | `{rel}` |")
    out.append("")
    REPORT.write_text("\n".join(out) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description="projectMM -> MoonLight rename sweep")
    ap.add_argument("--apply", action="store_true",
                    help="WRITE the changes (default is a dry-run that changes nothing)")
    args = ap.parse_args()

    total_hits = 0
    touched = 0
    per_file: list[tuple[str, int]] = []
    for path in iter_files():
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, ValueError):
            continue  # binary / non-utf8 — the text guard, belt-and-braces with the suffix filter
        hits = count_hits(text)
        if hits == 0:
            continue
        total_hits += hits
        touched += 1
        rel = path.relative_to(ROOT).as_posix()
        per_file.append((rel, hits))
        if args.apply:
            path.write_text(apply_replacements(text), encoding="utf-8")
            print(f"  rewrote {rel} ({hits} hit{'s' if hits != 1 else ''})")
        else:
            print(f"  {rel}: {hits} hit{'s' if hits != 1 else ''}")
            # show each line so the dry-run is reviewable
            for n, line in enumerate(text.splitlines(), 1):
                if not keeps_old_name(line) and any(old in line for old, _ in REPLACEMENTS):
                    print(f"      {n}: {line.strip()[:100]}")

    mode = "APPLIED" if args.apply else "DRY-RUN"
    if not args.apply:
        write_report(total_hits, touched, per_file)
        print(f"\nWritten to {REPORT.relative_to(ROOT)}")
    print(f"\n[{mode}] {total_hits} hit(s) across {touched} file(s).")
    if args.apply:
        print("Changes written. Run the full gate set (build all ESP32 variants, "
              "ctest, scenarios, check_devices, check_specs) before committing.")
        return 0
    # Dry-run: non-zero when there is still something to flip, so a pre-switch
    # check can assert the work isn't done; zero post-switch when clean.
    return 1 if total_hits else 0


if __name__ == "__main__":
    sys.exit(main())
