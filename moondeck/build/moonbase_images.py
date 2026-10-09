#!/usr/bin/env python3
"""Build the images that carry MoonBase beside the app: MoonBase itself and the migration image.

build_esp32.py calls both after the app build for a variant that carries them.
"""

import shutil
import subprocess
import sys
from pathlib import Path

import compute_version   # sibling: the one place a version string is derived
from generate_build_info import build_id   # sibling: the one place the build id is derived

ROOT = Path(__file__).resolve().parent.parent.parent

# MoonBase's output name, the `project()` name in moonbase/CMakeLists.txt; build_esp32.py re-exports it beside the app's.
MOONBASE_BIN = "MoonLight-moonbase.bin"


def build_moonbase(cmd: list[str], env: dict, chip: str, version: str = "") -> None:
    """Build the MoonBase image for `chip` into build/moonbase-<chip>.

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
    build_dir = ROOT / "build" / f"moonbase-{chip}"
    b_arg = ["-B", str(build_dir), f"-DSDKCONFIG={build_dir}/sdkconfig"]
    if not version:
        # The library.json default the app resolves through build_info.h's #ifndef, rather than a git-describe string the app cannot compare against.
        version = compute_version.compute("local", "")
    b_arg.append(f"-DPROJECT_VER={version}+{build_id()}")
    # Same trap as stale_feature_cache: IDF generates sdkconfig from the defaults only when it is
    # absent, so an edited moonbase/sdkconfig.defaults silently changes nothing. One defaults file
    # here, so mtime is a sufficient staleness signal.
    gen = build_dir / "sdkconfig"
    defaults = moonbase_dir / "sdkconfig.defaults"
    if gen.exists() and defaults.stat().st_mtime > gen.stat().st_mtime:
        print(f"MoonBase build dir {build_dir.name} predates sdkconfig.defaults; "
              "removing it for a clean reconfigure.")
        shutil.rmtree(build_dir)
    if not build_dir.exists():
        print(f"Setting MoonBase target to {chip}...")
        r = subprocess.run(cmd + b_arg + ["set-target", chip], cwd=moonbase_dir, env=env)
        if r.returncode != 0:
            sys.exit(r.returncode)
    print(f"Building MoonBase for {chip}...")
    r = subprocess.run(cmd + b_arg + ["build"], cwd=moonbase_dir, env=env)
    if r.returncode != 0:
        sys.exit(r.returncode)
    binp = build_dir / MOONBASE_BIN
    if binp.exists():
        kb = binp.stat().st_size / 1024
        # Slot fit is printed by IDF itself ("Smallest app partition ... free"); repeating a
        # hardcoded slot size here would lie the day the table changes.
        print(f"MoonBase image: {kb:.0f} KB")


def build_migrate(cmd: list[str], env: dict, chip: str, app_build_dir: Path, pause: int = 0) -> None:
    """Build the migration image for `chip` into build/migrate-<chip>, or with `pause` the rehearsal image into build/migrate-<chip>-pause.

    The image a board running WLED installs through WLED's own update page: it carries this variant's partition table and MoonBase, writes both, and restarts into MoonBase (moonbase/migrate/).
    It embeds what the app and MoonBase builds produced, so it builds after both.
    """
    project = ROOT / "moonbase" / "migrate"
    build_dir = ROOT / "build" / (f"migrate-{chip}-pause" if pause else f"migrate-{chip}")
    moonbase_bin = ROOT / "build" / f"moonbase-{chip}" / MOONBASE_BIN
    table_bin = app_build_dir / "partition_table" / "partition-table.bin"
    b_arg = ["-B", str(build_dir), f"-DSDKCONFIG={build_dir}/sdkconfig",
             f"-DMOONBASE_BIN={moonbase_bin}", f"-DTABLE_BIN={table_bin}"]
    if pause:
        # The pause rides in a defaults file of its own, layered after the project's, so the shipped image never carries it.
        extra = build_dir.with_suffix(".defaults")
        extra.write_text(f"CONFIG_MM_MIGRATE_PAUSE_S={pause}\n")
        b_arg.append(f"-DSDKCONFIG_DEFAULTS={project / 'sdkconfig.defaults'};{extra}")
    if not (build_dir / "CMakeCache.txt").exists():
        print(f"Setting migrate target to {chip}...")
        r = subprocess.run(cmd + b_arg + ["set-target", chip], cwd=project, env=env)
        if r.returncode != 0:
            sys.exit(r.returncode)
    print(f"Building the migration image for {chip}...")
    r = subprocess.run(cmd + b_arg + ["build"], cwd=project, env=env)
    if r.returncode != 0:
        sys.exit(r.returncode)
