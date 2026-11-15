# Plan: MoonLight on a phone or tablet

Plan of 2026-11-13, a Feature, for approval. MoonLight runs on the phone or tablet in your pocket, so effects and MoonLive scripts are written, run and previewed on the road with no board and no laptop: the same program the desktop runs, with the phone as the host and its own browser as the screen. The phone drives networked fixtures too, Art-Net, DDP and E1.31 over Wi-Fi, since that is what the desktop build does already.

## What exists

- **The desktop platform is POSIX C++**: sockets, pthreads, `mmap`, miniaudio, `std::filesystem` ([platform_desktop.cpp](../../src/platform/desktop/platform_desktop.cpp)). Android is Linux with Bionic underneath, and the NDK's CMake toolchain builds POSIX C++ as any Linux does. Both phone platforms are arm64, which [the arm64 MoonLive backend](../../src/platform/desktop/moonlive_emit_arm64.cpp) already targets.
- **MoonLive compiles to machine code and to nothing else** ([moonlive.md](../explanation/architecture/moonlive.md)): the IR is lowered to native instructions in an `allocExec` block and discarded; a host without a backend ([moonlive_emit_noarch.cpp](../../src/platform/desktop/moonlive_emit_noarch.cpp)) runs no script. Android lets an app write and execute its own memory. iOS forbids it to every app but Safari, so on iOS no MoonLive script runs as shipped.
- **Three desktop OS names** in [CMakeLists.txt](../../CMakeLists.txt): `macos`, `windows`, `linux`, each a `firmware-desktop-<os>-<arch>` program the release packages and the Firmware card installs.
- **Three update modes** ([platform.h](../../src/platform/platform.h)): `self` where the program may replace its own file, `host` inside a container, where the host updates it and the map says so, `manual` elsewhere. A phone app is `host`: its program is the package the OS installed, replaced by installing the next one.
- **The data folder** comes from `XDG_DATA_HOME` or the home folder ([platform_desktop.cpp](../../src/platform/desktop/platform_desktop.cpp), `userDataDir`), which an app shell sets to its own files folder before the firmware starts.
- **miniaudio carries the phones' audio backends** already, AAudio and OpenSL for Android, Core Audio for iOS ([vendor/miniaudio.h](../../src/platform/desktop/vendor/miniaudio.h)).
- **The UI is a web page the device serves**, so the phone's browser, or a WebView inside the app, is the whole front end; the preview and the MoonLive editor run there today.
- **The release pipeline** builds four desktop packages in parallel and attaches them to the release ([release.yml](../../.github/workflows/release.yml)); the installer page lists them.
- **No card** on the Project names a phone, so this plan absorbs none.

## End states

| | End state | Gains | Loses |
|---|---|---|---|
| A | **A documented Termux route**: the Linux arm64 build inside `proot-distro`'s Debian on Android, nothing built. | Zero code. | A tester types in a terminal to start it, mDNS is deaf since Termux takes no multicast lock, Android stops it in the background, and no iPad. A second way to install, documented, with none of the product's care in it. |
| **B** | **An Android app**: the desktop platform built with the NDK, inside a small Kotlin shell that keeps it running and shows its page. | An icon on the phone and the tablet, the full UI with the preview and the MoonLive editor, the mic, networked fixtures; the APK on the release beside the other packages. | No self-update: the next APK installs over this one, which the map already says for `host`. No iPhone or iPad. |
| C | **B, plus an iOS app** with an IR interpreter as MoonLive's backend where the OS refuses machine code. | iPhone and iPad. | A second way to run a script, which the architecture names as the rejected choice; an Apple developer account, TestFlight for every tester, the multicast entitlement, and a server iOS suspends about 30 s after the app leaves the screen. |

**Recommended: B.** It is the only end state where the stated use works as a product, and C's iOS half is a decision on MoonLive's architecture before it is a plan: it sits in [Open questions](#open-questions) and the [Backlog](#backlog).

## Design

- **Android is the fourth desktop OS, not a platform of its own.** The NDK toolchain compiles `mm_core`, `mm_platform` and the `MoonLight` executable as Linux does; `MM_DESKTOP_OS` becomes `android`, so the program is `firmware-desktop-android-arm64`. A difference Bionic forces is a guard on `__ANDROID__` carrying its one-line reason, next to the `__APPLE__` and `_WIN32` guards that exist; a new platform file is a finding that the desktop platform is not the one platform it claims to be. The raw Ethernet path for ColorLight cards and the DRM screen stay out, as on any machine without them.
- **The shell is the textbook Android server app.** One Kotlin `Activity` holding a `WebView` on `http://localhost:8080/`, and one foreground service of type `connectedDevice`, the type Android names for an app talking to devices over the network, which starts the firmware on a thread of its own through one JNI call, `mm_main`, the call [main_desktop.cpp](../../src/platform/desktop/main_desktop.cpp) makes. The service holds a `MulticastLock`, since Android drops multicast otherwise and mDNS, Art-Net broadcast and the presence packets are multicast, and a partial wake lock, so the effects keep running with the screen off. Its notification's one action stops it. The firmware itself does not know it is on a phone.
- **Permissions when needed.** `RECORD_AUDIO` is asked for by the shell the first time the audio input opens; nothing else needs a runtime permission. The service's own permissions are declared, not asked.
- **The data folder is the app's files folder**, set through the environment the desktop already reads, so a backup from the phone restores on a desktop and back.
- **Updates through the store the user chose.** The app reports `update: host`; the Firmware card and the map name the release's APK instead of installing it. The APK is a release asset, `MoonLight-v<version>.apk`, built and signed in CI; the installer page lists it with the other packages. A store listing is backlog.
- **The tablet is the phone with a wider page**; the UI already lays itself out by width.

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

1. ⏳ The NDK toolchain builds the desktop targets: `cmake -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28`, `MM_DESKTOP_OS=android`, `update: host`; the `MoonLight` binary pushed with `adb` runs in `adb shell` with the data folder set and serves its page to the phone's browser; a MoonLive effect compiles and runs on the phone, which is the JIT verified on Android; the unit tests build and pass there too. Each `__ANDROID__` guard carries its reason. x86-64 is built as well where it costs nothing, since the emulator and the x86-64 backend exist.
2. ⏳ `android/`, a Gradle project: the foreground service with its multicast and wake locks starting `mm_main` over JNI, the WebView activity, the stop action; the data folder in the app's files; MoonLight opens on a phone and a tablet from its icon, keeps running with the screen off, and finds the bench boards on the map. Verified on the product owner's phone and tablet.
3. ⏳ The Audio card lists the phone's microphone through miniaudio's AAudio backend; the shell asks for `RECORD_AUDIO` the first time the input opens; an audio-reactive effect follows the room.
4. ⏳ `build-android-arm64` in [release.yml](../../.github/workflows/release.yml): the APK signed with a key in the repository's secrets, attached as `MoonLight-v<version>.apk`; the installer page lists it; the Firmware card on the phone names it as the next version. A `latest` build installs over the previous one with its settings kept.
5. ⏳ `docs/reference/installing-on-android.md` beside the Linux page: the machine, the app, what the OS excludes, the mic, updating; [Install MoonLight](../moonwalk.md#install) with Android picked; the README's platform line; [mooncore.md](../explanation/architecture/mooncore.md)'s platform section names the fourth desktop OS.

## Decisions

- **End state B** (for the product owner's pick, 2026-11-13).
- **The desktop platform compiled as is, no `android` platform folder** (recommended): the one platform claim holds, and every Bionic difference is visible as a guard with a reason.
- **A foreground service of type `connectedDevice`** (recommended): the type Android 14 defines for an app that talks to external devices over the network, which is what MoonLight does; a wrong type is refused at start on Android 14 and later.
- **Minimum Android 9 (API 28)** (recommended): the floor the NDK's current toolchains default to and old enough for a tablet of 2018; raising it later costs nothing, lowering it does.
- **Sideloaded from the release, no store listing in this plan** (recommended): the store is a listing, a review and an account, none of which the stated use needs.

## Open questions

- **iOS.** Two of its rules bind. The app runs only on screen: iOS suspends it about 30 s after a switch to another app, and the one standard exemption that fits is the `audio` background mode, which keeps an app with the microphone open running, so an audio-reactive session survives a switch and a plain effect does not (Anjin's point in the Discord thread, 2026-11-13). The harder one is the ban on executing memory the app wrote, which leaves two ways: an interpreter over MoonLive's IR, kept alive instead of discarded, as a backend file like the others, or no MoonLive on iOS, which empties the stated use. An interpreter on an A-series chip is faster than compiled code on an ESP32 for the preview-sized layouts a tester carries, so the speed argument for codegen does not bind there; the architecture argument does, since it is a second execution engine. **Recommended (2026-11-13):** decide after step 2 ships and the Android app has been used on a trip; an iOS plan then starts from a design study on the interpreter backend, with the Apple account, TestFlight and the multicast entitlement as its costs.
- **Google's developer verification.** Android is moving to installs only from verified developers, sideloads included, by region from 2026 (announced 2025). A one-time registration of MoonModules as the developer, which step 4's signing key then belongs to. To be confirmed when step 4 starts.
- **The signing key's home**: a repository secret, as the macOS notarization key is, or the product owner's keystore with CI building an unsigned APK the product owner signs? **Recommended:** the secret, so a `latest` build is installable as the other packages are.

## Backlog

- An iOS app, after the interpreter decision above.
- **The phone as the installation's microphone** (Anjin, 2026-11-13): a small native app that sends the phone's mic as the audio sync packets a board already follows, no MoonLight on the phone. A different product from this plan, since it needs a board, so it goes to [Audio that answers the music](Plan-20261028%20-%20Audio%20that%20answers%20the%20music%20(11%20of%2012).md)'s backlog when picked.
- **The phone as a fixture** (Anjin, 2026-11-13; an important use case, the product owner): a small native app that takes DDP and Art-Net on the phone or tablet and shows the frame on its screen, so any sender drives it, a MoonLight board, the desktop, WLED or xLights, with no browser and no board of its own. It is the iOS answer while the MoonLight app is Android only: a receiver runs no script, so the ban on executing written memory does not touch it, and a display stays on screen, so the suspension does not either. Unicast needs no multicast entitlement. Its first plan is a spec: DDP and Art-Net in, the layout the sender's universes map to, full-screen out, on iOS and Android alike.
- A Play Store listing.
- The phone's camera as a video input, which [Video in, LED walls out](Plan-20261024%20-%20Video%20in%2C%20LED%20walls%20out%20(0%20of%2010).md) would take as another source.
- The phone's USB-C display output as the HDMI screen, a `SurfaceView` behind [screen.h](../../src/platform/desktop/screen.h)'s contract.
- Android TV and the tablet in a dock as a permanent controller, which is the Linux board's role and needs only the service to start at boot.

## What it removes

Nothing from the tree: the plan adds a build target, an app shell and a page. The subtraction it enforces is that Android adds no platform code of its own, which step 1 measures by its guard count, and that the Termux route of end state A is written down nowhere, so one way to install exists.
