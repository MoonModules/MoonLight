"""The Teensy 4.1's flash map has three readers that must agree: the platform, which places the program and the settings; the web installer's writer, which erases the settings by offset; and the build, which reports the program's room.

The platform is the one home (platform_teensy.cpp); the other two restate it in their own language, so this test reads all three and fails on drift.
An installer erase aimed at the wrong offset would silently leave the settings in place, or zero part of the program.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "build"))
import build_teensy  # noqa: E402


def _cpp_constant(name):
    text = (ROOT / "src" / "platform" / "teensy" / "platform_teensy.cpp").read_text(encoding="utf-8")
    m = re.search(rf"constexpr uint32_t {name} = ([0-9 *]+);", text)
    assert m, f"{name} not found in platform_teensy.cpp"
    return eval(m.group(1))   # a product of integer literals only, matched above


def _js_constant(name, known):
    text = (ROOT / "mooninstaller" / "teensy-writer.js").read_text(encoding="utf-8")
    m = re.search(rf"export const {name} = ([A-Z0-9_ *+-]+);", text)
    assert m, f"{name} not found in teensy-writer.js"
    return eval(m.group(1), {}, dict(known))   # literals and the constants named in `known`


FLASH = _cpp_constant("kFlashBytes")
RESERVED = _cpp_constant("kReservedBytes")
SETTINGS = _cpp_constant("kFsBytes")


def test_the_installer_writes_exactly_the_flash_below_the_reserved_top():
    assert _js_constant("TEENSY41_CODE_BYTES", {}) == FLASH - RESERVED


def test_the_installer_erases_the_settings_where_the_platform_keeps_them():
    code = _js_constant("TEENSY41_CODE_BYTES", {})
    assert _js_constant("TEENSY41_SETTINGS_OFFSET", {"TEENSY41_CODE_BYTES": code}) == FLASH - RESERVED - SETTINGS


def test_the_build_reports_the_program_room_the_platform_leaves():
    assert build_teensy.PROGRAM_FLASH_BYTES == FLASH - RESERVED - SETTINGS
