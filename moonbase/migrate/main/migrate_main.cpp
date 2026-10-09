// The migration image: installed through WLED's update page, it moves the board to MoonLight's layout and restarts into MoonBase.
//
// It first checks that it can finish, and otherwise boots WLED again with nothing written.
// Then it runs once, in this order, each step safe to repeat, so a power cut starts it over:
// 1. Out of the way: running from the slot MoonBase goes to, it copies itself to the other slot and restarts there.
// 2. The network: WLED's WiFi name and password, read from its filesystem while that still exists.
// 3. MoonBase, written into the factory slot and read back.
// 4. The network again, as a NetworkModule.json in the new filesystem, so MoonBase joins it. It overwrites part of WLED's filesystem, so a run after a cut finds no network there and leaves this file as it is.
// 5. The table, MoonLight's, written at 0x8000 and read back. A power cut during this write is the one that needs a cable.
// 6. The choice of app, cleared, so the bootloader starts the factory app: MoonBase.

#include "esp_flash.h"
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

#include <cstdint>
#include <cstdio>
#include <cstring>

extern const uint8_t moonbase_start[] asm("_binary_MoonLight_moonbase_bin_start");
extern const uint8_t moonbase_end[] asm("_binary_MoonLight_moonbase_bin_end");
extern const uint8_t table_start[] asm("_binary_partition_table_bin_start");
extern const uint8_t table_end[] asm("_binary_partition_table_bin_end");

namespace {

constexpr const char* kTag = "migrate";
constexpr uint32_t kSector = 4096;

struct Region { uint32_t offset = 0; uint32_t size = 0; };

// One entry of the embedded table by type and subtype, which is where everything this image writes lives.
Region entry(uint8_t type, uint8_t subtype) {
    for (const uint8_t* p = table_start; p + sizeof(esp_partition_info_t) <= table_end; p += sizeof(esp_partition_info_t)) {
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

// Erase, write, then read back in pieces and compare; false at the first difference.
bool writeVerified(uint32_t offset, const uint8_t* data, uint32_t len) {
    if (esp_flash_erase_region(nullptr, offset, roundUp(len)) != ESP_OK) return false;
    if (esp_flash_write(nullptr, data, offset, len) != ESP_OK) return false;
    static uint8_t back[1024];
    for (uint32_t at = 0; at < len; at += sizeof(back)) {
        const uint32_t n = len - at < sizeof(back) ? len - at : sizeof(back);
        if (esp_flash_read(nullptr, back, offset + at, n) != ESP_OK || std::memcmp(back, data + at, n) != 0) return false;
    }
    return true;
}

bool overlaps(const esp_partition_t* p, const Region& r) {
    return p->address < r.offset + r.size && p->address + p->size > r.offset;
}

bool overlapsRunning(const Region& r) { return overlaps(esp_ota_get_running_partition(), r); }

// The furthest any partition of the embedded table reaches, which the flash must hold.
uint32_t tableEnd() {
    uint32_t end = 0;
    for (const uint8_t* p = table_start; p + sizeof(esp_partition_info_t) <= table_end; p += sizeof(esp_partition_info_t)) {
        esp_partition_info_t e;
        std::memcpy(&e, p, sizeof(e));
        if (e.magic != ESP_PARTITION_MAGIC) break;
        if (e.pos.offset + e.pos.size > end) end = e.pos.offset + e.pos.size;
    }
    return end;
}

// Why this board cannot finish, before anything is written, or null when it can.
const char* refusal(const Region& factory, const Region& otadata, const Region& fs, uint32_t moonbaseLen) {
    if (!factory.size || !otadata.size || !fs.size || moonbaseLen > factory.size) return "the embedded table or MoonBase does not fit";
    uint32_t flash = 0;
    if (esp_flash_get_size(nullptr, &flash) != ESP_OK || flash < tableEnd()) return "the flash is smaller than MoonLight's table";
    // Encrypted or signed flash takes no raw writes, which would land scrambled.
    if (esp_flash_encryption_enabled()) return "flash encryption is on";
    if (esp_secure_boot_enabled()) return "secure boot is on";
    // Where the image runs while it writes: here, or the other slot once moved aside, and clear of everything it writes.
    const esp_partition_t* home = overlapsRunning(factory) ? esp_ota_get_next_update_partition(nullptr) : esp_ota_get_running_partition();
    if (!home) return "no other slot to move to";
    if (overlaps(home, factory) || overlaps(home, otadata) || overlaps(home, fs)) return "no slot clear of what MoonLight writes";
    return nullptr;
}

// Boot what ran before this image, the other slot, so a board that cannot move keeps working as it was.
void backToWhatRan(const char* why) {
    ESP_LOGE(kTag, "cannot move this board: %s; restarting what ran before", why);
    if (const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr)) esp_ota_set_boot_partition(other);
    esp_restart();
}

// Step 1: copy this image into the other slot and boot it, when this one is where MoonBase goes.
// A failed copy never falls through to the writes below: it restarts, into what ran before when that is still whole, else into this image again.
void moveAside(const Region& factory) {
    if (!overlapsRunning(factory)) return;
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    ESP_LOGI(kTag, "running where MoonBase goes: moving to %s", other->label);
    esp_ota_handle_t h = 0;
    if (esp_ota_begin(other, OTA_SIZE_UNKNOWN, &h) != ESP_OK) backToWhatRan("the other slot did not open");
    static uint8_t buf[4096];
    for (uint32_t at = 0; at < running->size; at += sizeof(buf)) {
        if (esp_partition_read(running, at, buf, sizeof(buf)) != ESP_OK || esp_ota_write(h, buf, sizeof(buf)) != ESP_OK) {
            esp_ota_abort(h);
            backToWhatRan("the copy did not write");
        }
    }
    if (esp_ota_end(h) != ESP_OK) backToWhatRan("the copy did not verify");
    esp_ota_set_boot_partition(other);   // refused, this image runs again and the move starts over
    esp_restart();
}

// The string value after the first `"key":"` in `text`, into `out`; false when absent.
bool jsonString(const char* text, const char* key, char* out, size_t outLen) {
    char pattern[24];
    std::snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = std::strstr(text, pattern);
    if (!p) return false;
    p += std::strlen(pattern);
    size_t i = 0;
    for (; *p && *p != '"' && i + 1 < outLen; p++) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p;
    }
    out[i] = 0;
    return i > 0;
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

// Step 2: WLED's first network, its name in cfg.json and its password in wsec.json, both under nw.ins[0].
bool readWledNetwork(char* ssid, size_t ssidLen, char* psk, size_t pskLen) {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/wled";
    conf.partition_label = "spiffs";
    conf.read_only = true;
    if (esp_vfs_littlefs_register(&conf) != ESP_OK) return false;
    static char buf[16384];   // WLED's cfg.json runs to a few KB, its network block near the top
    const bool ok = readFile("/wled/cfg.json", buf, sizeof(buf)) && jsonString(buf, "ssid", ssid, ssidLen);
    psk[0] = 0;
    if (ok && readFile("/wled/wsec.json", buf, sizeof(buf))) jsonString(buf, "psk", psk, pskLen);
    esp_vfs_littlefs_unregister("spiffs");
    return ok;
}

// `s` as a JSON string literal into `out`.
void quoted(const char* s, char* out, size_t len) {
    size_t i = 0;
    if (i + 1 < len) out[i++] = '"';
    for (; *s && i + 3 < len; s++) {
        if (*s == '"' || *s == '\\') out[i++] = '\\';
        out[i++] = *s;
    }
    out[i++] = '"';
    out[i] = 0;
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

// Step 5: a fresh filesystem holding the network as the state document MoonLight saves it in, which MoonBase also reads.
bool writeNetwork(const Region& fs, const char* ssid, const char* psk) {
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
    char s[72], p[136], doc[320];
    quoted(ssid, s, sizeof(s));
    quoted(psk, p, sizeof(p));
    const int n = std::snprintf(doc, sizeof(doc),
                                "{\"Network\":{\"WiFi\":{\"type\":\"WiFiModule\",\"known\":[{\"id\":1,\"ssid\":%s,\"password\":%s,\"ipSettings\":0}]}}}", s, p);
    bool ok = n > 0 && static_cast<size_t>(n) < sizeof(doc) && lfs_mkdir(&lfs, "/.config") == 0;
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
    const Region factory = entry(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY);
    const Region otadata = entry(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA);
    const Region fs = entry(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS);
    const uint32_t moonbaseLen = static_cast<uint32_t>(moonbase_end - moonbase_start);
    const uint32_t tableLen = static_cast<uint32_t>(table_end - table_start);
    if (const char* why = refusal(factory, otadata, fs, moonbaseLen)) backToWhatRan(why);

    // Confirmed at once: WLED's bootloader rolls an unconfirmed app back at the next reset, which after step 3 would start MoonBase under the old table, where a cut should start this image over.
    esp_ota_mark_app_valid_cancel_rollback();
    moveAside(factory);

    static char ssid[33], psk[65];
    const bool network = readWledNetwork(ssid, sizeof(ssid), psk, sizeof(psk));
    if (network) ESP_LOGI(kTag, "WLED's network: %s", ssid);
    else ESP_LOGW(kTag, "no WLED network found; the new filesystem keeps what an earlier run wrote");
    pause("network read");

    if (!writeVerified(factory.offset, moonbase_start, moonbaseLen)) { ESP_LOGE(kTag, "MoonBase did not write; the old table still starts this image"); return; }
    pause("MoonBase written");

    if (network && !writeNetwork(fs, ssid, psk)) ESP_LOGW(kTag, "the network did not write; MoonBase opens its access point");
    pause("filesystem written");

    if (!writeVerified(ESP_PARTITION_TABLE_OFFSET, table_start, tableLen)) { ESP_LOGE(kTag, "the table did not write"); return; }
    pause("table written");

    if (esp_flash_erase_region(nullptr, otadata.offset, otadata.size) != ESP_OK) { ESP_LOGE(kTag, "the app choice did not clear"); return; }
    pause("app choice cleared");
    ESP_LOGI(kTag, "restarting into MoonBase");
    esp_restart();
}
