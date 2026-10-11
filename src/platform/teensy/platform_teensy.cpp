/// @defgroup platform_teensy The Teensy 4.1 platform layer
/// @{
/// The platform seam on PJRC's Teensy 4 core: one core, no RTOS, settings in LittleFS on program flash, Ethernet through the on-board PHY or a W5500.

#include "platform/platform.h"

#include <Arduino.h>
#include <CrashReport.h>
#include <LittleFS.h>
#include <QNEthernet.h>
#include <qnethernet/QNDNSClient.h>
#include <lwip/opt.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <string>
#include <vector>

extern "C" {
extern unsigned long _heap_start;   // the linker script's OCRAM heap, which sbrk grows through
extern unsigned long _heap_end;
extern char* __brkval;
extern unsigned long _flashimagelen;

// The desktop's live UI folder is tried through stdio first; with no stdio filesystem here, the open fails and the embedded UI is served.
int _open(const char*, int, ...) { errno = ENOSYS; return -1; }
}

namespace mm::platform {


// --- Time ---------------------------------------------------------------------------------------

uint32_t millis() MM_NONBLOCKING { return ::millis(); }
uint32_t micros() MM_NONBLOCKING { return ::micros(); }
uintptr_t currentThreadId() MM_NONBLOCKING { return 1; }   // one thread, and the id is never zero
uint32_t cycleCount() { return ARM_DWT_CYCCNT; }
uint8_t currentCore() { return 0; }
void yield() { ::yield(); }
void pauseLoop() { ::yield(); }   // nothing else needs the core
void delayMs(uint32_t ms) { ::delay(ms); }
void delayUs(uint32_t us) { ::delayMicroseconds(us); }

// --- Heap: malloc on OCRAM, with the size kept before each block so the counters stay exact -----

namespace {
constexpr size_t kAllocHeader = 8;   // malloc's alignment on the Cortex-M7
size_t g_allocatedBytes = 0;
size_t g_allocatedPeak = 0;
uint32_t g_allocCount = 0;

size_t heapTop() { return reinterpret_cast<size_t>(&_heap_end) - reinterpret_cast<size_t>(__brkval); }
}  // namespace

void* alloc(size_t bytes) {
    void* raw = std::malloc(bytes + kAllocHeader);
    if (!raw) return nullptr;
    *static_cast<size_t*>(raw) = bytes;
    g_allocatedBytes += bytes;
    if (g_allocatedBytes > g_allocatedPeak) g_allocatedPeak = g_allocatedBytes;
    g_allocCount++;
    return static_cast<uint8_t*>(raw) + kAllocHeader;
}

void free(void* ptr) {
    if (!ptr) return;
    void* raw = static_cast<uint8_t*>(ptr) - kAllocHeader;
    g_allocatedBytes -= *static_cast<size_t*>(raw);
    g_allocCount--;
    std::free(raw);
}

void* allocInternal(size_t bytes) { return alloc(bytes); }   // one RAM, no PSRAM to keep out of

size_t allocatedBytes() { return g_allocatedBytes; }
size_t allocatedPeak() { return g_allocatedPeak; }
uint32_t allocatedCount() { return g_allocCount; }

size_t freeHeap() { return heapTop() + mallinfo().fordblks; }
size_t freeInternalHeap() { return freeHeap(); }
// malloc keeps freed memory at the top as its "top chunk" (keepcost) rather than returning it, and that chunk borders the never-used space.
size_t maxAllocBlock() { return heapTop() + mallinfo().keepcost; }
size_t maxInternalAllocBlock() { return maxAllocBlock(); }
size_t totalHeap() { return reinterpret_cast<size_t>(&_heap_end) - reinterpret_cast<size_t>(&_heap_start); }
size_t totalInternalHeap() { return totalHeap(); }

namespace {
// PJRC marks the OCRAM heap execute-never and the highest matching MPU region decides, so the last one re-maps it executable when MoonLive first asks.
void allowExecInOcram() {
    static bool done = false;
    if (done) return;
    done = true;
    const uint32_t last = ((SCB_MPU_TYPE >> 8) & 0xFF) - 1;   // DREGION, 16 regions on this core
    asm volatile("dsb" ::: "memory");
    SCB_MPU_RBAR = 0x20200000 | SCB_MPU_RBAR_REGION(last) | SCB_MPU_RBAR_VALID;
    // Normal write-back write-allocate memory and read-write, as PJRC's own region over it, with no XN bit.
    SCB_MPU_RASR = SCB_MPU_RASR_TEX(1) | SCB_MPU_RASR_C | SCB_MPU_RASR_B | SCB_MPU_RASR_AP(3)
                 | SCB_MPU_RASR_SIZE(18) | SCB_MPU_RASR_ENABLE;
    asm volatile("dsb\n isb" ::: "memory");
}
}  // namespace

// MoonLive's code runs from the heap; the 32-byte alignment keeps a block on cache lines of its own.
void* allocExec(size_t bytes) {
    if (bytes == 0) return nullptr;
    allowExecInOcram();
    return aligned_alloc(32, (bytes + 31) & ~size_t{31});
}
void freeExec(void* ptr, size_t) { std::free(ptr); }
void writeExec(void* dst, const void* src, size_t len) {
    if (!dst || !src || !len) return;
    std::memcpy(dst, src, len);
    arm_dcache_flush(dst, len);   // to memory, which instruction fetch reads
    SCB_CACHE_ICIALLU = 0;        // and no stale line from the block's previous code
    asm volatile("dsb\n isb" ::: "memory");
}
size_t maxExecAllocBlock() { return maxAllocBlock(); }

// --- Tasks: none without an RTOS, so callers keep their work on the one loop -------------------

size_t taskSnapshot(TaskInfo*, size_t) { return 0; }
void currentTaskOnCore(int, char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
const char* renderTaskName() { return ""; }
bool spawnPinnedTask(WorkerTask&, const char*, WorkerFn, void*, size_t, uint8_t, int) { return false; }
void notifyTask(WorkerTask&) {}
bool waitNotify(WorkerTask&, uint32_t) { return false; }
void stopPinnedTask(WorkerTask&) {}
// --- Watchdog: WDOG1 on the one loop, which feeds it every tick --------------------------------

namespace {
constexpr uint32_t kWdtTimeoutS = 12;            // as the ESP32's task watchdog
constexpr uint32_t kWdtResetMark = 0x0BAD0D06;   // in SRC_GPR5 beside the core's fault mark, so the next boot reads WDT
bool g_wdtOn = false;
void markBootStableIfDue();   // with the boot record below

// WDOG1 counts and flags its timeout, but its reset does not reach the core on this board, so the warning half a second ahead resets in software.
void wdtWarning() {
    SRC_GPR5 = kWdtResetMark;
    SCB_AIRCR = 0x05FA0004;   // SYSRESETREQ, as reboot() does
    for (;;) {}
}
}  // namespace

// The one loop is the one task, so the first subscription starts WDOG1 for good: its enable bit stays set until the next reset.
void taskWdtSubscribe() {
    if (g_wdtOn) return;
    CCM_CCGR3 |= CCM_CCGR3_WDOG1(CCM_CCGR_ON);
    WDOG1_WMCR = 0;   // the power-down counter, which would reset the chip 16 s after any reset
    // Highest priority, so a handler that hangs below it is caught too; only a hang with interrupts off is not.
    attachInterruptVector(IRQ_WDOG1, wdtWarning);
    NVIC_SET_PRIORITY(IRQ_WDOG1, 0);
    NVIC_ENABLE_IRQ(IRQ_WDOG1);
    WDOG1_WICR = (1u << 15) | 1;   // WIE, one half-second step before the timeout
    // Half-second steps, so the warning lands at the timeout the ESP32 uses.
    WDOG1_WCR = WDOG_WCR_WT(kWdtTimeoutS * 2) | WDOG_WCR_WDE | WDOG_WCR_SRS | WDOG_WCR_WDA;
    g_wdtOn = true;
    taskWdtReset();
}
void taskWdtUnsubscribe() {}   // WDOG1 cannot stop once started, and the loop never exits
// The service sequence the WDOG1 block defines.
void taskWdtReset() {
    if (!g_wdtOn) return;
    WDOG1_WSR = 0x5555;
    WDOG1_WSR = 0xAAAA;
    markBootStableIfDue();   // the loop's heartbeat is where a boot proves it stays up
}

// --- GPIO and ADC -------------------------------------------------------------------------------

GpioCapability gpioCapability(uint8_t gpio) {
    GpioCapability c;
    c.validGpio = gpio < CORE_NUM_DIGITAL;
    c.outputCapable = c.validGpio;
    return c;
}

namespace {
bool ethOwnsPin(uint8_t gpio);   // with the Ethernet below
}

const char* gpioRefusal(uint8_t gpio) {
    if (!gpioCapability(gpio).validGpio) return "does not exist on this board";
    if (ethOwnsPin(gpio)) return "carries the W5500's SPI bus";
    return nullptr;
}

GpioLiveState gpioLiveState(uint8_t gpio) {
    GpioLiveState s;
    if (gpio >= CORE_NUM_DIGITAL) return s;
    s.valid = true;
    s.level = digitalReadFast(gpio);
    s.output = (*portModeRegister(gpio) & digitalPinToBitMask(gpio)) != 0;
    s.input = true;
    return s;
}

bool gpioInputBegin(uint8_t gpio, GpioPull pull) {
    if (gpio >= CORE_NUM_DIGITAL) return false;
    ::pinMode(gpio, pull == GpioPull::Up ? INPUT_PULLUP : pull == GpioPull::Down ? INPUT_PULLDOWN : INPUT);
    return true;
}

bool gpioRead(uint8_t gpio) { return gpio < CORE_NUM_DIGITAL && ::digitalRead(gpio); }

bool gpioWrite(uint8_t gpio, bool high) {
    if (gpio >= CORE_NUM_DIGITAL) return false;
    ::pinMode(gpio, OUTPUT);
    ::digitalWrite(gpio, high ? HIGH : LOW);
    return true;
}

namespace {
// A0 to A13 are pins 14 to 27, A14 to A17 are pins 38 to 41.
bool isAnalogPin(uint8_t gpio) { return (gpio >= 14 && gpio <= 27) || (gpio >= 38 && gpio <= 41); }
constexpr uint16_t kAdcMax = 1023;   // the core's default 10-bit resolution
}  // namespace

bool adcRead(uint8_t gpio, uint16_t& raw) {
    if (!isAnalogPin(gpio)) return false;
    raw = static_cast<uint16_t>(::analogRead(gpio));
    return true;
}

uint16_t adcMaxCount() { return kAdcMax; }

bool adcReadMv(uint8_t gpio, uint16_t& mv) {
    uint16_t raw = 0;
    if (!adcRead(gpio, raw)) return false;
    mv = static_cast<uint16_t>(uint32_t{raw} * 3300 / kAdcMax);
    return true;
}

// --- Identity -----------------------------------------------------------------------------------

void getMacAddress(uint8_t mac[6]) {
    // PJRC programs a MAC into the fuses of every Teensy 4.1.
    const uint32_t hi = HW_OCOTP_MAC1, lo = HW_OCOTP_MAC0;
    mac[0] = hi >> 8; mac[1] = hi;
    mac[2] = lo >> 24; mac[3] = lo >> 16; mac[4] = lo >> 8; mac[5] = lo;
}

const char* macString() {
    static char buf[18] = {};
    if (!buf[0]) {
        uint8_t m[6];
        getMacAddress(m);
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    return buf;
}

const char* chipModel() { return "i.MX RT1062"; }
const char* hostPlatform() { return ""; }
const char* psramType() { return ""; }
const char* hostIp() { return ""; }

const char* sdkVersion() {
    static char buf[48] = {};
    if (!buf[0]) std::snprintf(buf, sizeof(buf), "Teensyduino %d.%02d, gcc %s", TEENSYDUINO / 100, TEENSYDUINO % 100, __VERSION__);
    return buf;
}

const char* cpuInfo() {
    static char buf[24] = {};
    std::snprintf(buf, sizeof(buf), "%lu MHz, 1 core", static_cast<unsigned long>(F_CPU_ACTUAL / 1000000));
    return buf;
}

// The reset status register keeps its bits across a warm reset, so it is read once and cleared for the next boot.
namespace {
// The cause the reset status register latched, the most specific first.
const char* reasonFor(uint32_t srsr, bool faulted, bool watchdog) {
    if ((srsr & SRC_SRSR_LOCKUP_SYSRESETREQ) && watchdog) return "WDT";
    if ((srsr & SRC_SRSR_LOCKUP_SYSRESETREQ) && faulted) return "PANIC";
    if (srsr & (SRC_SRSR_WDOG_RST_B | SRC_SRSR_WDOG3_RST_B)) return "WDT";
    if (srsr & SRC_SRSR_LOCKUP_SYSRESETREQ) return "SW";
    if (srsr & SRC_SRSR_IPP_RESET_B) return "POWERON";
    return "UNKNOWN";
}
}  // namespace

const char* resetReason() {
    static const char* reason = nullptr;
    if (reason) return reason;
    const uint32_t srsr = SRC_SRSR;
    const bool faulted = SRC_GPR5 == 0x0BAD00F1;   // the core's mark for its reboot after a fault
    const bool watchdog = SRC_GPR5 == kWdtResetMark;
    SRC_SRSR = srsr;
    SRC_GPR5 = 0;
    reason = reasonFor(srsr, faulted, watchdog);
    return reason;
}

// SNVS general-purpose register 0 keeps its value across every reset short of a power loss, which is what a crash loop needs: the failed boots in a row.
namespace {
constexpr uint32_t kBootTag = 0x4D4C0000;   // "ML", so a value left by another firmware reads as none
bool g_bootStable = false;

void storeBoot(uint8_t restarts) { SNVS_LPGPR0 = kBootTag | restarts; }

void markBootStableIfDue() {
    if (g_bootStable || ::millis() < kBootStableMs) return;
    g_bootStable = true;
    storeBoot(0);
}
}  // namespace

// No switch-on gesture without WiFi, and a power loss clears the register, so this counts the restarts nobody asked for: a watchdog or a fault.
const BootRecord& bootRecord() {
    static BootRecord record;
    static bool counted = false;
    if (counted) return record;
    counted = true;
    record.booted = true;
    const uint32_t reg = SNVS_LPGPR0;
    const bool ours = (reg & 0xFFFF0000) == kBootTag;
    const uint8_t restarts = ours ? static_cast<uint8_t>(reg & 0xFF) : 0;
    const char* why = resetReason();
    const bool failed = std::strcmp(why, "WDT") == 0 || std::strcmp(why, "PANIC") == 0;
    record.abnormalRestarts = failed ? static_cast<uint8_t>(restarts < 250 ? restarts + 1 : restarts) : 0;
    storeBoot(record.abnormalRestarts);
    return record;
}

void setLogLevel(LogLevel) {}   // stdout goes straight to USB serial

[[noreturn]] void reboot() {
    SCB_AIRCR = 0x05FA0004;   // SYSRESETREQ with the write key
    for (;;) {}
}

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

// Create every folder along the first `len` characters of `path`.
void mkdirs(const char* path, size_t len) {
    char buf[160];
    if (len >= sizeof(buf)) return;
    std::memcpy(buf, path, len);
    buf[len] = 0;
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] != '/' && buf[i] != 0) continue;
        const char saved = buf[i];
        buf[i] = 0;
        if (!g_fs.exists(buf)) g_fs.mkdir(buf);
        buf[i] = saved;
    }
}

void makeParent(const char* path) {
    const char* slash = std::strrchr(path, '/');
    if (slash && slash != path) mkdirs(path, static_cast<size_t>(slash - path));
}

// Write `tmp`, then rename it over `path`, which LittleFS does atomically.
template <class Fill>
bool writeAtomic(const char* path, Fill fill) {
    if (!g_mounted || !path) return false;
    makeParent(path);
    char tmp[168];
    if (std::strlen(path) + 5 > sizeof(tmp)) return false;
    std::snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    g_fs.remove(tmp);
    File f = g_fs.open(tmp, FILE_WRITE_BEGIN);
    if (!f) return false;
    const bool ok = fill(f);
    f.close();
    if (!ok || !g_fs.rename(tmp, path)) { g_fs.remove(tmp); return false; }
    return true;
}
}  // namespace

const char* fsRootPath() { return "/"; }

bool fsMount() {
    if (!g_mounted) g_mounted = g_fs.begin(kFsBytes);
    return g_mounted;
}

bool fsMkdir(const char* path) {
    if (!g_mounted || !path) return false;
    mkdirs(path, std::strlen(path));
    return g_fs.exists(path);
}

bool fsRemove(const char* path) {
    if (!g_mounted || !path) return false;
    return g_fs.remove(path) || g_fs.rmdir(path);
}

int fsRead(const char* path, char* buf, size_t maxLen) {
    if (!g_mounted || !buf || maxLen == 0) return -1;
    File f = g_fs.open(path, FILE_READ);
    if (!f || f.isDirectory()) return -1;
    const size_t n = f.read(buf, maxLen - 1);
    f.close();
    buf[n] = 0;
    return static_cast<int>(n);
}

long fsSize(const char* path) {
    if (!g_mounted) return -1;
    File f = g_fs.open(path, FILE_READ);
    if (!f || f.isDirectory()) return -1;
    const long size = static_cast<long>(f.size());
    f.close();
    return size;
}

int fsReadAt(const char* path, long offset, char* buf, size_t len) {
    if (!g_mounted || !buf) return -1;
    File f = g_fs.open(path, FILE_READ);
    if (!f || f.isDirectory()) return -1;
    if (!f.seek(static_cast<uint64_t>(offset))) { f.close(); return -1; }
    const size_t n = f.read(buf, len);
    f.close();
    return static_cast<int>(n);
}

bool fsWriteAtomic(const char* path, const char* data, size_t len) {
    return writeAtomic(path, [&](File& f) { return f.write(data, len) == len; });
}

bool fsWriteStream(const char* path, FsWriteSrc src, void* user) {
    if (!src) return false;
    return writeAtomic(path, [&](File& f) {
        char chunk[512];
        bool abort = false;
        for (;;) {
            const size_t got = src(chunk, sizeof(chunk), user, &abort);
            if (abort) return false;
            if (got == 0) return true;
            if (f.write(chunk, got) != got) return false;
        }
    });
}

void fsList(const char* dir, FsListCb cb, void* user) {
    if (!g_mounted || !cb) return;
    File d = g_fs.open(dir);
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

// --- Ethernet: the on-board PHY or a W5500 through QNEthernet, whose lwIP runs from yield() ----------

namespace {
using qindesign::network::Ethernet;
EthPinConfig g_ethConfig = ethConfigDefault;
bool g_ethStarted = false;

void copyIp(const IPAddress& from, uint8_t out[4]) {
    if (out) for (int i = 0; i < 4; i++) out[i] = from[i];
}

// Reconfiguring an SPI pin takes it from the bus, and the network with it until a reboot.
bool ethOwnsPin(uint8_t gpio) {
    if (!g_ethStarted) return false;
    const EthPinConfig& c = g_ethConfig;
    return gpio == c.spiMiso || gpio == c.spiMosi || gpio == c.spiSck || gpio == c.spiCs;
}
}  // namespace

void setEthConfig(const EthPinConfig& cfg) { g_ethConfig = cfg; }

// Starts DHCP and returns at once; false when the build's interface does not answer.
bool ethInit() {
    g_ethStarted = Ethernet.begin();
    std::printf("Ethernet: %s %s\n", hasEthW5500 ? "W5500" : "PHY",
                g_ethStarted ? "found, DHCP started" : "not found, check the wiring");
    return g_ethStarted;
}

void ethStop() {
    if (g_ethStarted) Ethernet.end();
    g_ethStarted = false;
}

bool ethLinkUp() MM_NONBLOCKING { return g_ethStarted && Ethernet.linkState(); }
bool ethConnected() MM_NONBLOCKING { return ethLinkUp() && Ethernet.localIP() != INADDR_NONE; }
void ethGetIPv4(uint8_t out[4]) MM_NONBLOCKING { copyIp(g_ethStarted ? Ethernet.localIP() : INADDR_NONE, out); }
uint16_t ethLinkSpeedMbps() MM_NONBLOCKING { return ethLinkUp() ? static_cast<uint16_t>(Ethernet.linkInfo().speed) : 0; }
bool ethRawL2Claimed() MM_NONBLOCKING { return false; }
bool networkReady() { return ethConnected(); }

void netSetStaticIPv4(NetIface, const uint8_t ip[4], const uint8_t gw[4], const uint8_t mask[4], const uint8_t dns[4]) {
    Ethernet.setDHCPEnabled(false);
    Ethernet.setLocalIP(IPAddress(ip));
    Ethernet.setSubnetMask(IPAddress(mask));
    Ethernet.setGatewayIP(IPAddress(gw));
    if (dns) Ethernet.setDNSServerIP(IPAddress(dns));
}

void netSetDhcp(NetIface) { Ethernet.setDHCPEnabled(true); }

void netGetIPv4(NetIface, uint8_t ip[4], uint8_t gw[4], uint8_t mask[4], uint8_t dns[4]) {
    copyIp(Ethernet.localIP(), ip);
    copyIp(Ethernet.gatewayIP(), gw);
    copyIp(Ethernet.subnetMask(), mask);
    copyIp(Ethernet.dnsServerIP(), dns);
}

void setHostname(const char* name) { if (name) Ethernet.setHostname(name); }

// --- mDNS: QNEthernet's responder, with the records the ESP32 announces ---------------------------

namespace {
using qindesign::network::DNSClient;
using qindesign::network::MDNS;

constexpr uint16_t kHttpPort = 80;
constexpr uint32_t kResolveTimeoutMs = 2000;   // bounded, as on the ESP32
struct ExtraService { char type[16]; char proto[8]; uint16_t port; };
ExtraService g_extraServices[4] = {};

// `mm=1` tells a browsing MoonLight peer this is one of its own without an HTTP probe.
std::vector<std::string> httpTxt() { return {"mm=1"}; }

// The WLED apps key a device on `mac=`, lowercase hex without separators, and drop a record without it.
std::vector<std::string> wledTxt() {
    uint8_t m[6];
    getMacAddress(m);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "mac=%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return {buf};
}

bool sameService(const ExtraService& e, const char* type, const char* proto) {
    return e.port && std::strcmp(e.type, type) == 0 && std::strcmp(e.proto, proto) == 0;
}

void announceExtra(const ExtraService& e) {
    MDNS.removeService(e.type, e.proto, e.port);   // a fresh announcement when the port moved
    MDNS.addService(e.type, e.proto, e.port);
}
}  // namespace

// Renames the responder when it already runs, so one path serves the first start and a rename.
bool mdnsInit(const char* deviceName) {
    if (!deviceName || !deviceName[0] || !MDNS.begin(deviceName)) return false;
    MDNS.removeService(deviceName, "_http", "_tcp", kHttpPort);
    MDNS.removeService(deviceName, "_wled", "_tcp", kHttpPort);
    const bool ok = MDNS.addService(deviceName, "_http", "_tcp", kHttpPort, httpTxt);
    MDNS.addService(deviceName, "_wled", "_tcp", kHttpPort, wledTxt);
    for (const auto& e : g_extraServices) if (e.port) announceExtra(e);
    std::printf("mDNS: %s.local%s\n", deviceName, ok ? "" : " (the _http._tcp record failed)");
    return ok;
}

void mdnsAdvertise(const char* type, const char* proto, uint16_t port) {
    ExtraService* slot = nullptr;
    for (auto& e : g_extraServices) {
        if (sameService(e, type, proto)) { slot = &e; break; }
        if (!e.port && !slot) slot = &e;
    }
    if (!slot) return;   // the table is full: the service stays unannounced
    if (slot->port && MDNS.hostname()[0]) MDNS.removeService(slot->type, slot->proto, slot->port);
    std::snprintf(slot->type, sizeof(slot->type), "%s", type);
    std::snprintf(slot->proto, sizeof(slot->proto), "%s", proto);
    slot->port = port;
    if (MDNS.hostname()[0]) announceExtra(*slot);
}

void mdnsWithdraw(const char* type, const char* proto) {
    for (auto& e : g_extraServices) {
        if (!sameService(e, type, proto)) continue;
        if (MDNS.hostname()[0]) MDNS.removeService(e.type, e.proto, e.port);
        e = ExtraService{};
    }
}

uint16_t mdnsAdvertisedPort(const char* type, const char* proto) {
    for (const auto& e : g_extraServices)
        if (sameService(e, type, proto)) return e.port;
    return 0;
}

// The responder restarts cheaply here, so stopping and shutting down are one call.
void mdnsStop() { MDNS.end(); }
void mdnsShutdown() { MDNS.end(); }

// Unicast DNS through lwIP; lwIP's responder answers for this device but does not query, so a .local name fails.
bool resolveHost(const char* name, uint8_t ip[4]) {
    IPAddress addr;
    if (!name || !ethConnected() || !DNSClient::getHostByName(name, addr, kResolveTimeoutMs)) return false;
    copyIp(addr, ip);
    return true;
}

// --- WiFi: none on a Teensy ---------------------------------------------------------------------

void wifiStaGetIPv4(uint8_t out[4]) MM_NONBLOCKING { std::memset(out, 0, 4); }
int wifiStaRssi() { return 0; }
bool wifiScanStart() { return false; }
int wifiScanResults(WifiNetwork*, int) { return 0; }
void wifiStaBssid(uint8_t out[6]) { std::memset(out, 0, 6); }
int wifiStaChannel() { return 0; }
bool wifiApConnected() { return false; }
int httpRequest(const char*, const char*, uint16_t, const char*, const char*, uint32_t, char* body, size_t bodyLen) {
    if (body && bodyLen) body[0] = '\0';
    return 0;
}
bool httpsAvailable() MM_NONBLOCKING { return false; }
bool httpsPost(const char*, const char*, uint32_t) { return false; }
bool hlsSegment(const char*, const uint8_t**, size_t*) { return false; }
void hlsSegmentRelease() {}

// --- Sockets: each class's descriptor is a slot in a fixed pool of QNEthernet objects -------------

namespace {
using qindesign::network::EthernetClient;
using qindesign::network::EthernetServer;
using qindesign::network::EthernetUDP;

// As many as lwIP holds control blocks for; DHCP, mDNS and the DNS client each keep a UDP one of their own.
constexpr int kTcpSlots = MEMP_NUM_TCP_PCB;
constexpr int kServerSlots = 4;
constexpr int kUdpSlots = MEMP_NUM_UDP_PCB - 3;
// Datagrams queued per socket between two drains, so a burst of Art-Net or DDP universes survives a frame.
constexpr size_t kUdpQueue = 16;

EthernetClient g_tcp[kTcpSlots];
bool g_tcpUsed[kTcpSlots] = {};
EthernetServer g_servers[kServerSlots];
bool g_serverUsed[kServerSlots] = {};
EthernetUDP g_udp[kUdpSlots];
bool g_udpUsed[kUdpSlots] = {};

template <size_t N>
int claimSlot(bool (&used)[N]) {
    for (size_t i = 0; i < N; i++)
        if (!used[i]) { used[i] = true; return static_cast<int>(i); }
    return -1;
}

// Said at most once a second, so a burst of refusals shows without flooding the log.
void degrade(const char* what) {
    static uint32_t last = 0;
    if (::millis() - last < 1000) return;
    last = ::millis();
    std::printf("  DEGRADE  %s\n", what);
}

int claimTcp(const EthernetClient& c) {
    const int slot = claimSlot(g_tcpUsed);
    if (slot < 0) {   // no slot: refuse the connection rather than leak it
        EthernetClient(c).abort();
        degrade("tcp: connection refused, all slots busy");
        return -1;
    }
    g_tcp[slot] = c;
    return slot;
}
}  // namespace

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open() {
    if (fd_ >= 0) return true;
    fd_ = claimSlot(g_udpUsed);
    if (fd_ >= 0) g_udp[fd_].setReceiveQueueCapacity(kUdpQueue);
    return fd_ >= 0;
}

// One interface, so `localIp` selects nothing here.
bool UdpSocket::bind(uint16_t port, const uint8_t[4]) {
    if (fd_ < 0) return false;
    if (g_udp[fd_].beginWithReuse(port)) return true;
    degrade("udp: bind refused");
    return false;
}

int UdpSocket::recvFrom(uint8_t* buf, size_t maxLen, uint8_t srcIp[4], uint16_t* srcPort) {
    if (fd_ < 0) return -1;
    EthernetUDP& udp = g_udp[fd_];
    const int size = udp.parsePacket();
    // 0-byte datagrams and an empty queue both mean nothing usable is pending.
    if (size <= 0) { keepMembership(); return -1; }
    heard();
    const int n = udp.read(buf, maxLen < static_cast<size_t>(size) ? maxLen : static_cast<size_t>(size));
    copyIp(udp.remoteIP(), srcIp);
    if (srcPort) *srcPort = udp.remotePort();
    return n;
}

bool UdpSocket::membership(const char* group, bool join) {
    IPAddress ip;
    if (fd_ < 0 || !group || !ip.fromString(group)) return false;
    return join ? Ethernet.joinGroup(ip) : Ethernet.leaveGroup(ip);
}

bool UdpSocket::sendToAddr(const uint8_t ip[4], uint16_t port, const uint8_t* data, size_t len) {
    return fd_ >= 0 && g_udp[fd_].send(IPAddress(ip), port, data, len);
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        if (joined_) membership(group_, false);
        g_udp[fd_].stop();
        g_udpUsed[fd_] = false;
        fd_ = -1;
    }
    group_[0] = 0;
    joined_ = false;
    joinCount_ = 0;
}

TcpConnection::~TcpConnection() { close(); }

// Bytes read, 0 once the peer closed, -1 while nothing is pending.
int TcpConnection::read(uint8_t* buf, size_t maxLen) {
    if (fd_ < 0) return -1;
    EthernetClient& c = g_tcp[fd_];
    if (c.available() > 0) return c.read(buf, maxLen);
    return c.connected() ? -1 : 0;
}

bool TcpConnection::peerIPv4(uint8_t out[4]) const {
    if (fd_ < 0 || !out) return false;
    copyIp(g_tcp[fd_].remoteIP(), out);
    return true;
}

bool TcpConnection::localIPv4(uint8_t out[4]) const {
    if (fd_ < 0 || !out) return false;
    copyIp(g_tcp[fd_].localIP(), out);
    return true;
}

// Bytes queued, 0 while the send buffer is full, -1 once the connection is gone.
int TcpConnection::writeSome(const uint8_t* data, size_t len) {
    if (fd_ < 0) return -1;
    EthernetClient& c = g_tcp[fd_];
    if (!c.connected()) return -1;
    if (len == 0) return 0;
    const int room = c.availableForWrite();
    if (room <= 0) return 0;
    const size_t n = c.write(data, len < static_cast<size_t>(room) ? len : static_cast<size_t>(room));
    c.flush();   // hand the segment to lwIP now rather than on the next loop
    return static_cast<int>(n);
}

// Every byte, bounded as on the other platforms: a stall without progress or a crawling peer gives up rather than hold the loop.
bool TcpConnection::write(const uint8_t* data, size_t len) {
    constexpr uint32_t kWriteStallMs = 2000;
    constexpr uint32_t kWriteTotalMs = 8000;
    const uint32_t start = ::millis();
    uint32_t lastProgress = start;
    size_t sent = 0;
    while (sent < len) {
        const int n = writeSome(data + sent, len - sent);
        if (n < 0) return false;
        const uint32_t now = ::millis();
        if (n > 0) { sent += static_cast<size_t>(n); lastProgress = now; continue; }
        if (now - lastProgress >= kWriteStallMs || now - start >= kWriteTotalMs) {
            degrade("tcp: write gave up on a stalled peer");
            return false;
        }
        ::yield();   // runs lwIP, which drains the send buffer as the peer acknowledges
    }
    return true;
}

bool TcpConnection::connectStart(const char* host, uint16_t port) {
    if (!host || !host[0]) return false;
    close();
    EthernetClient c;
    if (!c.connectNoWait(host, port)) return false;
    fd_ = claimTcp(c);
    return fd_ >= 0;
}

TcpConnection::ConnectResult TcpConnection::connectPoll() {
    if (fd_ < 0) return ConnectResult::Failed;
    EthernetClient& c = g_tcp[fd_];
    if (c.connected()) return ConnectResult::Connected;
    if (c.connecting()) return ConnectResult::Pending;
    close();
    return ConnectResult::Failed;
}

void TcpConnection::close() {
    if (fd_ < 0) return;
    g_tcp[fd_].close();
    g_tcp[fd_] = EthernetClient{};
    g_tcpUsed[fd_] = false;
    fd_ = -1;
}

TcpServer::~TcpServer() { close(); }

bool TcpServer::open(uint16_t port) {
    if (fd_ >= 0) return true;
    const int slot = claimSlot(g_serverUsed);
    if (slot < 0) return false;
    if (!g_servers[slot].beginWithReuse(port)) { g_serverUsed[slot] = false; return false; }
    fd_ = slot;
    return true;
}

TcpConnection TcpServer::accept() {
    if (fd_ < 0) return TcpConnection();
    EthernetClient c = g_servers[fd_].accept();
    if (!c) return TcpConnection();
    // Nagle off, as on the ESP32: a header write then a body write, and the second would wait on a delayed ACK.
    c.setNoDelay(true);
    return TcpConnection(claimTcp(c));
}

void TcpServer::close() {
    if (fd_ < 0) return;
    g_servers[fd_].end();
    g_serverUsed[fd_] = false;
    fd_ = -1;
}

// --- Updates: over USB until FlasherX -----------------------------------------------------------

bool http_fetch_to_ota(const char*, char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut, uint32_t* bytesTotalOut, const char*) {
    if (statusBuf && statusBufLen) std::snprintf(statusBuf, statusBufLen, "updates go over USB");
    if (bytesReadOut) *bytesReadOut = 0;
    if (bytesTotalOut) *bytesTotalOut = 0;
    return false;
}
bool otaHasMoonBase() { return false; }
bool otaBootMoonBase() { return false; }
bool otaRunningMoonBase() { return false; }
bool otaMoonBaseVersion(char*, size_t) { return false; }
bool otaMoonBaseBuild(char*, size_t) { return false; }
bool otaMoonBaseSize(uint32_t*, uint32_t*) { return false; }
void moonbaseClearStagedUrl() {}

// --- Peripherals that arrive in phase 4 ---------------------------------------------------------

bool i2cBusOpen(uint16_t, uint16_t) { return false; }
void i2cBusClose() {}
uint32_t i2cBusGeneration() MM_NONBLOCKING { return 0; }
size_t i2cScan(uint8_t*, size_t) { return kI2cBusUnavailable; }
bool irRead(uint16_t, uint32_t&) { return false; }
void irStop() {}
bool irChannelReady(uint16_t) { return false; }
bool audioMicSharedBusFree(MicMode) { return true; }   // nothing shares a bus with a microphone here
bool usbMidiBegin() { return false; }
void usbMidiEnd() {}
bool usbMidiConnected() { return false; }
size_t usbMidiRead(uint8_t (*)[4], size_t) { return 0; }
bool usbMidiWrite(const uint8_t (*)[4], size_t) { return false; }

}  // namespace mm::platform

/// @}
