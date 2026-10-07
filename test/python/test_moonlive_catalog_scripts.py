"""MoonLive's catalog_scripts.py reads what a shipped script declares about itself.

The catalog carries each factory script's `dimensions()` and `tags()` so the picker can show a row
before the script is downloaded. That makes the generator a SECOND reader of the MoonLive language,
and the risk is the two drifting: the catalog says a script is 2D while the compiled script renders
as 1D, and nothing reports it.

The generator is bounded to exactly the two forms the scripts write, and it FAILS THE BUILD on
anything else rather than recording a silent default. These tests pin both halves: that a normal
declaration is read correctly, and that a form the regex cannot read stops the build.

Run: `uv run --with pytest pytest test/python -q`.
"""

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src" / "core" / "moonlive"))

from catalog_scripts import declared  # noqa: E402


def write(tmp_path, body: str) -> Path:
    p = tmp_path / "probe.mle"
    p.write_text(body, encoding="utf-8")
    return p


def test_reads_a_declared_dimension_and_tags(tmp_path):
    p = write(tmp_path, "class T {\n"
                        "  int dimensions() { return 3; }\n"
                        '  string tags() { return "🌀"; }\n'
                        "  void tick() { fill(0, 0, 0); }\n"
                        "}\n")
    assert declared(p) == (3, "🌀")


def test_a_script_that_declares_nothing_reads_as_unsaid(tmp_path):
    # 0 rather than 2: the DEVICE owns what a silent script defaults to, and baking today's default
    # into the catalog would freeze it into every generated file.
    p = write(tmp_path, "class T {\n  void tick() { fill(0, 0, 0); }\n}\n")
    assert declared(p) == (0, "")


def test_each_declaration_is_read_independently(tmp_path):
    p = write(tmp_path, "class T {\n"
                        "  int dimensions() { return 1; }\n"
                        "  void tick() { fill(0, 0, 0); }\n"
                        "}\n")
    assert declared(p) == (1, "")


@pytest.mark.parametrize("body", [
    # Legal to the real compiler, unreadable to this regex: a computed value.
    "  int dimensions() { return 1 + 1; }\n",
    # A dimension outside the three the layer knows.
    "  int dimensions() { return 4; }\n",
])
def test_a_dimensions_the_generator_cannot_read_fails_the_build(tmp_path, body):
    p = write(tmp_path, "class T {\n" + body + "  void tick() { fill(0, 0, 0); }\n}\n")
    with pytest.raises(ValueError):
        declared(p)


def test_a_tags_the_generator_cannot_read_fails_the_build(tmp_path):
    # A member holding the value: the compiler accepts it, the regex cannot follow it, and a silent
    # "" would put an unmarked row in the picker while the device shows the real emoji.
    p = write(tmp_path, "class T {\n"
                        "  string tags() { return someMember; }\n"
                        "  void tick() { fill(0, 0, 0); }\n"
                        "}\n")
    with pytest.raises(ValueError):
        declared(p)


def test_every_shipped_script_is_readable():
    """The generator can read every script that actually ships.

    The build already fails on an unreadable one, so this is the same guarantee stated as a test:
    it names the offending file directly instead of surfacing as a build error someone has to trace
    back to a script.
    """
    scripts = sorted((ROOT / "moonlive").rglob("*.ml*"))
    assert scripts, "no scripts found: this test would pass without checking anything"
    for s in scripts:
        dim, _tags = declared(s)          # raises if the declaration is unreadable
        assert dim in (0, 1, 2, 3), f"{s}: dimension {dim}"
