#!/usr/bin/env python3
# /// script
# dependencies = []
# ///
"""The release pipeline's build and staging steps, one subcommand each, so a release stages locally exactly as CI does.

release.yml calls these; publishing itself (the release upload, the container registry, Pages) stays GitHub's own.
A step's outputs go to $GITHUB_OUTPUT when it is set, and always to stdout as key=value.

    tag            the release channel tag: the dispatch input, else `latest` on main, else the ref name
    version        the semver a tag builds as (compute_version.py)
    firmwares      the ESP32 build matrix, as JSON
    gcc-version    the Arm toolchain version that keys the Teensy toolchain cache
    stage-esp32    one firmware's build into dist/ under the release names
    stage-teensy   build and stage every shipping Teensy firmware
    gather         flatten the downloaded job artifacts into dist/
    extras         the control-surface session and the module help
    manifests      the installer manifest of every shipping firmware
    image-name     the lowercase container image path
    container-context   the Docker build context holding the release .debs
    smoke-images   run each container image before anything is published
    pages-site     the installer and its assets, staged for Pages
    merge-docs     the rendered docs site, copied into the Pages root beside the installer

    uv run moondeck/ci/release_stage.py tag --ref refs/heads/main --ref-name main
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DIST = ROOT / "dist"
FIRMWARES_JSON = ROOT / "mooninstaller" / "firmwares.json"
SETUP_TEENSY = ROOT / "moondeck" / "build" / "setup_teensy.py"
sys.path.insert(0, str(ROOT / "moondeck" / "build"))

# The release image's build context file, written by container-context: one .deb per architecture, then the runtime stage.
DOCKERFILE = """\
# --platform=$TARGETPLATFORM: the unpack must happen on the TARGET's architecture, or
# buildx would run the amd64 stage for both and ship an amd64 binary inside an arm64
# image. ARG TARGETARCH is set by BuildKit, no --build-arg needed.
FROM --platform=$TARGETPLATFORM debian:trixie-slim AS fetch
ARG TARGETARCH
COPY moonlight_$TARGETARCH.deb /tmp/moonlight.deb
# libcurl4t64 is installed because the binary LINKS it (the one outbound HTTPS call), and
# libpulse0 because its audio input LOADS it at runtime, which ldd cannot see; the sweep
# then collects both plus everything they need. See ../../Dockerfile.
RUN apt-get update \\
 && apt-get install -y --no-install-recommends libcurl4t64 libpulse0 \\
 && rm -rf /var/lib/apt/lists/* \\
 && dpkg-deb -x /tmp/moonlight.deb /rootfs \\
 && mkdir -p /deps \\
 && pulse=$(find /usr/lib -name libpulse.so.0 | head -1) \\
 && test -n "$pulse" \\
 && { ldd /rootfs/usr/bin/MoonLight; ldd "$pulse"; echo "libpulse.so.0 => $pulse (0)"; } | awk '/=> \\//{print $3}' | sort -u | grep -vE '/(libc|libm|libstdc\\+\\+|libgcc_s)\\.so' | xargs -I{} cp -L {} /deps/
# Both binaries are built on Ubuntu 22.04 and need glibc 2.35 at most, which
# check_glibc_floor.py enforces, so debian13 (2.41) runs them. See ../../Dockerfile.
# Pinned by digest, not by tag: `cc-debian13` is mutable, so an unpinned base means two
# builds of the same commit can ship different runtimes. Re-pin deliberately when picking
# up base updates (docker buildx imagetools inspect gcr.io/distroless/cc-debian13:latest).
# This digest is the multi-arch INDEX, not one platform's manifest, so the same pin serves
# amd64 and arm64; buildx resolves the right manifest per platform.
FROM gcr.io/distroless/cc-debian13@sha256:9b615fff20e1a4fad29c2b30562580b212c7dd5e2225236735cca0070ed11c78
COPY --from=fetch /rootfs/usr/bin/MoonLight /usr/bin/MoonLight
# The libraries the loader resolved, collected in the fetch stage. Without them the
# container dies at startup on "libcurl.so.4: cannot open shared object file", which the
# run before publishing now catches.
COPY --from=fetch /deps/ /usr/lib/
ENV XDG_DATA_HOME=/data
VOLUME /data
EXPOSE 8080
ENTRYPOINT ["/usr/bin/MoonLight"]
"""
ARCHITECTURES = ("amd64", "arm64")


def emit(key: str, value: str) -> None:
    """One step output: appended to $GITHUB_OUTPUT when GitHub provides it, always printed."""
    print(f"{key}={value}")
    target = os.environ.get("GITHUB_OUTPUT")
    if target:
        with open(target, "a", encoding="utf-8") as out:
            out.write(f"{key}={value}\n")


def resolve_tag(input_tag: str, ref: str, ref_name: str) -> str:
    """The release channel tag: the dispatch input wins, a push to main is `latest`, any other ref is its own name."""
    if input_tag:
        return input_tag
    return "latest" if ref == "refs/heads/main" else ref_name


def shipping(registry: dict, keep) -> list[str]:
    """The names of the shipping firmwares whose platform `keep` accepts, in registry order."""
    return [f["name"] for f in registry["firmwares"] if f["ships"] and keep(f["platform"])]


def load_registry() -> dict:
    return json.loads(FIRMWARES_JSON.read_text(encoding="utf-8"))


def esp32_matrix(registry: dict) -> str:
    """The build matrix of the shipping ESP32 firmwares as compact JSON: the firmware name and its IDF target chip."""
    rows = [{"firmware": f["name"], "chip": f["chip"]} for f in registry["firmwares"] if f["ships"] and f["platform"] == "esp32"]
    return json.dumps(rows, separators=(",", ":"), ensure_ascii=False)


def chip_of(build_dir: Path, prefix: str) -> str:
    """The chip a shared image's build directory names: build/moonbase-<chip> -> <chip>."""
    return build_dir.name[len(prefix):]


def migrate_chips(images: list[Path]) -> list[tuple[Path, str]]:
    """The migration images that ship, with their chip: the `-pause` rehearsal image never does."""
    chips = [(image, chip_of(image.parent, "migrate-")) for image in images]
    return [(image, chip) for image, chip in chips if not chip.endswith("-pause")]


def esp32_stage_plan(firmware: str, version: str) -> list[tuple[str, str]]:
    """Each (build output relative to its build dir, dist file name) one firmware's staging copies."""
    prefix = f"firmware-{firmware}-v{version}"
    return [("MoonLight.bin", f"{prefix}.bin"),
            ("bootloader/bootloader.bin", f"{prefix}-bootloader.bin"),
            # App, bootloader and partition table are unique per firmware (one flash size holds several layouts), so staged with the prefix.
            ("partition_table/partition-table.bin", f"{prefix}-partition-table.bin"),
            # ota-data is byte-identical for ALL firmwares, so it is staged under a SHARED name and the release uploads it once.
            ("ota_data_initial.bin", "shared-ota-data.bin"),
            # Per-firmware flasher_args.json: the manifests take their offsets from the real build.
            ("flasher_args.json", f"flasher-{firmware}.json")]


def stage_esp32(firmware: str, version: str) -> None:
    DIST.mkdir(exist_ok=True)
    build = ROOT / "build" / f"esp32-{firmware}"
    for source, name in esp32_stage_plan(firmware, version):
        shutil.copy(build / source, DIST / name)
    # MoonBase, the second boot image on the 4 MB tables: chip-shared. The slot-0 otadata makes a fresh install boot the APP with MoonBase standing by; blank otadata would boot MoonBase.
    moonbase = sorted((ROOT / "build").glob("moonbase-*/MoonLight-moonbase.bin"))
    for image in moonbase:
        shutil.copy(image, DIST / f"shared-moonbase-{chip_of(image.parent, 'moonbase-')}.bin")
    if moonbase:
        import build_esp32
        (DIST / "shared-ota-data-slot0.bin").write_bytes(build_esp32.otadata_slot0_bytes())
    # The migration image a board running WLED installs through WLED's own update page (moonbase/migrate/): one per chip, shared like MoonBase.
    for image, chip in migrate_chips(sorted((ROOT / "build").glob("migrate-*/MoonLight-migrate.bin"))):
        shutil.copy(image, DIST / f"shared-migrate-{chip}.bin")


def stage_teensy(tag: str, version: str) -> None:
    DIST.mkdir(exist_ok=True)
    for firmware in shipping(load_registry(), lambda platform: platform == "teensy"):
        subprocess.run(["uv", "run", str(ROOT / "moondeck" / "build" / "build_teensy.py"),
                        "--firmware", firmware, "--release", tag, "--version", version], cwd=ROOT, check=True)
        for extension in ("hex", "bin"):
            shutil.copy(ROOT / "build" / firmware / f"MoonLight.{extension}", DIST / f"firmware-{firmware}-v{version}.{extension}")


def list_dist() -> None:
    for path in sorted(DIST.iterdir()):
        print(f"{path.stat().st_size:>12}  {path.name}")


def gather() -> None:
    """Every file under artifacts/ moves to dist/ by its name; the byte-identical shared files collapse to one copy."""
    DIST.mkdir(exist_ok=True)
    for path in sorted(p for p in (ROOT / "artifacts").rglob("*") if p.is_file()):
        os.replace(path, DIST / path.name)
    list_dist()


def extras() -> None:
    """The Open Stage Control session and the module help, copied beside the binaries, not generated."""
    DIST.mkdir(exist_ok=True)
    shutil.copy(ROOT / "docs" / "reference" / "examples" / "open-stage-control.json", DIST / "MoonLight-control-surface.json")
    shutil.copy(ROOT / "docs" / "help" / "help.json", DIST / "MoonLight-help.json")


def manifest_command(firmware: str, version: str, base_url: str, dist: Path = DIST) -> list[str]:
    """The generate_manifest.py call for one firmware: an ESP32 manifest takes its offsets from IDF's flasher args, a Teensy one is the flat image at 0."""
    args = ["--firmware", firmware, "--version", version, "--release-url", base_url]
    flasher_args = dist / f"flasher-{firmware}.json"
    if flasher_args.is_file():
        args += ["--flasher-args", str(flasher_args)]
    return [sys.executable, str(ROOT / "moondeck" / "build" / "generate_manifest.py"), *args, "--out", str(dist / f"manifest-{firmware}.json")]


def manifests(tag: str, version: str, repo: str) -> None:
    """Absolute GitHub release-asset URLs, read by the on-device OTA picker; the Pages-relative ones are made at deploy."""
    base_url = f"https://github.com/{repo}/releases/download/{tag}"
    for firmware in shipping(load_registry(), lambda platform: platform != "desktop"):
        subprocess.run(manifest_command(firmware, version, base_url), cwd=ROOT, check=True)


def image_name(repo: str) -> str:
    """The registry path, lowercase because a registry path must be and the org is not."""
    return f"ghcr.io/{repo.lower()}"


def container_context() -> None:
    """A build context holding only the .debs, so the image installs THIS run's package, named by the architecture BuildKit asks for."""
    context = ROOT / "ctx"
    context.mkdir(exist_ok=True)
    for arch in ARCHITECTURES:
        debs = sorted(DIST.glob(f"moonlight_*_{arch}.deb"))
        if not debs:
            raise SystemExit(f"no moonlight_*_{arch}.deb in {DIST}")
        shutil.copy(debs[0], context / f"moonlight_{arch}.deb")
    (context / "Dockerfile").write_text(DOCKERFILE, encoding="utf-8")
    for path in sorted(context.iterdir()):
        print(f"{path.stat().st_size:>12}  {path.name}")


def docker(*args: str, check: bool = True, capture: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(["docker", *args], check=check, capture_output=capture, text=True)


def tail(text: str, lines: int = 20) -> str:
    return "\n".join(text.splitlines()[-lines:])


def smoke_images() -> None:
    """amd64 runs natively and must answer its API; arm64 runs under QEMU, where an emulated render loop makes a response time a matter of the runner, so it must survive its start."""
    docker("run", "-d", "--name", "smoke-amd64", "--platform", "linux/amd64", "-p", "18080:8080", "moonlight-smoke:amd64")
    answered = False
    for _ in range(60):
        try:
            with urllib.request.urlopen("http://localhost:18080/api/system", timeout=5):
                answered = True
                break
        except OSError:
            time.sleep(1)
    logs = docker("logs", "smoke-amd64", capture=True)
    print(tail(logs.stdout + logs.stderr))
    docker("rm", "-f", "smoke-amd64")
    if not answered:
        raise SystemExit("the amd64 image did not answer within 60 s")
    print("the amd64 image answers")
    docker("run", "-d", "--name", "smoke-arm64", "--platform", "linux/arm64", "moonlight-smoke:arm64")
    time.sleep(15)
    running = docker("inspect", "-f", "{{.State.Running}}", "smoke-arm64", capture=True).stdout.strip()
    logs = docker("logs", "smoke-arm64", capture=True)
    log = tail(logs.stdout + logs.stderr)
    print(log)
    Path("arm64.log").write_text(log + "\n", encoding="utf-8")
    docker("rm", "-f", "smoke-arm64")
    if running != "true" or re.search(r"GLIBC_|cannot open shared object", log):
        raise SystemExit("the arm64 image did not survive its start")
    print("the arm64 image runs")


def pages_site() -> None:
    """The installer page, the picker modules and the board images a deviceModels.json entry references, under pages/install/."""
    install = ROOT / "pages" / "install"
    install.mkdir(parents=True, exist_ok=True)
    shutil.copytree(ROOT / "mooninstaller", install, dirs_exist_ok=True)
    shutil.copy(ROOT / "src" / "ui" / "install-picker.js", install)
    # The device-catalog / chip-detection half of the picker, imported by index.html: it must ship beside install-picker.js or the ES-module import 404s.
    shutil.copy(ROOT / "src" / "ui" / "install-picker-devices.js", install)
    shutil.copy(ROOT / "library.json", install)   # the install page reads the project version from it
    # Only the images a deviceModels.json entry references, so "assets/deviceModels/<slug>.jpg" resolves same-origin from /install/ without shipping the unused library.
    (install / "assets" / "deviceModels").mkdir(parents=True, exist_ok=True)
    for entry in json.loads((ROOT / "mooninstaller" / "deviceModels.json").read_text(encoding="utf-8")):
        relative = entry.get("image")
        if not relative:
            continue
        source = ROOT / "docs" / relative
        if source.is_file():
            shutil.copy(source, install / relative)
        else:
            print(f"WARNING: deviceModels.json image not found: {source}")
    for directory in (ROOT / "pages", install):
        for path in sorted(directory.iterdir()):
            print(path.name)


def merge_docs(site_dir: Path) -> None:
    """Copy the rendered docs site INTO pages/, so the installer already staged under pages/install/ survives."""
    shutil.copytree(site_dir, ROOT / "pages", dirs_exist_ok=True)
    for path in sorted((ROOT / "pages").iterdir()):
        print(path.name)


def gcc_version() -> str:
    """The Arm toolchain version setup_teensy.py pins, which keys the toolchain cache."""
    found = re.search(r'^GCC_VERSION = "(.*)"', SETUP_TEENSY.read_text(encoding="utf-8"), re.MULTILINE)
    if not found:
        raise SystemExit(f"no GCC_VERSION in {SETUP_TEENSY.relative_to(ROOT)}: the Teensy toolchain cache needs its key")
    return found.group(1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    tag = sub.add_parser("tag", help="Emit tag=<release channel tag>.")
    tag.add_argument("--input-tag", default="", help="The workflow_dispatch tag, empty on a push.")
    tag.add_argument("--ref", required=True, help="The full ref, e.g. refs/heads/main.")
    tag.add_argument("--ref-name", required=True, help="The short ref name.")
    version = sub.add_parser("version", help="Emit version=<semver> for a tag.")
    version.add_argument("--tag", required=True)
    sub.add_parser("firmwares", help="Emit list=<ESP32 build matrix JSON>.")
    sub.add_parser("gcc-version", help="Emit version=<Arm toolchain version>.")
    esp32 = sub.add_parser("stage-esp32", help="Copy one ESP32 firmware's build into dist/.")
    esp32.add_argument("--firmware", required=True)
    esp32.add_argument("--version", required=True)
    teensy = sub.add_parser("stage-teensy", help="Build every shipping Teensy firmware into dist/.")
    teensy.add_argument("--tag", required=True)
    teensy.add_argument("--version", required=True)
    sub.add_parser("gather", help="Flatten artifacts/ into dist/.")
    sub.add_parser("extras", help="Copy the control-surface session and the module help into dist/.")
    manifest = sub.add_parser("manifests", help="Write dist/manifest-<firmware>.json for every shipping firmware.")
    manifest.add_argument("--tag", required=True)
    manifest.add_argument("--version", required=True)
    manifest.add_argument("--repo", required=True, help="owner/name")
    image = sub.add_parser("image-name", help="Emit name=<lowercase image path>.")
    image.add_argument("--repo", required=True, help="owner/name")
    sub.add_parser("container-context", help="Write ctx/ with the .debs and the release Dockerfile.")
    sub.add_parser("smoke-images", help="Run the amd64 and arm64 smoke images.")
    sub.add_parser("pages-site", help="Stage the installer under pages/install/.")
    docs = sub.add_parser("merge-docs", help="Copy the rendered docs site into pages/.")
    docs.add_argument("--site-dir", required=True, type=Path)
    args = parser.parse_args()
    if args.command == "tag":
        emit("tag", resolve_tag(args.input_tag, args.ref, args.ref_name))
    elif args.command == "version":
        import compute_version
        emit("version", compute_version.compute(compute_version.channel_for_tag(args.tag), args.tag))
    elif args.command == "firmwares":
        emit("list", esp32_matrix(load_registry()))
    elif args.command == "gcc-version":
        emit("version", gcc_version())
    elif args.command == "stage-esp32":
        stage_esp32(args.firmware, args.version)
    elif args.command == "stage-teensy":
        stage_teensy(args.tag, args.version)
    elif args.command == "gather":
        gather()
    elif args.command == "extras":
        extras()
    elif args.command == "manifests":
        manifests(args.tag, args.version, args.repo)
    elif args.command == "image-name":
        emit("name", image_name(args.repo))
    elif args.command == "container-context":
        container_context()
    elif args.command == "smoke-images":
        smoke_images()
    elif args.command == "pages-site":
        pages_site()
    else:
        merge_docs(args.site_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
