# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest", "jsonschema>=4"]
# ///
"""The test registry: every test by a derived id, valid against its schema, sharded by subject."""

import json
import sys
from pathlib import Path

import jsonschema
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / "docs"))
import _test_metadata  # noqa: E402
import gen_tests  # noqa: E402

SCHEMA = json.loads((Path(gen_tests.__file__).parent / "tests.schema.json").read_text(encoding="utf-8"))


def _no_other_kinds(monkeypatch):
    """A test that fakes unit tests and scenarios leaves out the Python, JS and check tests this repository registers."""
    for listing in ("python_tests", "js_tests", "check_scripts"):
        monkeypatch.setattr(gen_tests, listing, lambda: [])


def test_the_registry_of_this_repository_is_valid_against_its_schema():
    jsonschema.Draft202012Validator(SCHEMA).validate({"tests": gen_tests.registry()})


def test_a_unit_test_is_named_by_its_file_and_its_own_sentence_with_its_line_and_subjects():
    tests = gen_tests.registry()
    test_id = "unit:test/unit/core/unit_ModuleContract.cpp::" + next(
        t["title"] for i, t in tests.items() if i.startswith("unit:test/unit/core/unit_ModuleContract.cpp::"))
    entry = tests[test_id]
    assert entry["kind"] == "unit" and entry["size"] == "small" and entry["line"] > 1
    assert entry["subjects"][0] == "MoonModule"


def test_a_scenario_is_named_by_its_file_stem_and_carries_its_own_steps_in_order():
    entry = gen_tests.registry()["scenario:scenario_Control_a_palette_preset_changes_only_the_palette"]
    assert entry["kind"] == "scenario" and entry["size"] == "medium"
    assert entry["sequence"] and all("name" in s for s in entry["sequence"])
    assert entry["run"]["args"] == "--name scenario_Control_a_palette_preset_changes_only_the_palette"


def test_a_live_only_scenario_is_a_live_test_of_the_large_size():
    live = {i: t for i, t in gen_tests.registry().items() if t["kind"] == "live"}
    assert live and all(i.startswith("live:") and t["size"] == "large" for i, t in live.items())


def test_two_tests_with_one_id_are_refused_since_an_id_names_exactly_one_test(monkeypatch):
    twice = {"path": Path(_test_metadata.UNIT_DIR / "unit_X.cpp"), "module": "M", "also": [], "cases": [("same", None), ("same", None)], "case_lines": [1, 2], "case_steps": [[], []]}
    monkeypatch.setattr(_test_metadata, "collect_unit_files", lambda: [twice])
    monkeypatch.setattr(_test_metadata, "collect_scenario_files", lambda: [])
    _no_other_kinds(monkeypatch)
    with pytest.raises(ValueError, match="two tests share the id"):
        gen_tests.registry()


def test_a_test_about_two_subjects_is_in_the_shard_of_each(monkeypatch):
    two = {"path": Path(_test_metadata.UNIT_DIR / "unit_X.cpp"), "module": "A", "also": ["B"], "cases": [("it works", "why")], "case_lines": [3], "case_steps": [[]]}
    monkeypatch.setattr(_test_metadata, "collect_unit_files", lambda: [two])
    monkeypatch.setattr(_test_metadata, "collect_scenario_files", lambda: [])
    _no_other_kinds(monkeypatch)
    index, by_subject = gen_tests.shards(gen_tests.registry(), {"modules": {}, "catalogs": {}})
    assert {s: v["kinds"] for s, v in index["subjects"].items()} == {"A": {"unit": 1}, "B": {"unit": 1}} and index["tests"] == 1
    assert set(by_subject["A"]["tests"]) == set(by_subject["B"]["tests"])


def test_a_module_is_grouped_by_the_help_page_it_is_on_and_anything_else_by_the_domain_of_its_tests():
    help_json = {"modules": {"RainbowEffect": {"doc": "light/effects.md#rainboweffect"}, "SystemModule": {"doc": "core/system.md"}},
                 "catalogs": {"light/effects.md": {"title": "Effects"}, "core/system.md": {"title": "System"}}}
    tests = {"u": {"file": "test/unit/core/unit_Buffer.cpp"}}
    assert gen_tests.group_of("RainbowEffect", tests, help_json) == "Light \u00b7 Effects"
    assert gen_tests.group_of("SystemModule", tests, help_json) == "Core \u00b7 System"
    assert gen_tests.group_of("Buffer", tests, help_json) == "Core \u00b7 Internals"
    assert gen_tests.group_of("Uncategorized", {}, help_json) == "Other"


def test_every_subject_of_the_repository_has_a_group_and_the_groups_are_the_help_pages_and_the_kinds_of_test():
    index, _ = gen_tests.shards(gen_tests.registry())
    groups = {v["group"] for v in index["subjects"].values()}
    assert all(v["group"] for v in index["subjects"].values()), groups
    assert {"Core \u00b7 Internals", "Light \u00b7 Effects", "Scripts \u00b7 Python tests", "Scripts \u00b7 Checks", "UI \u00b7 JS tests"} <= groups, groups


def _scenario(steps, instances):
    return {"path": Path(_test_metadata.SCENARIO_DIR / "core" / "scenario_X.json"), "module": "Network", "also": [], "instances": instances, "name": "scenario_X",
            "description": "", "mode": "mutate", "live_only": False, "steps_proved": [], "steps": steps}


def _step(name, op, id_, key):
    return {"name": name, "description": None, "op": op, "measure": False, "contract": {}, "observed": {}, "id": id_, "key": key}


def test_a_scenario_names_its_controls_by_type_through_the_instances_it_declares(monkeypatch):
    s = _scenario([_step("a", "set_control", "Net", "mDNS"), _step("b", "expect_control", "Net", "mDNS"), _step("c", "reboot", None, None)], {"Net": "NetworkModule"})
    monkeypatch.setattr(_test_metadata, "collect_unit_files", lambda: [])
    monkeypatch.setattr(_test_metadata, "collect_scenario_files", lambda: [s])
    _no_other_kinds(monkeypatch)
    reg = gen_tests.registry()
    assert reg["scenario:scenario_X"]["controls"] == ["NetworkModule.mDNS"]
    assert gen_tests.by_control(reg) == {"NetworkModule.mDNS": [{"id": "scenario:scenario_X", "title": "X", "subject": "Network"}]}


def test_a_step_naming_an_instance_the_scenario_does_not_declare_is_refused_rather_than_guessed(monkeypatch):
    s = _scenario([_step("a", "set_control", "Mystery", "x")], {})
    monkeypatch.setattr(_test_metadata, "collect_unit_files", lambda: [])
    monkeypatch.setattr(_test_metadata, "collect_scenario_files", lambda: [s])
    _no_other_kinds(monkeypatch)
    with pytest.raises(ValueError, match="names the instance Mystery, which the scenario does not declare"):
        gen_tests.registry()


def test_a_steps_instance_has_the_type_its_last_state_document_gave_it_and_a_declared_one_a_document_creates_is_refused(monkeypatch):
    grid = dict(_step("grid", "apply_state", None, None), document={"Layouts": {"Grid": {"type": "GridLayout"}}})
    wheel = dict(_step("wheel", "apply_state", None, None), document={"Layouts": {"Grid": {"type": "WheelLayout"}}})
    s = _scenario([grid, _step("wide", "set_control", "Grid", "width"), wheel, _step("spokes", "expect_control", "Grid", "spokes")], {})
    monkeypatch.setattr(_test_metadata, "collect_unit_files", lambda: [])
    monkeypatch.setattr(_test_metadata, "collect_scenario_files", lambda: [s])
    _no_other_kinds(monkeypatch)
    assert gen_tests.registry()["scenario:scenario_X"]["controls"] == ["GridLayout.width", "WheelLayout.spokes"], "the swap is read as it is at each step"
    s["instances"] = {"Grid": "WheelLayout"}
    with pytest.raises(ValueError, match="declares Grid, which step grid creates before any step uses it"):
        gen_tests.registry()
    s["steps"].insert(0, _step("boot", "expect_control", "Grid", "width"))   # a module the device already has, used before a document replaces it
    assert gen_tests.registry()["scenario:scenario_X"]["controls"] == ["GridLayout.width", "WheelLayout.spokes", "WheelLayout.width"]


def test_a_step_is_tested_by_the_scenarios_that_set_or_expect_the_control_it_sets_or_expects():
    tests = {"scenario:scenario_A": {"title": "scenario_A", "subjects": ["Drivers"], "controls": ["Drivers.brightness"]},
             "scenario:scenario_B": {"title": "scenario_B", "subjects": ["Layer"], "controls": ["Layer.blendMode"]}}
    processes = {"p": {"steps": [{"id": "set", "when": {"set": {"module": "Drivers", "control": "brightness", "value": 20}}},
                                 {"id": "expect", "when": {"manual": "look"}, "then": [{"module": "Layer", "control": "blendMode", "equals": "add"}]},
                                 {"id": "none", "when": {"manual": "look"}}]}}
    assert gen_tests.step_tests(processes, tests) == {
        "p/set": [{"title": "A", "href": "reference/tests.html#drivers:scenario:scenario_A"}],
        "p/expect": [{"title": "B", "href": "reference/tests.html#layer:scenario:scenario_B"}]}


def test_a_step_tag_above_a_case_names_the_process_steps_it_proves_without_joining_its_description(tmp_path):
    f = tmp_path / "unit_X.cpp"
    f.write_text('/// @module X\n\n// It says what the case is for.\n// @step first-script/errors, http-api/set\nTEST_CASE("one") {}\n\nTEST_CASE("two") {}\n', encoding="utf-8")
    parsed = _test_metadata.parse_unit_file(f)
    assert parsed["case_steps"] == [["first-script/errors", "http-api/set"], []]
    assert parsed["cases"][0] == ("one", "It says what the case is for.")


def test_a_test_naming_a_step_no_process_has_is_reported_by_the_test_and_the_step():
    tests = {"scenario:scenario_X": {"title": "scenario_X", "subjects": ["M"], "steps": ["p/gone", "p/here"]}}
    assert gen_tests.unknown_steps({"p": {"steps": [{"id": "here"}]}}, tests) == ["scenario:scenario_X: names the step p/gone, which no process has"]


def test_a_test_that_names_a_step_comes_before_the_ones_that_only_share_its_control():
    tests = {"scenario:scenario_B": {"title": "scenario_B", "subjects": ["Drivers"], "controls": ["Drivers.brightness"]},
             "scenario:scenario_A": {"title": "scenario_A", "subjects": ["Drivers"], "steps": ["p/set"]}}
    processes = {"p": {"steps": [{"id": "set", "when": {"set": {"module": "Drivers", "control": "brightness", "value": 20}}}]}}
    assert [x["title"] for x in gen_tests.step_tests(processes, tests)["p/set"]] == ["A", "B"]


def test_the_gaps_are_the_device_changing_steps_no_test_covers_the_modules_no_test_is_about_and_the_controls_the_help_lacks():
    tests = {"scenario:scenario_A": {"title": "scenario_A", "subjects": ["Drivers"], "controls": ["Drivers.brightness", "Drivers.shade"], "steps": ["p/covered"]}}
    processes = {"p": {"steps": [
        {"id": "covered", "say": "Covered.", "when": {"add": {"module": "Effects", "type": "RainbowEffect"}}},
        {"id": "bare", "say": "Bare.", "when": {"set": {"module": "Fire", "control": "x", "value": 1}}},
        {"id": "read", "say": "Reads.", "when": {"call": {"operation": "getState", "method": "GET"}}},
        {"id": "write", "say": "Writes.", "when": {"call": {"operation": "applyState", "method": "PATCH", "body": "{}"}}}]}}
    help_json = {"modules": {"Drivers": {"controls": {"brightness": {}}}, "FireEffect": {"controls": {}}}, "catalogs": {}}
    assert gen_tests.gaps(processes, help_json, tests) == {
        "steps": [{"step": "p/bare", "say": "Bare.", "verb": "set"}, {"step": "p/write", "say": "Writes.", "verb": "call"}],
        "modules": ["FireEffect"], "unknownControls": ["Drivers.shade"]}


def test_a_doctest_junit_file_becomes_runs_by_the_derived_id_with_failures_and_skips_told_apart(tmp_path):
    import test_runs
    root = tmp_path / "repo"
    xml = (f'<testsuites><testsuite name="x">'
                   f'<testcase classname="{root}/test/unit/core/unit_A.cpp" name="it passes" time="0.5"/>'
                   f'<testcase classname="{root}/test/unit/core/unit_A.cpp" name="it fails" time="0.1"><failure message="m"/></testcase>'
                   f'<testcase classname="{root}/test/unit/core/unit_A.cpp" name="it skips" time="0"><skipped/></testcase>'
                   '</testsuite></testsuites>')
    assert test_runs.doctest_runs(xml, root) == {
        "unit:test/unit/core/unit_A.cpp::it passes": {"status": "pass", "seconds": 0.5},
        "unit:test/unit/core/unit_A.cpp::it fails": {"status": "fail", "seconds": 0.1},
        "unit:test/unit/core/unit_A.cpp::it skips": {"status": "skip", "seconds": 0.0}}


def test_a_recorded_run_is_current_only_on_the_tree_it_saw(tmp_path):
    import test_runs
    path = tmp_path / "runs.json"
    test_runs.record({"unit:f::a": {"status": "pass", "seconds": 0.1}}, path, tree="T1")
    assert test_runs.published(path, tree="T1")["unit:f::a"]["current"] is True
    assert test_runs.published(path, tree="T2")["unit:f::a"]["current"] is False
    assert test_runs.published(tmp_path / "none.json", tree="T1", checks=False) == {}


def test_doctests_one_output_file_is_told_into_its_console_text_and_its_junit_document():
    import test_runs
    text = '[doctest] version\n<?xml version="1.0" encoding="UTF-8"?>\nTEST CASE: x\n<testsuites><testsuite/></testsuites>\n'
    console, xml = test_runs.split_doctest_output(text)
    assert xml == "<testsuites><testsuite/></testsuites>" and "TEST CASE: x" in console and "<testsuites" not in console
    assert test_runs.split_doctest_output("no results here") == ("no results here", "")


def test_pytest_and_node_junit_files_become_runs_by_id_and_a_title_two_files_share_lands_on_both(tmp_path):
    import test_runs
    py = tmp_path / "p.xml"
    py.write_text('<testsuites><testsuite><testcase classname="test.python.test_a" name="test_it" time="0.2"/>'
                  '<testcase classname="test.python.test_a" name="test_bad" time="0.1"><failure/></testcase></testsuite></testsuites>', encoding="utf-8")
    assert test_runs.pytest_runs(py) == {"python:test/python/test_a.py::test_it": {"status": "pass", "seconds": 0.2},
                                         "python:test/python/test_a.py::test_bad": {"status": "fail", "seconds": 0.1}}
    node = tmp_path / "n.xml"
    node.write_text('<testsuites><testcase name="a title" time="0.01" classname="test"/><testcase name="unknown" time="0" classname="test"/></testsuites>', encoding="utf-8")
    titles = {"a title": ["js:test/js/a.test.mjs::a title", "js:test/js/b.test.mjs::a title"]}
    assert set(test_runs.node_runs(node, titles)) == {"js:test/js/a.test.mjs::a title", "js:test/js/b.test.mjs::a title"}


def test_scenario_results_and_a_checks_last_run_become_runs_by_id(tmp_path):
    import json
    import test_runs
    assert test_runs.scenario_runs([("scenario", "scenario_A", "pass", 1.23456), ("live", "scenario_B", "skip", 0.0)]) == {
        "scenario:scenario_A": {"status": "pass", "seconds": 1.235}, "live:scenario_B": {"status": "skip", "seconds": 0.0}}
    runs, dur = tmp_path / "r.json", tmp_path / "d.json"
    runs.write_text(json.dumps({"moondeck/check/check_x.py": {"passed": False, "tree": "T"}, "moondeck/check/check_y.py --fast": {"passed": True}}), encoding="utf-8")
    dur.write_text(json.dumps({"moondeck/check/check_x.py": [3.0, 5.0]}), encoding="utf-8")
    assert test_runs.check_runs(runs, dur) == {"check:check_x": {"status": "fail", "seconds": 5.0, "tree": "T"}}


def test_the_python_js_and_check_tests_of_this_repository_are_registered_in_groups_of_their_own():
    reg = gen_tests.registry()
    assert {t["kind"] for t in reg.values()} >= {"unit", "scenario", "live", "python", "js", "check"}
    index, _ = gen_tests.shards(reg)
    groups = {v["group"] for v in index["subjects"].values()}
    assert {"Scripts · Python tests", "UI · JS tests", "Scripts · Checks"} <= groups


def test_a_checks_findings_are_kept_in_one_shape_and_read_back_as_rules_with_counts_and_every_file_with_its_errors(tmp_path):
    sys.path.insert(0, str(Path(gen_tests.__file__).resolve().parents[1] / "check"))
    import _findings
    _findings.write("check_x", {"long function": {"reason": "over 60 lines", "standard": "s.md#a"}, "other": {"reason": "r"}},
                    [{"rule": "long function", "message": "f", "file": "a.cpp", "value": 70}, {"rule": "long function", "message": "g", "file": "a.cpp"},
                     {"rule": "long function", "message": "h", "file": "b.cpp", "line": 3, "level": "error"}], tmp_path)
    got = _findings.read_all(tmp_path, metrics=tmp_path)["check_x"]
    assert got["total"] == 3 and got["rules"]["long function"]["count"] == 3 and got["rules"]["other"]["count"] == 0
    assert got["files"] == [{"file": "a.cpp", "count": 2}, {"file": "b.cpp", "count": 1, "errors": [{"line": 3, "message": "h"}]}]


def test_a_ratcheted_check_that_never_ran_shows_the_counts_committed_with_the_tree(tmp_path):
    sys.path.insert(0, str(Path(gen_tests.__file__).resolve().parents[1] / "check"))
    import _findings
    (tmp_path / "code.json").write_text('{"(total)": 3, "long function": 3}')
    (tmp_path / "repo-health.json").write_text('{"x": 1}')   # a metric without a check is not one
    assert _findings.committed_counts(tmp_path) == {"check_code": {"rules": {"long function": {"count": 3}}, "total": 3, "files": [], "committed": True}}


def test_each_subject_of_the_index_says_how_many_tests_passed_failed_or_have_no_result_and_what_its_check_found():
    index = {"subjects": {"A": {"kinds": {"unit": 2}}, "check_x": {"kinds": {"check": 1}}, "C": {"kinds": {"unit": 1}}}}
    by_subject = {"A": {"tests": {"unit:a1": {}, "unit:a2": {}}}, "check_x": {"tests": {"check:check_x": {}}}, "C": {"tests": {"unit:c1": {}}}}
    runs = {"unit:a1": {"status": "fail"}, "unit:a2": {"status": "pass"}, "unit:c1": {"status": "pass"}}
    gen_tests.annotate(index, by_subject, runs, {"check_x": {"errors": 2, "warnings": 5}})
    assert index["subjects"]["A"] == {"kinds": {"unit": 2}, "failed": 1, "passed": 1}
    assert index["subjects"]["check_x"] == {"kinds": {"check": 1}, "errors": 2, "warnings": 5, "norun": 1}
    assert index["subjects"]["C"] == {"kinds": {"unit": 1}, "passed": 1}


def test_a_python_test_reads_as_a_sentence_and_its_file_says_what_it_is():
    one = next(t for t in gen_tests.registry().values() if t["kind"] == "python" and t["file"].endswith("test_gen_tests.py"))
    assert not one["title"].startswith("test_") and "_" not in one["title"]
    assert gen_tests.file_title("test/python/test_gen_tests.py").startswith("The test registry")
    assert gen_tests.file_title("test/js/moonwalk-device.test.mjs").startswith("MoonWalk on a device")


def test_a_subcase_run_counts_for_the_longest_registered_case_it_belongs_to(tmp_path):
    import test_runs
    path = tmp_path / "runs.json"
    test_runs.record({"unit:f::a/b/c": {"status": "fail", "seconds": 0.1}}, path, tree="T1")
    got = test_runs.published(path, tree="T1", known={"unit:f::a", "unit:f::a/b"}, checks=False)
    assert set(got) == {"unit:f::a/b"} and got["unit:f::a/b"]["status"] == "fail", "a case whose own name holds a `/` keeps its subcases"
