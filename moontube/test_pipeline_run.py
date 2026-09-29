"""Every UI run file, performed through the interface and checked over REST.

These are the same files `moondeck/moontube/mtvideo.py` records the videos from, so
a failure here means the UI no longer does what a video claims it does. One test per
run: a clip that cannot be performed is a broken clip, whichever consumer is asking.

Parameterized over the directory rather than naming files, so a new run file is a new
test with nothing to wire up: dropping `add-a-driver.json` into this folder is the
whole change.
"""

import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "moondeck" / "moontube"))

import mtrun  # noqa: E402

# The run files sit BESIDE this test, the way test/scenarios/ holds the pipeline
# scenarios its runner globs for. Discovered rather than listed: a new clip dropped
# in here is a new test with nothing to wire up.
#
# CLIPS only. A project in projects/ is an edit list, not something performed against
# a device: it names clips and durations, so running it as a UI scenario would find
# no actions at all. A slide script in slides/ is the same case for the same reason:
# mtnarrate renders it from `slides`, nothing performs it, and three of them sat here
# being loaded as runs with zero steps and passing vacuously.
RUNS_DIR = Path(__file__).resolve().parent / "clips"
RUNS = sorted(RUNS_DIR.glob("*.json"))
PROJECTS_DIR = Path(__file__).resolve().parent / "projects"

# Derived from the engine, never restated. A hand-kept copy drifted twice while this
# was built, each time letting a typo through as a confusing mid-run failure.
KNOWN_ACTIONS = set(mtrun.ACTIONS)


@pytest.fixture
def clean_pipeline():
    """Report what a run left behind, without failing on it.

    A CLIP ENDS WHERE ITS SUBJECT ENDS. `reset_device.py` is what guarantees the next take
    opens clean, so the layouts clip closes on a scripted shape because that shape is the
    point, and tidying it away on camera would show the housekeeping instead of the subject.
    A leftover module is therefore not a defect, which is why this reports rather than
    asserts.

    A delete step that did not delete IS a defect, and it is checked where the answer is
    knowable: `clear_children` reads the container back and appends to `failures`. After the
    run only the final state is visible, which cannot tell a clear that failed from one the
    run deliberately undid by adding again.

    NOT a cleanup: nothing here writes over REST, because the whole point of these runs is
    that state changes go through the interface.
    """
    snapshot: dict = {}

    def check(target: str, run=None):
        if target not in snapshot:
            return                        # nothing was snapshotted for this device
        leftover = sorted(
            mtrun.all_names(mtrun.state(target).get("modules", [])) - snapshot[target])
        if leftover:
            print(f"  the run ended holding: {', '.join(leftover)}"
                  f"  (reset_device clears this before the next take)")

    def take(target: str):
        snapshot[target] = mtrun.all_names(mtrun.state(target).get("modules", []))

    check.take = take
    yield check


def test_there_are_runs_to_check():
    """A glob that silently matches nothing would make every other test vacuous."""
    assert RUNS, f"no run files found in {RUNS_DIR}"


@pytest.mark.parametrize("run_path", RUNS, ids=lambda p: p.stem)
def test_run_uses_known_actions(run_path):
    """Every step names an action the engine defines."""
    run = mtrun.load_run(run_path)
    unknown = {s.action for s in run.steps} - KNOWN_ACTIONS
    assert not unknown, f"{run_path.name} uses undefined actions: {unknown}"


@pytest.mark.parametrize("run_path", RUNS, ids=lambda p: p.stem)
def test_run_performs_through_the_ui(ui_for, clean_pipeline, run_path):
    """The run completes, and the device agrees with each step that checks itself."""
    run = mtrun.load_run(run_path)
    driver = ui_for(run)               # skips when the run's app is not running
    # AFTER the driver resolved its host: a `requires` run picks its own device, and
    # snapshotting before that read whichever machine the lane defaulted to.
    if not run.host:                   # only a device run owns a module tree
        clean_pipeline.take(driver.host)
        # The SAME preconditions the recorder applies, in the recorder's order: reset first, then
        # setup. A run file is documented as being both a UI test and a video source, which only
        # holds if both start the run from the same state.
        #
        # The RESET is what this lane was missing. mtvideo.py calls it before every take, so a clip
        # always records against a boot-state device, while this lane inherited whatever the last
        # take left behind: a run opening a Grid card failed because Layouts held a MoonLive layout
        # from an earlier clip. The failure looked like a broken step and was a dirty device.
        sys.path.insert(0, str(Path(__file__).resolve().parent.parent
                               / "moondeck" / "moontube"))
        from reset_device import apply_setup, reset
        assert reset(driver.host) == 0, (
            f"{run_path.name}: the device did not reach its boot state, so the run would be "
            f"judged against a state no run file asked for")
        if run.setup:
            applied = apply_setup(driver.host, run.setup)
            assert applied == len(run.setup), (
                f"{run_path.name}: {applied} of {len(run.setup)} setup control(s) applied")
    failures = driver.run_all(run)
    assert not failures, f"{run_path.name} failed:\n  " + "\n  ".join(failures)
    if not run.host:
        clean_pipeline(driver.host, run)


@pytest.mark.parametrize("project_path",
                         sorted(PROJECTS_DIR.glob("*.json")) or [None],
                         ids=lambda p: p.stem if p else "none")
def test_project_clips_exist(project_path):
    """Every clip a project names is one the tooling produces.

    A project referencing a missing clip fails at cut time with a file-not-found, long
    after the edit was written. This catches a rename the moment it happens, and keeps
    a project honest about depending only on clips a run can regenerate.
    """
    if project_path is None:
        pytest.skip("no projects to check")
    import json
    proj = json.loads(project_path.read_text())
    names = {p.stem for p in RUNS}
    missing = [c["clip"] for c in proj.get("clips", [])
               if "clip" in c and c["clip"] not in names]
    assert not missing, f"{project_path.name} names clips with no run file: {missing}"


def test_clips_are_runs_not_slide_scripts():
    """A file in clips/ is performed against a device, so it carries `steps`.

    A slide script carries `slides` and is rendered by mtnarrate instead. Dropped in here it is
    globbed as a run, loads with zero steps, and every run test passes without performing anything
    which is a green tick that means nothing, the one failure a test suite must not have.
    """
    wrong = []
    for p in RUNS:
        d = json.loads(p.read_text())
        if "slides" in d or not d.get("steps"):
            wrong.append(p.name)
    assert not wrong, (f"slide scripts in clips/: {wrong}. They belong in "
                       f"moontube/slides/, which mtnarrate reads.")


def test_every_action_is_documented():
    """moontube.md's tables name every action the engine defines.

    The format's documentation is what a run file is written from, so an action missing
    from it is invisible: that is how eight of them went undocumented while the engine
    grew. Checked rather than remembered.
    """
    doc = (ROOT / "moondeck" / "moontube" / "moontube.md").read_text()
    undocumented = sorted(a for a in mtrun.ACTIONS if f"`{a}`" not in doc)
    assert not undocumented, f"actions missing from moontube.md: {undocumented}"
