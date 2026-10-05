#!/usr/bin/env python3
"""Split a firmware's flash change into what was added and what was saved, symbol by symbol.

The image size says how much a commit cost; this says what for.
A commit that adds a feature and removes duplication shows one net number, which hides both halves: the split separates them, the way bloaty's diff mode does, from the symbol tables of the ELF before and after.

- **new**: symbols that did not exist, which is new functionality.
- **removed**: symbols that are gone, which is deleted or merged code.
- **grown** and **shrunk**: the same symbol, larger or smaller, which is a change to existing code.
- **other**: what no symbol owns, such as merged strings and alignment, so the parts sum to the net.

The split is mechanical: a rename reads as removed plus new, and inlining moves bytes between symbols.
The net is exact, the split is a strong signal, and the largest moves are listed so a misreading shows.

The baseline is the measurement taken at the previous commit, kept beside each build in `build/`.
A run at a new HEAD rotates the last measurement into the baseline, so every run inside one commit compares against the same state, the one the last commit recorded.
"""

import json
import re
import subprocess
from pathlib import Path

SNAPSHOT = "flash-symbols.json"
BASELINE = "flash-symbols.base.json"
TOP = 5   # the largest moves listed per firmware, enough to see what drove the split

# The ELF machine field, which names the toolchain whose nm reads it: one nm per architecture.
_NM_BY_MACHINE = {94: "xtensa-esp-elf-nm", 243: "riscv32-esp-elf-nm"}

# nm --print-size lines: address, size, type, name; the name is demangled and may hold spaces.
_NM_LINE = re.compile(r"^[0-9a-fA-F]+ ([0-9a-fA-F]+) (\w) (.+)$")


def parse_nm(text: str) -> dict:
    """Bytes per symbol name from `nm --print-size` output, leaving out what the image does not carry (bss)."""
    sizes = {}
    for line in text.splitlines():
        m = _NM_LINE.match(line.strip())
        if not m or m.group(2) in "bB":
            continue
        # Two file-local statics may share a name; their bytes add up under it rather than one hiding the other.
        sizes[m.group(3)] = sizes.get(m.group(3), 0) + int(m.group(1), 16)
    return sizes


def split(base: dict, now: dict, net: int) -> dict:
    """The net change in four kinds plus the remainder no symbol owns, with the largest moves."""
    new = sum(v for k, v in now.items() if k not in base)
    removed = -sum(v for k, v in base.items() if k not in now)
    moves = {k: now.get(k, 0) - base.get(k, 0) for k in set(base) | set(now)}
    grown = sum(d for k, d in moves.items() if d > 0 and k in base and k in now)
    shrunk = sum(d for k, d in moves.items() if d < 0 and k in base and k in now)
    top = sorted((kv for kv in moves.items() if kv[1]), key=lambda kv: (-abs(kv[1]), kv[0]))[:TOP]
    return {"net": net, "new": new, "removed": removed, "grown": grown, "shrunk": shrunk,
            "other": net - (new + removed + grown + shrunk), "top": [[k, d] for k, d in top]}


def _machine(elf: Path) -> int:
    """The ELF header's e_machine, little-endian as every ESP32 image is."""
    with open(elf, "rb") as f:
        head = f.read(20)
    return int.from_bytes(head[18:20], "little") if head[:4] == b"\x7fELF" else 0


def symbols(elf: Path, env: dict) -> dict | None:
    """The image's symbol sizes, or None when the target's nm is not installed."""
    tool = _NM_BY_MACHINE.get(_machine(elf))
    if not tool:
        return None
    try:
        out = subprocess.run([tool, "--print-size", "--demangle", "--defined-only", str(elf)],
                             capture_output=True, text=True, env=env, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    return parse_nm(out.stdout) if out.returncode == 0 else None


def _load(path: Path) -> dict | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def measure(build_dir: Path, elf: Path, image_bytes: int, head: str, env: dict) -> dict | None:
    """This firmware's split against the previous commit's measurement, or None without a baseline or a toolchain."""
    now = symbols(elf, env)
    if now is None:
        return None
    snap, base_path = build_dir / SNAPSHOT, build_dir / BASELINE
    last = _load(snap)
    if last and last.get("head") != head:
        snap.replace(base_path)   # a commit happened since: that measurement is the baseline now
    snap.write_text(json.dumps({"head": head, "image": image_bytes, "symbols": now}), encoding="utf-8")
    base = _load(base_path)
    if not base or "symbols" not in base:
        return None
    result = split(base["symbols"], now, image_bytes - base.get("image", image_bytes))
    result["since"] = base.get("head", "?")
    return result


def _kb(n: int) -> str:
    return f"{n / 1024:+.1f}"


def line(splits: dict) -> str:
    """One line for a commit message: per firmware, the net and its parts in KB."""
    if not splits:
        return "Flash split: unmeasured (no firmware built at this commit and the last one)"
    parts = [f"{fw} {_kb(s['net'])} KB = {_kb(s['new'])} new, {_kb(s['removed'])} removed, "
             f"{_kb(s['grown'])} grown, {_kb(s['shrunk'])} shrunk, {_kb(s['other'])} other"
             for fw, s in sorted(splits.items())]
    return "Flash split: " + " | ".join(parts)
