"""wait_for counts a state it has already seen, so a brief one is not missed.

The installer's progress element marches Detected -> Erasing -> Writing on the device's
clock. The erase on an S3 lasts about twelve seconds, which is over before the step that
waits for it begins, so a wait reading only the present fails a recording that in fact
went perfectly. These pin the record-as-you-go behaviour that makes the wait honest.

No browser and no device: the driver is pointed at a page object that marches through the
states on a clock, which is what lets a timing rule be tested at all.
"""


import pytest
from uirun import Driver


class _Element:
    """One element whose text is whatever the timeline says it is right now."""

    def __init__(self, page):
        self._page = page

    def text_content(self):
        return self._page.text_now


class _Page:
    """A page whose one element marches through states on a clock the test owns.

    The clock advances only when the page is asked to wait, so a run takes no real time and
    lands on the same states every time. Wall-clock sleeps make a timing test slow and flaky
    at once, which is the pair of properties a test of timing can least afford.
    """

    def __init__(self, timeline):
        self._timeline = timeline
        self._now = 0.0

    def now(self):
        return self._now

    @property
    def text_now(self):
        current = self._timeline[0][1]
        for at, text in self._timeline:
            if self._now >= at:
                current = text
        return current

    def query_selector(self, _selector):
        return _Element(self)

    def wait_for_timeout(self, ms):
        self._now += ms / 1000.0


def _driver(timeline, paced=True):
    driver = Driver(_Page(timeline), host="test")
    driver.paced = paced
    return driver


SELECTOR = "#connecting-detail"


def test_a_state_that_passed_during_an_earlier_dwell_still_satisfies_its_wait():
    """The erase ends before its own step starts, and the wait passes on the record."""
    driver = _driver([(0.0, "Detected chip"), (0.2, "Erasing flash"), (0.6, "Writing at 0x0")])
    assert driver.wait_for(SELECTOR, "Detected", timeout=5)
    driver._settle(1.0)                       # the gap that the state lives and dies inside
    assert driver.wait_for(SELECTOR, "Erasing", timeout=5)
    assert driver.wait_for(SELECTOR, "Writing", timeout=5)


def test_a_state_the_page_never_shows_still_times_out():
    """The control: remembering what went by never invents a state that never happened."""
    driver = _driver([(0.0, "Detected chip"), (0.2, "Writing at 0x0")])
    assert driver.wait_for(SELECTOR, "Detected", timeout=5)
    with pytest.raises(TimeoutError) as caught:
        driver.wait_for(SELECTOR, "Erasing", timeout=1)
    assert "Erasing" in str(caught.value)
    assert "detected chip" in str(caught.value)   # the message names what it did see


def test_the_states_it_saw_are_reported_when_a_wait_times_out():
    """A failed take says what the page showed, so the next run needs no guesswork."""
    driver = _driver([(0.0, "Connecting"), (0.3, "Detected chip")])
    with pytest.raises(TimeoutError) as caught:
        driver.wait_for(SELECTOR, "Verifying", timeout=1)
    message = str(caught.value)
    assert "connecting" in message and "detected chip" in message


def test_an_unpaced_run_does_not_dwell_once_something_is_watched():
    """Sampling is correctness and pacing is for the camera, so one never drags in the other.

    A test run has nobody watching, so every dwell is skipped. Tying the sampling to the dwell
    made each later step sleep its full hold for the rest of the run, turning a fast test pass
    into a wall-clock one, and no test would have noticed because the run still passed.
    """
    driver = _driver([(0.0, "Detected chip")], paced=False)
    assert driver.wait_for(SELECTOR, "Detected", timeout=5)
    before = driver.page.now()
    driver._settle(2.0)                       # a hold a paced run would honour
    assert driver.page.now() == before        # the clock only moves when the page is asked to wait


def test_an_empty_element_does_not_satisfy_a_wait_that_names_no_text():
    """The installer blanks a field before filling it, and blank is the state before the event."""
    driver = _driver([(0.0, ""), (0.4, "MoonModules")])
    assert driver.wait_for(SELECTOR, timeout=5)
    assert "moonmodules" in driver._seen_text[SELECTOR]
