#!/usr/bin/env python3
"""Generate src/core/util/build_info.h.

Writes a single header carrying every compile-time identity fact the runtime exposes through
SystemModule. What each macro means lives in the header's own lead, which this template writes;
the `#ifndef` defaults are part of that template, so they survive regeneration.
"""

import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
LIBRARY_JSON = ROOT / "library.json"
OUT_FILE = ROOT / "src" / "core" / "util" / "build_info.h"

data = json.loads(LIBRARY_JSON.read_text())
version = data["version"]


def build_id() -> str:
    """The short git hash the binary was built from, with `+` if the tree was dirty.

    This is the answer to "which code is on this board?" — the question MM_BUILD_DATE
    cannot answer. `__DATE__`/`__TIME__` expand when the *including translation unit*
    compiles, and build_info.h is a header consumed by one TU: change a driver .cpp and
    that TU is NOT rebuilt, so the reported date FREEZES while the firmware moves on. A
    stale date reads as "the flash didn't take" and sends you debugging the wrong binary
    (measured the hard way, 2026-07-16). The hash comes from git at generate time and is
    regenerated on every build (the CMake rule is ALWAYS out-of-date by design), so it
    tracks the source, not a compile timestamp.

    Falls back to "nogit" for a tarball / no-git build — never fails the build.
    """
    def git(*args: str) -> str:
        return subprocess.run(("git", "-C", str(ROOT), *args),
                              capture_output=True, text=True, check=True).stdout.strip()
    try:
        h = git("rev-parse", "--short=8", "HEAD")
        dirty = "+" if git("status", "--porcelain") else ""
        return f"{h}{dirty}"
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        return "nogit"


build = build_id()

content = f'''#pragma once

/// @defgroup build_info Compile-time identity
/// @{{
/// Every fact about which binary this is, as the build system knew it.
///
/// @moreinfo
///
/// This header is generated from `library.json` by `moondeck/build/generate_build_info.py`, so an edit belongs in that script rather than here.
///
/// ## What each macro answers
///
/// | Macro | What it names |
/// |-------|---------------|
/// | `MM_VERSION` | the semver, defaulting to `library.json` and overridden by the release pipeline through `compute_version.py` |
/// | `MM_BUILD_DATE` | roughly when the including translation unit was compiled, for a human to read |
/// | `MM_BUILD_ID` | the short git hash the binary was built from, suffixed with `+` when the tree was dirty, or `nogit` without a checkout |
/// | `MM_FIRMWARE_NAME` | the compiled-binary variant, such as `esp32-eth` or `esp32s3-n16r8` |
/// | `MM_RELEASE` | the release channel the binary shipped on, such as `latest` or `v1.0.0` |
///
/// Each carries an `#ifndef` default, so a local build needs no flag.
/// A release `MM_VERSION` is the core semver for a stable tag, or `<core>-dev.<N>` for a moving `latest` build so successive builds are orderable.
///
/// ## Why the build id is the identity, not the date
///
/// `__DATE__` and `__TIME__` expand when the including translation unit compiles, and only that unit's own dependencies trigger a rebuild.
/// Edit a driver `.cpp` and the date does not move, so a freshly flashed board still reports the old timestamp.
/// Two different builds can also carry the same date.
/// A stale date reads as a flash that did not take and sends you debugging the wrong binary.
///
/// The build id comes from git at generate time and is regenerated on every build, the CMake rule being always out of date by design.
/// It therefore names the source rather than a compile moment.
/// Read it off a running device to answer what landed, which has to be answerable before any bench measurement can be trusted.
///
/// ## Firmware against board, and version against release
///
/// A firmware is the compiled-binary variant; the physical board is a separate concept the device cannot identify on its own, which the architecture page covers.
/// `build_esp32.py` passes the firmware name through `firmware_cmake_args`, and `package_desktop.py` does the same for a release desktop build.
/// A local CMake build falls through to `unknown`, since it is never published.
///
/// `MM_VERSION` says what code this is and `MM_RELEASE` says which channel it shipped on.
/// A moving `latest` build and a tagged release can share a semver and differ in channel.
/// A local build has no channel at all, so SystemModule then shows the bare semver.

#ifndef MM_VERSION
#define MM_VERSION    "{version}"
#endif

#define MM_BUILD_DATE __DATE__ " " __TIME__

#ifndef MM_BUILD_ID
#define MM_BUILD_ID   "{build}"
#endif

#ifndef MM_FIRMWARE_NAME
#define MM_FIRMWARE_NAME "unknown"
#endif

#ifndef MM_RELEASE
#define MM_RELEASE ""
#endif

namespace mm {{

/// The semver this binary reports.
constexpr const char* kVersion      = MM_VERSION;
/// Roughly when it was compiled, for a human rather than for identity.
constexpr const char* kBuildDate    = MM_BUILD_DATE;
/// The git hash it was built from, which is the identity signal.
constexpr const char* kBuildId      = MM_BUILD_ID;
/// Which compiled-binary variant this is.
constexpr const char* kFirmwareName = MM_FIRMWARE_NAME;
/// Which release channel it shipped on, empty for a local build.
constexpr const char* kRelease      = MM_RELEASE;

}} // namespace mm

/// @}}
'''

# Force UTF-8 on both read and write — Python's default on Windows is cp1252,
# which can't encode anything outside ASCII. Even though the template above is
# ASCII today, pinning the encoding makes the script robust if a future edit
# slips a non-ASCII character into the comments.
# Written only when run, so build_esp32 imports build_id() for MoonBase's version without regenerating the header.
if __name__ == "__main__":
    if not (OUT_FILE.exists() and OUT_FILE.read_text(encoding="utf-8") == content):   # unchanged: no rebuild
        OUT_FILE.write_text(content, encoding="utf-8")
        print(f"Generated build_info.h: version={version}")
