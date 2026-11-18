#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Run the gates a change triggers, once each, and learn from every run: the commit or merge gate the MoonWalk process names.

The trigger table is the YAML: each gate of `moonwalk/13-document-commit.yaml` and `14-merge.yaml` carries its `given: changed:` globs
and the script it runs, and a step naming a `process` brings that process's gates in. This runner reads that table instead of an
agent typing the chain by hand:

- **Which gates.** A gate runs when a file it names changed, matched by `walk_status.matches` (the matcher MoonDeck's map uses) against
  HEAD for a commit and against the merge base with origin/main (else main) for a merge. A gate naming no files runs on every change.
- **Generated first.** A step that `generates` a committed file (help.json) runs before every other gate of a commit, so the gates read
  and hash the file the source says, and its rewrite is left unstaged for the reader; a merge leaves it out.
- **Once.** A gate that passed is cached under a hash of its inputs: HEAD, its files' diff, its script with the modules it imports, and the built outputs it checks. A rerun after a fix runs
  only the gates whose inputs moved; only a pass is cached.
- **Scheduled by need, as make does.** A gate starts once the gates it `needs` finished, holding the job budget's tokens its `cores`
  names (`_budget.py`, GNU make's jobserver protocol), taken all at once or not at all: a compile takes every core, the Python suite and the KPI too (the KPI so no
  gate beside it skews a timing), a step naming no `cores` one. Its count reaches its builds (`CMAKE_BUILD_PARALLEL_LEVEL`) and its test workers.
  The ready gate on the longest path to the end goes first, each gate's time read from the earlier records, as critical-path list
  scheduling does; a gate that does not fit holds back the ones behind it, so a compile is never starved. All of it at low priority.
- **Reported.** One line per gate, PASS, FAIL, CACHED or SKIP with the reason, and a line each minute naming what runs. A step that is
  judgment rather than a script is listed for the reader, since a judgment gate stays silent until someone asks.
- **Plan-Do-Check-Act.** Each run writes a record to `build/gates/`: per gate its trigger, start, time, the load as it started, the
  objects it compiled, its result and output's end. The run ends with proposals drawn from the records: a ratchet that rose, as an edit
  that bypassed the write-time hook when the hook reports the rule per file and as a design question when only the whole tree shows it;
  a gate whose time grew, with its causes from the record; a gate that passes every run. The product owner picks which become a rule or
  a plan step.
"""

from __future__ import annotations

import argparse
import ast
import datetime as dt
import hashlib
import json
import os
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
RECORDS = ROOT / "build" / "gates"
STAGES = ("moonwalk/10-develop.yaml", "moonwalk/13-document-commit.yaml", "moonwalk/14-merge.yaml")
GUESS_S = 10.0   # a gate's time before any record has one, for its place in the schedule
TAIL = 40   # output lines a record keeps per gate
COMPILED = re.compile(r"Building (?:CXX|C|ASM) object")   # the line make and ninja print per object compiled, CMake's wording for both
RUN_START = time.monotonic()   # a gate's start is recorded from here, so the record shows which gates ran beside each other

sys.path.insert(0, str(ROOT / "moondeck"))
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
from _budget import JobBudget  # noqa: E402
from walk_status import matches  # noqa: E402


# ---------------------------------------------------------------------------------------------------------------------------------
# The trigger table

def processes() -> dict:
    """Every process of the stages a gate run reads, by id."""
    out = {}
    for stage in STAGES:
        for p in yaml.safe_load((ROOT / stage).read_text(encoding="utf-8")).get("processes", []):
            out[p["id"]] = p
    return out


def gates(process_id: str, procs: dict, globs: tuple = (), seen: frozenset = frozenset()) -> list[dict]:
    """The steps of `process_id` and the processes it names, in order: a script gate with its globs, else a step for the reader.

    A process a step names inherits that step's globs, as `tests` hands its trigger to the host-tests process; each step carries its own `ci` mark.
    """
    out = []
    for step in procs[process_id]["steps"]:
        conditions = step.get("given") or []
        given = tuple(g for c in conditions if isinstance(c, dict) for g in c.get("changed", [])) or globs
        when = step.get("when") or {}
        gid = f"{process_id}/{step['id']}"
        unread = [c for c in conditions if not (isinstance(c, dict) and set(c) == {"changed"})]   # an input or a bare string: no script can evaluate it
        if unread and ("process" in when or "run" in when):
            out.append({"id": gid, "judgment": f"{step.get('say', step['id'])} (when {', '.join(map(str, unread))}: a condition for the reader)"})
        elif "process" in when and when["process"] in procs and when["process"] not in seen:
            out += gates(when["process"], procs, given, seen | {process_id})
        elif "run" in when and "script" in when["run"]:
            out.append({"id": gid, "script": when["run"]["script"], "args": when["run"].get("args", ""), "globs": list(given), "ci": bool(step.get("ci")),
                        "generates": step.get("generates"), "cores": step.get("cores", 1), "outputs": step.get("outputs", []),
                        "needs": [n if "/" in n else f"{process_id}/{n}" for n in step.get("needs", [])]})
        elif "run" in when and "command" in when["run"]:
            out.append({"id": gid, "judgment": f"{step.get('say', step['id'])} (`{when['run']['command']}`)"})
        else:
            out.append({"id": gid, "judgment": step.get("say", step["id"])})
    return out


def changed_files(base: str, globs: list[str]) -> list[str]:
    """The files under `globs` that differ from `base`, the working tree and the index included, and the untracked ones.

    The globs are matched by `walk_status.matches`, the one matcher MoonDeck's map reads too: no globs names every file.
    """
    diff = _git("diff", "--name-only", "-z", base)
    new = _git("ls-files", "-z", "--others", "--exclude-standard")
    return sorted(f for f in set(filter(None, diff.split("\0"))) | set(filter(None, new.split("\0"))) if not globs or matches(f, globs))


def input_hash(gate: dict, base: str) -> str:
    """What a gate's pass depends on: HEAD, the contents of its files that differ from `base` (a deleted one by its name), its script and its arguments."""
    h = hashlib.sha256()
    h.update(_git("rev-parse", "HEAD").encode())
    for f in changed_files(base, gate["globs"]):
        p = ROOT / f
        h.update(f.encode() + (p.read_bytes() if p.is_file() else b"\0deleted"))
    for module in imported_scripts(ROOT / gate["script"]):   # the script and every module it imports from beside it, whose change moves its result
        h.update(module.read_bytes() if module.is_file() else str(module).encode())
    h.update(gate["args"].encode())
    return h.hexdigest()


def imported_scripts(script: Path) -> list[Path]:
    """The script and, transitively, the modules it imports that live beside it or in moondeck/ (_findings, _ratchet, check_nonblocking), by path."""
    found, todo = set(), [script]
    while todo:
        path = todo.pop()
        if path in found:
            continue
        found.add(path)
        if not path.is_file():
            continue
        names = []
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"))
        except (SyntaxError, ValueError):
            continue   # a script that does not parse is hashed by its bytes alone, so its gate runs and fails with its own traceback
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                names += [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
                names.append(node.module)
        todo += [m for n in names for d in (script.parent, ROOT / "moondeck") if (m := d / (n.split(".")[0] + ".py")).is_file()]
    return sorted(found)


def outputs_state(gate: dict) -> str:
    """The built outputs a gate's step declares (`outputs:`, globs from the repo root, `{host_build}` for the host's build directory), as they are now.

    A gate that passed for the outputs it saw reruns once one is deleted or replaced, as make rebuilds a target whose file is gone or newer.
    """
    if not gate.get("outputs"):
        return ""
    sys.path.insert(0, str(ROOT / "moondeck" / "build"))
    from build_host import host_build_dir  # noqa: PLC0415  (the build directory has one home)
    files = sorted(f for pattern in gate["outputs"] for f in ROOT.glob(pattern.replace("{host_build}", host_build_dir())) if f.is_file())
    return ",".join(f"{f.relative_to(ROOT).as_posix()}:{f.stat().st_size}:{f.stat().st_mtime_ns}" for f in files) or "none"


def cache_key(gate: dict) -> str:
    """What a cached pass is compared to: the gate's inputs and the outputs it left, taken after it ran."""
    return f"{gate['hash']}:{outputs_state(gate)}"


def _git(*args: str) -> str:
    """Git's answer; a failed command stops the run, since an empty answer would read as an empty diff and skip every gate."""
    proc = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"git {' '.join(args[:2])} failed: {proc.stderr.strip()[:200]}")
    return proc.stdout



def merge_base() -> str:
    """Where a merge's diff starts: the pull request's base, `origin/main`, else the local `main`, which a clone may hold stale or lack."""
    for ref in ("origin/main", "main"):
        try:
            return _git("merge-base", ref, "HEAD").strip()
        except RuntimeError:
            continue
    raise RuntimeError("git merge-base found neither origin/main nor main")

# ---------------------------------------------------------------------------------------------------------------------------------
# A run

def run_gate(gate: dict, budget: JobBudget, logdir: Path, cores: int = 1) -> dict:
    """Run one gate niced on `cores` jobs, its output to its log; its result, start, time, the load as it started, the objects it compiled
    and its output's end."""
    log = logdir / (gate["id"].replace("/", "__") + ".log")
    start = time.monotonic()
    load = _load_average()
    env = {**budget.env(), "CMAKE_BUILD_PARALLEL_LEVEL": str(cores)}   # build_jobs() reads it, for the builds and the test workers alike
    nice = ["nice", "-n", "10"] if shutil.which("nice") else []   # the `nice` program: a preexec_fn can deadlock a child forked from a worker thread
    with log.open("w", encoding="utf-8") as out:
        proc = subprocess.run([*nice, "uv", "run", gate["script"], *shlex.split(gate["args"])], cwd=ROOT, env=env,
                              stdout=out, stderr=subprocess.STDOUT, check=False)
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    return {"result": "PASS" if proc.returncode == 0 else "FAIL", "start": round(start - RUN_START, 1), "seconds": round(time.monotonic() - start, 1),
            "cores": cores, "load": load, "compiled": sum(1 for ln in lines if COMPILED.search(ln)), "exit": proc.returncode,
            "finding": _first_finding(lines), "rises": rises(lines), "tail": lines[-TAIL:], "log": str(log.relative_to(ROOT))}


def attempt(gate: dict, budget: JobBudget, logdir: Path, cores: int = 1) -> dict:
    """`run_gate`, a gate that cannot run (no `uv`, an unwritable log) recorded as a FAIL with the reason, so its dependents skip and the run exits nonzero."""
    start = time.monotonic()
    try:
        return run_gate(gate, budget, logdir, cores)
    except Exception as e:   # OSError from the launch or the log, whatever else a gate's run raises
        return {"result": "FAIL", "start": round(start - RUN_START, 1), "seconds": round(time.monotonic() - start, 1), "cores": cores, "exit": None,
                "finding": f"could not run: {type(e).__name__}: {e}"[:160], "tail": []}


def _load_average() -> float | None:
    try:
        return round(os.getloadavg()[0], 1)
    except (OSError, AttributeError):   # Windows has none
        return None


def _picked(gate: dict, only: set | None) -> bool:
    """Whether `--only` leaves the gate in: no filter, its full id, or its last part."""
    return not only or gate["id"] in only or gate["id"].split("/")[-1] in only


def reads(gate: dict, path: str) -> bool:
    """Whether the file at `path` is one of the files the gate's globs name."""
    return not gate["globs"] or matches(path, gate["globs"])


def select(process_id: str, base: str, use_cache: bool, only: set | None) -> tuple[list, list, list]:
    """The gates to run, the ones skipped with their reason, and the judgment steps; a merge leaves out what CI runs, and runs a script once."""
    cache = _load(RECORDS / "cache.json", {})
    run, skipped, judgment, commands = [], [], [], set()
    merge = process_id == "pre-merge"
    for g in gates(process_id, processes()):
        if "judgment" in g:
            judgment.append(g)
            continue
        if g.get("generates"):   # run before the selection, by generate(), or left out of a merge
            continue
        if not _picked(g, only):
            continue
        if merge and g.get("ci"):
            skipped.append({**g, "result": "SKIP", "why": "CI runs it on every push: read `gh pr checks`"})
            continue
        command = (g["script"], g["args"])
        if command in commands:
            skipped.append({**g, "result": "SKIP", "why": "the same command runs earlier in this process"})
            continue
        commands.add(command)
        if "<" in g["args"]:
            skipped.append({**g, "result": "SKIP", "why": f"needs a value the reader gives ({g['args']})"})
            continue
        hit = changed_files(base, g["globs"]) if g["globs"] else ["(every change)"]
        if not hit:
            skipped.append({**g, "result": "SKIP", "why": "no file it reads changed"})
            continue
        g["trigger"] = hit[:5] + ([f"and {len(hit) - 5} more"] if len(hit) > 5 else [])
        g["hash"] = input_hash(g, base)
        if use_cache and cache.get(g["id"]) == cache_key(g):
            skipped.append({**g, "result": "CACHED", "why": "its inputs are unchanged since it passed"})
            continue
        run.append(g)
    return run, skipped, judgment


def generators(process_id: str, base: str, only: set | None = None) -> list:
    """The steps writing a committed file whose sources changed; a merge has none, since a stale file there is a failure to fix.

    A filtered run keeps the generators it names, and the ones a kept gate depends on, as make builds only the prerequisites of the target asked
    for: a gate that `needs` the generator, or whose globs name the generated file.
    """
    if process_id == "pre-merge":
        return []
    every = gates(process_id, processes())
    wanted = [g for g in every if "judgment" not in g and not g.get("generates") and _picked(g, only)]
    return [g for g in every if g.get("generates") and changed_files(base, g["globs"])
            and (not only or _picked(g, only) or any(g["id"] in w["needs"] or reads(w, g["generates"]) for w in wanted))]


def generate(process_id: str, base: str, budget: JobBudget, logdir: Path, only: set | None = None) -> list:
    """Each generated file rewritten before any gate reads it or hashes its diff, as make builds a target before what reads it."""
    made = []
    for g in generators(process_id, base, only):
        g.update(attempt(g, budget, logdir))
        if g["result"] == "PASS" and _git("diff", "--name-only", "--", g["generates"]).strip():
            g["finding"] = f"{g['generates']} rewritten, unstaged"
        print(_line(g) + (f": {g['finding']}" if g["result"] == "PASS" and g.get("finding") else ""), flush=True)
        made.append(g)
    return made


def cores(gate: dict, budget: JobBudget) -> int:
    """The tokens a gate holds: its `cores`, `all` being the whole budget."""
    want = gate.get("cores", 1)
    return budget.tokens if want == "all" else max(1, min(int(want), budget.tokens))


def priorities(run: list, history: list[dict]) -> dict:
    """Each gate's longest path to the end of the run, in seconds: its own median time from the records plus the longest path of the gates
    that need it, the measure critical-path list scheduling orders by."""
    est = {}
    for g in run:
        past = [h["seconds"] for r in history for h in r["gates"] if h["id"] == g["id"] and h["result"] == "PASS" and h.get("seconds")]
        est[g["id"]] = statistics.median(past) if past else GUESS_S
    memo = {}

    def path(gid: str) -> float:
        if gid not in memo:
            memo[gid] = 0.0   # a cycle reads as the end, so a needs loop cannot recurse forever
            memo[gid] = est[gid] + max((path(d["id"]) for d in run if gid in d.get("needs", [])), default=0.0)
        return memo[gid]

    return {g["id"]: path(g["id"]) for g in run}


def unmet_needs(gate: dict, done: list) -> list[str]:
    """The gate's needs that finished without passing."""
    return sorted({g["id"] for g in done if g["result"] != "PASS"} & set(gate["needs"]))


def execute(run: list, budget: JobBudget, logdir: Path, history: list[dict] = (), every: float = 60.0, generated: list = ()) -> list:
    """Start each gate once its needs finished and its tokens are free, the longest path first; a progress line each `every` seconds.

    `generated` are the generators that ran before: a failed one skips the gates that need it, a passed one releases them.
    """
    ids, prio = {g["id"] for g in run}, priorities(run, list(history))
    pending = sorted(run, key=lambda g: -prio[g["id"]])   # stable: the process's order among equals
    done, running, cond = [], {}, threading.Condition()

    def work(g, n):
        try:
            g.update(attempt(g, budget, logdir, n))
        finally:
            with cond:
                running.pop(g["id"])
                done.append(g)
                for _ in range(n):
                    budget.release()
                cond.notify()
        print(_line(g), flush=True)

    started = last = time.monotonic()
    with cond:
        while pending or running:
            while skip := [g for g in pending if unmet_needs(g, [*generated, *done])]:   # down the chain: a skipped gate skips the gates that need it
                for g in skip:
                    pending.remove(g)
                    g.update(result="SKIP", why=f"needs {', '.join(unmet_needs(g, [*generated, *done]))}, which did not pass")
                    done.append(g)
                    print(_line(g), flush=True)
            passed = {g["id"] for g in done if g["result"] == "PASS"}
            for g in [g for g in pending if all(n in passed or n not in ids for n in g["needs"])]:
                if not budget.try_acquire(n := cores(g, budget)):
                    break   # it waits for its tokens, and the gates behind it wait with it
                pending.remove(g)
                running[g["id"]] = g
                threading.Thread(target=work, args=(g, n), daemon=True).start()
            if pending and not running and not any(all(n in passed or n not in ids for n in g["needs"]) for g in pending):
                for g in pending:   # what is left needs itself, through a loop in the YAML: a configuration error, said and failed rather than waited on
                    g.update(result="FAIL", seconds=0.0, finding="its needs form a loop")
                    done.append(g)
                    print(_line(g), flush=True)
                break
            if not (pending or running):
                break   # the skips above emptied the run
            cond.wait(timeout=max(0.1, every - (time.monotonic() - last)))
            if time.monotonic() - last >= every and (pending or running):
                last = time.monotonic()
                failed = [g["id"] for g in done if g["result"] == "FAIL"]
                print(f"… {int(last - started)} s: {len(done)} of {len(run)} done, running {', '.join(sorted(running)) or '-'}"
                      + (f", failed {', '.join(failed)}" if failed else ""), flush=True)
    return done


def _line(g: dict) -> str:
    if g["result"] in ("PASS", "FAIL"):
        why = "" if g["result"] == "PASS" else f": {g['finding']}"
        return f"{g['result']:<6} {g['id']} ({g['seconds']} s){why}"
    return f"{g['result']:<6} {g['id']}: {g['why']}"


def _first_finding(tail: list) -> str:
    for ln in tail:
        if re.search(r"ROSE|ERROR|error|FAIL|failed", ln):
            return ln.strip()[:160]
    return (tail[-1].strip() if tail else "no output")[:160]


# ---------------------------------------------------------------------------------------------------------------------------------
# Plan-Do-Check-Act

RISE = re.compile(r"^\s+(.+?): (\d+) -> (\d+)( \[whole tree\])?$")   # _ratchet.rise_line's shape


def rises(tail: list) -> list[tuple]:
    """The ratchet rises a check printed under its ROSE heading: (rule, before, after, whether only the whole tree shows the rule)."""
    out, inside = [], False
    for ln in tail:
        if "ROSE against the committed counts" in ln:
            inside = True
        elif inside and (m := RISE.match(ln)):
            out.append((m.group(1), int(m.group(2)), int(m.group(3)), bool(m.group(4))))
        elif inside and ln.strip() and not ln.startswith("  "):
            inside = False
    return out


def rise_proposal(gid: str, rule: str, was: int, now: int, whole_tree: bool) -> str:
    """What a ratchet rise asks for: a rule the hook reports per file rose past it, so the edit bypassed the hook; a rule only the whole
    tree shows needs a design that keeps it down, or the merge gate holding it."""
    if whole_tree:
        return (f"{gid}: {rule} rose {was} -> {now}, a rule only the whole tree shows, so no write-time check sees it: "
                f"propose the design change that keeps it from rising, or the merge gate holding it.")
    return (f"{gid}: {rule} rose {was} -> {now}, a rule the write-time hook (moondeck/check/hook_written.py) reports per file: "
            f"the edit that raised it bypassed the hook, or left its finding unfixed.")


def slower_causes(g: dict, record: dict) -> list[str]:
    """What the record says made a gate slower: the objects it compiled, the gates beside it, the load as it started, the headers the diff touched."""
    out = [f"it compiled {g['compiled']} objects"] if g.get("compiled") else []
    if g.get("start") is not None:
        end = g["start"] + g["seconds"]
        beside = [h["id"] for h in record["gates"] if h is not g and h.get("start") is not None
                  and h["start"] < end and h["start"] + h["seconds"] > g["start"]]
        out += [f"beside it ran {', '.join(beside)}"] if beside else []
    out += [f"the load was {g['load']} as it started"] if g.get("load") is not None and g["load"] > (os.cpu_count() or 1) else []
    headers = [f for f in g.get("trigger") or [] if f.endswith(".h")]
    out += [f"the diff touches {', '.join(headers)}"] if headers else []
    return out


def proposals(record: dict, history: list[dict]) -> list[str]:
    """What this run and the earlier ones suggest improving: a ratchet rise, a gate grown slower, a gate that passes every run."""
    out = []
    for g in record["gates"]:
        for rule, was, now, *whole in g.get("rises") or []:
            if rule != "(total)":   # the total rises with a rule, which is the proposal
                out.append(rise_proposal(g["id"], rule, was, now, bool(whole and whole[0])))
        if g["result"] in ("PASS", "FAIL"):
            past = [h["seconds"] for r in history[-5:] for h in r["gates"] if h["id"] == g["id"] and h["result"] == "PASS"]
            if len(past) >= 3 and g["seconds"] > 1.5 * statistics.median(past) and g["seconds"] - statistics.median(past) > 10:
                why = "; ".join(slower_causes(g, record)) or "the record names no cause: find what it now reads or does more"
                out.append(f"{g['id']} took {g['seconds']} s, up from a median of {statistics.median(past)} s: {why}.")
    ran = {}
    for r in history + [record]:
        for h in r["gates"]:
            if h["result"] in ("PASS", "FAIL"):
                ran.setdefault(h["id"], []).append(h["result"])
    for gid, results in sorted(ran.items()):
        if len(results) >= 10 and all(x == "PASS" for x in results[-10:]):
            out.append(f"{gid} passed its last {len(results[-10:])} runs: propose whether CI or the merge gate alone can hold it.")
    return out


def update_cache(cache: dict, done: list, base: str) -> None:
    """Cache each gate that passed, and drop the entry of one that did not."""
    for g in done:
        if g["result"] != "PASS":
            cache.pop(g["id"], None)   # a failure is retried and a skip is unrun, whatever passed under the same inputs before
        elif g.get("hash") and input_hash(g, base) == g["hash"]:   # a file edited while the gate ran makes its pass describe a version gone already
            cache[g["id"]] = cache_key(g)


def _load(path: Path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def history(limit: int = 20) -> list[dict]:
    """The earlier records, oldest first."""
    return [r for p in sorted(RECORDS.glob("run-*.json"))[-limit:] if (r := _load(p, None))]


# ---------------------------------------------------------------------------------------------------------------------------------

def announce(process_id: str, base: str, run: list, skipped: list, judgment: list) -> None:
    """The gate list written out whole before any gate runs: the counts, each skip with its reason, each gate to run, each step for the reader."""
    print(f"Gates of {process_id} against {base[:12]}: {len(run)} to run, {len(skipped)} skipped, {len(judgment)} for the reader.")
    for g in skipped:
        print(_line(g))
    for g in run:
        after = f", after {', '.join(g['needs'])}" if g["needs"] else ""
        print(f"RUN    {g['id']} ({g['cores']} core{'s' * (g['cores'] != 1)}{after}): {', '.join(g['trigger'])}")
    for g in judgment:
        print(f"READER {g['id']}: {g['judgment']}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--process", default="commit", help="the MoonWalk process whose gates run: commit (default) or pre-merge")
    ap.add_argument("--no-cache", action="store_true", help="run every triggered gate, passed before or not")
    ap.add_argument("--only", help="comma-separated gate ids (commit-docs/docgen) or step ids (docgen) to run, of those triggered")
    ap.add_argument("--list", action="store_true", help="say which gates would run and why, and run none")
    args = ap.parse_args()

    try:
        base = "HEAD" if args.process != "pre-merge" else merge_base()
    except RuntimeError as e:   # no main to compare against: every gate would read as untouched
        print(f"No base to compare against: {e}", file=sys.stderr)
        return 2
    only = set(args.only.split(",")) if args.only else None
    if args.list:
        for g in generators(args.process, base, only):
            print(f"GEN    {g['id']}: writes {g['generates']} first")
        run, skipped, judgment = select(args.process, base, not args.no_cache, only)
        announce(args.process, base, run, skipped, judgment)
        return 0

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    logdir = RECORDS / f"run-{stamp}"
    logdir.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    load = _load_average()
    past = history()
    with JobBudget(scheduler=True) as budget:
        print(f"Budget: {budget.tokens} jobs, niced.", flush=True)
        made = generate(args.process, base, budget, logdir, only)   # before the selection, which hashes the generated file's diff
        run, skipped, judgment = select(args.process, base, not args.no_cache, only)
        announce(args.process, base, run, skipped, judgment)
        done = made + execute(run, budget, logdir, past, generated=made)

    cache = _load(RECORDS / "cache.json", {})
    update_cache(cache, done, base)
    (RECORDS / "cache.json").write_text(json.dumps(cache, indent=1), encoding="utf-8")
    record = {"started": stamp, "process": args.process, "base": base, "load": load, "seconds": round(time.monotonic() - start, 1),
              "gates": [{k: g.get(k) for k in ("id", "script", "args", "trigger", "result", "why", "start", "seconds", "cores", "load", "compiled", "exit", "finding",
                                                 "rises", "tail", "log")}
                        for g in done + skipped]}
    (RECORDS / f"run-{stamp}.json").write_text(json.dumps(record, indent=1), encoding="utf-8")

    failed = [g for g in done if g["result"] == "FAIL"]
    print(f"\n{len(done) - len(failed)} passed, {len(failed)} failed, {len(skipped)} skipped in {record['seconds']} s. "
          f"Record: build/gates/run-{stamp}.json")
    if judgment:
        print("For the reader, as the process names them:")
        for g in judgment:
            print(f"  {g['id']}: {g['judgment']}")
    if ideas := proposals(record, past):
        print("Proposed improvements (Plan-Do-Check-Act), for the product owner to pick:")
        for p in ideas:
            print(f"  - {p}")
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as e:   # git could not answer: no gate result would mean anything
        print(e, file=sys.stderr)
        sys.exit(2)
