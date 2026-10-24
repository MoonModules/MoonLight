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
/// The compiled-binary variant (`MM_FIRMWARE_NAME`), such as `esp32-eth`, or `unknown` for a local desktop build.
extern const char* const kFirmwareName;
/// The release channel it shipped on (`MM_RELEASE`), such as `latest` or `v1.0.0`, empty for a local build.
extern const char* const kRelease;

} // namespace mm
