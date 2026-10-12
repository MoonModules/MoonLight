/// @defgroup platform_esp32_sdmmc SD card mount and the `/sd` backend
/// The microSD mount sequence and the file API behind every `/sd`-prefixed `fsXxx` call.
///
/// Dispatch from the shared `fsXxx` surface lives in platform_esp32_fs.cpp; this file owns only the card itself.
///
/// @moreinfo
///
/// ## The pins are a runtime argument, not a board constant
///
/// `sdMount()` takes an `SdPinConfig`, so no board gets its own `#ifdef`; `platform::hasSdCard` gates only on whether this chip has the hardware at all.
/// The bus width follows from which data lines are set: `d1`/`d2`/`d3` unset drops it to 1-bit mode, all three set is 4-bit.
///
/// ## Built field by field
///
/// `SDMMC_SLOT_CONFIG_DEFAULT()` hardwires `d4` to a fixed GPIO, an 8-bit-mode line this driver never uses in 1- or 4-bit mode.
/// On the Waveshare P4-ETH that pin is the card's own power switch, so the macro is skipped and the slot config is built field by field instead.
///
/// ## The power switch is driven before the mount
///
/// `pwr`, where a board wires one, is a plain GPIO output set low (P-channel high-side-switch convention) before `esp_vfs_fat_sdmmc_mount`, and high again on unmount or a failed mount.
///
/// ## Internal pullups on CMD/D0-D3
///
/// `SDMMC_SLOT_FLAG_INTERNAL_PULLUP` is set unconditionally, matching the reference wiring; harmless where the lines already have external pull-ups.
///
/// ## The chip's own IO power domain, where one exists
///
/// A `SOC_SDMMC_IO_POWER_EXTERNAL` chip's SDMMC IO lines are powered by an on-chip LDO the driver must turn on itself; this is a chip fact, not a board wiring choice.
///
/// ## CD and WP read as GPIO0 unless told otherwise
///
/// A zero-initialized slot config leaves `.cd`/`.wp` at GPIO0, which the driver then reads as a real write-protect signal and rejects every write.
/// Building the config field by field means the macro's own `SDMMC_SLOT_NO_CD`/`NO_WP` defaults are skipped too, so both are set explicitly here.
///
/// ## Drive 0 is assumed, not queried
///
/// File access below is raw FatFs (`f_open`/`f_read`/`f_write`), not stdio through the VFS mount, to drop stdio's share of the flash cost.
/// `esp_vfs_fat_sdmmc_mount` assigns the FatFs drive number dynamically and never returns it, so there is no public call to ask which one this mount got.
/// Drive 0 is assumed because nothing else in this firmware registers a FatFs volume (LittleFS is a separate component); if that ever changes, this assumption needs revisiting.
/// @{

#include "platform_esp32_sdmmc.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include <cstdio>    // snprintf only; file I/O below is raw FatFs, not stdio
#include <cstring>

#if defined(CONFIG_SOC_SDMMC_HOST_SUPPORTED) && defined(CONFIG_MM_SD_CARD)
#define MM_HAS_SDMMC 1
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "soc/soc_caps.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#endif

namespace mm::platform {

#if MM_HAS_SDMMC

static constexpr const char* SD_TAG = "mm_sd";
static constexpr const char* SD_MOUNT_POINT = "/sdcard";   // the VFS registration below needs one, even though raw FatFs calls never go through it
// Raw FatFs addresses a volume by drive number, not the VFS path above: @xref{drive-0-is-assumed-not-queried}.
static constexpr const char* SD_DRIVE = "0:";

static sdmmc_card_t* g_card = nullptr;
static bool g_mounted = false;
static int g_streamsOpen = 0;         // every open read/write stream, counted so unmount/format can refuse under one
static int8_t g_pwrGpio = -1;         // so unmount powers back down what mount powered up
// Every call below runs to completion before the next starts, so one shared FIL off the stack serves them all; the kept-open streams further down heap-allocate their own.
static FIL g_fil;
#if SOC_SDMMC_IO_POWER_EXTERNAL
static sd_pwr_ctrl_handle_t g_pwrCtrl = nullptr;
#endif

/// Strip the leading `/sd` and map what remains onto the FatFs drive path (`FF_FS_RPATH` is off, so every call needs the full `0:/...` form); false on truncation.
static bool sdTranslate(const char* apiPath, char* out, size_t outLen) {
    if (outLen == 0 || !apiPath) return false;
    const char* p = apiPath;
    if (*p == '/') p++;
    p += 2;                               // the caller already confirmed this is "sd" or "sd/..."
    if (*p == '/') p++;
    const int n = std::snprintf(out, outLen, "%s/%s", SD_DRIVE, p);
    if (n < 0 || static_cast<size_t>(n) >= outLen) { out[0] = 0; return false; }
    return true;
}

bool sdCardMounted() { return g_mounted; }

// A static scratch sector, not a stack buffer: keeps sdCardPresent's own frame off the 512-byte stack-frame rule, and this runs on one thread only.
static uint8_t g_presenceProbe[512];

// A 1-sector read, not SEND_STATUS (CMD13): CMD13 panics this chip's SDMMC host on this ESP-IDF build; a block read is the primitive real file access already proves safe.
bool sdCardPresent() {
    if (!g_mounted || !g_card) return false;
    if (g_card->csd.sector_size > sizeof(g_presenceProbe)) return false;   // never true for a real SD/SDHC/SDXC card; refuses rather than overflowing the scratch sector
    return sdmmc_read_sectors(g_card, g_presenceProbe, 0, 1) == ESP_OK;
}

bool sdCardBusy() { return g_streamsOpen > 0; }

bool sdCardBootsByDefault() { return hasSdCard; }

/// A pin this build never wires reads as -1; one it does but the chip refuses is a mount-time mistake, not UB.
static bool validOutputPin(int8_t pin) {
    return pin >= 0 && GPIO_IS_VALID_OUTPUT_GPIO(pin);
}

bool sdMount(const SdPinConfig& pins) {
    if (g_mounted) return true;
    if (!validOutputPin(pins.clk) || !validOutputPin(pins.cmd) || !validOutputPin(pins.d0)) return false;
    const bool fourBit = pins.d1 >= 0 && pins.d2 >= 0 && pins.d3 >= 0;
    if (fourBit && (!validOutputPin(pins.d1) || !validOutputPin(pins.d2) || !validOutputPin(pins.d3))) return false;
    if (pins.pwr >= 0 && !validOutputPin(pins.pwr)) return false;

    // Powered before anything talks to the slot, where this board wires a switch: @xref{the-power-switch-is-driven-before-the-mount}.
    if (pins.pwr >= 0) {
        gpio_config_t pwrCfg = {};
        pwrCfg.pin_bit_mask = 1ULL << pins.pwr;
        pwrCfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&pwrCfg);
        gpio_set_level(static_cast<gpio_num_t>(pins.pwr), 0);   // LOW = enabled
    }
    g_pwrGpio = pins.pwr;

    esp_vfs_fat_sdmmc_mount_config_t mountCfg = {};
    mountCfg.format_if_mount_failed = false;   // a blank/foreign card stays untouched
    mountCfg.max_files = 5;
    mountCfg.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

#if SOC_SDMMC_IO_POWER_EXTERNAL
    sd_pwr_ctrl_ldo_config_t ldoCfg = {};
    ldoCfg.ldo_chan_id = 4;
    const esp_err_t ldoErr = sd_pwr_ctrl_new_on_chip_ldo(&ldoCfg, &g_pwrCtrl);
    if (ldoErr != ESP_OK) {
        ESP_LOGE(SD_TAG, "SD LDO power control failed: %s", esp_err_to_name(ldoErr));
        if (g_pwrGpio >= 0) gpio_set_level(static_cast<gpio_num_t>(g_pwrGpio), 1);
        return false;
    }
    host.pwr_ctrl_handle = g_pwrCtrl;
#endif

    // Field by field, not SDMMC_SLOT_CONFIG_DEFAULT(): @xref{built-field-by-field}.
    sdmmc_slot_config_t slotCfg = {};
    slotCfg.clk = static_cast<gpio_num_t>(pins.clk);
    slotCfg.cmd = static_cast<gpio_num_t>(pins.cmd);
    slotCfg.d0  = static_cast<gpio_num_t>(pins.d0);
    if (fourBit) {
        slotCfg.d1 = static_cast<gpio_num_t>(pins.d1);
        slotCfg.d2 = static_cast<gpio_num_t>(pins.d2);
        slotCfg.d3 = static_cast<gpio_num_t>(pins.d3);
    }
    slotCfg.width = fourBit ? 4 : 1;
    slotCfg.cd = SDMMC_SLOT_NO_CD;              // @xref{cd-and-wp-read-as-gpio0-unless-told-otherwise}
    slotCfg.wp = SDMMC_SLOT_NO_WP;
    slotCfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slotCfg, &mountCfg, &g_card);
    if (err != ESP_OK) {
        ESP_LOGE(SD_TAG, "SD mount failed: %s", esp_err_to_name(err));
#if SOC_SDMMC_IO_POWER_EXTERNAL
        sd_pwr_ctrl_del_on_chip_ldo(g_pwrCtrl);
        g_pwrCtrl = nullptr;
#endif
        if (g_pwrGpio >= 0) gpio_set_level(static_cast<gpio_num_t>(g_pwrGpio), 1);
        g_card = nullptr;
        return false;
    }

    g_mounted = true;
    ESP_LOGI(SD_TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return true;
}

bool sdUnmount() {
    if (!g_mounted) return true;
    if (g_streamsOpen > 0) return false;   // a transfer holds a FIL into the context this would free
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, g_card);
#if SOC_SDMMC_IO_POWER_EXTERNAL
    if (g_pwrCtrl) { sd_pwr_ctrl_del_on_chip_ldo(g_pwrCtrl); g_pwrCtrl = nullptr; }
#endif
    if (g_pwrGpio >= 0) gpio_set_level(static_cast<gpio_num_t>(g_pwrGpio), 1);
    g_card = nullptr;
    g_mounted = false;
    return true;
}

bool sdFormat() {
    if (!g_mounted || !g_card) return false;
    if (g_streamsOpen > 0) return false;   // same conflict as an unmount under an open stream
    // Formats and remounts in one call; the mount stays live either way.
    return esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, g_card) == ESP_OK;
}

uint64_t sdCardUsed() {
    if (!g_mounted) return 0;
    FATFS* fs = nullptr;
    DWORD freeClusters = 0;
    if (f_getfree(SD_DRIVE, &freeClusters, &fs) != FR_OK || !fs) return 0;
    const uint64_t totalSectors = (static_cast<uint64_t>(fs->n_fatent) - 2) * fs->csize;
    const uint64_t freeSectors = static_cast<uint64_t>(freeClusters) * fs->csize;
    return (totalSectors - freeSectors) * fs->ssize;
}

uint64_t sdCardTotal() {
    if (!g_mounted || !g_card) return 0;
    return static_cast<uint64_t>(g_card->csd.capacity) * static_cast<uint64_t>(g_card->csd.sector_size);
}

// ---- Routine file ops: raw FatFs, @xref{drive-0-is-assumed-not-queried}. ----

/// mkdir -p: f_mkdir on each path component in turn; FR_EXIST on one already there is success, not failure.
bool sdCardMkdir(const char* apiPath) {
    if (!g_mounted) return false;
    char full[128];
    if (!sdTranslate(apiPath, full, sizeof(full))) return false;
    char* p = full + std::strlen(SD_DRIVE) + 1;   // skip "0:/"
    while (*p) {
        if (*p == '/') {
            *p = 0;
            f_mkdir(full);   // ignore result; could already exist
            *p = '/';
        }
        p++;
    }
    const FRESULT r = f_mkdir(full);
    return r == FR_OK || r == FR_EXIST;
}

bool sdCardExists(const char* apiPath) {
    if (!g_mounted) return false;
    char full[128];
    if (!sdTranslate(apiPath, full, sizeof(full))) return false;
    FILINFO fno;
    return f_stat(full, &fno) == FR_OK;
}

// f_unlink removes a file or an EMPTY directory; FatFs refuses one with children, same contract fsRemove already promises.
bool sdCardRemove(const char* apiPath) {
    if (!g_mounted) return false;
    char full[128];
    if (!sdTranslate(apiPath, full, sizeof(full))) return false;
    return f_unlink(full) == FR_OK;
}

int sdCardRead(const char* apiPath, char* buf, size_t maxLen) {
    if (!g_mounted || !buf || maxLen == 0) return -1;
    char full[128];
    if (!sdTranslate(apiPath, full, sizeof(full))) return -1;
    if (f_open(&g_fil, full, FA_READ) != FR_OK) return -1;
    UINT br = 0;
    const FRESULT r = f_read(&g_fil, buf, static_cast<UINT>(maxLen - 1), &br);
    f_close(&g_fil);
    if (r != FR_OK) return -1;
    buf[br] = 0;
    return static_cast<int>(br);
}

long sdCardSize(const char* apiPath) {
    if (!g_mounted) return -1;
    char full[128];
    if (!sdTranslate(apiPath, full, sizeof(full))) return -1;
    FILINFO fno;
    if (f_stat(full, &fno) != FR_OK || (fno.fattrib & AM_DIR)) return -1;
    return static_cast<long>(fno.fsize);
}

/// The folder a write lands in, made first, as the write's contract promises.
static void sdMakeParent(const char* apiPath) {
    if (!apiPath) return;
    const char* slash = std::strrchr(apiPath, '/');
    if (!slash || slash == apiPath) return;
    char dir[128];
    const size_t n = static_cast<size_t>(slash - apiPath);
    if (n >= sizeof(dir)) return;
    std::memcpy(dir, apiPath, n);
    dir[n] = 0;
    sdCardMkdir(dir);
}

// f_rename refuses an existing destination, unlike POSIX rename; the old file is unlinked first, with the brief gap as FAT's cost for having no atomic replace.
static bool sdCommitTmp(const char* tmp, const char* full) {
    f_unlink(full);
    if (f_rename(tmp, full) == FR_OK) return true;
    f_unlink(tmp);
    return false;
}

bool sdCardWriteAtomic(const char* apiPath, const char* data, size_t len) {
    if (!g_mounted) return false;
    if (len > 0 && !data) return false;
    sdMakeParent(apiPath);
    char full[128];
    char tmp[136];
    if (!sdTranslate(apiPath, full, sizeof(full))) return false;
    int n = std::snprintf(tmp, sizeof(tmp), "%s.tmp", full);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(tmp)) return false;

    if (f_open(&g_fil, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT bw = 0;
    const FRESULT wr = f_write(&g_fil, data, static_cast<UINT>(len), &bw);
    f_close(&g_fil);
    if (wr != FR_OK || bw != len) { f_unlink(tmp); return false; }
    return sdCommitTmp(tmp, full);
}

void sdCardList(const char* apiDir, FsListCb cb, void* user) {
    if (!g_mounted || !cb) return;
    char full[128];
    if (!sdTranslate(apiDir, full, sizeof(full))) return;
    FF_DIR dir;
    if (f_opendir(&dir, full) != FR_OK) return;
    FILINFO fno;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        const bool isDir = (fno.fattrib & AM_DIR) != 0;
        const uint32_t size = isDir ? 0 : static_cast<uint32_t>(fno.fsize);
        cb(fno.fname, isDir, size, user);
    }
    f_closedir(&dir);
}

// ---- Kept-open streams: one FIL across every chunk, not a reopen per chunk. ----

bool sdCardReadStreamOpen(void*& handle, const char* apiPath) {
    if (!g_mounted) return false;
    char full[128];
    if (!sdTranslate(apiPath, full, sizeof(full))) return false;
    auto* f = new FIL();
    if (f_open(f, full, FA_READ) != FR_OK) { delete f; return false; }
    handle = f;
    ++g_streamsOpen;
    return true;
}

int sdCardReadStreamRead(void* handle, char* buf, size_t len) {
    auto* f = static_cast<FIL*>(handle);
    if (!f) return -1;
    UINT br = 0;
    if (f_read(f, buf, static_cast<UINT>(len), &br) != FR_OK) return -1;
    return static_cast<int>(br);
}

void sdCardReadStreamClose(void* handle) {
    auto* f = static_cast<FIL*>(handle);
    if (!f) return;
    f_close(f);
    delete f;
    --g_streamsOpen;
}

// Carries both paths, not just the FIL, so close() can rename or discard without re-deriving them.
struct SdWriteHandle {
    FIL f;
    char full[128] = {};
    char tmp[136] = {};
};

bool sdCardWriteStreamOpen(void*& handle, const char* apiPath) {
    if (!g_mounted) return false;
    sdMakeParent(apiPath);
    auto* h = new SdWriteHandle();
    if (!sdTranslate(apiPath, h->full, sizeof(h->full))) { delete h; return false; }
    const int n = std::snprintf(h->tmp, sizeof(h->tmp), "%s.tmp", h->full);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(h->tmp)) { delete h; return false; }
    if (f_open(&h->f, h->tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) { delete h; return false; }
    handle = h;
    ++g_streamsOpen;
    return true;
}

bool sdCardWriteStreamWrite(void* handle, const char* buf, size_t len) {
    auto* h = static_cast<SdWriteHandle*>(handle);
    if (!h) return false;
    UINT bw = 0;
    return f_write(&h->f, buf, static_cast<UINT>(len), &bw) == FR_OK && bw == len;
}

bool sdCardWriteStreamClose(void* handle, bool commit) {
    auto* h = static_cast<SdWriteHandle*>(handle);
    if (!h) return false;
    f_close(&h->f);
    const bool ok = commit ? sdCommitTmp(h->tmp, h->full) : true;
    if (!commit) f_unlink(h->tmp);
    delete h;
    --g_streamsOpen;
    return ok;
}

#else  // !MM_HAS_SDMMC: every symbol below is inert so a board with no SDMMC host still links.

bool sdCardMounted() { return false; }
bool sdCardPresent() { return false; }
bool sdMount(const SdPinConfig&) { return false; }
bool sdCardBusy() { return false; }
bool sdCardBootsByDefault() { return false; }
bool sdUnmount() { return true; }
bool sdFormat() { return false; }
uint64_t sdCardUsed() { return 0; }
uint64_t sdCardTotal() { return 0; }
bool sdCardMkdir(const char*) { return false; }
bool sdCardExists(const char*) { return false; }
bool sdCardRemove(const char*) { return false; }
int  sdCardRead(const char*, char*, size_t) { return -1; }
long sdCardSize(const char*) { return -1; }
bool sdCardWriteAtomic(const char*, const char*, size_t) { return false; }
void sdCardList(const char*, FsListCb, void*) {}
bool sdCardReadStreamOpen(void*&, const char*) { return false; }
int  sdCardReadStreamRead(void*, char*, size_t) { return -1; }
void sdCardReadStreamClose(void*) {}
bool sdCardWriteStreamOpen(void*&, const char*) { return false; }
bool sdCardWriteStreamWrite(void*, const char*, size_t) { return false; }
bool sdCardWriteStreamClose(void*, bool) { return false; }

#endif  // MM_HAS_SDMMC

} // namespace mm::platform

/// @}
