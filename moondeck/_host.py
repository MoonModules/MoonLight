"""The name this host reports itself as in the metrics, shared across check/ and scenario/.

One home for the spelling, because a desktop measurement is only comparable against the same OS.
A tick from macOS, Windows and CI Linux differs by more than the code between two commits does, so
a metric keyed plainly `desktop` records whichever machine ran it last rather than what changed.
The scenario observations carry the same key, and so do repo-health's `flash`, `measured` and
`perf` entries.

The OS name itself comes from `build_desktop.host_build_dir()` rather than a second map here: it
already answers "what is this host called" for the build directory, and two maps would disagree
the first time a platform is added to one of them. That module is stdlib-only and does nothing at
import time, so this stays cheap enough for a PEP-723 script to import after adding moondeck/ to
sys.path, the same shape as `_moondeck_config.py` and `scenario/_net_probe.py`.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "build"))
from build_desktop import host_build_dir  # noqa: E402


def desktop_target() -> str:
    """This host's metrics key, such as `desktop-macos`.

    An unnamed OS gets its own name (`desktop-freebsd`) rather than a shared `desktop-unknown`,
    which keeps a measurement from a platform nobody has added yet in its own row instead of
    overwriting a named host's numbers, and says which platform it came from.
    """
    return "desktop-" + host_build_dir().rsplit("/", 1)[1]
