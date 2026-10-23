#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyyaml>=6", "jsonschema>=4"]
# ///
"""Generate the test registry: every unit test and scenario by a derived id, sharded by subject.

The registry is the test engine's index, its schema tests.schema.json (the plan that designed it, Test engine,
lives in docs/work while it is open). `index.json` lists the subjects with their counts and `<subject>.json` holds the tests about
one subject, so a page or a device fetches only what it opens, as MoonWalk fetches a process's text. The docs build
publishes them; run as a script, it writes only the tests of each process step, for the preview server.

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
UNCATEGORIZED = "Uncategorized"   # a test that names no subject


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


def _document_types(doc) -> dict:
    """Every instance a state document creates with its type, by name: a node that names its `type`."""
    out = {}
    if isinstance(doc, dict):
        for name, node in doc.items():
            if isinstance(node, dict) and isinstance(node.get("type"), str):
                out[name] = node["type"]
            out |= _document_types(node)
    elif isinstance(doc, list):
        for node in doc:
            out |= _document_types(node)
    return out


def _controls(scenario: dict) -> list:
    """The `Type.control` pairs a scenario sets or expects. A step names an instance, never a type: the type is the one the scenario's last state document gave that name, so an instance swapped for another type is read as it is at that step, and `instances` declares only what a step uses before any document creates it, the modules a device already has. A name with no type is refused rather than guessed, and so is a declaration a document makes redundant, a second home for its type."""
    now, out, unused = dict(scenario["instances"]), set(), set(scenario["instances"])
    for s in scenario["steps"]:
        if twice := sorted(unused & set(_document_types(s.get("document")))):
            raise ValueError(f"{scenario['name']}: `instances` declares {', '.join(twice)}, which step {s['name']} creates before any step uses it; declare only what a step uses before a document creates it")
        now |= _document_types(s.get("document"))
        if s["op"] in ("set_control", "expect_control") and s["id"] and s["key"]:
            unused.discard(s["id"])
            if s["id"] not in now:
                raise ValueError(f"{scenario['name']}: step {s['name']} names the instance {s['id']}, which the scenario does not declare in `instances`")
            out.add(f"{now[s['id']]}.{s['key']}")
    return sorted(out)


_NAME_TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"|\w+')
_SPACE = re.compile(r"\s*")


def _case_name_templates(text: str) -> list:
    """Each TEST_CASE's name as its parts in order, the string literals with their quotes and the macros by name: `"the " MM_ISA_NAME " code"` is three parts."""
    out = []
    for m in re.finditer(r"^TEST_CASE\s*\(", text, re.MULTILINE):
        parts, i = [], m.end()
        while True:
            tok = _NAME_TOKEN.match(text, _SPACE.match(text, i).end())
            if not tok:
                break
            parts.append(tok.group(0))
            i = tok.end()
        out.append(parts)
    return out


_JS_TEST_RE = re.compile(r"^\s*test\(\s*([\"'`])((?:\\.|(?!\1).)*)\1", re.MULTILINE)


def python_tests() -> list:
    """Every `test_*` function under test/python as (file, name, line, first docstring line)."""
    import ast
    out = []
    for path in sorted((ROOT / "test" / "python").glob("test_*.py")):
        for node in ast.parse(path.read_text(encoding="utf-8")).body:
            if isinstance(node, ast.FunctionDef) and node.name.startswith("test_"):
                out.append((_rel(path), node.name, node.lineno, (ast.get_docstring(node) or "").split("\n")[0]))
    return out


def js_tests() -> list:
    """Every top-level `test("title", …)` under test/js as (file, title, line)."""
    out = []
    for path in sorted((ROOT / "test" / "js").rglob("*.test.mjs")):
        text = path.read_text(encoding="utf-8")
        out += [(_rel(path), m.group(2).replace("\\'", "'").replace('\\"', '"'), text[:m.start()].count("\n") + 1) for m in _JS_TEST_RE.finditer(text)]
    return out


def js_titles() -> dict:
    """The ids of the JS tests by title, which node's JUnit file names a test by."""
    out: dict = {}
    for file, title, _ in js_tests():
        out.setdefault(title, []).append(f"js:{file}::{title}")
    return out


def check_scripts() -> list:
    """Every `check_*.py` under moondeck/check as (file, stem, what it checks), the first line of its docstring."""
    import ast
    out = []
    for path in sorted((ROOT / "moondeck" / "check").glob("check_*.py")):
        out.append((_rel(path), path.stem, (ast.get_docstring(ast.parse(path.read_text(encoding="utf-8"))) or "").split("\n")[0]))
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
    for file, name, line, doc in python_tests():
        entry = {"kind": "python", "size": "small", "title": name.removeprefix("test_").replace("_", " "), "file": file, "line": line, "subjects": [Path(file).stem.removeprefix("test_")],
                 "run": {"script": "moondeck/test/test_host.py", "args": "--python"}}
        if doc:
            entry["description"] = doc
        add(f"python:{file}::{name}", entry)
    for file, title, line in js_tests():
        add(f"js:{file}::{title}", {"kind": "js", "size": "small", "title": title, "file": file, "line": line, "subjects": [Path(file).name.removesuffix(".test.mjs")],
                                    "run": {"script": "moondeck/test/test_host.py", "args": "--js"}})
    for file, stem, doc in check_scripts():
        entry = {"kind": "check", "size": "medium", "title": stem.removeprefix("check_").replace("_", " ").capitalize() + " check", "file": file, "subjects": [stem], "run": {"script": file}}
        if doc:
            entry["description"] = doc
        add(f"check:{stem}", entry)
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
    kinds = {e.get("kind") for e in tests.values()}
    if "check" in kinds:   # a check and the Python tests named for it are one subject, listed with the checks
        return "Scripts \u00b7 Checks"
    if kinds == {"python"}:
        return "Scripts \u00b7 Python tests"
    if kinds == {"js"}:
        return "UI \u00b7 JS tests"
    domain = next((d for f in sorted(e["file"] for e in tests.values()) for d in ("core", "light") if f"/{d}/" in f), None)
    return f"{domain.capitalize()} \u00b7 Internals" if domain else "Other"


def anchor(subject: str, test_id: str) -> str:
    """The address of a test on the test page, which the help's and the map's links use: the subject as the page compares it, lowercase, and the test's id."""
    return f"{subject.lower()}:{test_id}"


def readable(title: str) -> str:
    """A test's title as a link reads it: a scenario's name without its prefix and with spaces."""
    return title.removeprefix("scenario_").replace("_", " ")


def by_control(tests: dict | None = None) -> dict:
    """The scenarios that set or expect each `Type.control`: id and the subject whose page lists it, for the help of that control."""
    out: dict = {}
    for test_id, entry in (tests if tests is not None else registry()).items():
        for control in entry.get("controls", []):
            out.setdefault(control, []).append({"id": test_id, "title": readable(entry["title"]), "subject": entry["subjects"][0]})
    return {c: sorted(v, key=lambda x: x["title"]) for c, v in out.items()}


def step_tests(processes: dict, tests: dict | None = None) -> dict:
    """The scenarios that prove each process step, by the key they share: a step that sets a control or expects its value, and a scenario that sets or expects the same `Type.control`. Each is a title and the test page's address, relative to the site."""
    tests = tests if tests is not None else registry()
    found = by_control(tests)
    named: dict = {}   # the tests that name a step themselves, which come before the ones that only share its control
    for test_id, entry in tests.items():
        for step in entry.get("steps", []):
            named.setdefault(step, []).append({"id": test_id, "title": readable(entry["title"]), "subject": entry["subjects"][0]})
    out: dict = {}
    for pid, p in processes.items():
        for s in p.get("steps", []):
            keys = [f"{e['module']}.{e['control']}" for e in s.get("then", []) if "control" in e]
            arg = next(iter(s["when"].values()))
            if isinstance(arg, dict) and "control" in arg and "module" in arg:
                keys.append(f"{arg['module']}.{arg['control']}")
            hits = list({h["id"]: h for h in [*named.get(f"{pid}/{s['id']}", []), *(h for k in dict.fromkeys(keys) for h in found.get(k, []))]}.values())
            if hits:
                out[f"{pid}/{s['id']}"] = [{"title": h["title"], "href": f"reference/tests.html#{anchor(h['subject'], h['id'])}"} for h in hits[:3]]
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
    """`steps.json` and `gaps.json` under `out`: the tests of each step, for the map to show, and what no test covers."""
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


def file_title(rel: str) -> str:
    """What a test file says it is: a Python file's docstring line, a JS file's first comment line; empty when it says nothing."""
    path = ROOT / rel
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return ""
    if rel.endswith(".py"):
        import ast
        return (ast.get_docstring(ast.parse(text)) or "").split("\n")[0]
    if rel.endswith(".mjs"):
        return next((line.lstrip("/ ").strip() for line in text.splitlines() if line.startswith("//")), "")
    return ""


def annotate(index: dict, by_subject: dict, runs: dict, findings: dict) -> None:
    """The state of each subject of the index, so the list shows it before a subject is opened: how many tests last passed, failed or have no result on this machine, and the errors and warnings a check last found. Only counts above zero are added, and the results only where something ran."""
    for subject, entry in index["subjects"].items():
        tests = by_subject[subject]["tests"]
        status = [runs.get(test_id, {}).get("status") for test_id in tests]
        found = findings.get(subject, {})
        counts = {"errors": found.get("errors", 0), "warnings": found.get("warnings", 0)}
        if runs:
            counts |= {"passed": status.count("pass"), "failed": status.count("fail"), "norun": sum(1 for s in status if s not in ("pass", "fail"))}
        entry.update({k: n for k, n in counts.items() if n})
        first = next(iter(tests.values()), {})
        if first.get("kind") in ("python", "js") and (title := file_title(first["file"])):
            entry["title"] = title
        if check := next((t for t in tests.values() if t.get("kind") == "check"), None):
            entry["title"] = check["title"]   # a check script is named by what it checks, its docstring is the card's description


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


def main() -> int:
    ap = argparse.ArgumentParser(description="Write steps.json and gaps.json, the tests of each process step and what no test covers, for a server that serves the map without the docs build.")
    ap.add_argument("--out", type=Path, required=True, help="the directory to write steps.json and gaps.json into")
    write_steps(ap.parse_args().out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
