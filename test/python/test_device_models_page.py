"""The device models page: the installer's catalog drawn as one table row per board, each with the anchor the device UI links to."""

import json
import re

import pytest

pytest.importorskip("markdown")   # the hooks import it; CI installs it, a bare env skips

import gen_help  # noqa: E402
from mkdocs_hooks import device_models_markdown  # noqa: E402


def _rows(page: str) -> list[str]:
    return [ln for ln in page.split("\n") if ln.startswith("| ") and not ln.startswith(("| Name ", "|---"))]


def test_the_page_has_one_row_per_catalog_entry():
    entries = json.loads(gen_help.DEVICE_MODELS.read_text(encoding="utf-8"))
    assert len(_rows(device_models_markdown("# Device models"))) == len(entries)


def test_every_row_anchor_is_the_slug_of_its_name_and_no_two_are_alike():
    entries = json.loads(gen_help.DEVICE_MODELS.read_text(encoding="utf-8"))
    ids = re.findall(r'<span id="([^"]+)" data-anchor>', device_models_markdown("# Device models"))
    assert sorted(ids) == sorted(gen_help.device_slug(e["name"]) for e in entries)
    assert len(set(ids)) == len(ids)
    assert all(re.fullmatch(r"[a-z0-9]+(-[a-z0-9]+)*", i) for i in ids)


def test_the_slug_collapses_each_run_of_other_characters_to_one_hyphen_and_trims_the_ends():
    assert gen_help.device_slug("ESP32-S3 DevKitC-1") == "esp32-s3-devkitc-1"
    assert gen_help.device_slug("Teensy 4.1-NE + W5500") == "teensy-4-1-ne-w5500"
    assert gen_help.device_slug("  (Board) ") == "board"


def test_the_chip_families_follow_the_page_order():
    assert list(gen_help.device_models())[:12] == ["ESP32", "ESP32-S3", "ESP32-P4", "ESP32-C3", "ESP32-S31", "Teensy 4.1",
                                                  "Raspberry Pi", "Linux", "macOS", "Windows", "Android", "Docker"]
