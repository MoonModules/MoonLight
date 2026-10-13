/// @defgroup platform_teensy_ota Firmware updates on the Teensy 4.1
/// @{
/// An uploaded flat image is staged in the free program flash above the running one, and checked. At the next reboot a routine running from RAM copies it over the program.
///
/// @moreinfo
///
/// ## One slot, so the copy waits for the reboot
///
/// An ESP32 writes its update into a second app slot and flips a boot pointer. The Teensy's program flash holds one image, so the upload goes into the erased space above it, where nothing runs.
/// The copy over the program happens in `reboot()`, after the HTTP answer is out. A power loss during the upload leaves the running image untouched.
/// One during the copy, a few seconds, leaves the board to PJRC's bootloader chip and its program button, never a hard brick.
/// Both sides work in 64 KB blocks, the flash's largest erase, and the copy skips every block the new image shares with the running one.
///
/// ## Why the copy masks every interrupt source
///
/// The application executes from flash, and the copy erases it.
/// The core's flash routines turn interrupts back on after every command. So the copy disables every interrupt source at the NVIC and stops SysTick instead, and nothing can fire into a half-erased image.
/// It runs from ITCM, as the core's flash routines do. It passes each sector through RAM, since the flash cannot be read while it is being programmed.
///
/// ## The image mark
///
/// Every image carries its firmware name, and an upload must carry the running one's: another program would leave the board off the network until a USB flash.
/// The search reads its prefix from the mark itself, so the prefix occurs once per image, and that reference keeps the linker from collecting the mark.

#include "platform/platform.h"
#include "core/util/build_info.h"   // MM_FIRMWARE_NAME, the variant an image must match

#include <Arduino.h>

#include "core/util/TeensyImage.h"   // teensyHeaderProblem, teensyImageRefused: the checks before an image may overwrite the program

#include <cstdio>
#include <cstring>

extern "C" {
void eepromemu_flash_write(void* addr, const void* data, uint32_t len);
void eepromemu_flash_erase_64K_block(void* addr);
}

namespace mm::platform {

namespace {
constexpr uint32_t kFlashBase = 0x60000000;
constexpr uint32_t kSectorBytes = 4096;
constexpr uint32_t kBlockBytes = 65536;   // the erase unit: one block erase costs about what one sector erase does, so 16x fewer of them
constexpr uint32_t kPageBytes = 256;   // the most one program command writes

// The firmware name an upload must carry: @xref{the-image-mark}.
constexpr char kImageMark[] = "MoonLight image for " MM_FIRMWARE_NAME ";";
constexpr uint32_t kMarkPrefixLen = sizeof("MoonLight image for ") - 1;

alignas(4) uint8_t g_sector[kSectorBytes];   // one sector in RAM, for the upload and for the copy
uint32_t g_stagedBytes = 0;                   // nonzero once a checked image waits for the reboot

// Block-aligned, so erasing a block of the program never touches an uncopied staged block.
uint32_t stagingBase() { return (kFlashBase + static_cast<uint32_t>(firmwareSize()) + kBlockBytes - 1) & ~(kBlockBytes - 1); }
uint32_t programEnd() { return kFlashBase + static_cast<uint32_t>(firmwarePartition()); }

bool allErased(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != 0xFF) return false;
    return true;
}

// One sector of the staged image, written page by page; its block is erased as the upload enters it, unless already erased.
void stageSector(uint32_t addr) {
    if (addr % kBlockBytes == 0 && !allErased(reinterpret_cast<const uint8_t*>(addr), kBlockBytes))
        eepromemu_flash_erase_64K_block(reinterpret_cast<void*>(addr));
    for (uint32_t p = 0; p < kSectorBytes; p += kPageBytes)
        if (!allErased(g_sector + p, kPageBytes)) eepromemu_flash_write(reinterpret_cast<void*>(addr + p), g_sector + p, kPageBytes);
}

// Copies the staged image over the program and resets, from ITCM with interrupts off; never inlined, since its caller runs from the flash it erases.
FASTRUN __attribute__((noinline)) [[noreturn]] void copyStagedImageAndReset(uint32_t from, uint32_t bytes) {
    __disable_irq();
    SYST_CSR = 0;
    for (uint32_t i = 0; i < (NVIC_NUM_INTERRUPTS + 31) / 32; i++) {
        (&NVIC_ICER0)[i] = 0xFFFFFFFF;   // disable every source, which the flash routines' __enable_irq cannot undo
        (reinterpret_cast<volatile uint32_t*>(0xE000E280))[i] = 0xFFFFFFFF;   // NVIC_ICPR: drop what is already pending
    }
    for (uint32_t block = 0; block < bytes; block += kBlockBytes) {
        WDOG1_WSR = 0x5555;
        WDOG1_WSR = 0xAAAA;
        const uint32_t end = bytes - block < kBlockBytes ? bytes : block + kBlockBytes;
        const uint32_t* src = reinterpret_cast<const uint32_t*>(from + block);
        const uint32_t* dst = reinterpret_cast<const uint32_t*>(kFlashBase + block);
        bool same = true;
        for (uint32_t w = 0; same && w < (end - block) / 4; w++) same = dst[w] == src[w];
        if (same) continue;   // a block the new image shares with the running one is left alone
        eepromemu_flash_erase_64K_block(reinterpret_cast<void*>(kFlashBase + block));
        for (uint32_t off = block; off < end; off += kSectorBytes) {
            const uint32_t* sector = reinterpret_cast<const uint32_t*>(from + off);
            uint32_t* buf = reinterpret_cast<uint32_t*>(g_sector);
            for (uint32_t w = 0; w < kSectorBytes / 4; w++) buf[w] = sector[w];
            for (uint32_t p = 0; p < kSectorBytes; p += kPageBytes) {
                bool erased = true;
                for (uint32_t b = 0; b < kPageBytes; b++) erased = erased && g_sector[p + b] == 0xFF;
                if (!erased) eepromemu_flash_write(reinterpret_cast<void*>(kFlashBase + off + p), g_sector + p, kPageBytes);
            }
        }
    }
    SCB_AIRCR = 0x05FA0004;   // SYSRESETREQ: boot the new image
    for (;;) {}
}
}  // namespace

// Called by reboot(): a checked upload waits for this moment, so the HTTP answer goes out before the program is replaced.
void applyStagedFirmware() {
    if (g_stagedBytes) copyStagedImageAndReset(stagingBase(), g_stagedBytes);
}

namespace {
// One sector of the upload into g_sector, its tail padded with what erased flash reads; false when the client aborted.
bool readSector(FsWriteSrc src, void* user, uint32_t& fill, bool& eof) {
    fill = 0;
    eof = false;
    while (fill < kSectorBytes) {
        bool abort = false;
        const size_t n = src(reinterpret_cast<char*>(g_sector + fill), kSectorBytes - fill, user, &abort);
        if (abort) return false;
        if (n == 0) { eof = true; break; }
        fill += static_cast<uint32_t>(n);
    }
    std::memset(g_sector + fill, 0xFF, kSectorBytes - fill);
    return true;
}

}  // namespace

bool otaWriteStream(FsWriteSrc src, void* user, size_t contentLen,
                    char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut) {
    if (!src || !statusBuf || statusBufLen == 0 || !bytesReadOut) return false;
    // A macro rather than a lambda, so the format reaches snprintf as a literal and keeps its check.
    #define setStatus(...) std::snprintf(statusBuf, statusBufLen, __VA_ARGS__)
    g_stagedBytes = 0;
    const uint32_t base = stagingBase();
    const uint32_t room = programEnd() - base;
    if (contentLen > room) {
        setStatus("error: image too large (%u > %u)", static_cast<unsigned>(contentLen), static_cast<unsigned>(room));
        return false;
    }
    setStatus("flashing");
    uint32_t written = 0;
    for (;;) {
        uint32_t fill = 0;
        bool eof = false;
        if (!readSector(src, user, fill, eof)) { setStatus("error: upload aborted"); return false; }
        if (fill == 0) break;
        const char* why = (written == 0) ? firmware::teensyHeaderProblem(g_sector, kSectorBytes) : nullptr;
        if (why) { setStatus("%s", why); return false; }
        if (written + fill > room) { setStatus("error: image too large for the free flash"); return false; }
        stageSector(base + written);
        written += fill;
        *bytesReadOut = written;
        taskWdtReset();   // a megabyte of erases runs on the render loop, which the watchdog guards
        if (eof) break;
    }
    if (contentLen && written < contentLen) {
        setStatus("error: incomplete upload (%u/%u)", static_cast<unsigned>(written), static_cast<unsigned>(contentLen));
        return false;
    }
    if (firmware::teensyImageRefused(reinterpret_cast<const uint8_t*>(base), written, kImageMark, kMarkPrefixLen,
                                     MM_FIRMWARE_NAME, statusBuf, statusBufLen)) return false;
    g_stagedBytes = written;
    setStatus("rebooting");
    return true;
    #undef setStatus
}

// GitHub serves release assets over HTTPS only, which this board cannot speak.
bool http_fetch_to_ota(const char*, char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut, uint32_t* bytesTotalOut, const char*) {
    if (statusBuf && statusBufLen) std::snprintf(statusBuf, statusBufLen, "error: no TLS on this board, upload the .bin instead");
    if (bytesReadOut) *bytesReadOut = 0;
    if (bytesTotalOut) *bytesTotalOut = 0;
    return false;
}

// One image and no recovery image beside it: the MoonBase questions all answer no.
bool otaWriteMoonBase(FsWriteSrc, void*, size_t, char* statusBuf, size_t statusBufLen, uint32_t*) {
    if (statusBuf && statusBufLen) std::snprintf(statusBuf, statusBufLen, "error: no MoonBase on this board");
    return false;
}
bool otaFetchMoonBaseUrl(const char*, char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    return http_fetch_to_ota(nullptr, statusBuf, statusBufLen, bytesReadOut, bytesTotalOut, nullptr);
}
bool moonbaseStageInstallUrl(const char*) { return false; }
bool otaHasMoonBase() { return false; }
bool otaBootMoonBase() { return false; }
bool otaRunningMoonBase() { return false; }
bool otaMoonBaseVersion(char*, size_t) { return false; }
bool otaMoonBaseBuild(char*, size_t) { return false; }
bool otaMoonBaseSize(uint32_t*, uint32_t*) { return false; }
void moonbaseClearStagedUrl() {}

}  // namespace mm::platform

/// @}
