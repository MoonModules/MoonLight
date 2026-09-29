"""repo-health's baseline must survive a malformed snapshot without corrupting a run.

Two separate loaders read it: `load_previous` (the COMMITTED snapshot, what the delta compares
against) and `load_working_tree` (the newest numbers, what carry-forward preserves). Both feed
`merge_carry_forward`, which does `old.get(section, {})`, so a JSON list or scalar where an object
belongs would raise, and a section holding a list would corrupt the merge.

A malformed file must also not read as "no previous numbers": that reports every metric as new,
which looks like a clean slate rather than a broken baseline. So the rule is reject-and-announce,
never silently accept.
"""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))

import repo_health  # noqa: E402


# ---- shape validation ----

def test_a_snapshot_that_is_not_an_object_is_rejected():
    """A list or scalar cannot be a snapshot; accepting one would raise inside the merge."""
    assert repo_health._valid_snapshot([1, 2, 3], "t") == {}
    assert repo_health._valid_snapshot("nope", "t") == {}
    assert repo_health._valid_snapshot(42, "t") == {}


def test_a_section_of_the_wrong_shape_is_rejected():
    """`flash`/`perf`/`complexity` are merged key-by-key, so each must be an object."""
    assert repo_health._valid_snapshot({"flash": []}, "t") == {}
    assert repo_health._valid_snapshot({"perf": 7}, "t") == {}
    assert repo_health._valid_snapshot({"complexity": "x"}, "t") == {}


def test_a_valid_snapshot_passes_through_unchanged():
    """The guard must not damage the normal case; this is the control for the rejections above."""
    good = {"flash": {"esp32": 1}, "perf": {}, "complexity": {}, "other": 5}
    assert repo_health._valid_snapshot(good, "t") is good


def test_a_snapshot_with_no_sections_is_still_valid():
    """Sections are optional: a first-ever snapshot has none, and that is not malformed."""
    assert repo_health._valid_snapshot({"lines": {"src": 10}}, "t") == {"lines": {"src": 10}}


# ---- the merge the validation protects ----

def test_carry_forward_keeps_previous_values_for_unmeasured_sections():
    """The reason the shapes matter: a run that measured nothing must not drop the old numbers."""
    # Real firmware keys: carry-forward now drops `esp32*` rows that are NOT in FIRMWARES, so a
    # made-up name would be filtered as a ghost and this would assert the wrong thing.
    old = {"flash": {"esp32": 100, "esp32s3-n16r8": 200, "desktop-macos": 300},
           "perf": {"tick": 5}, "complexity": {}}
    new = {"flash": {"esp32": 150}}
    merged = repo_health.merge_carry_forward(new, old)
    assert merged["flash"]["esp32"] == 150             # this run measured it
    assert merged["flash"]["esp32s3-n16r8"] == 200     # this run did not, so the old value survives
    assert merged["flash"]["desktop-macos"] == 300           # not an esp32* key: never filtered
    assert merged["perf"]["tick"] == 5


def test_carry_forward_drops_a_renamed_firmware():
    """A variant that no longer exists must not linger forever. Nothing measures it again, so
    without this it would be carried forward on every run — which is how `esp32p4-eth` outlived
    its rename to `esp32p4rev1-eth` and kept reporting a stale size."""
    old = {"flash": {"esp32p4-eth": 1600, "esp32p4rev1-eth": 1650, "desktop-macos": 300}}
    merged = repo_health.merge_carry_forward({"flash": {}}, old)
    assert "esp32p4-eth" not in merged["flash"]        # gone: not a known firmware
    assert merged["flash"]["esp32p4rev1-eth"] == 1650  # kept: it is one
    assert merged["flash"]["desktop-macos"] == 300           # kept: not an esp32* key at all


def test_carry_forward_survives_a_rejected_baseline():
    """A malformed baseline degrades to "nothing to carry", not to a crash."""
    merged = repo_health.merge_carry_forward({"flash": {"esp32": 1}},
                                             repo_health._valid_snapshot([1, 2], "t"))
    assert merged["flash"] == {"esp32": 1}


# ---- the loaders ----

def test_malformed_json_on_disk_reads_as_empty(tmp_path, monkeypatch):
    """Unparseable JSON is announced and treated as empty rather than raising mid-run."""
    bad = tmp_path / "repo-health.json"
    bad.write_text("{ not json", encoding="utf-8")
    monkeypatch.setattr(repo_health, "HEALTH_FILE", bad)
    assert repo_health.load_working_tree() == {}


def test_a_wrong_shape_on_disk_reads_as_empty(tmp_path, monkeypatch):
    """Valid JSON of the wrong shape is rejected by the same rule as unparseable JSON."""
    bad = tmp_path / "repo-health.json"
    bad.write_text(json.dumps([1, 2, 3]), encoding="utf-8")
    monkeypatch.setattr(repo_health, "HEALTH_FILE", bad)
    assert repo_health.load_working_tree() == {}


def test_a_good_file_on_disk_loads(tmp_path, monkeypatch):
    """The control: the loader must actually load a well-formed snapshot."""
    good = tmp_path / "repo-health.json"
    good.write_text(json.dumps({"flash": {"esp32": 1}}), encoding="utf-8")
    monkeypatch.setattr(repo_health, "HEALTH_FILE", good)
    assert repo_health.load_working_tree() == {"flash": {"esp32": 1}}


# ---- the git baseline (the behavior this branch adds) ----

def test_the_baseline_comes_from_the_commit_not_the_working_tree(tmp_path, monkeypatch):
    """Running the check twice must give the same delta.

    Reading the working-tree file made every run after the first compare against the run BEFORE it,
    so a second run inside one commit showed a delta of ~0 and the real change vanished. The
    baseline has to be the committed snapshot.
    """
    import subprocess
    repo = tmp_path / "repo"
    (repo / "docs" / "metrics").mkdir(parents=True)
    health = repo / "docs" / "metrics" / "repo-health.json"

    run = lambda *a: subprocess.run(a, cwd=repo, check=True, capture_output=True)
    run("git", "init", "-q")
    run("git", "config", "user.email", "t@t")
    run("git", "config", "user.name", "t")
    health.write_text(json.dumps({"flash": {"esp32": 100}}), encoding="utf-8")
    run("git", "add", "-A")
    run("git", "commit", "-qm", "baseline")

    # A later run overwrites the working tree; the COMMITTED value must still be the baseline.
    health.write_text(json.dumps({"flash": {"esp32": 999}}), encoding="utf-8")

    monkeypatch.setattr(repo_health, "ROOT", repo)
    monkeypatch.setattr(repo_health, "HEALTH_FILE", health)
    assert repo_health.load_previous() == {"flash": {"esp32": 100}}      # from the commit
    assert repo_health.load_working_tree() == {"flash": {"esp32": 999}}  # from disk


def test_the_baseline_falls_back_to_disk_when_git_cannot_answer(tmp_path, monkeypatch):
    """A fresh checkout with no commit yet must still produce a first snapshot rather than erroring."""
    repo = tmp_path / "norepo"
    (repo / "docs" / "metrics").mkdir(parents=True)
    health = repo / "docs" / "metrics" / "repo-health.json"
    health.write_text(json.dumps({"flash": {"esp32": 7}}), encoding="utf-8")

    monkeypatch.setattr(repo_health, "ROOT", repo)
    monkeypatch.setattr(repo_health, "HEALTH_FILE", health)
    assert repo_health.load_previous() == {"flash": {"esp32": 7}}
