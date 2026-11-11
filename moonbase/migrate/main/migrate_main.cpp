// The migration image: installed through the old firmware's update page (WLED's, Tasmota's or ESPHome's), it moves the board to MoonLight's layout and restarts into MoonBase.
//
// It first checks that it can finish, and otherwise boots the old firmware again with nothing written.
// Then it runs once, in this order, each step safe to repeat, so a power cut starts it over:
// 1. Out of the way: running from the slot MoonBase goes to, it copies itself to the other slot and restarts there.
// 2. The bootloader, MoonLight's for this board's flash among those it carries, as a cable install writes it (QIO where the variant runs it, the flash size in its header), read back. When it names more flash than the old one did, one restart lets it, so every later write reaches the whole chip.
// 3. The networks: every WiFi name and password WLED saved, and its Ethernet board, or the network a Tasmota, an ESPHome or, failing those, ESP-IDF itself saved, read while the old firmware's filesystem and NVS still exist, so the one the device is on is among them.
// 4. MoonBase, written into the factory slot, or into the second app slot of a table without one, and read back.
// 5. The networks again, as a NetworkModule.json in the new filesystem, so MoonBase comes up on the cable or joins the first network that answers. It overwrites part of WLED's filesystem, so a run after a cut finds no network there and leaves this file as it is.
// 6. The table beside that bootloader, written at 0x8000 and read back. A power cut during this write or the bootloader's is the one that needs a cable.
// 7. The choice of app, cleared so the bootloader starts the factory app, or set to the second app slot: MoonBase either way, which installs MoonLight into the first.

#include "esp_flash.h"
#include "nvs.h"         // a Tasmota's settings live in NVS
#include "nvs_flash.h"
#include "esp_image_format.h"   // esp_image_get_metadata: the running image's own length
#include "esp_rom_crc.h"
#include "esp_flash_encrypt.h"
#include "esp_flash_partitions.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_secure_boot.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lfs.h"
#include "soc/soc_caps.h"
#if SOC_SPI_MEM_SUPPORT_FLASH_OPI_MODE
#include "esp_efuse.h"
#include "esp_efuse_table.h"   // ESP_EFUSE_FLASH_TYPE: the flash type the chip's eFuse names
#elif CONFIG_IDF_TARGET_ESP32S3
#error "the S3's octal-flash capability is missing: the octal check would pass a quad bootloader onto octal flash"
#endif

#include "core/util/WledNetworks.h"   // WLED's saved networks, read as a desktop test reads them

#include <cstdint>
#include <cstdio>
#include <cstring>

extern const uint8_t moonbase_start[] asm("_binary_MoonLight_moonbase_bin_start");
extern const uint8_t moonbase_end[] asm("_binary_MoonLight_moonbase_bin_end");
extern const uint8_t tables_start[] asm("_binary_partition_tables_bin_start");   // one table per flash size, in ESP_PARTITION_TABLE_MAX_LEN slots
extern const uint8_t tables_end[] asm("_binary_partition_tables_bin_end");
extern const uint8_t bootloaders_start[] asm("_binary_bootloaders_bin_start");   // the bootloader beside each table, in slots from the bootloader's offset up to the table

namespace {

using mm::wlednet::Network;
using mm::wlednet::Ethernet;
constexpr int kMaxNetworks = 4;   // WLED 16 saves three
constexpr const char* kTag = "migrate";
constexpr uint32_t kSector = 4096;
constexpr uint32_t kBootSlot = ESP_PARTITION_TABLE_OFFSET - CONFIG_BOOTLOADER_OFFSET_IN_FLASH;

struct Region { uint32_t offset = 0; uint32_t size = 0; };

// The table this board takes and the bootloader beside it, chosen once at start among the embedded ones (chooseTable).
const uint8_t* table_ = nullptr;
const uint8_t* bootloader_ = nullptr;

// One entry of the embedded table by type and subtype, which is where everything this image writes lives.
Region entry(uint8_t type, uint8_t subtype) {
    for (const uint8_t* p = table_; p + sizeof(esp_partition_info_t) <= table_ + ESP_PARTITION_TABLE_MAX_LEN; p += sizeof(esp_partition_info_t)) {
        esp_partition_info_t e;
        std::memcpy(&e, p, sizeof(e));
        if (e.magic != ESP_PARTITION_MAGIC) break;
        if (e.type == type && e.subtype == subtype) return {e.pos.offset, e.pos.size};
    }
    return {};
}

void pause(const char* done) {
    ESP_LOGI(kTag, "%s", done);
#if CONFIG_MM_MIGRATE_PAUSE_S > 0
    ESP_LOGI(kTag, "pausing %d s: cut the power now to test this point", CONFIG_MM_MIGRATE_PAUSE_S);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_MM_MIGRATE_PAUSE_S * 1000));
#endif
}

uint32_t roundUp(uint32_t n) { return (n + kSector - 1) / kSector * kSector; }

// Whether the flash at `offset` already holds `data`, read in pieces.
bool holds(uint32_t offset, const uint8_t* data, uint32_t len) {
    static uint8_t back[1024];
    for (uint32_t at = 0; at < len; at += sizeof(back)) {
        const uint32_t n = len - at < sizeof(back) ? len - at : sizeof(back);
        if (esp_flash_read(nullptr, back, offset + at, n) != ESP_OK || std::memcmp(back, data + at, n) != 0) return false;
    }
    return true;
}

// Erase, write, then read back and compare; nothing erased when the bytes are there already, so a run after a cut adds no window of its own.
bool writeVerified(uint32_t offset, const uint8_t* data, uint32_t len) {
    if (holds(offset, data, len)) return true;
    if (esp_flash_erase_region(nullptr, offset, roundUp(len)) != ESP_OK) return false;
    if (esp_flash_write(nullptr, data, offset, len) != ESP_OK) return false;
    return holds(offset, data, len);
}

bool overlaps(const Region& a, const Region& b) { return a.offset < b.offset + b.size && a.offset + a.size > b.offset; }

// The bytes an app image fills in `p`: its own length from its header, not its slot's, since a slot can overlap what this image writes while the image inside it does not (Tasmota's single app slot).
Region imageIn(const esp_partition_t* p) {
    const esp_partition_pos_t pos = {p->address, p->size};
    esp_image_metadata_t meta = {};
    return {p->address, esp_image_get_metadata(&pos, &meta) == ESP_OK ? meta.image_len : p->size};
}

bool overlapsRunning(const Region& r) { return overlaps(imageIn(esp_ota_get_running_partition()), r); }

// Where MoonBase goes: the factory slot, or the second app slot of a table without one, which MoonLight's first network update takes back.
Region moonbaseTarget() {
    const Region factory = entry(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY);
    return factory.size ? factory : entry(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1);
}

// The app's filesystem, labeled `littlefs` or, on the two-slot tables, `spiffs`.
Region filesystem() {
    const Region fs = entry(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS);
    return fs.size ? fs : entry(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS);
}

// Point the bootloader at the second app slot: one selection entry with sequence 2, the second of two slots, its CRC over the sequence alone as the bootloader checks it.
bool selectSecondSlot(const Region& otadata) {
    esp_ota_select_entry_t e;
    std::memset(&e, 0xFF, sizeof(e));
    e.ota_seq = 2;
    e.ota_state = ESP_OTA_IMG_UNDEFINED;   // boots with or without rollback, which MoonBase confirms anyway
    e.crc = esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&e.ota_seq), sizeof(e.ota_seq));
    return esp_flash_erase_region(nullptr, otadata.offset, otadata.size) == ESP_OK
        && writeVerified(otadata.offset, reinterpret_cast<const uint8_t*>(&e), sizeof(e));
}

// The furthest any partition of the chosen table reaches, which the flash must hold.
uint32_t tableEnd() {
    uint32_t end = 0;
    for (const uint8_t* p = table_; p + sizeof(esp_partition_info_t) <= table_ + ESP_PARTITION_TABLE_MAX_LEN; p += sizeof(esp_partition_info_t)) {
        esp_partition_info_t e;
        std::memcpy(&e, p, sizeof(e));
        if (e.magic != ESP_PARTITION_MAGIC) break;
        if (e.pos.offset + e.pos.size > end) end = e.pos.offset + e.pos.size;
    }
    return end;
}

// Whether the chip's flash is octal, as its eFuse says; only an S3 has one.
bool flashIsOctal() {
#if SOC_SPI_MEM_SUPPORT_FLASH_OPI_MODE
    return esp_efuse_read_field_bit(ESP_EFUSE_FLASH_TYPE);
#else
    return false;
#endif
}

// Whether a bootloader fits this chip: built for its flash type (ESP-IDF writes DOUT into the header of a build for octal flash, DIO into a quad one's), and naming no more flash than it has, which the app's start refuses.
bool fits(const uint8_t* boot, uint32_t flash) {
    esp_image_header_t h;
    std::memcpy(&h, boot, sizeof(h));
    return (h.spi_mode == ESP_IMAGE_SPI_MODE_DOUT) == flashIsOctal()
        && static_cast<uint32_t>(esp_image_get_flash_size(static_cast<esp_image_flash_size_t>(h.spi_size))) <= flash;
}

// Take the largest embedded table the chip's flash holds, among those whose bootloader fits it, so one image serves each board of the family; false when none fits.
// The size is the chip's own, which the old bootloader's header can understate.
bool chooseTable() {
    uint32_t flash = 0, best = 0;
    if (esp_flash_get_physical_size(esp_flash_default_chip, &flash) != ESP_OK) return false;
    const uint8_t* chosen = nullptr;
    const uint8_t* boot = bootloaders_start;
    for (const uint8_t* t = tables_start; t + ESP_PARTITION_TABLE_MAX_LEN <= tables_end; t += ESP_PARTITION_TABLE_MAX_LEN, boot += kBootSlot) {
        table_ = t;
        const uint32_t end = tableEnd();
        if (fits(boot, flash) && end <= flash && end > best) { best = end; chosen = t; bootloader_ = boot; }
    }
    table_ = chosen;
    return chosen != nullptr;
}

// Why this board cannot finish, before anything is written, or null when it can.
const char* refusal(const Region& target, const Region& otadata, const Region& fs, uint32_t moonbaseLen) {
    if (!target.size || !otadata.size || !fs.size || moonbaseLen > target.size) return "the embedded table or MoonBase does not fit";
    // Encrypted or signed flash takes no raw writes, which would land scrambled.
    if (esp_flash_encryption_enabled()) return "flash encryption is on";
    if (esp_secure_boot_enabled()) return "secure boot is on";
    // Where the image's bytes lie while it writes: here, or in the other slot once moved aside, clear of every byte it writes.
    const esp_partition_t* running = esp_ota_get_running_partition();
    const Region image = imageIn(running);
    const Region write = {target.offset, moonbaseLen};
    Region home = image;
    if (overlaps(image, write)) {
        const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
        if (!other || other == running || other->size < image.size) return "no other slot to move to";
        home = {other->address, image.size};
    }
    if (overlaps(home, write) || overlaps(home, otadata) || overlaps(home, fs)) return "no slot clear of what MoonLight writes";
    return nullptr;
}

// Boot what ran before this image, the other slot, so a board that cannot move keeps working as it was; on a table of one app slot, Tasmota's, its safeboot in the factory slot.
void backToWhatRan(const char* why) {
    ESP_LOGE(kTag, "cannot move this board: %s; restarting what ran before", why);
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    if (!other || other == running) other = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    if (other && other != running) esp_ota_set_boot_partition(other);
    esp_restart();
}

// Step 1: copy this image into the other slot and boot it, when this one is where MoonBase goes.
// A failed copy never falls through to the writes below: it restarts, into what ran before when that is still whole, else into this image again.
void moveAside(const Region& write) {
    if (!overlapsRunning(write)) return;
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    ESP_LOGI(kTag, "running where MoonBase goes: moving to %s", other->label);
    esp_ota_handle_t h = 0;
    if (esp_ota_begin(other, OTA_SIZE_UNKNOWN, &h) != ESP_OK) backToWhatRan("the other slot did not open");
    static uint8_t buf[4096];
    const uint32_t len = imageIn(running).size;   // the image, not its slot, which may be larger than the other one
    for (uint32_t at = 0; at < len; at += sizeof(buf)) {
        if (esp_partition_read(running, at, buf, sizeof(buf)) != ESP_OK || esp_ota_write(h, buf, sizeof(buf)) != ESP_OK) {
            esp_ota_abort(h);
            backToWhatRan("the copy did not write");
        }
    }
    if (esp_ota_end(h) != ESP_OK) backToWhatRan("the copy did not verify");
    esp_ota_set_boot_partition(other);   // refused, this image runs again and the move starts over
    esp_restart();
}

// Read a whole small file into `buf`; false when it does not open.
bool readFile(const char* path, char* buf, size_t len) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    const size_t n = std::fread(buf, 1, len - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return true;
}

// Step 2: every network WLED saved, the names in cfg.json and the passwords in wsec.json, its Ethernet board into `eth`, `hasEth` saying whether it maps, and its TX power limit; how many networks have a name.
int readWledNetworks(Network* nets, int max, Ethernet& eth, bool& hasEth, int& txPower) {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/wled";
    conf.partition_label = "spiffs";
    conf.read_only = true;
    if (const esp_err_t err = esp_vfs_littlefs_register(&conf); err != ESP_OK) {
        ESP_LOGW(kTag, "WLED's filesystem did not mount: %s", esp_err_to_name(err));
        return 0;
    }
    static char cfg[16384], wsec[2048];   // WLED's cfg.json runs to a few KB, its network block near the top
    const bool haveCfg = readFile("/wled/cfg.json", cfg, sizeof(cfg));
    const bool haveSec = readFile("/wled/wsec.json", wsec, sizeof(wsec));
    esp_vfs_littlefs_unregister("spiffs");
    if (!haveCfg) ESP_LOGW(kTag, "WLED's filesystem holds no cfg.json");
    hasEth = haveCfg && mm::wlednet::readEthernet(cfg, eth);
    txPower = haveCfg ? mm::wlednet::readTxPower(cfg) : 0;
    return mm::wlednet::readNetworks(haveCfg ? cfg : nullptr, haveSec ? wsec : nullptr, nets, max);
}

// One blob out of the old firmware's NVS, the partition labeled `nvs` in its table, into `buf`; its length, or 0 when absent.
size_t readNvsBlob(const char* space, const char* key, uint8_t* buf, size_t cap) {
    if (nvs_flash_init_partition("nvs") != ESP_OK) return 0;
    size_t len = 0;
    nvs_handle_t h;
    if (nvs_open_from_partition("nvs", space, NVS_READONLY, &h) == ESP_OK) {
        size_t n = cap;
        if (nvs_get_blob(h, key, buf, &n) == ESP_OK) len = n;
        nvs_close(h);
    }
    nvs_flash_deinit_partition("nvs");
    return len;
}

// Step 3 without WLED's files: the network a Tasmota (its settings, `main`/`Settings`), an ESPHome (its WiFi preference) or, failing those, ESP-IDF itself (any other firmware) saved in NVS; 1 when found, else 0.
int readNvsNetwork(Network* nets) {
    static uint8_t blob[4096];   // a Tasmota's whole settings blob, TSettings being 4096 bytes; ESPHome's WiFi is 98
    if (size_t n = readNvsBlob("main", "Settings", blob, sizeof(blob)); n && mm::wlednet::readTasmotaNetwork(blob, n, nets[0])) return 1;
    if (size_t n = readNvsBlob(mm::wlednet::kEsphomeNamespace, mm::wlednet::kEsphomeWifiKey, blob, sizeof(blob)); n && mm::wlednet::readEsphomeNetwork(blob, n, nets[0])) return 1;
    static uint8_t psk[80];
    const size_t ssidLen = readNvsBlob(mm::wlednet::kIdfWifiNamespace, mm::wlednet::kIdfSsidKey, blob, sizeof(blob));
    const size_t pskLen = readNvsBlob(mm::wlednet::kIdfWifiNamespace, mm::wlednet::kIdfPasswordKey, psk, sizeof(psk));
    return mm::wlednet::readIdfNetwork(blob, ssidLen, pskLen ? psk : nullptr, pskLen, nets[0]) ? 1 : 0;
}

// The new filesystem as a block device on the main flash: no partition entry exists for it while the old table is loaded.
uint32_t g_fsOffset = 0;
int bdRead(const lfs_config* c, lfs_block_t b, lfs_off_t off, void* buf, lfs_size_t n) {
    return esp_flash_read(nullptr, buf, g_fsOffset + b * c->block_size + off, n) == ESP_OK ? 0 : LFS_ERR_IO;
}
int bdProg(const lfs_config* c, lfs_block_t b, lfs_off_t off, const void* buf, lfs_size_t n) {
    return esp_flash_write(nullptr, buf, g_fsOffset + b * c->block_size + off, n) == ESP_OK ? 0 : LFS_ERR_IO;
}
int bdErase(const lfs_config* c, lfs_block_t b) {
    return esp_flash_erase_region(nullptr, g_fsOffset + b * c->block_size, c->block_size) == ESP_OK ? 0 : LFS_ERR_IO;
}
int bdSync(const lfs_config*) { return 0; }

// Step 4: a fresh filesystem holding the networks.
bool writeNetworks(const Region& fs, const Network* nets, int count, const Ethernet* eth, int txPower) {
    g_fsOffset = fs.offset;
    // The geometry esp_littlefs mounts with, from the same component and settings, so MoonBase and MoonLight read this as their own.
    lfs_config cfg = {};
    cfg.read = bdRead;
    cfg.prog = bdProg;
    cfg.erase = bdErase;
    cfg.sync = bdSync;
    cfg.read_size = CONFIG_LITTLEFS_READ_SIZE;
    cfg.prog_size = CONFIG_LITTLEFS_WRITE_SIZE;
    cfg.block_size = kSector;
    cfg.block_count = fs.size / kSector;
    cfg.cache_size = CONFIG_LITTLEFS_CACHE_SIZE;
    cfg.lookahead_size = CONFIG_LITTLEFS_LOOKAHEAD_SIZE;
    cfg.block_cycles = CONFIG_LITTLEFS_BLOCK_CYCLES;
    lfs_t lfs;
    if (lfs_format(&lfs, &cfg) != 0 || lfs_mount(&lfs, &cfg) != 0) return false;
    static char doc[400 + kMaxNetworks * 240];   // the Ethernet card, and each entry's name and password escaped at most twice their length
    const int n = mm::wlednet::networkDocument(doc, sizeof(doc), nets, count, eth, txPower);
    bool ok = n > 0 && lfs_mkdir(&lfs, "/.config") == 0;
    lfs_file_t f;
    if (ok && lfs_file_open(&lfs, &f, "/.config/NetworkModule.json", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) == 0) {
        ok = lfs_file_write(&lfs, &f, doc, static_cast<lfs_size_t>(n)) == n;
        ok = lfs_file_close(&lfs, &f) == 0 && ok;
    } else {
        ok = false;
    }
    lfs_unmount(&lfs);
    return ok;
}

}  // namespace

extern "C" void app_main() {
    if (!chooseTable()) backToWhatRan("no table this image carries fits the flash");
    const bool hasFactory = entry(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY).size != 0;
    const Region target = moonbaseTarget();
    const Region otadata = entry(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA);
    const Region fs = filesystem();
    const uint32_t moonbaseLen = static_cast<uint32_t>(moonbase_end - moonbase_start);
    ESP_LOGI(kTag, "MoonLight's table for %u KB of flash", static_cast<unsigned>(tableEnd() / 1024));
    if (const char* why = refusal(target, otadata, fs, moonbaseLen)) backToWhatRan(why);

    // Confirmed at once: WLED's bootloader rolls an unconfirmed app back at the next reset, which after step 3 would start MoonBase under the old table, where a cut should start this image over.
    esp_ota_mark_app_valid_cancel_rollback();
    moveAside({target.offset, moonbaseLen});

    // Step 2. A cut from here until the table is written leaves the new bootloader on the old table, which starts this image again.
    // The whole slot, its erased padding included, so no byte of the old bootloader stays behind the new one.
    if (!writeVerified(CONFIG_BOOTLOADER_OFFSET_IN_FLASH, bootloader_, kBootSlot)) { ESP_LOGE(kTag, "the bootloader did not write"); return; }
    pause("bootloader written");
    if (uint32_t seen = 0; esp_flash_get_size(nullptr, &seen) == ESP_OK && seen < tableEnd()) {
        ESP_LOGI(kTag, "the old bootloader named %u KB of the chip's flash: restarting under the new one", static_cast<unsigned>(seen / 1024));
        esp_restart();
    }

    static Network nets[kMaxNetworks];
    Ethernet eth = {};
    bool hasEth = false;
    int txPower = 0;
    int count = readWledNetworks(nets, kMaxNetworks, eth, hasEth, txPower);
    if (!count) count = readNvsNetwork(nets);   // no WLED files: Tasmota, ESPHome and ESP-IDF itself keep a network in NVS
    if (txPower) ESP_LOGI(kTag, "WLED's TX power limit: %d dBm", txPower);
    for (int i = 0; i < count; i++) ESP_LOGI(kTag, "network %d: %s", i + 1, nets[i].ssid);
    if (hasEth) ESP_LOGI(kTag, "WLED's Ethernet board: PHY %d, MDC %d, MDIO %d, clock %d", eth.type, eth.mdcGpio, eth.mdioGpio, eth.clockGpio);
    if (!count && !hasEth) ESP_LOGW(kTag, "no WLED network found; the new filesystem keeps what an earlier run wrote");
    pause("network read");

    if (!writeVerified(target.offset, moonbase_start, moonbaseLen)) { ESP_LOGE(kTag, "MoonBase did not write; the old table still starts this image"); return; }
    pause("MoonBase written");

    if ((count || hasEth) && !writeNetworks(fs, nets, count, hasEth ? &eth : nullptr, txPower)) ESP_LOGW(kTag, "the networks did not write; MoonBase opens its access point");
    pause("filesystem written");

    if (!writeVerified(ESP_PARTITION_TABLE_OFFSET, table_, ESP_PARTITION_TABLE_MAX_LEN)) { ESP_LOGE(kTag, "the table did not write"); return; }
    pause("table written");

    if (hasFactory ? esp_flash_erase_region(nullptr, otadata.offset, otadata.size) != ESP_OK : !selectSecondSlot(otadata)) {
        ESP_LOGE(kTag, "the app choice did not write");
        return;
    }
    pause(hasFactory ? "app choice cleared" : "app choice set to the second slot");
    ESP_LOGI(kTag, "restarting into MoonBase");
    esp_restart();
}
