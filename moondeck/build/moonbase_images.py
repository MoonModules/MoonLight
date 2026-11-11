#!/usr/bin/env python3
"""Build the images that carry MoonBase beside the app: MoonBase itself and the migration image.

build_esp32.py calls both after the app build for a variant that carries them.
"""

import json
import subprocess
import sys
from pathlib import Path

import compute_version   # sibling: the one place a version string is derived
from generate_build_info import build_id   # sibling: the one place the build id is derived
import sdkconfig_fragments   # sibling: a changed fragment regenerates a build dir's sdkconfig

ROOT = Path(__file__).resolve().parent.parent.parent

# MoonBase's output name, the `project()` name in moonbase/CMakeLists.txt; build_esp32.py re-exports it beside the app's.
MOONBASE_BIN = "MoonLight-moonbase.bin"


def family_defaults(project: Path, family: str, chip: str) -> list[Path]:
    """The sdkconfig defaults a MoonBase or migration build of `family` layers: the project's, its chip's, which ESP-IDF adds by the target's name, and the family's own where it differs from the chip (the P4 from revision 3)."""
    names = ["sdkconfig.defaults", f"sdkconfig.defaults.{chip}"] + ([f"sdkconfig.defaults.{family}"] if family != chip else [])
    return [project / n for n in names if (project / n).exists()]


def build_moonbase(cmd: list[str], env: dict, chip: str, version: str = "", family: str = "") -> None:
    """Build the MoonBase image for `family`, by default the chip, into build/moonbase-<family>.

    MoonBase (moonbase/) is the second boot image the MoonBase variants carry in their factory
    partition: a small firmware whose job is installing the application, since a board with one
    app slot cannot rewrite the partition it is executing from. It is chip-specific but variant-
    agnostic, so every classic variant shares one build. Its size budget lives in
    moonbase/sdkconfig.defaults; the shared partition table keeps the two images provably agreed
    on where everything lives.

    `version` plus the build id becomes PROJECT_VER, `6.0.0+990a3d84` in SemVer's build-metadata form, which IDF writes into the image's app descriptor.
    The app reads it back from the factory partition and shows the version and the build id as it shows its own, so a device can say which MoonBase it carries.
    A changed id also changes the descriptor's compile flags, so IDF recompiles it and its date is the build's rather than the build folder's first.
    A version is variant-independent, so passing it keeps one image per chip valid.
    """
    moonbase_dir = ROOT / "moonbase"
    family = family or chip
    build_dir = ROOT / "build" / f"moonbase-{family}"
    fragments = family_defaults(moonbase_dir, family, chip)
    b_arg = ["-B", str(build_dir), f"-DSDKCONFIG={build_dir}/sdkconfig"]
    if family != chip:
        b_arg.append("-DSDKCONFIG_DEFAULTS=" + ";".join(str(f) for f in fragments if not f.name.endswith(f".{chip}")))
    if not version:
        # The library.json default the app resolves through build_info.cpp's #ifndef, rather than a git-describe string the app cannot compare against.
        version = compute_version.compute("local", "")
    b_arg.append(f"-DPROJECT_VER={version}+{build_id()}")
    # An edited moonbase/sdkconfig.defaults, or the chip's or family's own beside it, regenerates the sdkconfig, as the app's fragments do.
    digest = sdkconfig_fragments.refresh(cmd + b_arg, build_dir, fragments, moonbase_dir, env)
    if not build_dir.exists():
        print(f"Setting MoonBase target to {chip}...")
        r = subprocess.run(cmd + b_arg + ["set-target", chip], cwd=moonbase_dir, env=env)
        if r.returncode != 0:
            sys.exit(r.returncode)
    print(f"Building MoonBase for {family}...")
    r = subprocess.run(cmd + b_arg + ["build"], cwd=moonbase_dir, env=env)
    if r.returncode != 0:
        sys.exit(r.returncode)
    sdkconfig_fragments.record(build_dir, digest)
    binp = build_dir / MOONBASE_BIN
    if binp.exists():
        kb = binp.stat().st_size / 1024
        # Slot fit is printed by IDF itself ("Smallest app partition ... free"); repeating a
        # hardcoded slot size here would lie the day the table changes.
        print(f"MoonBase image: {kb:.0f} KB")


# A partition table's binary size, ESP-IDF's ESP_PARTITION_TABLE_MAX_LEN: the migration image carries its tables in slots of this size.
TABLE_SLOT = 0xC00
# Where the partition table starts, ESP-IDF's default CONFIG_PARTITION_TABLE_OFFSET: a bootloader's slot runs from its offset up to here.
TABLE_OFFSET = 0x8000


def migrate_blobs(cmd: list[str], env: dict, pairs: list[tuple[Path, Path]], out_dir: Path) -> tuple[Path, Path]:
    """The tables and the bootloaders a migration image carries, one pair per flash size and type, as two files of slots in the same order: the image writes the pair that fits the board's flash.

    A bootloader's slot runs from where the chip's ROM looks for it up to the table, padded with erased bytes, the length the image writes.
    """
    gen = Path(env["IDF_PATH"]) / "components" / "partition_table" / "gen_esp32part.py"
    out_dir.mkdir(parents=True, exist_ok=True)
    tables, boots = bytearray(), bytearray()
    for csv, boot in pairs:
        part = out_dir / (csv.stem + ".bin")
        if subprocess.run([cmd[0], str(gen), "--quiet", str(csv), str(part)], env=env).returncode != 0:
            sys.exit(f"the partition table {csv.name} did not generate")
        data = part.read_bytes()
        tables += data + b"\xff" * (TABLE_SLOT - len(data))
        offset = int(json.loads((boot.parent.parent / "flasher_args.json").read_text())["bootloader"]["offset"], 16)
        image = boot.read_bytes()
        if len(image) > TABLE_OFFSET - offset:
            sys.exit(f"{boot} is larger than the room before the table")
        boots += image + b"\xff" * (TABLE_OFFSET - offset - len(image))
    (out_dir / "partition-tables.bin").write_bytes(bytes(tables))
    (out_dir / "bootloaders.bin").write_bytes(bytes(boots))
    return out_dir / "partition-tables.bin", out_dir / "bootloaders.bin"


def build_migrate(cmd: list[str], env: dict, chip: str, family: str, pairs: list[tuple[Path, Path]], pause: int = 0) -> None:
    """Build the migration image for `family` into build/migrate-<family>, or with `pause` the rehearsal image into build/migrate-<family>-pause.

    The image a board running WLED, Tasmota or ESPHome installs through that firmware's own update page: it carries MoonBase and, per flash size and type, a MoonLight partition table with the bootloader of the variant it comes from; it writes MoonBase, then the bootloader and the table that fit the board's flash, and restarts into MoonBase (moonbase/migrate/).
    It embeds what the MoonBase build and the bootloader builds produced, so it builds after them.
    """
    project = ROOT / "moonbase" / "migrate"
    build_dir = ROOT / "build" / (f"migrate-{family}-pause" if pause else f"migrate-{family}")
    moonbase_bin = ROOT / "build" / f"moonbase-{family}" / MOONBASE_BIN
    table_bin, boot_bin = migrate_blobs(cmd, env, pairs, build_dir / "tables")
    b_arg = ["-B", str(build_dir), f"-DSDKCONFIG={build_dir}/sdkconfig",
             f"-DMOONBASE_BIN={moonbase_bin}", f"-DTABLE_BIN={table_bin}", f"-DBOOTLOADER_BIN={boot_bin}"]
    defaults = [f for f in family_defaults(project, family, chip) if not f.name.endswith(f".{chip}")]
    if pause:
        # The pause rides in a defaults file of its own, layered after the project's, so the shipped image never carries it.
        extra = build_dir.with_suffix(".defaults")
        extra.write_text(f"CONFIG_MM_MIGRATE_PAUSE_S={pause}\n")
        defaults.append(extra)
    if len(defaults) > 1:
        b_arg.append("-DSDKCONFIG_DEFAULTS=" + ";".join(str(f) for f in defaults))
    # The target as a define rather than set-target, which refuses a build folder holding the tables and no CMake cache yet.
    b_arg.append(f"-DIDF_TARGET={chip}")
    print(f"Building the migration image for {family}...")
    r = subprocess.run(cmd + b_arg + ["build"], cwd=project, env=env)
    if r.returncode != 0:
        sys.exit(r.returncode)
