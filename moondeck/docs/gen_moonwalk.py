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
import copy
import json
import re
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MOONWALK = ROOT / "moonwalk"
SCHEMA = MOONWALK / "moonwalk.schema.json"
HELP = ROOT / "docs" / "help" / "help.json"
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


def _strict_loader(yaml):
    """A YAML loader that refuses a key written twice in one mapping, where the safe loader keeps the last silently."""
    class Strict(yaml.SafeLoader):
        def construct_mapping(self, node, deep=False):
            seen = set()
            for key_node, _ in node.value:
                key = self.construct_object(key_node, deep=deep)
                if key in seen:
                    raise yaml.constructor.ConstructorError(None, None, f"duplicate key {key!r}", key_node.start_mark)
                seen.add(key)
            return super().construct_mapping(node, deep)
    return Strict


def build(root: Path = MOONWALK) -> dict:
    """Read and check every stage file, and return the map: stages in backbone order, processes by id, and the indexes by page, by module type and by control."""
    # Imported here rather than at the top, so the checks that import the docs hooks, and with them this module, need neither package.
    import jsonschema
    import yaml

    validator = jsonschema.Draft202012Validator(json.loads(SCHEMA.read_text(encoding="utf-8")))
    errors: list[str] = []
    stages, processes = [], {}
    for path in sorted(root.glob("[0-9][0-9]-*.yaml")):
        try:
            doc = yaml.load(path.read_text(encoding="utf-8"), Loader=_strict_loader(yaml))
        except yaml.YAMLError as e:
            errors.append(f"{path.name}: {e}")
            continue
        errors += [f"{path.name}: {'/'.join(map(str, e.absolute_path))}: {e.message}" for e in validator.iter_errors(doc)]
        if not isinstance(doc, dict) or not isinstance(doc.get("processes"), list):
            continue
        stage_id = path.stem[3:]
        # An entry that is no process with an id is the schema's to report; the rest of the file is still read.
        valid = [p for p in doc["processes"] if isinstance(p, dict) and isinstance(p.get("id"), str)]
        stages.append({"id": stage_id, "title": doc.get("stage", stage_id), "summary": doc.get("summary", ""),
                       "processes": [p["id"] for p in valid]})
        for p in valid:
            if p.get("id") in processes:
                errors.append(f"{path.name}: process id {p['id']} is also in stage {processes[p['id']]['stage']}")
            processes[p.get("id")] = {**p, "stage": stage_id}

    # A file that does not parse or validate leaves the rest unchecked, so its own errors are not buried under the dependencies it would have defined.
    if errors:
        raise ValueError("process files:\n  " + "\n  ".join(errors))
    known = systems()
    # A step names modules by type, the key the help uses, so every type and control it names is checked against the help the same build publishes.
    help_types = json.loads(HELP.read_text(encoding="utf-8"))["modules"]
    by_page: dict[str, list[str]] = {}
    by_module: dict[str, list[str]] = {}
    by_control: dict[str, list[str]] = {}
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
                    else:
                        for option in g.get("in", [g.get("equals")]):
                            if option not in inputs[g["input"]].get("enum", []):
                                errors.append(f"{pid}/{s['id']}: {g['input']} has no option {option}")
            targets = [arg] if isinstance(arg, dict) else []
            for t in targets + [e for e in s.get("then", []) if "module" in e]:
                for type_ in [t[k] for k in ("module", "type") if k in t]:
                    if type_ not in help_types:
                        errors.append(f"{pid}/{s['id']}: no module type {type_} in help.json")
                if "control" in t and t["module"] in help_types and t["control"] not in help_types[t["module"]].get("controls", {}):
                    errors.append(f"{pid}/{s['id']}: {t['module']} has no control {t['control']} in help.json")
            for t in targets:
                for type_ in dict.fromkeys(t[k] for k in ("module", "type") if k in t):
                    by_module.setdefault(type_, []).append(f"{pid}/{s['id']}")
            # A control's steps are the ones that set it or expect a value of it, under the key `Type.control`.
            for key in dict.fromkeys(f"{t['module']}.{t['control']}" for t in targets + s.get("then", []) if "control" in t):
                by_control.setdefault(key, []).append(f"{pid}/{s['id']}")
            if verb == "process" and arg not in processes:
                errors.append(f"{pid}/{s['id']}: runs no process: {arg}")
            for b in s.get("onSuccess", []) + s.get("onFailure", []):
                if b.get("type") == "goto" and b.get("stepId") not in step_ids:
                    errors.append(f"{pid}/{s['id']}: goto names no step: {b.get('stepId')}")

    if errors:
        raise ValueError("process files:\n  " + "\n  ".join(errors))
    return {"stages": stages, "systems": known, "processes": processes, "byPage": by_page, "byModule": by_module, "byControl": by_control}


# A link or a picture in a process's prose, written relative to docs/ as the MoonWalk page holds it.
_PROSE_LINK_RE = re.compile(r"(\]\()(?!https?://|#)([^)\s]+)\)")
_MEDIA_RE = re.compile(r"^!\[([^\]]*)\]\(([^)\s]+\.(?:webm|mp4))\)$", re.MULTILINE)
PROSE = ("intro", "outro")


def _prose_fields(p: dict):
    """Every prose field of a process, as (holder, key): its intro and outro, and each step's detail."""
    yield from ((p, k) for k in PROSE if p.get(k))
    yield from ((s, "detail") for s in p.get("steps", []) if s.get("detail"))


def absolute_links(model: dict) -> dict:
    """The model with every relative link in its prose made absolute on the docs site, since the view renders it on a device's page as well as the site's."""
    from gen_help import site_url   # imported here, so reading the stage files needs nothing gen_help needs
    page = ROOT / "docs" / "moonwalk.md"
    out = copy.deepcopy(model)
    for p in out["processes"].values():
        for holder, key in _prose_fields(p):
            holder[key] = _PROSE_LINK_RE.sub(lambda m: f"{m.group(1)}{site_url(m.group(2), page)})", holder[key])
    return out


def render_text(model: dict) -> str:
    """Every process, with its prose where it has some, as plain Markdown under the MoonWalk page: what the site's search indexes and a reader without JavaScript reads, until moonwalk.js draws the live view. Each heading's id is the process id, the address the view opens at."""
    out: list[str] = []
    # Every heading in the prose sits one level under its process's, as the view renders it under the section.
    prose = lambda text: re.sub(r"^(#+) ", r"#\1 ", text.strip(), flags=re.MULTILINE) + "\n"
    for p in model["processes"].values():
        # Every process has its heading and goal here, so any process is linkable by its id, as the view opens it.
        out.append(f"## {p['title']} {{#{p['id']}}}\n")
        out.append(f"*{p['goal']}*\n")
        if p.get("intro"):
            out.append(prose(p["intro"]))
        chapter, section = 0, None
        for s in p.get("steps", []):
            # A heading once per section, since each option of a choice may open the same one; a step for one option names it.
            if s.get("section") and s["section"] != section:
                chapter, section = chapter + 1, s["section"]
                out.append(f"### {chapter}. {section}\n")
            option = next((g.get("equals") or ", ".join(g["in"]) for g in s.get("given", []) if isinstance(g, dict) and "input" in g), None)
            out.append(f"**{option + ': ' if option else ''}{s['say']}**\n")
            if s.get("detail"):
                out.append(prose(s["detail"]))
        if p.get("outro"):
            out.append(prose(p["outro"]))
    # A clip written as a picture plays as a video on the page, as the view plays it.
    text = "\n".join(out)
    return _MEDIA_RE.sub(lambda m: f'<video src="{m.group(2)}" controls playsinline width="720" title="{m.group(1)}"></video>', text)


_REGENERATING = threading.Lock()   # the local servers answer requests on threads


def regenerate(out: Path) -> bool:
    """Write moonwalk.json to `out` as the stage files are now, for a local server answering a request: generated into a temporary file and swapped in only on success, one run at a time, so a failed run leaves no stale map behind and two requests never interleave. Run through uv, so the caller needs none of this generator's packages."""
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp")
    with _REGENERATING:
        try:
            ok = subprocess.run(["uv", "run", str(Path(__file__)), "--out", str(tmp)], check=False).returncode == 0
        except OSError:
            ok = False
        if not ok:
            tmp.unlink(missing_ok=True)
            out.unlink(missing_ok=True)
            return False
        tmp.replace(out)
    return True


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
        args.out.write_text(json.dumps(absolute_links(model), ensure_ascii=False), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
