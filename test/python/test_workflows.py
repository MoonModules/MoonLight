"""Every CI job also runs locally: a workflow is a thin caller of the MoonDeck scripts its process steps name, so a GitHub outage, or a CI service that changes or stops, costs a runner rather than the gates."""

import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
import gen_moonwalk  # noqa: E402

WORKFLOWS = ROOT / ".github" / "workflows"
# A step left out names its reason, so the exemption reads as a debt rather than a rule: what only GitHub's own services or runner can do. Keyed by (workflow, job, step name).
EXEMPT = {
    ("release.yml", "build-windows-x64", "Install NSIS (the Windows installer compiler)"): "runner plumbing: choco installs the tool this runner image lacks",
    ("release.yml", "build-linux-x64", "Install libcurl (MoonCloud, only used once consent is given in the app)"): "runner plumbing: apt installs the library this runner image lacks",
    ("release.yml", "build-linux-arm64", "Install libcurl (MoonCloud, only used once consent is given in the app)"): "runner plumbing: apt installs the library this runner image lacks",
    ("release.yml", "release", "Re-create the `latest` release fresh"): "publishing: the `latest` tag force-push and the release deletion are GitHub's own",
    ("release.yml", "deploy-pages", "Stage cumulative release content"): "publishing: it downloads the kept releases' assets through GitHub's release API",
    ("release.yml", "restage-pages-for-tag", "Dispatch a main-context Pages deploy for this tag"): "publishing: it dispatches a GitHub workflow run",
}
# One script call and its arguments, nothing chained after it; a `${{ }}` expression is GitHub's, evaluated before the shell sees the line.
SCRIPT = re.compile(r"^uv run (moondeck/\S+\.py)(?:\s+[^;&|`]*)?$")
EXPRESSION = re.compile(r"\$\{\{.*?\}\}")
# What only GitHub's runner needs: a tool the runner lacks, and the values its cache and path read.
PLUMBING = re.compile(r'^(sudo apt-get install -y \S+|curl -sSL https://github\.com/errata-ai/vale/\S+ \| tar -xz -C "\$RUNNER_TEMP" vale'
                      r'|echo .+ >> "\$GITHUB_(OUTPUT|PATH)")$')


def jobs():
    """Each (workflow, job, its run lines) of the workflows the rule covers."""
    for path in sorted(WORKFLOWS.glob("*.yml")):
        for name, job in (yaml.safe_load(path.read_text(encoding="utf-8")).get("jobs") or {}).items():
            lines = [EXPRESSION.sub("EXPR", line.strip()) for step in job.get("steps", [])
                     if "run" in step and (path.name, name, step.get("name")) not in EXEMPT
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


def test_an_exempt_step_names_its_reason_and_still_exists():
    assert all(reason for reason in EXEMPT.values())
    steps = {(path.name, job_name, step.get("name")) for path in WORKFLOWS.glob("*.yml")
             for job_name, job in (yaml.safe_load(path.read_text(encoding="utf-8")).get("jobs") or {}).items()
             for step in job.get("steps", []) if "run" in step}
    assert set(EXEMPT) <= steps, "an exemption outlives its step"


def test_a_script_call_chained_to_another_command_is_not_a_script_call():
    assert SCRIPT.match("uv run moondeck/test/test_host.py --python --junit-dir results")
    assert SCRIPT.match(EXPRESSION.sub("EXPR", "uv run moondeck/check/check_sanitizers.py --kind ${{ matrix.kind }} ${{ a && 'b' || '' }}"))
    assert not SCRIPT.match("uv run moondeck/test/test_host.py && rm -rf results")
    assert not SCRIPT.match("uv run moondeck/test/test_host.py; echo done")
