#pragma once

/// Every fact about which binary this is, as the build system knew it.
///
/// `build_info.cpp` defines them all, the one unit that sees the build flags and the generated `build_id.h`, so a new commit or build recompiles that unit and no other.

namespace mm {

/// The semver: `library.json`'s, or the release pipeline's `MM_VERSION`, `<next>-dev.<N>` on the moving `latest` channel.
extern const char* const kVersion;
/// When it was built, for a human rather than for identity.
extern const char* const kBuildDate;
/// The short git hash it was built from, `+` when the tree was dirty, `nogit` without a checkout: what landed on a board.
extern const char* const kBuildId;
/// The compiled-binary variant (`MM_FIRMWARE_NAME`), such as `esp32-ethonly`, or `host-linux-arm64` for a host (CMakeLists.txt), `unknown` on a processor no release packages.
extern const char* const kFirmwareName;
/// The MoonBase image this board takes, its family (`esp32`, `esp32p4rev3`, `esp32s3oflash`: the chip, the P4's revision line, octal flash), from build_esp32.py's migration_family; empty where the build carries no MoonBase.
extern const char* const kMoonBaseFamily;
/// The release channel it shipped on (`MM_RELEASE`), such as `latest` or `v1.0.0`, empty for a local build.
extern const char* const kRelease;

} // namespace mm
