"""A suite's whole output, kept beside the binary that printed it with that binary's hash, so a later reader of the same binary reads it instead of running the suite again.

The test gates run the unit suite and every scenario; the KPI reads the same runs' summary lines and ticks. A run is a function of its binary, so the binary's
hash is the key, as a build cache keys an object on its inputs: a rebuilt binary finds no kept output, and its reader runs the suite itself.

stdlib only and cheap to import, as `_host.py` is, so a PEP-723 script imports it after adding moondeck/ to sys.path.
"""

import hashlib
import json
import sys
from pathlib import Path


def scenario_files() -> list:
    """The scenario definitions a whole scenario run reads, sorted by path: the inputs besides the binary that its output is a function of."""
    return sorted((Path(__file__).resolve().parent.parent / "test" / "scenarios").rglob("scenario_*.json"))


def _record(exe: Path) -> Path:
    return exe.parent / f"{exe.name}.output.json"


def _hash(exe: Path, inputs=()) -> str:
    """The binary's digest and, for each input file in sorted order, its path and contents: what a run is a function of."""
    h = hashlib.sha256()
    for path in (exe, *sorted(inputs)):
        if path is not exe:
            h.update(path.name.encode() + b"\0")
        with path.open("rb") as f:
            for block in iter(lambda: f.read(1 << 20), b""):
                h.update(block)
    return h.hexdigest()


def fingerprint(exe: Path, inputs=()) -> str:
    """The key of a run of `exe` over `inputs` as they are now, taken before the run so a run that rewrites its inputs is not kept under their new text."""
    return _hash(exe, inputs)


def unchanged(exe: Path, inputs, key: str | None) -> bool:
    """Whether `exe` and `inputs` still hash to `key`, taken before a run: a definition edited while the run went is one a scenario did not read, so the run is not kept."""
    try:
        return key is not None and _hash(exe, inputs) == key
    except OSError:
        return False


def keep(exe: Path, output: str, key: str | None = None) -> None:
    """Keep a whole run's output of `exe` under `key`, the binary's own hash by default. Best effort: the reader runs the suite itself when the output is not kept."""
    try:
        _record(exe).write_text(json.dumps({"sha256": key or _hash(exe), "output": output}), encoding="utf-8")
    except OSError as e:
        print(f"Output of {exe.name} not kept: {e}", file=sys.stderr)


def keep_complete(exe: Path, output: str, key: str, summary: str) -> None:
    """Keep a whole run's output when it reached its `summary` line and `exe` still hashes to `key`: a run that died early, or a binary rebuilt meanwhile, is no result for a reader to take."""
    if summary in output and unchanged(exe, (), key):
        keep(exe, output, key)


def kept(exe: Path, inputs=()) -> str | None:
    """The output a whole run of this binary over `inputs` printed, or None when either changed since, or it ran only filtered."""
    try:
        rec = json.loads(_record(exe).read_text(encoding="utf-8"))
        return rec.get("output") if rec.get("sha256") == _hash(exe, inputs) else None
    except (OSError, ValueError):
        return None
