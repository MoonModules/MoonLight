#!/usr/bin/env -S uv run --script
"""One fact, one wording: the platform list must read identically wherever it appears.

README.md, docs/index.md and CLAUDE.md each open by saying what MoonLight runs on, for three
different readers: someone deciding whether to try it, someone already on the docs site, and an
agent about to change the code. Three audiences is a reason for three PAGES, not for three
different answers to the same question, and they had drifted into four orderings of the same six
platforms ("Windows, macOS or Linux" against "macOS, Windows, Linux", "RPi" against
"Raspberry Pi"), one of which also demoted five of the six to secondary. They are all targets.

An include would be the obvious fix and does not work here: MkDocs can pull a snippet into
docs/index.md, but GitHub renders README.md itself and would show the include directive. So the
one home is enforced rather than mechanical, which is what this check is.

Deliberately NOT a general prose comparison. It pins one sentence, the one that states a fact
about the product rather than an opinion about it, and leaves each page its own voice around it.

    uv run moondeck/check/check_taglines.py

Exit codes: 0 all files agree - 1 a file is missing the sentence or states it differently.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# The canonical sentence. Changing it here is the whole edit: the check then names every file
# that still carries the old wording.
TAGLINE = "One source tree drives ESP32, Teensy, Raspberry Pi, macOS, Windows and Linux."

# Every file that states it. A new front page belongs in this list on the day it is written.
FILES = ("README.md", "docs/index.md", "CLAUDE.md")


def main():
    missing = []
    for rel in FILES:
        path = ROOT / rel
        if not path.exists():
            missing.append((rel, "file not found"))
            continue
        if TAGLINE not in path.read_text(encoding="utf-8"):
            missing.append((rel, "does not carry the canonical platform sentence"))

    if missing:
        print(f"Tagline check: {len(missing)} file(s) out of step.\n")
        print(f"  canonical: {TAGLINE}\n")
        for rel, why in missing:
            print(f"  {rel}: {why}")
        print("\nEvery front page states what MoonLight runs on. Say it the same way in each, or")
        print("change TAGLINE in this script and update them together.")
        return 1

    print(f"Tagline check: {len(FILES)} front pages agree on the platform list.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
