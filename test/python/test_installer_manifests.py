"""The web installer's per-release files must all be generatable and self-consistent.

The installer fetches, per release, one ESP Web Tools manifest per *shipping* firmware
(`/install/releases/<tag>/manifest-<firmware>.json`), and each manifest names the `.bin`
parts to flash. Those parts MUST be files the release workflow actually stages onto Pages —
the staging step (`.github/workflows/release.yml`) downloads them with these globs:

    firmware-*.bin   shared-ota-data.bin   partition-table-*.bin

A manifest that references a filename outside those globs points at a file that never reaches
Pages → the installer 404s at fetch-firmware. That exact mismatch (a manifest referencing a
file the deploy didn't stage, or a shipping firmware with no manifest at all) shipped a broken
v2.0.0 installer; this test pins the contract so it can't recur:

  1. every `ships: true` firmware in mooninstaller/firmwares.json generates a valid manifest, and
  2. every part path in every manifest matches one of the staged-file globs above.

Self-contained: it feeds generate_manifest.py a synthetic flasher_args.json (the real one only
exists after an ESP-IDF build), so it runs in plain CI with no firmware build.
"""

import fnmatch
import json
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
FIRMWARES_JSON = ROOT / "mooninstaller" / "firmwares.json"
GENERATE = ROOT / "moondeck" / "build" / "generate_manifest.py"

# The exact globs the deploy's `gh release download` stages onto Pages (release.yml,
# "Stage cumulative release content"). A manifest part must match one of these, or the
# installer fetches a file that isn't there. Keep in lockstep with that step's --pattern list.
STAGED_GLOBS = ["firmware-*.bin", "shared-*.bin", "partition-table-*.bin"]
# shared-*.bin covers shared-ota-data.bin plus the MoonBase pair (shared-moonbase-<chip>.bin,
# shared-ota-data-slot0.bin); the same glob appears in release.yml's upload files and the
# Pages self-host download patterns, which is exactly the sync this test guards.

# A minimal but realistic IDF flasher_args.json — the four files every MoonLight build emits.
# generate_manifest.py maps these to the bundle names (firmware-<F>-v<ver>.bin, etc.).
FLASHER_ARGS = {
    "flash_settings": {"flash_size": "4MB"},
    "flash_files": {
        "0x1000": "bootloader/bootloader.bin",
        "0x8000": "partition_table/partition-table.bin",
        "0xe000": "ota_data_initial.bin",
        "0x10000": "projectMM.bin",
    },
}

VERSION = "9.9.9"  # arbitrary; the test asserts the version flows into the filenames


def _shipping_firmwares():
    # encoding='utf-8' — the JSON carries a fs/documentation-note character
    # (byte 0x8f is part of a UTF-8 multi-byte sequence for a non-ASCII glyph
    # in one of the comment fields) and Windows Python 3.14 defaults read_text
    # to cp1252, which fails with UnicodeDecodeError. Same class of Windows-
    # only encoding bug as verify_version's stdout crash.
    data = json.loads(FIRMWARES_JSON.read_text(encoding="utf-8"))
    return [f["name"] for f in data["firmwares"] if f.get("ships")]


def _generate(firmware, tmp_path):
    """Run generate_manifest.py for one firmware with a synthetic flasher_args; return the manifest."""
    flasher = tmp_path / f"flasher-{firmware}.json"
    flasher.write_text(json.dumps(FLASHER_ARGS))
    out = tmp_path / f"manifest-{firmware}.json"
    res = subprocess.run(
        [sys.executable, str(GENERATE),
         "--firmware", firmware,
         "--version", VERSION,
         "--release-url", ".",          # relative paths, as the Pages installer needs
         "--flasher-args", str(flasher),
         "--out", str(out)],
        capture_output=True, text=True,
    )
    assert res.returncode == 0, f"generate_manifest failed for {firmware}: {res.stdout}{res.stderr}"
    assert out.exists(), f"no manifest written for {firmware}"
    return json.loads(out.read_text(encoding="utf-8"))


def test_every_shipping_firmware_has_a_manifest_with_parts(tmp_path):
    firmwares = _shipping_firmwares()
    assert firmwares, "no shipping firmwares in firmwares.json — installer would have nothing to flash"
    for fw in firmwares:
        manifest = _generate(fw, tmp_path)
        assert manifest.get("builds"), f"{fw}: manifest has no builds[]"
        build = manifest["builds"][0]
        assert build.get("chipFamily"), f"{fw}: build missing chipFamily"
        assert build.get("parts"), f"{fw}: build has no parts[] (nothing to flash)"


def test_manifest_parts_match_staged_file_globs(tmp_path):
    """Every part the installer would fetch must be a file the release workflow stages on Pages."""
    for fw in _shipping_firmwares():
        manifest = _generate(fw, tmp_path)
        for part in manifest["builds"][0]["parts"]:
            # relative manifest → path is "./<filename>" (or "<filename>"); take the basename
            name = part["path"].rsplit("/", 1)[-1]
            assert any(fnmatch.fnmatch(name, g) for g in STAGED_GLOBS), (
                f"{fw}: manifest references '{name}', which matches none of the staged globs "
                f"{STAGED_GLOBS} — the installer would 404 fetching it. Either the deploy's "
                f"download patterns or generate_manifest.py's output drifted."
            )


def test_app_and_bootloader_carry_the_version(tmp_path):
    """The per-firmware app + bootloader filenames must embed the firmware key and version, so a
    release dir's files line up with the manifest the same dir serves (the v<ver> stamp is what
    differs per release)."""
    fw = _shipping_firmwares()[0]
    manifest = _generate(fw, tmp_path)
    names = [p["path"].rsplit("/", 1)[-1] for p in manifest["builds"][0]["parts"]]
    assert f"firmware-{fw}-v{VERSION}.bin" in names, f"{fw}: app part not named for firmware+version: {names}"
    assert f"firmware-{fw}-v{VERSION}-bootloader.bin" in names, f"{fw}: bootloader part misnamed: {names}"
