"""run_gates.py: the gates a change triggers, read from the MoonWalk trigger table, and what each run proposes improving; _budget.py: the
job budget a run's processes share through GNU make's jobserver protocol."""

import importlib.util
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))
_spec = importlib.util.spec_from_file_location("run_gates", ROOT / "moondeck" / "check" / "run_gates.py")
run_gates = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(run_gates)
from _budget import JobBudget  # noqa: E402


def test_the_commit_gates_come_from_the_trigger_table_with_each_process_it_names():
    gates = {g["id"]: g for g in run_gates.gates("commit", run_gates.processes())}
    assert gates["commit-docs/docgen"]["script"] == "moondeck/check/check_docgen.py"
    assert "src/**" in gates["commit-docs/docgen"]["globs"]
    assert gates["commit/specs"]["globs"] == []                       # a gate naming no files runs on every change
    assert "judgment" in gates["commit/reviewer"]                      # a step that is judgment is the reader's


def test_a_process_a_step_names_inherits_that_steps_trigger():
    gates = {g["id"]: g for g in run_gates.gates("commit", run_gates.processes())}
    assert gates["host-tests/test"]["globs"] == gates["commit-code/build-host"]["globs"]   # the tests run when the code changed


def test_an_excluding_glob_is_an_excluding_pathspec():
    assert run_gates.pathspecs(["src/**", "!test/js/**"]) == [":(glob)src/**", ":(exclude,glob)test/js/**"]
    assert run_gates.pathspecs([]) == ["."]


def test_each_gate_declares_what_it_needs_and_the_cores_it_holds():
    gates = {g["id"]: g for g in run_gates.gates("commit", run_gates.processes())}
    assert gates["host-tests/test"]["needs"] == ["host-tests/build-tests"]          # a step id of its own process
    assert "host-tests/scenarios" in gates["commit-code/kpi"]["needs"]               # process/step for another's
    assert gates["commit-code/code"]["needs"] == ["commit-code/build-host"]          # it reads the warnings that build saved
    assert gates["host-tests/build-tests"]["cores"] == "all" and gates["commit-code/kpi"]["cores"] == "all"
    assert gates["commit-web/js"]["cores"] == 1                                      # a gate naming none holds one


def _schedule(monkeypatch, run, tokens=4):
    """Run `run` through the scheduler with a fake gate that sleeps a moment; each gate's start and end order."""
    import threading
    import time
    log, lock = [], threading.Lock()

    def fake(g, budget, logdir, cores=1):
        with lock:
            log.append(("start", g["id"], cores))
        time.sleep(0.05)
        with lock:
            log.append(("end", g["id"], cores))
        return {"result": "FAIL" if g.get("fails") else "PASS", "seconds": 0.05}

    monkeypatch.setattr(run_gates, "run_gate", fake)
    monkeypatch.setattr(run_gates, "_line", lambda g: g["id"])
    with JobBudget(tokens, scheduler=True) as budget:
        done = run_gates.execute(run, budget, Path("."), [], every=60)
    return log, {g["id"]: g for g in done}


def test_a_gate_starts_after_its_needs_and_an_all_cores_gate_runs_alone(monkeypatch):
    monkeypatch.delenv("MAKEFLAGS", raising=False)
    run = [{"id": "p/build", "needs": [], "cores": "all"}, {"id": "p/test", "needs": ["p/build"], "cores": 1},
           {"id": "p/lint", "needs": [], "cores": 1}, {"id": "p/kpi", "needs": ["p/test"], "cores": "all"}]
    log, done = _schedule(monkeypatch, run)
    order = [(e, gid) for e, gid, _ in log]
    assert order.index(("start", "p/test")) > order.index(("end", "p/build"))
    for alone in ("p/build", "p/kpi"):                                              # it runs alone, from its start to its end
        s, e = order.index(("start", alone)), order.index(("end", alone))
        assert e == s + 1
    assert all(g["result"] == "PASS" for g in done.values())


def test_a_failed_need_skips_its_dependents_and_theirs(monkeypatch):
    monkeypatch.delenv("MAKEFLAGS", raising=False)
    run = [{"id": "p/build", "needs": [], "cores": 1, "fails": True}, {"id": "p/test", "needs": ["p/build"], "cores": 1},
           {"id": "p/kpi", "needs": ["p/test"], "cores": 1}, {"id": "p/other", "needs": ["q/untriggered"], "cores": 1}]
    _, done = _schedule(monkeypatch, run)
    assert done["p/test"]["result"] == "SKIP" and "p/build" in done["p/test"]["why"]
    assert done["p/kpi"]["result"] == "SKIP"
    assert done["p/other"]["result"] == "PASS"                                       # a need the change does not trigger is met


def test_the_scheduler_leaves_once_the_skips_empty_the_run_instead_of_waiting_for_its_progress_line(monkeypatch):
    monkeypatch.delenv("MAKEFLAGS", raising=False)
    import threading
    waits = []

    class Cond(threading.Condition):
        def wait(self, timeout=None):
            if timeout is not None:   # the scheduler's own timed wait; the budget's untimed one is another's
                waits.append(timeout)
            return super().wait(timeout)

    monkeypatch.setattr(run_gates.threading, "Condition", Cond)
    run = [{"id": "p/build", "needs": [], "cores": 1, "fails": True}, {"id": "p/test", "needs": ["p/build"], "cores": 1}]
    _schedule(monkeypatch, run)
    assert len(waits) == 1                                                           # one wait for the build; none after the final skip


def test_the_longest_path_to_the_end_starts_first():
    run = [{"id": "p/lint", "needs": []}, {"id": "p/build", "needs": []}, {"id": "p/test", "needs": ["p/build"]}]
    past = [{"gates": [{"id": "p/lint", "result": "PASS", "seconds": 50.0}, {"id": "p/build", "result": "PASS", "seconds": 30.0},
                       {"id": "p/test", "result": "PASS", "seconds": 40.0}]}]
    prio = run_gates.priorities(run, past)
    assert prio == {"p/lint": 50.0, "p/build": 70.0, "p/test": 40.0}                 # the build heads the 70 s chain


def test_a_ratchet_rise_is_read_from_the_checks_own_report_with_its_whole_tree_mark():
    import _ratchet
    tail = ["Docgen check: 0 error(s), 5 warning(s).", "", "ROSE against the committed counts. These only shrink:",
            _ratchet.rise_line("member comment > 1 line", 30, 32, set()), _ratchet.rise_line("name spread", 4, 5, {"name spread"}),
            _ratchet.rise_line("(total)", 1715, 1717, set()), "  Fix them, or say in the commit why."]
    assert run_gates.rises(tail) == [("member comment > 1 line", 30, 32, False), ("name spread", 4, 5, True), ("(total)", 1715, 1717, True)]


def test_a_rise_the_hook_reports_per_file_names_the_bypassed_hook_and_a_whole_tree_rise_asks_for_a_design():
    record = {"gates": [{"id": "commit-docs/docgen", "result": "FAIL", "seconds": 1.0,
                         "rises": [("hard wrap", 3, 4, False), ("name spread", 4, 5, True), ("(total)", 7, 9, True)]}]}
    ideas = run_gates.proposals(record, [])
    assert any("hard wrap rose 3 -> 4" in i and "bypassed the hook" in i for i in ideas)
    assert any("name spread rose 4 -> 5" in i and "design change" in i for i in ideas)
    assert not any("(total)" in i for i in ideas)                 # the total rises with a rule, which carries the proposal
    old = {"gates": [{"id": "commit-docs/docgen", "result": "FAIL", "seconds": 1.0, "rises": [["hard wrap", 3, 4]]}]}
    assert any("bypassed the hook" in i for i in run_gates.proposals(old, []))   # a record written before the mark reads as per file


def test_a_gate_grown_slower_names_its_causes_from_the_record():
    record = {"gates": [{"id": "host-tests/test", "result": "PASS", "start": 10.0, "seconds": 60.0, "compiled": 202, "load": None,
                         "trigger": ["src/core/Scheduler.h", "src/a.cpp"]},
                        {"id": "commit-docs/prose", "result": "PASS", "start": 30.0, "seconds": 5.0},
                        {"id": "commit/improv", "result": "SKIP", "tail": None}]}   # a skipped gate carries no output
    past = [{"gates": [{"id": "host-tests/test", "result": "PASS", "seconds": 30.0}]} for _ in range(3)]
    slower = next(i for i in run_gates.proposals(record, past) if "took 60.0 s, up from a median of 30.0 s" in i)
    assert "compiled 202 objects" in slower and "beside it ran commit-docs/prose" in slower and "Scheduler.h" in slower
    bare = {"gates": [{"id": "host-tests/test", "result": "PASS", "seconds": 60.0}]}
    assert any("names no cause" in i for i in run_gates.proposals(bare, past))


def test_a_gate_that_passed_its_last_ten_runs_is_proposed_for_ci_or_the_merge_gate_alone():
    runs = [{"gates": [{"id": "commit-web/devices", "result": "PASS", "seconds": 1.0}]} for _ in range(10)]
    assert any("commit-web/devices passed its last 10 runs" in i for i in run_gates.proposals(runs[-1], runs[:-1]))


def test_a_schedulers_budget_hands_out_every_token_all_at_once_or_none(monkeypatch):
    monkeypatch.delenv("MAKEFLAGS", raising=False)
    with JobBudget(3, scheduler=True) as budget:                  # the dispatcher runs no job, so its implicit token is in the pipe too
        assert budget.try_acquire(2)
        assert not budget.try_acquire(2)                          # one left: none taken rather than one of the two
        assert budget.try_acquire(1)
        budget.release()
        budget.release()
        assert budget.try_acquire(2)


def test_the_budget_hands_out_its_tokens_less_the_implicit_one_and_takes_them_back(monkeypatch):
    monkeypatch.delenv("MAKEFLAGS", raising=False)   # inside a gate run the runner's own budget is announced
    with JobBudget(3) as budget:
        assert f"-j3 --jobserver-auth=fifo:{budget.path}" in budget.env()["MAKEFLAGS"]
        os.set_blocking(budget._fd, False)
        budget.acquire()
        budget.acquire()                              # two tokens in the pipe beside the implicit one
        try:
            budget.acquire()
            raise AssertionError("a third token from a budget of three")
        except BlockingIOError:
            pass
        budget.release()
        budget.acquire()                              # a released token is there again


def test_a_nested_run_joins_the_budget_its_parent_announces(monkeypatch):
    monkeypatch.delenv("MAKEFLAGS", raising=False)
    with JobBudget(2) as parent:
        monkeypatch.setenv("MAKEFLAGS", parent.env()["MAKEFLAGS"])
        with JobBudget() as child:
            assert child.path == parent.path and child.tokens >= 1


def test_a_generated_file_is_written_before_the_gates_and_left_out_of_a_merge():
    commit = {g["id"]: g for g in run_gates.gates("commit", run_gates.processes())}
    assert commit["commit-docs/help"]["generates"] == "docs/help/help.json"
    run, skipped, _ = run_gates.select("commit", "HEAD", False, None)
    assert all(g["id"] != "commit-docs/help" for g in run + skipped)   # generate() runs it, before the selection hashes its output
    assert run_gates.generators("pre-merge", "HEAD") == []                # a stale file at a merge is a failure to fix


def test_a_merge_leaves_out_the_gates_ci_runs_and_runs_a_command_once():
    commit = {g["id"]: g for g in run_gates.gates("commit", run_gates.processes())}
    assert commit["commit-docs/docgen"]["ci"] and commit["host-tests/test"]["ci"]   # a process a ci step names is ci too
    assert not commit["commit-docs/docs"]["ci"] and not commit["commit-code/esp32-fresh"]["ci"]
    run, skipped, _ = run_gates.select("pre-merge", "HEAD", False, None)
    why = {g["id"]: g.get("why", "") for g in skipped}
    assert "CI runs it" in why["commit-docs/docgen"]
    assert all(not g.get("ci") for g in run)
    assert not any(g["id"] == "commit-code/kpi" and g.get("ci") for g in run)   # the KPI CI cannot measure runs at the merge


def test_a_git_that_cannot_answer_stops_the_run_rather_than_reading_as_an_empty_diff():
    import pytest
    with pytest.raises(RuntimeError, match="git diff"):
        run_gates.changed_files("no-such-revision", ["."])


def test_without_a_named_pipe_the_budget_is_the_runs_own_semaphore(monkeypatch):
    monkeypatch.delattr(os, "mkfifo", raising=False)   # Windows has none: there the attribute is already gone
    with JobBudget(2) as budget:
        assert budget.path is None and budget.env() == dict(os.environ)   # a child is told of no pipe
        budget.acquire()                              # the one token beside the implicit one
        budget.release()


def test_a_filtered_run_writes_only_the_generated_files_a_kept_gate_depends_on(monkeypatch):
    gen = {"id": "commit-docs/help", "script": "g.py", "args": "", "globs": ["src/**"], "generates": "docs/help/help.json", "needs": []}
    docs = {"id": "commit-docs/docs", "script": "d.py", "args": "", "globs": ["docs/**"], "needs": []}
    js = {"id": "commit-web/js", "script": "j.py", "args": "", "globs": ["src/ui/**"], "needs": []}
    docgen = {"id": "commit-docs/docgen", "script": "c.py", "args": "", "globs": ["src/**"], "needs": ["commit-docs/help"]}
    kpi = {"id": "commit-code/kpi", "script": "k.py", "args": "", "globs": ["src/**"], "needs": []}
    monkeypatch.setattr(run_gates, "processes", lambda: {})
    monkeypatch.setattr(run_gates, "gates", lambda *_: [gen, docs, js, docgen, kpi])
    monkeypatch.setattr(run_gates, "changed_files", lambda base, specs: ["src/core/a.h"] if ":(glob)src/**" in specs else [])
    monkeypatch.setattr(run_gates, "reads", lambda gate, path: gate is docs)
    assert run_gates.generators("commit", "HEAD") == [gen]                     # unfiltered: as before
    assert run_gates.generators("commit", "HEAD", {"js"}) == []                # `--only js` leaves help.json alone
    assert run_gates.generators("commit", "HEAD", {"kpi"}) == []               # a gate reading the same sources, not the file, leaves it alone
    assert run_gates.generators("commit", "HEAD", {"docs"}) == [gen]           # the docs gate's globs name it, so it is written first
    assert run_gates.generators("commit", "HEAD", {"docgen"}) == [gen]         # docgen needs it, though its globs do not name it
    assert run_gates.generators("commit", "HEAD", {"help"}) == [gen]           # named itself


def test_a_gate_is_rerun_once_an_output_its_step_declares_is_deleted_or_replaced(tmp_path, monkeypatch):
    monkeypatch.setattr(run_gates, "ROOT", tmp_path)
    gate = {"outputs": ["build/esp32-s3/*.bin"], "args": ""}
    out = tmp_path / "build" / "esp32-s3"
    out.mkdir(parents=True)
    empty = run_gates.outputs_state(gate)
    (out / "MoonLight.bin").write_bytes(b"x")
    (out / "bootloader.bin").write_bytes(b"y")
    built = run_gates.outputs_state(gate)
    assert built != empty
    (out / "MoonLight.bin").unlink()                                            # another image stays, the app is gone
    assert run_gates.outputs_state(gate) != built
    (out / "MoonLight.bin").write_bytes(b"x")
    os.utime(out / "MoonLight.bin", ns=(1, 1))                                  # an older copy of the same size
    assert run_gates.outputs_state(gate) != built
    assert run_gates.outputs_state({"outputs": []}) == ""                       # a step declaring none keeps its key


def test_the_host_builds_cmake_cache_is_a_declared_output_of_the_build_steps(tmp_path, monkeypatch):
    gates = {g["id"]: g for g in run_gates.gates("commit", run_gates.processes())}
    assert gates["commit-code/build-host"]["outputs"] == ["{host_build}/CMakeCache.txt"]
    assert gates["commit-code/esp32-fresh"]["outputs"] == ["build/esp32-esp32s3-n16r8/*.bin"]
    monkeypatch.setattr(run_gates, "ROOT", tmp_path)
    sys.path.insert(0, str(ROOT / "moondeck" / "build"))
    import build_host
    cache = tmp_path / build_host.host_build_dir() / "CMakeCache.txt"
    cache.parent.mkdir(parents=True)
    cache.write_text("x")
    built = run_gates.outputs_state(gates["commit-code/build-host"])
    cache.unlink()
    assert run_gates.outputs_state(gates["commit-code/build-host"]) != built


def test_a_gate_script_is_keyed_with_every_module_it_imports_from_beside_it(tmp_path, monkeypatch):
    check = ROOT / "moondeck" / "check"
    modules = {p.name for p in run_gates.imported_scripts(check / "check_code.py")}
    assert {"check_code.py", "check_nonblocking.py", "_findings.py", "_ratchet.py"} <= modules
    (tmp_path / "gate.py").write_text("import helper\nimport os\n")
    (tmp_path / "helper.py").write_text("def f(): from deep import g\n")
    (tmp_path / "deep.py").write_text("g = 1\n")
    assert [p.name for p in run_gates.imported_scripts(tmp_path / "gate.py")] == ["deep.py", "gate.py", "helper.py"]


def test_a_gate_whose_process_cannot_start_is_a_failure_that_skips_its_dependents(tmp_path, monkeypatch):
    monkeypatch.delenv("MAKEFLAGS", raising=False)
    monkeypatch.setattr(run_gates, "ROOT", tmp_path)
    monkeypatch.setattr(run_gates.subprocess, "run", lambda *a, **k: (_ for _ in ()).throw(FileNotFoundError("uv")))
    monkeypatch.setattr(run_gates, "_line", lambda g: g["id"])
    run = [{"id": "p/build", "script": "b.py", "args": "", "needs": [], "cores": 1},
           {"id": "p/test", "script": "t.py", "args": "", "needs": ["p/build"], "cores": 1}]
    with JobBudget(2, scheduler=True) as budget:
        done = {g["id"]: g for g in run_gates.execute(run, budget, tmp_path, [], every=60)}
        assert budget.try_acquire(2)                                             # its tokens came back
    assert done["p/build"]["result"] == "FAIL" and "FileNotFoundError" in done["p/build"]["finding"]
    assert done["p/test"]["result"] == "SKIP"


def test_a_gate_is_started_through_the_nice_program_not_a_preexec_callback_a_thread_could_deadlock_on(tmp_path, monkeypatch):
    monkeypatch.setattr(run_gates, "ROOT", tmp_path)
    seen = {}
    import subprocess
    monkeypatch.setattr(run_gates.shutil, "which", lambda name: "/usr/bin/nice")
    monkeypatch.setattr(run_gates.subprocess, "run", lambda cmd, **k: seen.update(cmd=cmd, kw=k) or subprocess.CompletedProcess(cmd, 0))
    with JobBudget(1, scheduler=True) as budget:
        run_gates.run_gate({"id": "p/g", "script": "g.py", "args": "--x 1"}, budget, tmp_path)
    assert seen["cmd"][:3] == ["nice", "-n", "10"] and "preexec_fn" not in seen["kw"]


def test_needs_that_form_a_loop_fail_the_run_rather_than_skip_quietly(monkeypatch):
    monkeypatch.delenv("MAKEFLAGS", raising=False)
    run = [{"id": "p/a", "needs": ["p/b"], "cores": 1}, {"id": "p/b", "needs": ["p/a"], "cores": 1}]
    _, done = _schedule(monkeypatch, run)
    assert done["p/a"]["result"] == "FAIL" and done["p/b"]["result"] == "FAIL"
    assert "loop" in done["p/a"]["finding"]


def test_a_failed_generator_skips_the_gates_that_need_it_and_a_passed_one_releases_them(monkeypatch):
    monkeypatch.delenv("MAKEFLAGS", raising=False)
    run = [{"id": "p/docs", "needs": ["p/help"], "cores": 1}, {"id": "p/js", "needs": [], "cores": 1}]
    monkeypatch.setattr(run_gates, "run_gate", lambda g, b, d, c=1: {"result": "PASS", "seconds": 0.0})
    monkeypatch.setattr(run_gates, "_line", lambda g: g["id"])
    for result, expected in (("FAIL", "SKIP"), ("PASS", "PASS")):
        with JobBudget(2, scheduler=True) as budget:
            done = {g["id"]: g for g in run_gates.execute([dict(g) for g in run], budget, Path("."), [], every=60,
                                                          generated=[{"id": "p/help", "result": result}])}
        assert done["p/docs"]["result"] == expected and done["p/js"]["result"] == "PASS"


def test_only_a_pass_is_cached_and_a_skipped_gate_loses_its_entry(monkeypatch):
    monkeypatch.setattr(run_gates, "input_hash", lambda g, base: g["hash"])
    monkeypatch.setattr(run_gates, "outputs_state", lambda g: "")
    done = [{"id": "p/build", "result": "FAIL", "hash": "1"}, {"id": "p/code", "result": "SKIP", "hash": "2"}, {"id": "p/js", "result": "PASS", "hash": "3"}]
    cache = {"p/build": "old:", "p/code": "2:"}
    run_gates.update_cache(cache, done, "HEAD")
    assert cache == {"p/js": "3:"}


def test_the_list_names_the_judgment_gates_for_the_reader(monkeypatch, capsys):
    monkeypatch.setattr(sys, "argv", ["run_gates.py", "--list", "--process", "pre-merge"])
    assert run_gates.main() == 0
    out = capsys.readouterr().out
    assert "READER pre-merge/reviewer" in out


def test_a_check_script_that_does_not_parse_still_hashes_so_its_gate_runs_and_fails(tmp_path):
    broken = tmp_path / "check_broken.py"
    broken.write_text("def oops(:\n", encoding="utf-8")
    assert run_gates.imported_scripts(broken) == [broken]   # the run goes on, and the gate reports its own SyntaxError
