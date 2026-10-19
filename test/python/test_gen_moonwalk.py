"""moonwalk.json is MoonWalk's journey map every runner reads, generated from the stage files under moonwalk/.

Each rule here is something the map or a runner relies on: every stage file in the repo is valid, a process names only processes that exist, a name YAML would turn into a boolean is refused, and a step's runners follow from its action."""

import shutil
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

import gen_moonwalk  # noqa: E402


def stage(tmp_path: Path, body: str) -> Path:
    """A moonwalk/ folder holding one stage file with this body."""
    (tmp_path / "01-test.yaml").write_text(body, encoding="utf-8")
    return tmp_path


def test_the_repo_stage_files_build_into_one_map():
    model = gen_moonwalk.build()
    assert [s["id"] for s in model["stages"]][:2] == ["install", "first-light"]
    assert model["processes"]["first-light-show"]["stage"] == "first-light"
    assert "run-linux-board" in model["byPage"]["how-to/installing-on-linux.html"]


def test_a_step_runs_in_the_runners_its_action_allows():
    steps = {s["id"]: s for s in gen_moonwalk.build()["processes"]["first-light-show"]["steps"]}
    assert "test" in steps["brightness"]["runners"]   # a set can be performed by a test
    assert steps["keep"]["runners"] == ["docs", "app"]


def test_a_dependency_on_a_missing_process_is_refused(tmp_path):
    root = stage(tmp_path, "stage: T\nprocesses:\n  - {id: a, title: A, actor: user, goal: G, dependsOn: [nowhere]}\n")
    with pytest.raises(ValueError, match="dependsOn names no process: nowhere"):
        gen_moonwalk.build(root)


def test_a_control_named_on_must_be_quoted_or_it_is_refused(tmp_path):
    # YAML 1.1 reads a bare `on` as true, so the control's name would silently become a boolean.
    root = stage(tmp_path, "stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: user\n    goal: G\n    steps:\n"
                           "      - {id: s, say: Turn it on., when: {set: {module: Drivers, control: on, value: true}}}\n")
    with pytest.raises(ValueError, match="is not of type 'string'"):
        gen_moonwalk.build(root)


def test_a_file_that_is_not_yaml_names_itself(tmp_path):
    root = stage(tmp_path, "stage: T\nprocesses:\n  - {id: a, title: A, actor: user, goal: Nothing to save: it persists}\n")
    with pytest.raises(ValueError, match="01-test.yaml"):
        gen_moonwalk.build(root)


def test_a_process_id_is_unique_across_stages(tmp_path):
    shutil.copy(ROOT / "moonwalk" / "02-install.yaml", tmp_path / "02-install.yaml")
    shutil.copy(ROOT / "moonwalk" / "02-install.yaml", tmp_path / "03-again.yaml")
    with pytest.raises(ValueError, match="know-moonlight is also in stage install"):
        gen_moonwalk.build(tmp_path)


def test_a_step_is_done_by_the_process_actor_unless_it_names_its_own(tmp_path):
    root = stage(tmp_path, "stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: contributor\n    goal: G\n    steps:\n"
                           "      - {id: own, say: Check., actor: agent, when: {manual: Check}}\n"
                           "      - {id: inherited, say: Stage., when: {manual: Stage}}\n")
    steps = {s["id"]: s for s in gen_moonwalk.build(root)["processes"]["a"]["steps"]}
    assert steps["own"]["actor"] == "agent"
    assert steps["inherited"]["actor"] == "contributor"


CHOICE = ("stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: user\n    goal: G\n"
          "    inputs: {type: object, properties: {platform: {type: string, enum: [windows, macos]}}}\n"
          "    steps:\n      - {id: s, say: Run it., given: [{input: %s, equals: %s}], when: {manual: Run it}}\n")


def test_a_choice_runs_a_step_for_one_option_of_a_declared_input(tmp_path):
    assert gen_moonwalk.build(stage(tmp_path, CHOICE % ("platform", "macos")))["processes"]["a"]["steps"][0]["given"]


def test_a_choice_naming_an_undeclared_input_is_refused(tmp_path):
    with pytest.raises(ValueError, match="given names no input of the process: os"):
        gen_moonwalk.build(stage(tmp_path, CHOICE % ("os", "macos")))


def test_a_choice_naming_an_option_the_input_lacks_is_refused(tmp_path):
    with pytest.raises(ValueError, match="platform has no option linux"):
        gen_moonwalk.build(stage(tmp_path, CHOICE % ("platform", "linux")))


def test_a_file_that_does_not_parse_is_reported_alone(tmp_path):
    (tmp_path / "02-next.yaml").write_text("stage: N\nprocesses:\n  - {id: b, title: B, actor: user, goal: G, dependsOn: [a]}\n", encoding="utf-8")
    with pytest.raises(ValueError) as caught:
        gen_moonwalk.build(stage(tmp_path, "stage: T\nprocesses:\n  - {id: a, title: A, actor: user, goal: Is it? yes: no}\n"))
    assert "01-test.yaml" in str(caught.value) and "dependsOn" not in str(caught.value)


def test_a_system_is_one_the_architecture_names(tmp_path):
    root = stage(tmp_path, "stage: T\nprocesses:\n  - {id: a, title: A, actor: user, goal: G, systems: [MoonRocket]}\n")
    with pytest.raises(ValueError, match="no system named MoonRocket"):
        gen_moonwalk.build(root)


def test_a_step_adds_the_systems_its_action_implies():
    processes = gen_moonwalk.build()["processes"]
    assert processes["install"]["systems"] == ["MoonCore", "MoonInstaller"]   # in the architecture's order, whatever order they are declared in
    assert "MoonDeck" in processes["commit"]["systems"]


def test_a_misspelled_expectation_key_is_refused_rather_than_never_checked(tmp_path):
    root = stage(tmp_path, "stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: user\n    goal: G\n    steps:\n"
                           "      - {id: s, say: Set it., when: {set: {module: GridLayout, control: width, value: 16}}, then: [{module: GridLayout, control: width, equal: 16}]}\n")
    with pytest.raises(ValueError, match="then/0"):
        gen_moonwalk.build(root)


def test_a_step_names_a_module_type_and_control_the_help_knows_or_it_is_refused(tmp_path):
    step = "      - {{id: s, say: Set it., when: {{set: {{module: {m}, control: {c}, value: 16}}}}}}\n"
    head = "stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: user\n    goal: G\n    steps:\n"
    with pytest.raises(ValueError, match="no module type Grid in help.json"):   # the instance's name is not the key
        gen_moonwalk.build(stage(tmp_path, head + step.format(m="Grid", c="width")))
    with pytest.raises(ValueError, match="GridLayout has no control widht in help.json"):
        gen_moonwalk.build(stage(tmp_path, head + step.format(m="GridLayout", c="widht")))


def test_an_expectation_is_a_value_or_a_range_never_both(tmp_path):
    head = "stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: user\n    goal: G\n    steps:\n"
    step = "      - {{id: s, say: Set it., when: {{set: {{module: Drivers, control: brightness, value: 20}}}}, then: [{{module: Drivers, control: brightness, {e}}}]}}\n"
    gen_moonwalk.build(stage(tmp_path, head + step.format(e="minimum: 1, maximum: 20")))
    gen_moonwalk.build(stage(tmp_path, head + step.format(e="maximum: 20")))
    with pytest.raises(ValueError, match="then/0"):
        gen_moonwalk.build(stage(tmp_path, head + step.format(e="equals: 20, maximum: 20")))


def test_a_step_runs_a_script_that_exists_or_it_is_refused(tmp_path):
    head = "stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: contributor\n    goal: G\n    steps:\n"
    gen_moonwalk.build(stage(tmp_path, head + "      - {id: s, say: Check., when: {run: {script: moondeck/check/check_specs.py}}}\n"))
    with pytest.raises(ValueError, match="runs no script: moondeck/check/check_spcs.py"):
        gen_moonwalk.build(stage(tmp_path, head + "      - {id: s, say: Check., when: {run: {script: moondeck/check/check_spcs.py}}}\n"))


def test_the_module_index_is_keyed_by_type_and_names_the_type_a_step_adds():
    by_module = gen_moonwalk.build()["byModule"]
    assert "first-light-show/grid-size" in by_module["GridLayout"]
    assert "first-light-show/add-effect" in by_module["Layer"]


def test_a_control_lists_the_steps_that_set_it_or_expect_its_value():
    by_control = gen_moonwalk.build()["byControl"]
    assert by_control["Drivers.brightness"] == ["first-light-show/brightness"]   # set
    assert by_control["WiFiModule.known"] == ["join-access-point/pick"]          # expected, set by hand in the WiFi card


def test_the_prose_is_plain_text_under_the_map_with_the_process_id_as_its_address():
    text = gen_moonwalk.render_text(gen_moonwalk.build())
    assert "## Show an effect on your lights {#first-light-show}" in text
    assert "### 1. Say where your lights are" in text and "### 4. Keep it" in text
    assert text.index("**Open Layouts.**") < text.index("A grid is a width, a height and a depth")   # the instruction, then its detail
    assert '<video src="assets/moontube/05-layouts.webm"' in text   # a clip plays on the page too
    assert "\n### Where to go next" in text and "\n## Where to go next" not in text   # the outro sits under its process


def test_the_map_carries_its_links_absolute_for_a_device_page():
    model = gen_moonwalk.absolute_links(gen_moonwalk.build())
    intro = model["processes"]["first-light-show"]["intro"]
    assert "(https://moonmodules.org/MoonLight/tutorials/how-moonlight-works.html)" in intro
    detail = {s["id"]: s for s in model["processes"]["first-light-show"]["steps"]}["grid-size"]["detail"]
    assert "(https://moonmodules.org/MoonLight/assets/moontube/05-layouts.webm)" in detail


def test_every_page_a_process_names_exists(tmp_path):
    root = stage(tmp_path, "stage: T\nprocesses:\n  - {id: a, title: A, actor: user, goal: G, page: nowhere.md}\n")
    with pytest.raises(ValueError, match="page does not exist: docs/nowhere.md"):
        gen_moonwalk.build(root)


def test_a_choice_shows_once_per_section_and_names_its_option_in_the_plain_text():
    text = gen_moonwalk.render_text(gen_moonwalk.build())
    desktop = text[text.index("{#install}"):text.index("{#install-windows-zip}")]
    assert desktop.count("Where your settings live") == 1   # three options, one heading
    assert "**Windows: Run the setup you kept.**" in desktop
    assert "**macOS: Drag MoonLight to Applications.**" in desktop
    assert desktop.count(". Updating\n") == 1   # a step per platform, one heading


def test_a_key_written_twice_is_refused_rather_than_the_last_kept(tmp_path):
    root = stage(tmp_path, "stage: T\nprocesses:\n  - id: a\n    title: A\n    title: B\n    actor: user\n    goal: G\n")
    with pytest.raises(ValueError, match="duplicate key 'title'"):
        gen_moonwalk.build(root)


def test_a_process_entry_that_is_not_a_mapping_is_reported_not_crashed_on(tmp_path):
    root = stage(tmp_path, "stage: T\nprocesses:\n  - just a string\n")
    with pytest.raises(ValueError, match="01-test.yaml"):
        gen_moonwalk.build(root)


def test_a_step_for_several_options_names_them_all_in_the_plain_text():
    text = gen_moonwalk.render_text(gen_moonwalk.build())
    assert "**Windows, macOS, Linux: Download the app for this computer.**" in text


def one_step(say: str, detail: str = "") -> str:
    """A stage file with one process of one step saying this, with this detail."""
    return ("stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: user\n    goal: G\n"
            f"    steps:\n      - id: s\n        say: {say}\n        when: {{manual: Do it}}\n        detail: {detail or 'D'}\n")


def test_a_step_instruction_over_eight_words_is_refused(tmp_path):
    with pytest.raises(ValueError, match="a/s: say is one action in at most 8 words"):
        gen_moonwalk.build(stage(tmp_path, one_step("Power the device on and wait until its light turns blue.")))


def test_a_step_instruction_of_two_sentences_is_refused_since_the_result_goes_in_its_note(tmp_path):
    with pytest.raises(ValueError, match="a/s: say is one action"):
        gen_moonwalk.build(stage(tmp_path, one_step("Power on the device. It opens MM-XXXX.")))


def test_a_detail_over_its_limit_is_a_warning_rather_than_a_refusal(tmp_path):
    processes = gen_moonwalk.build(stage(tmp_path, one_step("Open it.", "word " * 121)))["processes"]
    assert gen_moonwalk.size_warnings(processes) == [("test", "a: step s's detail 121 words > 120")]


def test_an_image_is_seen_rather_than_read_and_a_link_counts_its_text_only():
    assert gen_moonwalk._words("![A long description of the picture](assets/x.png) See [the page](how-to/a-long-path.md).") == 3


def test_a_process_with_a_choice_is_measured_per_reader_rather_than_as_the_sum_of_its_options(tmp_path):
    long = "word " * 110
    steps = "".join(f"      - {{id: s{i}, say: Do it., given: [{{input: platform, equals: {o}}}], when: {{manual: Do it}}, detail: {long}}}\n"
                    for i, o in enumerate(["windows", "macos"] * 3))
    body = ("stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: user\n    goal: G\n"
            "    inputs: {type: object, properties: {platform: {type: string, enum: [windows, macos]}}}\n    steps:\n" + steps)
    processes = gen_moonwalk.build(stage(tmp_path, body))["processes"]
    assert gen_moonwalk._reader_totals(processes["a"]) == {"windows": 336, "macos": 336}
    assert gen_moonwalk.size_warnings(processes) == []


def test_a_process_with_more_steps_than_a_reader_follows_is_a_warning_to_fold_them(tmp_path):
    steps = "".join(f"      - {{id: s{i}, say: Do it., when: {{manual: Do it}}}}\n" for i in range(11))
    body = "stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: user\n    goal: G\n    steps:\n" + steps
    processes = gen_moonwalk.build(stage(tmp_path, body))["processes"]
    assert gen_moonwalk.size_warnings(processes) == [("test", "a: 11 steps > 10; fold steps into a subprocess")]


def test_a_stage_with_more_processes_than_a_column_holds_is_a_warning_to_split_it(tmp_path):
    body = "stage: T\nprocesses:\n" + "".join(f"  - {{id: p{i}, title: P, actor: user, goal: G}}\n" for i in range(9))
    processes = gen_moonwalk.build(stage(tmp_path, body))["processes"]
    assert gen_moonwalk.size_warnings(processes) == [("test", "9 processes > 8; split the stage")]


def test_the_map_leaves_a_process_prose_to_its_own_file_fetched_when_the_view_opens():
    walk, texts = gen_moonwalk.split_text(gen_moonwalk.build())
    install = walk["processes"]["install"]
    assert install["text"] is True and "intro" not in install and all("detail" not in s for s in install["steps"])
    assert texts["install"]["intro"] and texts["install"]["details"]["keep"]
    assert "text" not in walk["processes"]["run-linux-board"] and "run-linux-board" not in texts   # a process with no prose of its own links its page


def test_writing_the_map_replaces_the_prose_of_a_process_that_is_gone(tmp_path):
    (tmp_path / "text").mkdir()
    (tmp_path / "text" / "removed.json").write_text("{}", encoding="utf-8")
    gen_moonwalk.write(gen_moonwalk.build(), tmp_path / "moonwalk.json")
    assert not (tmp_path / "text" / "removed.json").exists()
    assert (tmp_path / "text" / "install.json").exists() and "intro" not in (tmp_path / "moonwalk.json").read_text(encoding="utf-8")


def test_a_step_that_opens_an_address_or_reads_a_page_links_there():
    steps = {s["id"]: s for p in gen_moonwalk.build()["processes"].values() for s in p.get("steps", [])}
    assert steps["open-installer"]["url"] == "https://moonmodules.org/MoonLight/install/"
    assert steps["signal"]["url"] == "how-to/led-signal-integrity.html"


def test_a_step_reading_a_missing_page_is_refused(tmp_path):
    root = stage(tmp_path, "stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: user\n    goal: G\n    steps:\n"
                           "      - {id: s, say: Read it., when: {read: {page: nowhere.md}}}\n")
    with pytest.raises(ValueError, match="page does not exist: docs/nowhere.md"):
        gen_moonwalk.build(root)


def test_a_title_past_its_cap_or_a_goal_of_two_sentences_is_refused(tmp_path):
    long_title = stage(tmp_path, "stage: T\nprocesses:\n  - {id: a, title: " + "word " * 11 + ", actor: user, goal: G}\n")
    with pytest.raises(ValueError, match="title over 10 words"):
        gen_moonwalk.build(long_title)
    two = stage(tmp_path, "stage: T\nprocesses:\n  - {id: a, title: A, actor: user, goal: It runs. It is fast.}\n")
    with pytest.raises(ValueError, match="more than one sentence"):
        gen_moonwalk.build(two)


def test_a_step_calling_a_missing_process_or_jumping_to_a_missing_step_is_refused(tmp_path):
    call = stage(tmp_path, "stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: user\n    goal: G\n    steps:\n"
                           "      - {id: s, say: Run it., when: {process: nowhere}}\n")
    with pytest.raises(ValueError, match="runs no process: nowhere"):
        gen_moonwalk.build(call)
    jump = stage(tmp_path, "stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: user\n    goal: G\n    steps:\n"
                           "      - {id: s, say: Check it., when: {manual: Look}, onFailure: [{type: goto, stepId: gone}]}\n")
    with pytest.raises(ValueError, match="goto names no step: gone"):
        gen_moonwalk.build(jump)


def test_the_commit_checks_fold_into_one_subprocess_per_trigger_group_each_within_its_caps():
    processes = gen_moonwalk.build()["processes"]
    groups = {"docs": "commit-docs", "code": "commit-code", "web": "commit-web", "scripts": "commit-scripts"}
    parent = {s["id"]: s for s in processes["commit"]["steps"]}
    assert {sid: parent[sid]["when"].get("process") for sid in groups} == groups
    assert all("given" not in parent[sid] for sid in groups)   # a group triggers nothing itself: its steps' paths decide
    # Every check keeps its id, so a run record and a link keyed on it still find it.
    ids = [s["id"] for pid in ["commit", *groups.values()] for s in processes[pid]["steps"] if "run" in s["when"]]
    assert sorted(ids) == sorted(["specs", "prose", "commit", "docs", "docgen", "taglines", "code", "build-desktop", "build-tests", "test-desktop", "scenarios",
                                  "esp32-fresh", "esp32-fresh-s3", "esp32-fresh-p4", "no-jit", "kpi", "js", "devices", "python", "firmwares"])
    over = [w for w in gen_moonwalk.size_warnings(processes) if w[0] == "document-commit"]
    assert over == []   # the parent, each group and the stage within their caps
