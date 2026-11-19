#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyflakes"]
# ///
"""Claude Code PostToolUse hook: check every changed file after each tool call, so a finding is fixed while the line is still in mind.

The rules live in the coding standards and in the commit checks; this hook moves the cheap ones to the moment of writing, as an
editor's language server does on save. A finding found 200 lines later means a sweep and a rerun of the gate; found on the edit it
means a rewrite of the one line.

Wired in .claude/settings.json on **PostToolUse** of the tools that write: Edit, Write, NotebookEdit and Bash, whose scripts and `sed`
write too. It checks every governed file that differs from the index at a version not checked before, so a file written any way since
the last tool call is seen, and no file watcher has to stay registered across a session that continues from a summary. It runs in the
background (`async`), and its findings reach the agent on its next turn as the `systemMessage`.

**`--diff`**, the commit process's first gate: every file the change touches, checked afresh, and what it raised against HEAD (a
function past a lizard limit, a file past its lines, a duplicated block over the changed lines), so a rise shows in a minute rather
than after the builds. It exits 1 on what the change raised, and lists the rest.

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
    # A header, or a source not in this host's build: the flags of the nearest unit below src/, else of module_types.cpp, the unit that
    # includes every module, since a core unit does not see light/ and the light domain is headers only.
    parts = rel.split("/")[:-1]
    while entry is None and len(parts) > 1:
        folder = "/" + "/".join(parts) + "/"
        entry = next((e for e in entries if folder in e["file"]), None)
        parts.pop()
    entry = entry or next((e for e in entries if e["file"].endswith("/src/module_types.cpp")), entries[0] if entries else None)
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
        if r and r.returncode and r.stderr.strip():   # findings go to stdout; a nonzero exit with stderr text is the check failing, so its files are checked again
            UNFINISHED.append(f"{check} (failed: {r.stderr.strip().splitlines()[-1][:120]})")
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
        return path.relative_to(REPO).as_posix()   # git's own spelling, whatever the platform
    except ValueError:
        return path.as_posix()


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


def changed_now() -> set[str]:
    """Every governed file that differs from the index or is new, ignored ones left out: what any write since the last check can have touched."""
    listed = subprocess.run(["git", "ls-files", "-z", "--modified", "--others", "--exclude-standard"],
                            cwd=REPO, capture_output=True, text=True, check=False).stdout
    return {str(REPO / rel) for rel in filter(None, listed.split("\0")) if rel.endswith(GOVERNED)}


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
    listed = subprocess.run(["git", "--literal-pathspecs", "ls-files", "-z", "--modified", "--others", "--exclude-standard", "--", *rels],
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
    elif fcntl:
        for p in versions:   # the batch left the queue to be checked: it goes back for the next batch or the next write's hook
            _queue(str(p))
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


def drain(paths: set[str]) -> list[str]:
    """Queue `paths`; the hook that holds the run lock checks the queue in batches until it is empty or the budget is spent."""
    STATE.mkdir(parents=True, exist_ok=True)   # the versions checked live here on every platform
    if fcntl is None:
        return check_batch(paths)
    for p in paths:
        _queue(p)
    findings = []
    while True:
        with _Lock("run.lock", wait=False) as run:
            if not run.held:
                return findings   # the hook holding it checks this file in its next batch
            time.sleep(SETTLE_S)
            while time.monotonic() < DEADLINE and not UNFINISHED and (batch := _queue()):
                findings += check_batch(batch)
        # A file queued as the lock was let go found it held and left: this hook takes the lock again for it.
        # An unfinished check leaves its files queued for the next write's hook, which main() names.
        if UNFINISHED or not (left := _queue(take=False)):
            return findings
        if time.monotonic() >= DEADLINE:
            names = ", ".join(sorted(_rel(Path(p)) for p in left)[:5])
            return findings + [f"{len(left)} changed file(s) queued past the hook's {BUDGET_S} s, checked by the next write's hook: {names}."]


def touched() -> list[Path]:
    """Every governed file the change touches against HEAD, staged or not, and new ones: the commit's whole diff."""
    names = subprocess.run(["git", "diff", "--name-only", "-z", "HEAD"], cwd=REPO, capture_output=True, text=True, check=False).stdout
    names += subprocess.run(["git", "ls-files", "-z", "--others", "--exclude-standard"], cwd=REPO, capture_output=True, text=True, check=False).stdout
    return sorted({REPO / n for n in filter(None, names.split("\0")) if n.endswith(GOVERNED) and (REPO / n).is_file()})


def lizard_rises(paths: list[Path]) -> list[str]:
    """The lizard rules and the file-length rule a touched C++ file breaks more often than at HEAD: check_code's own measure and limits,
    HEAD's version measured from a copy under the same relative path."""
    import tempfile  # noqa: PLC0415
    import check_code  # noqa: PLC0415   (the rules' one home)
    cpp = [p for p in paths if p.suffix in CPP_SUFFIXES and _rel(p).startswith(("src/", "test/"))]
    if not cpp:
        return []
    now = check_code.measure(targets=[_rel(p) for p in cpp]) or []
    with tempfile.TemporaryDirectory() as td:
        olds = []
        for p in cpp:
            text = subprocess.run(["git", "show", f"HEAD:{_rel(p)}"], cwd=REPO, capture_output=True, text=True, check=False)
            if text.returncode == 0:
                (Path(td) / _rel(p)).parent.mkdir(parents=True, exist_ok=True)
                (Path(td) / _rel(p)).write_text(text.stdout, encoding="utf-8")
                olds.append(str(Path(td) / _rel(p)))
        was = check_code.measure(targets=olds) or [] if olds else []
        prefix = Path(td).resolve().as_posix() + "/"
        for f in was:
            f["file"] = Path(f["file"]).resolve().as_posix().replace(prefix, "") if f["file"].startswith("/") else f["file"]
    # Per rule over the touched files, as the ratchet's total feels it: a function that rose in one file and one that fell in another cancel.
    count = lambda rows: {rule: sum(1 for r in rows if r[1] == rule) for rule in {r[1] for r in rows}}  # noqa: E731
    before, after = count(check_code.findings(was)), count(check_code.findings(now))
    out = []
    for rule, n in sorted(after.items()):
        if n > before.get(rule, 0):
            names = sorted({f"{r[2]} ({r[0]})" for r in check_code.findings(now) if r[1] == rule} - {f"{r[2]} ({r[0]})" for r in check_code.findings(was) if r[1] == rule})
            out.append(f"{rule} {before.get(rule, 0)} -> {n} in the touched files: {', '.join(names[:6])}")
    for p in cpp:
        head = subprocess.run(["git", "show", f"HEAD:{_rel(p)}"], cwd=REPO, capture_output=True, text=True, check=False).stdout
        lines, old = len(p.read_text(encoding="utf-8").splitlines()), len(head.splitlines())
        if lines > check_code.MAX_FILE_LINES >= old:
            out.append(f"{_rel(p)}: large file {old} -> {lines} lines")
    return out


def clone_rises(paths: list[Path]) -> list[str]:
    """Duplicated blocks jscpd finds whose either side lies in lines this change added: a copy the change made."""
    import check_code  # noqa: PLC0415
    added: dict[str, list[tuple[int, int]]] = {}
    diff = subprocess.run(["git", "diff", "-U0", "HEAD", "--", *map(_rel, paths)], cwd=REPO, capture_output=True, text=True, check=False).stdout
    current = ""
    for ln in diff.splitlines():
        if ln.startswith("+++ b/"):
            current = ln[6:]
        elif ln.startswith("@@") and current:
            plus = ln.split("+", 1)[1].split(" ", 1)[0].split(",")
            start, n = int(plus[0]), int(plus[1]) if len(plus) > 1 else 1
            if n:
                added.setdefault(current, []).append((start, start + n - 1))
    for p in paths:   # a new file is added whole
        if not subprocess.run(["git", "cat-file", "-e", f"HEAD:{_rel(p)}"], cwd=REPO, capture_output=True, check=False).returncode == 0:
            added[_rel(p)] = [(1, 10**9)]
    new = lambda side: any(a <= side["end"] and side["start"] <= b for a, b in added.get(side["name"], []))  # noqa: E731
    report = check_code.clones() or {}
    return [f"duplicated block: {d['firstFile']['name']}:{d['firstFile']['start']}-{d['firstFile']['end']} == "
            f"{d['secondFile']['name']}:{d['secondFile']['start']}-{d['secondFile']['end']}"
            for d in report.get("duplicates", []) if new(d["firstFile"]) or new(d["secondFile"])]


def diff_gate() -> int:
    """The commit's first gate: the write-time checks on the whole diff, afresh, and what the change raised against HEAD."""
    global DEADLINE
    DEADLINE = time.monotonic() + 600   # a gate, not a hook: the runner's own timeout bounds it
    paths = touched()
    listed = []
    for step in (lambda: prose(paths), lambda: python(paths), *[lambda p=p: cpp(p) for p in paths], *[lambda p=p: backups(p) for p in paths]):
        listed += step()   # the write-time checks less the per-file listings, whose rises the comparison below reports
    raised = lizard_rises(paths) + clone_rises(paths)
    for ln in listed:
        print(ln)
    if raised:
        print("Raised against HEAD (the ratchets' rules over the touched files; Extract Function, as check_code says):")
        for ln in raised:
            print("  " + ln)
    failing = raised or any(ln.startswith(("Prose rule", "C++ does not compile", "Python (pyflakes)")) for ln in listed)
    print(f"{len(paths)} changed file(s): {'findings above' if failing else 'nothing raised'}.")
    return 1 if failing else 0


def main() -> int:
    if "--diff" in sys.argv[1:]:
        return diff_gate()
    try:
        json.load(sys.stdin)   # the tool call's payload: which tool wrote matters less than what changed since the last check
    except (json.JSONDecodeError, ValueError):
        return 0
    paths = changed_now()
    if not paths:
        return 0
    sys.path.insert(0, str(HERE.parent / "build"))
    from build_host import lower_priority  # noqa: PLC0415  (one home for yielding the CPU)
    lower_priority()   # beside the agent's builds and the product owner's editor
    findings = drain(paths)
    if UNFINISHED:
        findings.append(f"Not finished (out of the hook's {BUDGET_S} s, or failed), so not checked: {', '.join(UNFINISHED)}.")
    if findings:
        print(json.dumps({"systemMessage": "Write-time checks (moondeck/check/hook_written.py):\n" + "\n".join(findings)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
