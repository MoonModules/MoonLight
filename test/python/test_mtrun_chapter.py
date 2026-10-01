"""A chapter card lasts its `seconds` in the finished clip, once.

The card opens nearly every clip, and it held twice: the screencast holds it for its duration, and the driver then waited the same length again.
Every clip opened on several seconds of silence, nine in the longest.
These pin one hold, scaled by the clip's speed.

No browser: the screencast is a recorder of what it was asked, and the page's clock advances only when something waits on it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "moontube"))
from mtrun import Driver  # noqa: E402


class _Page:
    def __init__(self):
        self.waited = 0.0

    def wait_for_timeout(self, ms):
        self.waited += ms / 1000.0


class _Screencast:
    def __init__(self):
        self.durations = []

    def show_chapter(self, title, description=None, duration=None):
        self.durations.append(duration)


def _driver(speed):
    page, cast = _Page(), _Screencast()
    driver = Driver(page, host="test", screencast=cast)
    driver.speed = speed
    return driver, page, cast


def test_a_chapter_card_is_held_once_by_the_screencast():
    driver, page, cast = _driver(speed=1.0)
    driver.chapter("Your device", seconds=5.0)
    assert cast.durations == [5000]
    assert page.waited == 0.0


def test_a_chapter_card_lasts_its_seconds_once_the_clip_is_sped_up():
    driver, _, cast = _driver(speed=1.4)
    driver.chapter("Tested the way you use it", seconds=10.0)
    assert cast.durations == [14000]
