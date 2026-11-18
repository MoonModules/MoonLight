#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyflakes"]
# ///
"""Claude Code FileChanged hook: check a file the moment it changes on disk, so a finding is fixed while the line is still in mind.

The rules live in the coding standards and in the commit checks; this hook moves the cheap ones to the moment of writing, as an
editor's language server does on save. A finding found 200 lines later means a sweep and a rerun of the gate; found on the edit it
means a rewrite of the one line.

Wired in .claude/settings.json on two events. **SessionStart** hands Claude Code's file watcher the folders to watch (`watchPaths`):
every top folder holding a file a check governs, and each such file at the root; a watched folder covers its subfolders and the files
created in it. **FileChanged** then runs this hook for every write, whatever made it: the Edit and Write tools, a script run through
Bash, the editor. It runs in the background (`async`), and its findings reach the agent on its next turn as the `systemMessage`.

**One checker at a time, in batches.** A sweep by script changes many files at once and fires one hook each; each queues its file, and
the one holding the run lock checks the queue in batches until it is empty, as a work queue with one consumer does. Only a file that
differs from the index is checked, and a file is checked once per version, so a checkout or a file read back unchanged is skipped.
Each check runs only on the files it governs:

- **Prose** (check_prose.py's suffixes): an em-dash or a British spelling on an added line.
- **C++** (`src/`, `test/`): the file compiled alone with `-fsyntax-only` and its translation unit's flags from the host build's
  `compile_commands.json`, a header with any unit's flags since every header compiles on its own (the build's header-set check), so
  a broken declaration or a half-renamed name shows in seconds rather than in a rebuild.
- **Python**: pyflakes, an unused import or a name used but undefined.
- **Docgen and code findings** (check_docgen.py's and check_code.py's `--files`): a header's documentation rules, a page's hard wraps,
  a function's complexity, a file's size, for this file alone; the whole-tree rules and the counts stay with the commit gate.
- **A stray backup file** beside the written one (`*-E` from BSD `sed -i -E`, `*.orig`, `*.bak`, `*.rej`), before it is staged.

A malformed payload, a missing tool or a build not configured yet stays silent: this is a guardrail, not a gatekeeper.
"""

import json
import os
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

try:
    import fcntl
except ImportError:   # Windows: no flock, so each hook checks its own file
    fcntl = None

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
from check_prose import SUFFIXES as GOVERNED  # noqa: E402   (the widest set a check governs: prose reads every source and page)

BACKUP_SUFFIXES = ("-E", ".orig", ".bak", ".rej")
CPP_SUFFIXES = (".cpp", ".h")
STATE = REPO / "build" / "hook"   # the queue, its locks and the versions checked: this script's output, as build/ holds
SETTLE_S = 1.0   # a sweep's writes land within it, so they are checked as one batch
# The hook's 120 s timeout in .claude/settings.json, less the time to print what the finished checks found: one budget every step shares.
BUDGET_S = 100
DEADLINE = time.monotonic() + BUDGET_S


UNFINISHED: list[str] = []   # the checks the budget ran out on, named in the findings so a file reads as unchecked by them


def _run(cmd: list[str], **kw) -> subprocess.CompletedProcess | None:
    """A check's process within what is left of the shared budget; None when the budget runs out first, the other checks' findings kept."""
    left = DEADLINE - time.monotonic()
    try:
        if left > 1:
            return subprocess.run(cmd, capture_output=True, text=True, check=False, timeout=left, **kw)
    except subprocess.TimeoutExpired:
        pass
    UNFINISHED.append(Path(cmd[2] if cmd[:2] == ["uv", "run"] else cmd[0]).name)
    return None


def prose(paths: list[Path]) -> list[str]:
    """The prose rules on the lines these writes added, through check_prose.py's `--files`, the rules' one home, which reads them and leaves
    the index alone: the index is the product owner's, and this hook runs beside them."""
    r = _run(["uv", "run", str(HERE / "check_prose.py"), "--files", *map(str, paths)], cwd=REPO)
    lines = [ln for ln in r.stdout.splitlines() if ln.strip()] if r else []
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
    result = _run([compiler, *flags, "-fsyntax-only", "-Wno-pragma-once-outside-header", *lang, str(path)], cwd=entry.get("directory", REPO))
    errors = [ln for ln in result.stderr.splitlines() if " error: " in ln] if result and result.returncode else []   # a note from a file that compiles is no failure
    if not errors:
        return []
    return [f"C++ does not compile ({rel} alone, the host build's flags):", *errors[:12]]


def file_checks(paths: list[Path]) -> list[str]:
    """check_docgen's and check_code's per-file findings for these files, through each check's own `--files`, the rules' one home."""
    docgen = [p for p in paths if _rel(p).startswith(("src/", "test/", "docs/")) and p.suffix in (".h", ".cpp", ".md")]
    code = [p for p in docgen if _rel(p).startswith(("src/", "test/")) and p.suffix != ".md"]
    out = []
    for check, named in (("check_docgen.py", docgen), ("check_code.py", code)):
        r = _run(["uv", "run", str(HERE / check), "--files", *map(str, named)], cwd=REPO) if named else None
        if r and r.stdout.strip():
            out += [f"{check}:", *[ln for ln in r.stdout.splitlines() if ln.strip()]]
    return out


def python(paths: list[Path]) -> list[str]:
    """pyflakes on the Python files: an unused import, a name used but undefined, a redefinition."""
    named = [str(p) for p in paths if p.suffix == ".py"]
    if not named:
        return []
    result = _run([sys.executable, "-m", "pyflakes", *named])
    lines = [ln.replace(str(REPO) + "/", "") for ln in result.stdout.splitlines() if ln.strip()] if result else []
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


def check(paths: list[Path]) -> list[str]:
    """Every check on a batch: the batched ones once, the compile and the backup look per file."""
    findings = []
    steps = [lambda: prose(paths), lambda: file_checks(paths), lambda: python(paths)]
    steps += [lambda p=p: cpp(p) for p in paths] + [lambda p=p: backups(p) for p in paths]
    for step in steps:
        try:
            findings += step()
        except Exception:   # a check that cannot run stays silent
            continue
    return findings


def watch_paths() -> list[str]:
    """What the watcher watches: each top folder holding a tracked file a check governs, and each such file at the root."""
    tracked = subprocess.run(["git", "ls-files", "-z"], cwd=REPO, capture_output=True, text=True, check=False).stdout.split("\0")
    tops = {f.split("/", 1)[0] for f in tracked if f.endswith(GOVERNED)}
    return sorted(str(REPO / t) for t in tops)


def _seen_file() -> Path:
    return STATE / "checked.json"


def _seen() -> dict:
    try:
        return json.loads(_seen_file().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def changed(paths: set[str]) -> dict[Path, str]:
    """The paths that differ from the index or are new, ignored ones left out, at a version not checked before: each with the version read."""
    rels = sorted(_rel(Path(p)) for p in paths if Path(p).is_file())
    if not rels:
        return {}
    listed = subprocess.run(["git", "ls-files", "-z", "--modified", "--others", "--exclude-standard", "--", *rels],
                            cwd=REPO, capture_output=True, text=True, check=False).stdout
    seen = _seen()
    out = {}
    for rel in sorted(set(filter(None, listed.split("\0")))):
        p = REPO / rel
        if not p.is_file():
            continue
        st = p.stat()
        version = f"{st.st_mtime_ns}:{st.st_size}"
        if seen.get(rel) != version:
            out[p] = version
    return out


def remember(versions: dict[Path, str]) -> None:
    """Record each path as checked at its version, so the next hook reporting that version leaves it alone."""
    seen = _seen()
    seen.update({_rel(p): v for p, v in versions.items()})
    _seen_file().write_text(json.dumps(seen), encoding="utf-8")


def check_batch(batch: set[str]) -> list[str]:
    """The checks on the batch's changed files, each recorded as checked at the version read once they ran, unless the budget cut one short:
    a file a check left unfinished is checked again on its next report."""
    versions = changed(batch)
    cut = len(UNFINISHED)
    findings = check(list(versions))
    if len(UNFINISHED) == cut:
        remember(versions)
    return findings


class _Lock:
    """An flock on a file under STATE: blocking for the queue's short updates, non-blocking for the one checker's run."""

    def __init__(self, name: str, wait: bool):
        self.fd, self.held = os.open(STATE / name, os.O_CREAT | os.O_RDWR), False
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | (0 if wait else fcntl.LOCK_NB))
            self.held = True
        except BlockingIOError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *_):
        os.close(self.fd)   # closing releases the flock


def _queue(add: str | None = None, take: bool = True) -> set[str]:
    """Append `add` to the queue, or read it (and empty it when `take`), under the queue's own short lock."""
    q = STATE / "queue"
    with _Lock("queue.lock", wait=True):
        if add is not None:
            with q.open("a", encoding="utf-8") as f:
                f.write(add + "\n")
            return set()
        queued = set(filter(None, q.read_text(encoding="utf-8").splitlines())) if q.exists() else set()
        if take:
            q.write_text("", encoding="utf-8")
        return queued


def drain(path: Path) -> list[str]:
    """Queue `path`; the hook that holds the run lock checks the queue in batches until it is empty or the budget is spent."""
    if fcntl is None:
        return check_batch({str(path)})
    STATE.mkdir(parents=True, exist_ok=True)
    _queue(str(path))
    findings = []
    while True:
        with _Lock("run.lock", wait=False) as run:
            if not run.held:
                return findings   # the hook holding it checks this file in its next batch
            time.sleep(SETTLE_S)
            while time.monotonic() < DEADLINE and (batch := _queue()):
                findings += check_batch(batch)
        # A file queued as the lock was let go found it held and left: this hook takes the lock again for it.
        if not (left := _queue(take=False)):
            return findings
        if time.monotonic() >= DEADLINE:
            names = ", ".join(sorted(_rel(Path(p)) for p in left)[:5])
            return findings + [f"{len(left)} changed file(s) queued past the hook's {BUDGET_S} s, checked by the next write's hook: {names}."]


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0
    if payload.get("hook_event_name") == "SessionStart":
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "SessionStart", "watchPaths": watch_paths()}}))
        return 0
    name = payload.get("file_path", "")
    if not name or payload.get("event") == "unlink" or not name.endswith(GOVERNED):
        return 0
    if hasattr(os, "nice"):
        os.nice(10)   # beside the agent's builds and the product owner's editor
    findings = drain(Path(name).resolve())
    if UNFINISHED:
        findings.append(f"Not finished within the hook's {BUDGET_S} s, so not checked: {', '.join(UNFINISHED)}.")
    if findings:
        print(json.dumps({"systemMessage": "Write-time checks (moondeck/check/hook_written.py):\n" + "\n".join(findings)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
