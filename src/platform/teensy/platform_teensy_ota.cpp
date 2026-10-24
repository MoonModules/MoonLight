/// An uploaded flat image is staged in the free program flash above the running one, and checked. At the next reboot a routine running from RAM copies it over the program.
///
/// @technical
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

// The firmware name an upload must carry, MM_FIRMWARE_NAME from teensy/CMakeLists.txt: @xref{the-image-mark}.
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
// The one image under way: where it stages, how much is in, and where its progress is reported.
struct Image {
    uint32_t base = 0;
    uint32_t room = 0;
    uint32_t written = 0;      // staged bytes
    uint32_t fill = 0;         // bytes in g_sector, waiting to be staged
    size_t contentLen = 0;
    char* status = nullptr;
    size_t statusLen = 0;
    uint32_t* bytesOut = nullptr;
};
Image g_image;

// Stage what g_sector holds, its tail padded with what erased flash reads; the first sector's header is checked before a byte lands.
bool stageFill(Image& img) {
    if (!img.fill) return true;
    std::memset(g_sector + img.fill, 0xFF, kSectorBytes - img.fill);
    const char* why = img.written == 0 ? firmware::teensyHeaderProblem(g_sector, kSectorBytes) : nullptr;
    if (why) { std::snprintf(img.status, img.statusLen, "%s", why); return false; }
    if (img.written + img.fill > img.room) { std::snprintf(img.status, img.statusLen, "error: image too large for the free flash"); return false; }
    stageSector(img.base + img.written);
    img.written += img.fill;
    *img.bytesOut = img.written;
    img.fill = 0;
    return true;
}
}  // namespace

// One board, one image: the writer is the static above, so a second begin restarts it.
bool otaBegin(OtaWrite& w, OtaTarget target, size_t contentLen, char* statusBuf, size_t statusBufLen, uint32_t* bytesOut) {
    if (!statusBuf || statusBufLen == 0 || !bytesOut) return false;
    // One image and no recovery image beside it.
    if (target == OtaTarget::MoonBase) { std::snprintf(statusBuf, statusBufLen, "error: no MoonBase on this board"); return false; }
    g_stagedBytes = 0;
    g_image = Image{stagingBase(), programEnd() - stagingBase(), 0, 0, contentLen, statusBuf, statusBufLen, bytesOut};
    if (contentLen > g_image.room) {
        std::snprintf(statusBuf, statusBufLen, "error: image too large (%u > %u)", static_cast<unsigned>(contentLen), static_cast<unsigned>(g_image.room));
        return false;
    }
    std::snprintf(statusBuf, statusBufLen, "flashing");
    *bytesOut = 0;
    w.impl = &g_image;
    return true;
}

bool otaWrite(OtaWrite& w, const uint8_t* data, size_t len) {
    auto* img = static_cast<Image*>(w.impl);
    if (!img) return false;
    while (len) {
        const size_t n = len < kSectorBytes - img->fill ? len : kSectorBytes - img->fill;
        std::memcpy(g_sector + img->fill, data, n);
        img->fill += static_cast<uint32_t>(n);
        data += n;
        len -= n;
        if (img->fill == kSectorBytes && !stageFill(*img)) { w.impl = nullptr; return false; }
    }
    return true;
}

bool otaEnd(OtaWrite& w, bool commit) {
    auto* img = static_cast<Image*>(w.impl);
    w.impl = nullptr;
    if (!img) return false;
    if (!commit) { std::snprintf(img->status, img->statusLen, "error: upload aborted"); return false; }
    if (!stageFill(*img)) return false;
    if (img->contentLen && img->written < img->contentLen) {
        std::snprintf(img->status, img->statusLen, "error: incomplete upload (%u/%u)", static_cast<unsigned>(img->written), static_cast<unsigned>(img->contentLen));
        return false;
    }
    if (firmware::teensyImageRefused(reinterpret_cast<const uint8_t*>(img->base), img->written, kImageMark, kMarkPrefixLen,
                                     MM_FIRMWARE_NAME, img->status, img->statusLen)) return false;
    g_stagedBytes = img->written;
    std::snprintf(img->status, img->statusLen, "rebooting");
    return true;
}

// GitHub serves release assets over HTTPS only, which this board cannot speak.
bool http_fetch_to_ota(const char*, char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut, uint32_t* bytesTotalOut, const char*) {
    if (statusBuf && statusBufLen) std::snprintf(statusBuf, statusBufLen, "error: no TLS on this board, upload the .bin instead");
    if (bytesReadOut) *bytesReadOut = 0;
    if (bytesTotalOut) *bytesTotalOut = 0;
    return false;
}

// One image and no recovery image beside it: the MoonBase questions all answer no.
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
