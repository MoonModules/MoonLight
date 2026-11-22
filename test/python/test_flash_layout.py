"""flash_esp32.py's layout choice: the board's measured flash picks the layout among those the firmware lists.

Imports the real function (no ESP-IDF or board needed). Run: `uv run --with pytest pytest test/python -q`.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "build"))

from flash_esp32 import layout_for  # noqa: E402


def test_a_board_gets_the_layout_of_its_own_flash_size():
    assert layout_for([8, 16], 16)[0] == 16
    assert layout_for([8, 16], 8)[0] == 8
    assert layout_for([4, 8, 16], 4)[0] == 4


def test_more_flash_than_any_layout_takes_the_largest_that_fits():
    assert layout_for([8, 16], 32)[0] == 16


def test_a_board_smaller_than_every_layout_is_refused_and_told_to_take_a_lite_firmware():
    size, note = layout_for([8, 16], 4)
    assert size is None
    assert "-lite" in note


def test_unread_flash_takes_the_smallest_layout_which_boots_on_any_board_the_firmware_serves():
    assert layout_for([8, 16], None)[0] == 8
