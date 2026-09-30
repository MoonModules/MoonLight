#!/usr/bin/env python3
"""Where a module's bytes land on the device: flash, RAM, or strings.

The other size checks answer "how big is the binary". This one answers **which module made it
that big, and which memory it spends** — the question a footprint budget actually needs, because
those bytes are not interchangeable. Flash is ~1.5 MB and cheap; internal RAM is ~320 KB shared
with WiFi, the HTTP stack and every task stack, so a byte of `.bss` costs far more than a byte of
`.text`. A report that adds them together hides the only distinction that matters.

Reads the ESP32 **ELF**, not the source: the linker's own accounting is the ground truth, and it
already resolved every inline, template instantiation and dead-stripped symbol. Attribution comes
from DWARF (`nm --line-numbers`), so a symbol is credited to the source FILE that defines it —
including free functions and file-statics that no name-parsing heuristic could place.

**An unused module should cost nothing.** The STATIC column is that check: `.bss` + `.data` are
held from boot whether or not the module is ever enabled, so anything there is a standing tax.
Code and rodata are only read when called, so they cost flash but not headroom.

Needs a built firmware — `uv run moondeck/build/build_esp32.py --firmware <fw>` first. Exits 2
when the ELF is missing rather than reporting an empty table (an absent measurement is not a
zero).

Usage:
  uv run moondeck/check/check_footprint.py                    # every module, biggest first
  uv run moondeck/check/check_footprint.py --module HueDriver
  uv run moondeck/check/check_footprint.py --firmware esp32   # another target
"""

import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_clang_query  # noqa: E402  — the module→files resolver, one owner

DEFAULT_FW = "esp32s3-n16r8"
MAX_ROWS = 40

# nm's symbol-type letters, grouped by the memory they occupy. Uppercase = global, lowercase =
# file-local; both cost the same bytes, so they are folded together.
#   T/t .text    — code in flash (or IRAM)
#   W/V    — weak/COMDAT: an inline or template body, emitted once and merged by the linker
#   R/r .rodata  — NAMED constants only. String literals and most constexpr tables are anonymous
#                  (no symbol at all), so `.flash.rodata` is ~427 KB while barely any of it is
#                  attributable — which is why RODATA is reported but usually near zero, and why
#                  `--strings` measures that section wholesale instead.
#   D/d .data    — initialized globals: RAM at runtime AND a flash copy to initialize from
#   B/b .bss     — zero-init globals: RAM only
_AREA = {"T": "code", "t": "code", "W": "code", "V": "code",
         "R": "rodata", "r": "rodata",
         "D": "data", "d": "data",
         "B": "bss", "b": "bss"}

# `<addr> <size> <type> <name>\t<abs path>:<line>` — the DWARF file is what credits the bytes.
_SRC = re.compile(re.escape(ROOT.as_posix()) + r"/(src/[^:\t]+):\d+\s*$")


def _tool(name, firmware):
    """The binutil for the firmware's ARCHITECTURE, or None.

    The arch has to be selected, not globbed: `.espressif/tools` also holds `esp32ulp-elf-*` (the
    ULP coprocessor's binutils) and, on a P4 machine, the RISC-V set. The ULP `nm` reads an Xtensa
    ELF happily and returns a plausible-looking symbol list — with every `.bss`/`.data` symbol
    silently unattributed, so the STATIC column read 0 across the whole tree. A wrong-tool answer
    that looks right is worse than no answer.
    """
    arch = "riscv32-esp-elf" if ("p4" in firmware or "c6" in firmware or "s31" in firmware) \
        else "xtensa-esp-elf"
    hits = sorted(Path.home().glob(f".espressif/tools/{arch}/**/*-{name}"))
    # Newest toolchain last: several IDF versions can be installed side by side.
    return str(hits[-1]) if hits else None


def elf_path(firmware):
    return ROOT / "build" / f"esp32-{firmware}" / "projectMM.elf"


def collect(elf, nm):
    """One row per source file: bytes per memory area, from the linker's own symbol table."""
    p = subprocess.run([nm, "--print-size", "--line-numbers", "-C", str(elf)],
                       capture_output=True, text=True, check=False)
    if p.returncode != 0:
        return None

    per = collections.defaultdict(collections.Counter)
    for ln in p.stdout.splitlines():
        src = _SRC.search(ln)
        if not src:
            continue                      # SDK, managed component, or no debug info — not ours
        parts = ln.split(None, 3)
        if len(parts) < 4:
            continue
        area = _AREA.get(parts[2])
        if not area:
            continue
        try:
            size = int(parts[1], 16)      # absent on a symbol with no size — skip it
        except ValueError:
            continue
        per[src.group(1)][area] += size
    return per


def collect_strings(elf, objdump):
    """Every string literal in `.flash.rodata`, longest first.

    Counted from the section bytes rather than the source, so a literal that the compiler merged
    or dropped is counted exactly once — which is the number that reaches the device.
    """
    p = subprocess.run([objdump, "-s", "-j", ".flash.rodata", str(elf)],
                       capture_output=True, text=True, check=False)
    if p.returncode != 0:
        return None
    raw = bytearray()
    for ln in p.stdout.splitlines():
        m = re.match(r"^\s+[0-9a-f]+\s((?:[0-9a-f]{2,8}\s){1,4})", ln)
        if m:
            for group in m.group(1).split():
                raw += bytes.fromhex(group)
    # 4+ printable bytes then a NUL: short runs are usually data that happens to look like text.
    return [s[:-1].decode("ascii", "replace") for s in re.findall(rb"[\x20-\x7e]{4,}\x00", bytes(raw))]


def render(per, only, firmware):
    rows = []
    for f, c in per.items():
        if only is not None and f not in only:
            continue
        static = c["bss"] + c["data"]                 # held from boot, enabled or not
        rows.append({"file": f, "code": c["code"], "rodata": c["rodata"],
                     "static": static, "total": c["code"] + c["rodata"] + static})
    # ANY file holding static RAM sorts above every file that holds none, and only then by size.
    # The two columns are not the same currency: internal RAM is ~320 KB shared with WiFi and the
    # task stacks, flash is ~1.5 MB and cheap. On a pure TOTAL sort a file with a big buffer and
    # little code — platform_esp32_tasks.cpp, 230 B of code against 1440 B of static — sinks below
    # files that merely have more code, and can fall off the end of a capped table entirely. A
    # weighting factor would only postpone that; the two-key sort makes it impossible.
    rows.sort(key=lambda r: (r["static"] == 0, -r["static"], -r["total"]))

    L = [f"Footprint on {firmware} — {len(rows)} source file(s), "
         f"{sum(r['total'] for r in rows)} B attributed."]
    if not rows:
        # A module scoped out of THIS firmware is the expected zero, not a failed run — say which,
        # because "nothing attributed" reads as a broken tool and a real zero is indistinguishable
        # from a tool that read nothing. A driver gated on a per-firmware flag (MM_PANEL_CARDS) is
        # simply absent from a variant that does not set it, which is the gate working.
        if only:
            return L + [f"  Not linked into {firmware}: compiled out of this variant by a "
                        f"per-firmware gate, so it costs nothing here.",
                        f"  Measure it on a variant that includes it "
                        f"(--firmware esp32s31 for the panel-card driver)."]
        return L + ["  Nothing attributed. Build the firmware, or the ELF carries no debug info."]

    # What the columns MEAN lives in MoonDeck.md, not in every run's output: it is reference text
    # that does not change between runs, so printing it each time buries the numbers it explains.
    L += [""]
    w = min(max(len(r["file"]) for r in rows), 52)
    L += [f"  {'TOTAL':>8}  {'CODE':>8}  {'RODATA':>8}  {'STATIC':>8}  FILE",
          f"  {'-' * 8}  {'-' * 8}  {'-' * 8}  {'-' * 8}  {'-' * w}"]
    for r in rows[:MAX_ROWS]:
        L.append(f"  {r['total']:>8}  {r['code']:>8}  {r['rodata']:>8}  {r['static']:>8}  "
                 f"{r['file'][:w]}")
    if len(rows) > MAX_ROWS:
        # Say what the cut was by: with STATIC leading the sort, "smallest" would be misleading.
        L.append(f"  … {len(rows) - MAX_ROWS} more — all with STATIC 0, ordered by total.")

    # The one line worth repeating: it is a RESULT (a number that moves), not an explanation.
    held = [r for r in rows if r["static"]]
    L += ["", f"  {len(held)} file(s) hold static RAM; {sum(r['static'] for r in held)} B total."]
    return L


def defining_site(elf, nm):
    """`mangled name -> "src/path.h:line"` for every function, from the ELF's DWARF.

    The object file only names the TRANSLATION UNIT, so a function defined in a header is credited
    to whichever `.cpp` included it — `MoonI80Peripheral::refreshBusKpi` read as `main.cpp`, which
    is where it was compiled but not where it lives. `nm --line-numbers` on the LINKED image gives
    the defining file AND line, which is what a reader needs to go and look at it.
    """
    p = subprocess.run([nm, "--line-numbers", str(elf)], capture_output=True, text=True, check=False)
    out = {}
    for ln in p.stdout.splitlines():
        m = re.match(r"\S+\s+\S+\s+(\S+)\t" + re.escape(ROOT.as_posix()) + r"/(src/[^:]+):(\d+)", ln)
        if m:
            out.setdefault(m.group(1), f"{m.group(2)}:{m.group(3)}")
    return out


def our_strings(firmware, objdump, cxxfilt, sites=None):
    """Every string literal OUR code defines: `(bytes, text, source file, function)`.

    Read from the per-source **object files**, not the linked image. The link merges every
    `.rodata.*.str` input section into one `.flash.rodata`, and a literal carries no symbol — so
    from the ELF alone a string cannot be traced back to who wrote it, which is what made the
    earlier whole-image report impossible to act on (its longest entries were all ESP-IDF assert
    expressions).

    `-ffunction-sections` is what makes this work: each function's literals land in
    `.rodata.<mangled>.str1.N` inside its own object, so the section name IS the owning function
    and the object path IS the source file. The name is demangled for reading.
    """
    objdir = ROOT / "build" / f"esp32-{firmware}" / "esp-idf" / "main" / "CMakeFiles" / "__idf_main.dir"
    if not objdir.is_dir():
        return None
    objs = sorted(objdir.rglob("*.obj"))
    if not objs:
        return None

    rows, mangled = [], []
    for obj in objs:
        src = str(obj)
        src = "src/" + src.split("/src/", 1)[1][:-4] if "/src/" in src else obj.name[:-4]
        head = subprocess.run([objdump, "-h", str(obj)], capture_output=True, text=True, check=False)
        for ln in head.stdout.splitlines():
            m = re.search(r"(\.rodata\.(\S+?)\.str[\d.]*)\s+([0-9a-f]{8})", ln)
            if not m or int(m.group(3), 16) == 0:
                continue
            dump = subprocess.run([objdump, "-s", "-j", m.group(1), str(obj)],
                                  capture_output=True, text=True, check=False)
            raw = bytearray()
            for dl in dump.stdout.splitlines():
                h = re.match(r"^\s+[0-9a-f]+\s((?:[0-9a-f]{2,8}\s){1,4})", dl)
                if h:
                    for g in h.group(1).split():
                        raw += bytes.fromhex(g)
            for lit in re.findall(rb"[\x20-\x7e]{2,}", bytes(raw)):
                site = (sites or {}).get(m.group(2), src)
                rows.append([len(lit) + 1, lit.decode("ascii", "replace"), site, m.group(2)])
                mangled.append(m.group(2))

    # One c++filt for every name: a call per string would dominate the runtime.
    if mangled:
        out = subprocess.run([cxxfilt], input="\n".join(mangled), capture_output=True,
                             text=True, check=False).stdout.splitlines()
        for row, name in zip(rows, out):
            row[3] = name
    return rows


def render_strings(strs, ours, only):
    """Two tables: OUR literals (per file + function, actionable) and the image total (context)."""
    image_total = sum(len(s) + 1 for s in strs)
    L = []

    if ours is None:
        L.append("Strings: object files not found — build the firmware to attribute them.")
        return L

    # `r[2]` is "file:line", so compare on the file half — `only` holds bare paths.
    scoped = [r for r in ours if only is None or r[2].rsplit(":", 1)[0] in only]
    mine = sum(r[0] for r in scoped)
    # OURS first and IDF's second, because only one of them is a decision. The image total is
    # context — its longest entries are IDF assert expressions we cannot shorten, and reporting
    # them mixed together made the whole table unactionable.
    L += [f"Strings we define: {len(scoped)}, {mine} B "
          f"(the whole .flash.rodata is {image_total} B including ESP-IDF, which we cannot change).",
          ""]
    if not scoped:
        return L + ["  None in scope."]

    # Grouped by what they are FOR: the trimming answer differs per kind — prose can be shortened,
    # an identifier is an API contract, and a wire template is a protocol.
    cat, size = collections.Counter(), collections.Counter()
    for n, s, _f, _fn in scoped:
        low = s.lower()
        if s.startswith(("{", "[")) or '":' in s:
            k = "JSON / wire format"
        elif any(word in low for word in ("fail", "error", "invalid", "cannot", "unable")):
            k = "error / status text"
        elif " " not in s:
            k = "identifiers / keys"
        else:
            k = "other prose"
        cat[k] += 1
        size[k] += n
    for k, _ in size.most_common():
        L.append(f"  {k:<22} {size[k]:>7} B  {100 * size[k] / mine:5.1f}%  ({cat[k]} strings)")

    L += ["", "  LONGEST — file and function, so a long literal can be found and judged:"]
    fw = min(max(len(r[2]) for r in scoped), 42)
    L += [f"  {'BYTES':>5}  {'FILE:LINE':<{fw}}  {'FUNCTION':<30}  TEXT",
          f"  {'-' * 5}  {'-' * fw}  {'-' * 30}  {'-' * 44}"]
    for n, s, f, fn in sorted(scoped, key=lambda r: -r[0])[:20]:
        # Just the member name: the FILE column already says which class, so `mm::Class::` is
        # repetition that pushes the readable part of a long signature off the row.
        # GCC's outlined variants (`…$part$0`, `…$isra$0`) are not valid mangled names, so
        # c++filt hands them back untouched; strip the suffix and retry the readable tail.
        base = fn.split("$")[0]
        short = base.split("(")[0].split("::")[-1] or base
        if short.startswith("_ZN"):
            short = re.sub(r"^_ZN?[0-9]+", "", short)   # last resort: drop the mangling prefix
        L.append(f"  {n:>5}  {f[-fw:]:<{fw}}  {short[:30]:<30}  {s[:64]}")
    return L


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--firmware", default=DEFAULT_FW, help=f"Built firmware (default {DEFAULT_FW}).")
    ap.add_argument("--module", help="Only this module's source files.")
    args = ap.parse_args()

    elf = elf_path(args.firmware)
    if not elf.exists():
        print(f"No ELF at {elf.relative_to(ROOT)} — build it first:\n"
              f"  uv run moondeck/build/build_esp32.py --firmware {args.firmware}", file=sys.stderr)
        return 2

    nm = _tool("nm", args.firmware)
    if not nm:
        print("No nm in the ESP-IDF toolchain — install/activate ESP-IDF.", file=sys.stderr)
        return 2
    per = collect(elf, nm)
    if per is None:
        print(f"nm could not read {elf.name}.", file=sys.stderr)
        return 2
    # An empty result means the ELF carried no usable debug info — say so rather than print a
    # clean-looking zero (the silent-zero rule the other checks follow).
    if not per:
        print(f"No symbol mapped back to src/ in {elf.name}. The build carries no DWARF, so "
              f"nothing can be attributed — this is a broken measurement, not a small one.",
              file=sys.stderr)
        return 2

    only = None
    if args.module:
        only = check_clang_query.module_files(args.module)
        if not only:
            print(f"No source files for module '{args.module}'.", file=sys.stderr)
            return 2
        print(f"Filtered to {args.module}: {', '.join(only)}")
        # Resolved by FILENAME, so a module whose bytes partly live in a differently-named sibling
        # (a *Packet.h wire format, say) reports only the files that share its name. Said out loud
        # because a partial number that looks whole is worse than one that admits its scope.
        print("  (files matching the module name; a differently-named sibling header is not "
              "included)\n")

    print("\n".join(render(per, only, args.firmware)))

    # The string table rides along in the SAME run: both halves answer one question ("what does
    # this cost"), they read the same ELF, and the whole report is under a second. A separate mode
    # meant remembering to run it, which is how a report goes unread.
    objdump = _tool("objdump", args.firmware)
    cxxfilt = _tool("c++filt", args.firmware)
    if objdump and cxxfilt:
        strs = collect_strings(elf, objdump) or []
        ours = our_strings(args.firmware, objdump, cxxfilt, defining_site(elf, nm))
        print()
        print("\n".join(render_strings(strs, ours, only)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
