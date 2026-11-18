"""hook_written.py: the write-time hook Claude Code runs on every changed file, whatever wrote it, and the folders it asks the watcher to watch."""

import importlib.util
import json
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("hook_written", ROOT / "moondeck" / "check" / "hook_written.py")
hook = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hook)


needs_flock = pytest.mark.skipif(hook.fcntl is None, reason="the queue and its locks are flock-based; Windows checks each file directly")


@pytest.fixture
def repo(tmp_path, monkeypatch):
    """A temporary git repository with one tracked Markdown file, standing in for the checkout and its hook state."""
    work = tmp_path / "work"
    work.mkdir()
    for cmd in (["init", "-q"], ["config", "user.email", "t@t"], ["config", "user.name", "t"]):
        subprocess.run(["git", *cmd], cwd=work, check=True)
    (work / "tracked.md").write_text("# Tracked\n", encoding="utf-8")
    subprocess.run(["git", "add", "tracked.md"], cwd=work, check=True)
    subprocess.run(["git", "commit", "-qm", "seed"], cwd=work, check=True)
    monkeypatch.setattr(hook, "REPO", work.resolve())
    monkeypatch.setattr(hook, "STATE", tmp_path / "state")
    (tmp_path / "state").mkdir()
    monkeypatch.setattr(hook, "UNFINISHED", [])
    return work.resolve()


def test_the_watcher_watches_each_top_folder_holding_a_governed_file_and_each_such_file_at_the_root():
    watched = hook.watch_paths()
    assert str(ROOT / "src") in watched and str(ROOT / "moonwalk") in watched and str(ROOT / "CLAUDE.md") in watched
    assert str(ROOT / "build") not in watched                     # script output, outside every check


def test_a_file_equal_to_the_index_is_left_unchecked_and_a_new_one_once_per_version(repo):
    assert hook.changed({str(repo / "tracked.md")}) == {}          # as a checkout leaves it: the index holds it already
    fresh = repo / "fresh.md"
    fresh.write_text("# Probe\n", encoding="utf-8")
    assert list(hook.changed({str(fresh)})) == [fresh]
    hook.remember(hook.changed({str(fresh)}))
    assert hook.changed({str(fresh)}) == {}                        # the same version a second hook reports
    fresh.write_text("# Probe, edited\n", encoding="utf-8")
    assert list(hook.changed({str(fresh)})) == [fresh]


@needs_flock
def test_a_sweeps_files_are_checked_as_one_batch_by_the_hook_holding_the_run_lock(tmp_path, monkeypatch):
    monkeypatch.setattr(hook, "STATE", tmp_path)
    monkeypatch.setattr(hook, "SETTLE_S", 0)
    monkeypatch.setattr(hook, "DEADLINE", time.monotonic() + 100)   # the budget of this test, not of the suite's elapsed time
    monkeypatch.setattr(hook, "UNFINISHED", [])
    batches = []
    monkeypatch.setattr(hook, "check_batch", lambda paths: batches.append(sorted(paths)) or [f"{len(paths)} checked"])
    for name in ("a.md", "b.md"):
        hook._queue(str(tmp_path / name))                        # queued by the hooks that found the run lock held
    assert hook.drain(tmp_path / "c.md") == ["3 checked"] and len(batches) == 1


@needs_flock
def test_a_hook_finding_the_run_lock_held_queues_its_file_and_leaves(tmp_path, monkeypatch):
    monkeypatch.setattr(hook, "STATE", tmp_path)
    monkeypatch.setattr(hook, "check", lambda paths: ["checked here"])
    monkeypatch.setattr(hook, "DEADLINE", time.monotonic() + 100)
    with hook._Lock("run.lock", wait=False):
        assert hook.drain(tmp_path / "x.md") == []
    assert hook._queue(take=False) == {str(tmp_path / "x.md")}  # left for the holder's next batch


def test_findings_reach_the_agent_as_the_systemMessage_and_a_deleted_or_ungoverned_file_is_ignored(monkeypatch, capsys):
    monkeypatch.setattr(hook, "drain", lambda path: [f"finding in {path.name}"])
    monkeypatch.setattr(hook.os, "nice", lambda n: 0)               # the dispatch under test, not the test runner's priority
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
    monkeypatch.setattr(hook.shutil, "which", lambda name: "/usr/bin/c++")   # a compiler is found, so both cases reach the simulated result
    monkeypatch.setattr(build_host, "host_build_dir", lambda: str(tmp_path / "bld"))   # absolute: the repo root joined to it is itself
    note = "x.cpp:1: note: #pragma message: no error: here\nx.cpp:1: warning: an error: in prose\n"
    monkeypatch.setattr(hook, "_run", lambda cmd, **k: subprocess.CompletedProcess(cmd, 0, stdout="", stderr=note))
    assert hook.cpp(source) == []                                  # the file compiled: its diagnostics are not a failure
    monkeypatch.setattr(hook, "_run", lambda cmd, **k: subprocess.CompletedProcess(cmd, 1, stdout="", stderr="x.cpp:2:1: error: expected ';'\n"))
    assert "C++ does not compile" in hook.cpp(source)[0]


def test_a_file_is_recorded_as_checked_once_its_checks_ran_and_not_when_the_budget_cut_one_short(repo, monkeypatch):
    fresh = repo / "cut.md"
    fresh.write_text("# Probe\n", encoding="utf-8")
    monkeypatch.setattr(hook, "check", lambda paths: hook.UNFINISHED.append("vale") or [])
    hook.check_batch({str(fresh)})
    assert list(hook.changed({str(fresh)})) == [fresh]        # unfinished: the next report checks it again
    monkeypatch.setattr(hook, "check", lambda paths: [])
    hook.check_batch({str(fresh)})
    assert hook.changed({str(fresh)}) == {}


def test_a_check_that_fails_to_run_leaves_its_files_unfinished_while_one_that_reports_findings_does_not(tmp_path, monkeypatch):
    import subprocess
    monkeypatch.setattr(hook, "UNFINISHED", [])
    header = ROOT / "src" / "core" / "Module.h"
    ran = lambda code, out, err: (lambda cmd, **k: subprocess.CompletedProcess(cmd, code, stdout=out, stderr=err))
    monkeypatch.setattr(hook, "_run", ran(1, "Docgen, 1 file(s): 1 error(s)\n", ""))
    assert "check_docgen.py:" in hook.file_checks([header]) and hook.UNFINISHED == []      # a finding: exit 1 with its report on stdout
    monkeypatch.setattr(hook, "_run", ran(1, "", "Traceback ...\nRuntimeError: git check-ignore failed (128)\n"))
    assert hook.file_checks([header]) == []
    assert hook.UNFINISHED and "failed" in hook.UNFINISHED[0]                               # a crash: the next report checks the file again


def test_the_state_folder_exists_before_the_hook_records_a_version_on_a_platform_without_flock(tmp_path, monkeypatch):
    monkeypatch.setattr(hook, "STATE", tmp_path / "build" / "hook")
    monkeypatch.setattr(hook, "fcntl", None)
    monkeypatch.setattr(hook, "check_batch", lambda paths: [(tmp_path / "build" / "hook").is_dir() and "state ready"])
    assert hook.drain(tmp_path / "a.md") == ["state ready"]


def test_a_filename_with_a_glob_character_names_only_itself_to_git(repo):
    (repo / "test1.md").write_text("one\n")
    (repo / "test?.md").write_text("two\n")
    assert list(hook.changed({str(repo / "test?.md")})) == [repo / "test?.md"]   # `test1.md` matches the pattern, not the name


@needs_flock
def test_an_unfinished_batch_goes_back_to_the_queue_for_the_next_hook(repo, monkeypatch):
    fresh = repo / "requeue.md"
    fresh.write_text("# Probe\n", encoding="utf-8")
    monkeypatch.setattr(hook, "check", lambda paths: hook.UNFINISHED.append("vale") or [])
    hook.check_batch({str(fresh)})
    assert hook._queue(take=False) == {str(fresh)}


def test_a_repository_relative_path_is_spelled_with_forward_slashes_as_git_lists_it(monkeypatch):
    from pathlib import PureWindowsPath
    monkeypatch.setattr(hook, "REPO", PureWindowsPath("C:/repo"))
    assert hook._rel(PureWindowsPath("C:/repo/src/Foo.cpp")) == "src/Foo.cpp"
