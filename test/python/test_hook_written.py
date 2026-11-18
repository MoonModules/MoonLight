"""hook_written.py: the write-time hook Claude Code runs on every changed file, whatever wrote it, and the folders it asks the watcher to watch."""

import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("hook_written", ROOT / "moondeck" / "check" / "hook_written.py")
hook = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hook)


def test_the_watcher_watches_each_top_folder_holding_a_governed_file_and_each_such_file_at_the_root():
    watched = hook.watch_paths()
    assert str(ROOT / "src") in watched and str(ROOT / "moonwalk") in watched and str(ROOT / "CLAUDE.md") in watched
    assert str(ROOT / "build") not in watched                     # script output, outside every check


def test_a_file_equal_to_the_index_is_left_unchecked_and_a_new_one_once_per_version(tmp_path, monkeypatch):
    monkeypatch.setattr(hook, "STATE", tmp_path)
    import subprocess
    git = lambda *a: set(subprocess.run(["git", *a], cwd=ROOT, capture_output=True, text=True).stdout.split())
    unchanged = sorted(git("ls-files", "*.md") - git("ls-files", "--modified"))[0]
    assert hook.changed({str(ROOT / unchanged)}) == {}             # as a checkout leaves it: the index holds it already
    fresh = ROOT / "test" / "python" / "_hook_probe_new.md"
    try:
        fresh.write_text("# Probe\n", encoding="utf-8")
        assert list(hook.changed({str(fresh)})) == [fresh]
        hook.remember(hook.changed({str(fresh)}))
        assert hook.changed({str(fresh)}) == {}                   # the same version a second hook reports
        fresh.write_text("# Probe, edited\n", encoding="utf-8")
        assert list(hook.changed({str(fresh)})) == [fresh]
    finally:
        fresh.unlink(missing_ok=True)


def test_a_sweeps_files_are_checked_as_one_batch_by_the_hook_holding_the_run_lock(tmp_path, monkeypatch):
    monkeypatch.setattr(hook, "STATE", tmp_path)
    monkeypatch.setattr(hook, "SETTLE_S", 0)
    batches = []
    monkeypatch.setattr(hook, "check_batch", lambda paths: batches.append(sorted(paths)) or [f"{len(paths)} checked"])
    for name in ("a.md", "b.md"):
        hook._queue(str(tmp_path / name))                        # queued by the hooks that found the run lock held
    assert hook.drain(tmp_path / "c.md") == ["3 checked"] and len(batches) == 1


def test_a_hook_finding_the_run_lock_held_queues_its_file_and_leaves(tmp_path, monkeypatch):
    monkeypatch.setattr(hook, "STATE", tmp_path)
    monkeypatch.setattr(hook, "check", lambda paths: ["checked here"])
    with hook._Lock("run.lock", wait=False):
        assert hook.drain(tmp_path / "x.md") == []
    assert hook._queue(take=False) == {str(tmp_path / "x.md")}  # left for the holder's next batch


def test_findings_reach_the_agent_as_the_systemMessage_and_a_deleted_or_ungoverned_file_is_ignored(monkeypatch, capsys):
    monkeypatch.setattr(hook, "drain", lambda path: [f"finding in {path.name}"])
    for payload, says in (({"hook_event_name": "FileChanged", "file_path": str(ROOT / "README.md"), "event": "change"}, "finding in README.md"),
                          ({"hook_event_name": "FileChanged", "file_path": str(ROOT / "README.md"), "event": "unlink"}, None),
                          ({"hook_event_name": "FileChanged", "file_path": str(ROOT / "x.pyc"), "event": "add"}, None)):
        monkeypatch.setattr(sys, "stdin", type("In", (), {"read": lambda self, p=payload: json.dumps(p)})())
        assert hook.main() == 0
        out = capsys.readouterr().out
        assert (says in json.loads(out)["systemMessage"]) if says else out == ""


def test_a_compiler_note_that_says_error_is_no_failure_while_a_nonzero_exit_is(tmp_path, monkeypatch):
    import subprocess
    sys.path.insert(0, str(ROOT / "moondeck" / "build"))
    import build_host
    source = ROOT / "src" / "main.cpp"
    (tmp_path / "bld").mkdir()
    (tmp_path / "bld" / "compile_commands.json").write_text(json.dumps([{"file": str(source), "directory": str(tmp_path), "command": f"c++ -c {source}"}]))
    monkeypatch.setattr(build_host, "host_build_dir", lambda: str(tmp_path / "bld"))   # absolute: the repo root joined to it is itself
    note = "x.cpp:1: note: #pragma message: no error: here\nx.cpp:1: warning: an error: in prose\n"
    monkeypatch.setattr(hook, "_run", lambda cmd, **k: subprocess.CompletedProcess(cmd, 0, stdout="", stderr=note))
    assert hook.cpp(source) == []                                  # the file compiled: its diagnostics are not a failure
    monkeypatch.setattr(hook, "_run", lambda cmd, **k: subprocess.CompletedProcess(cmd, 1, stdout="", stderr="x.cpp:2:1: error: expected ';'\n"))
    assert "C++ does not compile" in hook.cpp(source)[0]


def test_a_file_is_recorded_as_checked_once_its_checks_ran_and_not_when_the_budget_cut_one_short(tmp_path, monkeypatch):
    monkeypatch.setattr(hook, "STATE", tmp_path)
    fresh = ROOT / "test" / "python" / "_hook_probe_cut.md"
    try:
        fresh.write_text("# Probe\n", encoding="utf-8")
        monkeypatch.setattr(hook, "check", lambda paths: hook.UNFINISHED.append("vale") or [])
        hook.check_batch({str(fresh)})
        assert list(hook.changed({str(fresh)})) == [fresh]        # unfinished: the next report checks it again
        monkeypatch.setattr(hook, "check", lambda paths: [])
        hook.check_batch({str(fresh)})
        assert hook.changed({str(fresh)}) == {}
    finally:
        fresh.unlink(missing_ok=True)
        hook.UNFINISHED.clear()
