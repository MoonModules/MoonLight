"""MoonWalk's rule for a contributor's script steps: a step is triggered when the change touches its paths, and its pass holds while those paths are unchanged since it ran.

Each test builds a small git repository of its own, so the trees and the change are the test's. Run: `uv run moondeck/test/test_host.py --python`.
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

import walk_status  # noqa: E402
from walk_status import matches, run_key, status, tree_now  # noqa: E402


def test_a_glob_crosses_folders_with_two_stars_and_stays_in_one_with_one():
    assert matches("src/ui/app.js", ["src/**"])
    assert matches("README.md", ["**/*.md"]) and matches("docs/reference/faq.md", ["**/*.md"])
    assert not matches("src/ui/app.js", ["src/*.js"])


def test_the_last_matching_pattern_decides_so_an_exclusion_carves_out_a_folder():
    globs = ["src/**", "!src/platform/desktop/**"]
    assert matches("src/core/Module.h", globs)
    assert not matches("src/platform/desktop/main.cpp", globs)


def git(repo: Path, *args):
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True)


def repo_with(tmp_path: Path) -> Path:
    for name, text in (("src/a.cpp", "a"), ("docs/b.md", "b")):
        (tmp_path / name).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / name).write_text(text)
    git(tmp_path, "init", "-q")
    git(tmp_path, "add", "-A")
    git(tmp_path, "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null", "commit", "-q", "-m", "start")   # no signing or hooks from the machine
    return tmp_path


MODEL = {"processes": {"commit": {"steps": [
    {"id": "build", "given": [{"changed": ["src/**"]}], "when": {"run": {"script": "moondeck/build/build_desktop.py"}}},
    {"id": "docs", "given": [{"changed": ["**/*.md"]}], "when": {"run": {"script": "moondeck/docs/build_docs.py", "args": "--strict"}}},
    {"id": "stage", "when": {"manual": "Stage."}},
]}}}


def by_id(result: dict) -> dict:
    return {s["id"]: s for s in result["commit"]}


def test_a_step_is_triggered_only_when_the_change_touches_its_paths(tmp_path):
    repo = repo_with(tmp_path)
    (repo / "src/a.cpp").write_text("a2")
    steps = by_id(status(MODEL, {}, root=repo))
    assert steps["build"]["triggered"] and not steps["docs"]["triggered"]
    assert "stage" not in steps   # only steps that run a script have a status


def test_a_pass_holds_until_a_file_it_covers_changes(tmp_path):
    repo = repo_with(tmp_path)
    (repo / "src/a.cpp").write_text("a2")
    runs = {run_key("moondeck/build/build_desktop.py"): {"tree": tree_now(repo), "passed": True}}
    (repo / "docs/b.md").write_text("b2")   # outside the build's paths
    assert by_id(status(MODEL, runs, root=repo))["build"]["fresh"]
    (repo / "src/a.cpp").write_text("a3")   # inside them
    assert not by_id(status(MODEL, runs, root=repo))["build"]["fresh"]


def test_a_pass_that_writes_a_file_it_covers_still_holds(tmp_path):
    # A docs check rewrites its own report: the tree recorded is the one the run leaves, so its output cannot make it stale.
    repo = repo_with(tmp_path)
    (repo / "docs/report.md").write_text("written by the run")
    walk_status.record_finished(tmp_path / "walk-runs.json", run_key("moondeck/docs/build_docs.py", "--strict"), True, root=repo)
    runs = walk_status.read_runs(tmp_path / "walk-runs.json")
    assert by_id(status(MODEL, runs, root=repo))["docs"]["fresh"]


def test_a_failed_run_or_one_with_other_arguments_proves_nothing(tmp_path):
    repo = repo_with(tmp_path)
    tree = tree_now(repo)
    failed = {run_key("moondeck/build/build_desktop.py"): {"tree": tree, "passed": False}}
    assert not by_id(status(MODEL, failed, root=repo))["build"]["fresh"]
    other = {run_key("moondeck/docs/build_docs.py", "--serve"): {"tree": tree, "passed": True}}
    assert not by_id(status(MODEL, other, root=repo))["docs"]["fresh"]


def test_the_snapshot_leaves_the_real_index_as_it_was(tmp_path):
    repo = repo_with(tmp_path)
    (repo / "src/new.cpp").write_text("n")
    tree_now(repo)
    staged = subprocess.run(["git", "diff", "--cached", "--name-only"], cwd=repo, capture_output=True, text=True).stdout
    assert staged == ""   # untracked stays untracked: staging is the contributor's


def test_a_record_that_cannot_be_read_is_no_runs(tmp_path):
    (tmp_path / "walk-runs.json").write_text("{not json")
    assert walk_status.read_runs(tmp_path / "walk-runs.json") == {}
    walk_status.record_run(tmp_path / "walk-runs.json", "moondeck/x.py", "abc", True)
    assert walk_status.read_runs(tmp_path / "walk-runs.json") == {"moondeck/x.py": {"tree": "abc", "passed": True}}


def test_a_step_takes_as_long_as_its_longest_recent_run_and_is_slow_past_the_line(tmp_path):
    path = tmp_path / "durations.json"
    for seconds in (1.9, 2.1, 95.0, 2.0, 2.2, 1.8):   # one clean build among incremental ones
        walk_status.record_duration(path, "moondeck/build/build_desktop.py", seconds)
    kept = walk_status.read_runs(path)["moondeck/build/build_desktop.py"]
    assert kept == [2.1, 95.0, 2.0, 2.2, 1.8]   # the last five
    repo = repo_with(tmp_path / "repo")
    build = by_id(status(MODEL, {}, walk_status.read_runs(path), root=repo))["build"]
    assert build["seconds"] == 95.0 and build["slow"]   # a clean build can take minutes, so it is slow
    docs = by_id(status(MODEL, {}, walk_status.read_runs(path), root=repo))["docs"]
    assert docs["seconds"] is None and not docs["slow"]   # never measured


def test_an_untracked_file_triggers_the_steps_whose_paths_it_falls_under(tmp_path):
    repo = repo_with(tmp_path)
    (repo / "src/new.cpp").write_text("n")   # never added: the snapshot sees it all the same
    steps = by_id(status(MODEL, {}, root=repo))
    assert steps["build"]["triggered"] and not steps["docs"]["triggered"]


ALWAYS = {"processes": {"commit": {"steps": [{"id": "specs", "when": {"run": {"script": "moondeck/check/check_specs.py"}}}]}}}


def test_a_step_without_paths_runs_on_every_change_and_any_change_makes_its_pass_stale(tmp_path):
    repo = repo_with(tmp_path)
    (repo / "docs/b.md").write_text("b2")
    runs = {run_key("moondeck/check/check_specs.py"): {"tree": tree_now(repo), "passed": True}}
    assert by_id(status(ALWAYS, runs, root=repo))["specs"]["fresh"]
    (repo / "src/a.cpp").write_text("a2")
    specs = by_id(status(ALWAYS, runs, root=repo))["specs"]
    assert specs["triggered"] and not specs["fresh"]


def test_a_pass_on_a_tree_git_has_pruned_proves_nothing(tmp_path):
    repo = repo_with(tmp_path)
    runs = {run_key("moondeck/build/build_desktop.py"): {"tree": "0" * 40, "passed": True}}
    assert not by_id(status(MODEL, runs, root=repo))["build"]["fresh"]


def test_a_merge_reads_the_change_against_where_the_branch_left_main_and_a_release_every_step(tmp_path):
    repo = repo_with(tmp_path)
    git(repo, "branch", "-M", "main")
    git(repo, "switch", "-q", "-c", "topic")
    (repo / "src/a.cpp").write_text("a2")
    git(repo, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-am", "change")
    assert not by_id(status(MODEL, {}, root=repo))["build"]["triggered"]            # committed: nothing against HEAD
    assert by_id(status(MODEL, {}, root=repo, base="main"))["build"]["triggered"]   # the branch's change
    assert all(s["triggered"] for s in status(MODEL, {}, root=repo, every=True)["commit"])


def test_a_step_names_its_call_and_only_the_process_file_holds_it():
    model = {"processes": {"commit": {"steps": MODEL["processes"]["commit"]["steps"]}}}
    assert walk_status.step_run(model, "commit/docs") == {"script": "moondeck/docs/build_docs.py", "args": "--strict"}
    assert walk_status.step_run(model, "commit/stage") is None       # a manual step runs nothing
    assert walk_status.step_run(model, "commit/nothing") is None
    assert walk_status.step_run(model, "../etc/passwd") is None


def test_two_runs_ending_together_both_land_in_the_record(tmp_path):
    import threading
    path = tmp_path / "walk-runs.json"
    threads = [threading.Thread(target=walk_status.record_run, args=(path, f"moondeck/x{i}.py", "t", True)) for i in range(20)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert len(walk_status.read_runs(path)) == 20


def test_a_card_counts_its_own_calls_but_not_another_cards():
    sys.path.insert(0, str(ROOT / "moondeck"))
    from moondeck import SCRIPTS, card_duration
    build = next(c for c in SCRIPTS if c["id"] == "build_desktop")
    tests = next(c for c in SCRIPTS if c.get("args") == ["--tests"] and c["script"] == "build/build_desktop.py")
    durations = {"moondeck/build/build_desktop.py --tests": [30.1], "moondeck/build/build_desktop.py": [1.9]}
    assert card_duration(build, durations)["seconds"] == 1.9    # the Compile Tests card's call is not the Build card's
    assert card_duration(tests, durations)["seconds"] == 30.1


def test_a_step_with_a_placeholder_in_its_call_is_the_readers_to_run():
    model = {"processes": {"p": {"steps": [{"id": "flash", "when": {"run": {"script": "moondeck/x.py", "args": "--port <port>"}}}]}}}
    assert walk_status.step_run(model, "p/flash") is None


def test_a_step_running_a_subprocess_reports_the_subprocess_steps_under_their_own_ids(tmp_path):
    repo = repo_with(tmp_path)
    (repo / "src/a.cpp").write_text("a2")
    model = {"processes": {
        "commit": {"steps": [{"id": "specs", "when": {"run": {"script": "moondeck/check/check_specs.py"}}},
                             {"id": "code", "when": {"process": "commit-code"}}]},
        "commit-code": {"steps": MODEL["processes"]["commit"]["steps"]},
    }}
    result = status(model, {}, root=repo)
    assert [(s["process"], s["id"]) for s in result["commit"]] == [("commit", "specs"), ("commit-code", "build"), ("commit-code", "docs")]
    assert result["commit"][1]["triggered"] and not result["commit"][2]["triggered"]   # each keeps its own trigger
    assert [s["id"] for s in result["commit-code"]] == ["build", "docs"]
    assert walk_status.step_run(model, "commit-code/build") == {"script": "moondeck/build/build_desktop.py"}   # a reported step runs by its own process and id


def test_a_calls_paths_guard_the_steps_it_calls_so_a_docs_change_runs_no_code_check(tmp_path):
    repo = repo_with(tmp_path)
    model = {"processes": {
        "commit-code": {"steps": [{"id": "tests", "given": [{"changed": ["src/**"]}], "when": {"process": "desktop-tests"}}]},
        "desktop-tests": {"steps": [{"id": "test", "when": {"run": {"script": "moondeck/test/test_desktop.py"}}}]},
    }}
    (repo / "docs/b.md").write_text("b2")
    assert not status(model, {}, root=repo)["commit-code"][0]["triggered"]
    assert status(model, {}, root=repo)["desktop-tests"][0]["triggered"]   # called on its own, it has no guard
    (repo / "src/a.cpp").write_text("a2")
    assert status(model, {}, root=repo)["commit-code"][0]["triggered"]


def test_a_subprocess_reached_twice_or_through_a_cycle_is_read_once(tmp_path):
    repo = repo_with(tmp_path)
    build = {"id": "build", "when": {"run": {"script": "moondeck/build/build_desktop.py"}}}
    model = {"processes": {
        "a": {"steps": [build, {"id": "to-b", "when": {"process": "b"}}, {"id": "again", "when": {"process": "b"}}]},
        "b": {"steps": [{"id": "test", "when": {"run": {"script": "moondeck/test/test_desktop.py"}}}, {"id": "back", "when": {"process": "a"}}]},
        "lost": {"steps": [{"id": "nowhere", "when": {"process": "missing"}}]},
    }}
    result = status(model, {}, root=repo)
    assert [(s["process"], s["id"]) for s in result["a"]] == [("a", "build"), ("b", "test")]
    assert [(s["process"], s["id"]) for s in result["b"]] == [("b", "test"), ("a", "build")]
    assert "lost" not in result   # a process naming none that exists runs no script
