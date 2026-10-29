#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# ///
"""Mirror the GitHub Project to docs/work/backlog_mirror/, one Markdown document per Area plus cards.json for builds to read, and to build/project.json for searching.

The cards are the backlog's one home; the documents are generated from them, so the repository versions every edit and a reader reads the backlog as documents.
`--check` renders without writing and fails when a document differs from what the board renders, which catches a hand edit and a forgotten run alike; without a network it skips.
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import urllib.parse
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_OUT = ROOT / "build" / "project.json"
DEFAULT_MD = ROOT / "docs" / "work" / "backlog_mirror"
REPO_URL = "https://github.com/MoonModules/MoonLight/blob/main/"
FIELDS = ("status", "area", "module", "kind", "trigger", "platform", "stage", "process")
DATA_FILE = "cards.json"   # the open cards as data, beside the per-Area documents


def owner_env(owner: str) -> dict:
    """The environment for `gh`, carrying the owner's token when `gh` holds one for that account."""
    env = dict(os.environ)
    r = subprocess.run(["gh", "auth", "token", "--user", owner], capture_output=True, text=True)
    if r.returncode == 0 and r.stdout.strip():
        env["GH_TOKEN"] = r.stdout.strip()
    return env


def fetch(owner: str, number: int) -> dict:
    """The Project's items as `gh project item-list` reports them; raises when `gh` cannot read the board."""
    r = subprocess.run(
        ["gh", "project", "item-list", str(number), "--owner", owner, "--format", "json", "--limit", "5000"],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", env=owner_env(owner),
    )
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip() or f"gh exited {r.returncode}")
    return json.loads(r.stdout)


def card_ids(owner: str, number: int) -> dict[str, str]:
    """Each item's database id by its node id: the number a card's URL carries."""
    ids, after = {}, None
    while True:
        q = ('query($o:String!,$n:Int!,$a:String){user(login:$o){projectV2(number:$n){items(first:100,after:$a)'
             '{pageInfo{hasNextPage endCursor} nodes{id fullDatabaseId}}}}}')
        args = ["gh", "api", "graphql", "-f", f"query={q}", "-F", f"o={owner}", "-F", f"n={number}"] + (["-F", f"a={after}"] if after else [])
        r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", env=owner_env(owner))
        if r.returncode != 0:
            raise RuntimeError(r.stderr.strip() or f"gh exited {r.returncode}")
        page = json.loads(r.stdout)["data"]["user"]["projectV2"]["items"]
        ids.update({n["id"]: n["fullDatabaseId"] for n in page["nodes"]})
        if not page["pageInfo"]["hasNextPage"]:
            return ids
        after = page["pageInfo"]["endCursor"]


def card_url(project_url: str, database_id: str) -> str:
    return f"{project_url}?pane=issue&itemId={database_id}" if database_id else ""


def normalize(raw: dict, ids: dict | None = None, project_url: str = "") -> list[dict]:
    """One flat record per item, its fields always present and empty when unset, ordered by area then title so a re-run on an unchanged board writes the same file."""
    items = []
    for pos, it in enumerate(raw.get("items", [])):
        content = it.get("content") or {}
        rec = {
            "id": it.get("id", ""),
            "position": pos,
            "type": content.get("type", ""),
            "title": it.get("title") or content.get("title", ""),
            "number": content.get("number"),
            "url": content.get("url", ""),
        }
        for f in FIELDS:
            rec[f] = it.get(f) or ""
        rec["body"] = content.get("body", "")
        rec["card"] = card_url(project_url, (ids or {}).get(rec["id"], ""))
        items.append(rec)
    return sorted(items, key=lambda r: (r["area"], r["title"].lower()))


def field_line(r: dict) -> str:
    """The fields under a title: Kind first, a default left out."""
    parts = [f"Kind: {r['kind']}"] if r["kind"] else []
    if r["platform"] not in ("", "all"):
        parts.append(f"Platform: {r['platform']}")
    parts += [f"{k.capitalize()}: {r[k]}" for k in ("trigger", "stage", "process") if r[k]]
    if r["status"] not in ("", "Backlog"):
        parts.append(f"Status: {r['status']}")
    if r["number"]:
        parts.append(f"Issue: [#{r['number']}]({r['url']})")
    if r.get("card"):
        parts.append(f"[Card]({r['card']})")
    return " · ".join(parts)


def relative_links(body: str, folder: str) -> str:
    """A card's links into this repository, made relative to the document's folder, so the docs build checks them and they work in a checkout."""
    def fix(m):
        path, _, anchor = m.group(1).partition("#")
        rel = os.path.relpath(path, folder).replace(os.sep, "/")
        return f"]({rel}{'#' + anchor if anchor else ''})"
    return re.sub(r"\]\(" + re.escape(REPO_URL) + r"([^)\s]+)\)", fix, body)


def file_name(area: str) -> str:
    return f"{(area or 'no-area').lower().replace(' ', '-')}.md"


def render(items: list[dict], project_url: str, folder: str = "docs/work/backlog_mirror") -> dict[str, str]:
    """One document per Area, Done cards left out, grouped by module with named modules first and the rest under Across modules, each module in board order."""
    docs = {}
    live = [r for r in items if r["status"] != "Done"]
    for area in sorted({r["area"] for r in live}):
        mine = [r for r in live if r["area"] == area]
        modules = sorted({r["module"] for r in mine if r["module"]}) + ([""] if any(not r["module"] for r in mine) else [])
        out = [f"# Backlog: {area or 'No area'}", "",
               f"Generated from [the Project]({project_url}) by `moondeck/report/project_mirror.py`: edit the cards, not this file.", ""]
        for m in modules:
            out += [f"## {m or 'Across modules'}", ""]
            for r in sorted((r for r in mine if r["module"] == m), key=lambda r: r["position"]):
                out += [f"### {r['title']}", ""]
                line = field_line(r)
                if line:
                    out += [line, ""]
                if r["body"].strip():
                    out += [relative_links(r["body"].strip(), folder), ""]
        docs[file_name(area)] = "\n".join(out).rstrip("\n") + "\n"
    docs[DATA_FILE] = cards_data(live)
    return docs



def cards_data(live: list[dict]) -> str:
    """The open cards as data, for a build that reads the backlog rather than parsing its documents: no timestamp and a stable order, so an unchanged board writes the same file."""
    keep = ("title", "area", "module", "kind", "status", "process", "card", "number")
    rows = sorted(({k: r[k] for k in keep} for r in live), key=lambda r: (r["area"], r["title"].lower()))
    return json.dumps(rows, indent=1, ensure_ascii=False) + "\n"


def differences(docs: dict[str, str], folder: Path) -> list[str]:
    """The documents that differ from a fresh render: changed, missing, or left over from an Area with no cards."""
    out = [n for n, text in docs.items() if not (folder / n).exists() or (folder / n).read_text(encoding="utf-8") != text]
    out += [p.name for p in [*folder.glob("*.md"), *folder.glob("*.json")] if p.name not in docs] if folder.exists() else []
    return sorted(out)


CARD_LINK = re.compile(r"projects/\d+\?pane=issue&itemId=(\d+)")
MIRROR_LINK = re.compile(r"\]\(([^)\s]*backlog_mirror/[a-z-]+\.md(?:#[\w-]+)?)\)")


def dead_links(items: list[dict], docs_root: Path) -> list[str]:
    """A card link whose card is gone, and a link into the generated mirror, which should link the card or the Project instead."""
    alive = {r["card"].rsplit("=", 1)[-1] for r in items if r.get("card")}
    out = []
    for p in sorted(docs_root.rglob("*.md")):
        if p.parent.name == "backlog_mirror":
            continue
        text = p.read_text(encoding="utf-8")
        rel = p.relative_to(ROOT) if p.is_relative_to(ROOT) else p
        out += [f"{rel}: card {m.group(1)} is not on the board" for m in CARD_LINK.finditer(text) if m.group(1) not in alive]
        out += [f"{rel}: links the mirror ({m.group(1)}), link the card or the Project instead" for m in MIRROR_LINK.finditer(text)]
    return out


# A repository link's path, a balanced parenthesis group included, since a plan's file name holds one.
REPO_LINK = re.compile(r"\]\(" + re.escape(REPO_URL) + r"((?:[^()#\s]|\([^()\s]*\))+)")


def generated_pages() -> set[str]:
    """The catalog pages the docs build generates from help.json, which a card may link though no file holds them."""
    sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
    from gen_help import CATALOGS
    return {f"docs/moonmodules/{page}" for page in CATALOGS}


def missing_targets(items: list[dict], root: Path = ROOT, generated: set[str] | None = None) -> list[str]:
    """An open card's link into this repository whose file is gone, which the rendered document would carry as a dead relative link."""
    generated = generated_pages() if generated is None else generated
    out = []
    for r in items:
        for m in REPO_LINK.finditer(r["body"]) if r["status"] != "Done" else ():
            path = urllib.parse.unquote(m.group(1))
            if not (root / path).exists() and path not in generated:
                out.append(f"card {r['title']!r} links {path}, which the repository does not hold")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--owner", default="MoonModules")
    ap.add_argument("--number", type=int, default=5)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--markdown", type=Path, default=DEFAULT_MD, help="the folder for the per-Area documents")
    ap.add_argument("--check", action="store_true", help="fail when a document differs from the board; write nothing")
    args = ap.parse_args()

    try:
        project = f"https://github.com/users/{args.owner}/projects/{args.number}"
        items = normalize(fetch(args.owner, args.number), card_ids(args.owner, args.number), project)
    except (RuntimeError, FileNotFoundError) as e:   # FileNotFoundError: no `gh` on the PATH
        if args.check:
            print(f"SKIP: the board is unreadable: offline, no `gh`, or no token with the project scope ({e})")
            return 0
        sys.exit(f"project_mirror: gh could not read project {args.number} of {args.owner}: {e}")
    folder = args.markdown.resolve()
    rel = os.path.relpath(folder, ROOT).replace(os.sep, "/")
    docs = render(items, project, rel)

    if args.check:
        diff, dead = differences(docs, folder), dead_links(items, ROOT / "docs") + missing_targets(items)
        if diff:
            print(f"FAIL: {len(diff)} backlog document(s) differ from the board: {', '.join(diff)}. Run project_mirror.py and stage the result.")
        for d in dead:
            print(f"FAIL: {d}")
        if diff or dead:
            return 1
        print(f"PASS: {len(docs)} backlog documents match the board's {len(items)} cards")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({"project": project, "generated": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
                                    "items": items}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    folder.mkdir(parents=True, exist_ok=True)
    for stale in differences(docs, folder):
        if stale not in docs:
            (folder / stale).unlink()
    for name, text in docs.items():
        (folder / name).write_text(text, encoding="utf-8")
    by_status = Counter(r["status"] or "(no status)" for r in items)
    print(f"{len(items)} cards to {rel}/ ({len(docs)} documents) and {args.out.relative_to(ROOT) if args.out.is_relative_to(ROOT) else args.out}")
    for status, n in sorted(by_status.items()):
        print(f"  {status}: {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
