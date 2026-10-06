#!/usr/bin/env python3
"""Verify every instruction MoonLive emits against the toolchain's own assembler.

We hand-encode machine instructions. That is the right call for a JIT, an assembler cannot be
shipped to a device, but it means a wrong offset field, a truncated displacement or a mis-placed
register nibble is a bug nothing else notices: the golden-bytes tests compare our emission to our
PREVIOUS emission, and the structural checks compare it to a model we also wrote. Both agree with a
consistent mistake.

This compares it to something nobody here wrote: `xtensa-esp32-elf-as` / `riscv32-esp-elf-as`, which
ARE the definition of a valid encoding for these ISAs. For each instruction we emit, assemble the
same mnemonic and require identical bytes.

What it covers, and what it does not: it proves each instruction is ENCODED correctly. It cannot
prove the SEQUENCE is right, a correctly encoded instruction can still save the wrong register.
Execution-level checks (QEMU, the bench) are what cover that.

    uv run moondeck/moonlive/check_encodings.py            # every ISA that has a toolchain
    uv run moondeck/moonlive/check_encodings.py --isa xtensa
"""

import argparse
import glob
import os
import subprocess
import sys
import tempfile

# Per ISA: where its assembler lives, and the instructions our emitter produces. Each case is
# (mnemonic-as-the-toolchain-spells-it, the bytes our assembler emits for it, why it is here).
#
# The expected bytes are OURS, written down from the emitter's own encoders. The test's value is
# that `as` must independently agree; when it does not, one of the two is wrong and the diff says
# which field.
ISAS = {
    "xtensa": {
        "as": "~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32-elf-as",
        "objcopy": "~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32-elf-objcopy",
        # --no-transform stops `as` "helpfully" relaxing a wide instruction into its narrow
        # equivalent (s32i -> s32i.n). Both encode the same operation, but our emitter always picks
        # the wide form, one encoder that covers every offset, so without this the comparison
        # reports a difference that is not a defect.
        "asflags": ["--no-transform"],
        "cases": [
            # The frame. `entry`'s immediate counts EIGHT-byte units and sits at bits 12..23, the
            # field that was silently wrong when the base-save area was missing.
            ("entry a1, 160",        "364101", "prologue: the whole-routine frame"),
            ("entry a1, 48",         "366100", "prologue: the minimum frame"),
            # Frame slots. The offset field counts FOUR-byte words, so a slot index maps onto it
            # directly, and a slot past 255 words would silently truncate.
            ("s32i a2, a1, 112",     "22611c", "spillStore: park a host argument"),
            ("l32i a11, a1, 128",    "b22120", "spillLoad: reload the arena pointer"),
            ("s32i a10, a1, 32",     "a26108", "call: park the result"),
            ("addi a9, a1, 60",      "92c13c", "slotAddr: the address of an argument block"),
            ("addmi a9, a1, 256",    "92d101", "slotAddr: a slot past byte 127 takes the 256-byte step first"),
            ("addi a9, a9, -124",    "92c984", "slotAddr: then the signed rest, so byte 132 is reached"),
            # Arithmetic, narrow and wide forms.
            ("add.n a2, a2, a13",    "da22",   "add: the narrow two-byte form"),
            ("mull a9, a2, a11",     "b09282", "mul"),
            ("movi a2, 255",         "22a0ff", "movi: an eight-bit immediate"),
            ("mov.n a13, a9",        "dd09",   "mov: the narrow form"),
            # Control flow. `bltu`'s displacement is a single SIGNED byte (+/-127), the field that
            # truncated silently on a long loop body before branch relaxation.
            ("callx8 a8",            "e00800", "call: the windowed call"),
            # The script-to-script call. The offset field is patched later, so what is pinned here
            # is the OPCODE: a wrong low-six-bits would decode as an unrelated instruction, and the
            # first version of this used 0x25 in the wrong byte position. Assembled at a known pc
            # with a known target so the displacement is reproducible.
            ("call8 . - 4",          "a5ffff", "call: a function in this block, by label"),
            ("retw.n",               "1df0",   "epilogue"),
            ("l8ui a2, a11, 8",      "220b08", "LoadCtrl: read a control byte from the arena"),
            ("s8i a3, a12, 0",       "324c00", "StoreElem: write one channel"),
        ],
    },
    "riscv": {
        "as": "~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-as",
        "objcopy": "~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-objcopy",
        # rv32im WITHOUT the compressed extension: our emitter only ever produces the 4-byte
        # forms, and letting `as` pick a 2-byte one would compare two valid encodings of the
        # same mnemonic and report a difference that is not a defect.
        "asflags": ["-march=rv32im", "-mno-relax"],
        "cases": [
            ("addi sp, sp, -96",     "130101fa", "prologue: allocate the frame"),
            ("sw s0, 92(sp)",        "232e8104", "prologue: save the frame pointer"),
            ("addi s0, sp, 96",      "13040106", "prologue: s0 = the caller's sp"),
            ("sw a0, -32(s0)",       "2320a4fe", "spillStore: park a host argument"),
            ("lw a1, -28(s0)",       "832544fe", "spillLoad"),
            ("addi t1, s0, -96",     "130304fa", "slotAddr"),
            ("mul a0, a1, a2", "3385c502", "mul"),
            ("jalr ra, t6, 0",       "e7800f00", "call"),
            ("ret",                  "67800000", "epilogue"),
        ],
    },
}


def tool(pattern: str, isa: str, what: str) -> str:
    hits = glob.glob(os.path.expanduser(pattern))
    if not hits:
        sys.exit(f"{what} for {isa} not found, install the matching ESP-IDF toolchain")
    return sorted(hits)[-1]


def check(isa: str) -> int:
    spec = ISAS[isa]
    as_bin = tool(spec["as"], isa, "assembler")
    objcopy = tool(spec["objcopy"], isa, "objcopy")
    failures = 0

    print(f"\n== {isa} ==")
    with tempfile.TemporaryDirectory() as tmp:
        for mnemonic, ours, why in spec["cases"]:
            src = os.path.join(tmp, "one.S")
            with open(src, "w") as f:
                f.write(f"    {mnemonic}\n")
            obj = os.path.join(tmp, "one.o")
            r = subprocess.run([as_bin, *spec.get("asflags", []), "-o", obj, src],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(f"  FAIL {mnemonic:24s} assembler rejected it: {r.stderr.strip()[:80]}")
                failures += 1
                continue
            binf = os.path.join(tmp, "one.bin")
            oc = subprocess.run([objcopy, "-O", "binary", obj, binf], capture_output=True, text=True)
            if oc.returncode != 0:
                print(f"  FAIL {mnemonic:24s} objcopy failed: {oc.stderr.strip()[:80]}")
                failures += 1
                continue
            with open(binf, "rb") as f:
                theirs = f.read().hex()
            if theirs == ours:
                print(f"  ok   {mnemonic:24s} {ours:<10s} {why}")
            else:
                print(f"  FAIL {mnemonic:24s} we emit {ours}, the assembler says {theirs}  ({why})")
                failures += 1
    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--isa", choices=[*ISAS, "all"], default="all")
    args = ap.parse_args()

    isas = list(ISAS) if args.isa == "all" else [args.isa]
    total = sum(check(i) for i in isas)
    print()
    if total:
        print(f"{total} encoding(s) disagree with the toolchain assembler")
        return 1
    print("every encoding matches the toolchain assembler")
    return 0


if __name__ == "__main__":
    sys.exit(main())
