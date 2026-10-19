"""Which steps of a process a change needs, and which of their passes still hold: MoonWalk's rule for the steps that run a script, read by MoonDeck's map and by an agent through MoonDeck's `/api/gates`.

A step is triggered when the change touches the paths its `changed` names, matched as GitHub Actions matches `on.push.paths`. A pass holds while the paths it covers are unchanged since it ran: each run records the working tree it saw as a git tree id, the build system's up-to-date check. Nothing here runs a step.
"""

import json
import os
import re
import shutil
import subprocess
import tempfile
import threading
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DURATIONS = ROOT / "build" / "moondeck-logs" / "durations.json"   # each call's recent passing runs on this machine, beside its run record: a duration is this machine's, as a run is
SLOW_SECONDS = 45   # where the measured runs split: the checks finish within about 30 s, the builds, sweeps and flashes take a minute and more
KEEP = 5            # passing runs kept per call: a clean build and an incremental one both count


def _git(*args, env=None, root=ROOT) -> str:
    return subprocess.run(["git", *args], cwd=root, env=env, capture_output=True, text=True, check=True).stdout.strip()


def in_checkout(root: Path = ROOT) -> bool:
    """Whether `root` is a git working tree, where a run's tree can be recorded."""
    try:
        return _git("rev-parse", "--is-inside-work-tree", root=root) == "true"
    except (OSError, subprocess.CalledProcessError):
        return False


def tree_now(root: Path = ROOT) -> str:
    """The working tree as one git tree id, untracked files included, written through a copy of the index so the real one stays untouched."""
    with tempfile.TemporaryDirectory() as d:
        index = Path(d) / "index"
        real = root / _git("rev-parse", "--git-path", "index", root=root)
        if real.exists():
            shutil.copyfile(real, index)   # its cached file stats keep `add -A` from hashing the whole tree again
        env = {**os.environ, "GIT_INDEX_FILE": str(index)}
        _git("add", "-A", env=env, root=root)
        return _git("write-tree", env=env, root=root)


@lru_cache(maxsize=256)
def _glob(pattern: str) -> re.Pattern:
    """One path glob as a regex: `**` crosses folders, `*` and `?` stay inside one."""
    out, i = "", 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out, i = out + "(?:.*/)?", i + 3
        elif pattern.startswith("**", i):
            out, i = out + ".*", i + 2
        elif pattern[i] == "*":
            out, i = out + "[^/]*", i + 1
        elif pattern[i] == "?":
            out, i = out + "[^/]", i + 1
        else:
            out, i = out + re.escape(pattern[i]), i + 1
    return re.compile(out + r"\Z")


def matches(path: str, globs: list) -> bool:
    """Whether `path` falls under `globs`, the last pattern that matches deciding, as GitHub Actions reads a `!` pattern."""
    hit = False
    for g in globs:
        negated = g.startswith("!")
        if _glob(g[1:] if negated else g).match(path):
            hit = not negated
    return hit


def changed_between(a: str, b: str, root: Path = ROOT) -> list:
    """The paths that differ between two trees."""
    return _git("diff", "--name-only", a, b, root=root).splitlines() if a != b else []


def read_runs(path: Path) -> dict:
    """Each step's last run, by `script args`: the tree it saw, whether it passed and how long it took. A missing or broken file is no runs."""
    try:
        runs = json.loads(path.read_text(encoding="utf-8"))
        return runs if isinstance(runs, dict) else {}
    except (OSError, ValueError):
        return {}


_record_lock = threading.Lock()   # runs end on their own threads: one read-modify-write at a time


def _update(path: Path, change) -> None:
    """Read a record file, change it and write it back whole, through a temporary file renamed into place, so a reader never meets half a file and two runs ending together both land. Best-effort: a failed write loses the record, never the run."""
    with _record_lock:
        data = read_runs(path)
        change(data)
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            tmp = path.with_name(path.name + ".tmp")
            tmp.write_text(json.dumps(data, indent=1, sort_keys=True) + "\n", encoding="utf-8")
            os.replace(tmp, path)
        except OSError:
            pass


def record_run(path: Path, key: str, tree: str, passed: bool) -> None:
    """Keep one run's result on this machine: the tree it saw and whether it passed."""
    _update(path, lambda runs: runs.__setitem__(key, {"tree": tree, "passed": passed}))


def record_finished(path: Path, key: str, passed: bool, root: Path = ROOT) -> None:
    """Keep a finished run's result with the tree it leaves behind, its own outputs (a report, observations, the durations) inside it, as a build counts its outputs newer than its inputs; an edit made during the run counts as seen."""
    record_run(path, key, tree_now(root), passed)


def record_duration(path: Path, key: str, seconds: float) -> None:
    """Add a passing run's seconds to the call's recent runs, the last KEEP of them."""
    def add(durations):
        recent = durations.get(key) if isinstance(durations.get(key), list) else []
        durations[key] = (recent + [round(seconds, 1)])[-KEEP:]
    _update(path, add)


def longest(durations: dict, key: str):
    """The longest of a call's recent passing runs, the answer to "can this take minutes", or None when it was never measured."""
    recent = [x for x in durations.get(key) or [] if isinstance(x, (int, float))]
    return max(recent) if recent else None


def run_key(script: str, args: str = "") -> str:
    """A run's key: the script and the arguments that ran, the call a step names."""
    return f"{script} {args}".strip()


def runnable(run) -> bool:
    """Whether a step's `run` is a call MoonDeck can make as written: a script, and no `<placeholder>` the reader fills in."""
    return isinstance(run, dict) and bool(run.get("script")) and "<" not in run.get("args", "")


def step_run(model: dict, ref: str) -> dict | None:
    """The `run` of the step `process/step` names, or None: a step's call comes from the process file, never from the request that asks for it."""
    pid, _, sid = (ref or "").partition("/")
    step = next((s for s in model.get("processes", {}).get(pid, {}).get("steps", []) if s.get("id") == sid), None)
    run = (step or {}).get("when", {}).get("run")
    return run if runnable(run) else None


def status(model: dict, runs: dict, durations: dict | None = None, root: Path = ROOT, now: str | None = None,
           base: str | None = None, every: bool = False) -> dict:
    """Per process that runs scripts, each step's status: triggered by the change, whether its last pass still holds, and the longest of its recent runs with whether that is slow. A step that runs a subprocess stands for the subprocess's steps, each keyed by its own process and step id, as a call graph is flattened: a process reached twice, or through a cycle, is read once.

    The change is the working tree against HEAD, a commit's; with `base`, against where the branch left it (`main`), a merge's; `every` triggers every step, a release's.
    """
    now = now or tree_now(root)
    since_base = _git("merge-base", base, "HEAD", root=root) if base else "HEAD"
    touched = changed_between(_git("rev-parse", f"{since_base}^{{tree}}", root=root), now, root)
    since = {}   # the paths changed since each recorded run's tree, computed once per tree

    def one(pid: str, s: dict, run: dict) -> dict:
        globs = [x for g in s.get("given", []) if isinstance(g, dict) for x in g.get("changed", [])]
        covers = (lambda paths: any(matches(q, globs) for q in paths)) if globs else bool
        key = run_key(run["script"], run.get("args", ""))
        last = runs.get(key)
        fresh = False
        if last and last.get("passed") and last.get("tree"):
            if last["tree"] not in since:
                try:
                    since[last["tree"]] = changed_between(last["tree"], now, root)
                except subprocess.CalledProcessError:
                    since[last["tree"]] = None   # a tree git has pruned: the pass proves nothing
            fresh = since[last["tree"]] is not None and not covers(since[last["tree"]])
        seconds = longest(durations or {}, key)
        return {"id": s["id"], "process": pid, "script": run["script"], "args": run.get("args", ""), "triggered": every or covers(touched),
                "fresh": fresh, "seconds": seconds, "slow": seconds is not None and seconds > SLOW_SECONDS}

    def steps_of(pid: str, seen: set) -> list:
        if pid in seen or pid not in model["processes"]:
            return []
        seen.add(pid)
        out = []
        for s in model["processes"][pid].get("steps", []):
            when = s.get("when") or {}
            if runnable(when.get("run")):
                out.append(one(pid, s, when["run"]))
            elif isinstance(when.get("process"), str):
                out += steps_of(when["process"], seen)
        return out

    out = {}
    for pid in model["processes"]:
        steps = steps_of(pid, set())
        if steps:
            out[pid] = steps
    return out
