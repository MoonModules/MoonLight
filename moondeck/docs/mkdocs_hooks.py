"""MkDocs build hooks. Wired via `hooks:` in mkdocs.yml. Three jobs:

1. on_files — synthesise `reference/tests/unit-tests.md` + `reference/tests/scenario-tests.md` into
   MkDocs' virtual file tree from the test files (via `_test_metadata.py`, the
   same parser the CLI generator + MoonDeck use). So the inventory pages are NOT
   committed to the repo and can't drift — rebuilt from source every build. The
   per-effect `[Tests]` links in the catalog pages point at these pages; kept as
   plain one-line links so the effect cards stay compact (a link, not a case dump).

2. on_page_markdown — repoint links that escape docs/ into repo files
   (`../src/*.h`, `../CLAUDE.md`, `../moondeck/*`) to absolute GitHub blob URLs,
   since the site can't host `src/` (pre-Doxide) and a relative link would 404.

3. on_post_build (serve-only) — stage the web installer into the built site under
   /install/ so the local preview mirrors production's single-origin layout.
"""

import json
import logging
import os
import posixpath
import re
import subprocess
from pathlib import Path

# moondeck/docs/ is on sys.path when mkdocs runs from repo root; import the shared parser.
import sys
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from _test_metadata import (  # noqa: E402
    ROOT,
    collect_unit_files,
    parse_unit_file,
    collect_scenario_files,
)
from generate_test_docs import render_unit_tests, render_scenarios  # noqa: E402
from gen_help import ATTRIBUTIONS, catalog_rows, display_name  # noqa: E402
from gen_moonwalk import build as build_moonwalk, absolute_links, render_text as moonwalk_text, split_text as split_moonwalk  # noqa: E402


# ---- source-link rewrite: docs link OUT to repo files the site can't host ----

# A link that escapes docs/ into the repo (src/, moondeck/, test/, CLAUDE.md, README.md, mooninstaller/) 404s on the site, which hosts none of it, so it becomes the file on GitHub. A header is the technical reference this way: its `///` comments read in place.
def _blob_base() -> str:
    """The GitHub blob URL a source link is built on, pinned to the branch being documented, so a page built on a feature branch links a file main may not have yet; main without git or on a detached head."""
    branch = "main"
    try:
        out = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"],
                             cwd=ROOT, capture_output=True, text=True, timeout=5)
        name = out.stdout.strip()
        if out.returncode == 0 and name and name != "HEAD":
            branch = name
    except (OSError, subprocess.SubprocessError):
        pass
    return f"https://github.com/MoonModules/MoonLight/blob/{branch}"


_BLOB_BASE = _blob_base()


def _repo_url(path: str) -> str:
    """A repo path on GitHub: a folder under /tree/, as GitHub names it, a file under /blob/."""
    base = _BLOB_BASE.replace("/blob/", "/tree/", 1) if (ROOT / path.split("#", 1)[0]).is_dir() else _BLOB_BASE
    return f"{base}/{path}"

# Repo top-level dirs/files a doc may link into but the site doesn't host.
_OUT_OF_DOCS = ("src/", "moondeck/", "test/", "esp32/", "mooninstaller/", "moonwalk/",
                ".github/", "CLAUDE.md", "README.md", "library.json", "CMakeLists.txt",
                ".clang-tidy", ".clangd")

# A markdown link into a repo file the site doesn't host. Two authored shapes, both
# common in the transient docs/work/ notes:
#   ../../src/foo   — climbs out of docs/ with `../`
#   src/foo         — repo-ROOT-relative, no `../` (resolves to a nonexistent docs/src/foo)
# The pattern accepts an optional `../` run, then the href; _sub sorts out which case.
_OUT_LINK_RE = re.compile(r'\]\((?P<href>(?:\.\./)*(?P<rest>[^)#]+)(?P<frag>#[^)]*)?)\)')

# The repo-root-relative prefixes a bare (no-`../`) link must start with to be treated
# as an out-of-docs source link. `docs/` is included because a plan that links
# `docs/plan.md` / `docs/architecture-light.md` (files that never existed or were
# renamed) still resolves outside the current page and 404s — send it to GitHub.
_ROOT_REL_PREFIXES = _OUT_OF_DOCS + ("docs/",)


def _rewrite_out_of_docs_links(markdown: str, src_uri: str) -> str:
    """Rewrite links that climb out of docs/ into repo files → absolute GitHub blob
    URLs, resolved against the page's own location so any `../` depth is handled.
    src_uri is the page's path under docs/ (e.g. 'moonmodules/light/effects.md')."""
    # The page's directory within docs/ — the anchor `../` hops resolve against.
    page_dir = Path("docs") / src_uri
    page_dir = page_dir.parent

    def _sub(m: re.Match) -> str:
        href = m.group("href")
        frag = m.group("frag") or ""
        rel = href.split("#", 1)[0]
        # Resolve against the page dir to a repo-relative path (the correct case).
        target = os.path.normpath(str(page_dir / rel)).replace(os.sep, "/")
        if target.startswith(_OUT_OF_DOCS) and not target.startswith("docs/"):
            return f"]({_repo_url(target)}{frag})"
        # Fallback for links authored relative to the REPO ROOT (common in the
        # transient docs/work/ notes, written before their file sat
        # this deep under docs/): strip leading `../` and see if the remainder is
        # itself an out-of-docs repo path. Catches `../../moondeck/x.py` from a
        # docs/work/past/plans/ page, which resolves to a nonexistent docs/moondeck/x.py,
        # AND a bare `src/x.js` / `docs/plan.md` with no `../` at all (same intent,
        # written root-relative) — both 404 in-site, so send them to the GitHub blob.
        stripped = re.sub(r'^(?:\.\./)+', '', rel)
        if stripped.startswith(_ROOT_REL_PREFIXES):
            return f"]({_repo_url(stripped)}{frag})"
        return m.group(0)  # in-docs link (incl. ../ into another docs page) — leave it

    return _OUT_LINK_RE.sub(_sub, markdown)


# ---- MkDocs hooks ----

# ---- catalog-page table transform: render prose ### blocks as a MoonLight-style table ----

# The module types whose preview is MOTION, so their image is a .gif rather than a .png:
# a still frame of a moving effect says almost nothing about it. One home, because three
# places act on it: check_docgen enforces the format, screenshot_modules captures it, and
# the catalog pages carry the result.
ANIMATED_TYPES = ("effects", "modifiers", "layouts")
ANIMATED_PAGES = frozenset(f"moonmodules/light/{t}.md" for t in ANIMATED_TYPES)

# The summary pages, whose rows carry no preview. A catalog card leads with its image
# because a reader picks an effect by looking at it, where these rows describe a base
# class, a binding type or an init-order sequencer: there is no card in the UI to capture.
# The other card rules still apply, since a summary row is read across like any other.
PREVIEWLESS_PAGES = frozenset({
    "moonmodules/core/supporting.md",
    "moonmodules/light/supporting.md",
})

_SITE_RE = re.compile(r"https://moonmodules\.org/MoonLight/([^)\"\s#]*)(#[^)\"\s]*)?")


def _site_relative(md: str, src_uri: str) -> str:
    """The help's absolute site links made relative to the page, so the build checks them and a local preview shows its own files: `moonmodules/core/services.html` is `../core/services.md`."""
    def repl(m: re.Match) -> str:
        path, anchor = m.group(1), m.group(2) or ""
        if path.endswith(".html") or not path:
            path = (path[:-5] or "index") + ".md"
        if path == src_uri:
            return anchor or "#"
        return posixpath.relpath(path, posixpath.dirname(src_uri)) + anchor
    return _SITE_RE.sub(repl, md)


_help_cache = {}


def _help_file() -> dict:
    """docs/help/help.json, read once per build."""
    if "file" not in _help_cache:
        path = Path(__file__).resolve().parent.parent.parent / "docs" / "help" / "help.json"
        _help_cache["file"] = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    return _help_cache["file"]


def generated_catalog(src_uri: str, intro: str | None = None) -> str | None:
    """A catalog page drawn from help.json. The intro is the page's own markdown when it has one, else the class comment its modules share; the deck holds each module as plain markdown, which search indexes and a reader without JavaScript reads until the deck script replaces it, and each module's guide is its details section."""
    page = src_uri.removeprefix("moonmodules/")
    cat = _help_file().get("catalogs", {}).get(page)
    if not cat:
        return None
    # The class whose comment is the intro is the page itself, so it is no card on it.
    mods = sorted(((t, m) for t, m in _help_file().get("modules", {}).items()
                   if (m.get("doc") == page or m.get("doc", "").startswith(page + "#")) and t != cat.get("base")),
                  key=lambda tm: display_name(tm[0]).lower())
    out = [intro.rstrip(), ""] if intro is not None else _class_intro(cat, src_uri)
    out += [f'<div class="mm-deck" data-page="{page}" markdown="1">', ""]
    for t, m in mods:
        out += _module_markdown(t, m, src_uri)
    out += ["</div>", ""]
    for t, m in mods:
        if m.get("guide"):
            out += [f"## {display_name(t)}, details", ""]
            for g in m["guide"]:
                out += [f"#### {g['title']}", "", _site_relative(g["text"], src_uri), ""]
    return "\n".join(out)


def _class_intro(cat: dict, src_uri: str) -> list[str]:
    """A catalog's intro from the class comment its modules share: the title, the picture, the line, the summary and the clips."""
    out = [f"# {cat['title']}", ""]
    if cat.get("image"):
        out += [f'<img src="{_site_relative(cat["image"], src_uri)}" width="300" alt="{cat.get("alt", "")}">', ""]
    out += [_site_relative(cat.get("line", ""), src_uri), ""]
    for para in (p for p in cat.get("summary", "").split("\n\n") if p):
        out += [_site_relative(para, src_uri), ""]
    # The clips share one line, so they sit side by side rather than stacked.
    clips = [f'<video src="{_site_relative(clip["video"], src_uri)}" controls playsinline title="{clip.get("caption", "")}"></video>'
             for clip in cat.get("videos", [])]
    if clips:
        out += [" ".join(clips), ""]
    return out


def catalog_markdown(src_uri: str) -> str | None:
    """A catalog page's markdown: generated from help.json, its own file being the intro when it has one; a page that is no catalog is its file."""
    path = Path(__file__).resolve().parent.parent.parent / "docs" / src_uri
    written = path.read_text(encoding="utf-8") if path.exists() else None
    return generated_catalog(src_uri, written) or written


def _tests_link(url: str, src_uri: str) -> str:
    """The test inventory's section for a test file, which reads its cases as sentences, where the file names its `@module`; else the file on GitHub, the link the help itself carries."""
    rel = url.split("/blob/", 1)[-1].partition("/")[2]
    path = ROOT / rel
    module = parse_unit_file(path).get("module") if rel and path.exists() else None
    if not module:
        return url
    page = posixpath.relpath("reference/tests/unit-tests.md", posixpath.dirname(src_uri))
    return f"{page}#{_slug(module)}"


def _module_markdown(type_name: str, m: dict, src_uri: str) -> list[str]:
    """One module as plain markdown: its anchor and name, picture, line and summary, controls, attribution and links. The name is bold rather than a heading, so the page's table of contents lists no entry the deck replaces."""
    name = display_name(type_name)
    anchor = m["doc"].partition("#")[2] or _slug(name)
    suffix = m.get("kind") or (f"{m['dim']}D" if m.get("dim") else "")
    out = [f'<a id="{anchor}"></a>', "", f"**{name}** {m.get('tags', '')}".rstrip() + (f" · {suffix}" if suffix else ""), ""]
    if m.get("image"):
        out += [f'<img src="{_site_relative(m["image"], src_uri)}" width="300" alt="{m.get("alt") or name + " preview"}">', ""]
    out += [_site_relative(" ".join([m.get("line", "")] + m.get("summary", "").split("\n\n")).strip(), src_uri), ""]
    controls = [f"- `{c}`: {_site_relative(h['line'], src_uri)}" for c, h in m.get("controls", {}).items()]
    if controls:
        out += controls + [""]
    if m.get("attribution"):
        out += [f"Attribution: {_site_relative(m['attribution'], src_uri)}", ""]
    links = ([f"[Source]({m['source']})"] if m.get("source") else []) + ([f"[Tests]({_tests_link(m['tests'], src_uri)})"] if m.get("tests") else [])
    return out + ([" · ".join(links), ""] if links else [])


def _slug(text: str) -> str:
    """The heading anchor id MkDocs generates for a `## Heading`, so a hand-built
    `#anchor` link resolves. Delegates to Python-Markdown's OWN toc slugify (the exact
    function MkDocs' toc uses) rather than re-implementing it — a hand-rolled mimic
    drifts on unicode (accent-stripping), consecutive separators, and decoration.
    e.g. 'LED output — details' -> 'led-output-details'. `markdown` is always present
    in the build env (it's a MkDocs dependency)."""
    from markdown.extensions.toc import slugify
    return slugify(text, "-")


def on_pre_build(config):
    """Forget the last build's help.json, so a live server draws the file as it is now."""
    _help_cache.clear()


def on_files(files, config):
    """Inject the build-time pages into the virtual tree: the test inventories, the help the deck reads, and the catalog pages drawn from help.json."""
    from mkdocs.structure.files import File

    def _add(src_uri: str, content: str):
        f = File.generated(config, src_uri, content=content)
        existing = files.get_file_from_path(src_uri)   # replace a same-URI file if any
        if existing:
            files.remove(existing)
        files.append(f)

    _add("reference/tests/unit-tests.md", render_unit_tests(collect_unit_files()))
    # The card deck renders help with the app's own help.js, served beside assets/help-deck.js, so a card on the site reads as the module's help does on a device. One deck per catalog page.
    _add("assets/help.js", (ROOT / "src" / "ui" / "help.js").read_text(encoding="utf-8"))
    # A catalog page whose markdown is gone is generated from help.json, under the same name, so every link and anchor into it holds.
    for page in _help_file().get("catalogs", {}):
        if not (ROOT / "docs" / "moonmodules" / page).exists():
            _add(f"moonmodules/{page}", generated_catalog(f"moonmodules/{page}"))
    _add("reference/tests/scenario-tests.md", render_scenarios(collect_scenario_files()))
    # MoonWalk's journey map, from the stage files under moonwalk/, and each process's prose beside it, which the view fetches when the process opens; links absolute, since the view renders them on a device's page too. An invalid file fails the build.
    walk, texts = split_moonwalk(absolute_links(build_moonwalk()))
    _add("moonwalk/moonwalk.json", json.dumps(walk, ensure_ascii=False))
    for pid, text in texts.items():
        _add(f"moonwalk/text/{pid}.json", json.dumps(text, ensure_ascii=False))

    return files


# Pages that embed a file living at the REPO ROOT via a pymdownx snippet. Their
# borrowed text spells doc links `docs/explanation/architecture/index.md` — correct from the root (that
# is where CLAUDE.md is read by agents and on GitHub), but one level too deep once the
# text is rendered from inside docs/, where it resolves to docs/docs/… and 404s.
_EMBEDS_REPO_ROOT_FILE = {"contributing/principles-and-process.md"}

# `href="docs/<rest>"` → `href="<rest>"`, and the same for the `.md` → `.html` form MkDocs
# has already applied by this stage. Only the `docs/` prefix is dropped; anchor and path
# ride along untouched.
_DOCS_PREFIXED_HREF_RE = re.compile(r'href="docs/([^"#]+?)(\.md)?(#[^"]*)?"')

# The same borrowed text also links to files OUTSIDE docs/ (`moondeck/MoonDeck.md`), which
# have no page on the site at all. `_rewrite_out_of_docs_links` already sends those to the
# GitHub blob, but it runs at markdown stage where this page is still an unexpanded
# `--8<--` directive — so the embedded ones need the same treatment here.
_OUT_OF_DOCS_HREF_RE = re.compile(
    rf'href="({"|".join(re.escape(p) for p in _OUT_OF_DOCS)})([^"#]*)(#[^"]*)?"')

# The page's first `<h1 ...>text</h1>`, split so the tag and its attributes (the id the
# table of contents anchors on) survive and only the text is replaced.
_FIRST_H1_RE = re.compile(r"(<h1[^>]*>)(.*?)(</h1>)", re.S)


def _rebase_repo_root_doc_links(html):
    """Drop the leading `docs/` from links in content embedded from the repo root.

    Runs on the rendered HTML, not the source markdown: pymdownx expands a `--8<--`
    snippet during markdown *conversion*, so at on_page_markdown time this page is still
    just the one-line directive and there is nothing to rewrite yet.

    The alternative — rewriting CLAUDE.md's own links to be docs-relative — would break
    them everywhere they are actually used (an agent reading the file, GitHub's own
    rendering) to satisfy one embedded copy. Rebasing at build time keeps the source
    correct at the root and correct on the site.

    `.md` becomes `.html` here too. MkDocs normally does that itself, but only for links
    whose target it resolved during validation — and these did not resolve, precisely
    because of the `docs/` prefix this function strips. So they arrive still pointing at
    the source file and would 404 on the built site.
    """
    def _fix(m):
        path, _, anchor = m.group(1), m.group(2), m.group(3) or ""
        return f'href="{path}.html{anchor}"' if _ else f'href="{path}{anchor}"'
    html = _DOCS_PREFIXED_HREF_RE.sub(_fix, html)
    return _OUT_OF_DOCS_HREF_RE.sub(
        lambda m: f'href="{_BLOB_BASE}/{m.group(1)}{m.group(2)}{m.group(3) or ""}"', html)


class _MuteRebasedLinkWarnings(logging.Filter):
    """Drop the link warnings for the page whose links this hook rebases after validation.

    `principles-and-process.md` embeds CLAUDE.md, whose links (`docs/explanation/architecture/index.md`,
    `moondeck/MoonDeck.md`) are correct where that file is actually read — the repo root and
    GitHub — and `_rebase_repo_root_doc_links` turns them into working site links. But
    validation is a markdown treeprocessor inside `Page.render()`, so it judges the
    pre-rebase text and reports 13 broken links the built site does not have.

    Filtering the log is the narrow fix. `validation.links.not_found: ignore` in mkdocs.yml
    would hide genuinely missing pages across every other doc, and MkDocs has no per-page
    validation setting; the `inclusion` levels that do suppress warnings (`DRAFT` and below)
    also drop the page from the built site.
    """

    def filter(self, record):
        msg = record.getMessage()
        return not any(f"Doc file '{p}' contains a link" in msg for p in _EMBEDS_REPO_ROOT_FILE)


logging.getLogger("mkdocs.structure.pages").addFilter(_MuteRebasedLinkWarnings())


def on_page_content(html, page, config, files):
    """Post-conversion fixups for a page that embeds a repo-root file: rebase its doc
    links, and replace the embedded `# <filename>` heading with the page's own title.

    The embedded file opens with a heading that names it as a file (`# CLAUDE.md`) —
    right at the repo root and on GitHub, wrong in a docs menu, where the reader wants
    the subject. Front matter `title:` fixes the nav entry and the browser tab but not
    the rendered H1, so it is swapped here rather than by editing the source file."""
    if page.file.src_uri not in _EMBEDS_REPO_ROOT_FILE:
        return html
    html = _rebase_repo_root_doc_links(html)
    if page.title:
        html = _FIRST_H1_RE.sub(
            lambda m: f"{m.group(1)}{page.title}{m.group(3)}", html, count=1)
    return html


_GITHUB_OWNER_RE = re.compile(r"^https://github\.com/([A-Za-z0-9-]+)(?:/|$)")


# A source file's own `@attribution` line: a header's or a `.cpp`'s comment, or a script's docstring.
_SOURCE_ATTRIBUTION_RE = re.compile(r"^\s*(?:///|//|#)?\s*@attribution\s+(.+?)\s*$")


def _source_attributions(modules: dict) -> list[tuple[str, str]]:
    """Each source file outside the modules that names attributions, as (its link on GitHub, the names): code that is no module, such as a power function or MoonBase, credits where its idea lives."""
    module_sources = {m.get("source", "").partition("/blob/")[2].partition("/")[2] for m in modules.values()}
    found = subprocess.run(["git", "grep", "-n", "-F", "@attribution", "--", "src", "moonbase", "moondeck", "esp32"],
                           cwd=ROOT, capture_output=True, text=True)
    if found.returncode > 1:   # 1 is no match; more is no repository, which would drop every credit in silence
        raise RuntimeError(f"git grep for @attribution failed: {found.stderr.strip()}")
    out = []
    for hit in found.stdout.splitlines():
        rel, _, text = hit.partition(":")
        m = _SOURCE_ATTRIBUTION_RE.match(text.partition(":")[2])
        if m and rel not in module_sources:
            out.append((f"[{Path(rel).name}]({_BLOB_BASE}/{rel})", m.group(1)))
    return sorted(out)


def deck_around_table(markdown: str) -> str:
    """The page's table inside a deck, which the deck script turns into cards; the table stays for search and for a reader without JavaScript."""
    lines = markdown.split("\n")
    start = next((i for i, ln in enumerate(lines) if ln.startswith("|")), None)
    if start is None:
        return markdown
    end = next((i for i in range(start, len(lines)) if not lines[i].startswith("|")), len(lines))
    return "\n".join(lines[:start] + ['<div class="mm-deck" markdown="1">', ""] + lines[start:end] + ["", "</div>"] + lines[end:])


def attributed_by(markdown: str) -> str:
    """The attribution catalog as the site shows it. Under each name goes its picture: the Wikimedia Commons file the row names, else its GitHub avatar. The Attributed by column lists the modules naming the entry by link or name, read from help.json, then the other source files whose `@attribution` names it, so the page lists none by hand."""
    rows = {i: (names, url, picture) for i, names, url, picture in catalog_rows(markdown)}
    users = {i: [] for i in rows}
    modules = _help_file().get("modules", {})
    credits = []
    for t, m in sorted(modules.items(), key=lambda tm: display_name(tm[0]).lower()):
        page, _, anchor = m.get("doc", "").partition("#")
        credits.append((f"[{display_name(t)}](../moonmodules/{page}#{anchor})" if page else display_name(t), m.get("attribution", "")))
    credits += _source_attributions(modules)
    for name, attr in credits:
        urls = set(re.findall(r"\]\((https?://[^)\s]+)\)", attr))
        said = {re.sub(r"^\[([^\]]+)\]\(.*\)$", r"\1", s.strip()).lower() for s in attr.split(",")}
        said |= {s.lower() for s in re.findall(r"\[([^\]]+)\]\(", attr)}
        for i, (names, url, _) in rows.items():
            if ((url and url in urls) or any(n.lower() in said for n in names)) and name not in users[i]:
                users[i].append(name)
    out = []
    for i, ln in enumerate(markdown.split("\n")):
        if i in rows:
            _, url, picture = rows[i]
            cell = ln.split("|")[1]
            name = cell.partition(" File:")[0].strip()
            owner = _GITHUB_OWNER_RE.match(url or "")
            img = '<img src="{}" width="48" height="48" alt="" loading="lazy" style="width: 48px; height: 48px; object-fit: cover">'
            if picture:
                # The file page credits the picture's author and license, as reuse of a Commons file asks.
                name += f'<br><a href="https://commons.wikimedia.org/wiki/File:{picture}">{img.format(f"https://commons.wikimedia.org/wiki/Special:FilePath/{picture}?width=96")}</a>'
            elif owner:
                name += "<br>" + img.format(f"https://github.com/{owner.group(1)}.png?size=96")
            ln = ln.replace(cell, f" {name} ", 1) + f" {', '.join(users[i])} |"
        elif ln.startswith("| Name "):
            ln += " Attributed by |"
        elif ln.startswith("|---"):
            ln += "---|"
        out.append(ln)
    return "\n".join(out)


def on_page_markdown(markdown, page, config, files):
    """Repoint out-of-docs source links to GitHub blob URLs, follow a written catalog page with its deck, and give the attribution catalog its pictures and the modules naming each entry."""
    # The MoonWalk page carries every process's prose as plain text, which search indexes and a reader without JavaScript reads; moonwalk.js hides it once the live view draws. Appended first, so its links leaving docs/ are rewritten like the page's own.
    if page.file.src_uri == "moonwalk.md":
        markdown += '\n<div class="mm-walk-text" markdown>\n\n' + moonwalk_text(build_moonwalk()) + '\n</div>\n'
    markdown = _rewrite_out_of_docs_links(markdown, page.file.src_uri)
    # A written catalog page is the intro its generated deck follows.
    if page.file.abs_src_path and Path(page.file.abs_src_path).exists():
        markdown = generated_catalog(page.file.src_uri, markdown) or markdown
    if page.file.abs_src_path and Path(page.file.abs_src_path) == ATTRIBUTIONS:
        markdown = deck_around_table(attributed_by(markdown))
    return markdown


def on_post_build(config):
    """LOCAL PREVIEW ONLY: stage the web installer into the built site under
    /install/, so the local docs preview mirrors the SINGLE-ORIGIN production
    layout (docs at /, installer at /install/). Without it the docs preview
    (:8422) 404s on the Flash link's /MoonLight/install/ target — the installer
    preview is a separate server on :8421, and a production-relative link can't
    reach it. Copying it in makes every /install/ link resolve locally exactly
    as deployed.

    Gated to serve mode via MM_DOCS_SERVE (set by build_docs.py --serve): in CI,
    release.yml does its OWN, more complete install/ staging (incl. the
    releases/<tag>/ firmware binaries), so this must NOT run there and risk
    overlaying the binary-less repo copy over it. Mirrors release.yml's
    `cp -r mooninstaller/. pages/install/` + the install-picker*.js + library.json
    siblings. Best-effort: absent files are skipped."""
    import os
    import shutil

    if os.environ.get("MM_DOCS_SERVE") != "1":
        return  # CI / plain build — release.yml owns install/ staging

    site = Path(config["site_dir"])
    dst = site / "install"
    src = ROOT / "mooninstaller"
    if not src.is_dir():
        return
    dst.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, dst, dirs_exist_ok=True)
    # The picker's JS halves live in src/ui/ (embedded in firmware too); CI stages
    # them beside the installer. library.json feeds the installer its version.
    for extra in ("src/ui/install-picker.js", "src/ui/install-picker-devices.js", "library.json"):
        p = ROOT / extra
        if p.is_file():
            shutil.copy(p, dst / p.name)
