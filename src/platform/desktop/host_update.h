// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// A desktop installing a new version of itself as a board does, the desktop half of `otaBegin`, `otaWrite`, `otaEnd` and `reboot`, and the seams a test drives it through.

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace mm::platform {

/// Whether this process runs in a container, whose host replaces the program rather than the program itself.
bool inContainer();

/// A download's receiver, handed each chunk as it arrives; false ends the transfer as failed.
using DownloadChunkFn = bool (*)(void* user, const char* data, size_t n);
/// Download `url` into `take` a chunk at a time, following redirects, the answer's size into `total` once known; the HTTP status, or 0.
int httpDownload(const char* url, uint32_t timeoutMs, DownloadChunkFn take, void* user, uint32_t* total);

/// The running program's file, or the stand-in a test set.
std::filesystem::path programPath();
/// Stand a scratch file in for the running program, so a test installs over it; an empty path restores the real one.
void setTestProgramPath(const std::filesystem::path& path);

/// Vet `next` and its `sha256Hex` when named, keep `program` as `.prev` and put `next` in its place, marked `.pending`; false with the reason in `status`.
bool putProgramInPlace(const std::filesystem::path& next, const std::filesystem::path& program, const char* sha256Hex,
                       char* status, size_t statusLen);

/// Confirm a pending program once this run has stayed up `kBootStableMs`, which ends the service's rollback window; called once at start.
void confirmProgramWhenStable();

}  // namespace mm::platform
