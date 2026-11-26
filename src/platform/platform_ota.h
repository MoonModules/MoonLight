#pragma once
/// The platform's firmware update: how this host updates, the image writer and MoonBase, the boot image an update hands over to.
///
/// One concern of the platform interface ([platform.h](platform.h) gathers them all), so a file includes the concerns it uses and an edit to one rebuilds its users alone.

#include "platform/platform_base.h"   // target flags, MM_NONBLOCKING and the compiler macros

namespace mm::platform {


/// How this device takes a new version: it installs one itself, its container is replaced from outside, or someone installs it by hand (a program in a folder it cannot write, or an app with no installer lent).
enum class UpdateMode : uint8_t { Self, Container, Manual };
/// The mode's name, as `/api/system` reports it.
constexpr const char* updateModeName(UpdateMode m) {
    return m == UpdateMode::Self ? "self" : m == UpdateMode::Container ? "container" : "manual";
}
/// How this device takes a new version: a board installs its own images, a host installs itself when its program sits in a folder it may write, a container is its host's.
UpdateMode updateMode();
/// True once an installed image went to the operating system's installer, which replaces and restarts the program itself, so the caller leaves the restart to it and keeps running meanwhile.
bool imageAwaitsInstaller();

/// Fetch a firmware image from `url`, or `fallbackUrl` where that cannot be opened, and install it, returning at once; a host checks `sha256Hex` when it is non-empty, a board's image its own.
bool http_fetch_to_ota(const char* url,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut,
                       const char* fallbackUrl = nullptr, const char* sha256Hex = nullptr);

/// An image written as its bytes arrive, which whoever receives it pushes a chunk at a time.
struct OtaWrite { void* impl = nullptr; };
/// Which image an install writes: the application's slot, MoonBase's, which only the running application can write, or the radio chip's firmware, streamed over its link.
enum class OtaTarget : uint8_t { App, MoonBase, Coprocessor };
/// Start an image of `contentLen` bytes (0 when unknown) for `target`, its progress in `statusBuf` and `bytesOut`; false with the reason there.
bool otaBegin(OtaWrite& w, OtaTarget target, size_t contentLen, char* statusBuf, size_t statusBufLen, uint32_t* bytesOut);
/// Hand the image its next bytes; false once it is refused or the flash fails, the reason in its status and the image abandoned.
bool otaWrite(OtaWrite& w, const uint8_t* data, size_t len);
/// Finish the image: with `commit` check it whole and, for the application, point the boot at it; with it false, abandon it. True once committed.
bool otaEnd(OtaWrite& w, bool commit);

/// Does this partition table carry MoonBase, the second boot image (a board cannot rewrite the partition it executes from)?
bool otaHasMoonBase();
/// Point the bootloader at MoonBase; false when there is none.
bool otaBootMoonBase();
/// Are we executing from MoonBase right now?
bool otaRunningMoonBase();
/// Which MoonBase is installed, read from its app descriptor alone.
bool otaMoonBaseVersion(char* out, size_t len);
/// When that MoonBase was built, so the UI shows its identity as it shows the app's.
bool otaMoonBaseBuild(char* out, size_t len);
/// The handoff version that MoonBase speaks, read from the record after its descriptor; 0 when there is no MoonBase or record.
uint32_t otaMoonBaseHandoff();
/// How much of its slot MoonBase fills.
bool otaMoonBaseSize(uint32_t* used, uint32_t* total);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// Give a host a MoonBase with this descriptor version, build time and handoff version, for a host test; a null version removes it.
void setTestMoonBase(const char* version, const char* built, uint32_t handoff);
#endif
/// An install of `target` from a URL, which the device fetches itself; runs on its own task and returns at once. For the radio chip it starts the link first where it is still unstarted.
bool otaFetchUrl(OtaTarget target, const char* url, char* statusBuf, size_t statusBufLen,
                 uint32_t* bytesReadOut, uint32_t* bytesTotalOut);
/// Stage an install URL for MoonBase to pick up on its next boot, at most 255 bytes.
bool moonbaseStageInstallUrl(const char* url);
/// Erase a staged URL still unconsumed, which a power cut mid-stage leaves armed.
void moonbaseClearStagedUrl();

}  // namespace mm::platform
