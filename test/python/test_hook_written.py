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


def test_after_a_tool_call_every_governed_file_differing_from_the_index_is_a_candidate_whatever_wrote_it(repo):
    (repo / "fresh.md").write_text("# Probe\n", encoding="utf-8")   # as a shell script writes it, no editor tool involved
    (repo / "fresh.pyc").write_bytes(b"\0")                         # no check governs it
    assert hook.changed_now() == {str(repo / "fresh.md")}


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
    assert hook.drain({str(tmp_path / "c.md")}) == ["3 checked"] and len(batches) == 1


@needs_flock
def test_a_hook_finding_the_run_lock_held_queues_its_file_and_leaves(tmp_path, monkeypatch):
    monkeypatch.setattr(hook, "STATE", tmp_path)
    monkeypatch.setattr(hook, "check", lambda paths: ["checked here"])
    monkeypatch.setattr(hook, "DEADLINE", time.monotonic() + 100)
    with hook._Lock("run.lock", wait=False):
        assert hook.drain({str(tmp_path / "x.md")}) == []
    assert hook._queue(take=False) == {str(tmp_path / "x.md")}  # left for the holder's next batch


def test_findings_reach_the_agent_as_the_systemMessage_and_a_call_that_changed_nothing_is_silent(monkeypatch, capsys):
    monkeypatch.setattr(hook, "drain", lambda paths: [f"finding in {Path(p).name}" for p in sorted(paths)])
    monkeypatch.setattr(hook.os, "nice", lambda n: 0)               # the dispatch under test, not the test runner's priority
    payload = {"hook_event_name": "PostToolUse", "tool_name": "Bash"}
    for changed, says in (({str(ROOT / "README.md")}, "finding in README.md"), (set(), None)):
        monkeypatch.setattr(hook, "changed_now", lambda c=changed: c)
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


def test_a_header_compiles_through_a_unit_that_includes_it_so_a_constant_only_includers_use_is_no_error(tmp_path, monkeypatch):
    import shutil
    if not shutil.which("c++"):
        pytest.skip("no C++ compiler")
    sys.path.insert(0, str(ROOT / "moondeck" / "build"))
    import build_host
    src = tmp_path / "src"
    src.mkdir()
    unit, header, broken = src / "unit.cpp", src / "consts.h", src / "broken.h"
    unit.write_text("int main() { return 0; }\n")
    header.write_text("#pragma once\nstatic constexpr int kOnlyIncludersUseIt = 3;\n")
    broken.write_text("#pragma once\nint missingSemicolon()\n")
    (tmp_path / "bld").mkdir()
    (tmp_path / "bld" / "compile_commands.json").write_text(json.dumps(
        [{"file": str(unit), "directory": str(tmp_path), "command": f"c++ -Wall -Werror -Wunused-const-variable -c {unit}"}]))
    monkeypatch.setattr(build_host, "host_build_dir", lambda: str(tmp_path / "bld"))
    monkeypatch.setattr(hook, "_rel", lambda p: "src/" + Path(p).name)
    assert hook.cpp(header) == []
    assert "C++ does not compile" in hook.cpp(broken)[0]


def test_another_platforms_file_is_left_to_its_own_build_and_this_hosts_platform_file_is_compiled(tmp_path, monkeypatch):
    import subprocess
    sys.path.insert(0, str(ROOT / "moondeck" / "build"))
    import build_host
    host_unit = ROOT / "src" / "platform" / "host" / "platform_host.cpp"
    (tmp_path / "bld").mkdir()
    (tmp_path / "bld" / "compile_commands.json").write_text(json.dumps([{"file": str(host_unit), "directory": str(tmp_path), "command": f"c++ -c {host_unit}"}]))
    monkeypatch.setattr(build_host, "host_build_dir", lambda: str(tmp_path / "bld"))
    monkeypatch.setattr(hook.shutil, "which", lambda name: "/usr/bin/c++")
    monkeypatch.setattr(hook, "_run", lambda cmd, **k: subprocess.CompletedProcess(cmd, 1, stdout="", stderr="x.cpp:1:1: error: sdkconfig.h not found\n"))
    assert hook.cpp(ROOT / "src" / "platform" / "esp32" / "platform_esp32_h264.cpp") == []
    assert "C++ does not compile" in hook.cpp(ROOT / "src" / "platform" / "host" / "host_address.h")[0]


def test_a_file_is_recorded_as_checked_once_its_checks_ran_and_not_when_the_budget_cut_one_short(repo, monkeypatch):
    fresh = repo / "cut.md"
    fresh.write_text("# Probe\n", encoding="utf-8")
    monkeypatch.setattr(hook, "check", lambda paths: hook.UNFINISHED.append("vale") or [])
    hook.check_batch({str(fresh)})
    assert list(hook.changed({str(fresh)})) == [fresh]        # unfinished: the next report checks it again
    monkeypatch.setattr(hook, "check", lambda paths: [])
    hook.check_batch({str(fresh)})
    assert hook.changed({str(fresh)}) == {}


def test_the_diff_gate_fails_on_a_check_the_deadline_cut_short_and_passes_on_a_whole_clean_run(monkeypatch, capsys):
    monkeypatch.setattr(hook, "UNFINISHED", [])
    monkeypatch.setattr(hook, "touched", lambda: [])
    monkeypatch.setattr(hook, "lizard_rises", lambda paths: [])
    monkeypatch.setattr(hook, "clone_rises", lambda paths: [])
    monkeypatch.setattr(hook, "python", lambda paths: [])
    monkeypatch.setattr(hook, "prose", lambda paths: [])
    assert hook.diff_gate() == 0
    monkeypatch.setattr(hook, "prose", lambda paths: hook.UNFINISHED.append("check_prose.py") or [])
    assert hook.diff_gate() == 1
    assert "Unfinished within the gate's time: check_prose.py" in capsys.readouterr().out


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


def _clone(a, b):
    return {"firstFile": {"name": a, "start": 1, "end": 6}, "secondFile": {"name": b, "start": 11, "end": 16}}


def test_a_clone_counts_as_created_only_where_its_pair_of_files_holds_more_than_at_head():
    art = _clone("src/art.h", "src/art.h")          # two animation frames sharing rows, there before the change
    copy = _clone("test/a.cpp", "test/a.cpp")       # a block the change pasted
    assert hook.clones_created([art, copy], now=[art, copy], head=[art]) == [copy]
    assert hook.clones_created([art], now=[art], head=[art]) == []          # an edit inside an existing clone creates none
    assert hook.clones_created([copy], now=[copy], head=[]) == [copy]


def test_a_second_clone_in_a_pair_of_files_counts_as_created_whichever_order_names_them():
    old = _clone("src/a.h", "src/b.h")
    new = {"firstFile": {"name": "src/b.h", "start": 30, "end": 36}, "secondFile": {"name": "src/a.h", "start": 40, "end": 46}}
    assert hook.clones_created([new], now=[old, new], head=[old]) == [new]


def test_every_touched_clone_counts_when_head_could_not_be_scanned():
    copy = _clone("test/a.cpp", "src/b.h")
    assert hook.clones_created([copy], now=[copy], head=None) == [copy]
