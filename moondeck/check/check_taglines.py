#!/usr/bin/env -S uv run --script
"""One fact, one wording: the platform list must read identically wherever it appears.

README.md and CLAUDE.md each open by saying what MoonLight runs on, for two readers: someone
deciding whether to try it and an agent about to change the code. The site's home embeds README,
so it carries README's sentence. Two audiences is a reason for two pages, not for two answers to
the same question: the six platforms are all targets, in one order and one spelling.

An include would make the second page mechanical too, and does not work here: GitHub renders
README.md and CLAUDE.md itself and would show the include directive. So the one wording is
enforced rather than mechanical, which is what this check is.

Deliberately NOT a general prose comparison. It pins one sentence, the one that states a fact
about the product rather than an opinion about it, and leaves each page its own voice around it.

    uv run moondeck/check/check_taglines.py

Exit codes: 0 all files agree - 1 a file is missing the sentence or states it differently.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import _findings  # noqa: E402

RULES = {"tagline out of step": {"reason": "A front page does not carry the canonical platform sentence.", "fix": "Say the platform list the same way in each front page, or change TAGLINE and update them together.", "standard": "docs/contributing/documentation-standards.md"}}

# The canonical sentence. Changing it here is the whole edit: the check then names every file
# that still carries the old wording.
TAGLINE = "One source tree drives ESP32, Teensy, Raspberry Pi, macOS, Windows and Linux."

# Every file that states it. A new front page belongs in this list on the day it is written; the site's home embeds README, so it carries README's sentence rather than its own.
FILES = ("README.md", "CLAUDE.md")


def main():
    missing = []
    for rel in FILES:
        path = ROOT / rel
        if not path.exists():
            missing.append((rel, "file not found"))
            continue
        if TAGLINE not in path.read_text(encoding="utf-8"):
            missing.append((rel, "does not carry the canonical platform sentence"))

    _findings.write("check_taglines", RULES, [{"rule": "tagline out of step", "message": why, "file": rel, "level": "error"} for rel, why in missing])

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
