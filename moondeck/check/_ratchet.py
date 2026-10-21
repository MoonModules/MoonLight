"""The ratchet every counting check shares: the committed counts are the number to beat, and they only fall.

A ratcheted check keeps its counts tracked under docs/reference/metrics/ (`code.json`, `docgen.json`, `prose.json`), and the test page shows its findings. The last commit's counts are the baseline. A run rewrites the file before it compares, which is why the baseline is read from git rather than from the working tree. The comparison is per rule as well as on the total, because a total hides one rule paying for another, and over the UNION of baseline and current rules, because a rule at zero is absent from the counts and iterating the baseline alone let it rise in silence.

check_docgen, check_prose and check_code each count their own rules; recording and comparing the counts is this module.
"""

import json
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def committed(report: Path) -> str | None:
    """The committed text of a tracked file, or None when there is no baseline yet; a git that does not answer raises, since reading it as a first run would let any count pass."""
    rel = report.relative_to(ROOT).as_posix()
    r = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=ROOT, capture_output=True, text=True, timeout=60)
    if r.returncode == 0:
        return r.stdout
    # Absent from HEAD is a first run; any other failure is git's, and raises.
    listed = subprocess.run(["git", "ls-tree", "--name-only", "HEAD", "--", rel], cwd=ROOT,
                            capture_output=True, text=True, timeout=60, check=True)
    if not listed.stdout.strip():
        return None
    raise RuntimeError(f"git show HEAD:{rel} failed: {r.stderr.strip()}")


def committed_counts(path: Path) -> dict | None:
    """The counts the last commit recorded in `path`, or None when there is no baseline yet."""
    text = committed(path)
    return None if text is None else json.loads(text)


def current_counts(path: Path) -> dict | None:
    """The counts the last run wrote in `path` in the working tree, or None when no run wrote them."""
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else None


def write_json(path: Path, doc) -> None:
    """Write `doc` to `path` as JSON, swapped in whole so a reader never meets half a file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def record(path: Path, counts: dict) -> None:
    """Keep this run's counts in `path`, sorted so a diff shows only what moved."""
    write_json(path, dict(sorted(counts.items())))


def risen(base: dict | None, now: dict) -> list:
    """Every rule whose count rose against the baseline, as (rule, was, now), biggest rise first.

    No baseline means a first run, which records rather than refuses. A rule missing on either side reads as zero.
    """
    if base is None:
        return []
    rows = [(rule, base.get(rule, 0), now.get(rule, 0)) for rule in set(base) | set(now)
            if now.get(rule, 0) > base.get(rule, 0)]
    return sorted(rows, key=lambda r: (r[1] - r[2], r[0]))
