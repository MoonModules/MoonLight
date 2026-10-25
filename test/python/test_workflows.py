"""Every CI job also runs locally: a workflow is a thin caller of the MoonDeck scripts its process steps name, so a GitHub outage, or a CI service that changes or stops, costs a runner rather than the gates."""

import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
import gen_moonwalk  # noqa: E402

WORKFLOWS = ROOT / ".github" / "workflows"
# A workflow left out names its reason, so the exemption reads as a debt rather than a rule.
EXEMPT = {"release.yml": "its build steps move into scripts after v7.0.0 ships; its publishing (the release upload, Pages, the container registry) is GitHub's own"}
SCRIPT = re.compile(r"^uv run (moondeck/\S+\.py)\b")
# What only GitHub's runner needs: a tool the runner lacks, and the values its cache and path read.
PLUMBING = re.compile(r'^(sudo apt-get install -y \S+|curl -sSL https://github\.com/errata-ai/vale/\S+ \| tar -xz -C "\$RUNNER_TEMP" vale'
                      r'|echo .+ >> "\$GITHUB_(OUTPUT|PATH)")$')


def jobs():
    """Each (workflow, job, its run lines) of the workflows the rule covers."""
    for path in sorted(WORKFLOWS.glob("*.yml")):
        if path.name in EXEMPT:
            continue
        for name, job in (yaml.safe_load(path.read_text(encoding="utf-8")).get("jobs") or {}).items():
            lines = [line.strip() for step in job.get("steps", []) if "run" in step
                     for line in step["run"].splitlines() if line.strip() and not line.strip().startswith("#")]
            yield path.name, name, lines


def process_scripts() -> set:
    """Every script a MoonWalk step runs."""
    model = gen_moonwalk.build()
    return {s["when"]["run"]["script"] for p in model["processes"].values() for s in p.get("steps", [])
            if isinstance(s["when"].get("run"), dict) and s["when"]["run"].get("script")}


def test_every_workflow_line_calls_a_script_or_feeds_githubs_own_plumbing():
    bad = [f"{wf} {job}: {line}" for wf, job, lines in jobs() for line in lines
           if not SCRIPT.match(line) and not PLUMBING.match(line)]
    assert bad == [], "move these into a MoonDeck script, so the job runs locally too:\n" + "\n".join(bad)


def test_every_job_that_runs_commands_calls_a_script_a_process_step_names():
    named = process_scripts()
    lost = [f"{wf} {job}" for wf, job, lines in jobs() if lines
            and not any(m and m.group(1) in named for m in map(SCRIPT.match, lines))]
    assert lost == [], "no MoonWalk step names a script these jobs run, so MoonDeck cannot run them:\n" + "\n".join(lost)


def test_an_exempt_workflow_names_its_reason():
    assert all(reason for reason in EXEMPT.values())
    assert all((WORKFLOWS / name).exists() for name in EXEMPT), "an exemption outlives its workflow"
