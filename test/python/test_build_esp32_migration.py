"""build_esp32.py contracts for the migration images: which boards one image serves and what it carries.

Every shipping firmware is a migration target. One image per family (the chip, and the P4 split at revision 3) carries,
per flash size and flash type, a partition table and the bootloader of the variant it comes from.

Imports the real dicts from moondeck/build/build_esp32.py (no ESP-IDF needed).
Run: `uv run --with pytest pytest test/python -q`.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "build"))

from build_esp32 import FIRMWARES, family_members, migration_family, migration_tables  # noqa: E402


def families() -> set[str]:
    return {migration_family(k) for k, v in FIRMWARES.items() if v.get("ships")}


def test_every_shipping_firmware_belongs_to_a_family_that_builds_its_image():
    assert families() == {"esp32", "esp32c3", "esp32s3", "esp32s31", "esp32p4", "esp32p4rev3"}
    for fw, spec in FIRMWARES.items():
        if spec.get("ships"):
            assert fw in family_members(migration_family(fw))


def test_the_p4_splits_at_revision_3_because_the_two_images_exclude_each_other():
    assert migration_family("esp32p4rev1-eth") == "esp32p4"
    assert migration_family("esp32p4rev1-eth-wifi") == "esp32p4"
    assert migration_family("esp32p4rev3-eth") == "esp32p4rev3"
    assert migration_family("esp32p4rev3-eth-wifi") == "esp32p4rev3"


def test_the_emulator_is_no_migration_target():
    assert "qemu" not in family_members("esp32")


def test_each_flash_size_and_type_gets_one_table_with_its_variants_bootloader():
    # Where two variants share a size, the table with MoonBase's own slot serves both: N8R2's 8 MB table, not N8R8's two-slot one.
    s3 = [(csv.name, fw) for csv, fw in migration_tables("esp32s3")]
    assert s3 == [("4mb_moonbase.csv", "esp32s3-zero"), ("8mb_moonbase.csv", "esp32s3-n8r2"),
                  ("ota_16mb.csv", "esp32s3-n16r8"), ("ota_32mb.csv", "esp32s3-n32r16")]
    classic = [(csv.name, fw) for csv, fw in migration_tables("esp32")]
    assert classic == [("4mb_moonbase.csv", "esp32"), ("8mb_moonbase.csv", "esp32-pico"), ("ota_16mb_moonbase.csv", "esp32-16mb")]


def test_every_family_carries_at_least_one_table():
    for family in families():
        assert migration_tables(family), family
