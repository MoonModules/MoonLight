# Running MoonLight on a Linux machine

MoonLight runs as an ordinary Linux application: the same effect pipeline, web UI and network drivers as on a board, with a real CPU behind them. A small always-on machine makes a good installation controller, whether a server, a Raspberry Pi or a NanoPi.

The facts follow; the steps are MoonWalk's. [Install MoonLight](../moonwalk.md#install) with Linux or Docker picked. [Prepare a Linux board](../moonwalk.md#prepare-linux-board), and [keep MoonLight running](../moonwalk.md#run-linux-board) on it. When a container exits at once: [Start the container on an overlay root](../moonwalk.md#container-overlay-root).

## The machine

- **Two architectures**, each with a package and the container image: `x86_64` (Intel or AMD, which releases name `amd64` or `x64`) and `aarch64` (arm64 boards: Pi, NanoPi, most SBCs). `uname -m` says which. No 32-bit build is published.
- **The package needs glibc 2.35**: Raspberry Pi OS Bookworm, Debian 12 and 13, Ubuntu 22.04 and later. apt refuses a package for the wrong architecture by name rather than installing something that cannot run. On an older system, build from source or run the container.
- **A Pi 4 or 5 has ample headroom.** A NanoPi R28S has two Gigabit ports, so it can sit between the house network and the lighting network, and 1 GB of RAM, which runs MoonLight comfortably.
- **The card is a fine home for it.** MoonLight writes to disk only when settings change; the risk is the operating system's own writes.

## Docker

**Use `--network host`.** The web UI and unicast fixture output work through ordinary port mapping (`-p 8080:8080`). mDNS discovery and Art-Net's broadcast mode are multicast, and neither crosses a bridge network. Host networking is what lets a container find boards and be found by them. Verified on a NanoPi R28S driving a ColorLight card.

`:latest` follows the rolling prerelease, the same build the installer offers; a version tag such as `:6.0.0` pins one. The release workflow builds each image from the `.deb` that release ships, so the image and the package are the same build.

| | |
|---|---|
| **Config** | `/data/MoonLight/.config/` in the volume (`XDG_DATA_HOME=/data`) |
| **Identity** | `/data/MoonLight/.config/identity`, written on the first run |
| **Logs** | standard output, so `docker logs` |
| **Output** | Art-Net UDP 6454, DDP 4048, E1.31 5568, all outbound |
| **Capabilities** | none: it binds its port as an ordinary process |
| **Audio input** | a PulseAudio server: [Set up audio input](../moonwalk.md#audio-input) |

**Several instances** run side by side: each container has its own ports and writes its own identity, so they are told apart on the network. CPU is the limit rather than memory, about one core each, since the desktop build renders as fast as it may; cap it with `cpus:` when running a fleet.

## Package or container: which to pick

| | Package (`.deb`) | Docker |
|---|---|---|
| Updates | download the new `.deb` | `docker pull` and recreate |
| Survives a reboot | needs a systemd unit | `--restart unless-stopped` |
| Runs on | Debian family, glibc 2.35+ | any Linux with Docker |
| Disk | the binary plus libraries the system already has | an image carrying its own libraries |
| Isolation | none: an ordinary program | its own filesystem and process space |
| Debugging | logs in the terminal, files on disk | `docker logs`, and no shell inside the image |

**Take the package** on a board you own and administer: it is smaller, it starts faster, and its files are where you expect them.

**Take Docker** where you would rather not install anything permanent, where the machine already runs containers, or where the distribution is too old for the package's glibc floor. A container carries its own libraries, so the host's age stops mattering.

## Where else this runs

Docker widens the target from "a Debian-family board" to "anything that runs containers":

- **Raspberry Pi** (3, 4, 5, and Zero 2 W): arm64, so the same image and the same commands as a NanoPi.
- **A NAS**: Synology (Container Manager), QNAP (Container Station), Unraid, TrueNAS. Give it host networking, or mDNS and Art-Net broadcast will not leave the box.
- **A mini PC or home server**: amd64, the largest and least surprising target.
- **A router or firewall appliance** running OpenWrt or OPNsense with Docker: plausible where the device has the RAM, and its root filesystem is likely an overlay.
- **Kubernetes**: one pod, one volume. Multicast needs an L2 CNI rather than the default bridge.

The realistic limits are architecture and memory, not the kind of device: 64-bit, arm64 or amd64. Memory is rarely the binding constraint: a container rendering a 16x16 grid measures 4.5 MB on a NanoPi. A large installation grows that by its buffers rather than by a fixed overhead.
