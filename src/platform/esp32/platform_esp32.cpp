/// @defgroup platform_esp32 The ESP32 platform layer core
/// The system primitives, the network, and the sockets.
///
/// Time, allocation, restart and chip information; ethernet, the two wireless modes and discovery; the three socket kinds.
///
/// @moreinfo
///
/// ## What lives in sibling files, and what does not
///
/// Three subsystems are self-contained, each owning its private state and reaching this file only through declared symbols: the filesystem, updates and serial provisioning.
/// The network stayed because its parts share file-scope state, the event handler and the interface pointers and the init flags among them.
/// Splitting it would take either an internal header of external declarations or a singleton, which is a separate change for when it earns its keep.
///
/// ## Making written code visible to instruction fetch
///
/// The bytes go in through data-bus stores, so on a cache-backed region they may still sit in the data cache, and two steps are needed in order.
/// First write the data cache back so memory holds the code, then invalidate the instruction cache for the range so the core refetches it.
/// A single instruction-type sync does only the second: on the P4 that refetches stale memory, the bytes never having left the data cache, and the core decodes garbage.
/// On the other chip the region is directly executable and this is belt and braces, but it is correct on both.
///
/// ## The coprocessor version query is asked twice, then never again
///
/// The P4's radio runs on a companion chip, and the query asks what firmware version it reported over the link.
/// It is a blocking call over that link, and the module asks from the one-second tick, which runs inline on the render thread.
///
/// Measured on a board with the radio live: the call times out after about a second, every second, forever.
/// The module showed over a million microseconds per tick at zero frames, and every request queued a second or more behind the render loop.
/// The link works while this particular call does not answer, so retrying buys nothing and costs a second of every tick.
/// Two attempts rather than five, since each unanswered one is a second of stutter, and one retry still catches a companion that was mid-handshake.
/// After that the display keeps what it learned, the version being unable to change while the host runs.
///
/// ## The vendor PHY needs two steps the generic driver cannot do
///
/// No dedicated driver exists for this part, so both go through the standard register interface, mirroring the vendor's own example for this board.
/// Without the first the link never negotiates, which is why the activity indicator stays dark: the part disables auto-negotiation on reset, undocumented behavior the generic reset leaves alone.
/// The second configures the interface's internal clock delays through the extended-register window, at the values that example uses; a board whose trace lengths need a different skew tunes them there.
///
/// ## Why the hostname is applied at link-up
///
/// The default wired interface starts its address client from its own connected handler, so a name set earlier is clobbered when that client restarts nameless and the lease lands blank.
/// The name only takes on a stopped client, so the sequence is stop, set, start, and the fresh request then carries it.
/// The wireless side needs none of this, its client starting on association, well after the name is set.
///
/// ## The reconnect is ours to make, and unbounded
///
/// Without an explicit call a dropped association is permanent: the device keeps rendering but is unreachable until it is power-cycled.
/// Which for a controller in a ceiling is a real failure.
/// The vendor's own example has the same call in the same place.
/// The retry is unbounded by design, since the recoverable causes outlast any retry count and self-healing is the entire point.
///
/// It reconnects immediately and does not sleep to pace itself, running on the event task that also carries the wired and address events, where blocking would stall the whole stack.
/// The pacing is free: a failing association takes a second or two to time out before the next event arrives, so even a wrong credential retries at a sane rate.
///
/// The reason code is logged because without it the line says only that it disconnected, which sends a user hunting coverage for a cause the radio already named.
/// A board on an incompatible band reports no access point found on every attempt, and that read identically to a weak-signal drop.
///
/// ## The companion chip initializes itself
///
/// On the chip with no native radio, the calls are forwarded to a companion that self-initializes at boot through a constructor, setting up its transport and channels.
/// Do not call the init or connect entry points here: init is already done, and connect is really a transport reconfigure that resets the companion and re-initializes its bus.
/// On a live link that fails and tears down the working boot-time connection, proven on the bench.
///
/// ## Advertising needs the interface registered by hand
///
/// The component claims to run by default on preconfigured interfaces, but on one chip that does not catch the wired one, so the record ships with no address.
/// The device then advertises a service a home automation system can see but not resolve, leaving a blank address in its browser and no discovery.
/// Registering the interface by pointer and enabling it forces the probe and announce onto the real one, and is harmless where the default already covers it.
///
/// A re-advertise removes the service record and adds it back rather than renaming it.
/// Since renaming does not reliably re-announce on the current interface while a remove and add drives it back through the state machine.

#include "platform/platform.h"

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"   // esp_ptr_external_ram — the ptrIsPsram residency probe
#include "esp_cache.h"        // esp_cache_msync — I-cache sync after writing MoonLive code to IRAM
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_cpu.h"       // esp_cpu_get_cycle_count — the cycleCount() seam
#include "esp_mac.h"
#include "esp_idf_version.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"     // for esp_ota_get_running_partition (sysInfo)
#include "esp_image_format.h"
#include "esp_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#ifdef CONFIG_IDF_TARGET_ESP32P4
#include "esp_eth_phy_ip101.h"   // P4-NANO PHY — managed component espressif/ip101
#endif
// The external controller over the serial bus, on a chip with no internal controller and the driver enabled.
// The vendor moved these out of the core into managed components, so the headers come from one gated to that chip.
// Not under emulation, which turns the internal one off for want of emulated silicon and would otherwise pull in a component that variant does not carry.
// The marker below keeps the rest of the file from repeating this compound condition.
#if defined(CONFIG_ETH_USE_SPI_ETHERNET) && !defined(CONFIG_ETH_USE_ESP32_EMAC) && \
    !defined(CONFIG_ETH_USE_OPENETH)
#define MM_ETH_W5500 1
#include "driver/spi_master.h"   // W5500 SPI Ethernet (S3 boards) — bus + device config
#include "esp_eth_mac_w5500.h"   // espressif/eth_w5500 managed component
#include "esp_eth_phy_w5500.h"
#endif
#ifndef MM_NO_WIFI
#include "esp_wifi.h"
#if defined(CONFIG_IDF_TARGET_ESP32P4)
// On the P4 the radio calls are forwarded to the companion chip, which self-initializes at boot, so no bring-up call is needed. This header is only for the read-only version query, and matches the guard on that function.
#include "esp_hosted.h"
#endif
#endif
#include "esp_log.h"
#include "esp_rom_sys.h"     // esp_rom_delay_us (delayUs)
#include "mdns.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"   // getaddrinfo — hostname resolution for TcpConnection::connect

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>     // hostname store: writer (config apply) and reader (link-up events) race
#include <unistd.h>

namespace mm::platform {

// Test-only override for millis(); 0 means "use the real clock". Honoured on ESP32 too so a hardware scenario run can freeze time the same way unit tests do (no separate desktop-vs-ESP32 mocking surface).
static std::atomic<uint32_t> testNowMs{0};

void setTestNowMs(uint32_t ms) { testNowMs.store(ms, std::memory_order_relaxed); }

// Host-test hook (see platform.h); no ESP32 test drives a bind failure, so it is inert here.
void setTestBindFails(bool) {}

uint32_t millis() MM_NONBLOCKING {
    uint32_t override_ = testNowMs.load(std::memory_order_relaxed);
    if (override_) return override_;
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// The task handle IS the identity, and reading it costs one load, no TLS, so it works on a task however it was created. That matters: THREADPTR is 0 on a task without TLS set up, which made C++ thread_local fault at 0xfffffff0 here.
uintptr_t currentThreadId() MM_NONBLOCKING {
    return reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
}

uint32_t micros() MM_NONBLOCKING {
    return static_cast<uint32_t>(esp_timer_get_time());
}

void* alloc(size_t bytes) {
#ifdef CONFIG_SPIRAM
    // Try PSRAM first, fall back to internal RAM
    void* ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) return ptr;
#endif
    return heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

void* allocInternal(size_t bytes) {
    // Internal only, no PSRAM fallback here, the caller chose this seam because PSRAM latency breaks it (an ISR-read buffer); a silent PSRAM grant would hand back the exact problem. Caller falls back.
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

bool ptrIsPsram(const void* p) {
    return p != nullptr && esp_ptr_external_ram(p);
}

void free(void* ptr) {
    heap_caps_free(ptr);
}

// Executable memory for emitted code, from instruction-fetchable memory.
// Null when that is exhausted, which a busy device really does hit, so the caller degrades rather than asserting.
// The request rounds up to a word, since both the final store and the cache sync round up and the block must hold that whole word.
void* allocExec(size_t bytes) {
    if (bytes == 0) return nullptr;
    size_t padded = (bytes + 3) & ~size_t(3);
    return heap_caps_malloc(padded, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
}

void freeExec(void* ptr, size_t /*bytes*/) {
    heap_caps_free(ptr);   // size is the desktop munmap's; IRAM free needs only the ptr
}

void writeExec(void* dst, const void* src, size_t len) {
    if (!dst || !src || !len) return;
    // IRAM is writable only by 32-bit-aligned WORD stores (a byte/halfword store to IRAM faults), so copy word-by-word, padding the final partial word with the bytes already there, never a sub-word store. allocExec returns 4-byte-aligned IRAM, so dst is aligned; src may not be, so read it bytewise into the word.
    auto* d = static_cast<volatile uint32_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    size_t words = len / 4;
    for (size_t i = 0; i < words; i++) {
        uint32_t w = static_cast<uint32_t>(s[i*4]) | (static_cast<uint32_t>(s[i*4+1]) << 8) |
                     (static_cast<uint32_t>(s[i*4+2]) << 16) | (static_cast<uint32_t>(s[i*4+3]) << 24);
        d[i] = w;
    }
    size_t rem = len % 4;
    if (rem) {
        uint32_t w = d[words];                       // preserve the untouched high bytes
        for (size_t b = 0; b < rem; b++) {
            w &= ~(0xFFu << (b*8));
            w |= static_cast<uint32_t>(s[words*4 + b]) << (b*8);
        }
        d[words] = w;
    }
    // Make the written code visible to instruction fetch, in two steps: @xref{making-written-code-visible-to-instruction-fetch|why one is not enough}. Unaligned, because the code block is not cache-line sized.
    const size_t paddedLen = (len + 3) & ~size_t(3);
    esp_cache_msync(dst, paddedLen,
                    ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(dst, paddedLen,
                    ESP_CACHE_MSYNC_FLAG_TYPE_INST | ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void yield() {
    vTaskDelay(pdMS_TO_TICKS(1));
}

void delayMs(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void pauseLoop() {
    // Nothing: yield() here is vTaskDelay(1), which already yields to the idle task for a tick. A further sleep would come straight out of the render budget.
}

void delayUs(uint32_t us) {
    // Busy-wait, fine for the few-hundred-µs protocol gaps this exists for (e.g. the WS2812 inter-frame latch), off any latency-critical context.
    esp_rom_delay_us(us);
}

void reboot() {
    esp_restart();
}

// The same three numbers the desktop counts by hand, from the allocator that already tracks them. A scenario reads one metric on both platforms: how much the system has taken, its high-water mark, and how many blocks are live. USED rather than free, so the figure means the same thing on a board with 320 KB and a laptop with gigabytes.
size_t allocatedBytes() {
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    return info.total_allocated_bytes;
}
size_t allocatedPeak() {
    // The minimum-ever free, expressed as a peak used: IDF tracks the low-water mark of free heap, which is the same fact from the other side.
    const size_t total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    const size_t minFree = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    return total > minFree ? total - minFree : 0;
}
uint32_t allocatedCount() {
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    return static_cast<uint32_t>(info.allocated_blocks);
}

size_t freeHeap() {
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

size_t freeInternalHeap() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Test-only cap on the reported largest-free block; 0 = no cap. atomic to match the desktop seam's cross-thread contract. It only ever LOWERS the reported value (min with the real block), a cap can't claim more contiguous heap than the device actually has, so a forced-paging test stays honest.
static std::atomic<size_t> testMaxBlock{0};
void setTestMaxAllocBlock(size_t bytes) { testMaxBlock.store(bytes, std::memory_order_relaxed); }

size_t maxAllocBlock() {
    size_t real = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t cap = testMaxBlock.load(std::memory_order_relaxed);
    return (cap != 0 && cap < real) ? cap : real;
}

size_t maxInternalAllocBlock() {
    // MALLOC_CAP_INTERNAL excludes PSRAM. The internal heap is the scarce resource (WiFi, TCP/IP, FreeRTOS stacks all draw from it); PSRAM is huge by construction so its largest-free-block tells you nothing about memory pressure.
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t maxExecAllocBlock() {
    // The pool allocExec draws from. On a classic ESP32 that is IRAM, tens of KB rather than the hundreds the data heap has, and it is not visible in freeInternalHeap: a script whose compiled code does not fit reported "codegen failed" with 80 KB of DRAM free, which is what made this worth reporting rather than inferring.
    return heap_caps_get_largest_free_block(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
}

size_t totalHeap() {
    return heap_caps_get_total_size(MALLOC_CAP_8BIT);
}

size_t totalInternalHeap() {
    return heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void getMacAddress(uint8_t mac[6]) {
    esp_efuse_mac_get_default(mac);
}

const char* macString() {
    // The base MAC is fixed for the chip's life, so format it once into a static buffer the caller can point at (no per-module copy). Not called before the first use, single-threaded init.
    static char buf[18] = {};
    if (buf[0] == 0) {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return buf;
}

const char* hostPlatform() {
    // Empty on a device: a board cannot self-identify, so `deviceModel` is injected by tooling from the catalog (MoonDeck, or the web installer over serial). Answering something here would overwrite a real board name with a guess.
    return "";
}

const char* chipModel() {
    esp_chip_info_t info;
    esp_chip_info(&info);
    switch (info.model) {
        case CHIP_ESP32:   return "ESP32";
        case CHIP_ESP32S2: return "ESP32-S2";
        case CHIP_ESP32S3: return "ESP32-S3";
        case CHIP_ESP32S31: return "ESP32-S31";
        case CHIP_ESP32C3: return "ESP32-C3";
        case CHIP_ESP32P4: return "ESP32-P4";
        default:           return "ESP32-?";
    }
}

uint32_t IRAM_ATTR cycleCount() { return esp_cpu_get_cycle_count(); }

uint8_t currentCore() { return static_cast<uint8_t>(xPortGetCoreID()); }

const char* cpuInfo() {
    // Frequency from the running clock (esp_rom_get_cpu_ticks_per_us == MHz), not the sdkconfig macro, so a config/hardware mismatch shows up. Cores from esp_chip_info, same source chipModel uses.
    static char buf[24] = {};
    if (!buf[0]) {
        esp_chip_info_t info;
        esp_chip_info(&info);
        std::snprintf(buf, sizeof(buf), "%u MHz, %u cores",
                      static_cast<unsigned>(esp_rom_get_cpu_ticks_per_us()),
                      static_cast<unsigned>(info.cores));
    }
    return buf;
}

const char* hostIp() {
    // The device IP belongs to NetworkModule (WiFi/Ethernet), not the platform layer, it isn't known until an interface comes up. Empty here.
    return "";
}

// Read a netif's current IPv4 as raw octets (out[0..3]); all-zero on no IP / null netif. esp_ip4_addr_t.addr is little-endian-packed (octet i = byte i), matching IP2STR's `(addr >> (8*i)) & 0xff`, so this is the byte-form of the same value the old IPSTR getters printed. Shared by ethGetIPv4/wifiStaGetIPv4.
static void netifIPv4(esp_netif_t* netif, uint8_t out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!netif) return;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return;
    const uint32_t a = info.ip.addr;
    out[0] = static_cast<uint8_t>(a & 0xff);
    out[1] = static_cast<uint8_t>((a >> 8) & 0xff);
    out[2] = static_cast<uint8_t>((a >> 16) & 0xff);
    out[3] = static_cast<uint8_t>((a >> 24) & 0xff);
}

const char* sdkVersion() {
    return esp_get_idf_version();
}

const char* psramType() {
    // The PSRAM interface mode is a compile-time choice (IDF has no runtime getter). CONFIG_SPIRAM_MODE_OCT is set only for octal parts (S3/S2 -R8); classic-ESP32 quad PSRAM (WROVER) leaves it unset. Report "" when PSRAM isn't compiled in at all, so a non-PSRAM board naturally shows nothing.
#if !defined(CONFIG_SPIRAM)
    return "";
#elif defined(CONFIG_SPIRAM_MODE_OCT)
    return "octal";
#else
    return "quad";
#endif
}

const char* coprocessorWifi() {
#if defined(CONFIG_IDF_TARGET_ESP32P4) && !defined(MM_NO_WIFI)
    // What firmware version the companion chip reported over the link; an empty answer means it never completed its handshake, which is the case worth surfacing rather than inferring. Asked a bounded number of times and then never again: @xref{the-coprocessor-version-query-is-asked-twice-then-never-again|the measurement behind that}.
    static char buf[24] = "querying…";
    static uint8_t attemptsLeft = 2;
    if (attemptsLeft == 0) return buf;
    esp_hosted_coprocessor_fwver_t ver = {};
    if (esp_hosted_get_coprocessor_fwversion(&ver) == ESP_OK
        && (ver.major1 || ver.minor1 || ver.patch1)) {
        attemptsLeft = 0;                       // answered: never ask again
        std::snprintf(buf, sizeof(buf), "C6 fw %u.%u.%u",
                      static_cast<unsigned>(ver.major1),
                      static_cast<unsigned>(ver.minor1),
                      static_cast<unsigned>(ver.patch1));
    } else if (--attemptsLeft == 0) {
        // Out of attempts. Say WHY the field is empty rather than asserting the C6 is absent: the query is what failed, and on this bench WiFi runs fine while it does.
        std::snprintf(buf, sizeof(buf), "no version reply");
    }
    return buf;
#else
    return "";   // native-radio targets have no WiFi co-processor
#endif
}

const char* resetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_EXT:        return "EXT";
        case ESP_RST_SW:         return "SW";
        case ESP_RST_PANIC:      return "PANIC";
        case ESP_RST_INT_WDT:    return "INT_WDT";
        case ESP_RST_TASK_WDT:   return "TASK_WDT";
        case ESP_RST_WDT:        return "WDT";
        case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_SDIO:       return "SDIO";
        default:                 return "UNKNOWN";
    }
}

void setLogLevel(LogLevel level) {
    // LogLevel's values are chosen to equal esp_log_level_t (None=0 … Verbose=5), so the mapping is a plain cast, the "*" tag sets the level for every component at once.
    esp_log_level_set("*", static_cast<esp_log_level_t>(level));
}

size_t firmwareSize() {
    // Get actual running image size from the image header
    const esp_partition_t* part = esp_ota_get_running_partition();
    if (!part) return 0;
    esp_partition_pos_t partPos = { .offset = part->address, .size = part->size };
    esp_image_metadata_t metadata;
    if (esp_image_get_metadata(&partPos, &metadata) == ESP_OK) {
        return metadata.image_len;
    }
    return 0;
}

size_t firmwarePartition() {
    const esp_partition_t* part = esp_ota_get_running_partition();
    if (part) return part->size;
    return 0;
}

size_t flashChipSize() {
    uint32_t chipSize = 0;
    esp_flash_get_size(nullptr, &chipSize);
    return chipSize;
}


// -----------------------------------------------------------------------
// Network
// -----------------------------------------------------------------------

static const char* NET_TAG = "mm_net";

// Connection state tracked by the event handlers, atomic because they cross threads: the event loop writes and the render task reads every tick. Relaxed ordering is enough, each flag being an independent signal with nothing else published through it.
#ifndef MM_NO_ETH
static std::atomic<bool> ethLinkUp_{false};
static std::atomic<bool> ethConnected_{false};
// Fixed-addressing state, so the connected handler restores the address on a re-plug rather than letting the client pull a lease.
// Written on the render task and read on the event task; the octets are published before the flag's release store.
// So a handler that sees it set reads a complete config.
static std::atomic<bool> ethStatic_{false};
static uint8_t ethStaticIp_[4]   = {};
static uint8_t ethStaticGw_[4]   = {};
static uint8_t ethStaticMask_[4] = {};
static uint8_t ethStaticDns_[4]  = {};
static esp_netif_t* ethNetif_ = nullptr;
// Retained so a live reconfigure can tear the driver down cleanly: the running driver, and whether the serial bus was initialized so the stop frees it. The second exists only where that bus does, keeping the other builds free of an unused-variable warning.
static esp_eth_handle_t ethHandle_ = nullptr;
#ifdef MM_ETH_W5500
static bool ethSpiActive_ = false;
#endif
#endif
static bool netifInitDone_ = false;

// The hostname pushed before bring-up and applied to each interface before its address client starts; empty leaves the default.
// Writer and reader run on different tasks once live, so both sides copy under a lock.
// And the reader takes a snapshot first because stack calls inside it would nest into the event loop.
static char hostname_[32] = {};
static std::mutex hostnameMutex_;

void setHostname(const char* name) {
    std::lock_guard<std::mutex> lock(hostnameMutex_);
    if (!name) { hostname_[0] = 0; return; }
    std::strncpy(hostname_, name, sizeof(hostname_) - 1);
    hostname_[sizeof(hostname_) - 1] = 0;
}

// Apply the stored hostname to an interface so it rides the address request; call after the interface is started.
// The order is the crux: @xref{why-the-hostname-is-applied-at-link-up|stop, set, start}.
// Stopping an already-stopped client is benign and ignored, and this is a no-op when no name is set.
static void applyHostname(esp_netif_t* netif) {
    char name[sizeof(hostname_)];
    {
        std::lock_guard<std::mutex> lock(hostnameMutex_);
        std::memcpy(name, hostname_, sizeof(name));
    }
    if (!netif || !name[0]) return;
    esp_netif_dhcpc_stop(netif);    // must be stopped for set_hostname to take; ignore ALREADY_STOPPED
    esp_err_t e = esp_netif_set_hostname(netif, name);
    if (e != ESP_OK) ESP_LOGW(NET_TAG, "set_hostname('%s') failed: %s", name, esp_err_to_name(e));
    else ESP_LOGI(NET_TAG, "DHCP hostname: %s", name);
    // Restart the DHCP client and check the result, if it fails, the interface has no DHCP client and will never acquire an IP, so surface it rather than silently leaving the device offline. (Don't return on stop/set failure above: we still must restart the client we stopped.)
    esp_err_t se = esp_netif_dhcpc_start(netif);
    if (se != ESP_OK)
        ESP_LOGW(NET_TAG, "dhcpc_start after set_hostname failed: %s", esp_err_to_name(se));
}

#ifndef MM_NO_WIFI
// WiFi-only state, absent in the Ethernet-only build. Atomic for the same reason as the eth pair: written by the IDF event loop, read from the render task.
static std::atomic<bool> wifiStaConnected_{false};
static bool wifiApActive_ = false;
// Association state, distinct from having an address: a fixed-address station is reachable once associated, so this is the signal the apply keys off. Atomic for the same reason as the flag above, being written by the event handler and read on the caller's thread.
static std::atomic<bool> wifiStaAssociated_{false};
// Static-addressing state for WiFi STA, mirroring the eth pair. `wifiStaConnected_` normally means "has a DHCP IP" (set on GOT_IP), which never fires on a DHCP-less network, so for a static STA the address is pinned at L2 association (WIFI_EVENT_STA_CONNECTED) and connected is marked there. std::atomic with the same cross-task publish contract as ethStatic_ (octets before flag).
static std::atomic<bool> staStatic_{false};
static uint8_t staStaticIp_[4]   = {};
static uint8_t staStaticGw_[4]   = {};
static uint8_t staStaticMask_[4] = {};
static uint8_t staStaticDns_[4]  = {};
static esp_netif_t* staNetif_ = nullptr;
static esp_netif_t* apNetif_ = nullptr;
static std::atomic<bool> wifiInitDone_{false};   // atomic: the radio poller reads it from its own task

// Radio telemetry, read on the render tick and refreshed off it. With a co-processor radio every esp_wifi query is a blocking RPC, 55-90 ms measured, and the state push made one a second. A task polls instead.
static std::atomic<int8_t> radioRssi_{0};
static std::atomic<int8_t> radioTxPowerQ_{0};  // quarter dBm, the unit the stack uses
static std::atomic<uint64_t> radioAp_{0};      // BSSID << 8 | channel, one word so a reader never sees half a connect
constexpr uint32_t kRadioPollMs = 2000;        // a signal-strength readout, not an instrument

static void radioPollTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kRadioPollMs));
        if (!wifiInitDone_.load(std::memory_order_relaxed)) continue;
        if (wifiStaConnected_.load(std::memory_order_relaxed)) {
            int rssi = 0;
            if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) radioRssi_.store(static_cast<int8_t>(rssi), std::memory_order_relaxed);
        }
        int8_t power = 0;
        if (esp_wifi_get_max_tx_power(&power) == ESP_OK) radioTxPowerQ_.store(power, std::memory_order_relaxed);
    }
}

// Spawned once, on the first WiFi init, and kept across a deinit: wifiInitDone_ gates the asking.
static void ensureRadioPoller() {
    static bool started = false;
    if (started) return;
    // 4 KB: the hosted RPC path serializes through protobuf below this call.
    started = xTaskCreate(&radioPollTask, "mmradio", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr) == pdPASS;
    if (!started) ESP_LOGW(NET_TAG, "no radio poller task: RSSI and TX power will read 0");
}
#endif

static void ensureNetifInit() {
    if (!netifInitDone_) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        netifInitDone_ = true;
    }
}

#ifndef MM_NO_ETH

uint16_t ethLinkSpeedMbps() MM_NONBLOCKING;   // defined below; the link-up log reports it

static void ethEventHandler(void* /*arg*/, esp_event_base_t base,
                            int32_t id, void* data) {
    if (base == ETH_EVENT) {
        if (id == ETHERNET_EVENT_CONNECTED) {
            ethLinkUp_.store(true, std::memory_order_relaxed);
            // The NEGOTIATED speed rather than "up". A gigabit PHY that fell back to 100M behaves differently enough to matter (the S31's RGMII Tx-clock skew is speed-dependent), and "link up" alone sent one debug session hunting DHCP when the question was the speed.
            ESP_LOGI(NET_TAG, "Ethernet link up (%u Mbps)", ethLinkSpeedMbps());
            if (ethStatic_.load(std::memory_order_acquire)) {
                // Static mode: do NOT let the DHCP client restart on this link-up (applyHostname would), that is what made a re-plugged cable grab a DHCP lease instead of the configured static IP. Re-pin the stored static config directly so the interface returns to its static address immediately (netSetStaticIPv4 stops dhcpc + sets it).
                netSetStaticIPv4(NetIface::Eth, ethStaticIp_, ethStaticGw_, ethStaticMask_, ethStaticDns_);
            } else {
                // The name is set here rather than at init: @xref{why-the-hostname-is-applied-at-link-up|why the earlier one is clobbered}.
                applyHostname(ethNetif_);
            }
        } else if (id == ETHERNET_EVENT_DISCONNECTED) {
            ethLinkUp_.store(false, std::memory_order_relaxed);
            ESP_LOGI(NET_TAG, "Ethernet link down");
            ethConnected_.store(false, std::memory_order_relaxed);
        } else if (id == ETHERNET_EVENT_START) {
            ESP_LOGI(NET_TAG, "Ethernet started");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(NET_TAG, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ethConnected_.store(true, std::memory_order_relaxed);
    }
}

// The runtime pin and PHY config, seeded with the per-chip default so an unprovisioned board still comes up on its historical pins. The module overrides it with the board's own values before init; which driver exists is a per-chip build choice, and this only selects pins and which to use.
static EthPinConfig ethConfig_ = ethConfigDefault;

void setEthConfig(const EthPinConfig& cfg) { ethConfig_ = cfg; }

// The internal-controller path, only on chips that have one, so the guard keeps this function out of the build where the external one is used instead.
// The two interface kinds share the same constructor and the driver and event tail, only their clock and data-pin block differing.
// So they live in one function branched at compile time rather than two near-identical copies.
#ifdef CONFIG_ETH_USE_ESP32_EMAC

#ifdef CONFIG_IDF_TARGET_ESP32S31
// The vendor PHY's board init, two steps through the standard register interface: @xref{the-vendor-phy-needs-two-steps-the-generic-driver-cannot-do|what each one does}. A remaining clock fix for one link rate is backlogged; this init is what brings the link up.
static esp_err_t ethYt8531BoardInit(esp_eth_handle_t eth_handle) {
    bool autoNegoEn = true;
    esp_err_t err = esp_eth_ioctl(eth_handle, ETH_CMD_S_AUTONEGO, &autoNegoEn);
    if (err != ESP_OK) return err;

    uint32_t regVal = 0;
    esp_eth_phy_reg_rw_data_t phyReg = {};
    phyReg.reg_value_p = &regVal;

    // RX ~2 ns coarse delay: EXT_CHIP_CONFIG (0xA001) bit 8 (rxc_dly_en).
    regVal = 0xA001; phyReg.reg_addr = 0x1E;
    if ((err = esp_eth_ioctl(eth_handle, ETH_CMD_WRITE_PHY_REG, &phyReg)) != ESP_OK) return err;
    phyReg.reg_addr = 0x1F;
    if ((err = esp_eth_ioctl(eth_handle, ETH_CMD_READ_PHY_REG,  &phyReg)) != ESP_OK) return err;
    regVal |= (1U << 8);
    if ((err = esp_eth_ioctl(eth_handle, ETH_CMD_WRITE_PHY_REG, &phyReg)) != ESP_OK) return err;

    // TX + RX delays: EXT_RGMII_CONFIG1 (0xA003). Bits [3:0] ge_tx_delay, [7:4] fe_tx_delay, [13:10] rx_delay, each 0..15 = 0.000..2.250 ns in 0.150 ns steps (Motorcomm YT8521/YT8531 map). TX = 13 (1.95 ns). RX data delay [13:10] is set here (the 0xA001 bit-8 above is only the coarse RXC enable). MM_YT8531_{RX,TX}_DELAY are per-board tuning knobs; the defaults match IDF's example.
#ifndef MM_YT8531_RX_DELAY
#define MM_YT8531_RX_DELAY 0
#endif
#ifndef MM_YT8531_TX_DELAY
#define MM_YT8531_TX_DELAY 13
#endif
    regVal = 0xA003; phyReg.reg_addr = 0x1E;
    if ((err = esp_eth_ioctl(eth_handle, ETH_CMD_WRITE_PHY_REG, &phyReg)) != ESP_OK) return err;
    phyReg.reg_addr = 0x1F;
    if ((err = esp_eth_ioctl(eth_handle, ETH_CMD_READ_PHY_REG,  &phyReg)) != ESP_OK) return err;
    regVal = (regVal & ~0x3CFFU)                     // clear rx_delay [13:10] + tx [7:0]
           | ((uint32_t)(MM_YT8531_RX_DELAY & 0xF) << 10)  // rx_delay
           | ((uint32_t)(MM_YT8531_TX_DELAY & 0xF) << 4)   // fe_tx
           | ((uint32_t)(MM_YT8531_TX_DELAY & 0xF) << 0);  // ge_tx
    if ((err = esp_eth_ioctl(eth_handle, ETH_CMD_WRITE_PHY_REG, &phyReg)) != ESP_OK) return err;

    ESP_LOGI(NET_TAG, "YT8531 RGMII init: auto-nego re-enabled, Rx+Tx delays set");
    return ESP_OK;
}
#endif  // CONFIG_IDF_TARGET_ESP32S31

static bool ethInitEmac() {
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    ethNetif_ = esp_netif_new(&netif_cfg);

    // Pins from the runtime config: a per-board default map, or an override pushed from the device model. The default config macro is chip-fixed, so the interface-specific block below branches at compile time and the union member that exists is the one the macro set.
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    // A preprocessor branch rather than a compile-time one, because those union members exist only in the header on chips that have the wider interface. A constant-condition branch would still fail to compile where they are absent.
#ifdef CONFIG_IDF_TARGET_ESP32S31
    // The gigabit path's fixed pads, the only pins the controller accepts for each signal.
    // Validated against the IO_MUX table in the vendor's own esp32s31/emac_periph.c, and matching the board schematic.
    // Listed explicitly for clarity though the defaults would pick the same.
    // A name not in the list returns the not-found sentinel, which the driver rejects at init rather than silently wiring the first pad.
    constexpr auto rgmiiPad = [](const char* want) -> int {
        for (uint8_t i = 0; i < ethFixedPadCount; i++) {
            const char* n = ethFixedPads[i].name;
            const char* w = want;
            while (*n && *n == *w) { ++n; ++w; }
            if (*n == 0 && *w == 0) return ethFixedPads[i].gpio;
        }
        return -1;   // not found: IDF rejects it loudly at eth init
    };
    // Looked up BY NAME out of platform::ethFixedPads, the ONE list of these pads: NetworkModule reports the same entries through fixedPins() so the pin map can show what the MAC holds. By name rather than by index so reordering that list cannot silently rewire the MAC, and a typo is a refused init rather than a scrambled bus.
    emac_config.clock_config.rgmii.clock_tx_gpio = rgmiiPad("ethTxClk");
    emac_config.clock_config.rgmii.clock_rx_gpio = rgmiiPad("ethRxClk");
    emac_config.emac_dataif_gpio.rgmii = eth_mac_rgmii_gpio_config_t{
        /*tx_ctl*/ rgmiiPad("ethTxCtl"),
        /*txd0*/   rgmiiPad("ethTxd0"), /*txd1*/ rgmiiPad("ethTxd1"),
        /*txd2*/   rgmiiPad("ethTxd2"), /*txd3*/ rgmiiPad("ethTxd3"),
        /*rx_ctl*/ rgmiiPad("ethRxCtl"),
        /*rxd0*/   rgmiiPad("ethRxd0"), /*rxd1*/ rgmiiPad("ethRxd1"),
        /*rxd2*/   rgmiiPad("ethRxd2"), /*rxd3*/ rgmiiPad("ethRxd3"),
    };
#else
    emac_config.clock_config.rmii.clock_mode =
        ethConfig_.rmiiClockExtIn ? EMAC_CLK_EXT_IN : EMAC_CLK_OUT;
    emac_config.clock_config.rmii.clock_gpio =
        static_cast<gpio_num_t>(ethConfig_.rmiiClockGpio);
    // The data pins are left at the macro's defaults: fixed in silicon on one chip, and already the board wiring on the other, which the proven build relied on.
#endif
    if (ethConfig_.mdcGpio >= 0)  emac_config.smi_gpio.mdc_num  = ethConfig_.mdcGpio;
    if (ethConfig_.mdioGpio >= 0) emac_config.smi_gpio.mdio_num = ethConfig_.mdioGpio;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ethConfig_.phyAddr;
    phy_config.reset_gpio_num = ethConfig_.rstGpio;

    // Helper to unwind whatever was created so far on any failure, ethInit runs once at boot, but a clean release means a broken PHY/cable degrades (returns false → the WiFi/AP cascade takes over) instead of leaking the netif + MAC/PHY drivers.
    auto fail = [&](const char* what, esp_eth_mac_t* m, esp_eth_phy_t* p) -> bool {
        ESP_LOGE(NET_TAG, "Ethernet %s", what);
        if (p) p->del(p);
        if (m) m->del(m);
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        return false;
    };

    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (!mac) return fail("MAC create failed", nullptr, nullptr);
    // One PHY constructor is a managed component while the generic one stays in the core, and its symbol is declared only on the chip that uses it. So the runtime branch below is wrapped to match, or another build would fail to compile an undeclared call.
    esp_eth_phy_t* phy;
#ifdef CONFIG_IDF_TARGET_ESP32P4
    if (ethConfig_.phyType == ethIp101) phy = esp_eth_phy_new_ip101(&phy_config);
    else                                phy = esp_eth_phy_new_generic(&phy_config);
#else
    // LAN8720 (classic RMII) and YT8531 (S31 RGMII) are both IEEE-802.3-standard-register PHYs → the generic ctor drives both; no PHY-specific managed component needed.
    phy = esp_eth_phy_new_generic(&phy_config);
#endif
    if (!phy) return fail("PHY create failed", mac, nullptr);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = nullptr;
    esp_err_t err = esp_eth_driver_install(&eth_config, &eth_handle);
    if (err != ESP_OK) {
        return fail(esp_err_to_name(err), mac, phy);
    }
    // From here the driver owns mac+phy (driver_uninstall frees them); the remaining failure paths uninstall the driver instead of del-ing mac/phy.
#ifdef CONFIG_IDF_TARGET_ESP32S31
    // The YT8531 needs a vendor-specific auto-nego re-enable (+ RGMII delays) the generic driver can't do, without it the RGMII link never negotiates. Run right after install (driver/PHY exist, before start), the same order IDF's example uses. Non-fatal: a failed register write logs a warning and continues (the link may not come up) rather than dropping Ethernet.
    {
        esp_err_t yterr = ethYt8531BoardInit(eth_handle);
        if (yterr != ESP_OK) ESP_LOGW(NET_TAG, "YT8531 RGMII init failed: %s (link may not come up)",
                                      esp_err_to_name(yterr));
    }
#endif
    ESP_ERROR_CHECK(esp_netif_attach(ethNetif_, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &ethEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &ethEventHandler, nullptr));

    err = esp_eth_start(eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethEventHandler);
        esp_eth_driver_uninstall(eth_handle);  // frees mac + phy
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        return false;
    }
    // DHCP hostname is set in the ETHERNET_EVENT_CONNECTED handler, not here, see the comment there (IDF starts the eth DHCP client on link-up, which would clobber a name set at init time).

    ethHandle_ = eth_handle;   // retained (ethStop is W5500-only today, but keep it set)
    ESP_LOGI(NET_TAG, "Ethernet init done (%s, non-blocking)", isEsp32S31 ? "RGMII, S31" : "RMII");
    return true;
}
#endif // CONFIG_ETH_USE_ESP32_EMAC

// The external controller path, compiled only where that driver is enabled and no internal one exists, its constructors coming from a managed component.
// Pins come from the runtime config, which such a board must set, there being no universal default.
// It returns false on any failure including no module present, so a build with the driver in but nothing attached cascades to the radio cleanly.
#ifdef MM_ETH_W5500
static bool ethInitSpi() {
    if (ethConfig_.spiMiso < 0 || ethConfig_.spiMosi < 0 ||
        ethConfig_.spiSck < 0 || ethConfig_.spiCs < 0) {
        ESP_LOGW(NET_TAG, "W5500 selected but SPI pins unset — skipping (set them in deviceModels.json)");
        return false;
    }
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    ethNetif_ = esp_netif_new(&netif_cfg);

    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num = ethConfig_.spiMiso;
    buscfg.mosi_io_num = ethConfig_.spiMosi;
    buscfg.sclk_io_num = ethConfig_.spiSck;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    constexpr spi_host_device_t kSpiHost = SPI2_HOST;
    if (spi_bus_initialize(kSpiHost, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(NET_TAG, "W5500 SPI bus init failed");
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        return false;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.mode = 0;
    devcfg.clock_speed_hz = 20 * 1000 * 1000;   // 20 MHz — W5500 spec ceiling for stable SPI
    devcfg.spics_io_num = ethConfig_.spiCs;
    devcfg.queue_size = 20;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(kSpiHost, &devcfg);
    w5500_config.int_gpio_num = ethConfig_.spiIrq;   // wired INT pin (interrupt), or -1 for polling
    if (ethConfig_.spiIrq >= 0) {
        // Interrupt-driven RX: the W5500 driver registers its handler with gpio_isr_handler_add(), which requires the per-pin ISR service to be installed first. Install it once here; ESP_ERR_INVALID_STATE means another driver already installed it, which is fine.
        esp_err_t isr = gpio_install_isr_service(0);
        if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(NET_TAG, "gpio_install_isr_service failed (%s) — W5500 INT may not fire",
                     esp_err_to_name(isr));
        }
    } else {
        // No INT pin: IDF v6's W5500 driver requires a poll period when int_gpio_num < 0, so drive the MAC by polling, 10 ms services RX promptly without an interrupt.
        w5500_config.poll_period_ms = 10;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ethConfig_.phyAddr;
    phy_config.reset_gpio_num = ethConfig_.rstGpio;   // -1 if the module self-resets

    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_config);
    auto fail = [&](const char* what) -> bool {
        ESP_LOGE(NET_TAG, "W5500 %s", what);
        if (phy) phy->del(phy);
        if (mac) mac->del(mac);
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        spi_bus_free(kSpiHost);
        return false;
    };
    if (!mac) return fail("MAC create failed");
    if (!phy) return fail("PHY create failed");

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = nullptr;
    if (esp_eth_driver_install(&eth_config, &eth_handle) != ESP_OK) return fail("driver install failed");

    // W5500 has no factory MAC, derive one from the chip's efuse base MAC so the netif has a unique address (IDF requirement for SPI Ethernet).
    uint8_t mac_addr[6];
    esp_read_mac(mac_addr, ESP_MAC_ETH);
    esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr);

    ESP_ERROR_CHECK(esp_netif_attach(ethNetif_, esp_eth_new_netif_glue(eth_handle)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethEventHandler, nullptr));

    if (esp_eth_start(eth_handle) != ESP_OK) {
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethEventHandler);
        esp_eth_driver_uninstall(eth_handle);
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        spi_bus_free(kSpiHost);
        return false;
    }
    // DHCP hostname is set in the ETHERNET_EVENT_CONNECTED handler (see note there).
    ethHandle_ = eth_handle;   // retained for a live reconfigure (ethStop)
    ethSpiActive_ = true;
    ESP_LOGI(NET_TAG, "Ethernet init done (W5500 SPI, non-blocking)");
    return true;
}
#endif // MM_ETH_W5500

// Tear a running driver down so a fresh init can bring it up with new config, the live-reconfigure path. Only the external driver uses it today, the internal one's release being fiddlier and backlogged; safe to call when nothing is running.
void ethStop() {
    if (!ethHandle_) return;
    esp_eth_stop(ethHandle_);
    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethEventHandler);
    esp_eth_driver_uninstall(ethHandle_);
    ethHandle_ = nullptr;
    if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
#ifdef MM_ETH_W5500
    if (ethSpiActive_) { spi_bus_free(SPI2_HOST); ethSpiActive_ = false; }
#endif
    ethLinkUp_.store(false, std::memory_order_relaxed);
    ethConnected_.store(false, std::memory_order_relaxed);
}

#ifdef CONFIG_ETH_USE_OPENETH
// The emulated controller: no pins, no clock and no register access, the emulator presenting a ready one, so this path is a fraction of the real setup.
// It exists because an emulated device with no address stack can only be watched on the console.
// While with one the interface and the tests drive it exactly as on hardware.
static bool ethInitOpeneth() {
    // STEP-BY-STEP LOGGED, deliberately. Bringing this up is a chain of six calls where any one can fail quietly, and a silent failure looks identical to a working stack with no cable: the device simply never gets an address. Logging each step means the serial log ALONE says which link of the chain broke, instead of the failure having to be re-derived from a missing IP.
    std::printf("mm_net: openeth 1/6: creating netif\n");
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    ethNetif_ = esp_netif_new(&netif_cfg);
    if (!ethNetif_) { std::printf("mm_net: openeth 1/6 FAILED: esp_netif_new returned null\n"); return false; }

    std::printf("mm_net: openeth 2/6: creating MAC + PHY\n");
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;              // the address QEMU's model answers on
    phy_config.reset_gpio_num = -1;       // nothing to reset in an emulator

    esp_eth_mac_t* mac = esp_eth_mac_new_openeth(&mac_config);
    // The generic ctor: QEMU's model answers the standard IEEE-802.3 registers, and the per-PHY ctors (dp83848 and friends) left esp_eth core in IDF v6 anyway.
    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phy_config);
    if (!mac || !phy) {
        std::printf("mm_net: openeth 2/6 FAILED: mac=%p phy=%p\n", (void*)mac, (void*)phy);
        if (phy) phy->del(phy);
        if (mac) mac->del(mac);
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        return false;
    }

    std::printf("mm_net: openeth 3/6: installing driver\n");
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = nullptr;
    esp_err_t err = esp_eth_driver_install(&eth_config, &eth_handle);
    if (err != ESP_OK) {
        std::printf("mm_net: openeth 3/6 FAILED: %s\n", esp_err_to_name(err));
        phy->del(phy); mac->del(mac);
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        return false;
    }

    // PROMISCUOUS: QEMU's MAC implements no multicast filter, so esp_netif's attach logs an error registering one for IPv4. Accepting every frame is the emulator's stand-in for the filter it does not model, and costs nothing here, there is no real wire to be flooded from.
    std::printf("mm_net: openeth 4/6: promiscuous mode\n");
    bool promiscuous = true;
    err = esp_eth_ioctl(eth_handle, ETH_CMD_S_PROMISCUOUS, &promiscuous);
    if (err != ESP_OK) ESP_LOGW(NET_TAG, "openeth 4/6: promiscuous not set (%s), continuing",
                                esp_err_to_name(err));

    // From here the driver owns mac+phy, so every failure unwinds through driver_uninstall rather than del-ing them, exactly as ethInitEmac and ethInitSpi do. Written once as a lambda because three exits share it, and a half-cleaned failure leaks a netif and a driver on a device that then has no network to report the problem over.
    auto fail = [&](const char* what) -> bool {
        std::printf("mm_net: openeth FAILED: %s\n", what);
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethEventHandler);
        esp_eth_driver_uninstall(eth_handle);   // frees mac + phy
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        return false;
    };

    std::printf("mm_net: openeth 5/6: attaching netif glue\n");
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    if (!glue) return fail("netif glue is null");
    err = esp_netif_attach(ethNetif_, glue);
    if (err != ESP_OK) return fail(esp_err_to_name(err));

    // The handlers before the start: link-up is what kicks the address client and the address event is what lets the module proceed. Registering them after would race the first event the emulator raises immediately, and omitting them leaves an interface that is up and never asks for an address.
    std::printf("mm_net: openeth 6/6: registering event handlers + starting driver\n");
    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler, nullptr);
    if (err != ESP_OK) return fail(esp_err_to_name(err));
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethEventHandler, nullptr);
    if (err != ESP_OK) return fail(esp_err_to_name(err));

    err = esp_eth_start(eth_handle);
    if (err != ESP_OK) return fail(esp_err_to_name(err));

    // Retained like the other two init paths, so ethStop() can tear this interface down instead of silently no-opping on a null handle.
    ethHandle_ = eth_handle;
    std::printf("mm_net: openeth up, waiting for link + DHCP\n");
    return true;
}
#endif  // CONFIG_ETH_USE_OPENETH

bool ethInit() {
    ensureNetifInit();
    // Dispatch on the board's PHY type at run time, each path returning false on any failure including none present, so the module cascades to the radio.
    // A build with a driver in but nothing wired falls through without grabbing pins or hanging.
    // And a PHY whose driver is not in this firmware returns false the same way.
    switch (ethConfig_.phyType) {
#ifdef MM_ETH_W5500
        case ethW5500:   return ethInitSpi();
#endif
#ifdef CONFIG_ETH_USE_OPENETH
        case ethOpeneth: return ethInitOpeneth();
#endif
#ifdef CONFIG_ETH_USE_ESP32_EMAC
        case ethLan8720:
        case ethIp101:                   // RMII PHYs (classic ESP32, P4)
        case ethYt8531:  return ethInitEmac();   // RGMII PHY (S31); ethInitEmac's RGMII block is #ifdef'd to the S31 chip
#endif
        default:         return false;   // ethNone, or a PHY this firmware can't drive
    }
}

bool ethLinkUp() MM_NONBLOCKING {
    return ethLinkUp_.load(std::memory_order_relaxed);
}

bool ethConnected() MM_NONBLOCKING {
    return ethConnected_.load(std::memory_order_relaxed);
}

void ethGetIPv4(uint8_t out[4]) MM_NONBLOCKING {
    netifIPv4(ethNetif_, out);
}

// One raw frame straight to the controller, handed to the transfer engine with no address stack, route lookup or lease involved.
// Gated on the LINK rather than on having an address, since that stack is a layer above and requiring one would stop a board driving panels it can drive.
// Synchronous by contract, so the caller may reuse its buffer at once, and any error means the frame did not go out.
// The count below is how many drivers have claimed the link.
static std::atomic<int> ethRawClaims_{0};

void ethClaimRawL2(bool claim) {
    if (claim) {
        ethRawClaims_.fetch_add(1, std::memory_order_relaxed);
    } else if (ethRawClaims_.load(std::memory_order_relaxed) > 0) {
        ethRawClaims_.fetch_sub(1, std::memory_order_relaxed);
    }
}

bool ethRawL2Claimed() MM_NONBLOCKING {
    return ethRawClaims_.load(std::memory_order_relaxed) > 0;
}

// Consecutive failures, so a caller can distinguish back-pressure from a wedged path (see platform.h). Written on the render task, read by the driver's 1 Hz status tick.
static std::atomic<uint32_t> ethSendFails_{0};
// Split by cause; see platform.h. esp_eth_transmit checks the link BEFORE the MAC, so the two errors are genuinely distinct conditions rather than degrees of the same one.
static std::atomic<uint32_t> ethFailLinkDown_{0};
static std::atomic<uint32_t> ethFailRingFull_{0};

bool ethSendRaw(const uint8_t* frame, size_t len) MM_NONBLOCKING {
    if (!ethHandle_ || !frame || len == 0) return false;
    if (!ethLinkUp_.load(std::memory_order_relaxed)) {
        // Counted, not silent: the driver counts every false into its own total, so skipping this one would make `dropped` and the per-cause totals describe different sets of frames. Deliberately NOT part of the streak: the streak drives wedge detection and re-arming, and a link genuinely down is the case a restart cannot fix.
        ethFailLinkDown_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const esp_err_t err = esp_eth_transmit(ethHandle_, const_cast<uint8_t*>(frame), len);
    if (err != ESP_OK) {
        ethSendFails_.fetch_add(1, std::memory_order_relaxed);
        if (err == ESP_ERR_INVALID_STATE) ethFailLinkDown_.fetch_add(1, std::memory_order_relaxed);
        else if (err == ESP_ERR_NO_MEM)   ethFailRingFull_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ethSendFails_.store(0, std::memory_order_relaxed);
    return true;
}

// The MAC takes each frame as ethSendRaw hands it over, so no burst is held back and there is nothing to flush. Present because the seam is platform-wide (see platform.h).
void ethFlushRaw() MM_NONBLOCKING {}

void ethSendFailCounts(uint32_t& linkDown, uint32_t& ringFull) MM_NONBLOCKING {
    linkDown = ethFailLinkDown_.load(std::memory_order_relaxed);
    ringFull = ethFailRingFull_.load(std::memory_order_relaxed);
}

uint32_t ethSendFailStreak() MM_NONBLOCKING {
    return ethSendFails_.load(std::memory_order_relaxed);
}

// One MAC per chip, so there is no interface to choose: ethSendRaw always uses it. Accepting the call (rather than failing) keeps the driver's control identical on every target, the field is simply ignored here, which is what the driver's own comment tells the user.
bool ethBindRawInterface(const char*) { return true; }

bool ethRestartTx() {
    if (!ethHandle_) return false;
    // Clear our own flag first, since the restart re-runs negotiation and the event sets it again if the link really returns.
    // Leaving it true keeps the send trying against a driver mid-restart.
    // A failed stop leaves a state we did not establish, so report rather than starting on top of it.
    // Nothing is cleared before this point: clearing and then failing would refuse transmit permanently with no event able to set the flag again.
    if (esp_eth_stop(ethHandle_) != ESP_OK) return false;
    ethLinkUp_.store(false, std::memory_order_relaxed);
    ethSendFails_.store(0, std::memory_order_relaxed);
    return esp_eth_start(ethHandle_) == ESP_OK;
}

// Negotiated link speed, asked of the driver rather than assumed from the PHY type: a gigabit PHY on a 100 Mbit switch (or a bad cable) negotiates down, and that is precisely the case worth reporting. 0 when there is no link to describe.
uint16_t ethLinkSpeedMbps() MM_NONBLOCKING {
    if (!ethHandle_ || !ethLinkUp_.load(std::memory_order_relaxed)) return 0;
    eth_speed_t speed = ETH_SPEED_10M;
    if (esp_eth_ioctl(ethHandle_, ETH_CMD_G_SPEED, &speed) != ESP_OK) return 0;
    switch (speed) {
        case ETH_SPEED_1000M: return 1000;
        case ETH_SPEED_100M:  return 100;
        default:              return 10;
    }
}

#else // MM_NO_ETH — firmware excludes EMAC support (chip-side or sdkconfig fragment
      // wasn't layered. Provide stubs matching the desktop platform's no-eth behavior so NetworkModule's cascade falls straight to WiFi (or AP).

void setEthConfig(const EthPinConfig&)  {}
void ethStop()                          {}
bool ethInit()                          { return false; }
bool ethLinkUp() MM_NONBLOCKING                        { return false; }
bool ethConnected() MM_NONBLOCKING                     { return false; }
void ethGetIPv4(uint8_t out[4]) MM_NONBLOCKING         { out[0] = out[1] = out[2] = out[3] = 0; }
bool ethSendRaw(const uint8_t*, size_t) MM_NONBLOCKING { return false; }   // no MAC to hand a frame to
void ethFlushRaw() MM_NONBLOCKING                      {}                  // nothing batched
void ethClaimRawL2(bool)                               {}                  // no link to claim
bool ethRawL2Claimed() MM_NONBLOCKING                  { return false; }
bool ethRestartTx()                                    { return false; }   // no driver to restart
uint16_t ethLinkSpeedMbps() MM_NONBLOCKING             { return 0; }       // no link to describe
uint32_t ethSendFailStreak() MM_NONBLOCKING            { return 0; }       // nothing sends, nothing fails
void ethSendFailCounts(uint32_t& a, uint32_t& b) MM_NONBLOCKING { a = b = 0; }
bool ethBindRawInterface(const char*)                  { return true; }    // no MAC, nothing to bind

#endif // MM_NO_ETH

#ifndef MM_NO_WIFI

// Set while a deliberate teardown (wifiStaStop) is in progress, so the disconnect it provokes is not answered with a reconnect, that would race esp_wifi_deinit() with an in-flight connect. Atomic, not volatile: it is written from a task and read from IDF's event-loop task, and volatile carries no atomicity or ordering guarantee, only the compiler's promise not to elide the access.
static std::atomic<bool> wifiStaStopping_{false};

// How many stations are associated with our SoftAP right now. Written from IDF's event-loop task, read from the render task, so it is atomic.
static std::atomic<uint32_t> apClients_{0};

// WiFi event handler
static void wifiEventHandler(void* /*arg*/, esp_event_base_t base,
                             int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_CONNECTED) {
            // L2 association complete (before DHCP). In Static mode, pin the stored config now and mark connected, a DHCP-less network never fires GOT_IP, so waiting for it would strand a static STA. Mirrors the eth CONNECTED handler's ethStatic_ re-pin. DHCP mode is a no-op here (the DHCP client runs and GOT_IP sets wifiStaConnected_ as before).
            wifiStaAssociated_.store(true, std::memory_order_relaxed);
            // The event carries the AP's identity, so nobody ever has to ask the radio for it.
            if (const auto* ev = static_cast<wifi_event_sta_connected_t*>(data)) {
                uint64_t ap = 0;
                for (int i = 0; i < 6; i++) ap = (ap << 8) | ev->bssid[i];
                radioAp_.store((ap << 8) | ev->channel, std::memory_order_relaxed);
            }
            if (staStatic_.load(std::memory_order_acquire)) {
                netSetStaticIPv4(NetIface::Sta, staStaticIp_, staStaticGw_, staStaticMask_, staStaticDns_);
            }
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifiStaConnected_.store(false, std::memory_order_relaxed);
            wifiStaAssociated_.store(false, std::memory_order_relaxed);
            radioAp_.store(0, std::memory_order_relaxed);
            radioRssi_.store(0, std::memory_order_relaxed);
            // The reconnect is ours to make, and unbounded: @xref{the-reconnect-is-ours-to-make-and-unbounded|why}.
            if (!wifiStaStopping_.load(std::memory_order_relaxed)) {
                // Immediately, and without sleeping to pace it: the pacing is free and blocking here would stall the whole stack. The counter is diagnostic and does not gate the retry.
                static uint32_t attempts = 0;
                if (attempts < UINT32_MAX) attempts++;
                // Log the reason, which the radio already named: the generic formatter does not cover this range, so the number is logged and the common ones named.
                const auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
                const uint8_t why = ev ? ev->reason : 0;
                const char* whyText =
                    why == WIFI_REASON_NO_AP_FOUND        ? " (no AP with that SSID — wrong name, or a 5 GHz-only network: ESP32 is 2.4 GHz)"
                  : why == WIFI_REASON_AUTH_FAIL          ? " (auth failed — wrong password)"
                  : why == WIFI_REASON_HANDSHAKE_TIMEOUT  ? " (handshake timeout — usually a wrong password)"
                  : why == WIFI_REASON_BEACON_TIMEOUT     ? " (beacon timeout — out of range or the AP went away)"
                  : "";
                ESP_LOGI(NET_TAG, "WiFi STA disconnected, reason %u%s — reconnecting (attempt %u)",
                         (unsigned)why, whyText, (unsigned)attempts);
                esp_wifi_connect();
            } else {
                ESP_LOGI(NET_TAG, "WiFi STA disconnected");
            }
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            // Track the count so the AP-fallback's periodic STA retry can hold off while somebody is actually using the portal: re-initializing STA switches the radio to WIFI_MODE_STA, which drops the AP. See NetworkModule's State::AP retry.
            apClients_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGI(NET_TAG, "WiFi AP client connected");
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            uint32_t n = apClients_.load(std::memory_order_relaxed);
            if (n > 0) apClients_.fetch_sub(1, std::memory_order_relaxed);
            ESP_LOGI(NET_TAG, "WiFi AP client disconnected");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(NET_TAG, "WiFi STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifiStaConnected_.store(true, std::memory_order_relaxed);
    }
}

// True on success, failures propagating up so the module's state machine can fall back to a path that needs no radio.
// This once aborted the device on any failure.
// Fatal exactly when the heap was too fragmented for the radio to claim its buffers, which is the case the fallback existed for.
static bool ensureWifiInit() {
    if (wifiInitDone_) return true;

    // No bring-up is needed on the chip whose radio is a companion: @xref{the-companion-chip-initializes-itself|why calling init here breaks a live link}.

    ensureNetifInit();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "esp_wifi_init failed: %s (heap %u, largest %u)",
                 esp_err_to_name(err),
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        return false;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &wifiEventHandler, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "WIFI_EVENT register failed: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return false;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &wifiEventHandler, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "IP_EVENT register failed: %s", esp_err_to_name(err));
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler);
        esp_wifi_deinit();
        return false;
    }

    wifiInitDone_ = true;
    ensureRadioPoller();
    return true;
}

bool wifiStaInit(const char* ssid, const char* password) {
    if (!ssid || ssid[0] == 0) return false;

    // Guard against repeated init leaking the previous netif (the cascade can call wifiStaInit again after an Ethernet drop without a prior stop). Stop before ensureWifiInit(), wifiStaStop() deinits the WiFi driver.
    if (staNetif_) wifiStaStop();
    if (!ensureWifiInit()) return false;   // out-of-memory / event register failure

    staNetif_ = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password && password[0] != 0) {
        std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password, sizeof(wifi_config.sta.password) - 1);
    }

    // From here every call can fail for transient runtime reasons (mode conflict, driver-state mismatch, etc.). Log + clean up + return false so NetworkModule's state machine can fall back rather than panic.
    esp_err_t err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi STA set_mode failed: %s", esp_err_to_name(err));
        wifiStaStop();
        return false;
    }
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config)) != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi STA set_config failed: %s", esp_err_to_name(err));
        wifiStaStop();
        return false;
    }
    if ((err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi STA start failed: %s", esp_err_to_name(err));
        wifiStaStop();
        return false;
    }
    // DHCP hostname (option 12), after esp_wifi_start: the STA netif isn't "ready" (set_hostname returns IF_NOT_READY) until the WiFi driver glue starts it. Association + DHCP happen later still, so the name lands in the lease request.
    applyHostname(staNetif_);

    // Disable modem power saving, whose default sleeps the radio between beacons and causes intermittent stalls in socket handling and the pause class of glitch. The whole lineage turns it off for the same reason, a wall-powered controller having no battery to save; non-fatal if it fails.
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) {
        ESP_LOGW(NET_TAG, "WiFi power-save disable failed: %s", esp_err_to_name(err));
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi STA connect failed: %s", esp_err_to_name(err));
        wifiStaStop();   // tear down the driver/netif stood up above
        return false;
    }

    ESP_LOGI(NET_TAG, "WiFi STA init done (non-blocking), SSID: %s", ssid);
    return true;
}

bool wifiStaConnected() MM_NONBLOCKING {
    return wifiStaConnected_.load(std::memory_order_relaxed);
}

void wifiStaGetIPv4(uint8_t out[4]) {
    netifIPv4(staNetif_, out);
}

void wifiStaStop() {
    // Tell the event handler this disconnect is deliberate, so it does not answer with a reconnect that would then race esp_wifi_deinit() below.
    wifiStaStopping_.store(true, std::memory_order_relaxed);
    esp_wifi_disconnect();
    esp_wifi_stop();
    // Unregister event handlers before deinit so subsequent init/stop cycles don't accumulate duplicate registrations. Guard on wifiInitDone_ since ensureWifiInit() bails before the registration step if init failed.
    if (wifiInitDone_) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler);
    }
    esp_wifi_deinit();
    if (staNetif_) {
        esp_netif_destroy_default_wifi(staNetif_);
        staNetif_ = nullptr;
    }
    wifiStaConnected_.store(false, std::memory_order_relaxed);
    // Association state must clear with the interface: a later netSetStaticIPv4(Sta) keys off this flag, and a stale `true` from a torn-down STA would apply a static IP to nothing.
    wifiStaAssociated_.store(false, std::memory_order_relaxed);
    wifiInitDone_ = false;
    wifiStaStopping_.store(false, std::memory_order_relaxed);   // a later wifiStaInit() reconnects normally
    ESP_LOGI(NET_TAG, "WiFi STA stopped + deinit");
}

// Cached, see radioPollTask: none of the readouts below asks the radio.
int wifiStaRssi() {
    if (!wifiStaConnected_.load(std::memory_order_relaxed)) return 0;
    return radioRssi_.load(std::memory_order_relaxed);
}

void wifiStaBssid(uint8_t out[6]) {
    const uint64_t ap = radioAp_.load(std::memory_order_relaxed) >> 8;
    for (int i = 0; i < 6; i++) out[i] = static_cast<uint8_t>(ap >> (8 * (5 - i)));
}

int wifiStaChannel() {
    return static_cast<int>(radioAp_.load(std::memory_order_relaxed) & 0xFF);
}

bool wifiApInit(const char* apName, const char* ip) {
    // Guard against repeated init leaking the previous AP netif. Stop before ensureWifiInit(), wifiApStop() deinits the WiFi driver.
    if (apNetif_) wifiApStop();
    if (!ensureWifiInit()) return false;   // out-of-memory / event register failure

    apNetif_ = esp_netif_create_default_wifi_ap();

    // Set static IP for AP
    if (ip && ip[0] != 0) {
        esp_netif_dhcps_stop(apNetif_);
        esp_netif_ip_info_t ipInfo = {};
        esp_netif_str_to_ip4(ip, &ipInfo.ip);
        ipInfo.gw = ipInfo.ip;
        IP4_ADDR(&ipInfo.netmask, 255, 255, 255, 0);
        esp_netif_set_ip_info(apNetif_, &ipInfo);
        esp_netif_dhcps_start(apNetif_);
    }

    wifi_config_t wifi_config = {};
    if (apName) {
        std::strncpy(reinterpret_cast<char*>(wifi_config.ap.ssid), apName, sizeof(wifi_config.ap.ssid) - 1);
        wifi_config.ap.ssid_len = static_cast<uint8_t>(std::strlen(apName));
    }
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    esp_err_t err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi AP set_mode failed: %s", esp_err_to_name(err));
        wifiApStop();
        return false;
    }
    if ((err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config)) != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi AP set_config failed: %s", esp_err_to_name(err));
        wifiApStop();
        return false;
    }
    if ((err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi AP start failed: %s", esp_err_to_name(err));
        wifiApStop();
        return false;
    }

    wifiApActive_ = true;
    apClients_.store(0, std::memory_order_relaxed);
    ESP_LOGI(NET_TAG, "WiFi AP started: %s @ %s", apName ? apName : "?", ip ? ip : "?");
    return true;
}

bool wifiApConnected() {
    return wifiApActive_;
}

uint32_t wifiApClientCount() { return apClients_.load(std::memory_order_relaxed); }

void wifiApStop() {
    esp_wifi_stop();
    // Mirror wifiStaStop(): unregister the event handlers before deinit so re-init doesn't accumulate duplicate registrations.
    if (wifiInitDone_) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler);
    }
    esp_wifi_deinit();
    if (apNetif_) {
        esp_netif_destroy_default_wifi(apNetif_);
        apNetif_ = nullptr;
    }
    wifiApActive_ = false;
    apClients_.store(0, std::memory_order_relaxed);
    wifiInitDone_ = false;
    ESP_LOGI(NET_TAG, "WiFi AP stopped + deinit");
}

int wifiTxPower() {
    if (!wifiInitDone_) return 0;
    // ESP-IDF reports TX power in units of 0.25 dBm; round to nearest whole dBm.
    return (radioTxPowerQ_.load(std::memory_order_relaxed) + 2) / 4;
}

bool wifiSetTxPower(int8_t quarterDbm) {
    if (quarterDbm == 0) return true;       // 0 = "no override", caller-friendly skip
    if (!wifiInitDone_) return false;       // esp_wifi_set_max_tx_power requires the stack started
    // ESP-IDF accepts 8..84 (2..21 dBm); clamp into range so a bad injected value doesn't make esp_wifi_set_max_tx_power return ESP_ERR_INVALID_ARG and leave the radio at default power without anyone noticing.
    if (quarterDbm < 8)  quarterDbm = 8;
    if (quarterDbm > 84) quarterDbm = 84;
    esp_err_t err = esp_wifi_set_max_tx_power(quarterDbm);
    if (err != ESP_OK) {
        ESP_LOGW(NET_TAG, "WiFi set TX power %d (q-dBm) failed: %s", quarterDbm, esp_err_to_name(err));
        return false;
    }
    radioTxPowerQ_.store(quarterDbm, std::memory_order_relaxed);   // so the readout follows the set at once
    ESP_LOGI(NET_TAG, "WiFi TX power capped to %d (q-dBm) ≈ %d dBm", quarterDbm, (quarterDbm + 2) / 4);
    return true;
}

#else // MM_NO_WIFI — Ethernet-only build: WiFi compiled out.

// Stub definitions so the linker is satisfied (platform.h declares these and NetworkModule's discarded `if constexpr (hasWiFi)` branch still ODR-uses them). With hasWiFi==false the calls are not code-generated, so --gc-sections drops these stubs from the final image.
bool wifiStaInit(const char* /*ssid*/, const char* /*password*/) { return false; }
bool wifiStaConnected() MM_NONBLOCKING { return false; }
void wifiStaGetIPv4(uint8_t out[4])      { out[0] = out[1] = out[2] = out[3] = 0; }
void wifiStaStop() {}
int wifiStaRssi() { return 0; }
void wifiStaBssid(uint8_t out[6]) { std::memset(out, 0, 6); }
int wifiStaChannel() { return 0; }
bool wifiApInit(const char* /*apName*/, const char* /*ip*/) { return false; }
bool wifiApConnected() { return false; }
void wifiApStop() {}
uint32_t wifiApClientCount() { return 0; }
int wifiTxPower() { return 0; }
// Match the API contract: 0 is a successful no-op even when WiFi isn't compiled in. Any non-zero value (actual cap attempt) returns false because there's no radio to set.
bool wifiSetTxPower(int8_t quarterDbm) { return quarterDbm == 0; }

#endif // MM_NO_WIFI

// Socket-safe once any interface has an IP: at that point esp_netif_init() has run and the lwip core mutex exists, so opening a socket won't assert. Each predicate is stubbed to false in the build that lacks its interface, so this OR compiles and answers correctly on every firmware.
bool networkReady() {
    return ethConnected() || wifiStaConnected() || wifiApConnected();
}

// Resolve a NetIface to its netif pointer. Each arm is compiled out on a build that lacks that interface (MM_NO_ETH / MM_NO_WIFI), so the static-addressing setters below compile everywhere and simply no-op for an absent interface (null netif → the callers return early).
static esp_netif_t* resolveNetif(NetIface iface) {
    switch (iface) {
        case NetIface::Eth:
#ifndef MM_NO_ETH
            return ethNetif_;
#else
            return nullptr;
#endif
        case NetIface::Sta:
#ifndef MM_NO_WIFI
            return staNetif_;
#else
            return nullptr;
#endif
    }
    return nullptr;
}

// Pin a fixed address onto a client interface, stopping its address client so a lease cannot overwrite it.
// Then set the address, gateway and mask, plus a name server when one is given.
// The mirror of the access-point block in client form; an all-zero address is a no-op guard, and this is idempotent.
void netSetStaticIPv4(NetIface iface, const uint8_t ip[4], const uint8_t gw[4],
                      const uint8_t mask[4], const uint8_t dns[4]) {
    esp_netif_t* netif = resolveNetif(iface);
    if (!netif) return;
    if (!ip || (!ip[0] && !ip[1] && !ip[2] && !ip[3])) return;   // no static IP set — leave DHCP

    esp_netif_dhcpc_stop(netif);   // ignore ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED

    esp_netif_ip_info_t info = {};
    IP4_ADDR(&info.ip,      ip[0],   ip[1],   ip[2],   ip[3]);
    IP4_ADDR(&info.gw,      gw[0],   gw[1],   gw[2],   gw[3]);
    IP4_ADDR(&info.netmask, mask[0], mask[1], mask[2], mask[3]);
    esp_netif_set_ip_info(netif, &info);

    if (dns && (dns[0] || dns[1] || dns[2] || dns[3])) {   // only set DNS if one was given
        esp_netif_dns_info_t dnsInfo = {};
        dnsInfo.ip.type = ESP_IPADDR_TYPE_V4;
        IP4_ADDR(&dnsInfo.ip.u_addr.ip4, dns[0], dns[1], dns[2], dns[3]);
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dnsInfo);
    }

    // Applying a static IP IS the "interface now has an address" moment, there is no DHCP GOT_IP event to wait for. Both interfaces' "connected" flags key off that DHCP event, so a static apply must set them itself (symmetric with how GOT_IP would). Record the config + flag too, so each interface's link-up handler re-pins static on a reconnect instead of restarting DHCP.
#ifndef MM_NO_ETH
    if (iface == NetIface::Eth) {
        // Octets first, flag last (release): the event task's link-up re-pin acquires the flag, so a true flag guarantees a fully-written config.
        for (int i = 0; i < 4; i++) {
            ethStaticIp_[i] = ip[i]; ethStaticGw_[i] = gw[i];
            ethStaticMask_[i] = mask[i]; ethStaticDns_[i] = dns ? dns[i] : 0;
        }
        ethStatic_.store(true, std::memory_order_release);
        // Mark connected only if the link is actually up, else a static apply racing a cable pull would leave ethConnected() true on a dead link (the state machine would sit in ConnectedEth instead of cascading). On a genuine link-up the CONNECTED handler re-applies + sets it.
        if (ethLinkUp_.load(std::memory_order_relaxed)) ethConnected_.store(true, std::memory_order_relaxed);
    }
#endif
#ifndef MM_NO_WIFI
    if (iface == NetIface::Sta) {
        // Octets first, flag last (release), same publish contract as the eth arm above.
        for (int i = 0; i < 4; i++) {
            staStaticIp_[i] = ip[i]; staStaticGw_[i] = gw[i];
            staStaticMask_[i] = mask[i]; staStaticDns_[i] = dns ? dns[i] : 0;
        }
        staStatic_.store(true, std::memory_order_release);
        // wifiStaConnected_ normally means "got a DHCP IP", which never fires on a DHCP-less network, the very case static addressing exists for. So mark connected here (the IP is applied); WIFI_EVENT_STA_CONNECTED re-applies on a reconnect. Only when the STA is actually associated, so a static apply while the radio is down doesn't fake a connection.
        if (wifiStaAssociated_.load(std::memory_order_relaxed)) wifiStaConnected_.store(true, std::memory_order_relaxed);
    }
#endif
    ESP_LOGI(NET_TAG, "Static IPv4 set on %s: %u.%u.%u.%u",
             iface == NetIface::Eth ? "eth" : "sta", ip[0], ip[1], ip[2], ip[3]);
}

// Return a client interface to DHCP: (re)start its DHCP client so it re-leases without a reboot. The counterpart to netSetStaticIPv4 for a Static→DHCP toggle. Safe if already running.
void netSetDhcp(NetIface iface) {
    esp_netif_t* netif = resolveNetif(iface);
    if (!netif) return;
#ifndef MM_NO_ETH
    if (iface == NetIface::Eth) {
        ethStatic_.store(false, std::memory_order_release);   // link-up handler goes back to the DHCP hostname path
        ethConnected_.store(false, std::memory_order_relaxed);   // static forced this true; drop it so the state machine re-evaluates
                                 // (GOT_IP re-sets it on a lease). Else a Static→DHCP toggle on a network that can't lease wedges in ConnectedEth at 0.0.0.0.
    }
#endif
#ifndef MM_NO_WIFI
    if (iface == NetIface::Sta) {
        staStatic_.store(false, std::memory_order_release);   // stop re-pinning static on the next association
        wifiStaConnected_.store(false, std::memory_order_relaxed);  // static forced this true; GOT_IP re-sets it once DHCP leases
    }
#endif
    esp_netif_dhcpc_start(netif);   // ignore ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED
    ESP_LOGI(NET_TAG, "DHCP restarted on %s", iface == NetIface::Eth ? "eth" : "sta");
}

// Bring the discovery stack up and advertise this device by name, gated on the user's toggle while the stack itself stays up.
// So toggling back on re-advertises without a full restart.
// Advertise-only: peer discovery is datagram presence elsewhere.
static bool mdnsStackUp_ = false;

static bool ensureMdnsStack() {
    if (mdnsStackUp_) return true;
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return false;
    }
    mdnsStackUp_ = true;
    return true;
}

bool mdnsInit(const char* deviceName) {
    if (!ensureMdnsStack()) return false;
    esp_err_t err = mdns_hostname_set(deviceName);
    ESP_LOGI(NET_TAG, "mDNS hostname set %s: %s", deviceName, esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        return false;
    }

    // Register the wired interface explicitly: @xref{advertising-needs-the-interface-registered-by-hand|why the default does not catch it}. Guarded, since the handle exists only in a build that has the peripheral at all.
#ifndef MM_NO_ETH
    if (ethNetif_ && ethConnected()) {
        esp_err_t regErr = mdns_register_netif(ethNetif_);
        if (regErr == ESP_OK || regErr == ESP_ERR_INVALID_STATE) {
            esp_err_t actErr = mdns_netif_action(ethNetif_, MDNS_EVENT_ENABLE_IP4);
            ESP_LOGI(NET_TAG, "mDNS eth netif register:%s enable:%s",
                     regErr == ESP_OK ? "new" : "already", esp_err_to_name(actErr));
        } else {
            ESP_LOGW(NET_TAG, "mDNS eth netif register failed: %s", esp_err_to_name(regErr));
        }
    }
#endif

    // Force a fresh announcement by removing the record and adding it back: @xref{advertising-needs-the-interface-registered-by-hand|why renaming does not announce}. The remove is a no-op on a first run, so one path serves both cases.
    const bool reAdvertise = mdns_service_exists("_http", "_tcp", nullptr);
    mdns_service_remove("_http", "_tcp");
    mdns_service_remove("_wled", "_tcp");

    // `_http._tcp`: how other devices DISCOVER us by browsing the service type (the standard push-style announce, WLED/ESPHome/Hue all advertise `_http._tcp`). Fatal if it fails: discovery is the point. Instance name = deviceName, port = HTTP (80).
    esp_err_t httpErr = mdns_service_add(deviceName, "_http", "_tcp", 80, nullptr, 0);
    ESP_LOGI(NET_TAG, "mDNS _http._tcp add (%s): %s",
             reAdvertise ? "re-advertise" : "fresh", esp_err_to_name(httpErr));
    if (httpErr != ESP_OK) {
        ESP_LOGE(NET_TAG, "mDNS _http._tcp advertise failed: %s", esp_err_to_name(httpErr));
        return false;
    }
    // `mm=1` TXT so a browsing MoonLight peer tells us apart from a generic `_http._tcp` box without an HTTP probe, DevicesModule classifies us MoonLight straight from the announcement. Non-fatal (advertising still works without it).
    esp_err_t txtErr = mdns_service_txt_item_set("_http", "_tcp", "mm", "1");
    ESP_LOGI(NET_TAG, "mDNS _http._tcp TXT mm=1 set: %s", esp_err_to_name(txtErr));

    // `_wled._tcp`: the service the native WLED apps + Home Assistant browse for, how a MoonLight device appears in the WLED ecosystem without speaking WLED's UDP protocol (the HTTP server on :80 answers their /json/info probe). Non-fatal: a failure means we don't show in those apps; the rest of discovery still works.
    esp_err_t wledErr = mdns_service_add(deviceName, "_wled", "_tcp", 80, nullptr, 0);
    ESP_LOGI(NET_TAG, "mDNS _wled._tcp add: %s", esp_err_to_name(wledErr));
    // `mac=` TXT, a real WLED carries `mac=<12 hex>` on its _wled._tcp record, and the native apps key the discovered device on it (without it the record is discarded, so the device never lists). Lowercase hex, no separators, matching WLED's format.
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    char macStr[13];
    std::snprintf(macStr, sizeof(macStr), "%02x%02x%02x%02x%02x%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_err_t wledTxtErr = mdns_service_txt_item_set("_wled", "_tcp", "mac", macStr);
    ESP_LOGI(NET_TAG, "mDNS _wled._tcp TXT mac=%s set: %s", macStr, esp_err_to_name(wledTxtErr));

    // Summary reflects the ACTUAL per-step results (each logged above): _http._tcp is up (we returned early on its failure), the TXT / _wled additions are non-fatal so report ok/fail rather than claiming success unconditionally.
    ESP_LOGI(NET_TAG, "mDNS started: %s.local (_http._tcp:80 mm=1:%s, _wled._tcp:80:%s mac=%s:%s)",
             deviceName,
             txtErr == ESP_OK ? "ok" : "fail",
             wledErr == ESP_OK ? "ok" : "fail",
             macStr,
             wledTxtErr == ESP_OK ? "ok" : "fail");
    return true;
}

void mdnsStop() {
    // Stop advertising but keep the stack up, so a re-init re-advertises cheaply; freeing it entirely is the release path's job. Both services and the hostname go, matching what the init adds, or a stale record survives an interface switch and confuses the next announcement.
    if (mdnsStackUp_) {
        esp_err_t httpRm = mdns_service_remove("_http", "_tcp");
        esp_err_t wledRm = mdns_service_remove("_wled", "_tcp");
        mdns_hostname_set("");
        ESP_LOGI(NET_TAG, "mDNS stopped advertising (_http remove: %s, _wled remove: %s)",
                 esp_err_to_name(httpRm), esp_err_to_name(wledRm));
    }
}

// Full stack release (mdns_free), only at module release.
void mdnsShutdown() {
    if (mdnsStackUp_) { mdns_free(); mdnsStackUp_ = false; }
}

// Advertise-only: discovery is datagram presence, each device broadcasting and listening for a small packet on its own port. Keeping discovery off this protocol also keeps the advertisement stable, since a query for a service this device hosts destabilizes its own.

// Outbound HTTP request (plain HTTP, LAN, no TLS), see platform.h. A bounded blocking lwIP socket call; the caller (HueDriver) runs it off the render path on tick1s. Mirrors the desktop impl: build request → connect → send → read response → return status + body.
int httpRequest(const char* method, const char* host, uint16_t port, const char* path,
                const char* reqBody, uint32_t timeoutMs, char* body, size_t bodyLen) {
    if (body && bodyLen) body[0] = '\0';
    if (!method || !host || !path) return 0;

    // One shared budget for every phase rather than a fresh one each, which let the total reach three times the caller's timeout. The remainder is floored above zero, since zero means block forever, and it is tracked as elapsed time, which stays correct across the counter's rollover.
    const uint32_t start = millis();
    auto remainingMs = [&]() -> uint32_t {
        const uint32_t elapsed = millis() - start;
        return elapsed >= timeoutMs ? 1u : (timeoutMs - elapsed);
    };

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct CloseGuard { int f; ~CloseGuard() { ::close(f); } } guard{fd};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return 0;

    // Bound the CONNECT by timeoutMs: a blocking connect to an unreachable host hangs for the OS default, and this runs on the driver's tick1s (shared with the render loop), so it must not stall. Connect non-blocking, wait writable via select() up to timeoutMs, then restore blocking for the bounded send/recv (which use SO_*TIMEO below).
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int cr = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (cr != 0 && errno != EINPROGRESS) return 0;   // immediate hard failure
    if (cr != 0) {                                    // connect in progress — wait for writable
        fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
        const uint32_t cms = remainingMs();
        timeval ctv{};
        ctv.tv_sec = static_cast<time_t>(cms / 1000);
        // decltype the field (not suseconds_t) so the same code compiles on Winsock's timeval too, where tv_usec is `long` and suseconds_t doesn't exist, see platform_desktop.cpp.
        ctv.tv_usec = static_cast<decltype(ctv.tv_usec)>((cms % 1000) * 1000);
        if (::select(fd + 1, nullptr, &wf, nullptr, &ctv) <= 0) return 0;   // timeout / error
        int soerr = 0; socklen_t len = sizeof(soerr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr != 0) return 0;                      // connect failed
    }
    fcntl(fd, F_SETFL, flags);                         // back to blocking

    // Bound the request send + response recv with SO_RCVTIMEO/SO_SNDTIMEO, using the time LEFT on the shared deadline (not a fresh timeoutMs) so connect + send + recv together stay within the caller's budget.
    const uint32_t sms = remainingMs();
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(sms / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((sms % 1000) * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[1024];
    const size_t blen = reqBody ? std::strlen(reqBody) : 0;
    int n = blen
        ? std::snprintf(req, sizeof(req),
              "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
              "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
              method, path, host, blen, reqBody)
        : std::snprintf(req, sizeof(req),
              "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
              method, path, host);
    if (n <= 0 || n >= static_cast<int>(sizeof(req))) return 0;
    // Send the whole request, a blocking send can return short under backpressure, so loop until all n bytes are out (retry on a positive partial, fail only on 0 / error).
    for (int off = 0; off < n;) {
        int w = ::send(fd, req + off, n - off, 0);
        if (w > 0) off += w;
        else return 0;
    }

    // Read the response. When the caller wants the body, read into THEIR buffer (so they size it, a Hue /lights body runs several KB) and shift the body to the front. When they don't (body==null, e.g. a fire-and-forget PUT), read into a small local scratch far enough to get the status line, the request still executes.
    char scratch[256];
    char* buf = body ? body : scratch;
    const size_t cap = body ? bodyLen : sizeof(scratch);
    if (cap < 16) return 0;
    int total = 0;
    while (total < static_cast<int>(cap - 1)) {
        int r = ::recv(fd, buf + total, cap - 1 - total, 0);
        if (r > 0) total += r;
        else break;   // closed or timeout
    }
    buf[total] = '\0';
    if (total < 12 || std::strncmp(buf, "HTTP/1.", 7) != 0) { if (body) body[0] = '\0'; return 0; }
    int status = std::atoi(buf + 9);   // "HTTP/1.1 NNN ..."
    if (body) {
        char* b = std::strstr(body, "\r\n\r\n");
        if (b) std::memmove(body, b + 4, std::strlen(b + 4) + 1);   // drop headers, keep the body
        else body[0] = '\0';
    }
    return status;
}

// UdpSocket

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open() {
    if (fd_ >= 0) return true;
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    // Allow sends to a broadcast address (e.g. 255.255.255.255 for an Art-Net / E1.31 spray to every device on the LAN). Without SO_BROADCAST the stack rejects such a send; it has no effect on unicast/multicast sends.
    const int on = 1;
    setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    return true;
}

bool UdpSocket::connect(const char* ip, uint16_t port) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return false;
    return ::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool UdpSocket::sendTo(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    return ::send(fd_, data, len, 0) >= 0;
}

bool UdpSocket::bind(uint16_t port) {
    if (fd_ < 0) return false;
    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    // Non-blocking so the render loop's drain never stalls waiting for a packet.
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    return true;
}

int UdpSocket::recvFrom(uint8_t* buf, size_t maxLen, uint8_t srcIp[4]) {
    if (fd_ < 0) return -1;
    sockaddr_in src{};
    socklen_t srcLen = sizeof(src);
    auto n = ::recvfrom(fd_, buf, maxLen, 0,
                        reinterpret_cast<sockaddr*>(&src), &srcLen);
    // 0-byte datagrams and EWOULDBLOCK both mean "nothing usable pending".
    if (n <= 0) return -1;
    if (srcIp) std::memcpy(srcIp, &src.sin_addr.s_addr, 4);   // network order = octets
    return static_cast<int>(n);
}

// Join an IPv4 multicast group so the bound socket receives datagrams sent to it. WLED audio sync multicasts to 239.0.0.1; without this membership the datagrams never reach the socket. INADDR_ANY as the interface lets lwip pick the default route's netif.
bool UdpSocket::joinMulticast(const char* group) {
    if (fd_ < 0 || !group) return false;
    ip_mreq mreq{};
    if (inet_pton(AF_INET, group, &mreq.imr_multiaddr) != 1) return false;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    return setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0;
}

bool UdpSocket::sendToAddr(const uint8_t ip[4], uint16_t port,
                           const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::memcpy(&addr.sin_addr.s_addr, ip, 4);
    return ::sendto(fd_, data, len, 0,
                    reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) >= 0;
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        lwip_close(fd_);
        fd_ = -1;
    }
}

// TcpConnection

TcpConnection::~TcpConnection() {
    close();
}

int TcpConnection::read(uint8_t* buf, size_t maxLen) {
    if (fd_ < 0) return -1;
    auto n = lwip_read(fd_, buf, maxLen);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
    return 0;
}

// getpeername rather than a field captured at accept: a copy taken earlier outlives a reconnect on the same slot.
bool TcpConnection::peerIPv4(uint8_t out[4]) const {
    if (fd_ < 0 || !out) return false;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
    if (addr.sin_family != AF_INET) return false;
    const uint32_t ip = ntohl(addr.sin_addr.s_addr);
    out[0] = static_cast<uint8_t>(ip >> 24);
    out[1] = static_cast<uint8_t>(ip >> 16);
    out[2] = static_cast<uint8_t>(ip >> 8);
    out[3] = static_cast<uint8_t>(ip);
    return true;
}

bool TcpConnection::write(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    // Send every byte, retrying on a full buffer, since a response must arrive complete and a healthy interface drains in microseconds.
    // This runs on the render thread, though, and a stalled peer would make an unbounded retry hang it until the watchdog reboots the device.
    // Two bounds, therefore: the stall bound, which progress resets, lets a slow but steady transfer finish where a total-only bound truncated large assets mid-body.
    // The total bound keeps a peer trickling one byte per window from holding the thread indefinitely, which would be a remotely triggerable reboot.
    constexpr uint32_t kWriteStallMs = 2000;
    constexpr uint32_t kWriteTotalMs = 8000;
    const uint32_t start = millis();
    uint32_t lastProgress = start;
    size_t sent = 0;
    while (sent < len) {
        auto n = lwip_write(fd_, data + sent, len - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            lastProgress = millis();
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const uint32_t now = millis();
            if (now - lastProgress >= kWriteStallMs || now - start >= kWriteTotalMs)
                return false;   // stalled or crawling peer: close it, never hang the render loop
            vTaskDelay(pdMS_TO_TICKS(1)); // wait for send buffer space
        } else {
            return false; // real error
        }
    }
    return true;
}

int TcpConnection::writeSome(const uint8_t* data, size_t len) {
    if (fd_ < 0) return -1;
    if (len == 0) return 0;
    ssize_t n = lwip_write(fd_, data, len);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  // buffer full — try later
    return -1;                                              // real socket error
}


bool TcpConnection::connectStart(const char* host, uint16_t port) {
    if (!host || !host[0]) return false;
    close();

    // One bounded DNS lookup up front (lwip_getaddrinfo is synchronous, the one unavoidable block); the CONNECT itself then proceeds non-blocking and is polled across ticks.
    char portStr[6];
    std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (lwip_getaddrinfo(host, portStr, &hints, &res) != 0 || !res) return false;
    struct AiGuard { struct addrinfo* p; ~AiGuard() { if (p) lwip_freeaddrinfo(p); } } aiGuard{res};

    int fd = lwip_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) return false;
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int cr = lwip_connect(fd, res->ai_addr, res->ai_addrlen);
    const bool inProgress = (cr != 0 && errno == EINPROGRESS);
    if (cr != 0 && !inProgress) { lwip_close(fd); return false; }   // immediate hard failure
    fd_ = fd;   // in flight — connectPoll() resolves which
    return true;
}

TcpConnection::ConnectResult TcpConnection::connectPoll() {
    if (fd_ < 0) return ConnectResult::Failed;
    fd_set wf; FD_ZERO(&wf); FD_SET(fd_, &wf);
    struct timeval zero = {};   // 0s / 0us — never blocks
    const int r = lwip_select(fd_ + 1, nullptr, &wf, nullptr, &zero);
    if (r == 0) return ConnectResult::Pending;
    if (r < 0)  { close(); return ConnectResult::Failed; }
    int soerr = 0; socklen_t len = sizeof(soerr);
    lwip_getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &len);
    if (soerr != 0) { close(); return ConnectResult::Failed; }
    return ConnectResult::Connected;
}

void TcpConnection::close() {
    if (fd_ >= 0) {
        lwip_close(fd_);
        fd_ = -1;
    }
}

// TcpServer

TcpServer::~TcpServer() {
    close();
}

bool TcpServer::open(uint16_t port) {
    if (fd_ >= 0) return true;
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    int opt = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        lwip_close(fd_);
        fd_ = -1;
        return false;
    }

    // The backlog is sized for a browser's page-load burst, which opens the document, several assets and the socket upgrade at once. With a smaller one the excess connections are dropped and the browser must retry, which is the load-it-twice symptom.
    if (listen(fd_, 8) < 0) {
        lwip_close(fd_);
        fd_ = -1;
        return false;
    }

    // Set non-blocking
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    return true;
}

TcpConnection TcpServer::accept() {
    if (fd_ < 0) return TcpConnection();
    int clientFd = ::accept(fd_, nullptr, nullptr);
    if (clientFd < 0) return TcpConnection();

    int flags = fcntl(clientFd, F_GETFL, 0);
    fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);

    return TcpConnection(clientFd);
}

void TcpServer::close() {
    if (fd_ >= 0) {
        lwip_close(fd_);
        fd_ = -1;
    }
}

// irRead (IR receive) lives in platform_esp32_ir.cpp, an RMT NEC decoder.

} // namespace mm::platform
