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
    assert [s["id"] for s in model["stages"]][:2] == ["read", "install"]
    assert model["processes"]["first-light-show"]["stage"] == "first-light"
    assert "first-light-show" in model["byPage"]["tutorials/first-light-show.html"]


def test_a_step_runs_in_the_runners_its_action_allows():
    steps = {s["id"]: s for s in gen_moonwalk.build()["processes"]["first-light-show"]["steps"]}
    assert "test" in steps["grid-width"]["runners"]
    assert steps["keep"]["runners"] == ["docs", "tour"]


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
    shutil.copy(ROOT / "moonwalk" / "01-read.yaml", tmp_path / "01-read.yaml")
    shutil.copy(ROOT / "moonwalk" / "01-read.yaml", tmp_path / "02-again.yaml")
    with pytest.raises(ValueError, match="turn-help-on is also in stage read"):
        gen_moonwalk.build(tmp_path)


def test_a_step_is_done_by_the_process_actor_unless_it_names_its_own():
    steps = {s["id"]: s for s in gen_moonwalk.build()["processes"]["commit"]["steps"]}
    assert steps["checks"]["actor"] == "agent"
    assert steps["stage"]["actor"] == "product-owner"


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
    assert processes["install-desktop"]["systems"] == ["MoonCore", "MoonInstaller"]   # declared MoonCore, the installer's address adds MoonInstaller, in the architecture's order
    assert "MoonDeck" in processes["commit"]["systems"]


def test_a_misspelt_expectation_key_is_refused_rather_than_never_checked(tmp_path):
    root = stage(tmp_path, "stage: T\nprocesses:\n  - id: a\n    title: A\n    actor: user\n    goal: G\n    steps:\n"
                           "      - {id: s, say: Set it., when: {set: {module: Grid, control: width, value: 16}}, then: [{module: Grid, control: width, equal: 16}]}\n")
    with pytest.raises(ValueError, match="then/0"):
        gen_moonwalk.build(root)
