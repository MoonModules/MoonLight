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


def test_a_build_or_a_suite_runs_one_at_a_time_and_a_quick_check_beside_them():
    assert run_gates.heavy({"script": "moondeck/build/build_host.py", "args": "--tests"})
    assert run_gates.heavy({"script": "moondeck/test/test_tooling.py", "args": "--python"})
    assert not run_gates.heavy({"script": "moondeck/test/test_tooling.py", "args": "--js"})
    assert not run_gates.heavy({"script": "moondeck/check/check_specs.py", "args": ""})


def test_a_ratchet_rise_is_read_from_the_checks_own_report():
    tail = ["Docgen check: 0 error(s), 5 warning(s).", "", "ROSE against the committed counts. These only shrink:",
            "  member comment > 1 line: 30 -> 32", "  (total): 1715 -> 1717", "  Fix them, or say in the commit why."]
    assert run_gates.rises(tail) == [("member comment > 1 line", 30, 32), ("(total)", 1715, 1717)]


def test_a_run_proposes_a_write_time_check_for_each_rise_and_names_a_gate_grown_slower():
    record = {"gates": [{"id": "commit-docs/docgen", "result": "FAIL", "seconds": 60.0, "rises": [("hard wrap", 3, 4)]},
                        {"id": "commit/improv", "result": "SKIP", "tail": None}]}   # a skipped gate carries no output
    past = [{"gates": [{"id": "commit-docs/docgen", "result": "PASS", "seconds": 30.0}]} for _ in range(3)]
    ideas = run_gates.proposals(record, past)
    assert any("hard wrap rose 3 -> 4" in i and "hook_written.py" in i for i in ideas)
    assert any("took 60.0 s, up from a median of 30.0 s" in i for i in ideas)


def test_a_gate_that_passed_its_last_ten_runs_is_proposed_for_ci_or_the_merge_gate_alone():
    runs = [{"gates": [{"id": "commit-web/devices", "result": "PASS", "seconds": 1.0}]} for _ in range(10)]
    assert any("commit-web/devices passed its last 10 runs" in i for i in run_gates.proposals(runs[-1], runs[:-1]))


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


def test_a_merge_leaves_out_the_gates_ci_runs_and_runs_a_command_once():
    commit = {g["id"]: g for g in run_gates.gates("commit", run_gates.processes())}
    assert commit["commit-docs/docgen"]["ci"] and commit["host-tests/test"]["ci"]   # a process a ci step names is ci too
    assert not commit["commit-docs/docs"]["ci"] and not commit["commit-code/esp32-fresh"]["ci"]
    run, skipped, _ = run_gates.select("pre-merge", "HEAD", False, None)
    why = {g["id"]: g.get("why", "") for g in skipped}
    assert "CI runs it" in why["commit-docs/docgen"]
    assert all(not g.get("ci") for g in run)
    assert not any(g["id"] == "commit-code/kpi" and g.get("ci") for g in run)   # the KPI CI cannot measure runs at the merge
