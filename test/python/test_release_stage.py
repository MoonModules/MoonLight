# /// script
# dependencies = ["pytest"]
# ///
"""release_stage tests: the release pipeline's tag, file names and firmware selection, pinned so a CI step and a local run stage the same release."""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / "ci"))
import release_stage as rs  # noqa: E402

REGISTRY = {"firmwares": [
    {"name": "esp32", "platform": "esp32", "chip": "esp32", "ships": True},
    {"name": "esp32s3-n16r8", "platform": "esp32", "chip": "esp32s3", "ships": True},
    {"name": "esp32-dev", "platform": "esp32", "chip": "esp32", "ships": False},
    {"name": "teensy41", "platform": "teensy", "chip": "imxrt1062", "ships": True},
    {"name": "desktop-macos-arm64", "platform": "desktop", "chip": "macos-arm64", "ships": True},
]}


def test_a_dispatch_input_tag_wins_over_the_ref():
    assert rs.resolve_tag("v7.0.1", "refs/heads/main", "main") == "v7.0.1"


def test_a_push_to_main_releases_as_latest():
    assert rs.resolve_tag("", "refs/heads/main", "main") == "latest"


def test_a_version_tag_push_releases_under_its_own_name():
    assert rs.resolve_tag("", "refs/tags/v7.1.0-rc1", "v7.1.0-rc1") == "v7.1.0-rc1"


def test_a_branch_other_than_main_releases_under_its_ref_name():
    assert rs.resolve_tag("", "refs/heads/next-iteration", "next-iteration") == "next-iteration"


def test_the_esp32_matrix_lists_only_shipping_esp32_firmwares_as_compact_json():
    assert rs.esp32_matrix(REGISTRY) == '[{"firmware":"esp32","chip":"esp32"},{"firmware":"esp32s3-n16r8","chip":"esp32s3"}]'


def test_manifests_cover_every_shipping_firmware_except_desktop():
    assert rs.shipping(REGISTRY, lambda platform: platform != "desktop") == ["esp32", "esp32s3-n16r8", "teensy41"]


def test_the_teensy_build_covers_only_shipping_teensy_firmwares():
    assert rs.shipping(REGISTRY, lambda platform: platform == "teensy") == ["teensy41"]


def test_an_esp32_firmware_stages_under_the_release_names():
    assert rs.esp32_stage_plan("esp32", "7.0.0") == [
        ("MoonLight.bin", "firmware-esp32-v7.0.0.bin"),
        ("bootloader/bootloader.bin", "firmware-esp32-v7.0.0-bootloader.bin"),
        ("partition_table/partition-table.bin", "firmware-esp32-v7.0.0-partition-table.bin"),
        ("ota_data_initial.bin", "shared-ota-data.bin"),
        ("flasher_args.json", "flasher-esp32.json")]


def test_the_pause_rehearsal_migrate_image_never_ships():
    images = [Path("build/migrate-esp32/MoonLight-migrate.bin"), Path("build/migrate-esp32-pause/MoonLight-migrate.bin"),
              Path("build/migrate-esp32s3/MoonLight-migrate.bin")]
    assert [chip for _, chip in rs.migrate_chips(images)] == ["esp32", "esp32s3"]


def test_staging_a_firmware_copies_its_files_and_the_shared_images_once_per_chip(tmp_path, monkeypatch):
    monkeypatch.setattr(rs, "ROOT", tmp_path)
    monkeypatch.setattr(rs, "DIST", tmp_path / "dist")
    build = tmp_path / "build"
    for source, _ in rs.esp32_stage_plan("esp32", "1.0.0"):
        (build / "esp32-esp32" / source).parent.mkdir(parents=True, exist_ok=True)
        (build / "esp32-esp32" / source).write_bytes(source.encode())
    for image in ("moonbase-esp32/MoonLight-moonbase.bin", "migrate-esp32/MoonLight-migrate.bin", "migrate-esp32-pause/MoonLight-migrate.bin"):
        (build / image).parent.mkdir(parents=True)
        (build / image).write_bytes(b"x")
    rs.stage_esp32("esp32", "1.0.0")
    assert sorted(p.name for p in (tmp_path / "dist").iterdir()) == sorted([
        "firmware-esp32-v1.0.0-bootloader.bin", "firmware-esp32-v1.0.0-partition-table.bin", "firmware-esp32-v1.0.0.bin",
        "flasher-esp32.json", "shared-moonbase-esp32.bin", "shared-migrate-esp32.bin", "shared-ota-data-slot0.bin", "shared-ota-data.bin"])


def test_an_esp32_manifest_takes_its_offsets_from_the_flasher_args_when_the_build_staged_them(tmp_path):
    (tmp_path / "flasher-esp32.json").write_text("{}")
    command = rs.manifest_command("esp32", "7.0.0", "https://x/y", tmp_path)
    assert command[2:] == ["--firmware", "esp32", "--version", "7.0.0", "--release-url", "https://x/y",
                           "--flasher-args", str(tmp_path / "flasher-esp32.json"), "--out", str(tmp_path / "manifest-esp32.json")]


def test_a_teensy_manifest_is_the_flat_image_without_flasher_args(tmp_path):
    assert "--flasher-args" not in rs.manifest_command("teensy41", "7.0.0", "https://x/y", tmp_path)


def test_the_image_path_is_lowercase_because_a_registry_path_must_be():
    assert rs.image_name("MoonModules/MoonLight") == "ghcr.io/moonmodules/moonlight"


def test_a_step_output_is_appended_to_the_github_output_file(tmp_path, monkeypatch, capsys):
    out = tmp_path / "out"
    monkeypatch.setenv("GITHUB_OUTPUT", str(out))
    rs.emit("tag", "latest")
    assert out.read_text() == "tag=latest\n"
    assert capsys.readouterr().out == "tag=latest\n"


def test_a_step_output_prints_alone_outside_github(monkeypatch, capsys):
    monkeypatch.delenv("GITHUB_OUTPUT", raising=False)
    rs.emit("version", "7.0.0")
    assert capsys.readouterr().out == "version=7.0.0\n"


def test_the_teensy_toolchain_cache_key_reads_the_pinned_gcc_version():
    assert rs.gcc_version() == "15.3.rel2"


def test_gathering_moves_every_artifact_file_flat_into_dist(tmp_path, monkeypatch):
    monkeypatch.setattr(rs, "ROOT", tmp_path)
    monkeypatch.setattr(rs, "DIST", tmp_path / "dist")
    for job, name in (("esp32-a", "firmware-a.bin"), ("esp32-b", "shared-ota-data.bin"), ("esp32-a", "shared-ota-data.bin")):
        (tmp_path / "artifacts" / job).mkdir(parents=True, exist_ok=True)
        (tmp_path / "artifacts" / job / name).write_bytes(b"1")
    rs.gather()
    assert sorted(p.name for p in (tmp_path / "dist").iterdir()) == ["firmware-a.bin", "shared-ota-data.bin"]


def test_the_release_dockerfile_selects_its_package_by_target_architecture():
    assert "COPY moonlight_$TARGETARCH.deb" in rs.DOCKERFILE and rs.DOCKERFILE.endswith('ENTRYPOINT ["/usr/bin/MoonLight"]\n')
    assert json.dumps(rs.ARCHITECTURES) == '["amd64", "arm64"]'
