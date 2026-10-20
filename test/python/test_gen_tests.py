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
    live = [i for i, t in gen_tests.registry().items() if t["kind"] == "live"]
    assert live and all(i.startswith("live:") for i in live)


def test_two_tests_with_one_id_are_refused_since_an_id_names_exactly_one_test(monkeypatch):
    twice = {"path": Path(_test_metadata.UNIT_DIR / "unit_X.cpp"), "module": "M", "also": [], "cases": [("same", None), ("same", None)], "case_lines": [1, 2], "case_steps": [[], []]}
    monkeypatch.setattr(_test_metadata, "collect_unit_files", lambda: [twice])
    monkeypatch.setattr(_test_metadata, "collect_scenario_files", lambda: [])
    with pytest.raises(ValueError, match="two tests share the id"):
        gen_tests.registry()


def test_a_test_about_two_subjects_is_in_the_shard_of_each(monkeypatch):
    two = {"path": Path(_test_metadata.UNIT_DIR / "unit_X.cpp"), "module": "A", "also": ["B"], "cases": [("it works", "why")], "case_lines": [3], "case_steps": [[]]}
    monkeypatch.setattr(_test_metadata, "collect_unit_files", lambda: [two])
    monkeypatch.setattr(_test_metadata, "collect_scenario_files", lambda: [])
    index, by_subject = gen_tests.shards(gen_tests.registry(), {"modules": {}, "catalogs": {}})
    assert {s: v["kinds"] for s, v in index["subjects"].items()} == {"A": {"unit": 1}, "B": {"unit": 1}} and index["tests"] == 1
    assert set(by_subject["A"]["tests"]) == set(by_subject["B"]["tests"])


def test_writing_leaves_an_index_and_a_shard_per_subject_and_removes_a_stale_one(tmp_path):
    (tmp_path / "Gone.json").write_text("{}", encoding="utf-8")
    n = gen_tests.write(tmp_path)
    index = json.loads((tmp_path / "index.json").read_text(encoding="utf-8"))
    assert n == len(index["subjects"]) == len(index["files"])
    assert not (tmp_path / "Gone.json").exists()
    assert all((tmp_path / f).exists() for f in index["files"].values())


def test_a_module_is_grouped_by_the_help_page_it_is_on_and_anything_else_by_the_domain_of_its_tests():
    help_json = {"modules": {"RainbowEffect": {"doc": "light/effects.md#rainboweffect"}, "SystemModule": {"doc": "core/system.md"}},
                 "catalogs": {"light/effects.md": {"title": "Effects"}, "core/system.md": {"title": "System"}}}
    tests = {"u": {"file": "test/unit/core/unit_Buffer.cpp"}}
    assert gen_tests.group_of("RainbowEffect", tests, help_json) == "Light \u00b7 Effects"
    assert gen_tests.group_of("SystemModule", tests, help_json) == "Core \u00b7 System"
    assert gen_tests.group_of("Buffer", tests, help_json) == "Core \u00b7 Internals"
    assert gen_tests.group_of("Uncategorized", {}, help_json) == "Other"


def test_every_subject_of_the_repository_has_a_group_and_the_groups_are_few():
    index, _ = gen_tests.shards(gen_tests.registry())
    groups = {v["group"] for v in index["subjects"].values()}
    assert all(v["group"] for v in index["subjects"].values()) and 4 <= len(groups) <= 14, groups


def _scenario(steps, instances):
    return {"path": Path(_test_metadata.SCENARIO_DIR / "core" / "scenario_X.json"), "module": "Network", "also": [], "instances": instances, "name": "scenario_X",
            "description": "", "mode": "mutate", "live_only": False, "steps_proved": [], "steps": steps}


def _step(name, op, id_, key):
    return {"name": name, "description": None, "op": op, "measure": False, "contract": {}, "observed": {}, "id": id_, "key": key}


def test_a_scenario_names_its_controls_by_type_through_the_instances_it_declares(monkeypatch):
    s = _scenario([_step("a", "set_control", "Net", "mDNS"), _step("b", "expect_control", "Net", "mDNS"), _step("c", "reboot", None, None)], {"Net": "NetworkModule"})
    monkeypatch.setattr(_test_metadata, "collect_unit_files", lambda: [])
    monkeypatch.setattr(_test_metadata, "collect_scenario_files", lambda: [s])
    reg = gen_tests.registry()
    assert reg["scenario:scenario_X"]["controls"] == ["NetworkModule.mDNS"]
    assert gen_tests.by_control(reg) == {"NetworkModule.mDNS": [{"id": "scenario:scenario_X", "title": "scenario_X", "subject": "Network"}]}


def test_a_step_naming_an_instance_the_scenario_does_not_declare_is_refused_rather_than_guessed(monkeypatch):
    s = _scenario([_step("a", "set_control", "Mystery", "x")], {})
    monkeypatch.setattr(_test_metadata, "collect_unit_files", lambda: [])
    monkeypatch.setattr(_test_metadata, "collect_scenario_files", lambda: [s])
    with pytest.raises(ValueError, match="names the instance Mystery, which the scenario does not declare"):
        gen_tests.registry()


def test_a_step_is_tested_by_the_scenarios_that_set_or_expect_the_control_it_sets_or_expects():
    tests = {"scenario:scenario_A": {"title": "scenario_A", "subjects": ["Drivers"], "controls": ["Drivers.brightness"]},
             "scenario:scenario_B": {"title": "scenario_B", "subjects": ["Layer"], "controls": ["Layer.blendMode"]}}
    processes = {"p": {"steps": [{"id": "set", "when": {"set": {"module": "Drivers", "control": "brightness", "value": 20}}},
                                 {"id": "expect", "when": {"manual": "look"}, "then": [{"module": "Layer", "control": "blendMode", "equals": "add"}]},
                                 {"id": "none", "when": {"manual": "look"}}]}}
    assert gen_tests.step_tests(processes, tests) == {
        "p/set": [{"title": "scenario_A", "href": "reference/tests.html#drivers:scenario:scenario_A"}],
        "p/expect": [{"title": "scenario_B", "href": "reference/tests.html#layer:scenario:scenario_B"}]}


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
    assert [x["title"] for x in gen_tests.step_tests(processes, tests)["p/set"]] == ["scenario_A", "scenario_B"]


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
    assert test_runs.published(tmp_path / "none.json", tree="T1") == {}


def test_doctests_one_output_file_is_told_into_its_console_text_and_its_junit_document():
    import test_runs
    text = '[doctest] version\n<?xml version="1.0" encoding="UTF-8"?>\nTEST CASE: x\n<testsuites><testsuite/></testsuites>\n'
    console, xml = test_runs.split_doctest_output(text)
    assert xml == "<testsuites><testsuite/></testsuites>" and "TEST CASE: x" in console and "<testsuites" not in console
    assert test_runs.split_doctest_output("no results here") == ("no results here", "")
