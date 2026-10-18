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
import gzip
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

# How long each part of a process may be, the procedure style guides' rule (one action per step, verb first) and Diátaxis' (a how-to holds steps, not reference or explanation). The short fields are errors; the long ones warn in check_docgen's report and may only fall.
MAX_SAY_WORDS = 8        # a step's instruction: one action, read at a glance on the map and in the bar
MAX_TITLE_WORDS = 10     # a process's title: a map block and a link
MAX_GOAL_WORDS = 25      # its goal: one sentence, the outcome
MAX_DETAIL_WORDS = 120   # a step's detail: what the reader needs at that step; the rest links to its kind of page
MAX_FRAME_WORDS = 150    # an intro or an outro: context, not content
MAX_PROCESS_WORDS = 600  # a whole process as one reader reads it: past it, it is two processes or it holds reference
MAX_READER_STEPS = 10    # a process's steps as one reader meets them: past it, steps fold into a subprocess, as procedure guides split a long task
MAX_STAGE_PROCESSES = 8  # a stage's processes, one column of the map: past it, the stage is two stages
MAX_DOWNLOAD_KB = 40     # the map and its renderer, gzipped, as a page or the app fetches them: past it, the details load on demand
RENDERER = ROOT / "docs" / "assets" / "moonwalk.js"
# The systems' one home is the architecture's Part table, so a process names them as that table does and no second list exists.
ARCHITECTURE = ROOT / "docs" / "explanation" / "architecture" / "index.md"
_PART_RE = re.compile(r"^\| \[(Moon\w+)\]\(", re.MULTILINE)

# Which runner can perform an action, derived here rather than written on each step: the model's table of actions.
RUNNERS = {
    "open": ["app", "film", "docs"], "set": ["test", "app", "film", "docs"],
    "add": ["test", "app", "film", "docs"], "replace": ["test", "app", "film", "docs"],
    "delete": ["test", "app", "film", "docs"], "clear": ["test", "app", "film", "docs"],
    "apply": ["test", "docs"], "call": ["test", "docs"], "run": ["agent", "ci", "docs"],
    "read": ["docs", "app"], "process": ["test", "app", "film", "docs", "agent", "ci"], "manual": ["docs", "app"],
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


def _words(text) -> int:
    """The words a reader reads: an image is seen rather than read, and a link's target is not shown."""
    return len(re.sub(r"\]\([^)]*\)", "]", re.sub(r"!\[[^\]]*\]\([^)]*\)", "", text or "")).split())


def _one_sentence(text: str) -> bool:
    """Whether text is one sentence: no sentence end before its last character."""
    return not re.search(r"[.!?]\s+[A-Z]", (text or "").strip())


def shape_errors(processes: dict) -> list:
    """The short fields over their limits: a step's instruction, a title and a goal."""
    errors = []
    for pid, p in processes.items():
        if _words(p.get("title")) > MAX_TITLE_WORDS:
            errors.append(f"{pid}: title over {MAX_TITLE_WORDS} words")
        if _words(p.get("goal")) > MAX_GOAL_WORDS or not _one_sentence(p.get("goal", "")):
            errors.append(f"{pid}: goal over {MAX_GOAL_WORDS} words or more than one sentence")
        for st in p.get("steps", []):
            if _words(st.get("say")) > MAX_SAY_WORDS or not _one_sentence(st.get("say", "")):
                errors.append(f"{pid}/{st.get('id')}: say is one action in at most {MAX_SAY_WORDS} words; the rest goes in its note or detail")
    return errors


def split_text(model: dict) -> tuple:
    """The map without its prose, and each process's prose by id: a page loads the map, and a process's intro, outro and details when its view opens. A process with prose is marked `text`, so the view knows to fetch it."""
    m = copy.deepcopy(model)
    texts = {}
    for pid, p in m["processes"].items():
        t = {k: p.pop(k) for k in ("intro", "outro") if k in p}
        details = {s["id"]: s.pop("detail") for s in p.get("steps", []) if "detail" in s}
        if details:
            t["details"] = details
        if t:
            texts[pid] = t
            p["text"] = True
    return m, texts


def write(model: dict, out: Path) -> None:
    """The map at `out` and each process's prose beside it, under text/<process>.json, with links absolute since the view renders them on a device's page too."""
    m, texts = split_text(absolute_links(model))
    folder = out.parent / "text"
    folder.mkdir(parents=True, exist_ok=True)
    # Each file swapped in whole and the stale ones removed after, so a reader fetching a text during a rewrite never meets a gap.
    for pid, t in texts.items():
        tmp = folder / f"{pid}.json.tmp"
        tmp.write_text(json.dumps(t, ensure_ascii=False), encoding="utf-8")
        tmp.replace(folder / f"{pid}.json")
    for old in folder.glob("*.json"):
        if old.stem not in texts:
            old.unlink()
    out.write_text(json.dumps(m, ensure_ascii=False), encoding="utf-8")


def download_kb(model: dict) -> int:
    """What a reader downloads to see MoonWalk, in KB: the map as --out writes it, without its prose, and the renderer, each gzipped as a server sends them."""
    sizes = [len(gzip.compress(json.dumps(split_text(absolute_links(model))[0], ensure_ascii=False).encode("utf-8"))), len(gzip.compress(RENDERER.read_bytes()))]
    return round(sum(sizes) / 1024)


def _readers(p: dict) -> dict:
    """The steps one reader meets, per option of the process's choice, or once, keyed "", without one."""
    steps = p.get("steps", [])
    inputs = p.get("inputs", {}).get("properties", {})
    if not inputs:
        return {"": steps}
    name, spec = next(iter(inputs.items()))
    def applies(st, option):
        return all(not (isinstance(g, dict) and g.get("input") == name) or (option in g["in"] if "in" in g else g.get("equals") == option)
                   for g in st.get("given", []))
    return {o: [st for st in steps if applies(st, o)] for o in spec.get("enum", [])}


def _reader_totals(p: dict) -> dict:
    """The words one reader reads, per option: the process's intro and outro, and the steps they meet."""
    frame = _words(p.get("intro")) + _words(p.get("outro"))
    return {o: frame + sum(_words(st.get("say")) + _words(st.get("detail")) for st in steps) for o, steps in _readers(p).items()}


def size_warnings(processes: dict) -> list:
    """What is over its cap, as (stage, message): a stage's processes, a process's steps and words, a step's detail, an intro or outro."""
    out = []
    stages = {}
    for pid, p in processes.items():
        stages.setdefault(p["stage"], []).append(pid)
    out += [(stage, f"{len(pids)} processes > {MAX_STAGE_PROCESSES}; split the stage") for stage, pids in stages.items() if len(pids) > MAX_STAGE_PROCESSES]
    for pid, p in processes.items():
        for field in ("intro", "outro"):
            if _words(p.get(field)) > MAX_FRAME_WORDS:
                out.append((p["stage"], f"{pid}: {field} {_words(p.get(field))} words > {MAX_FRAME_WORDS}"))
        for st in p.get("steps", []):
            if _words(st.get("detail")) > MAX_DETAIL_WORDS:
                out.append((p["stage"], f"{pid}: step {st.get('id')}'s detail {_words(st.get('detail'))} words > {MAX_DETAIL_WORDS}"))
        # What one reader meets: the steps for their choice, so a process with six platforms is measured per platform rather than as their sum.
        for option, steps in _readers(p).items():
            if len(steps) > MAX_READER_STEPS:
                out.append((p["stage"], f"{pid}: {len(steps)} steps > {MAX_READER_STEPS}{f' for {option}' if option else ''}; fold steps into a subprocess"))
        for option, total in _reader_totals(p).items():
            if total > MAX_PROCESS_WORDS:
                out.append((p["stage"], f"{pid}: {total} words > {MAX_PROCESS_WORDS}{f' for {option}' if option else ''}; split it, or move reference and explanation to their pages"))
    return out


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
        stages.append({"id": stage_id, "file": path.name, "title": doc.get("stage", stage_id), "summary": doc.get("summary", ""),
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
            if verb == "run" and isinstance(arg, dict) and "script" in arg and not (ROOT / arg["script"]).is_file():
                errors.append(f"{pid}/{s['id']}: runs no script: {arg['script']}")
            if verb == "process" and arg not in processes:
                errors.append(f"{pid}/{s['id']}: runs no process: {arg}")
            for b in s.get("onSuccess", []) + s.get("onFailure", []):
                if b.get("type") == "goto" and b.get("stepId") not in step_ids:
                    errors.append(f"{pid}/{s['id']}: goto names no step: {b.get('stepId')}")

    errors += shape_errors(processes)
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
    """Write moonwalk.json to `out` as the stage files are now, with each process's prose beside it, for a local server answering a request: generated into a temporary file and swapped in only on success, one run at a time, so a failed run leaves no stale map behind and two requests never interleave. Run through uv, so the caller needs none of this generator's packages."""
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
    ap.add_argument("--out", type=Path, help="write moonwalk.json here, and each process's prose under text/ beside it")
    args = ap.parse_args()
    try:
        model = build()
    except ValueError as e:
        print(e, file=sys.stderr)
        return 1
    steps = sum(len(p.get("steps", [])) for p in model["processes"].values())
    print(f"gen_moonwalk: {len(model['stages'])} stages, {len(model['processes'])} processes, {steps} steps")
    if args.out:
        write(model, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
