#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyflakes"]
# ///
"""Claude Code PostToolUse hook: check the file just written, so a finding is fixed while the line is still in mind.

The rules live in the coding standards and in the commit checks; this hook moves the cheap ones to the moment of writing, as an
editor's language server does on save. A finding found 200 lines later means a sweep and a rerun of the gate; found on the edit it
means a rewrite of the one line.

Wired in .claude/settings.json as a PostToolUse hook on Write|Edit. Exit 2 tells Claude Code the write had a problem and feeds stderr
back, so the fix happens in context. Each check runs only on the files it governs:

- **Prose** (check_prose.py's suffixes): an em-dash or a British spelling on an added line.
- **C++** (`src/`, `test/`): the file compiled alone with `-fsyntax-only` and its translation unit's flags from the host build's
  `compile_commands.json`, a header with any unit's flags since every header compiles on its own (the build's header-set check), so
  a broken declaration or a half-renamed name shows in seconds rather than in a rebuild.
- **Python**: pyflakes, an unused import or a name used but undefined.
- **Docgen and code findings** (check_docgen.py's and check_code.py's `--files`): a header's documentation rules, a page's hard wraps,
  a function's complexity, a file's size, for this file alone; the whole-tree rules and the counts stay with the commit gate.
- **A stray backup file** beside the written one (`*-E` from BSD `sed -i -E`, `*.orig`, `*.bak`, `*.rej`), before it is staged.

A malformed payload, a missing tool or a build not configured yet never blocks a write: this is a guardrail, not a gatekeeper.
"""

import json
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
BACKUP_SUFFIXES = ("-E", ".orig", ".bak", ".rej")
CPP_SUFFIXES = (".cpp", ".h")


def prose(path: Path) -> list[str]:
    """The prose rules on the lines this write added, through check_prose.py's `--files`, the rules' one home, which reads them and leaves
    the index alone: the index is the product owner's, and this hook runs beside them."""
    r = subprocess.run(["uv", "run", str(HERE / "check_prose.py"), "--files", str(path)], capture_output=True, text=True, cwd=REPO,
                       check=False, timeout=40)
    lines = [ln for ln in r.stdout.splitlines() if ln.strip()]
    return ["Prose rule (coding standards): American spelling, no em-dashes, say what a thing is.", *lines] if lines else []


def cpp(path: Path) -> list[str]:
    """The file compiled alone with its unit's flags, a header with any unit's, from the host build's compile database."""
    rel = _rel(path)
    if path.suffix not in CPP_SUFFIXES or not rel.startswith(("src/", "test/")):
        return []
    sys.path.insert(0, str(HERE.parent / "build"))
    try:
        from build_host import host_build_dir  # noqa: PLC0415  (the build directory has one home)
    except ImportError:
        return []
    db = REPO / host_build_dir() / "compile_commands.json"
    if not db.exists():
        return []
    entries = json.loads(db.read_text())
    entry = next((e for e in entries if Path(e["file"]).resolve() == path), None)
    if entry is None:   # a header, or a source not in this host's build: the flags of a unit in the same top folder
        top = "/" + rel.split("/", 1)[0] + "/"
        entry = next((e for e in entries if top in e["file"]), entries[0] if entries else None)
    if entry is None:
        return []
    args = entry.get("arguments") or shlex.split(entry["command"])
    # The unit's own command less what names its outputs and its source, as clangd reuses one: -o, -c and the dependency file.
    flags, skip = [], False
    for a in args[1:]:
        if skip:
            skip = False
        elif a in ("-o", "-MT", "-MF", "-MQ"):
            skip = True
        elif a not in ("-c", "-MD", "-MMD") and Path(a).name != Path(entry["file"]).name:
            flags.append(a)
    compiler = args[0] if shutil.which(args[0]) else shutil.which("c++")
    if not compiler:
        return []
    lang = ["-x", "c++"] if path.suffix == ".h" else []
    result = subprocess.run([compiler, *flags, "-fsyntax-only", "-Wno-pragma-once-outside-header", *lang, str(path)],
                            capture_output=True, text=True, cwd=entry.get("directory", REPO), check=False, timeout=40)
    errors = [ln for ln in result.stderr.splitlines() if " error: " in ln]
    if result.returncode == 0 or not errors:
        return []
    return [f"C++ does not compile ({rel} alone, the host build's flags):", *errors[:12]]


def file_checks(path: Path) -> list[str]:
    """check_docgen's and check_code's per-file findings for this file, through each check's own `--files`, the rules' one home."""
    rel = _rel(path)
    out = []
    if rel.startswith(("src/", "test/", "docs/")) and path.suffix in (".h", ".cpp", ".md"):
        for check in ("check_docgen.py", "check_code.py") if path.suffix != ".md" else ("check_docgen.py",):
            if check == "check_code.py" and not rel.startswith(("src/", "test/")):
                continue
            r = subprocess.run(["uv", "run", str(HERE / check), "--files", str(path)], capture_output=True, text=True, cwd=REPO,
                               check=False, timeout=40)
            if r.stdout.strip():
                out += [f"{check} on {rel}:", *[ln for ln in r.stdout.splitlines() if ln.strip()]]
    return out


def python(path: Path) -> list[str]:
    """pyflakes on a Python file: an unused import, a name used but undefined, a redefinition."""
    if path.suffix != ".py":
        return []
    result = subprocess.run([sys.executable, "-m", "pyflakes", str(path)], capture_output=True, text=True, check=False)
    lines = [ln.replace(str(REPO) + "/", "") for ln in result.stdout.splitlines() if ln.strip()]
    return ["Python (pyflakes):", *lines] if lines else []


def backups(path: Path) -> list[str]:
    """A backup file beside the written one, which a later `git add` would stage."""
    strays = sorted(p.name for p in path.parent.glob(path.name + "*") if p.name != path.name and p.name.endswith(BACKUP_SUFFIXES))
    return [f"A backup file beside {path.name}: {', '.join(strays)}; remove it before it is staged."] if strays else []


def _rel(path: Path) -> str:
    try:
        return str(path.relative_to(REPO))
    except ValueError:
        return str(path)


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0
    name = (payload.get("tool_input") or {}).get("file_path", "")
    if not name:
        return 0
    path = Path(name).resolve()
    if not path.exists():
        return 0
    findings = []
    for check in (prose, cpp, file_checks, python, backups):
        try:
            findings += check(path)
        except Exception:   # a check that cannot run stays silent, so the write goes through
            continue
    if not findings:
        return 0
    print("\n".join(findings), file=sys.stderr)
    return 2   # tells Claude Code to surface stderr back to the model


if __name__ == "__main__":
    sys.exit(main())
