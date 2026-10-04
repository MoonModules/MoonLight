"""The ratchet every metrics report shares: the committed copy is the number to beat, and it only falls.

A report under docs/reference/metrics/ is tracked, so the last commit's copy is the baseline. A run rewrites the file before it compares, which is why the baseline is read from git rather than from the working tree. The comparison is per rule as well as on the total, because a total hides one rule paying for another, and over the UNION of baseline and current rules, because a rule at zero is absent from the report and iterating the baseline alone let it rise in silence.

check_docgen, check_prose and check_code each parse their own table shape; this is the part they would otherwise copy.
"""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def committed(report: Path) -> str | None:
    """The committed text of a tracked report, or None when there is no baseline yet."""
    rel = report.relative_to(ROOT).as_posix()
    r = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=ROOT, capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def risen(base: dict | None, now: dict) -> list:
    """Every rule whose count rose against the baseline, as (rule, was, now), biggest rise first.

    No baseline means a first run, which records rather than refuses. A rule missing on either side reads as zero.
    """
    if base is None:
        return []
    rows = [(rule, base.get(rule, 0), now.get(rule, 0)) for rule in set(base) | set(now)
            if now.get(rule, 0) > base.get(rule, 0)]
    return sorted(rows, key=lambda r: (r[1] - r[2], r[0]))
