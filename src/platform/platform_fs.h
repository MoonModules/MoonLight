#pragma once
/// The platform's storage: the filesystem, streamed reads and writes, the SD card, zlib inflation and the folder a host serves its interface from.
///
/// One concern of the platform interface ([platform.h](platform.h) gathers them all), so a file includes the concerns it uses and an edit to one rebuilds its users alone.
///
/// @technical
///
/// ## Why a second write primitive
///
/// `fsWriteStream` pulls a whole upload to completion in one call, which suits a config file but not a card-sized one shared with a render loop. `fsWriteStreamOpen`/`Write`/`Close` take one bounded chunk per tick instead.

#include "platform/platform_base.h"   // target flags, MM_NONBLOCKING and the compiler macros

namespace mm::platform {

/// Filesystem bytes used.
size_t filesystemUsed();
/// Filesystem bytes total.
size_t filesystemTotal();

// LittleFS on ESP32, std::filesystem on host. Paths start with '/', which host strips to reach its root.

/// Redirect the host root, so a test gets an isolated directory while the working directory stays put; call it before fsMount.
void fsSetRoot(const char* path);
const char* fsRootPath();                                    // the resolved root, for diagnostics
bool fsMount();                                              // idempotent; safe to call multiple times
void fsUnmount();
bool fsMkdir(const char* path);                              // mkdir -p; no error if exists
bool fsExists(const char* path);
bool fsRemove(const char* path);                             // file or empty dir
/// Read a whole file; bytes read, or -1 on error, null-terminated on success.
int  fsRead(const char* path, char* buf, size_t maxLen);
/// A file's size in bytes, or -1 when missing; wide since a real card file can pass 2 GiB, more than a 32-bit long holds signed.
int64_t fsSize(const char* path);
/// Write a whole file atomically, through a temporary and a rename, creating its parent folder.
bool fsWriteAtomic(const char* path, const char* data, size_t len);
/// Fill up to `cap` bytes and answer the count; 0 ends the stream.
using FsWriteSrc = size_t(*)(char* buf, size_t cap, void* user, bool* abort);
/// Called once per child of a listed directory; a directory reports size 0.
using FsListCb = void(*)(const char* name, bool isDir, uint32_t sizeBytes, void* user);
/// List one level of `dir`, calling `cb` per child.
void fsList(const char* dir, FsListCb cb, void* user);

/// A read kept open across chunks, so a multi-megabyte download opens and seeks once.
struct FsReadStream { void* impl = nullptr; };
/// Open `path` for streamed reads; false when it does not exist or is a directory.
bool fsReadStreamOpen(FsReadStream& h, const char* path);
/// Pull up to `len` bytes from where the last read left off; 0 at EOF, -1 on error.
int fsReadStreamRead(FsReadStream& h, char* buf, size_t len);
/// Close the handle; safe on one that failed to open.
void fsReadStreamClose(FsReadStream& h);

/// A write kept open across chunks, so a caller can hand it bytes one bounded slice per tick rather than all at once. @xref{why-a-second-write-primitive}
struct FsWriteStream { void* impl = nullptr; };
/// Open a temporary file beside `path`, creating its parent folder; false on failure.
bool fsWriteStreamOpen(FsWriteStream& h, const char* path);
/// Append one chunk; false on a write failure, which the caller closes uncommitted.
bool fsWriteStreamWrite(FsWriteStream& h, const char* buf, size_t len);
/// `commit`: fsync and rename the temporary onto `path`. Otherwise: discard it. Either way the handle is spent.
bool fsWriteStreamClose(FsWriteStream& h, bool commit);

/// Write a file atomically from `src`, pulling it in chunks and creating its parent folder; false on abort or a write failure. One core loop over the resumable primitives above, so platform code implements only those, not this pull shape too.
inline bool fsWriteStream(const char* path, FsWriteSrc src, void* user) {
    if (!src) return false;
    FsWriteStream h{};
    if (!fsWriteStreamOpen(h, path)) return false;
    char chunk[1024];
    bool ok = true, abort = false;
    for (;;) {
        const size_t got = src(chunk, sizeof(chunk), user, &abort);
        if (abort) { ok = false; break; }
        if (got == 0) break;                                    // clean end of stream
        if (!fsWriteStreamWrite(h, chunk, got)) { ok = false; break; }
    }
    const bool committed = fsWriteStreamClose(h, ok);
    return ok && committed;
}

/// Inflate one self-contained zlib block in one call; `outLen` is set only on success. Real decoder only on ESP32 (the ROM's `tinfl_decompress`, zero flash cost); host and Teensy both stub to false, no decoder linked on either.
bool zlibInflateAll(void* workspace, const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap, size_t& outLen);
/// The bytes of decoder state the caller lends `zlibInflateAll` as its `workspace`, held only while it plays zlib; 0 where no decoder is linked.
size_t zlibWorkspaceBytes();

// A second, independent mount (microSD, FAT) sharing the fsXxx surface above through its own `/sd` path prefix; inert wherever hasSdCard is false.

/// Whether `path` names the filesystem's root: "" or "/", the two forms a listing of the root arrives as.
inline bool isFsRoot(const char* path) {
    return path && (path[0] == '\0' || (path[0] == '/' && path[1] == '\0'));
}

/// Whether `path` is rooted at the SD mount: "sd", "/sd", "sd/foo" or "/sd/foo"; false wherever hasSdCard is false, so a flash folder named `sd` stays ordinary.
inline bool isSdPath(const char* path) {
    if constexpr (!hasSdCard) return false;
    if (!path) return false;
    if (*path == '/') path++;
    return path[0] == 's' && path[1] == 'd' && (path[2] == '\0' || path[2] == '/');
}

/// The bus a slot is wired for: the native SD bus, or the SD spec's SPI mode.
enum class SdBus : uint8_t { Sdmmc, Spi };
/// A board's slot wiring, read at mount time rather than compiled in; -1 leaves a line unused. SPI mode reuses the SD spec's own pin mapping: CLK is SCK, CMD is MOSI, D0 is MISO and D3 is CS.
struct SdPinConfig {
    int8_t clk = -1;   ///< clock
    int8_t cmd = -1;   ///< command line, MOSI in SPI mode
    int8_t d0  = -1;   ///< data line 0, required, MISO in SPI mode
    int8_t d1  = -1;   ///< unset alongside d2/d3 drops the SD bus to 1-bit mode
    int8_t d2  = -1;   ///< data line 2
    int8_t d3  = -1;   ///< data line 3, CS in SPI mode
    int8_t pwr = -1;   ///< a slot's card-power switch, or -1 where the slot has none
    SdBus bus = SdBus::Sdmmc;   ///< which bus the lines above form
};
/// Mount the card under `/sd`, idempotent; false if no card, no slot, or the pins are unset, with the reason in sdMountError().
bool sdMount(const SdPinConfig& pins);
/// Why a mount fails: a pin that cannot drive the slot, no card answering, a card whose sectors do not read, or a card with no FAT filesystem.
enum class SdMountError : uint8_t { None, Pins, NoCard, Unreadable, NoFilesystem };
/// The reason the last sdMount() failed, None after a success.
SdMountError sdMountError();
/// True from a successful sdMount until the next unmount. A chip capable of SD is not the same as a card present, so the root listing gates on this, not hasSdCard.
bool sdCardMounted();
/// True while the mounted card still answers a status probe; false once it has been pulled, even though sdCardMounted() has not noticed yet. SdCardService polls this to detect a live removal.
bool sdCardPresent();
/// True while a stream is open on the card or a format runs; sdUnmount and sdFormat refuse meanwhile, so a caller checks it first for the specific reason.
bool sdCardBusy();
/// Unmount it; false (refused) while a stream is open or a format runs, true otherwise, including when no card was mounted.
bool sdUnmount();
/// Start formatting the mounted card, or one the last mount found with no FAT filesystem, in the background; false when no card awaits a format or the card is busy.
bool sdFormat();
/// Where the last sdFormat() stands; the card is unmounted while it runs and mounted again once it succeeds.
enum class SdFormatState : uint8_t { Idle, Running, Succeeded, Failed };
/// The state of the last sdFormat(), Idle before the first.
SdFormatState sdFormatState();
/// Card bytes used, 0 when unmounted; wide because a real card routinely exceeds 4 GiB.
uint64_t sdCardUsed();
/// Card bytes total, 0 when unmounted.
uint64_t sdCardTotal();
/// Whether SdCardService boots by default via kBootModules: matches hasSdCard on ESP32, off by default on host, so a plain host run carries no SD clutter unless a test opts in.
bool sdCardBootsByDefault();
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls
/// Shrink the host's fictional card, so a test reaches the "not enough space" path with a few small writes in place of a real gigabyte.
void setTestSdCardTotal(uint64_t bytes);
/// Opt SdCardService's default boot-wiring in or out for one test's scope.
void setTestSdCardBootsByDefault(bool enabled);
/// Simulate the card being pulled (false) or reinserted (true); also gates sdMount's own success, the way a real absent card would fail to answer.
void setTestSdCardPresent(bool present);
/// Simulate a card that answers but holds no FAT filesystem (false), which only a format makes mountable.
void setTestSdCardHasFilesystem(bool hasFilesystem);
/// Simulate a card that answers but whose sectors do not read (false), which no format helps.
void setTestSdCardReadable(bool readable);
/// Hold a started format Running (true), the way a large card's format outlasts many ticks; false completes it.
void setTestSdFormatHeld(bool held);
#endif

/// A UI file from the folder a host serves its interface from, read whole into a block the caller gives back with `free`. False on a device, whose interface is the embedded one alone, and for a name that is not a plain file in that folder.
bool uiFolderFile(const char* name, char*& data, size_t& len);
/// The folder a host serves its interface from, `src/ui` of the checkout it runs in; null serves the embedded interface alone.
void setUiFolder(const char* path);

}  // namespace mm::platform
