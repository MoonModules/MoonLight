#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6", "jsonschema>=4"]
# ///
"""Generate moonwalk.json, MoonWalk's journey map every runner reads, from the stage files under moonwalk/.

Each `moonwalk/NN-<stage>.yaml` is one stage of the journey and its processes, validated against moonwalk/moonwalk.schema.json; the model is docs/work/future/moonwalk.md. The docs build serves the result at <site>/moonwalk/moonwalk.json, through mkdocs_hooks.py.

Usage:
    uv run moondeck/docs/gen_moonwalk.py                  # validate, print the counts
    uv run moondeck/docs/gen_moonwalk.py --out <file>     # also write moonwalk.json
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MOONWALK = ROOT / "moonwalk"
SCHEMA = MOONWALK / "moonwalk.schema.json"
# The systems' one home is the architecture's Part table, so a process names them as that table does and no second list exists.
ARCHITECTURE = ROOT / "docs" / "explanation" / "architecture" / "index.md"
_PART_RE = re.compile(r"^\| \[(Moon\w+)\]\(", re.MULTILINE)

# Which runner can perform an action, derived here rather than written on each step: the model's table of actions.
RUNNERS = {
    "open": ["tour", "film", "docs"], "set": ["test", "tour", "film", "docs"],
    "add": ["test", "tour", "film", "docs"], "replace": ["test", "tour", "film", "docs"],
    "delete": ["test", "tour", "film", "docs"], "clear": ["test", "tour", "film", "docs"],
    "apply": ["test", "docs"], "call": ["test", "docs"], "run": ["agent", "ci", "docs"],
    "read": ["docs", "tour"], "process": ["test", "tour", "film", "docs", "agent", "ci"], "manual": ["docs", "tour"],
}


def systems() -> list[str]:
    """The system names in the architecture's Part table, in its order."""
    return _PART_RE.findall(ARCHITECTURE.read_text(encoding="utf-8"))


def implied_systems(step: dict) -> set[str]:
    """The systems a step's action implies, so a stepped process gets them without naming them."""
    (verb, arg), = step["when"].items()
    found = set()
    if verb == "run" and isinstance(arg, dict):
        script = arg.get("script", "")
        found |= {"MoonTube"} if script.startswith("moondeck/moontube/") else {"MoonDeck"} if script.startswith("moondeck/") else set()
    if verb == "open" and isinstance(arg, dict) and "/install/" in arg.get("url", ""):
        found.add("MoonInstaller")
    module = arg.get("module", "") if isinstance(arg, dict) else ""
    if module.startswith("MoonLive"):
        found.add("MoonLive")
    return found


def page_url(page: str) -> str:
    """The site address of a docs page, as MkDocs writes it with use_directory_urls off."""
    return page[:-3] + ".html" if page.endswith(".md") else page


def build(root: Path = MOONWALK) -> dict:
    """Read and check every stage file, and return the map: stages in backbone order, processes by id, and the indexes by page and by module."""
    # Imported here rather than at the top, so the checks that import the docs hooks, and with them this module, need neither package.
    import jsonschema
    import yaml

    validator = jsonschema.Draft202012Validator(json.loads(SCHEMA.read_text(encoding="utf-8")))
    errors: list[str] = []
    stages, processes = [], {}
    for path in sorted(root.glob("[0-9][0-9]-*.yaml")):
        try:
            doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        except yaml.YAMLError as e:
            errors.append(f"{path.name}: {e}")
            continue
        errors += [f"{path.name}: {'/'.join(map(str, e.absolute_path))}: {e.message}" for e in validator.iter_errors(doc)]
        if not isinstance(doc, dict) or not isinstance(doc.get("processes"), list):
            continue
        stage_id = path.stem[3:]
        stages.append({"id": stage_id, "title": doc.get("stage", stage_id), "summary": doc.get("summary", ""),
                       "processes": [p.get("id") for p in doc["processes"]]})
        for p in doc["processes"]:
            if p.get("id") in processes:
                errors.append(f"{path.name}: process id {p['id']} is also in stage {processes[p['id']]['stage']}")
            processes[p.get("id")] = {**p, "stage": stage_id}

    # A file that does not parse or validate leaves the rest unchecked, so its own errors are not buried under the dependencies it would have defined.
    if errors:
        raise ValueError("process files:\n  " + "\n  ".join(errors))
    known = systems()
    by_page: dict[str, list[str]] = {}
    by_module: dict[str, list[str]] = {}
    for pid, p in processes.items():
        for dep in p.get("dependsOn", []):
            if dep not in processes:
                errors.append(f"{pid}: dependsOn names no process: {dep}")
        if "page" in p:
            if not (ROOT / "docs" / p["page"]).exists():
                errors.append(f"{pid}: page does not exist: docs/{p['page']}")
            p["url"] = page_url(p["page"])
            by_page.setdefault(p["url"], []).append(pid)
        step_ids = [s.get("id") for s in p.get("steps", [])]
        if len(step_ids) != len(set(step_ids)):
            errors.append(f"{pid}: two steps share an id")
        named = set(p.get("systems", []))
        for name in named - set(known):
            errors.append(f"{pid}: no system named {name} in the architecture's Part table")
        for s in p.get("steps", []):
            named |= implied_systems(s)
        p["systems"] = [name for name in known if name in named]
        inputs = p.get("inputs", {}).get("properties", {})
        for s in p.get("steps", []):
            (verb, arg), = s["when"].items()
            s["runners"] = RUNNERS.get(verb, [])
            s.setdefault("actor", p.get("actor"))
            for g in s.get("given", []):
                if isinstance(g, str) and g not in processes:
                    errors.append(f"{pid}/{s['id']}: given names no process: {g}")
                if isinstance(g, dict) and "input" in g:
                    if g["input"] not in inputs:
                        errors.append(f"{pid}/{s['id']}: given names no input of the process: {g['input']}")
                    elif g.get("equals") not in inputs[g["input"]].get("enum", []):
                        errors.append(f"{pid}/{s['id']}: {g['input']} has no option {g.get('equals')}")
            if isinstance(arg, dict) and "module" in arg:
                by_module.setdefault(arg["module"], []).append(f"{pid}/{s['id']}")
            if verb == "process" and arg not in processes:
                errors.append(f"{pid}/{s['id']}: runs no process: {arg}")
            for b in s.get("onSuccess", []) + s.get("onFailure", []):
                if b.get("type") == "goto" and b.get("stepId") not in step_ids:
                    errors.append(f"{pid}/{s['id']}: goto names no step: {b.get('stepId')}")

    if errors:
        raise ValueError("process files:\n  " + "\n  ".join(errors))
    return {"stages": stages, "systems": known, "processes": processes, "byPage": by_page, "byModule": by_module}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", type=Path, help="write moonwalk.json here")
    args = ap.parse_args()
    try:
        model = build()
    except ValueError as e:
        print(e, file=sys.stderr)
        return 1
    steps = sum(len(p.get("steps", [])) for p in model["processes"].values())
    print(f"gen_moonwalk: {len(model['stages'])} stages, {len(model['processes'])} processes, {steps} steps")
    if args.out:
        args.out.write_text(json.dumps(model, ensure_ascii=False), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
