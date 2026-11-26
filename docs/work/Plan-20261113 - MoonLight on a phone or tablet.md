# Plan: MoonLight on a phone or tablet

Plan of 2026-11-13, a Feature, approved 2026-11-16 (end state B, the recommended decisions). MoonLight runs on the phone or tablet in your pocket, so effects and MoonLive scripts are written, run and previewed on the road with no board and no laptop: the same program the desktop runs, with the phone as the host and its own browser as the screen. The phone drives networked fixtures too, Art-Net, DDP and E1.31 over Wi-Fi, since that is what the desktop build does already.

## What exists

- **The desktop platform is POSIX C++**: sockets, pthreads, `mmap`, miniaudio, `std::filesystem` ([platform_host.cpp](../../src/platform/host/platform_host.cpp)). Android is Linux with Bionic underneath, and the NDK's CMake toolchain builds POSIX C++ as any Linux does. Both phone platforms are arm64, which [the arm64 MoonLive backend](../../src/platform/host/moonlive_emit_arm64.cpp) already targets.
- **MoonLive compiles to machine code and to nothing else** ([moonlive.md](../explanation/architecture/moonlive.md)): the IR is lowered to native instructions in an `allocExec` block and discarded; a host without a backend ([moonlive_emit_noarch.cpp](../../src/platform/host/moonlive_emit_noarch.cpp)) runs no script. Android lets an app write and execute its own memory. iOS forbids it to every app but Safari, so on iOS no MoonLive script runs as shipped.
- **Three desktop OS names** in [CMakeLists.txt](../../CMakeLists.txt): `macos`, `windows`, `linux`, each a `firmware-host-<os>-<arch>` program the release packages and the Firmware card installs.
- **Three update modes** ([platform.h](../../src/platform/platform.h)): `self` where the program may replace its own file, `host` inside a container, where the host updates it and the map says so, `manual` elsewhere. A phone app is `host`: its program is the package the OS installed, replaced by installing the next one.
- **The data folder** comes from `XDG_DATA_HOME` or the home folder ([platform_host.cpp](../../src/platform/host/platform_host.cpp), `userDataDir`), which an app shell sets to its own files folder before the firmware starts.
- **miniaudio carries the phones' audio backends** already, AAudio and OpenSL for Android, Core Audio for iOS ([vendor/miniaudio.h](../../src/platform/host/vendor/miniaudio.h)).
- **The UI is a web page the device serves**, so the phone's browser, or a WebView inside the app, is the whole front end; the preview and the MoonLive editor run there today.
- **The release pipeline** builds four desktop packages in parallel and attaches them to the release ([release.yml](../../.github/workflows/release.yml)); the installer page lists them.
- **No card** on the Project names a phone, so this plan absorbs none.

## End states

| | End state | Gains | Loses |
|---|---|---|---|
| A | **A documented Termux route**: the Linux arm64 build inside `proot-distro`'s Debian on Android, nothing built. | Zero code. | A tester types in a terminal to start it, mDNS is deaf since Termux takes no multicast lock, Android stops it in the background, and no iPad. A second way to install, documented, with none of the product's care in it. |
| **B** | **An Android app**: the desktop platform built with the NDK, inside a small Kotlin shell that keeps it running and shows its page. | An icon on the phone and the tablet, the full UI with the preview and the MoonLive editor, the mic, networked fixtures; the APK on the release beside the other packages. | No iPhone or iPad. |
| C | **B, plus an iOS app** with an IR interpreter as MoonLive's backend where the OS refuses machine code. | iPhone and iPad. | A second way to run a script, which the architecture names as the rejected choice; an Apple developer account, TestFlight for every tester, the multicast entitlement, and a server iOS suspends about 30 s after the app leaves the screen. |

**Recommended: B.** It is the only end state where the stated use works as a product, and C's iOS half is a decision on MoonLive's architecture before it is a plan: it sits in [Open questions](#open-questions) and the [Backlog](#backlog).

## Design

- **Android is the fourth desktop OS, not a platform of its own.** The NDK toolchain compiles `mm_core`, `mm_platform` and the `MoonLight` executable as Linux does; `MM_HOST_OS` becomes `android`, so the program is `firmware-host-android-arm64`. A difference Bionic forces is a guard on `__ANDROID__` carrying its one-line reason, next to the `__APPLE__` and `_WIN32` guards that exist; a new platform file is a finding that the desktop platform is not the one platform it claims to be. The raw Ethernet path for ColorLight cards and the DRM screen stay out, as on any machine without them.
- **The shell is the textbook Android server app.** One Kotlin `Activity` holding a `WebView` on `http://localhost:8080/`, and one foreground service of type `connectedDevice`, the type Android names for an app talking to devices over the network, which starts the firmware on a thread of its own through one JNI call, `mm_main`, the call [main_host.cpp](../../src/platform/host/main_host.cpp) makes. The service holds a `MulticastLock`, since Android drops multicast otherwise and mDNS, Art-Net broadcast and the presence packets are multicast, and a partial wake lock, so the effects keep running with the screen off. Its notification's one action stops it. The firmware itself does not know it is on a phone.
- **Permissions when needed.** `RECORD_AUDIO` is asked for by the shell the first time the audio input opens; nothing else needs a runtime permission. The service's own permissions are declared, not asked.
- **The data folder is the app's files folder**, set through the environment the desktop already reads, so a backup from the phone restores on a desktop and back.
- **The app updates itself, as a desktop does.** The program downloads and vets the release's APK and hands it to Android's `PackageInstaller` through the shell, which replaces the app and starts it again; the app reports `update: self` once the shell lends its installer, `host` without it. Android asks once to allow installs from MoonLight and to confirm the first update, which makes MoonLight the installer of record, so the updates after it usually run unattended; Android can still ask, and the shell opens its prompt. The APK is a release asset, `MoonLight-android-arm64-v<version>.apk`, built and signed in CI; the installer page lists it with the other packages. A store listing is backlog.
- **The tablet is the phone with a wider page**; the UI already lays itself out by width.
- **A headset is an Android too.** A Meta Quest runs Horizon OS and a Pico its own, both Android on arm64, so the same APK runs there as a flat panel in front of the wearer, sideloaded in developer mode (SideQuest or `adb`); the store listing is a separate step, as on a phone. The Vision Pro runs visionOS, iOS's rules included, so it waits on the same MoonLive decision. What a headset adds, the lights drawn in the room around the wearer, is the UI's 3D preview on a WebXR page, backlog.

## Estimate

| Step | Work |
|---|---|
| 1. The NDK build of the existing program, run from `adb shell` | 1 day |
| 2. The shell: service, locks, WebView, JNI, the stop action | 2 days |
| 3. The mic through AAudio and the permission | 0.5 day |
| 4. CI: the APK job, signing, the release asset, the installer page | 1 day |
| 5. Docs and the install process | 0.5 day |
| **Total** | **about 5 days**, plus phone time |

## Steps

1. ✅ The NDK toolchain builds the host targets: [Running MoonLight on Android](../reference/installing-on-android.md), [one host platform](../explanation/architecture/mooncore.md), [build_android](../../moondeck/MoonDeck.md#build_android). x86-64 is not built.
2. ✅ `android/`, the Gradle shell around the host program: [what the app does](../reference/installing-on-android.md#what-the-app-does).
3. ✅ The microphone through AAudio: [what the app does](../reference/installing-on-android.md#what-the-app-does).
4. ✅ `build-android-arm64` in [release.yml](../../.github/workflows/release.yml), the APK signed by a key in four repository secrets: [build_android](../../moondeck/MoonDeck.md#build_android).
5. ✅ The docs: [Running MoonLight on Android](../reference/installing-on-android.md).

## Decisions

- **End state B** (for the product owner's pick, 2026-11-13).
- **The desktop platform compiled as is, no `android` platform folder** (recommended): the one platform claim holds, and every Bionic difference is visible as a guard with a reason.
- **A foreground service of type `connectedDevice`** (recommended): the type Android 14 defines for an app that talks to external devices over the network, which is what MoonLight does; a wrong type is refused at start on Android 14 and later.
- **Minimum Android 9 (API 28)** (recommended): the floor the NDK's current toolchains default to and old enough for a tablet of 2018; raising it later costs nothing, lowering it does.
- **Sideloaded from the release, no store listing in this plan** (recommended): the store is a listing, a review and an account, none of which the stated use needs.
- **The signing key in the repository's secrets** (recommended, done 2026-11-16): four `ANDROID_*` secrets sign every CI build, as the macOS notarization key does, so a `latest` build installs over the one before it; the keystore's home copy is the product owner's, its password in their Keychain. Rejected: the product owner signing each APK by hand, which leaves `latest` unsigned.

## Open questions

- **iOS**: the card "MoonLight on iPhone and iPad" on [the Project](https://github.com/users/MoonModules/projects/5), where the two binding rules and the first step, a design study on an interpreter backend for MoonLive, live. Picked after the Android app has been used on a trip.
- **Google's developer verification.** From 30 September 2026, a Google-certified Android phone or tablet in Brazil, Indonesia, Singapore and Thailand installs an app outside `adb` only from a verified developer, and every country follows in 2027. Meta's Horizon OS is no Google-certified Android, so a Quest stays outside it. A verified developer is an account in the Android Developer Console: a one-time US$25 fee and the owner's identity checked, then the package name `org.moonmodules.moonlight` and the release certificate (SHA-256 `5258d66568cbece7568a17dc4d595da9914c3061ce12fee00713a7f8a007ad6b`, `CN=MoonModules MoonLight`) registered to it. The free limited-distribution account reaches 20 devices, too few for a release. **Decided (2026-11-17):** registration waits until a user on a certified phone or tablet asks for it; the product owner then registers MoonModules, since the identity and the fee are theirs, and nothing in the tree changes, the same key signs on. The same account is what a Play Store listing needs, so both go on that one card.

## Backlog

- **iOS**: the card "MoonLight on iPhone and iPad" on [the Project](https://github.com/users/MoonModules/projects/5).

Proposals for the rest, each for the product owner's pick before the plan closes:

- **The phone as the installation's microphone** (Anjin, 2026-11-13). On Android the app is this already: the phone's mic is the Audio card's input, and the audio service's sync sends what it hears to every board. **Proposal:** no card for Android, a phone test of the sync send instead; the iOS half joins the receiver card below as its second job.
- **The phone as a fixture** (Anjin, 2026-11-13; an important use case, the product owner). On Android the app is this already as well: Network Receive takes DDP and Art-Net, and the preview shows the frame, which the Quest's cinema mode enlarges in ewowi's clips of 16 November. **Proposal:** one card, **an iOS receiver app**: DDP and Art-Net in, the frame full screen out, and the phone's mic out as audio sync, a small native app that runs no script, so neither iOS rule binds. Area Platform, Kind Feature.
- **A Play Store listing.** **Proposal:** one card, **Android developer verification and a Play Store listing**, picked when a user asks: the verified account first, then the listing, its review, and an update channel beside the self-update. Area Platform, Kind Feature.
- **The phone's camera as a video input.** **Proposal:** a line in Video in, LED walls out's backlog, which owns every video source, rather than a card.
- **The phone's USB-C display output as the HDMI screen.** Android mirrors its screen to a USB-C display on its own, so the preview on a mirrored display shows the lights with no code. **Proposal:** dropped; a second, independent picture is a card only when someone asks for one.
- **The 3D preview in a headset**, on a WebXR page, so a Quest shows the lights in the room before they are built. **Proposal:** one card, Area UI, Kind Feature.
- **Android TV and a docked tablet as a permanent controller.** What it needs is the service starting at boot (a `BOOT_COMPLETED` receiver) and, for a TV, a launcher entry. **Proposal:** one card, Area Platform, Kind Feature.

## What it removes

Nothing from the tree: the plan adds a build target, an app shell and a page. The subtraction it enforces is that Android adds no platform code of its own, which step 1 measures by its guard count, and that the Termux route of end state A is written down nowhere, so one way to install exists.
