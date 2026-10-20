#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyyaml>=6", "jsonschema>=4"]
# ///
"""Generate the test registry: every unit test and scenario by a derived id, sharded by subject.

The registry is the test engine's index (docs/work/present/Plan-20261020 - Test engine.md, its schema is
tests.schema.json). `index.json` lists the subjects with their counts and `<subject>.json` holds the tests about
one subject, so a page or a device fetches only what it opens, as MoonWalk fetches a process's text.

    uv run moondeck/docs/gen_tests.py --out <dir>
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _test_metadata as meta  # noqa: E402

ROOT = meta.ROOT
UNCATEGORIZED = "Uncategorized"   # a test that names no subject, as the generated pages group it today


def _rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def _subjects(module, also) -> list:
    return list(dict.fromkeys([module or UNCATEGORIZED, *also]))


def _perf(step: dict) -> dict:
    """A measured step's numbers per target: the contract it is held to and the range it was observed in, without the samples."""
    out = {}
    for target in sorted(set(step["contract"]) | set(step["observed"])):
        c = {k: v for k, v in (step["contract"].get(target) or {}).items() if k in ("tick_us", "free_heap", "max_alloc_block")}
        o = {k: {q: v[q] for q in ("min", "p50", "max") if q in v}
             for k, v in (step["observed"].get(target) or {}).items() if isinstance(v, dict) and k in ("tick_us", "free_heap", "max_alloc_block")}
        if c or o:
            out[target] = {"contract": c, "observed": o}
    return out


def _step(s: dict) -> dict:
    entry = {k: s[k] for k in ("name", "op", "description") if s.get(k)}
    if s["measure"]:
        entry["measure"] = True
    if perf := _perf(s):
        entry["perf"] = perf
    return entry


def _controls(scenario: dict) -> list:
    """The `Type.control` pairs a scenario sets or expects, through the instances it declares: a step names an instance, never a type, and a name with no declared type is refused rather than guessed."""
    out = set()
    for s in scenario["steps"]:
        if s["op"] in ("set_control", "expect_control") and s["id"] and s["key"]:
            if s["id"] not in scenario["instances"]:
                raise ValueError(f"{scenario['name']}: step {s['name']} names the instance {s['id']}, which the scenario does not declare in `instances`")
            out.add(f"{scenario['instances'][s['id']]}.{s['key']}")
    return sorted(out)


_NAME_TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"|\w+')


def _case_name_templates(text: str) -> list:
    """Each TEST_CASE's name as its parts in order, the string literals with their quotes and the macros by name: `"the " MM_ISA_NAME " code"` is three parts."""
    out = []
    for m in re.finditer(r"^TEST_CASE\s*\(", text, re.MULTILINE):
        parts, i = [], m.end()
        while True:
            tok = _NAME_TOKEN.match(text, text.index(next(c for c in text[i:] if not c.isspace()), i))
            if not tok:
                break
            parts.append(tok.group(0))
            i = tok.end()
        out.append(parts)
    return out


def registry() -> dict:
    """Every test by its derived id: the kind, the file and the test's own name, so a rename is a new test."""
    tests: dict = {}

    def add(test_id: str, entry: dict) -> None:
        if test_id in tests:
            raise ValueError(f"two tests share the id {test_id}")
        tests[test_id] = entry

    files = meta.collect_unit_files()
    for f in files:
        for (name, description), line, steps in zip(f["cases"], f["case_lines"], f["case_steps"]):
            entry = {"kind": "unit", "size": "small", "title": name, "file": _rel(f["path"]), "line": line,
                     "subjects": _subjects(f["module"], f["also"])}
            if steps:
                entry["steps"] = steps
            if description:
                entry["description"] = description
            if f["module"]:
                entry["run"] = {"script": "moondeck/test/test_desktop.py", "args": f"--module {f['module']}"}
            add(f"unit:{entry['file']}::{name}", entry)
    # A shared body of cases in an .inc file is compiled into each unit file that includes it, once per target: its names are built from a macro the includer defines, so a case is one test per includer, as a parametrized test is.
    for inc in sorted(meta.UNIT_DIR.rglob("*.inc")):
        parsed = meta.parse_unit_file(inc)
        if not parsed["cases"]:
            continue
        template = _case_name_templates(inc.read_text(encoding="utf-8"))
        for u in files:
            try:
                text = u["path"].read_text(encoding="utf-8")
            except OSError:
                continue
            if f'"{inc.name}"' not in text:
                continue
            macros = dict(re.findall(r'#define\s+(\w+)\s+"([^"]*)"', text))
            for (_, description), line, steps, parts in zip(parsed["cases"], parsed["case_lines"], parsed["case_steps"], template):
                name = "".join(macros.get(p, p) if not p.startswith('"') else p[1:-1] for p in parts)
                entry = {"kind": "unit", "size": "small", "title": name, "file": _rel(inc), "line": line, "subjects": _subjects(u["module"], u["also"])}
                if description:
                    entry["description"] = description
                if steps:
                    entry["steps"] = steps
                add(f"unit:{entry['file']}::{name}", entry)
    for f in meta.collect_scenario_files():
        live = f["live_only"]
        entry = {"kind": "live" if live else "scenario", "size": "large" if live else "medium", "title": f["name"],
                 "file": _rel(f["path"]), "subjects": _subjects(f["module"], f["also"]),
                 "sequence": [_step(s) for s in f["steps"]]}
        if controls := _controls(f):
            entry["controls"] = controls
        if f["steps_proved"]:
            entry["steps"] = f["steps_proved"]
        if f["description"]:
            entry["description"] = f["description"]
        entry["run"] = ({"script": "moondeck/scenario/run_live_scenario.py", "args": f"--name {f['name']}"} if live
                        else {"script": "moondeck/scenario/run_scenario.py", "args": f"--name {f['name']}"})
        add(f"{entry['kind']}:{f['name']}", entry)
    return tests


def _help() -> dict:
    return json.loads((ROOT / "docs" / "help" / "help.json").read_text(encoding="utf-8"))


def group_of(subject: str, tests: dict, help_json: dict) -> str:
    """The category a subject is listed under: the page and group its help puts a module in ("Light · Effects", "Core · Services"), else the domain its tests live in ("Core · Internals")."""
    page = help_json["modules"].get(subject, {}).get("doc", "").split("#")[0]
    if page in help_json["catalogs"]:
        return f"{page.split('/')[0].capitalize()} \u00b7 {help_json['catalogs'][page]['title']}"
    domain = next((d for f in sorted(e["file"] for e in tests.values()) for d in ("core", "light") if f"/{d}/" in f), None)
    return f"{domain.capitalize()} \u00b7 Internals" if domain else "Other"


def by_control(tests: dict | None = None) -> dict:
    """The scenarios that set or expect each `Type.control`: id and the subject whose page lists it, for the help of that control."""
    out: dict = {}
    for test_id, entry in (tests if tests is not None else registry()).items():
        for control in entry.get("controls", []):
            out.setdefault(control, []).append({"id": test_id, "title": entry["title"], "subject": entry["subjects"][0]})
    return {c: sorted(v, key=lambda x: x["title"]) for c, v in out.items()}


def step_tests(processes: dict, tests: dict | None = None) -> dict:
    """The scenarios that prove each process step, by the key they share: a step that sets a control or expects its value, and a scenario that sets or expects the same `Type.control`. Each is a title and the test page's address, relative to the site."""
    tests = tests if tests is not None else registry()
    found = by_control(tests)
    named: dict = {}   # the tests that name a step themselves, which come before the ones that only share its control
    for test_id, entry in tests.items():
        for step in entry.get("steps", []):
            named.setdefault(step, []).append({"id": test_id, "title": entry["title"], "subject": entry["subjects"][0]})
    out: dict = {}
    for pid, p in processes.items():
        for s in p.get("steps", []):
            keys = [f"{e['module']}.{e['control']}" for e in s.get("then", []) if "control" in e]
            arg = next(iter(s["when"].values()))
            if isinstance(arg, dict) and "control" in arg and "module" in arg:
                keys.append(f"{arg['module']}.{arg['control']}")
            hits = list({h["id"]: h for h in [*named.get(f"{pid}/{s['id']}", []), *(h for k in dict.fromkeys(keys) for h in found.get(k, []))]}.values())
            if hits:
                out[f"{pid}/{s['id']}"] = [{"title": h["title"], "href": f"reference/tests.html#{h['subject'].lower()}:{h['id']}"} for h in hits[:3]]
    return out


def unknown_steps(processes: dict, tests: dict | None = None) -> list:
    """The tests that name a process step no process has: a renamed step, or a typo, which fails the build rather than leaving a dead claim."""
    steps = {f"{pid}/{s['id']}" for pid, p in processes.items() for s in p.get("steps", [])}
    return [f"{test_id}: names the step {step}, which no process has"
            for test_id, entry in (tests if tests is not None else registry()).items() for step in entry.get("steps", []) if step not in steps]


DEVICE_VERBS = {"add", "set", "replace", "delete", "clear", "apply"}   # the verbs that change the device: a test can prove them


def gaps(processes: dict, help_json: dict, tests: dict | None = None) -> dict:
    """What no test covers: the device-changing steps with none, the help's module types with none, and the controls a scenario sets that the help does not know."""
    tests = tests if tests is not None else registry()
    covered = step_tests(processes, tests)
    steps = []
    for pid, p in processes.items():
        for s in p.get("steps", []):
            verb, arg = next(iter(s["when"].items()))
            writes = verb in DEVICE_VERBS or (verb == "call" and arg.get("method") not in (None, "GET") and "body" in arg)
            if writes and f"{pid}/{s['id']}" not in covered:
                steps.append({"step": f"{pid}/{s['id']}", "say": s["say"], "verb": verb})
    tested = {subject for e in tests.values() for subject in e["subjects"]}
    modules = sorted(m for m in help_json["modules"] if m not in tested)
    unknown = sorted({c for c in by_control(tests) if c.split(".", 1)[1] not in help_json["modules"].get(c.split(".", 1)[0], {}).get("controls", {})})
    return {"steps": steps, "modules": modules, "unknownControls": unknown}


def write_steps(out: Path) -> None:
    """`steps.json` under `out`: the tests of each step, for the map to show."""
    import gen_moonwalk
    out.mkdir(parents=True, exist_ok=True)
    processes = gen_moonwalk.build()["processes"]
    (out / "steps.json").write_text(json.dumps(step_tests(processes), ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    (out / "gaps.json").write_text(json.dumps(gaps(processes, _help()), ensure_ascii=False, separators=(",", ":")), encoding="utf-8")


def shards(tests: dict, help_json: dict | None = None) -> tuple[dict, dict]:
    """The index of subjects with their counts per kind, and a shard per subject holding the tests about it."""
    by_subject: dict = {}
    for test_id, entry in tests.items():
        for subject in entry["subjects"]:
            by_subject.setdefault(subject, {})[test_id] = entry
    help_json = help_json or _help()
    index = {"subjects": {s: {"group": group_of(s, t, help_json),
                              "kinds": {k: sum(1 for e in t.values() if e["kind"] == k) for k in sorted({e["kind"] for e in t.values()})}}
                          for s, t in sorted(by_subject.items())},
             "tests": len(tests)}
    return index, {s: {"subject": s, "tests": dict(sorted(t.items()))} for s, t in sorted(by_subject.items())}


def text() -> str:
    """Every test's name under its group and its first subject, as Markdown: what search indexes and a reader without JavaScript reads."""
    index, by_subject = shards(registry())
    out = []
    for group in sorted({s["group"] for s in index["subjects"].values()}):
        out.append(f"## {group}\n")
        for subject, shard in by_subject.items():
            if index["subjects"][subject]["group"] != group:
                continue
            out.append(f"### {subject}\n")
            out += [f"- {t['title']}" for t in shard["tests"].values() if t["subjects"][0] == subject]
            out.append("")
    return "\n".join(out)


def file_name(subject: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", subject) + ".json"


def write(out: Path) -> int:
    """index.json and a shard per subject under `out`, each swapped in whole and the stale ones removed after."""
    index, by_subject = shards(registry())
    index["files"] = {s: file_name(s) for s in by_subject}
    out.mkdir(parents=True, exist_ok=True)
    for subject, shard in by_subject.items():
        tmp = out / (file_name(subject) + ".tmp")
        tmp.write_text(json.dumps(shard, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        tmp.replace(out / file_name(subject))
    for old in out.glob("*.json"):
        if old.name not in index["files"].values() and old.name not in ("index.json", "steps.json", "gaps.json"):
            old.unlink()
    (out / "index.json").write_text(json.dumps(index, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    return len(by_subject)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, required=True, help="the directory to write index.json and the shards into")
    ap.add_argument("--steps-only", action="store_true", help="write only steps.json, the tests of each process step")
    args = ap.parse_args()
    if args.steps_only:
        write_steps(args.out)
        return 0
    n = write(args.out)
    write_steps(args.out)
    print(f"gen_tests: {n} subjects, {json.loads((args.out / 'index.json').read_text())['tests']} tests under {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
