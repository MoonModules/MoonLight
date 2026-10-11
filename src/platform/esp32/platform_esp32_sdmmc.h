#pragma once

#include "platform/platform.h"

/// The SD card backend behind every `/sd`-prefixed `fsXxx` call, split out of platform_esp32_fs.cpp, which dispatches here.
/// Every path below is the full API path (such as `/sd/foo.txt`), translated internally onto the real FatFs mount point.

namespace mm::platform {

/// Whether `sdMount()` has succeeded and `sdUnmount()` has not run since.
bool sdCardMounted();
/// mkdir -p on the card; no error if it exists.
bool sdCardMkdir(const char* apiPath);
/// Whether `apiPath` exists on the card.
bool sdCardExists(const char* apiPath);
/// Remove a file or empty directory from the card.
bool sdCardRemove(const char* apiPath);
/// Read a whole file from the card; bytes read, or -1 on error, null-terminated on success.
int  sdCardRead(const char* apiPath, char* buf, size_t maxLen);
/// A card file's size in bytes, or -1 when it is missing or not a file.
long sdCardSize(const char* apiPath);
/// Read up to `len` bytes at `offset` on the card; bytes read, 0 at the end, -1 on error.
int  sdCardReadAt(const char* apiPath, long offset, char* buf, size_t len);
/// Write a whole file atomically onto the card, through a temporary and a rename.
bool sdCardWriteAtomic(const char* apiPath, const char* data, size_t len);
/// Write a card file atomically, pulling `src` in chunks until it signals done or abort.
bool sdCardWriteStreamCb(const char* apiPath, FsWriteSrc src, void* user);
/// List one level of a card directory, calling `cb` per child.
void sdCardList(const char* apiDir, FsListCb cb, void* user);

/// Open `apiPath` for streamed reads from the card; false when it does not exist or is a directory.
bool sdCardReadStreamOpen(void*& handle, const char* apiPath);
/// Pull up to `len` bytes from where the last read left off; 0 at EOF, -1 on error.
int  sdCardReadStreamRead(void* handle, char* buf, size_t len);
/// Close a read handle; safe on one that failed to open.
void sdCardReadStreamClose(void* handle);

/// Open a temporary file beside `apiPath` on the card, creating its parent folder; false on failure.
bool sdCardWriteStreamOpen(void*& handle, const char* apiPath);
/// Append one chunk to a card write handle; false on a write failure.
bool sdCardWriteStreamWrite(void* handle, const char* buf, size_t len);
/// Flush and close a card write handle; the rename-or-discard on `commit` is the caller's, in platform_esp32_fs.cpp.
bool sdCardWriteStreamClose(void* handle, bool commit);

} // namespace mm::platform
