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
///
/// ## Built field by field
///
/// `SDMMC_SLOT_CONFIG_DEFAULT()` hardwires `d4`, an unused 8-bit line, to the GPIO that powers the card on the Waveshare P4-ETH. So the slot config is built field by field, and sets the macro's `SDMMC_SLOT_NO_CD`/`NO_WP` itself, since zero would read GPIO0 as write-protect.
///
/// ## The power switch is driven before the mount
///
/// `pwr`, where a board wires one, is a plain GPIO output set low (P-channel high-side-switch convention) before `esp_vfs_fat_sdmmc_mount`, and high again on unmount or a failed mount.
///
/// ## A format runs on its own task
///
/// Writing a large card's FAT tables outlasts the render loop's task watchdog, whose panic leaves a half-written volume. So the format runs on a one-shot unwatched task, with the card unmounted until it publishes its result.
///
/// ## A write lands in a staging folder first
///
/// Every write goes to `/sd/.tmp` and is renamed into place once whole, so a failed upload never leaves a partial file under its real name. A card that stops answering mid-write cannot delete its temporary file either, so a mount empties that one folder rather than walking the whole card.
///
/// ## Drive 0 is assumed, not queried
///
/// File access below is raw FatFs (`f_open`/`f_read`/`f_write`), not stdio through the VFS mount, to drop stdio's share of the flash cost.
///
/// `esp_vfs_fat_sdmmc_mount` assigns the FatFs drive number and never returns it, so drive 0 is assumed: nothing else in this firmware registers a FatFs volume, LittleFS being a separate component.
/// @{

#include "platform_esp32_sdmmc.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include <atomic>
#include <new>
#include <cstdio>    // snprintf only; file I/O below is raw FatFs, not stdio
#include <cstring>

#if defined(CONFIG_SOC_SDMMC_HOST_SUPPORTED) && defined(CONFIG_MM_SD_CARD)
#define MM_HAS_SDMMC 1
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ff.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "soc/soc_caps.h"
#include "soc/sdmmc_pins.h"
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
static std::atomic<bool> g_mounted{false};   // atomic, since the format task publishes it
static int g_streamsOpen = 0;         // every open read/write stream, counted so unmount/format can refuse under one
static SdMountError g_mountError = SdMountError::None;   // why the last mount failed
static std::atomic<SdFormatState> g_formatState{SdFormatState::Idle};
static bool g_formatUnmounted = false;   // whether the running format mounts a card that had no filesystem; set before the task starts
static SdPinConfig g_lastPins{};   // what a format of a filesystem-less card mounts on
static int8_t g_pwrGpio = -1;         // so unmount powers back down what mount powered up
static bool g_spiBus = false;         // whether the mount brought up the SPI bus, which unmount frees
// SPI3 where the chip has it: the S3's W5500 Ethernet holds SPI2.
static constexpr spi_host_device_t kSdSpiHost = SOC_SPI_PERIPH_NUM > 2 ? SPI3_HOST : SPI2_HOST;
// What only a mounted card needs, allocated at mount and freed at unmount, so a board without a card holds none of it.
struct SdScratch {
    FIL fil;                          // every call below runs to completion before the next, so one FIL serves them all; kept-open streams allocate their own
    alignas(4) uint8_t probe[512];    // sdCardPresent's sector, word-aligned or sdmmc_read_sectors bounces it through another buffer
};
static SdScratch* g_scratch = nullptr;
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

// A 1-sector read, not SEND_STATUS (CMD13): CMD13 panics this chip's SDMMC host on this ESP-IDF build; a block read is the primitive real file access already proves safe.
bool sdCardPresent() {
    if (!g_mounted || !g_card) return false;
    if (g_card->csd.sector_size > static_cast<int>(sizeof(g_scratch->probe))) return false;   // never true for a real SD/SDHC/SDXC card; refuses rather than overflowing the scratch sector
    return sdmmc_read_sectors(g_card, g_scratch->probe, 0, 1) == ESP_OK;
}

bool sdCardBusy() { return g_streamsOpen > 0 || g_formatState == SdFormatState::Running; }

bool sdCardBootsByDefault() { return hasSdCard; }

/// A pin this build never wires reads as -1; one it does but the chip refuses is a mount-time mistake, not UB.
static bool validOutputPin(int8_t pin) {
    return pin >= 0 && GPIO_IS_VALID_OUTPUT_GPIO(pin);
}
/// SPI's MISO only reads, so an input-only pin such as the classic's GPIO36 serves.
static bool validInputPin(int8_t pin) {
    return pin >= 0 && GPIO_IS_VALID_GPIO(pin);
}

namespace {
// The data lines for 4-bit mode, all three set; otherwise the bus runs 1-bit.
bool fourBitPins(const SdPinConfig& pins) { return pins.d1 >= 0 && pins.d2 >= 0 && pins.d3 >= 0; }

// The SD bus drives D0, and D1-D3 too in 4-bit mode.
bool usableSdBusPins(const SdPinConfig& pins) {
    if (!validOutputPin(pins.d0)) return false;
    return !fourBitPins(pins) || (validOutputPin(pins.d1) && validOutputPin(pins.d2) && validOutputPin(pins.d3));
}

// Every pin the mount drives is an output-capable GPIO, and SPI's MISO one it can read; an unset power switch passes.
bool usablePins(const SdPinConfig& pins) {
    if (!validOutputPin(pins.clk) || !validOutputPin(pins.cmd)) return false;
    if (pins.pwr >= 0 && !validOutputPin(pins.pwr)) return false;
    if (pins.bus == SdBus::Spi) return validInputPin(pins.d0) && validOutputPin(pins.d3);
    return usableSdBusPins(pins);
}

#ifdef SDMMC_SLOT0_IOMUX_PIN_NUM_CLK
// Slot 0's own IOMUX pins, which the P4's slot 0 alone reaches: its default slot 1 routes them through the GPIO matrix instead.
bool onSlot0Iomux(const SdPinConfig& pins) {
    if (pins.clk != SDMMC_SLOT0_IOMUX_PIN_NUM_CLK || pins.cmd != SDMMC_SLOT0_IOMUX_PIN_NUM_CMD || pins.d0 != SDMMC_SLOT0_IOMUX_PIN_NUM_D0) return false;
    return !fourBitPins(pins) || (pins.d1 == SDMMC_SLOT0_IOMUX_PIN_NUM_D1 && pins.d2 == SDMMC_SLOT0_IOMUX_PIN_NUM_D2 && pins.d3 == SDMMC_SLOT0_IOMUX_PIN_NUM_D3);
}
#endif

// Powered before anything talks to the slot, where this board wires a switch: @xref{the-power-switch-is-driven-before-the-mount}.
void powerSlot(int8_t pwr) {
    g_pwrGpio = pwr;
    if (pwr < 0) return;
    gpio_config_t pwrCfg = {};
    pwrCfg.pin_bit_mask = 1ULL << pwr;
    pwrCfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&pwrCfg);
    gpio_set_level(static_cast<gpio_num_t>(pwr), 0);   // LOW = enabled
}

// Field by field, not SDMMC_SLOT_CONFIG_DEFAULT(): @xref{built-field-by-field}.
sdmmc_slot_config_t slotConfig(const SdPinConfig& pins) {
    sdmmc_slot_config_t slotCfg = {};
    slotCfg.clk = static_cast<gpio_num_t>(pins.clk);
    slotCfg.cmd = static_cast<gpio_num_t>(pins.cmd);
    slotCfg.d0  = static_cast<gpio_num_t>(pins.d0);
    const bool fourBit = fourBitPins(pins);
    if (fourBit) {
        slotCfg.d1 = static_cast<gpio_num_t>(pins.d1);
        slotCfg.d2 = static_cast<gpio_num_t>(pins.d2);
        slotCfg.d3 = static_cast<gpio_num_t>(pins.d3);
    }
    slotCfg.width = fourBit ? 4 : 1;
    slotCfg.cd = SDMMC_SLOT_NO_CD;              // @xref{built-field-by-field}
    slotCfg.wp = SDMMC_SLOT_NO_WP;
    slotCfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   // the reference wiring's, harmless over external pull-ups
    return slotCfg;
}

// The card on the SPI bus, its CS on D3 per the SD spec's SPI pin mapping.
sdspi_device_config_t spiDevice(const SdPinConfig& pins) {
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = kSdSpiHost;
    dev.gpio_cs = static_cast<gpio_num_t>(pins.d3);
    return dev;
}

// The host the wiring's bus calls for, on slot 0 where the pins are that slot's own.
sdmmc_host_t hostFor(const SdPinConfig& pins) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    if (pins.bus == SdBus::Spi) {
        host = SDSPI_HOST_DEFAULT();
        host.slot = kSdSpiHost;
    }
#ifdef SDMMC_SLOT0_IOMUX_PIN_NUM_CLK
    else if (onSlot0Iomux(pins)) host.slot = SDMMC_HOST_SLOT_0;
#endif
    return host;
}

// Bring up the SPI bus the card sits on, or nothing on the SD bus; false when the bus will not start.
bool startBus(const SdPinConfig& pins) {
    if (pins.bus != SdBus::Spi) return true;
    spi_bus_config_t bus = {};
    bus.mosi_io_num = pins.cmd;
    bus.miso_io_num = pins.d0;
    bus.sclk_io_num = pins.clk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;   // the IDF SD SPI example's own figure
    g_spiBus = spi_bus_initialize(kSdSpiHost, &bus, SDSPI_DEFAULT_DMA) == ESP_OK;
    return g_spiBus;
}

void stopBus() {
    if (g_spiBus) spi_bus_free(kSdSpiHost);
    g_spiBus = false;
}

// Initialize the card a failed SD-bus mount left, for fatVerdict; false when it does not answer.
bool initCard(sdmmc_host_t& host, const SdPinConfig& pins, sdmmc_card_t& card) {
    const sdmmc_slot_config_t slotCfg = slotConfig(pins);
    return host.init() == ESP_OK && sdmmc_host_init_slot(host.slot, &slotCfg) == ESP_OK && sdmmc_card_init(&host, &card) == ESP_OK;
}
}  // namespace

// Release what a mount took, the chip's LDO, the power switch and the SPI bus, on every exit that ends unmounted.
static void powerDown() {
#if SOC_SDMMC_IO_POWER_EXTERNAL
    if (g_pwrCtrl) { sd_pwr_ctrl_del_on_chip_ldo(g_pwrCtrl); g_pwrCtrl = nullptr; }
#endif
    if (g_pwrGpio >= 0) gpio_set_level(static_cast<gpio_num_t>(g_pwrGpio), 1);
    stopBus();
}

// FatFs's own verdict on a card whose mount failed with ESP_FAIL, which IDF returns for an unreadable card and for one without a filesystem alike.
static FRESULT fatVerdict(sdmmc_host_t host, const SdPinConfig& pins) {
    FRESULT res = FR_NOT_READY;
    FATFS* fs = new (std::nothrow) FATFS;
    if (!fs) return res;
    sdmmc_card_t card = {};
    BYTE pdrv = FF_DRV_NOT_USED;
    if (initCard(host, pins, card) && ff_diskio_get_drive(&pdrv) == ESP_OK && pdrv != FF_DRV_NOT_USED) {
        ff_diskio_register_sdmmc(pdrv, &card);
        const char drv[3] = {static_cast<char>('0' + pdrv), ':', 0};
        res = f_mount(fs, drv, 1);
        f_mount(nullptr, drv, 0);
        ff_diskio_unregister(pdrv);
    }
    if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) host.deinit_p(host.slot);
    else host.deinit();
    delete fs;
    return res;
}

static void clearStaging();

// `format` writes a FAT filesystem when the card has none, which only the user's format asks for.
static bool mountCard(const SdPinConfig& pins, bool format) {
    if (g_mounted) return true;
    g_lastPins = pins;
    if (!usablePins(pins)) { g_mountError = SdMountError::Pins; return false; }
    powerSlot(pins.pwr);

    esp_vfs_fat_sdmmc_mount_config_t mountCfg = {};
    mountCfg.format_if_mount_failed = format;   // true only on the format task's mount: a blank or foreign card stays untouched otherwise
    mountCfg.max_files = 5;
    mountCfg.allocation_unit_size = 32 * 1024;   // the SD Association's FAT32 cluster for 1 to 32 GB cards

    sdmmc_host_t host = hostFor(pins);
    if (!startBus(pins)) { powerDown(); g_mountError = SdMountError::Pins; return false; }

#if SOC_SDMMC_IO_POWER_EXTERNAL
    // This chip powers its SDMMC IO lines from an on-chip LDO the driver turns on itself, a chip fact rather than board wiring.
    sd_pwr_ctrl_ldo_config_t ldoCfg = {};
    ldoCfg.ldo_chan_id = 4;   // VO4: the P4 TRM's own reference design wires this LDO channel to VDD_IO_5, the SD card's switchable supply.
    const esp_err_t ldoErr = sd_pwr_ctrl_new_on_chip_ldo(&ldoCfg, &g_pwrCtrl);
    if (ldoErr != ESP_OK) {
        ESP_LOGE(SD_TAG, "SD LDO power control failed: %s", esp_err_to_name(ldoErr));
        powerDown();
        g_mountError = SdMountError::NoCard;   // no reason names a chip supply that would not start, and without it no card can answer
        return false;
    }
    host.pwr_ctrl_handle = g_pwrCtrl;
#endif

    esp_err_t err;
    if (pins.bus == SdBus::Spi) {
        const sdspi_device_config_t dev = spiDevice(pins);
        err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &dev, &mountCfg, &g_card);
    } else {
        const sdmmc_slot_config_t slotCfg = slotConfig(pins);
        err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slotCfg, &mountCfg, &g_card);
    }
    if (err != ESP_OK) {
        ESP_LOGE(SD_TAG, "SD mount failed: %s", esp_err_to_name(err));
        // ESP_FAIL is a card that answers; it is formattable on exactly the FatFs results IDF's own formatting mount formats on, every other error being the card not answering.
        if (err != ESP_FAIL) g_mountError = SdMountError::NoCard;
        else {
            // SPI reads as no filesystem: a second SPI init after IDF's failed-mount cleanup asserts in the SPI bus lock.
            const FRESULT res = pins.bus == SdBus::Spi ? FR_NO_FILESYSTEM : fatVerdict(host, pins);
            g_mountError = res == FR_NO_FILESYSTEM || res == FR_INT_ERR ? SdMountError::NoFilesystem : SdMountError::Unreadable;
        }
        powerDown();
        g_card = nullptr;
        return false;
    }

    // DMA-capable, so the probe sector is read straight into it on every chip.
    g_scratch = static_cast<SdScratch*>(heap_caps_calloc(1, sizeof(SdScratch), MALLOC_CAP_DMA));
    if (!g_scratch) {
        ESP_LOGE(SD_TAG, "SD scratch allocation failed");
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, g_card);
        powerDown();
        g_card = nullptr;
        g_mountError = SdMountError::NoCard;
        return false;
    }
    g_mounted = true;
    g_mountError = SdMountError::None;
    clearStaging();
    ESP_LOGI(SD_TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return true;
}

bool sdMount(const SdPinConfig& pins) {
    if (g_formatState == SdFormatState::Running) return false;
    return mountCard(pins, false);
}

SdMountError sdMountError() { return g_mountError; }

// Unmount and power the slot down, for a caller that has already ruled out an open stream.
static void unmountCard() {
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, g_card);
    powerDown();
    heap_caps_free(g_scratch);
    g_scratch = nullptr;
    g_card = nullptr;
    g_mounted = false;
}

bool sdUnmount() {
    if (g_formatState == SdFormatState::Running) return false;
    if (!g_mounted) return true;
    if (g_streamsOpen > 0) return false;   // a transfer holds a FIL into the context this would free
    unmountCard();
    return true;
}

// The format itself, on its own task: @xref{a-format-runs-on-its-own-task}.
static void formatTask(void*) {
    bool ok = false;
    // A card the last mount found without a filesystem is formatted by a mount that may format, the one path that writes a new FAT.
    if (g_formatUnmounted) ok = mountCard(g_lastPins, true);
    else if (esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, g_card) == ESP_OK) ok = g_mounted = true;
    else unmountCard();
    g_formatState = ok ? SdFormatState::Succeeded : SdFormatState::Failed;
    vTaskDelete(nullptr);
}

bool sdFormat() {
    if (sdCardBusy()) return false;
    g_formatUnmounted = !g_mounted && g_mountError == SdMountError::NoFilesystem;
    if (!g_formatUnmounted && (!g_mounted || !g_card)) return false;
    g_mounted = false;   // every file call refuses until the task publishes its result
    g_formatState = SdFormatState::Running;
    // One above the render loop, the textbook favoring of an I/O-bound task: it takes the CPU only between transfers it waits on.
    if (xTaskCreate(&formatTask, "sdFormat", 4096, nullptr, 2, nullptr) == pdPASS) return true;
    g_mounted = !g_formatUnmounted;
    g_formatState = SdFormatState::Failed;
    return false;
}

SdFormatState sdFormatState() { return g_formatState; }

uint64_t sdCardUsed() {
    if (!g_mounted) return 0;
    FATFS* fs = nullptr;
    DWORD freeClusters = 0;
    // Fast with a valid FSINFO sector, a full FAT scan otherwise; both callers run on the render thread, a deliberate tradeoff.
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
    if (f_open(&g_scratch->fil, full, FA_READ) != FR_OK) return -1;
    UINT br = 0;
    const FRESULT r = f_read(&g_scratch->fil, buf, static_cast<UINT>(maxLen - 1), &br);
    f_close(&g_scratch->fil);
    if (r != FR_OK) return -1;
    buf[br] = 0;
    return static_cast<int>(br);
}

int64_t sdCardSize(const char* apiPath) {
    if (!g_mounted) return -1;
    char full[128];
    if (!sdTranslate(apiPath, full, sizeof(full))) return -1;
    FILINFO fno;
    if (f_stat(full, &fno) != FR_OK || (fno.fattrib & AM_DIR)) return -1;
    return static_cast<int64_t>(fno.fsize);
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

// The staging folder every write lands in first: @xref{a-write-lands-in-a-staging-folder-first}.
static constexpr const char* SD_STAGING = "0:/.tmp";

// A staging name unique among the writes in flight; false on truncation.
static bool stagingPath(char* out, size_t outLen) {
    static uint32_t seq = 0;
    f_mkdir(SD_STAGING);   // FR_EXIST once it is there
    const int n = std::snprintf(out, outLen, "%s/w%lu.tmp", SD_STAGING, static_cast<unsigned long>(seq++));
    return n > 0 && static_cast<size_t>(n) < outLen;
}

// Delete what writes cut short by a failed card left behind.
static void clearStaging() {
    FF_DIR dir;
    if (f_opendir(&dir, SD_STAGING) != FR_OK) return;
    FILINFO info;
    char path[40];
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
        const int n = std::snprintf(path, sizeof(path), "%s/%s", SD_STAGING, info.fname);
        if (n > 0 && static_cast<size_t>(n) < sizeof(path)) f_unlink(path);
    }
    f_closedir(&dir);
}

// FAT has no atomic replace and f_rename refuses an existing destination, so the old file steps aside into staging and returns if the new one cannot take its name.
static bool sdCommitTmp(const char* tmp, const char* full) {
    char old[32];
    const bool movedOld = stagingPath(old, sizeof(old)) && f_rename(full, old) == FR_OK;
    if (f_rename(tmp, full) == FR_OK) {
        if (movedOld) f_unlink(old);
        return true;
    }
    if (movedOld) f_rename(old, full);
    f_unlink(tmp);
    return false;
}

bool sdCardWriteAtomic(const char* apiPath, const char* data, size_t len) {
    if (!g_mounted) return false;
    if (len > 0 && !data) return false;
    sdMakeParent(apiPath);
    char full[128];
    char tmp[32];
    if (!sdTranslate(apiPath, full, sizeof(full)) || !stagingPath(tmp, sizeof(tmp))) return false;

    if (f_open(&g_scratch->fil, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT bw = 0;
    const FRESULT wr = f_write(&g_scratch->fil, data, static_cast<UINT>(len), &bw);
    const FRESULT closed = f_close(&g_scratch->fil);   // the final flush: a file that did not close whole never replaces the old one
    if (wr != FR_OK || bw != len || closed != FR_OK) { f_unlink(tmp); return false; }
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

// Carries both paths beside the FIL, so close() can rename or discard without re-deriving them.
struct SdWriteHandle {
    FIL f;
    char full[128] = {};
    char tmp[32] = {};
};

bool sdCardWriteStreamOpen(void*& handle, const char* apiPath) {
    if (!g_mounted) return false;
    sdMakeParent(apiPath);
    auto* h = new SdWriteHandle();
    if (!sdTranslate(apiPath, h->full, sizeof(h->full))) { delete h; return false; }
    if (!stagingPath(h->tmp, sizeof(h->tmp))) { delete h; return false; }
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
    const bool closed = f_close(&h->f) == FR_OK;   // the final flush, as in sdCardWriteAtomic
    const bool ok = commit && closed ? sdCommitTmp(h->tmp, h->full) : !commit;
    if (!commit || !closed) f_unlink(h->tmp);
    delete h;
    --g_streamsOpen;
    return ok;
}

#else  // !MM_HAS_SDMMC: every symbol below is inert so a board with no SDMMC host still links.

bool sdCardMounted() { return false; }
bool sdCardPresent() { return false; }
bool sdMount(const SdPinConfig&) { return false; }
SdMountError sdMountError() { return SdMountError::NoCard; }
bool sdCardBusy() { return false; }
bool sdCardBootsByDefault() { return false; }
bool sdUnmount() { return true; }
bool sdFormat() { return false; }
SdFormatState sdFormatState() { return SdFormatState::Idle; }
uint64_t sdCardUsed() { return 0; }
uint64_t sdCardTotal() { return 0; }
bool sdCardMkdir(const char*) { return false; }
bool sdCardExists(const char*) { return false; }
bool sdCardRemove(const char*) { return false; }
int  sdCardRead(const char*, char*, size_t) { return -1; }
int64_t sdCardSize(const char*) { return -1; }
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
