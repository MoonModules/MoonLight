#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# ///
"""What a firmware image spends its bytes on, per symbol, and what changed since the last look.

    uv run moondeck/report/size_symbols.py --firmware esp32        # build/esp32-esp32/MoonLight.elf
    uv run moondeck/report/size_symbols.py --firmware teensy41     # build/teensy41/MoonLight.elf
    uv run moondeck/report/size_symbols.py --firmware esp32 --top 50

It lists the largest symbols, code and data apart, and keeps the table beside the image
(`size-symbols.json`). The next run compares against it, so building, looking, changing and
building again shows which symbols grew and by how much: the question a commit's size line
raises and cannot answer.

The `nm` that reads the image is the one beside the compiler that built it, as that build
directory's CMake cache records it, so one rule covers every ESP32 toolchain and Teensy's Arm
toolchain. A firmware image only: a macOS binary stores no symbol sizes. Stdlib only.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SNAPSHOT = "size-symbols.json"

# nm's type letter, lower case for a local symbol: what the bytes are.
KINDS = {"t": "code", "w": "code", "r": "const", "d": "data", "b": "bss", "v": "data"}


def build_dir(firmware: str) -> Path:
    """The build directory a firmware's build script writes."""
    if firmware.startswith("teensy"):
        return ROOT / "build" / firmware
    return ROOT / "build" / f"esp32-{firmware}"


def nm_for(bdir: Path) -> str:
    """The nm beside the compiler the CMake cache names, or the host's nm."""
    cache = bdir / "CMakeCache.txt"
    if cache.exists():
        m = re.search(r"^CMAKE_CXX_COMPILER:\w+=(.+)$", cache.read_text(errors="replace"), re.M)
        if m:
            compiler = Path(m.group(1).strip())
            nm = compiler.with_name(re.sub(r"(g\+\+|c\+\+|clang\+\+)(\.exe)?$", r"nm\2", compiler.name))
            if nm != compiler and nm.exists():
                return str(nm)
    return "nm"


def parse_nm(text: str) -> dict:
    """{name: {"kind", "size"}} from `nm -S -C` lines; a name defined in several files sums."""
    out = {}
    for line in text.splitlines():
        parts = line.split(None, 3)
        if len(parts) != 4:
            continue   # an undefined symbol, which has no size
        _, size, letter, name = parts
        kind = KINDS.get(letter.lower())
        if not kind:
            continue
        try:
            n = int(size, 16)
        except ValueError:
            continue
        if n == 0:
            continue
        entry = out.setdefault(name, {"kind": kind, "size": 0})
        entry["size"] += n
    return out


def totals(symbols: dict) -> dict:
    """Bytes per kind."""
    t = {}
    for s in symbols.values():
        t[s["kind"]] = t.get(s["kind"], 0) + s["size"]
    return t


def diff(before: dict, after: dict) -> list:
    """(delta, name, old, new) for every symbol whose size changed, appeared or went, largest change first."""
    rows = []
    for name in before.keys() | after.keys():
        old = before.get(name, {}).get("size", 0)
        new = after.get(name, {}).get("size", 0)
        if old != new:
            rows.append((new - old, name, old, new))
    rows.sort(key=lambda r: (-abs(r[0]), r[1]))
    return rows


def report(symbols: dict, top: int, before: dict | None) -> str:
    lines = []
    t = totals(symbols)
    lines.append("Bytes by kind: " + ", ".join(f"{k} {t[k]:,}" for k in sorted(t, key=lambda k: -t[k])))
    lines.append("")
    lines.append(f"Largest {top} symbols:")
    for name, s in sorted(symbols.items(), key=lambda kv: -kv[1]["size"])[:top]:
        lines.append(f"  {s['size']:>8,}  {s['kind']:<5}  {name}")
    if before is not None:
        rows = diff(before, symbols)
        net = sum(r[0] for r in rows)
        lines.append("")
        lines.append(f"Since the last run: {net:+,} bytes over {len(rows)} symbols")
        for delta, name, old, new in rows[:top]:
            what = "new" if old == 0 else "gone" if new == 0 else f"{old:,} -> {new:,}"
            lines.append(f"  {delta:>+8,}  {what:<18}  {name}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--firmware", required=True, help="an ESP32 firmware (esp32, esp32s3-n16r8, …) or teensy41")
    ap.add_argument("--top", type=int, default=30, help="how many symbols and changes to list (default 30)")
    args = ap.parse_args()
    bdir = build_dir(args.firmware)
    image = bdir / "MoonLight.elf"   # the linked image, which keeps the symbol table the flashed binary drops
    if not image.exists():
        print(f"No image at {image}: build {args.firmware} first.")
        return 1
    r = subprocess.run([nm_for(bdir), "-S", "-C", str(image)], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr.strip() or f"nm failed on {image}")
        return 1
    symbols = parse_nm(r.stdout)
    snapshot = bdir / SNAPSHOT
    before = json.loads(snapshot.read_text()) if snapshot.exists() else None
    print(f"{image.relative_to(ROOT)}\n")
    print(report(symbols, args.top, before))
    snapshot.write_text(json.dumps(symbols))
    return 0


if __name__ == "__main__":
    sys.exit(main())
