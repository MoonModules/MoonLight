"""UTF-8 output for a MoonDeck script, whatever the console's code page.

A redirected Windows stdout takes the locale encoding, cp1252, so a script printing an arrow, a dash or a track's name through a pipe or a CI log fails on the print rather than on its work. An attached console has been UTF-8 since Python 3.6 (PEP 528), and stdio is UTF-8 on POSIX, so there the call keeps what is already right. errors=replace: a stray glyph prints as a question mark and the result stands. Python 3.15 makes UTF-8 mode the default (PEP 686), and with it this module goes.

stdlib only and cheap to import, as `_host.py` is, so a PEP-723 script imports it after adding moondeck/ to sys.path.
"""

import sys


def utf8_stdio() -> None:
    """Write stdout and stderr as UTF-8, a missing stream (a GUI launcher's) left as it is."""
    for stream in (sys.stdout, sys.stderr):
        if stream is not None:
            stream.reconfigure(encoding="utf-8", errors="replace")
