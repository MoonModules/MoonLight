"""Every shipped MoonLive script is valid C++.

MoonLive is documented as a subset of C++, and that claim is worth more than a description: it is
what lets someone read a script without learning a language, and what keeps the grammar from
drifting into a dialect one feature at a time. A claim nothing checks is a claim that decays.

So each script is wrapped in a generated prelude and handed to a real C++ compiler. The prelude
declares the vocabulary (the builtins, the system variables, the type aliases) and nothing else:
the SCRIPT's own text is compiled unmodified except for the one shape difference the language
deliberately has, applied mechanically here and listed in the language reference:

  - a class needs `public:` and a trailing semicolon in C++

Anything else a compiler rejects is a real divergence, and this test is where it surfaces.

The prelude is GENERATED from the engine's own builtin table rather than hand-written, so a builtin
added to the language cannot leave this test asserting against a stale vocabulary. Signatures are
`int` throughout: this is a syntax check (`-fsyntax-only`), never a run, so the shapes that matter
are the arity and the name.

Skips when no C++ compiler is on PATH, which is the case in a bare CI container. Run:
`uv run --with pytest pytest test/python/test_scripts_are_cpp.py -q`.
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
# Both vocabularies: a .mle compiles against the light table, a .mls against the service one, and
# this check asks only "is the script valid C++", which needs every name either could call.
BUILTINS = [
    ROOT / "src" / "core" / "moonlive" / "MoonLiveBuiltins_common.h",
    ROOT / "src" / "light" / "moonlive" / "MoonLiveBuiltins_light.h",
    ROOT / "src" / "core" / "moonlive" / "MoonLiveBuiltins_service.h",
]
SYSVARS = ("t", "width", "height", "depth", "xPos", "yPos", "zPos")

CXX = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
pytestmark = pytest.mark.skipif(CXX is None, reason="no C++ compiler on PATH")


def builtins():
    """Every builtin either vocabulary registers: (name, argc).

    Read from the registration tables themselves. A hand-kept list here would pass while the engine
    moved on, which is the exact failure this test exists to prevent one level down.
    """
    found = []
    for path in BUILTINS:
        text = path.read_text(encoding="utf-8")
        # Both registration shapes: `t.add({"name", n, ...})`, and a `Builtin` built first so a field past the aggregate's order can be set.
        found += re.findall(r'(?:t\.add\(\{|Builtin\s*\w*\s*\{)"([A-Za-z0-9_]+)",\s*(\d+)', text)
    assert found, "no builtins parsed: the registration shape changed"
    # A name can register twice (an overload by arity); keep the widest, since a call with fewer
    # arguments still matches a declaration with more only if defaults exist, which these lack.
    widest: dict[str, int] = {}
    for name, argc in found:
        widest[name] = max(widest.get(name, 0), int(argc))
    return sorted(widest.items())


def json_builtins():
    """The JSON builtins, typed: (name, return type, parameter types), from `addJsonBuiltins`.

    Each registration carries which arguments are documents (`jsonArgs`) and which are quoted text (`byStr`), so the C++ signature follows from the table as the compiler's own checks do.
    """
    found = []
    for path in BUILTINS:
        text = path.read_text(encoding="utf-8")
        for name, argc, jargs, sargs, jret in re.findall(
                r'add\("(\w+)",\s*(\d+),\s*&\w+,\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+),\s*(true|false)', text):
            j, st = int(jargs, 16), int(sargs, 16)
            params = ["json" if j >> i & 1 else "string" if st >> i & 1 else "int" for i in range(int(argc))]
            found.append((name, "json" if jret == "true" else "int", params))
    return found


def structs():
    """Every struct type the tables register: (name, field type, field names), from `addStruct`."""
    found = []
    for path in BUILTINS:
        text = path.read_text(encoding="utf-8")
        for name, ctype, fields in re.findall(r'addStruct\(\{"(\w+)",\s*CtrlType::(\w+),\s*\d+,\s*\{([^}]*)\}', text):
            found.append((name, "byte" if ctype == "Byte" else "int", re.findall(r'"(\w+)"', fields)))
    return found


def struct_returns():
    """Builtins answering a struct, by name: a `Builtin x{"name", ...}` whose `x.retStruct` is set."""
    found = {}
    for path in BUILTINS:
        text = path.read_text(encoding="utf-8")
        var_to_name = dict(re.findall(r'Builtin\s+(\w+)\s*\{"(\w+)"', text))
        for var, st in re.findall(r'(\w+)\.retStruct\s*=\s*"(\w+)"', text):
            if var in var_to_name:
                found[var_to_name[var]] = st
    return found


def prelude() -> str:
    lines = [
        "#include <cstdint>",
        "using byte = uint8_t;",
        "using fixed = int32_t;",
        "using string = const char*;",
        # A document's handle, which a string literal converts into as the compiler parses one.
        "struct json { json() = default; json(string) {} };",
    ]
    for v in SYSVARS:
        lines.append(f"inline int {v} = 0;")
    # An aggregate, so `RGB(1, 2, 3)` is C++20's parenthesized initialization and `RGB c;` a declaration.
    types = structs()
    assert types, "no struct types parsed: the addStruct shape changed"
    for name, ftype, fields in types:
        lines.append(f"struct {name} {{ {ftype} {', '.join(fields)}; }};")
    answers = struct_returns()
    # Builtins whose first argument is a NAME IN QUOTES are declared by hand below: the generated
    # int-only form cannot express a string, and emitting both makes the call ambiguous rather than
    # resolving it.
    byString = {"addControl", "addSelect", "setControl"}
    typed = json_builtins()
    assert typed, "no JSON builtins parsed: the addJsonBuiltins shape changed"
    for name, ret, params in typed:
        # `json(text)` is the type's own converting constructor in C++, which the struct above declares.
        if name != "json":
            lines.append(f"{ret} {name}({', '.join(params)});")
    byString |= {name for name, _, _ in typed}
    for name, argc in builtins():
        if name in byString:
            continue
        args = ", ".join(["int"] * argc)
        lines.append(f"{answers.get(name, 'int')} {name}({args});")
        # A struct argument fills as many positions as it has fields, so each run of positions a struct can fill gets its overload.
        for st, _, fields in types:
            for k in range(argc - len(fields) + 1):
                parts = ["int"] * k + [st] + ["int"] * (argc - k - len(fields))
                lines.append(f"{answers.get(name, 'int')} {name}({', '.join(parts)});")
    # toFixed/toInt are KEYWORDS, not builtins: the compiler recognizes them inline so each costs
    # one shift instruction rather than a host call (MoonLiveCompiler.cpp), so the registration
    # table above does not carry them.
    lines.append("int toFixed(int); int toInt(int);")
    # addControl binds a member BY REFERENCE and takes its label as a string, so the generated
    # int-only declaration cannot express it. Both member widths, spelled out.
    # setControl names its target in quotes, so the int-only declaration cannot express it either.
    # Same exception as addControl below, for the same reason.
    lines.append("int setControl(string, int);")
    lines.append("void addControl(string, int&, int, int);")
    lines.append("void addControl(string, byte&, int, int);")
    lines.append("void addControl(string, bool&, int, int);")   # one arity for every type, as the builtin table registers it
    lines.append("void addSelect(string, byte&, string);")      # its options are one string, names split by '|'
    return "\n".join(lines) + "\n"


def as_cpp(script: str) -> str:
    """One script, as the C++ it claims to be."""
    src = re.sub(r"^(class \w+ \{)", r"\1\npublic:", script, count=1, flags=re.M)
    return src.rstrip() + ";\n"


def scripts():
    out = sorted((ROOT / "moonlive").rglob("*.ml*"))
    assert out, "no scripts found: this test would pass without checking anything"
    return out


@pytest.mark.parametrize("script", scripts(), ids=lambda p: p.name)
def test_script_compiles_as_cpp(script, tmp_path):
    cpp = tmp_path / "probe.cpp"
    (tmp_path / "prelude.h").write_text(prelude(), encoding="utf-8")
    cpp.write_text('#include "prelude.h"\n' + as_cpp(script.read_text(encoding="utf-8")),
                   encoding="utf-8")
    r = subprocess.run([CXX, "-std=c++20", "-fsyntax-only", str(cpp)],
                       capture_output=True, text=True, cwd=tmp_path)
    assert r.returncode == 0, f"{script.name} is not valid C++:\n{r.stderr}"


def test_the_check_can_fail(tmp_path):
    """A control: a script with a real C++ error must be rejected.

    Without this the suite above is indistinguishable from one where the compiler never ran, which
    is the failure mode a green test cannot show on its own.
    """
    (tmp_path / "prelude.h").write_text(prelude(), encoding="utf-8")
    cpp = tmp_path / "bad.cpp"
    cpp.write_text('#include "prelude.h"\nclass T {\npublic:\n  void tick() { nosuchcall(1); }\n};\n',
                   encoding="utf-8")
    r = subprocess.run([CXX, "-std=c++20", "-fsyntax-only", str(cpp)],
                       capture_output=True, text=True, cwd=tmp_path)
    assert r.returncode != 0, "the compiler accepted an undeclared call: this check proves nothing"
