#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Run the gates a change triggers, once each, and learn from every run: the commit or merge gate the MoonWalk process names.

The trigger table is the YAML: each gate of `moonwalk/13-document-commit.yaml` and `14-merge.yaml` carries its `given: changed:` globs
and the script it runs, and a step naming a `process` brings that process's gates in. This runner reads that table instead of an
agent typing the chain by hand:

- **Which gates.** A gate runs when a file it names changed, read with git's own pathspecs (`:(glob)`, `:(exclude,glob)`) against
  HEAD for a commit and against the merge base with main for a merge. A gate naming no files runs on every change.
- **Once.** A gate that passed is cached under a hash of its inputs: HEAD, its files' diff and its script. A rerun after a fix runs
  only the gates whose inputs moved.
- **In order, within the budget.** Gates that compile or load every core (`HEAVY`) run one at a time in the order the process lists
  them, so a build comes before the tests that read it; the other gates run beside them, each holding a token of the machine's job
  budget (`_budget.py`, GNU make's jobserver protocol), all at low priority.
- **Reported.** One line per gate, PASS, FAIL, CACHED or SKIP with the reason, and a line each minute naming what runs. A step that is
  judgment rather than a script is listed for the reader, since a judgment gate stays silent until someone asks.
- **Plan-Do-Check-Act.** Each run writes a record to `build/gates/`: per gate its trigger, time, result and output's end. The run ends
  with proposals drawn from the records: a ratchet that rose and the write-time check that would have kept it from being written, a
  gate whose time grew, a gate that passes every run. The product owner picks which become a rule or a plan step.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import datetime as dt
import hashlib
import json
import os
import re
import shlex
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
# Gates that compile or keep every core busy, one at a time in the process's order, so a build lands before the tests that read it.
HEAVY = ("moondeck/build/", "moondeck/test/test_host.py", "moondeck/scenario/", "moondeck/check/collect_kpi.py",
         "moondeck/check/check_code.py", "moondeck/test/test_tooling.py --python", "moondeck/docs/build_docs.py")
TAIL = 40   # output lines a record keeps per gate

sys.path.insert(0, str(ROOT / "moondeck"))
from _budget import JobBudget  # noqa: E402


# ---------------------------------------------------------------------------------------------------------------------------------
# The trigger table

def processes() -> dict:
    """Every process of the stages a gate run reads, by id."""
    out = {}
    for stage in STAGES:
        for p in yaml.safe_load((ROOT / stage).read_text(encoding="utf-8")).get("processes", []):
            out[p["id"]] = p
    return out


def gates(process_id: str, procs: dict, globs: tuple = (), seen: frozenset = frozenset(), ci: bool = False) -> list[dict]:
    """The steps of `process_id` and the processes it names, in order: a script gate with its globs, else a step for the reader.

    A process a step names inherits that step's globs and its `ci` mark, as `tests` hands its trigger to the host-tests process.
    """
    out = []
    for step in procs[process_id]["steps"]:
        given = tuple(g for c in step.get("given") or [] for g in c.get("changed", [])) or globs
        on_ci = ci or bool(step.get("ci"))
        when = step.get("when") or {}
        gid = f"{process_id}/{step['id']}"
        if "process" in when and when["process"] in procs and when["process"] not in seen:
            out += gates(when["process"], procs, given, seen | {process_id}, on_ci)
        elif "run" in when and "script" in when["run"]:
            out.append({"id": gid, "script": when["run"]["script"], "args": when["run"].get("args", ""), "globs": list(given), "ci": on_ci})
        elif "run" in when and "command" in when["run"]:
            out.append({"id": gid, "judgment": f"{step.get('say', step['id'])} (`{when['run']['command']}`)"})
        else:
            out.append({"id": gid, "judgment": step.get("say", step["id"])})
    return out


def pathspecs(globs: list) -> list[str]:
    """A gate's globs as git pathspecs: `!pattern` excludes."""
    return [f":(exclude,glob){g[1:]}" if g.startswith("!") else f":(glob){g}" for g in globs] or ["."]


def changed_files(base: str, specs: list[str]) -> list[str]:
    """The files under `specs` that differ from `base`, the working tree and the index included, and the untracked ones."""
    diff = _git("diff", "--name-only", "-z", base, "--", *specs)
    new = _git("ls-files", "-z", "--others", "--exclude-standard", "--", *specs)
    return sorted(set(filter(None, diff.split("\0"))) | set(filter(None, new.split("\0"))))


def input_hash(gate: dict, base: str) -> str:
    """What a gate's pass depends on: HEAD, its files' diff against `base`, their untracked contents, its script and its arguments."""
    specs = pathspecs(gate["globs"])
    h = hashlib.sha256()
    h.update(_git("rev-parse", "HEAD").encode())
    h.update(_git("diff", base, "--", *specs).encode())
    for f in filter(None, _git("ls-files", "-z", "--others", "--exclude-standard", "--", *specs).split("\0")):
        p = ROOT / f
        if p.is_file():
            h.update(f.encode() + p.read_bytes())
    script = ROOT / gate["script"]
    h.update(script.read_bytes() if script.is_file() else gate["script"].encode())
    for helper in sorted(script.parent.glob("_*.py")):   # the helpers a check imports beside it (_findings, _ratchet), whose change moves its result
        h.update(helper.read_bytes())
    h.update(gate["args"].encode())
    return h.hexdigest()


def heavy(gate: dict) -> bool:
    command = f"{gate['script']} {gate['args']}".strip()
    return any(command.startswith(h) or gate["script"].startswith(h) for h in HEAVY)


def _git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=False).stdout


# ---------------------------------------------------------------------------------------------------------------------------------
# A run

def run_gate(gate: dict, budget: JobBudget, logdir: Path) -> dict:
    """Run one gate niced, its output to its log; its result, time and output's end."""
    log = logdir / (gate["id"].replace("/", "__") + ".log")
    start = time.monotonic()
    with log.open("w", encoding="utf-8") as out:
        proc = subprocess.run(["uv", "run", gate["script"], *shlex.split(gate["args"])], cwd=ROOT, env=budget.env(),
                              stdout=out, stderr=subprocess.STDOUT, check=False,
                              preexec_fn=(lambda: os.nice(10)) if hasattr(os, "nice") else None)
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    return {"result": "PASS" if proc.returncode == 0 else "FAIL", "seconds": round(time.monotonic() - start, 1), "exit": proc.returncode,
            "finding": _first_finding(lines), "rises": rises(lines), "tail": lines[-TAIL:], "log": str(log.relative_to(ROOT))}


def select(process_id: str, base: str, use_cache: bool, only: set | None) -> tuple[list, list, list]:
    """The gates to run, the ones skipped with their reason, and the judgment steps; a merge leaves out what CI runs, and runs a script once."""
    cache = _load(RECORDS / "cache.json", {})
    run, skipped, judgment, commands = [], [], [], set()
    merge = process_id == "pre-merge"
    for g in gates(process_id, processes()):
        if "judgment" in g:
            judgment.append(g)
            continue
        if only and g["id"] not in only and g["id"].split("/")[-1] not in only:
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
        hit = changed_files(base, pathspecs(g["globs"])) if g["globs"] else ["(every change)"]
        if not hit:
            skipped.append({**g, "result": "SKIP", "why": "no file it reads changed"})
            continue
        g["trigger"] = hit[:5] + ([f"and {len(hit) - 5} more"] if len(hit) > 5 else [])
        g["hash"] = input_hash(g, base)
        if use_cache and cache.get(g["id"]) == g["hash"]:
            skipped.append({**g, "result": "CACHED", "why": "its inputs are unchanged since it passed"})
            continue
        run.append(g)
    return run, skipped, judgment


def execute(run: list, budget: JobBudget, logdir: Path, every: float = 60.0) -> list:
    """The heavy gates one at a time in order, the others beside them within the budget; a progress line each `every` seconds."""
    done, running, lock = [], set(), threading.Lock()
    implicit = threading.Semaphore(1)   # the jobserver's implicit token: the first job needs none from the pipe

    def one(g):
        took_implicit = implicit.acquire(blocking=False)
        if not took_implicit:
            budget.acquire()
        with lock:
            running.add(g["id"])
        try:
            g.update(run_gate(g, budget, logdir))
        finally:
            with lock:
                running.discard(g["id"])
                done.append(g)
            implicit.release() if took_implicit else budget.release()
        print(_line(g), flush=True)
        return g

    def lane(heavies):
        for g in heavies:
            one(g)

    started = time.monotonic()
    with cf.ThreadPoolExecutor(max_workers=max(2, budget.tokens)) as pool:
        futures = [pool.submit(lane, [g for g in run if heavy(g)])] + [pool.submit(one, g) for g in run if not heavy(g)]
        while not all(f.done() for f in futures):
            cf.wait(futures, timeout=every)
            if not all(f.done() for f in futures):
                with lock:
                    failed = [g["id"] for g in done if g["result"] == "FAIL"]
                    print(f"… {int(time.monotonic() - started)} s: {len(done)} of {len(run)} done, running {', '.join(sorted(running)) or '-'}"
                          + (f", failed {', '.join(failed)}" if failed else ""), flush=True)
        for f in futures:
            f.result()
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

RISE = re.compile(r"^\s+(.+?): (\d+) -> (\d+)$")


def rises(tail: list) -> list[tuple]:
    """The ratchet rises a check printed under its ROSE heading: (rule, before, after)."""
    out, inside = [], False
    for ln in tail:
        if "ROSE against the committed counts" in ln:
            inside = True
        elif inside and (m := RISE.match(ln)):
            out.append((m.group(1), int(m.group(2)), int(m.group(3))))
        elif inside and ln.strip() and not ln.startswith("  "):
            inside = False
    return out


def proposals(record: dict, history: list[dict]) -> list[str]:
    """What this run and the earlier ones suggest improving: a ratchet rise, a gate grown slower, a gate that passes every run."""
    out = []
    for g in record["gates"]:
        for rule, was, now in g.get("rises") or []:
            out.append(f"{g['id']}: {rule} rose {was} -> {now}. Propose the write-time check that keeps it from being written "
                       f"(moondeck/check/hook_written.py), so the next one is fixed as the line is written.")
        if g["result"] in ("PASS", "FAIL"):
            past = [h["seconds"] for r in history[-5:] for h in r["gates"] if h["id"] == g["id"] and h["result"] == "PASS"]
            if len(past) >= 3 and g["seconds"] > 1.5 * statistics.median(past) and g["seconds"] - statistics.median(past) > 10:
                out.append(f"{g['id']} took {g['seconds']} s, up from a median of {statistics.median(past)} s: find what it now reads or does more.")
    ran = {}
    for r in history + [record]:
        for h in r["gates"]:
            if h["result"] in ("PASS", "FAIL"):
                ran.setdefault(h["id"], []).append(h["result"])
    for gid, results in sorted(ran.items()):
        if len(results) >= 10 and all(x == "PASS" for x in results[-10:]):
            out.append(f"{gid} passed its last {len(results[-10:])} runs: propose whether CI or the merge gate alone can hold it.")
    return out


def _load(path: Path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def history(limit: int = 20) -> list[dict]:
    """The earlier records, oldest first."""
    return [r for p in sorted(RECORDS.glob("run-*.json"))[-limit:] if (r := _load(p, None))]


# ---------------------------------------------------------------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--process", default="commit", help="the MoonWalk process whose gates run: commit (default) or pre-merge")
    ap.add_argument("--no-cache", action="store_true", help="run every triggered gate, passed before or not")
    ap.add_argument("--only", help="comma-separated gate ids (commit-docs/docgen) or step ids (docgen) to run, of those triggered")
    ap.add_argument("--list", action="store_true", help="say which gates would run and why, and run none")
    args = ap.parse_args()

    base = "HEAD" if args.process != "pre-merge" else (_git("merge-base", "main", "HEAD").strip() or "main")
    only = set(args.only.split(",")) if args.only else None
    run, skipped, judgment = select(args.process, base, not args.no_cache, only)
    print(f"Gates of {args.process} against {base[:12]}: {len(run)} to run, {len(skipped)} skipped, {len(judgment)} for the reader.")
    for g in skipped:
        print(_line(g))
    if args.list:
        for g in run:
            print(f"RUN    {g['id']}{' (one at a time)' if heavy(g) else ''}: {', '.join(g['trigger'])}")
        return 0

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    logdir = RECORDS / f"run-{stamp}"
    logdir.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    try:
        load = os.getloadavg()[0]
    except (OSError, AttributeError):
        load = None
    with JobBudget() as budget:
        print(f"Budget: {budget.tokens} jobs, niced.", flush=True)
        done = execute(run, budget, logdir)

    cache = _load(RECORDS / "cache.json", {})
    cache.update({g["id"]: g["hash"] for g in done if g["result"] == "PASS"})
    (RECORDS / "cache.json").write_text(json.dumps(cache, indent=1), encoding="utf-8")
    record = {"started": stamp, "process": args.process, "base": base, "load": load, "seconds": round(time.monotonic() - start, 1),
              "gates": [{k: g.get(k) for k in ("id", "script", "args", "trigger", "result", "why", "seconds", "exit", "finding", "rises", "tail", "log")}
                        for g in done + skipped]}
    past = history()
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
    sys.exit(main())
