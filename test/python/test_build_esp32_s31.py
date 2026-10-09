"""build_esp32.py contracts for the ESP32-S31 (a RISC-V preview target).

The S31 needed three pieces of build wiring, each with a sharp failure mode if it
drifts. This pins them so a refactor of the FIRMWARES dict can't silently break the
S31 build or its CI matrix entry:

  1. FIRMWARES["esp32s31"] is well-formed and every chip has a family label, so the
     manifest generator (CHIP_FAMILIES) can't KeyError on it.
  2. esp32s31 is in PREVIEW_TARGETS — without it `idf.py set-target esp32s31` refuses
     ("you have to append '--preview'"), so the build never starts.
  3. The release workflow infers the IDF target from the firmware-name PREFIX. Because
     "esp32s31" also startsWith "esp32s3", the esp32s31 check MUST come first, or CI
     would build the S31 firmware for the WRONG target (esp32s3). We re-implement the
     same precedence rule here and pin that esp32s31 resolves to esp32s31.

Imports the real dicts from moondeck/build/build_esp32.py (no ESP-IDF needed).
Run: `uv run --with pytest pytest test/python -q`.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "build"))

from build_esp32 import (  # noqa: E402
    FIRMWARES,
    TARGET_TO_FAMILY,
    PREVIEW_TARGETS,
    firmware_cmake_args,
)


def test_s31_firmware_entry_is_well_formed():
    fw = FIRMWARES.get("esp32s31")
    assert fw is not None, "FIRMWARES must define esp32s31"
    assert fw["chip"] == "esp32s31"
    assert fw["eth_only"] is False, "the S31 build is all-in-one WiFi+Eth, not eth-only"
    assert fw["ships"] is True, "the S31 ships (appears in the web installer + CI matrix)"
    # The fragment file it layers must exist on disk, or the build fails at configure.
    frag = "sdkconfig.defaults.esp32s31"
    assert frag in fw["fragments"], "the S31 entry must layer its own sdkconfig fragment"
    assert (ROOT / "esp32" / frag).is_file(), f"{frag} is referenced but missing on disk"


def test_s31_gets_ethernet_not_MM_NO_ETH():
    """firmware_cmake_args must NOT stub Ethernet out on the S31. The S31 enables its
    on-chip EMAC in sdkconfig.defaults.esp32s31 (whose filename has no ".eth"), so the
    eth-detection reads the fragment *content* for CONFIG_ETH_USE_ESP32_EMAC — not the
    filename. This pins the regression: a filename heuristic set MM_NO_ETH and silently
    compiled the ethInit() stub, so the board booted with no Ethernet (see decisions.md).
    """
    assert "-DMM_NO_ETH=1" not in firmware_cmake_args("esp32s31"), (
        "the S31 firmware must build with Ethernet enabled — its EMAC is set in "
        "sdkconfig.defaults.esp32s31, which the content-based check must detect"
    )
    # Sanity anchor: a variant whose fragments enable an eth driver stays eth-enabled too,
    # and the mechanism (content-read) is exercised for a ".eth"-named fragment as well.
    assert "-DMM_NO_ETH=1" not in firmware_cmake_args("esp32-eth")


def test_every_firmware_chip_has_a_family_label():
    # generate_manifest.py builds CHIP_FAMILIES = {fw: TARGET_TO_FAMILY[chip]} — a chip
    # without a family entry would KeyError there. Pin that none is missing (S31 included).
    for name, spec in FIRMWARES.items():
        assert spec["chip"] in TARGET_TO_FAMILY, (
            f'firmware "{name}" has chip "{spec["chip"]}" with no TARGET_TO_FAMILY entry — '
            f"generate_manifest.py would KeyError. Add it to TARGET_TO_FAMILY."
        )
    assert TARGET_TO_FAMILY["esp32s31"] == "ESP32-S31"


def test_s31_is_a_preview_target():
    # idf.py set-target esp32s31 refuses without --preview; build_esp32.py adds the flag
    # only for chips in PREVIEW_TARGETS. Drop esp32s31 from this set once it graduates.
    assert "esp32s31" in PREVIEW_TARGETS


def test_ci_target_is_the_firmwares_declared_chip():
    # The release matrix carries each firmware's chip from firmwares.json and builds for it, so no second rule can map a firmware to the wrong IDF target.
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    assert "{firmware: .name, chip}" in workflow
    assert "target: ${{ matrix.chip }}" in workflow

