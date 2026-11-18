"""A suite's whole output, kept beside the binary that printed it with that binary's hash, so a later reader of the same binary reads it instead of running the suite again.

The test gates run the unit suite and every scenario; the KPI reads the same runs' summary lines and ticks. A run is a function of its binary, so the binary's
hash is the key, as a build cache keys an object on its inputs: a rebuilt binary finds no kept output, and its reader runs the suite itself.

stdlib only and cheap to import, as `_host.py` is, so a PEP-723 script imports it after adding moondeck/ to sys.path.
"""

import hashlib
import json
from pathlib import Path


def _record(exe: Path) -> Path:
    return exe.parent / f"{exe.name}.output.json"


def _hash(exe: Path) -> str:
    h = hashlib.sha256()
    with exe.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def keep(exe: Path, output: str) -> None:
    """Keep a whole run's output of `exe`, keyed on the binary as it is now."""
    _record(exe).write_text(json.dumps({"sha256": _hash(exe), "output": output}), encoding="utf-8")


def kept(exe: Path) -> str | None:
    """The output a whole run of this binary printed, or None when it was rebuilt since or ran only filtered."""
    try:
        rec = json.loads(_record(exe).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return rec.get("output") if rec.get("sha256") == _hash(exe) else None
