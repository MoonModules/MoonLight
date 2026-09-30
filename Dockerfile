# MoonLight as a container: the desktop firmware, which is the whole system without an ESP32.
#
# The desktop build is not a simulator. It runs the same effect pipeline, the same web UI and the
# same driver stack as a board, and it drives real fixtures over Art-Net, DDP and E1.31, so a
# container is a complete live installation for anyone whose fixtures are on the network.
#
# It INSTALLS the released .deb rather than building from source, deliberately. The release already
# produces that package; building here would be a second build path to keep working, and the two
# would drift. The image is packaging, not a build.
#
#   docker build -t moonlight .                              # the rolling prerelease (default)
#   docker build --build-arg RELEASE=stable -t moonlight .   # the newest stable release
#   docker build --build-arg RELEASE=v4.0.0 -t moonlight .   # a specific tag
#   docker run --rm -p 8080:8080 -v moonlight:/data moonlight
#
# Then open http://localhost:8080/.
#
# **Ports.** 8080 is the web UI, and the only port needed to try it out. Driving fixtures is
# OUTBOUND: Art-Net on UDP 6454, DDP on 4048, E1.31/sACN on 5568. Those are L3 and reach a unicast
# fixture address through ordinary bridge networking.
#
# **When L2 matters.** mDNS discovery (finding boards, being found by them) is multicast and does
# not cross a bridge network, and Art-Net's broadcast mode has the same problem. For those, attach
# the container to the host's network directly (`--network host`, or an L2 CNI on Kubernetes).
# Unicast output needs none of it. Verified on a NanoPi R28S (arm64, Debian 13): the container
# serves its UI and reaches the LAN through `--network host`. On macOS and Windows the question
# cannot be answered, because Docker Desktop runs a Linux VM and host networking joins the VM
# rather than the machine's LAN.
#
# **Capabilities.** None. It binds 8080 as an ordinary process and needs no added capability.
#
# **amd64 and arm64.** The release builds a .deb for each, so `docker build` on an arm64 host
# (a Pi, a NanoPi, an Apple-silicon Mac running Docker Desktop) fetches the arm64 package by
# itself: the stage below resolves the asset from the architecture it is building for.

# --- stage 1: fetch the release and unpack it -------------------------------------------------
# A full Debian image, used only to resolve and extract the .deb. None of it reaches the result.
FROM debian:trixie-slim AS fetch

# BuildKit sets TARGETARCH to amd64 or arm64, the same two suffixes the release writes, so the
# asset is selected by the architecture being built rather than hardcoded. A plain `docker build`
# on any host therefore produces an image that runs there.
ARG TARGETARCH

# WHICH release to install, and the default is the ROLLING PRERELEASE, matching what the installer
# page offers rather than the last tagged version: MoonLight ships from `main` continuously, so a
# tagged release can be months behind what a board would be flashed with, and an image that lagged
# the firmware would be the wrong thing to test against.
#
# `latest` here is a real git TAG carrying that rolling build, not GitHub's "latest release" idea.
# `stable` is the special value asking for GitHub's newest NON-prerelease, and anything else is
# taken as a literal tag. The two words genuinely differ, which is why both exist.
ARG RELEASE=latest
ARG REPO=MoonModules/MoonLight

# libcurl4t64 is installed, not merely downloaded: the release binary links it (the one outbound
# HTTPS call), so the image must carry it AND everything it in turn needs. Letting apt resolve that
# is the point: the chain runs deep (TLS, Kerberos, LDAP, SASL, libssh2, compression), and a
# hand-written COPY list would be wrong the first time any of them changed.
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl libcurl4t64 \
 && if [ "$RELEASE" = "stable" ]; then \
        api="https://api.github.com/repos/${REPO}/releases/latest"; \
    else \
        api="https://api.github.com/repos/${REPO}/releases/tags/${RELEASE}"; \
    fi \
 && url=$(curl -fsSL "$api" | grep -o "https://[^\"]*_${TARGETARCH}\.deb" | head -1) \
 && test -n "$url" || { echo "no ${TARGETARCH} .deb in release ${RELEASE}" >&2; exit 1; } \
 && curl -fsSL -o /tmp/moonlight.deb "$url" \
 && dpkg-deb -x /tmp/moonlight.deb /rootfs \
 # Every shared object the binary resolves to, gathered by asking the loader rather than by
 # listing names: ldd walks the whole transitive chain, so this stays correct as that chain moves.
 # The four the distroless base already carries (libc, libstdc++, libm, libgcc_s) are EXCLUDED
 # rather than copied over: the base and this trixie stage are pinned independently, so shipping
 # both would put two glibc builds in one image and let the loader pick by path order.
 && mkdir -p /deps \
 && ldd /rootfs/usr/bin/MoonLight | awk '/=> \//{print $3}' | sort -u | grep -vE '/(libc|libm|libstdc\+\+|libgcc_s)\.so' | xargs -I{} cp -L {} /deps/

# --- stage 2: the image that ships ------------------------------------------------------------
# Distroless: the binary plus the shared libraries it resolves, with no shell and no package
# manager, so the attack surface is the application rather than a distribution.
#
# It used to be four libraries (libstdc++, libm, libgcc_s, libc), all of them in the base. Linking
# libcurl for the one outbound HTTPS call added a chain of its own (TLS, Kerberos, LDAP, compression),
# which is why the fetch stage now collects what the loader actually resolves instead of the image
# relying on the base to happen to carry it.
#
# **debian13, NOT debian12**, and this is load-bearing. The release is built on ubuntu-24.04
# (glibc 2.39), so the binary requires glibc >= 2.38. The debian12/bookworm images ship 2.36, where
# it installs cleanly and then dies at startup with "GLIBC_2.38 not found" from libc and libm.
# Verified both ways on the bench. If the release ever moves to an older builder, this can too.
# Pinned by digest, not by tag: `cc-debian13` is mutable, so an unpinned base means two
# builds of the same commit can ship different runtimes. Re-pin deliberately when picking
# up base updates (docker buildx imagetools inspect gcr.io/distroless/cc-debian13:latest).
FROM gcr.io/distroless/cc-debian13@sha256:9b615fff20e1a4fad29c2b30562580b212c7dd5e2225236735cca0070ed11c78

COPY --from=fetch /rootfs/usr/bin/MoonLight /usr/bin/MoonLight
# The libraries the loader resolved in the fetch stage, collected there rather than named here.
# Without this the container starts and dies immediately on "libcurl.so.4: cannot open shared
# object file", which is what shipped between dev.126 and this fix: the publish job builds the
# image but never runs it, so a missing library passes CI and fails on a user's board.
COPY --from=fetch /deps/ /usr/lib/

# WHERE THE CONFIG LIVES, and why this line is required rather than a convenience. The desktop
# build resolves its data directory from the environment (platform_desktop.cpp, userDataDir): on
# Linux XDG_DATA_HOME first, then HOME/.local/share. A container has NEITHER, and the function then
# returns empty, so without this the app has nowhere defined to write. Setting it explicitly also
# gives the volume one documented path instead of a guess: config lands in /data/MoonLight/.config.
ENV XDG_DATA_HOME=/data
VOLUME /data

EXPOSE 8080

ENTRYPOINT ["/usr/bin/MoonLight"]
