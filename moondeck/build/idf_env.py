#!/usr/bin/env python3
"""Find, pin and run ESP-IDF: where it is installed, which commit every target is validated against, its Python and the environment and command idf.py runs in.

One concern of the ESP32 build, which build_esp32.py, the flash and erase scripts, the KPI and the bench tests share.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

from build_host import build_jobs   # sibling: the one bound on compile jobs

ROOT = Path(__file__).resolve().parent.parent.parent

# Common ESP-IDF install locations
IDF_SEARCH_PATHS = [
    Path.home() / "esp" / "esp-idf",
    Path.home() / ".espressif" / "esp-idf",
    Path("/opt/esp-idf"),
]

# The ESP-IDF commit every target (classic ESP32, S3, P4, S31) has been
# validated against; the `v6.1.1` tag, a bugfix release on the earliest IDF
# line that carries the esp32s31 preview target. Kept here (not in setup_esp_idf.py) so
# the pre-build drift check below can share the constant; a stale local IDF is
# the single most common source of an "it built for me last week" ESP32 build
# failure, so the check runs on every build_esp32 invocation, not only when the
# user remembers to re-run setup_esp_idf.py. setup_esp_idf.py imports these
# two constants.
PINNED_IDF_COMMIT = "051e4c4eb20346e043cdd47bc388fde75ff8af63"
PINNED_IDF_VERSION = "v6.1.1"


def installed_idf_commit(idf_path: Path) -> str:
    """Return the git HEAD SHA of the installed IDF, or '' if unavailable."""
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(idf_path),
                           capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else ""
    except OSError:
        return ""


def check_idf_pin(idf_path: Path) -> None:
    """Abort the build if the installed IDF isn't at PINNED_IDF_COMMIT.

    Reason: silently building against a drifted IDF produces the classic
    "missing symbol" / "renamed cap macro" errors that look like *our* bug but
    are Espressif renaming an API between snapshots. Failing fast with
    the exact fix command turns 30-minute debugging into a 30-second
    re-checkout. Skipped when HEAD can't be determined (not a git checkout, or
    git missing), so a skip rather than a false alarm.

    Deliberate escape hatch: a dev testing an upcoming IDF release (beta1 → RC,
    RC → GA) needs to build against a drifted IDF on purpose. Symmetric with
    setup_esp_idf.py's --no-checkout, that path is `--skip-idf-pin-check`
    threaded from main(); passed here as `bypass=True`.
    """
    installed = installed_idf_commit(idf_path)
    if not installed or installed == PINNED_IDF_COMMIT:
        return
    print(f"\nESP-IDF commit drift: installed {installed[:12]} != "
          f"pinned {PINNED_IDF_COMMIT[:12]} ({PINNED_IDF_VERSION}).", file=sys.stderr)
    print("The build was validated against the pinned commit; a drifted IDF is "
          "the most common source of ESP32 build failures that look like "
          "MoonLight bugs but are actually Espressif renaming a symbol.",
          file=sys.stderr)
    print("Fix: re-run `uv run moondeck/build/setup_esp_idf.py` (it will offer "
          "to check out the pinned commit + resync submodules + reinstall "
          "toolchains). See docs/reference/build-system.md § ESP-IDF version for the "
          "manual command if you'd rather do it by hand.", file=sys.stderr)
    print("Or pass --skip-idf-pin-check to build anyway (deliberate migration "
          "to a newer IDF release; re-tests then update PINNED_IDF_COMMIT).",
          file=sys.stderr)
    sys.exit(2)


def find_idf() -> Path | None:
    """Find ESP-IDF installation. Checks IDF_PATH env, then common locations."""
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        p = Path(idf_path)
        if (p / "tools" / "idf.py").exists():
            return p

    for p in IDF_SEARCH_PATHS:
        if (p / "tools" / "idf.py").exists():
            return p
    return None


# Python venv layout differs between platforms; POSIX puts the interpreter in
# `<venv>/bin/python`; Windows in `<venv>/Scripts/python.exe`. Same for the
# toolchain `bin` dirs ESP-IDF unpacks under ~/.espressif/tools (POSIX) vs
# %USERPROFILE%\.espressif\tools\…\Scripts (Windows for some tools, `bin` for
# the GCC toolchains). The constants below capture the per-platform split.
_VENV_BIN = "Scripts" if sys.platform == "win32" else "bin"
_PYTHON_EXE = "python.exe" if sys.platform == "win32" else "python"


def find_idf_python(idf_path: Path | None = None) -> Path | None:
    """Find the ESP-IDF Python venv for the target IDF version.

    ESP-IDF names each venv `idf<major.minor>_py<X.Y>_env` (e.g.
    `idf6.1_py3.12_env`), one per IDF version × Python version, exactly as
    `idf_tools.py` computes it. We select by matching the *target* IDF version,
    NOT by mtime: with two IDFs installed (e.g. a 5.5 alongside 6.1 for a
    version fallback), the most-recently-activated venv is the newest by mtime
    but belongs to whichever IDF was last sourced, so an mtime pick silently
    hands a 6.1 build the 5.5 venv (mismatched esptool → "requirements not
    satisfied"). Matching the version makes selection a function of what we're
    building, not what was last run.

    Among venvs for the right IDF version (there can be several Python-minor
    variants) the newest wins. Falls back to newest-overall only when the IDF
    version can't be determined or no versioned venv matches; preserving the
    single-IDF behavior where mtime is unambiguous anyway.
    """
    venv_dir = Path.home() / ".espressif" / "python_env"
    if not venv_dir.exists():
        return None
    candidates = []
    for d in venv_dir.iterdir():
        if (d / _VENV_BIN / _PYTHON_EXE).exists():
            candidates.append((d.stat().st_mtime, d))
    if not candidates:
        return None

    if idf_path is not None:
        # idf_version() → "6.1.0"; the venv prefix uses only major.minor.
        prefix = "idf" + ".".join(idf_version(idf_path).split(".")[:2]) + "_"
        matched = [c for c in candidates if c[1].name.startswith(prefix)]
        if matched:
            matched.sort(reverse=True)
            return matched[0][1]

    candidates.sort(reverse=True)
    return candidates[0][1]


def idf_version(idf_path: Path) -> str:
    """Extract a clean semver from the IDF version string."""
    version_file = idf_path / "version.txt"
    if version_file.exists():
        raw = version_file.read_text(encoding="utf-8").strip()
        # Extract major.minor.patch from strings like "v6.1-dev-399-gd1b91b79b5"
        m = re.match(r"v?(\d+\.\d+)(?:\.(\d+))?", raw)
        if m:
            return f"{m.group(1)}.{m.group(2) or '0'}"
    return "5.4.0"  # safe fallback


def idf_env(idf_path: Path) -> dict:
    """Build the environment for running idf.py."""
    env = dict(os.environ)
    env["IDF_PATH"] = str(idf_path)
    env["ESP_IDF_VERSION"] = idf_version(idf_path)
    # ESP-IDF's Python tooling refuses to run on a non-UTF-8 locale. Windows
    # defaults to cp1252 (locale "English_Netherlands.1252", for one), so idf.py
    # bails with "Support for Unicode is required". PYTHONUTF8=1 (PEP 540)
    # forces Python into UTF-8 mode regardless of the system locale, and
    # PYTHONIOENCODING covers the stdin/stdout/stderr streams.
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    # idf.py's ninja takes every core unless IDF_PY_BUILD_JOBS names a count.
    env.setdefault("IDF_PY_BUILD_JOBS", str(build_jobs()))

    venv_path = find_idf_python(idf_path)
    if venv_path:
        env["IDF_PYTHON_ENV_PATH"] = str(venv_path)

    # IDF's post-build gen_gdbinit.py reads ESP_ROM_ELF_DIR (export.sh sets it;
    # this hand-built env must too). The step only re-runs when its inputs
    # change, so the missing variable failed builds intermittently.
    rom_elfs = Path.home() / ".espressif" / "tools" / "esp-rom-elfs"
    if rom_elfs.exists():
        versions = sorted((d for d in rom_elfs.iterdir() if d.is_dir()), reverse=True)
        if versions:
            env["ESP_ROM_ELF_DIR"] = str(versions[0]) + os.sep

    # Build PATH: venv bin + IDF tools + toolchains + existing PATH
    extra_paths = []

    if venv_path:
        extra_paths.append(str(venv_path / _VENV_BIN))

    extra_paths.append(str(idf_path / "tools"))

    # Add toolchain paths from ~/.espressif/tools. Tool layout varies; POSIX
    # tools have `bin/` subdirs (xtensa-esp-elf, cmake), Windows tools often
    # don't (ninja, ccache, idf-exe ship the .exe at the version-dir root, or
    # inside a single product-named subdir). Add both: the version dir itself
    # (catches flat layouts) plus any nested `bin/` subdir (catches POSIX
    # layouts). Together this covers every tool IDF installs on either host.
    tools_dir = Path.home() / ".espressif" / "tools"
    if tools_dir.exists():
        for tool in tools_dir.iterdir():
            if tool.is_dir():
                for version_dir in sorted(tool.iterdir(), reverse=True):
                    extra_paths.append(str(version_dir))
                    bin_dirs = list(version_dir.rglob("bin"))
                    if bin_dirs:
                        extra_paths.append(str(bin_dirs[0]))
                    break

    env["PATH"] = os.pathsep.join(extra_paths + [env.get("PATH", "")])
    return env


def idf_cmd(idf_path: Path) -> list[str]:
    """Return the command to invoke idf.py via the venv Python.

    On Windows the entry point is `_idf_win_shim.py` instead of idf.py
    directly; the shim calls `locale.setlocale(LC_ALL, "en_US.UTF-8")`
    BEFORE idf.py runs, which is the only way to make IDF's locale check
    (`locale.getlocale()`) pass on Windows installs whose system locale
    is non-UTF-8 (e.g. Dutch / German / French). See the shim's docstring.
    """
    venv_path = find_idf_python(idf_path)
    python_exe = (str(venv_path / _VENV_BIN / _PYTHON_EXE)
                  if venv_path else "python")
    if sys.platform == "win32":
        shim = Path(__file__).resolve().parent / "_idf_win_shim.py"
        return [python_exe, str(shim)]
    return [python_exe, str(idf_path / "tools" / "idf.py")]
