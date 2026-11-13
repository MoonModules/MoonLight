# /// script
# dependencies = ["pytest"]
# ///
"""The .deb's layout: the program as a seed under /usr/lib/moonlight, the service and its rollback, the maintainer scripts that install them, and a menu entry opening the page.
Read from the staged tree at the moment dpkg-deb would pack it, so it runs on any machine."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / "ci"))
import package_desktop as pd  # noqa: E402


def staged(tmp_path, monkeypatch):
    """The files package_deb stages, relative to the package root, with their modes."""
    binary = tmp_path / "MoonLight"
    binary.write_text("program")
    monkeypatch.setattr(pd, "DIST_DIR", tmp_path / "dist")
    monkeypatch.setattr(pd.shutil, "which", lambda name: "/usr/bin/dpkg-deb")
    seen = {}

    def pack(cmd, *args, **kwargs):
        stage = Path(cmd[-2])
        seen.update({p.relative_to(stage).as_posix(): p.stat().st_mode & 0o777 for p in stage.rglob("*") if p.is_file()})
        seen["_desktop"] = (stage / "usr/share/applications/moonlight.desktop").read_text()

    monkeypatch.setattr(pd, "run", pack)
    pd.package_deb(binary, "6.2.0-dev.3")
    return seen


def test_the_program_is_a_seed_the_service_copies_into_its_own_folder(tmp_path, monkeypatch):
    files = staged(tmp_path, monkeypatch)
    assert "usr/lib/moonlight/MoonLight" in files
    assert "usr/bin/MoonLight" not in files


def test_the_package_carries_the_service_its_rollback_and_the_scripts_that_install_them(tmp_path, monkeypatch):
    files = staged(tmp_path, monkeypatch)
    for unit in ("usr/lib/systemd/system/moonlight.service", "usr/lib/systemd/system/moonlight-rollback.service"):
        assert unit in files
    for script in ("DEBIAN/postinst", "DEBIAN/prerm", "DEBIAN/postrm", "usr/lib/moonlight/rollback"):
        assert files[script] == 0o755, script


def test_the_menu_entry_opens_the_running_service_page(tmp_path, monkeypatch):
    desktop = staged(tmp_path, monkeypatch)["_desktop"]
    assert "Exec=xdg-open http://localhost:8080/" in desktop
    assert "Terminal=false" in desktop


def test_the_service_runs_the_copy_its_user_owns_and_rolls_back_through_systemd():
    unit = (pd.LINUX_DIR / "moonlight.service").read_text()
    assert "ExecStart=/var/lib/moonlight/bin/MoonLight" in unit
    assert "User=moonlight" in unit
    assert "Restart=always" in unit
    assert "OnFailure=moonlight-rollback.service" in unit
    for confinement in ("NoNewPrivileges=yes", "ProtectSystem=strict", "ReadWritePaths=/var/lib/moonlight"):
        assert confinement in unit, confinement
    # One capability, the raw frames a ColorLight card takes, granted and bounded alike.
    assert "AmbientCapabilities=CAP_NET_RAW\n" in unit and "CapabilityBoundingSet=CAP_NET_RAW\n" in unit
    postinst = (pd.LINUX_DIR / "postinst").read_text()
    assert "/usr/lib/moonlight/MoonLight" in postinst and "/var/lib/moonlight/bin" in postinst
