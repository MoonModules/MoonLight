/// @defgroup platform_teensy_fs Files on the Teensy 4.1
/// @{
/// The filesystem seam: settings in LittleFS on the top of program flash, and the built-in microSD socket under `/sd`, each path routed to the store that holds it.

#include "platform/platform.h"

#include <Arduino.h>
#include <CrashReport.h>
#include <LittleFS.h>
#include <SD.h>

#include <cstdio>
#include <cstring>

// The program image's length, which the linker script defines: the filesystem starts above it.
extern unsigned long _flashimagelen;

namespace mm::platform {

// --- Filesystem: LittleFS on the top of program flash -------------------------------------------

namespace {
constexpr uint32_t kFlashBytes = 8 * 1024 * 1024;
constexpr uint32_t kReservedBytes = 256 * 1024;   // EEPROM emulation and the restore program, at the top of flash
constexpr uint32_t kFsBytes = 1024 * 1024;
// PJRC's LittleFS_Program uses 64 KB blocks on the 4.1, which eight settings files fill; the chip's 4 KB sectors are the block size, erased as the core's EEPROM emulation does.
extern "C" void eepromemu_flash_write(void* addr, const void* data, uint32_t len);
extern "C" void eepromemu_flash_erase_sector(void* addr);

class ProgramFlashFs : public LittleFS {
public:
    // `bytes` at the top of the program flash, below the EEPROM emulation; formats media that does not mount.
    bool begin(uint32_t bytes) {
        constexpr uint32_t kProgramFlashEnd = kFlashBytes - kReservedBytes;
        if (bytes == 0 || bytes % kSectorBytes || reinterpret_cast<uint32_t>(&_flashimagelen) > kProgramFlashEnd - bytes)
            return false;
        base_ = 0x60000000u + kProgramFlashEnd - bytes;
        config = {};
        config.read = &read;
        config.prog = &prog;
        config.erase = &erase;
        config.sync = &sync;
        config.read_size = 128;
        config.prog_size = 128;
        config.block_size = kSectorBytes;
        config.block_count = bytes / kSectorBytes;
        config.block_cycles = 800;
        config.cache_size = 128;
        config.lookahead_size = 128;
        config.name_max = LFS_NAME_MAX;
        configured = true;
        lfs = {};
        if (lfs_mount(&lfs, &config) < 0 && (lfs_format(&lfs, &config) < 0 || lfs_mount(&lfs, &config) < 0)) return false;
        mounted = true;
        return true;
    }

private:
    static constexpr uint32_t kSectorBytes = 4096;
    static inline uint32_t base_ = 0;

    static uint8_t* at(lfs_block_t block, lfs_off_t offset) {
        return reinterpret_cast<uint8_t*>(base_ + block * kSectorBytes + offset);
    }
    static int read(const lfs_config*, lfs_block_t block, lfs_off_t offset, void* buffer, lfs_size_t size) {
        std::memcpy(buffer, at(block, offset), size);
        return 0;
    }
    static int prog(const lfs_config*, lfs_block_t block, lfs_off_t offset, const void* buffer, lfs_size_t size) {
        eepromemu_flash_write(at(block, offset), buffer, size);
        return 0;
    }
    static int erase(const lfs_config*, lfs_block_t block) {
        eepromemu_flash_erase_sector(at(block, 0));
        return 0;
    }
    static int sync(const lfs_config*) { return 0; }
};

ProgramFlashFs g_fs;
bool g_mounted = false;

// The card while mounted, flash otherwise: every path below names one of the two, the card's under "/sd".
bool g_sdMounted = false;
int g_sdStreams = 0;   // open card streams, which unmount and format refuse under

// The filesystem `path` lives on and the path on it: the card for "/sd/...", flash for the rest; null when that one is not mounted.
FS* fsFor(const char* path, const char*& rel) {
    if (!path) return nullptr;
    if (isSdPath(path)) {
        rel = path + (path[0] == '/' ? 3 : 2);
        if (!*rel) rel = "/";
        return g_sdMounted ? static_cast<FS*>(&SD) : nullptr;
    }
    rel = path;
    return g_mounted ? static_cast<FS*>(&g_fs) : nullptr;
}

// Create every folder along the first `len` characters of `path`.
void mkdirs(FS& fs, const char* path, size_t len) {
    char buf[160];
    if (len >= sizeof(buf)) return;
    std::memcpy(buf, path, len);
    buf[len] = 0;
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] != '/' && buf[i] != 0) continue;
        const char saved = buf[i];
        buf[i] = 0;
        if (!fs.exists(buf)) fs.mkdir(buf);
        buf[i] = saved;
    }
}

void makeParent(FS& fs, const char* path) {
    const char* slash = std::strrchr(path, '/');
    if (slash && slash != path) mkdirs(fs, path, static_cast<size_t>(slash - path));
}

// Write `tmp`, then rename it over `path`, which LittleFS does atomically and the card nearly so.
template <class Fill>
bool writeAtomic(const char* apiPath, Fill fill) {
    const char* path = nullptr;
    FS* fs = fsFor(apiPath, path);
    if (!fs) return false;
    makeParent(*fs, path);
    char tmp[168];
    if (std::strlen(path) + 5 > sizeof(tmp)) return false;
    std::snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fs->remove(tmp);
    File f = fs->open(tmp, FILE_WRITE_BEGIN);
    if (!f) return false;
    const bool ok = fill(f);
    f.close();
    if (!ok) { fs->remove(tmp); return false; }
    if (fs == &SD) fs->remove(path);   // the card's rename refuses an existing destination; LittleFS replaces it atomically, so its old file stays until the rename
    if (!fs->rename(tmp, path)) { fs->remove(tmp); return false; }
    return true;
}
}  // namespace

const char* fsRootPath() { return "/"; }

bool fsMount() {
    if (!g_mounted) g_mounted = g_fs.begin(kFsBytes);
    return g_mounted;
}

bool fsMkdir(const char* apiPath) {
    const char* path = nullptr;
    FS* fs = fsFor(apiPath, path);
    if (!fs) return false;
    mkdirs(*fs, path, std::strlen(path));
    return fs->exists(path);
}

bool fsRemove(const char* apiPath) {
    const char* path = nullptr;
    FS* fs = fsFor(apiPath, path);
    return fs && (fs->remove(path) || fs->rmdir(path));
}

int fsRead(const char* apiPath, char* buf, size_t maxLen) {
    const char* path = nullptr;
    FS* fs = fsFor(apiPath, path);
    if (!fs || !buf || maxLen == 0) return -1;
    File f = fs->open(path, FILE_READ);
    if (!f || f.isDirectory()) return -1;
    const size_t n = f.read(buf, maxLen - 1);
    f.close();
    buf[n] = 0;
    return static_cast<int>(n);
}

int64_t fsSize(const char* apiPath) {
    const char* path = nullptr;
    FS* fs = fsFor(apiPath, path);
    if (!fs) return -1;
    File f = fs->open(path, FILE_READ);
    if (!f || f.isDirectory()) return -1;
    const int64_t size = static_cast<int64_t>(f.size());
    f.close();
    return size;
}

namespace {
// A kept-open read, and whether it holds the card.
struct ReadHandle { File f; bool onCard; };
}  // namespace

bool fsReadStreamOpen(FsReadStream& h, const char* apiPath) {
    const char* path = nullptr;
    FS* fs = fsFor(apiPath, path);
    if (!fs) return false;
    File f = fs->open(path, FILE_READ);
    if (!f || f.isDirectory()) return false;
    h.impl = new ReadHandle{f, fs == &SD};
    if (fs == &SD) ++g_sdStreams;
    return true;
}

int fsReadStreamRead(FsReadStream& h, char* buf, size_t len) {
    auto* r = static_cast<ReadHandle*>(h.impl);
    if (!r || !buf) return -1;
    return static_cast<int>(r->f.read(buf, len));
}

void fsReadStreamClose(FsReadStream& h) {
    auto* r = static_cast<ReadHandle*>(h.impl);
    if (!r) return;
    r->f.close();
    if (r->onCard) --g_sdStreams;
    delete r;
    h.impl = nullptr;
}

bool fsWriteAtomic(const char* path, const char* data, size_t len) {
    return writeAtomic(path, [&](File& f) { return f.write(data, len) == len; });
}

namespace {
// The open file, both paths and its filesystem, so close() renames or discards without re-deriving them.
struct WriteHandle { File f; FS* fs; char path[160]; char tmp[168]; };
}  // namespace

// The same temp-then-rename as writeAtomic, held open across the caller's chunks.
bool fsWriteStreamOpen(FsWriteStream& h, const char* apiPath) {
    const char* path = nullptr;
    FS* fs = fsFor(apiPath, path);
    if (!fs || std::strlen(path) >= sizeof(WriteHandle::path)) return false;
    makeParent(*fs, path);
    auto* w = new WriteHandle();
    w->fs = fs;
    std::snprintf(w->path, sizeof(w->path), "%s", path);
    std::snprintf(w->tmp, sizeof(w->tmp), "%s.tmp", path);
    fs->remove(w->tmp);
    w->f = fs->open(w->tmp, FILE_WRITE_BEGIN);
    if (!w->f) { delete w; return false; }
    if (fs == &SD) ++g_sdStreams;
    h.impl = w;
    return true;
}

bool fsWriteStreamWrite(FsWriteStream& h, const char* buf, size_t len) {
    auto* w = static_cast<WriteHandle*>(h.impl);
    return w && buf && w->f.write(buf, len) == len;
}

bool fsWriteStreamClose(FsWriteStream& h, bool commit) {
    auto* w = static_cast<WriteHandle*>(h.impl);
    if (!w) return false;
    w->f.close();
    bool renamed = false;
    if (commit) {
        if (w->fs == &SD) w->fs->remove(w->path);   // the card's rename refuses an existing destination; LittleFS replaces it atomically
        renamed = w->fs->rename(w->tmp, w->path);
    }
    if (!renamed) w->fs->remove(w->tmp);
    if (w->fs == &SD) --g_sdStreams;
    delete w;
    h.impl = nullptr;
    return commit ? renamed : true;
}

// --- The built-in microSD socket: PJRC's SD library on the 4.1's own SDIO host ----------------------

namespace {
SdMountError g_sdError = SdMountError::None;
SdFormatState g_sdFormatState = SdFormatState::Idle;

// The socket's own pins, fixed by the board and named again by SdCardService's "Teensy 4.1 SD socket" preset: an edit to one is an edit to both.
bool isSocketWiring(const SdPinConfig& p) {
    return p.bus == SdBus::Sdmmc && p.clk == 44 && p.cmd == 45 && p.d0 == 43 && p.d1 == 42 && p.d2 == 47 && p.d3 == 46;
}

// Why a begin failed: the card answered with no volume it reads, or no card answered at all.
SdMountError failureReason() {
    SdCard* card = SD.sdfs.card();
    return card && !card->errorCode() ? SdMountError::NoFilesystem : SdMountError::NoCard;
}
}  // namespace

bool sdMount(const SdPinConfig& pins) {
    if (g_sdMounted) return true;
    if (!isSocketWiring(pins)) { g_sdError = SdMountError::Pins; return false; }
    g_sdMounted = SD.begin(BUILTIN_SDCARD);
    g_sdError = g_sdMounted ? SdMountError::None : failureReason();
    return g_sdMounted;
}

SdMountError sdMountError() { return g_sdError; }
bool sdCardMounted() { return g_sdMounted; }
// A transfer in flight answers for the card itself: SdFat holds a multi-block SDIO transfer open between calls, and a status command in the middle breaks it.
bool sdCardPresent() { return g_sdMounted && (g_sdStreams > 0 || SD.mediaPresent()); }
bool sdCardBusy() { return g_sdStreams > 0; }

bool sdUnmount() {
    if (!g_sdMounted) return true;
    if (g_sdStreams > 0) return false;   // an open stream holds a file on the card
    SD.sdfs.end();
    g_sdMounted = false;
    return true;
}

// In the call: the Teensy has no task to hand it to, so the render pauses for a format's few seconds.
bool sdFormat() {
    const bool unformatted = !g_sdMounted && g_sdError == SdMountError::NoFilesystem;
    if (sdCardBusy() || (!g_sdMounted && !unformatted)) return false;
    g_sdFormatState = SdFormatState::Running;
    g_sdMounted = SD.format(0, 0);   // FAT32 or exFAT by size, as the SD Association assigns; it mounts the result
    g_sdError = g_sdMounted ? SdMountError::None : failureReason();
    g_sdFormatState = g_sdMounted ? SdFormatState::Succeeded : SdFormatState::Failed;
    return true;
}

SdFormatState sdFormatState() { return g_sdFormatState; }
uint64_t sdCardUsed() { return g_sdMounted ? SD.usedSize() : 0; }
uint64_t sdCardTotal() { return g_sdMounted ? SD.totalSize() : 0; }
bool sdCardBootsByDefault() { return hasSdCard; }

void fsList(const char* apiDir, FsListCb cb, void* user) {
    if (!cb) return;
    if (isFsRoot(apiDir) && g_sdMounted) cb("sd", true, 0, user);   // a second mount, listed at the root by hand
    const char* dir = nullptr;
    FS* fs = fsFor(apiDir, dir);
    if (!fs) return;
    File d = fs->open(dir);
    if (!d || !d.isDirectory()) return;
    while (File e = d.openNextFile()) {
        const bool isDir = e.isDirectory();
        cb(e.name(), isDir, isDir ? 0 : static_cast<uint32_t>(e.size()), user);
        e.close();
    }
    d.close();
}

size_t filesystemUsed() { return g_mounted ? static_cast<size_t>(g_fs.usedSize()) : 0; }
size_t filesystemTotal() { return g_mounted ? static_cast<size_t>(g_fs.totalSize()) : 0; }
size_t firmwareSize() { return reinterpret_cast<size_t>(&_flashimagelen); }
size_t firmwarePartition() { return kFlashBytes - kReservedBytes - kFsBytes; }
size_t flashChipSize() { return kFlashBytes; }

}  // namespace mm::platform

/// @}
