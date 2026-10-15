#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["libclang>=18"]
# ///
"""Generate help.json, the help the app shows per module and per control.

The header is the source: clang's AST attaches each `///` comment to its declaration, so a module's help is its class comment and a control's help is the comment on the member its `add…` call publishes. libclang (the LLVM project's Python bindings) reads the registry's translation unit, `src/module_types.cpp`, which includes every registered module, in about a second, with no build: the registry needs only its include paths.

A module whose catalog card still exists is read from that card instead: the bridge of the strangler-fig migration, so the file covers every module while the headers take over one by one. A card cut down to its heading is what moves its module to the header, and the docs build renders that card from the help. The bridge goes with the last card body.

Usage:
    uv run moondeck/docs/gen_help.py                 # writes docs/help/help.json
    uv run moondeck/docs/gen_help.py --ref v6.1.0 --out <dir>/help.json   # links at that tag
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

SITE = "https://moonmodules.org/MoonLight/"
REPO = "https://github.com/MoonModules/MoonLight/blob/"
REGISTRY = "src/module_types.cpp"
CATALOG = ROOT / "docs" / "moonmodules"
# In the docs tree, so every docs deploy publishes it at <site>/help/help.json, and committed, so the docs build reads it without LLVM and a header's change shows in the diff as its help line.
HELP = ROOT / "docs" / "help" / "help.json"
# A catalog page drawn whole from help.json, named once here with the base class whose comment introduces it: the page's title and its intro, what every module on it shares.
# A catalog with no shared class keeps its intro as the page's own markdown, and its base is None.
CATALOGS = {"light/modifiers.md": ("Modifiers", "ModifierBase"), "light/layouts.md": ("Layouts", "LayoutBase"),
            "light/effects.md": ("Effects", "EffectBase"), "light/drivers.md": ("Drivers", "DriverBase"),
            "light/supporting.md": ("Supporting", None), "core/system.md": ("System", None),
            "core/services.md": ("Services", "Services")}

_MARKER_RE = re.compile(r"^(@guide|@moreinfo)\s*$", re.MULTILINE)
_XREF_RE = re.compile(r"@xref\{([a-z0-9-]+)(?:\|([^}]+))?\}")
_LINK_RE = re.compile(r"(\]\(|src=\")(?!https?://|#)([^)\"\s]+)")
_ANCHOR_LINK_RE = re.compile(r"(\]\()#([^)]+)\)")
# The lead's block tags, Javadoc's `@author` construct: a line opening `@<tag>` starts the field, and the field runs to the next blank line or tag. Each tag is the help field of the same name, so a header names what help.json carries; a new structured field is one entry here.
#   card         the picture, a file under docs/assets/, then optionally its alt text
#   attribution  what the module learned from or who wrote it first, shown apart from the summary, since writing our own rather than depending removes the automatic credit
#   video        a clip, a file under docs/assets/, then optionally its title; a catalog's intro shows each one, in order
BLOCK_TAGS = ("card", "attribution", "video")
# The block tags a lead may repeat, each occurrence one item of a list field.
REPEATED_TAGS = ("video",)
_TAG_RE = re.compile(r"^@(\w+)\s*(.*)$")
_SENTENCE_RE = re.compile(r"(?<=[.!?])\s+(?=[A-Z`*\[])")
# Doxygen's cross-reference, `@ref ClassName`: the class name, which the compiler keeps unique and a display rename leaves standing.
_REF_RE = re.compile(r"@ref\s+(\w+)")
# The attribution catalog: a table whose rows name an entry, linked to its person, project or page when it has one, which a module's `@attribution` refers to by any of its names.
ATTRIBUTIONS = ROOT / "docs" / "reference" / "attributions.md"
_LINKED_RE = re.compile(r"^\[([^\]]+)\]\((https?://[^)\s]+)\)$")
# The role nouns a type name ends in, which its display name drops.
_ROLE_NOUNS = ("Effect", "Modifier", "Layout", "Driver", "Module", "Service")


# ---- the header's comments ------------------------------------------------------------------

def comment_text(raw: str | None) -> str:
    """A `///` or `///<` comment as plain text: the markers gone, the lines kept."""
    if not raw:
        return ""
    lines = [re.sub(r"^\s*///<?\s?", "", ln) for ln in raw.splitlines()]
    return _XREF_RE.sub(lambda m: m.group(2) or m.group(1), "\n".join(lines)).strip()


def paragraphs(text: str) -> list[str]:
    return [p.strip() for p in re.split(r"\n\s*\n", text) if p.strip()]


def block_tags(lead: str) -> tuple[str, dict]:
    """The lead's prose, and its block tags as fields: `{"card": "X.gif", "attribution": "..."}`."""
    prose, fields, tag = [], {}, None
    for ln in lead.split("\n"):
        m = _TAG_RE.match(ln.strip())
        if m and m.group(1) in BLOCK_TAGS:
            tag = m.group(1)
            if tag in REPEATED_TAGS:
                fields.setdefault(tag, []).append(m.group(2).strip())
            else:
                fields[tag] = m.group(2).strip()
        elif tag and ln.strip():
            if tag in REPEATED_TAGS:
                fields[tag][-1] += " " + ln.strip()
            else:
                fields[tag] += " " + ln.strip()
        else:
            tag = None
            prose.append(ln)
    return "\n".join(prose), fields


def class_help(raw: str | None) -> dict:
    """A class comment as help levels: the first paragraph is the line, the rest of the lead the summary, each block tag its field, and each `## ` section after `@guide` a guide section. The `@moreinfo` sections are written for developers and stay in the header, which the help links."""
    parts = _MARKER_RE.split(comment_text(raw))
    lead, appendices = parts[0], dict(zip(parts[1::2], parts[2::2]))
    prose, fields = block_tags(lead)
    paras = paragraphs(prose)
    guide = []
    for sec in re.split(r"^## ", appendices.get("@guide", ""), flags=re.MULTILINE)[1:]:
        title, _, body = sec.partition("\n")
        guide.append({"title": title.strip(), "text": "\n\n".join(paragraphs(body))})
    return {"line": paras[0] if paras else "", "summary": "\n\n".join(paras[1:]),
            "guide": guide, **fields}


# ---- the card, the bridge -------------------------------------------------------------------

def site_url(target: str, page: Path) -> str:
    """A link written on a catalog page, made absolute: a page becomes its URL on the site, an asset its file there, and a path outside docs/ its file on GitHub."""
    if re.match(r"https?://", target):
        return target
    path, _, anchor = target.partition("#")
    resolved = Path(os.path.normpath(page.parent / path)) if path else page
    try:
        rel = resolved.relative_to(ROOT / "docs").as_posix()
    except ValueError:
        return REPO + "main/" + resolved.relative_to(ROOT).as_posix() + (f"#{anchor}" if anchor else "")
    if rel.endswith(".md"):
        rel = rel[:-3] + ".html"   # mkdocs.yml sets use_directory_urls: false
    return SITE + rel + (f"#{anchor}" if anchor else "")


def absolutize(md: str, page: Path) -> str:
    md = _ANCHOR_LINK_RE.sub(lambda m: f"{m.group(1)}{site_url('#' + m.group(2), page)})", md)
    return _LINK_RE.sub(lambda m: m.group(1) + site_url(m.group(2), page), md)


# What the card reader leaves out because help shows inline text only, counted so the run says how much a card holds that its help does not.
DROPPED = {"code block": 0, "table": 0, "html": 0}


def plain(lines: list[str]) -> list[str]:
    """Card markdown as the help's inline text: a fenced block, a table and an HTML line dropped and counted, an inline tag reduced to its text, a heading made bold, a bullet that is not a control kept as a line; the picture and the controls pass as written."""
    out, fenced = [], False
    for ln in lines:
        s = ln.strip()
        if s.startswith("```"):
            if not fenced:
                DROPPED["code block"] += 1
            fenced = not fenced
        elif fenced:
            continue
        elif s.startswith("|"):
            DROPPED["table"] += s.startswith("|---") or s.startswith("| ---")
        elif s.startswith("<") and not s.startswith(("<a ", "<img")):
            DROPPED["html"] += 1
        elif m := re.match(r"#{2,6}\s+(.+)", s):
            out.append(f"**{m.group(1)}**")
        else:
            out.append(re.sub(r"</?(?:span|br|em|strong|small|sup|sub|kbd)\b[^>]*>", "", re.sub(r"^(\s*)- (?!`)", r"\1", ln)))
    return out


def card_block(page: Path, anchor: str):
    """The card a registry link points at, as (name, its lines, the page's lines); None when the page has no such card."""
    from mkdocs_hooks import _ANCHOR_RE, card_name, split_blocks
    if not page.exists():
        return None
    markdown = page.read_text(encoding="utf-8")
    lines = markdown.split("\n")
    for title, start, end in split_blocks(markdown):
        ids = {m.group("id") for ln in lines[max(0, start - 3):start] if (m := _ANCHOR_RE.match(ln))}
        if anchor in ids or _slug(card_name(title)) == anchor:
            return card_name(title), lines[start + 1:end], lines
    return None


def details_guide(page: Path, name: str, lines: list[str]) -> list:
    """A card's `## <Name>, details` section on its page, read as a guide section while it lives there: the bridge for what a page says beside a card."""
    from mkdocs_hooks import _H2_RE, _DETAILS_RE
    guide = []
    for i, ln in enumerate(lines):
        if (m := _DETAILS_RE.match(ln)) and m.group("name") == name:
            stop = next((j for j in range(i + 1, len(lines)) if _H2_RE.match(lines[j])), len(lines))
            body = absolutize("\n".join(plain(lines[i + 1:stop])), page)
            guide.append({"title": f"{name}, details", "text": "\n\n".join(paragraphs(body))})
    return guide


def card_help(page: Path, anchor: str) -> dict | None:
    """The card a registry link points at, read as help. None when the page has no such card, or keeps only its heading, as a module whose header carries its help does."""
    from mkdocs_hooks import _split_control
    found = card_block(page, anchor)
    if not found:
        return None
    name, block, lines = found
    image, alt, prose, controls, attribution = None, "", [], {}, ""
    for ln in plain(block):
        ln = ln.strip()
        if m := re.match(r'<img\b[^>]*src="([^"]+)"', ln):
            image = site_url(m.group(1), page)
            alt = (re.search(r'alt="([^"]*)"', ln) or [None, ""])[1]
        elif ln.startswith("- `"):
            # `- `name`: text`, or several names sharing one text: `- `width` / `height`: text`.
            head, sep, tail = _split_control(ln[2:])
            if sep:
                text = absolutize(tail[:1].upper() + tail[1:], page)
                for cname in re.findall(r"`([^`]+)`", head):
                    controls[cname] = {"line": text}
        elif m := re.match(r"Origin:\s*(.+)", ln):
            attribution = absolutize(m.group(1), page)
        elif ln and not re.match(r"(Detail|Tests):|\[Tests\]|<a id=", ln):
            prose.append(absolutize(ln, page))
    if not (image or prose or controls):
        return None
    first = _SENTENCE_RE.split(prose[0], 1) if prose else [""]
    summary = "\n\n".join(([first[1]] if len(first) > 1 else []) + prose[1:])
    return {"line": first[0], "summary": summary, "attribution": attribution, "guide": details_guide(page, name, lines),
            "image": image, "alt": alt, "controls": controls}


def _slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


# ---- clang ----------------------------------------------------------------------------------

# The registry's compile flags. Its desktop entry in compile_commands.json holds these and only these, so the generator needs no build: the docs job parses the tree as it is checked out.
CLANG_ARGS = ["-x", "c++", "-std=c++20", "-DNDEBUG", f"-I{ROOT / 'src'}",
              f"-I{ROOT / 'src' / 'platform' / 'desktop'}", "-Wno-unknown-warning-option"]


def load_clang():
    """libclang and the flags that find the standard headers. The library comes from the LLVM `check_clang_query` uses when one is installed, else from the `libclang` wheel; the resource dir comes from that LLVM's clang, as the wheel ships no headers."""
    import clang.cindex as ci
    from check_clang_tidy import _find_tool, _toolchain_args
    anchor = _find_tool("clang-query") or _find_tool("clang")
    if not anchor:
        sys.exit("clang not found: install LLVM (brew install llvm, apt install clang).")
    llvm = Path(anchor).resolve().parent.parent
    lib = next(iter(sorted((llvm / "lib").glob("libclang.*"))), None)
    if lib:
        ci.Config.set_library_file(str(lib))
    clang = llvm / "bin" / "clang"
    resource = subprocess.run([str(clang if clang.exists() else anchor), "-print-resource-dir"],
                              capture_output=True, text=True).stdout.strip()
    return ci, (["-resource-dir", resource] if resource else []) + _toolchain_args()


def _referenced_field(expr):
    """The member an argument names, through casts and array decay; None for a temporary."""
    for node in expr.walk_preorder():
        ref = node.referenced
        if ref is not None and ref.kind.name in ("FIELD_DECL", "VAR_DECL"):
            return ref
    return None


def _literal(expr) -> str | None:
    for tok in expr.get_tokens():
        if tok.kind.name == "LITERAL" and tok.spelling.startswith('"'):
            return tok.spelling[1:-1]
        return None
    return None


def _line_comment(call) -> str:
    """The `///` run above an `add…` line, or a `///<` behind it: the help of a control with no member."""
    src = Path(call.location.file.name).read_text(encoding="utf-8").splitlines()
    line = call.location.line
    after = src[line - 1].partition("///<")
    if after[1]:
        return after[2].strip()
    run, k = [], line - 2
    while k >= 0 and src[k].strip().startswith("///") and not src[k].strip().startswith("///<"):
        run.insert(0, src[k].strip()[3:].strip())
        k -= 1
    return "\n".join(run)


def control_help(text: str) -> dict:
    """A control's comment as its help: the first line is its line, and a `@guide` line below it its longer help, Doxygen's brief and detailed description."""
    # Only a line opening `@guide`, so the word inside a sentence stays text.
    m = re.search(r"(?m)^@guide\b", text)
    line, more = (text[:m.start()], text[m.end():]) if m else (text, "")
    return {"line": line.strip(), **({"more": more.strip()} if more.strip() else {})}


def controls_of(cls) -> dict:
    """Each control the `defineControls` the class runs publishes, in order, with its comment: the `///` on the member the `add…` call names, else the run above the call or a `///<` behind it; empty when there is neither. A call to one of the class's own methods is followed as the virtual call resolves it, so a base's template method and its helpers count, and a branch on a method returning a literal `true` or `false` follows that value."""
    controls, entered = {}, set()

    def own(node):
        """The method the class runs for a call to one of its own or its bases' methods, else None."""
        ref = node.referenced
        if node.kind.name != "CALL_EXPR" or ref is None or ref.kind.name != "CXX_METHOD":
            return None
        return _runs(cls, node.spelling) if _is_base_or_self(cls, ref.semantic_parent) else None

    def visit(node):
        if node.kind.name == "IF_STMT":
            kids = list(node.get_children())
            cond = next((n for n in kids[0].walk_preorder() if own(n) is not None), None) if kids else None
            value = _returns(cls, cond.spelling) if cond is not None else []
            if value in (["true"], ["false"]):
                # The branch a literal return takes, flipped by a leading `!`; the else is kids[2] when there is one.
                negated = next((t.spelling for t in kids[0].get_tokens()), "") == "!"
                for k in (kids[1:2] if (value == ["true"]) != negated else kids[2:]):
                    visit(k)
                return
        if node.kind.name == "CALL_EXPR":
            args = list(node.get_arguments())
            # A local lambda named `add…` publishes the same way, its call being `operator()` on the variable.
            callee = node.spelling if node.spelling != "operator()" else \
                next((n.spelling for n in node.get_children() if n.kind.name in ("DECL_REF_EXPR", "UNEXPOSED_EXPR")), "")
            if node.spelling == "operator()" and args and _literal(args[0]) is None:
                args = args[1:]   # the lambda object itself leads the arguments
            name = _literal(args[0]) if args and callee.startswith("add") else None
            if name:
                field = _referenced_field(args[1]) if len(args) > 1 else None
                controls[name] = (comment_text(field.raw_comment) if field is not None else "") \
                    or _line_comment(node)
                return
            method = own(node)
            if method is not None and method.hash not in entered:
                entered.add(method.hash)
                visit(method)
                return
        for child in node.get_children():
            visit(child)

    root = _runs(cls, "defineControls")
    if root is not None:
        visit(root)
    return controls


def _is_base_or_self(cls, other) -> bool:
    """Whether `other` is the class or one of its bases, the classes whose methods a call on `this` reaches."""
    queue, seen = [cls], set()
    while queue:
        c = queue.pop(0)
        if c is None or c.hash in seen:
            continue
        if c.spelling == other.spelling:
            return True
        seen.add(c.hash)
        queue += [(b.referenced.get_definition() or b.referenced) for b in c.get_children()
                  if b.kind.name == "CXX_BASE_SPECIFIER" and b.referenced is not None]
    return False


def _runs(cls, name):
    """The definition of method `name` the class runs: its own, else the nearest base's, as the virtual call resolves it. A method the class only declares is defined in its header's `.cpp`, which is read for it."""
    queue, seen = [cls], set()
    while queue:
        c = queue.pop(0)
        if c.hash in seen:
            continue
        seen.add(c.hash)
        for m in c.get_children():
            if m.kind.name == "CXX_METHOD" and m.spelling == name:
                return m if m.is_definition() else (m.get_definition() or _out_of_line(m))
        queue += [(b.referenced.get_definition() or b.referenced) for b in c.get_children()
                  if b.kind.name == "CXX_BASE_SPECIFIER" and b.referenced is not None]
    return None


# The clang bindings and flags `generate` loaded, and each `.cpp` read so far, since the registry's unit holds headers only.
_CLANG: list = []
_CPP_UNITS: dict = {}


def _out_of_line(decl):
    """The definition of a method its class only declares, from the `.cpp` beside the header; None when there is none."""
    if not _CLANG or decl.location.file is None:
        return None
    cpp = Path(decl.location.file.name).with_suffix(".cpp")
    if not cpp.exists():
        return None
    ci, extra = _CLANG
    if cpp not in _CPP_UNITS:
        _CPP_UNITS[cpp] = ci.Index.create().parse(str(cpp), args=CLANG_ARGS + extra)
    owner = decl.semantic_parent.spelling
    return next((c for c in _CPP_UNITS[cpp].cursor.walk_preorder()
                 if c.kind.name == "CXX_METHOD" and c.spelling == decl.spelling and c.is_definition()
                 and c.semantic_parent is not None and c.semantic_parent.spelling == owner), None)


def _returns(cls, name) -> list[str]:
    """The tokens of what method `name` returns, `ModuleRole :: Effect` or a string literal."""
    m = _runs(cls, name)
    toks = [t.spelling for t in m.get_tokens()] if m is not None else []
    if "return" not in toks:
        return []
    rest = toks[toks.index("return") + 1:]
    rest = rest[:rest.index(";")] if ";" in rest else rest
    if len(rest) == 1 and re.fullmatch(r"\w+", rest[0]) and m is not None:
        # A named constant or a member, `return kTags;` or `return dim_;`: its initializer is the value.
        decl = next((n.referenced for n in m.walk_preorder()
                     if n.kind.name in ("DECL_REF_EXPR", "MEMBER_REF_EXPR") and n.spelling == rest[0]
                     and n.referenced is not None), None)
        dt = [t.spelling for t in decl.get_tokens()] if decl is not None else []
        for opener in ("=", "{"):
            if opener in dt:
                init = dt[dt.index(opener) + 1:]
                return [t for t in init if t not in (";", "}")]
    return rest


def identity(cls) -> dict:
    """What /api/types says of a type, read from the code: its role (ModuleRole lowercased, as roleName gives it), its tags() and its dimensions(), so the docs site's chips match the app's."""
    role = _returns(cls, "role")
    tags = "".join(t[1:-1] for t in _returns(cls, "tags") if t.startswith('"'))
    dim = _returns(cls, "dimensions")
    name = role[-1].lower() if role else "generic"
    out = {"role": name if name in ("effect", "modifier", "driver", "layout", "layer", "service") else "generic",
           "tags": tags,
           "dim": int(dim[-1][1:]) if dim and re.fullmatch(r"D\d", dim[-1]) else 0}
    if out["role"] == "modifier":
        # Dynamic re-applies per frame, which is what hasModifyLive says; animated keeps a static fold and rebuilds it on its own clock, a tick of its own asking through consumeNeedsRebuild; static is baked into the mapping.
        own = lambda name: (m := _runs(cls, name)) is not None and m.semantic_parent.spelling == cls.spelling
        out["kind"] = ("dynamic" if _returns(cls, "hasModifyLive") == ["true"]
                       else "animated" if own("tick") and own("consumeNeedsRebuild") else "static")
    return out


def registrations(tu, ci):
    """Each `registerType<Class>("Type", "page.md#anchor")` in the registry, in order."""
    for call in tu.cursor.walk_preorder():
        if (call.kind != ci.CursorKind.CALL_EXPR or call.spelling != "registerType"
                or not call.location.file or not call.location.file.name.endswith(REGISTRY)):
            continue
        callee = next(call.get_children())
        cls = next((n.referenced for n in callee.walk_preorder()
                    if n.kind == ci.CursorKind.TYPE_REF and n.referenced.spelling != "ModuleFactory"),
                   None)
        args = [_literal(a) for a in call.get_arguments()]
        if cls is not None and args and args[0]:
            yield args[0], cls.get_definition() or cls, (args[1] if len(args) > 1 else "") or ""


# ---- the file -------------------------------------------------------------------------------

def asset_url(fname: str) -> str | None:
    """A picture or clip `@card` and `@video` name, a file under docs/assets/, as its URL on the site."""
    hits = sorted((ROOT / "docs" / "assets").rglob(fname))
    return SITE + hits[0].relative_to(ROOT / "docs").as_posix() if hits else None


def display_name(type_name: str) -> str:
    """A type without its role noun, as the app's typeDisplayName and ModuleFactory::displayNameFor name it."""
    noun = next((n for n in _ROLE_NOUNS if len(type_name) > len(n) and type_name.endswith(n)), "")
    return type_name[:-len(noun)] if noun else type_name


def technical_url(cls) -> str:
    """A class's technical page, its header's page that gen_api renders, as the app's technicalUrl derives it from a source link."""
    src = Path(cls.location.file.name).resolve().relative_to(ROOT / "src")
    return f"{SITE}moonmodules/{src.parts[0]}/moxygen/{src.stem}.html"


def catalog_rows(text: str):
    """Each row of the catalog's table: its line number, its names, its link and its picture, None for an entry with no page or none. The names are the first cell's text split on ` / `, a person and their company or handle being one entry, and a trailing `File:<name>` is its picture on Wikimedia Commons."""
    body = False
    for i, ln in enumerate(text.split("\n")):
        if not ln.startswith("|"):
            body = False
        elif ln.startswith("|-"):
            body = True
        elif body:
            cell, _, picture = ln.split("|")[1].strip().partition(" File:")
            m = _LINKED_RE.match(cell.strip())
            names, url = (m.group(1), m.group(2)) if m else (cell.strip(), None)
            yield i, [n.strip() for n in names.split(" / ")], url, picture.strip() or None


def attribution_catalog(path: Path = ATTRIBUTIONS) -> dict:
    """The catalog's names, each lowercased to its entry's link, None for an entry with no page."""
    text = path.read_text(encoding="utf-8") if path.exists() else ""
    return {n.lower(): url for _, names, url, _ in catalog_rows(text) for n in names}


def resolve_attribution(text: str, catalog: dict) -> tuple[str, list[str]]:
    """An attribution's comma-separated names, each in the catalog linked to its entry and any other kept as written, and the ones the catalog does not list yet."""
    items = [s.strip() for s in text.split(",")]
    return (", ".join(f"[{s}]({catalog[s.lower()]})" if catalog.get(s.lower()) else s for s in items),
            [s for s in items if s.lower() not in catalog])


def ref_link(name: str, registered: dict, definition) -> str:
    """`@ref name` as a markdown link: a module to its catalog card, named as the app names it, and any other class to its technical page, under its own name; LookupError when no class has that name."""
    type_name, doc = registered.get(name, (None, ""))
    if doc:
        return f"[{display_name(type_name)}]({site_url(doc, CATALOG / 'index.md')})"
    cls = definition(name)
    if cls is None:
        raise LookupError(f"@ref {name}: no class of that name in the registry's translation unit")
    return f"[{name}]({technical_url(cls)})"


def resolve_refs(node, link):
    """Every `@ref Name` in the help's text replaced by `link(Name)`, its markdown link."""
    if isinstance(node, str):
        return _REF_RE.sub(lambda m: link(m.group(1)), node)
    if isinstance(node, dict):
        return {k: resolve_refs(v, link) for k, v in node.items()}
    if isinstance(node, list):
        return [resolve_refs(v, link) for v in node]
    return node


def tests_url(cls_name: str, ref: str) -> str | None:
    stem = re.sub(r"(Effect|Modifier|Layout|Driver|Service)$", "", cls_name)
    for name in dict.fromkeys((cls_name, stem)):
        hits = sorted((ROOT / "test").rglob(f"unit_{name}.cpp"))
        if hits:
            return REPO + ref + "/" + hits[0].relative_to(ROOT).as_posix()
    return None


def header_help(cls) -> dict:
    """A module's help from its header: the class comment's levels, and per control its line and, from the `@guide` line on its member, its more."""
    h = class_help(cls.raw_comment)
    controls = {name: control_help(text) for name, text in controls_of(cls).items()}
    image, _, alt = (h.get("card") or "").partition(" ")
    return {"line": h["line"], "summary": h["summary"], "attribution": h.get("attribution"),
            "guide": h["guide"],
            "image": asset_url(image) if image else None, "alt": alt.strip(), "controls": controls}


def module_help(cls, doc: str, ref: str) -> tuple[dict, str]:
    """One module's entry, and where it came from: `card` while its card has a body, else `header`."""
    page, _, anchor = doc.partition("#")
    entry = card_help(CATALOG / page, anchor) if page else None
    origin = "card"
    if entry:
        # The code says which controls exist; the card only describes them.
        cards = entry["controls"]
        entry["controls"] = {c: cards.get(c, {"line": ""}) for c in controls_of(cls)} or cards
    else:
        entry, origin = header_help(cls), "header"
        # A details section the page still holds beside the card joins the header's guide.
        found = card_block(CATALOG / page, anchor) if page else None
        if found:
            entry["guide"] = entry["guide"] + details_guide(CATALOG / page, found[0], found[2])
    source = Path(cls.location.file.name).resolve().relative_to(ROOT).as_posix()
    entry.update(identity(cls))
    entry["doc"] = doc
    entry["source"] = REPO + ref + "/" + source
    entry["tests"] = tests_url(cls.spelling, ref)
    return {k: v for k, v in entry.items() if v not in (None, "", [], {})}, origin


def generate(ref: str = "main") -> tuple[dict, dict]:
    """The help file, and a report: which modules came from headers and cards, and the controls with no line."""
    ci, extra = load_clang()
    _CLANG[:] = [ci, extra]
    tu = ci.Index.create().parse(str(ROOT / REGISTRY), args=CLANG_ARGS + extra)
    errors = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
    if errors:
        # A translation unit with errors loses declarations silently; a short file is not a clean one.
        sys.exit("clang could not parse the registry:\n  " + "\n  ".join(map(str, errors[:5])))
    modules, report = {}, {"card": [], "header": [], "missing": {}, "unlisted": {}, "missing_assets": []}
    attributions = attribution_catalog()
    regs = list(registrations(tu, ci))
    registered = {cls.spelling: (type_name, doc) for type_name, cls, doc in regs}

    def definition(name):
        return next((c for c in tu.cursor.walk_preorder() if c.kind in (ci.CursorKind.CLASS_DECL, ci.CursorKind.STRUCT_DECL)
                     and c.spelling == name and c.is_definition()), None)

    def link(name):
        return ref_link(name, registered, definition)

    for type_name, cls, doc in regs:
        entry, origin = module_help(cls, doc, ref)
        try:
            entry = resolve_refs(entry, link)
        except LookupError as e:
            sys.exit(f"{type_name}: {e}")
        if entry.get("attribution"):
            entry["attribution"], unlisted = resolve_attribution(entry["attribution"], attributions)
            if unlisted and origin == "header":
                report["unlisted"][type_name] = unlisted
        modules[type_name] = entry
        report[origin].append(type_name)
        missing = [c for c, h in entry.get("controls", {}).items() if not h.get("line")]
        if missing or not entry.get("line"):
            report["missing"][type_name] = missing if entry.get("line") else ["(module)"] + missing
    catalogs = {}
    for page, (title, base) in CATALOGS.items():
        cls = definition(base) if base else None
        if base is None:
            catalogs[page] = {"title": title}
        elif cls is not None:
            try:
                h = resolve_refs(class_help(cls.raw_comment), link)
            except LookupError as e:
                sys.exit(f"{base}: {e}")
            image, _, alt = (h.get("card") or "").partition(" ")
            videos = []
            for v, _, c in (tag.partition(" ") for tag in h.get("video", [])):
                # A clip whose file is missing is reported and left out, so the page never plays a broken source.
                if url := asset_url(v):
                    videos.append({"video": url, "caption": c.strip()})
                else:
                    report["missing_assets"].append(f"{base}: {v}")
            catalogs[page] = {k: v for k, v in {"title": title, "base": base, "line": h["line"], "summary": h["summary"],
                                                 "image": asset_url(image) if image else None, "alt": alt.strip(),
                                                 "videos": videos,
                                                 "guide": h["guide"], "attribution": h.get("attribution")}.items() if v}
    return {"version": ref, "modules": modules, "catalogs": catalogs}, report


def render(data: dict) -> str:
    """The file's text: one line per field, so a header's edit diffs as the help line it changed."""
    return json.dumps(data, ensure_ascii=False, indent=1) + "\n"


def stale() -> bool:
    """Whether the committed file differs from what the headers and cards generate now."""
    return not HELP.exists() or HELP.read_text(encoding="utf-8") != render(generate()[0])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ref", default="main", help="Git ref the source and test links point at.")
    ap.add_argument("--out", default=str(HELP))
    args = ap.parse_args()
    data, report = generate(args.ref)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(render(data), encoding="utf-8")
    print(f"{out.relative_to(ROOT) if out.is_relative_to(ROOT) else out}: {len(data['modules'])} modules, "
          f"{len(report['header'])} from headers, {len(report['card'])} from cards, "
          f"{out.stat().st_size // 1024} KB")
    dropped = ", ".join(f"{n} {k}{'s' if n != 1 else ''}" for k, n in DROPPED.items() if n)
    if dropped:
        print(f"  left out of card help, which shows inline text only: {dropped}")
    for t, names in sorted(report["missing"].items()):
        print(f"  no help: {t}: {', '.join(names)}")
    for asset in report["missing_assets"]:
        print(f"  no such file under docs/assets/: {asset}")
    for t, names in sorted(report["unlisted"].items()):
        print(f"  not in {ATTRIBUTIONS.relative_to(ROOT)} yet: {t}: {', '.join(names)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
