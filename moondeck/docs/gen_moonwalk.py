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
import os
import re
import subprocess
import sys
import threading
import urllib.parse
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
MAX_DOWNLOAD_KB = 50     # the map and its renderer, gzipped, as a page or the app fetches them: past it, the details load on demand
RENDERER = ROOT / "docs" / "assets" / "moonwalk.js"
# The systems' one home is the architecture's Part table, so a process names them as that table does and no second list exists.
ARCHITECTURE = ROOT / "docs" / "explanation" / "architecture" / "index.md"
_PART_RE = re.compile(r"^\| \[(Moon\w+)\]\(", re.MULTILINE)
# A call step names an operation of the device's REST API by its operationId, the OpenAPI description being the API's one home.
OPENAPI = ROOT / "docs" / "reference" / "openapi.yaml"

# Which runner can perform an action, derived here rather than written on each step: the model's table of actions.
RUNNERS = {
    "open": ["app", "docs"], "set": ["test", "app", "docs"],
    "add": ["test", "app", "docs"], "replace": ["test", "app", "docs"],
    "delete": ["test", "app", "docs"], "clear": ["test", "app", "docs"],
    "apply": ["test", "docs"], "call": ["test", "app", "docs", "agent"], "run": ["agent", "ci", "docs"],
    "read": ["docs", "app"], "process": ["test", "app", "docs", "agent", "ci"], "manual": ["docs", "app"],
}


def systems() -> list[str]:
    """The system names in the architecture's Part table, in its order."""
    return _PART_RE.findall(ARCHITECTURE.read_text(encoding="utf-8"))


def operations() -> dict:
    """Every operation of the REST API by its operationId: its method, its path, where each of its parameters goes, path or query, and whether its body is a file's bytes."""
    import yaml
    spec = yaml.safe_load(OPENAPI.read_text(encoding="utf-8"))
    resolve = lambda prm: spec["components"]["parameters"][prm["$ref"].rsplit("/", 1)[1]] if "$ref" in prm else prm
    return {op["operationId"]: {"method": method.upper(), "path": path,
                                "params": {resolve(prm)["name"]: resolve(prm)["in"] for prm in item.get("parameters", []) + op.get("parameters", [])},
                                "binary": "application/octet-stream" in op.get("requestBody", {}).get("content", {})}
            for path, item in spec["paths"].items() for method, op in item.items() if isinstance(op, dict) and "operationId" in op}


def implied_systems(step: dict) -> set[str]:
    """The systems a step's action implies, so a stepped process gets them without naming them."""
    (verb, arg), = step["when"].items()
    found = set()
    if verb == "run" and isinstance(arg, dict):
        script = arg.get("script", "")
        found |= {"MoonDeck"} if script.startswith("moondeck/") else set()
    if verb == "open" and isinstance(arg, dict) and "/install/" in arg.get("url", ""):
        found.add("MoonInstaller")
    module = arg.get("module", "") if isinstance(arg, dict) else ""
    if module.startswith("MoonLive"):
        found.add("MoonLive")
    if verb == "call":
        found.add("MoonCore")   # the device's API is MoonCore's
    return found


def page_exists(page: str) -> bool:
    """Whether a docs page exists: its file, or a catalog page the docs build generates from help.json."""
    from gen_help import CATALOGS   # imported here, as site_url is
    return (ROOT / "docs" / page).exists() or page.removeprefix("moonmodules/") in CATALOGS


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
                errors.append(f"{pid}/{st.get('id')}: say is one action in at most {MAX_SAY_WORDS} words; the rest goes in its detail")
    return errors


def split_text(model: dict) -> tuple:
    """The map without its prose, and each process's prose by id: a page loads the map, and a process's intro, outro and details when its view opens. A process with prose is marked `text`, so the view knows to fetch it. A step's runners stay with build(), which the runners that read them call, so no page downloads them."""
    m = copy.deepcopy(model)
    texts = {}
    for pid, p in m["processes"].items():
        for s in p.get("steps", []):
            s.pop("runners", None)
        t = {k: p.pop(k) for k in ("intro", "outro") if k in p}
        details = {s["id"]: s.pop("detail") for s in p.get("steps", []) if "detail" in s}
        if details:
            t["details"] = details
        if t:
            texts[pid] = t
            p["text"] = True
    return m, texts


def dumps(obj) -> str:
    """JSON as a page downloads it: compact, since only code reads it."""
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def write(model: dict, out: Path) -> None:
    """The map at `out` and each process's prose beside it, under text/<process>.json, with links absolute since the view renders them on a device's page too."""
    m, texts = split_text(absolute_links(model))
    folder = out.parent / "text"
    folder.mkdir(parents=True, exist_ok=True)
    # Each file swapped in whole and the stale ones removed after, so a reader fetching a text during a rewrite never meets a gap.
    for pid, t in texts.items():
        tmp = folder / f"{pid}.json.tmp"
        tmp.write_text(dumps(t), encoding="utf-8")
        tmp.replace(folder / f"{pid}.json")
    for old in folder.glob("*.json"):
        if old.stem not in texts:
            old.unlink()
    out.write_text(dumps(m), encoding="utf-8")


def derived_then(verb: str, arg) -> list:
    """What the device shows once a step's action holds, when the verb says it: an add leaves its module, a set leaves its control's value. Empty for any other verb, whose result is not in its action."""
    if verb == "add":
        return [{"module": arg["type"], "exists": True}]
    if verb == "set":
        return [{"module": arg["module"], "control": arg["control"], "equals": arg["value"]}]
    return []


def download_kb(model: dict) -> int:
    """What a reader downloads to see MoonWalk, in KB: the map as --out writes it, without its prose, and the renderer, each gzipped as a server sends them."""
    sizes = [len(gzip.compress(dumps(split_text(absolute_links(model))[0]).encode("utf-8"))), len(gzip.compress(RENDERER.read_bytes()))]
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


AUTOMATED_RUNNERS = {"test", "ci", "agent"}   # the runners that perform a step without a person: a test, CI, or an agent reading the process


def unverified(processes: dict) -> list:
    """The processes no runner verifies, as (stage, message): none of their steps has an automated runner or a condition the device state answers, so only a person can tell that the process works."""
    return [(p["stage"], f"{pid}: no step has an automated runner or a condition, so no run proves it works")
            for pid, p in processes.items()
            if not any(AUTOMATED_RUNNERS & set(s["runners"]) or s.get("then") for s in p["steps"])]


def shared_runs(processes: dict) -> list:
    """Two or more steps in a row that act the same in two processes, as (stage, message): one process both should call, as a repeated block becomes one function."""
    # Per reader, as the caps measure: two steps a choice keeps apart are alternatives, not a run.
    reads = {pid: list(_readers(p).values()) for pid, p in processes.items()}
    out = []
    pids = list(processes)
    for i, a in enumerate(pids):
        for b in pids[i + 1:]:
            best, run = 0, []
            for x in reads[a]:
                for y in reads[b]:
                    n, end = _longest_common_run([json.dumps(s.get("when"), sort_keys=True) for s in x], [json.dumps(s.get("when"), sort_keys=True) for s in y])
                    if n > best:
                        best, run = n, x[end - n:end]
            if best >= 2:
                out.append((processes[b]["stage"], f"{b}: {best} steps repeat {a}'s ({', '.join(s['id'] for s in run)}); make them one process both call"))
    return out


def _longest_common_run(x: list, y: list) -> tuple:
    """The length of the longest run `x` and `y` share and where it ends in `x`: the textbook longest common substring."""
    best, end = 0, 0
    prev = [0] * (len(y) + 1)
    for m in range(1, len(x) + 1):
        cur = [0] * (len(y) + 1)
        for n in range(1, len(y) + 1):
            if x[m - 1] == y[n - 1]:
                cur[n] = prev[n - 1] + 1
                if cur[n] > best:
                    best, end = cur[n], m
        prev = cur
    return best, end


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

    _ANCHORS.clear()   # a long-running server builds again after the pages changed: their anchors are read afresh
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
    api = operations()
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
            if not page_exists(p["page"]):
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
            # Where the step takes the reader, as the view links it: a page as the site addresses it, or an address to open; one with a placeholder to fill in is no link.
            if verb == "read":
                page, _, anchor = arg["page"].partition("#")
                if not page_exists(page):
                    errors.append(f"{pid}/{s['id']}: page does not exist: docs/{page}")
                elif anchor and (why := link_error(arg["page"], processes)):
                    errors.append(f"{pid}/{s['id']}: read step's {why}")
                from gen_help import heading_id   # imported here, as site_url is
                if not anchor and arg.get("section") and page_exists(page) and (why := link_error(f"{page}#{heading_id(arg['section'])}", processes)):
                    errors.append(f"{pid}/{s['id']}: read step's section: {why}")
                anchor = anchor or (heading_id(arg["section"]) if arg.get("section") else "")
                s["url"] = page_url(page) + (f"#{anchor}" if anchor else "")
            elif verb == "open" and "url" in arg and "<" not in arg["url"]:
                s["url"] = arg["url"]
            # A call is the operation's method and its path with the parameters filled in, linked to its entry on the REST API page.
            elif verb == "call" and arg["operation"] not in api:
                errors.append(f"{pid}/{s['id']}: calls no operation of the REST API: {arg['operation']}")
            elif verb == "call":
                op, params = api[arg["operation"]], arg.get("params", {})
                for name in sorted(params.keys() - op["params"].keys()):
                    errors.append(f"{pid}/{s['id']}: {arg['operation']} takes no parameter {name}")
                for name in re.findall(r"\{(\w+)\}", op["path"]):
                    if name not in params:
                        errors.append(f"{pid}/{s['id']}: {arg['operation']} needs parameter {name}")
                query = "&".join(f"{k}={_encoded(v, '/')}" for k, v in params.items() if op["params"].get(k) == "query")
                arg["method"] = op["method"]
                if op["binary"]:
                    arg["binary"] = True
                arg["path"] = re.sub(r"\{(\w+)\}", lambda m: _encoded(params.get(m.group(1), m.group(0)), ""), op["path"]) + (f"?{query}" if query else "")
                s["url"] = page_url("reference/api.md") + f"#operation/{arg['operation']}"
            s.setdefault("actor", p.get("actor"))
            if command_of(s):
                s["command"] = command_of(s)   # written once here, so every host copies the same line
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
            if "then" not in s and (derived := derived_then(verb, arg)):
                s["then"] = derived
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
            if verb == "run":
                errors += [f"{pid}/{s['id']}: {why}" for why in run_errors(arg)]
            if verb == "process" and arg not in processes:
                errors.append(f"{pid}/{s['id']}: runs no process: {arg}")

    for pid, p in processes.items():
        for step_id, text in prose_texts(p):
            for target in dict.fromkeys(_MD_LINK_RE.findall(_FENCE_RE.sub("", text))):
                if why := link_error(target, processes):
                    errors.append(f"{pid}/{step_id}: {why}" if step_id else f"{pid}: {why}")
    errors += shape_errors(processes)
    if root == MOONWALK:   # the repository's own tests name its own steps; a map built from other files is judged on its own
        import gen_tests   # imported here, as the tests name process steps and read the map for them
        errors += gen_tests.unknown_steps(processes)
    if errors:
        raise ValueError("process files:\n  " + "\n  ".join(errors))
    return {"stages": stages, "systems": known, "processes": processes, "byPage": by_page, "byModule": by_module, "byControl": by_control}


# ---- references: what a process's prose and commands point at must exist --------------------------

# A link or a picture in prose, its target without a title.
_MD_LINK_RE = re.compile(r"\]\(([^)\s]+)\)")
_FENCE_RE = re.compile(r"^(```|~~~).*?^\1[^\n]*$", re.MULTILINE | re.DOTALL)
_HEADING_RE = re.compile(r"^#{1,6}[ \t]+(.+?)[ \t]*#*[ \t]*$", re.MULTILINE)
_EXPLICIT_ID_RE = re.compile(r"\{:?[ \t]*#([\w:.-]+)[^}]*\}")
_HTML_ID_RE = re.compile(r"""\bid=["']([^"']+)["']""")
_ANCHORS: dict = {}   # file -> its anchors, read once per run


def anchors_of(path: Path) -> set:
    """The anchors of a Markdown file as the docs build renders them: each heading's id (a repeat gets `_1`, `_2`, as the toc extension numbers them), an explicit `{#id}` and an `id="..."` in HTML; a fenced block holds code, so it holds none."""
    if path in _ANCHORS:
        return _ANCHORS[path]
    from gen_help import heading_id   # imported here, as site_url is
    text = _FENCE_RE.sub("", path.read_text(encoding="utf-8"))
    found, seen = set(_HTML_ID_RE.findall(text)), {}
    for m in _HEADING_RE.finditer(text):
        title = m.group(1)
        explicit = _EXPLICIT_ID_RE.search(title)
        if explicit:
            found.add(explicit.group(1))
            continue
        # A code span's text is kept whole, `<target>` included, where HTML outside it is dropped.
        plain = "".join(part if i % 2 else re.sub(r"<[^>]+>", "", part).replace("*", "")
                        for i, part in enumerate(re.split(r"`([^`]*)`", re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", title))))
        slug = heading_id(plain)
        n = seen.get(slug, 0)
        seen[slug] = n + 1
        found.add(slug if n == 0 else f"{slug}_{n}")
    _ANCHORS[path] = found
    return found


def link_error(target: str, processes: dict) -> str | None:
    """Why a link in a process's prose points nowhere, or None: a site-relative target (relative to docs/, `../` leaving it for the repo) names a file that exists and an anchor that file holds. Web addresses and targets with a `<placeholder>` are the reader's."""
    if re.match(r"(https?|mailto):", target) or "<" in target or target.startswith("#"):
        return None
    path, _, anchor = target.partition("#")
    path = urllib.parse.unquote(path)
    if path.startswith("../"):
        file = Path(os.path.normpath(ROOT / "docs" / path))
        if not file.is_file():
            return f"link to a missing file: {target}"
    elif not page_exists(path):
        return f"link to a missing page: docs/{path}"
    else:
        file = ROOT / "docs" / path
    if not anchor:
        return None
    if path == "moonwalk.md":
        pid, _, step = anchor.partition("/")
        return None if pid in processes and (not step or step in [s.get("id") for s in processes[pid].get("steps", [])]) else f"link to no process or step: {target}"
    from gen_help import CATALOGS
    if file.suffix != ".md" or path.removeprefix("moonmodules/") in CATALOGS:
        return None   # a catalog page is generated at build time, so the stub on disk holds none of its anchors
    return None if anchor in anchors_of(file) else f"link to a missing anchor: {target}"


def prose_texts(p: dict):
    """Every prose text of a process: its intro and outro, and each step's detail, note and manual text, as (step id or None, text)."""
    yield from ((None, p[k]) for k in PROSE if p.get(k))
    for s in p.get("steps", []):
        yield from ((s.get("id"), t) for t in (s.get("detail"), s.get("note"), s["when"].get("manual")) if isinstance(t, str))


def declared_options(script: Path) -> set | None:
    """The `--options` a script declares with add_argument, subparsers' included; None when it declares no argument at all, so its flags cannot be told."""
    text = script.read_text(encoding="utf-8")
    calls = re.findall(r"add_argument\(\s*((?:[\"'][^\"']*[\"']\s*,\s*)*[\"'][^\"']*[\"'])", text)
    if not calls:
        return None
    return {o for c in calls for o in re.findall(r"[\"'](--[\w-]+)[\"']", c)} | {"--help"}


def _run_lines(arg) -> list:
    """The scripts a run step invokes with the arguments given to each: (script path relative to the repo, argument text)."""
    if not isinstance(arg, dict):
        return []
    if "script" in arg:
        return [(arg["script"], arg.get("args", ""))]
    return [(m.group(1), m.group(2)) for m in re.finditer(r"uv run (\S+\.py)([^;&|]*)", arg.get("command", ""))]


def run_errors(arg) -> list:
    """What a run step names that does not exist: a flag its script does not declare, a scenario with no file and a repo path with no file."""
    errors = []
    text = " ".join(filter(None, (arg.get("script", ""), arg.get("args", ""), arg.get("command", "")))) if isinstance(arg, dict) else ""
    for script, args in _run_lines(arg):
        file = ROOT / script
        options = declared_options(file) if file.is_file() else None
        if options is not None:
            errors += [f"`{script}` has no option --{flag}" for flag in dict.fromkeys(re.findall(r"(?<![\w-])--([\w-]+)", args)) if f"--{flag}" not in options]
        if Path(script).name in ("run_scenario.py", "run_live_scenario.py"):
            for value in re.findall(r"--name[= ]\s*(\S+)", args):
                for name in [n for n in value.strip("\"'").split(",") if n and "<" not in n]:
                    if not list((ROOT / "test" / "scenarios").rglob(f"{name}.json")):
                        errors.append(f"no scenario named {name}: test/scenarios/**/{name}.json")
    tops = {d.name for d in ROOT.iterdir() if d.is_dir()}
    for token in text.split():
        token = token.lstrip("@").strip("\"',;")
        first = token.split("/", 1)[0]
        if "/" in token and first in tops and first != "build" and not re.search(r"[<$*]|\.\.\.", token) and not (ROOT / token).exists():
            errors.append(f"no such path: {token}")
    return errors


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


def _encoded(value, safe: str) -> str:
    """A parameter value percent-encoded for a URL, its `<placeholder>` parts left as written for the reader to fill in."""
    return "".join(part if re.fullmatch(r"<[^>]+>", part) else urllib.parse.quote(part, safe=safe) for part in re.split(r"(<[^>]+>)", str(value)))


def _quoted(text: str) -> str:
    """Single-quoted for a POSIX shell, so the URL's ? and & and the body's braces reach curl as written."""
    return "'" + text.replace("'", "'\\''") + "'"


def _curl(call: dict) -> str:
    """An API call as the curl line that sends it to the reader's device; a file's bytes go without `Expect: 100-continue`, which the device does not answer."""
    body = f"-d {_quoted(call['body'])}" if call.get("body") else '--data-binary @<file> -H "Expect:"' if call.get("binary") else None
    return " ".join(filter(None, ("curl", call["method"] != "GET" and f"-X {call['method']}", _quoted(f"http://<device>{call['path']}"), body)))


def command_of(step: dict):
    """A step's call as a reader types it, as moonwalk.js's commandOf writes it: a MoonDeck script with its arguments, a command, or an API call as the curl line that sends it; None for a step that runs nothing."""
    run, call = step["when"].get("run"), step["when"].get("call")
    if call:
        return _curl(call) if "method" in call else None   # an unknown operation, already refused
    return run and (f"uv run {run['script']}{' ' + run['args'] if run.get('args') else ''}" if "script" in run else run.get("command"))


def render_text(model: dict) -> str:
    """Every process, with its prose where it has some, as plain Markdown under the MoonWalk page: what the site's search indexes and a reader without JavaScript reads, until moonwalk.js draws the live view. Each heading's id is the process id, the address the view opens at."""
    out: list[str] = []
    # Every heading in the prose sits one level under its process's, as the view renders it under the section.
    # A fenced block is code, so a `# comment` in it stays as written: the split keeps fences at the odd indexes.
    prose = lambda text: "".join(part if i % 2 else re.sub(r"^(#+) ", r"#\1 ", part, flags=re.MULTILINE)
                                 for i, part in enumerate(re.split(r"(^```.*?^```)", text.strip(), flags=re.MULTILINE | re.DOTALL))) + "\n"
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
            if s.get("command"):
                out.append(f"`` {s['command']} ``\n")
            # Where the step takes the reader, under its instruction: a page link names the source file, so the docs build checks it and turns it into the page's address.
            if s.get("url"):
                web = re.match(r"https?://", s["url"])
                # Redoc draws the REST API page's anchors in the browser, where the docs build cannot check them, so a call links the page alone.
                url = s["url"].partition("#")[0] if "call" in s["when"] else s["url"]
                out.append(f"[{url[web.end():] if web else 'Read the page'}]({url if web else re.sub(r'^([^:#]+)[.]html', r'\1.md', url)})\n")
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
