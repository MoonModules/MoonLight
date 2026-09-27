"""Fixtures for the UI tests: a browser pointed at a running MoonLight.

These are the only tests that need a device. Everything in test/python is pure and
runs anywhere; a UI test drives a real interface against real firmware, so it SKIPS
rather than fails when nothing is listening. That is the same shape the JS lane uses
for a missing node: a bench without a running desktop build is a normal setup, and
"this does not apply here" is what skip means.

Run:  uv run moondeck/test/test_host.py --ui
"""

import os
import sys
from pathlib import Path

import pytest
import requests

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / "uiscenario"))

import uirun  # noqa: E402  (after the path insert)

HOST = os.environ.get("PROJECTMM_HOST", "localhost:8080")


def _alive(host: str, path: str = "/api/state") -> bool:
    """Is the thing we need answering there?

    The probe is the caller's, not a list to fall through: a DEVICE must answer
    `/api/state`, and accepting `/` as a fallback let any web server pass the check.
    Every test then failed on the first step instead of skipping with a clear reason.
    """
    try:
        return requests.get(f"http://{host}{path}", timeout=3).ok
    except requests.RequestException:
        return False


@pytest.fixture(scope="session")
def host() -> str:
    if not _alive(HOST):
        pytest.skip(f"no MoonLight answering on {HOST} "
                    f"(start one: uv run moondeck/run/run_desktop.py)")
    return HOST


@pytest.fixture(scope="session")
def _browser_available(playwright):
    """Skip the lane, once, when Playwright's browser is not installed.

    Without this the first test errors deep inside a launch, and the fix (one install
    command) is nowhere in the output. A bench that has never run these is a normal
    bench, which is what SKIP means.

    A DEPENDENCY of ui_for rather than autouse: the checks that only read run files
    (the action vocabulary, the documentation, a project's clip names) need no browser,
    and autouse skipped them on a machine that has never installed chromium.
    """
    try:
        browser = playwright.chromium.launch()
        browser.close()
    except Exception as exc:
        pytest.skip(f"Playwright's chromium is not installed ({exc}).\n"
                    f"uv run --with playwright playwright install chromium")


@pytest.fixture(scope="session", autouse=True)
def _test_ids(playwright):
    """Point get_by_test_id at the attributes app.js already finds its own DOM by.

    The UI carries no data-testid. These three are a real contract rather than
    styling hooks (updateModuleControls queries them), so they cannot drift without
    breaking the app, which is exactly what a test id has to promise.
    """
    playwright.selectors.set_test_id_attribute(uirun.TEST_ID_ATTRS)


@pytest.fixture
def ui_for(page, host, _browser_available):
    """A Driver for whichever app a run names.

    Most runs drive the device UI. One drives the web installer on its preview port,
    which is a different app on a different port and is not always running: that run
    SKIPS rather than fails, the same way the whole lane skips without a device. A
    missing optional preview server is a normal bench, not a defect.
    """
    def make(run):
        if run.requires:
            found = uirun.device_for(run.requires)
            if not found:
                pytest.skip(f"no bench device with {run.requires!r} is answering")
            target = found
        else:
            target = run.host or host
        if target != host and not _alive(target, "/"):
            pytest.skip(f"{target} is not answering "
                        f"(uv run moondeck/run/preview_installer.py)")
        driver = uirun.Driver(page, target)
        # Cards unless it is another APP: a `requires` run resolves to a real device
        # (target != host) that renders cards like any other, so keying on the address
        # made a hardware run skip the wait it needs.
        driver.open_app(cards=not run.host)
        return driver
    return make
