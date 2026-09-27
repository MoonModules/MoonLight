#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include "platform_config.h"  // hasOta / hasPsram / …: flags this header's contract refers to

/// @defgroup platform The platform layer
/// @{
/// The one interface every module reaches hardware through, so the same source drives an ESP32, a Teensy, a Raspberry Pi and a desktop.
///
/// Core and the light domain call these names and never a vendor SDK.
/// A module that needs something this interface does not offer gets a new function here rather than a target check at the call site.
///
/// @moreinfo
///
/// ## Ethernet transmit can wedge
///
/// The driver's internal link state can diverge from both the PHY and our own event-driven flag.
/// Observed on an S31 under sustained transmit, with the link genuinely lost, no disconnect event delivered, and nothing recovering short of a reboot.
/// A stop and start re-runs link negotiation, which is the only supported way back, and it blocks for up to four seconds while autonegotiation polls the PHY to its timeout.
/// The housekeeping tick calls it only where every frame is being refused anyway, so a stalled render loop for one tick costs nothing a user can see.
///
/// ## DMA cannot read PSRAM at the expander's clock
///
/// Measured fine at 2.67 MHz and never completing at 26.67 MHz, which is why a frame above the expander's cap is never materialized.
/// The streaming ring loops a small pool of internal buffers instead, and the CPU encodes the next slice into each as it drains.
///
/// ## Internal RAM for what an interrupt reads
///
/// A PSRAM-resident encode source measured about 595 microseconds per slice refill against a 151 microsecond drain budget.
/// So a buffer an interrupt reads per byte comes from `allocInternal` rather than the PSRAM-first `alloc`.

// The render path must not allocate or block, which clang checks transitively through overrides.
#if defined(__clang__) && defined(__has_cpp_attribute) && __has_cpp_attribute(clang::nonblocking)
  // noexcept is part of the contract, since unwinding allocates and clang warns without it.
  #define MM_NONBLOCKING noexcept [[clang::nonblocking]]
#else
  #define MM_NONBLOCKING noexcept
#endif

namespace mm::platform {

/// Milliseconds since boot, the clock every animation is written against.
uint32_t millis() MM_NONBLOCKING;

/// An opaque identity for the calling task, stable for its lifetime and never zero.
uintptr_t currentThreadId() MM_NONBLOCKING;
/// Microseconds since boot, for measuring a tick rather than driving one.
uint32_t micros() MM_NONBLOCKING;

// A device honours it too, so a scenario run on real hardware can freeze time.
/// Drive `millis()` from a test; zero restores the real clock, which a test must do in release.
void setTestNowMs(uint32_t ms);

// Hogging the port instead is not portable: Linux permits the overlapping bind under SO_REUSEADDR, and macOS lets a non-root process bind port 80.
/// Make the next bind fail, so a test reaches that path; false restores.
void setTestBindFails(bool fail);

/// Allocate, preferring PSRAM where the target has it.
void* alloc(size_t bytes);
/// Release what `alloc` returned.
void free(void* ptr);

/// Allocate internal RAM only, for buffers a hot ISR reads per byte; free with `free`.
void* allocInternal(size_t bytes);

/// Bytes taken on purpose through alloc and allocInternal.
size_t allocatedBytes();
/// The high-water mark of allocatedBytes.
size_t allocatedPeak();
/// Live block count, separating one grown buffer from a per-frame allocation.
uint32_t allocatedCount();

/// True when the pointer resolves to external (PSRAM) memory; always false on desktop.
bool ptrIsPsram(const void* p);

/// CPU cycle counter, safe in an ISR; wraps at 2^32, so callers difference two reads.
uint32_t cycleCount();

/// Allocate memory the CPU can fetch from, for MoonLive's emitted code; nullptr when exhausted.
void* allocExec(size_t bytes);
/// Release an allocExec block; the size is for backends that need it, and ESP32 ignores it.
void  freeExec(void* ptr, size_t bytes);

/// Copy emitted code into an allocExec block and make it executable; `len` may be any length.
void  writeExec(void* dst, const void* src, size_t len);

/// Give the scheduler a turn.
void yield();

/// Which core the caller runs on; a driver seeing core 1 knows the render/encode split is engaged.
uint8_t currentCore();
/// Upper bound on cores running driver code at once, sizing per-CPU scratch; single-core targets leave slice 1 unused.
inline constexpr uint8_t kMaxCores = 2;
/// Pace the render loop for one pass, so a desktop loop stops spinning a whole core.
void pauseLoop();
/// Blocking sleep; only use outside the hot path.
void delayMs(uint32_t ms);
/// Blocking busy-wait for a sub-ms protocol gap such as the WS2812 latch, up to a few hundred µs.
void delayUs(uint32_t us);
/// Total free heap, internal plus PSRAM where the target has it.
size_t freeHeap();
/// Free internal RAM, which is what the stack, HTTP and WiFi reserve is checked against.
size_t freeInternalHeap();
/// Largest contiguous block of any memory type, PSRAM included.
size_t maxAllocBlock();
/// Largest contiguous block of internal RAM, the scarce one, so this is the memory-pressure KPI.
size_t maxInternalAllocBlock();

/// Largest contiguous block of executable memory, which is what bounds a MoonLive script's size.
size_t maxExecAllocBlock();

// --- RTOS task introspection: an allocation-free snapshot, on tick1s rather than per frame ----
/// What a task is doing right now.
enum class TaskState : uint8_t { Running, Ready, Blocked, Suspended, Deleted, Invalid, Unknown };
/// Stands in for cpuPermille where run-time stats are compiled out, so the caller can omit the column.
constexpr uint32_t kTaskCpuUnmeasured = 0xFFFFFFFFu;
/// One RTOS task, as TasksModule reports it.
struct TaskInfo {
    char      name[16] = {};                     ///< the task's own name
    TaskState state = TaskState::Unknown;        ///< running, blocked, suspended
    int8_t    core = -1;                         ///< 0, 1, or -1 for no affinity
    uint8_t   priority = 0;                      ///< its RTOS priority
    uint32_t  stackFreeBytes = 0;                ///< high-water mark: the least free stack seen
    uint32_t  cpuPermille = kTaskCpuUnmeasured;  ///< 0..1000, or unmeasured when stats are off
};
/// Fill `out` with up to `maxTasks` rows, answering how many; 0 where there is no RTOS.
size_t taskSnapshot(TaskInfo* out, size_t maxTasks);
/// The task running on a core, or empty on a single-core target.
void currentTaskOnCore(int core, char* out, size_t cap);
/// The task the render loop runs in, empty where no distinct one exists.
const char* renderTaskName();
/// Inject a canned snapshot from a test; `tasks` must outlive the use.
void setTestTaskSnapshot(const TaskInfo* tasks, size_t count, const char* renderTask);

// --- Pinned worker task and wake: FreeRTOS's lock-free pairing for a single producer ----------
/// An opaque handle to a spawned task, keeping FreeRTOS types out of the header.
struct WorkerTask { void* impl = nullptr; };
/// The body a pinned task runs; it owns its loop and returns once stopPinnedTask signals it.
using WorkerFn = void(*)(void* user);
/// Spawn `fn(user)` pinned to `core`, where -1 means no affinity; false when the task cannot be created.
bool spawnPinnedTask(WorkerTask& t, const char* name, WorkerFn fn, void* user,
                     size_t stackBytes, uint8_t priority, int core);
/// Wake the task blocked in waitNotify, safe from any task on any core.
void notifyTask(WorkerTask& t);
/// Block inside the spawned fn until notifyTask fires or `timeoutMs` elapses; false on timeout.
bool waitNotify(WorkerTask& t, uint32_t timeoutMs);
/// Signal stop and wake, then block until the worker fn has returned and the task is torn down.
void stopPinnedTask(WorkerTask& t);
/// Subscribe THIS task to the watchdog, so a wedge reboots instead of hanging silently.
void taskWdtSubscribe();
/// Unsubscribe THIS task before it exits, so a torn-down worker leaves no dangling entry.
void taskWdtUnsubscribe();
/// Feed THIS task's subscription, once a tick.
void taskWdtReset();

// --- GPIO capability introspection (PinsModule) ---------------------------------------------
/// What one GPIO is, so the pin ownership map can flag a claim landing on an unsafe pin.
///
/// @moreinfo The SDK answers the first three fields; `strap` and `reserved` come from a per-chip table, being datasheet knowledge.
/// Desktop reports everything valid and nothing reserved, a host build having no real pins to protect.
struct GpioCapability {
    bool validGpio = true;      ///< a real, usable GPIO on this chip
    bool outputCapable = true;  ///< has an output driver (classic ESP32 34-39 are input-only)
    bool rtc = false;           ///< an RTC pin, usable for deep-sleep wake and RTC I/O
    bool strap = false;         ///< a boot-strapping pin: driving it at reset can change boot mode
    bool reserved = false;      ///< wired to flash, PSRAM or native USB: routing I/O here corrupts the device
};
/// Look up one pin's static capability; pure lookup, no state.
GpioCapability gpioCapability(uint8_t gpio);
/// Why a driver must refuse this pin, or null when it may use it.
const char* gpioRefusal(uint8_t gpio);
/// Make gpioCapability answer `cap` for one gpio, so pin severity is testable where every real pin is safe.
void setTestGpioCapability(uint8_t gpio, GpioCapability cap);
/// Drop every injected capability.
void clearTestGpioCapability();

/// What one GPIO is doing now, the pin map's second axis beside gpioCapability's static view.
///
/// @moreinfo The pad reads on any pin, even one a peripheral drives.
/// So a driver's output must toggle while it renders, and a mic clock while the mic runs.
struct GpioLiveState {
    bool valid = false;    ///< pin is readable; false out of range, and on desktop, which omits the columns
    bool level = false;    ///< current pad level, true being HIGH
    bool output = false;   ///< the pad's output driver is enabled right now
    bool input = false;    ///< the pad's input buffer is enabled right now; a pin can be both
    uint8_t driveCap = 0;  ///< output drive strength 0..3 = WEAK / MEDIUM / STRONG / STRONGEST
};
/// Sample one pin's live state, on tick1s rather than per frame.
GpioLiveState gpioLiveState(uint8_t gpio);
/// Inject a live state for one gpio, so the level and drive columns are host-testable.
void setTestGpioLiveState(uint8_t gpio, GpioLiveState state);
/// Drop every injected live state.
void clearTestGpioLiveState();

/// Cap what maxAllocBlock reports, 0 being no cap, so a test reaches the paged-destinations fallback without a fragmented heap.
void setTestMaxAllocBlock(size_t bytes);
/// Total heap capacity, internal plus PSRAM.
size_t totalHeap();
/// Total internal heap capacity.
size_t totalInternalHeap();

/// Heap to keep free for stack, HTTP, WiFi and overhead; any allocator checks against this before committing.
constexpr size_t HEAP_RESERVE = 32768;

/// The device's MAC address.
void getMacAddress(uint8_t mac[6]);
/// The MAC as canonical "XX:XX:XX:XX:XX:XX", in a static buffer a ReadOnly control can bind straight to.
const char* macString();
/// The chip this runs on, as a static string.
const char* chipModel();

/// The hardware this installation runs on, empty on a device, where tooling injects `deviceModel` instead.
const char* hostPlatform();
/// The SDK this was built against, as a static string.
const char* sdkVersion();

/// CPU frequency and core count, read from the running hardware so a downclock is visible in the UI.
const char* cpuInfo();

/// PSRAM interface type, "quad" or "octal", and empty where this build has no PSRAM.
const char* psramType();

/// WiFi co-processor status for a board whose radio lives on a separate chip, empty on a native radio.
const char* coprocessorWifi();

/// This host's LAN IPv4 as a dotted string, or empty; on ESP32 the device IP belongs to NetworkModule.
const char* hostIp();

/// Why the device last reset, which the UI reads to flag a crashed prior boot.
const char* resetReason();

/// Serial log verbosity, low to high, ordered as syslog and ESP-IDF order it.
enum class LogLevel : uint8_t { None = 0, Error, Warn, Info, Debug, Verbose };
/// Apply a verbosity to the logger and to the KPI-line gate.
void setLogLevel(LogLevel level);

/// Firmware image bytes.
size_t firmwareSize();
/// App partition size, which is the firmware capacity.
size_t firmwarePartition();
/// Total flash chip capacity.
size_t flashChipSize();
/// Filesystem bytes used.
size_t filesystemUsed();
/// Filesystem bytes total.
size_t filesystemTotal();

// LittleFS on ESP32, std::filesystem on desktop. Paths start with '/', which desktop strips to reach its root.
/// Redirect the desktop root, so a test gets an isolated directory without chdir; call it before fsMount.
void fsSetRoot(const char* path);
const char* fsRootPath();                                    // the resolved root, for diagnostics
bool fsMount();                                              // idempotent; safe to call multiple times
void fsUnmount();
bool fsMkdir(const char* path);                              // mkdir -p; no error if exists
bool fsExists(const char* path);
bool fsRemove(const char* path);                             // file or empty dir
/// Read a whole file; bytes read, or -1 on error, null-terminated on success.
int  fsRead(const char* path, char* buf, size_t maxLen);
/// A file's size in bytes, or -1 when it is missing or not a file.
long fsSize(const char* path);
/// Read up to `len` bytes at `offset`; bytes read, 0 at the end, -1 on error.
int  fsReadAt(const char* path, long offset, char* buf, size_t len);
/// Write a whole file atomically, through a temporary and a rename.
bool fsWriteAtomic(const char* path, const char* data, size_t len);
/// Fill up to `cap` bytes and answer the count; 0 ends the stream.
using FsWriteSrc = size_t(*)(char* buf, size_t cap, void* user, bool* abort);
/// Write a file atomically from `src`, pulling it in chunks; false on abort or a write failure.
bool fsWriteStream(const char* path, FsWriteSrc src, void* user);
/// Called once per child of a listed directory; a directory reports size 0.
using FsListCb = void(*)(const char* name, bool isDir, uint32_t sizeBytes, void* user);
/// List one level of `dir`, calling `cb` per child.
void fsList(const char* dir, FsListCb cb, void* user);

// Network: ESP32 only, stubs on desktop.
/// Override the per-chip default pin and PHY map with a board's own, before ethInit.
void setEthConfig(const EthPinConfig& cfg);
/// Bring up the Ethernet driver.
bool ethInit();
/// Tear down a running driver so ethInit can re-init with new config; safe when nothing runs.
void ethStop();
/// PHY link detected, meaning a cable is plugged; a fast check.
bool ethLinkUp() MM_NONBLOCKING;
/// An IP is assigned, meaning DHCP completed.
bool ethConnected() MM_NONBLOCKING;
/// Current IP as raw octets, all-zero meaning none yet.
void ethGetIPv4(uint8_t out[4]) MM_NONBLOCKING;

/// Put one complete Ethernet frame on the wire, below IP, which is how panel receiver cards are addressed.
bool ethSendRaw(const uint8_t* frame, size_t len) MM_NONBLOCKING;

/// End of one wall frame: hand what ethSendRaw batched to the wire and start a new batch.
void ethFlushRaw() MM_NONBLOCKING;

/// Claim the Ethernet interface for direct L2 use, or release it; reference-counted.
void ethClaimRawL2(bool claim);

/// True while any driver holds a raw-L2 claim, which separates a broken link from a deliberately leaseless one.
bool ethRawL2Claimed() MM_NONBLOCKING;

/// Send failures since boot, split by cause, the two being different faults with different fixes.
void ethSendFailCounts(uint32_t& linkDown, uint32_t& ringFull) MM_NONBLOCKING;

/// Consecutive send failures since the last success, which tells back-pressure from a wedged path.
uint32_t ethSendFailStreak() MM_NONBLOCKING;

/// Restart the driver after transmit has wedged; blocks for up to ~4 seconds, so it is not MM_NONBLOCKING.
bool ethRestartTx();

/// Negotiated link speed in Mbit/s, 0 when no link or no driver.
uint16_t ethLinkSpeedMbps() MM_NONBLOCKING;

/// Bind raw sending to a host interface by name; null or empty returns to capture mode.
bool ethBindRawInterface(const char* ifName);

/// Enumerate the host NICs raw sending could bind, with entry 0 always "none (capture only)".
size_t rawInterfaces(const char* const** optionsOut);
/// The bind name behind row `i`, which differs from its label on Windows; null for row 0.
const char* rawInterfaceName(size_t i);
#ifndef ESP_PLATFORM
/// Replace the enumeration with a fixed list, so the control is pinnable without real NICs.
void setTestRawInterfaces(const char* const* names, size_t count);
#endif

// --- NDI output, gated by `hasNdi`: the user's proprietary runtime, resolved on demand ---------

/// Is the NDI runtime present and loaded? Loads on first call.
bool ndiAvailable();

/// Create a named NDI source, replacing any sender already open.
bool ndiSenderOpen(const char* name);

/// Destroy the sender; safe with none open, so a driver's release need not track state.
void ndiSenderClose();

/// Send one frame of tightly-packed RGB, w*h*3 bytes, declaring `fps` to the receiver.
bool ndiSendFrame(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t fps);

// A desktop seam recording what the driver sent, pinning geometry, packing and pacing without a runtime.
#ifndef ESP_PLATFORM
/// Force the runtime's apparent presence, overriding what is installed.
enum class NdiTestMode : uint8_t { Off, ForceAvailable, ForceMissing };
/// Apply a test mode.
void setTestNdiMode(NdiTestMode mode);
/// Force availability on or off, in the older two-state form.
inline void setTestNdiAvailable(bool available) {
    setTestNdiMode(available ? NdiTestMode::ForceAvailable : NdiTestMode::Off);
}
/// How many frames were recorded.
size_t ndiTestFrameCount();
/// The recorded frame's width.
uint16_t ndiTestFrameWidth(size_t i);
/// The recorded frame's height.
uint16_t ndiTestFrameHeight(size_t i);
/// The rate declared with the recorded frame.
uint8_t  ndiTestFrameFps(size_t i);
/// The recorded frame's tight-RGB bytes, as the driver handed them over.
const uint8_t* ndiTestFrameData(size_t i);
/// The name the sender was opened with, pinning the blank-means-device-name rule.
const char* ndiTestSenderName();
/// Drop every recorded frame.
void ndiTestClearFrames();
#endif

// --- HLS output, gated by `hasHls`: the seam carries numbers rather than an encoder argv -------

/// What to encode; geometry and rate are the frame contract.
struct EncoderConfig {
    uint16_t    width;         ///< frame width
    uint16_t    height;        ///< frame height
    uint8_t     fps;           ///< also the GOP, since a cut needs a keyframe and a long GOP lengthens every segment
    uint16_t    bitrateKbit;   ///< target bitrate
    const char* encoderName;   ///< a desktop ffmpeg encoder; ignored where the platform has only one
    const char* outDir;        ///< absolute directory for the playlist and segments
};

/// Start encoding to `cfg`, replacing any encoder already running.
bool encoderStart(const EncoderConfig& cfg);

/// Hand one whole frame to the encoder, which never blocks the caller.
int encoderWrite(const uint8_t* data, size_t len);

/// Is the spawned encoder still alive? Reaps the child when it exited.
bool encoderRunning();

/// Stop the encoder, letting it finalize the playlist first; safe with none running.
void encoderStop();

/// The one claim slot, since there is one encoder. Shared by every caller through the accessors below.
inline const void*& encoderOwnerSlot() {
    static const void* owner = nullptr;
    return owner;
}

/// Which module holds the encoder, null where none does. For a status line, never for a decision.
inline const void* encoderOwner() { return encoderOwnerSlot(); }

/// Claim the one encoder for `owner`, false where another module already holds it.
inline bool encoderClaim(const void* owner) {
    if (!owner) return false;
    const void* held = encoderOwnerSlot();
    if (held && held != owner) return false;   // another module is streaming: refused, not stolen
    encoderOwnerSlot() = owner;
    return true;
}

/// Release the claim where `owner` holds it, and stop the encoder; a non-holder is ignored.
inline void encoderRelease(const void* owner) {
    if (!owner || encoderOwnerSlot() != owner) return;   // never release another module's claim
    encoderOwnerSlot() = nullptr;
    encoderStop();
}

/// Serve an HLS file the platform holds in RAM; false where this platform writes segments to disk.
bool hlsSegment(const char* name, const uint8_t** data, size_t* len);
/// Release what hlsSegment handed out, required after every call that answered true.
void hlsSegmentRelease();

// --- The encoder is claimed, never shared ------------------------------------------------------
// One instance per target, so a second encoderStart would silently reconfigure the first driver's stream, and the claim below makes that visible. Unsynchronised: both callers claim from prepare(), on the render thread.

// --- RTSP output, gated by `hasRtsp`: the encoded frame itself, before any muxing ---------------

/// One encoded frame as the encoder produced it, valid until the encoder writes the next.
struct EncodedFrame {
    const uint8_t* nal;       ///< the frame's NAL units, Annex B, start codes included
    size_t         len;       ///< bytes at `nal`
    uint32_t       pts90;     ///< presentation time in the RTP clock's 90 kHz units
    bool           keyframe;  ///< an IDR, which a joining client decodes from
};

/// The frame the encoder produced since the last take, valid until the next `encoderWrite`.
bool rtspTakeFrame(EncodedFrame* out);

/// Release what `rtspTakeFrame` handed out, required after every call that answered true: the frame stays valid until then, and the encoder reuses the buffer after.
void rtspReleaseFrame();

/// The encoder's parameter sets where it emits them separately, false where each keyframe carries its own.
bool rtspParameterSets(EncodedFrame* sps, EncodedFrame* pps);

#ifndef ESP_PLATFORM
/// Record instead of encoding, or force the not-installed path, since CI has no ffmpeg.
enum class EncoderTestMode : uint8_t { Off, Record, ForceMissing };
/// Apply a test mode.
void setTestEncoderMode(EncoderTestMode mode);
/// Force encoderWrite's next result in Record mode, 0 being would-block and -1 dead.
void setTestEncoderWriteResult(int result);
/// How many frames were recorded.
size_t encoderTestFrameCount();
/// One recorded frame's size.
size_t encoderTestFrameSize(size_t i);
/// One recorded frame's bytes.
const uint8_t* encoderTestFrameData(size_t i);
/// The argv encoderStart was called with, joined by spaces, for pinning the arg builder.
const char* encoderTestArgs();
/// Drop every recorded frame.
void encoderTestClearFrames();
#endif

// The frames ethSendRaw captured, active whenever no raw interface is bound.
#ifndef ESP_PLATFORM
/// The panel format's largest frame; a longer one records truncated, its true length still reported.
constexpr size_t kEthTestFrameMax = 1512;
/// How many frames were captured.
size_t ethTestFrameCount();
/// One captured frame's true length, even where the recording was truncated.
size_t ethTestFrameLength(size_t i);
/// One captured frame's bytes.
const uint8_t* ethTestFrameData(size_t i);
/// Drop every captured frame.
void ethTestClearFrames();
/// Make the next sends fail, so a test can exercise the link-down path.
void setTestEthSendFails(bool fail);
/// Override the reported link speed, so a test can exercise the too-slow-link status.
void setTestEthLinkSpeed(uint16_t mbps);
/// Make ethRestartTx fail, so a test can exercise the recovery-failed path.
void setTestEthRestartFails(bool fail);
/// How many times ethRestartTx has run, which pins the once-per-wedge bound.
uint32_t ethRestartCountForTest();
#endif

/// Join `ssid` as a station.
bool wifiStaInit(const char* ssid, const char* password);
/// Whether the station is associated and holds an IP.
bool wifiStaConnected() MM_NONBLOCKING;
/// The station's IP as raw octets, on ethGetIPv4's contract.
void wifiStaGetIPv4(uint8_t out[4]);
/// Tear the station down.
void wifiStaStop();

// Refreshed off the render task every few seconds, since on a co-processor radio the query is a blocking RPC, and every reading below shares that contract.
/// Station RSSI in dBm, a negative number, and 0 when the station is not associated.
int wifiStaRssi();

/// The associated access point's BSSID, zeroed when the station is not associated.
void wifiStaBssid(uint8_t out[6]);
/// The WiFi channel in use, 0 when the station is not associated.
int  wifiStaChannel();

/// A client interface whose addressing NetworkModule sets.
enum class NetIface : uint8_t { Sta, Eth };
/// Pin a static IPv4 config onto a client interface, stopping its DHCP client; idempotent.
void netSetStaticIPv4(NetIface iface, const uint8_t ip[4], const uint8_t gw[4],
                      const uint8_t mask[4], const uint8_t dns[4]);
/// Return a client interface to DHCP, re-leasing live without a reboot.
void netSetDhcp(NetIface iface);
/// Make wifiStaInit succeed, so a host test can drive the station cascade a radio-less desktop never enters.
void setTestWifiStaAvailable(bool available);
/// How many static-addressing applies reached the platform for one interface.
uint32_t testNetStaticApplyCount(NetIface iface);

/// Bring up the SoftAP under `apName` at `ip`.
bool wifiApInit(const char* apName, const char* ip);
/// Whether the SoftAP is up.
bool wifiApConnected();
/// Tear the SoftAP down.
void wifiApStop();
/// Stations associated with our SoftAP, 0 when it is down or empty.
uint32_t wifiApClientCount();

/// True when a socket is safe to open: the stack is initialized and an interface holds an IP.
bool networkReady();

/// Current WiFi transmit power in dBm, 0 when WiFi is not initialized. Cached like wifiStaRssi.
int wifiTxPower();

/// Cap the WiFi transmit power in quarter-dBm units, 8 to 84; 0 keeps the stack default.
bool wifiSetTxPower(int8_t quarterDbm);

/// Advertise this device over mDNS as `_http._tcp` and `_wled._tcp`, which is how the WLED app and Home Assistant find it.
bool mdnsInit(const char* deviceName);
/// Stop advertising but keep the stack up, so a later mdnsInit needs no full re-init.
void mdnsStop();
/// Free the mDNS stack, at release.
void mdnsShutdown();

/// Store the DHCP hostname the next bring-up advertises; call it before ethInit or wifiStaInit.
void setHostname(const char* name);

/// Fetch a firmware image from `url` and flash it to the next OTA partition, returning at once; `fallbackUrl` is tried where the first cannot be opened.
bool http_fetch_to_ota(const char* url,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut,
                       const char* fallbackUrl = nullptr);

/// Flash a firmware image streamed from `src`, on fsWriteStream's producer shape; true once the boot pointer flipped.
bool otaWriteStream(FsWriteSrc src, void* user, size_t contentLen,
                    char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut);

// MoonBase is the second boot image: a board cannot rewrite the partition it executes from.
/// Does this partition table carry MoonBase?
bool otaHasMoonBase();
/// Point the bootloader at MoonBase; false when there is none.
bool otaBootMoonBase();
/// Are we executing from MoonBase right now?
bool otaRunningMoonBase();
/// Which MoonBase is installed, read from its app descriptor without booting it.
bool otaMoonBaseVersion(char* out, size_t len);
/// When that MoonBase was built, so the UI shows its identity as it shows the app's.
bool otaMoonBaseBuild(char* out, size_t len);
/// How much of its slot MoonBase fills.
bool otaMoonBaseSize(uint32_t* used, uint32_t* total);
/// Install a new MoonBase, which only the running app can do; false on desktop.
bool otaWriteMoonBase(FsWriteSrc src, void* user, size_t contentLen,
                      char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut);
/// The same install from a URL, which the device fetches itself; runs on its own task and returns at once.
bool otaFetchMoonBaseUrl(const char* url, char* statusBuf, size_t statusBufLen,
                         uint32_t* bytesReadOut, uint32_t* bytesTotalOut);
/// Stage an install URL for MoonBase to pick up on its next boot, at most 255 bytes.
bool moonbaseStageInstallUrl(const char* url);
/// Erase a staged URL that was never consumed, which a power cut mid-stage leaves armed.
void moonbaseClearStagedUrl();

/// One cleartext HTTP request to a LAN host, answering the status code or 0 on failure.
int httpRequest(const char* method, const char* host, uint16_t port, const char* path,
                const char* reqBody, uint32_t timeoutMs, char* body, size_t bodyLen);

/// One HTTPS POST to a public server, answering the status or 0; `url` is a full URL, so a hostname resolves.
bool httpsPost(const char* url, const char* body, uint32_t timeoutMs);

/// Whether this build can make an outbound HTTPS request at all.
bool httpsAvailable() MM_NONBLOCKING;

// Improv provisioning over UART0, sharing the channel with logging, which writes registers directly.
/// What Improv tells the client about this device; the task copies the strings, so pass static storage.
struct ImprovDeviceInfo {
    const char* name;            ///< device hostname, such as "MM-3A7F"
    const char* chipFamily;      ///< "ESP32", "ESP32-S3" and the rest
    const char* firmwareVersion; ///< the running version
};
/// Start provisioning: credentials land in the caller's buffers and set `ready`, which its tick clears.
bool improvProvisioningInit(const ImprovDeviceInfo& info,
                            char* ssidOut, size_t ssidOutLen,
                            char* passwordOut, size_t passwordOutLen,
                            std::atomic<bool>* ready,
                            char* statusBuf, size_t statusBufLen,
                            uint8_t* txPowerOut = nullptr,
                            std::atomic<bool>* txPowerReady = nullptr,
                            char* opOut = nullptr, size_t opOutLen = 0,
                            std::atomic<bool>* opReady = nullptr);

/// One UDP socket: the wire every light protocol sends and receives on.
class UdpSocket {
public:
    /// An unopened socket.
    UdpSocket() = default;
    /// Close whatever is open.
    ~UdpSocket();

    /// Sockets are not copied; a file descriptor has one owner.
    UdpSocket(const UdpSocket&) = delete;
    /// Sockets are not copy-assigned, for the same reason.
    UdpSocket& operator=(const UdpSocket&) = delete;

    /// Open the socket, answering whether it came up.
    bool open();
    // A fixed destination lets each send skip the address parse and route lookup.
    /// Bind a fixed destination; false on a bad address.
    bool connect(const char* ip, uint16_t port);
    /// Send to the connected destination.
    bool sendTo(const uint8_t* data, size_t len);
    // Listening flips the whole socket non-blocking, sends included.
    /// Listen on a port on any interface; false when it is taken.
    bool bind(uint16_t port);
    // A datagram longer than `maxLen` is truncated; `srcIp` also answers who sent it.
    /// Receive one datagram without blocking: bytes copied, or -1 when nothing is pending.
    int recvFrom(uint8_t* buf, size_t maxLen, uint8_t srcIp[4] = nullptr);
    // For replying on a bound, unconnected socket; a connected one keeps using sendTo.
    /// Send once to an explicit address.
    bool sendToAddr(const uint8_t ip[4], uint16_t port, const uint8_t* data, size_t len);
    // Without the membership the OS never delivers those datagrams, however correct the port.
    /// Join a multicast group on a bound socket; false is retried rather than fatal.
    bool joinMulticast(const char* group);
    /// Close it, which the destructor also does.
    void close();

private:
    int fd_ = -1;
};

/// One TCP connection, non-blocking so a client never stalls the render loop.
class TcpConnection {
public:
    /// An unconnected socket.
    TcpConnection() = default;
    /// Adopt an already-open descriptor, which is what accept hands over.
    explicit TcpConnection(int fd) : fd_(fd) {}
    /// Close whatever is open.
    ~TcpConnection();

    /// Connections are not copied; a file descriptor has one owner.
    TcpConnection(const TcpConnection&) = delete;
    /// Connections are not copy-assigned, for the same reason.
    TcpConnection& operator=(const TcpConnection&) = delete;

    /// Move the descriptor, leaving the source empty.
    TcpConnection(TcpConnection&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    /// Close what this holds, then take the other's descriptor.
    TcpConnection& operator=(TcpConnection&& other) noexcept {
        if (this != &other) { close(); fd_ = other.fd_; other.fd_ = -1; }
        return *this;
    }

    /// How an in-flight connect is going.
    enum class ConnectResult : uint8_t { Pending, Connected, Failed };
    // The caller polls across ticks and enforces its own timeout, gating on networkReady().
    /// Start a connect and return at once; false is an immediate DNS or socket failure.
    bool connectStart(const char* host, uint16_t port);
    /// Check that connect without blocking; call it after connectStart.
    ConnectResult connectPoll();

    /// Whether this holds an open socket.
    bool valid() const { return fd_ >= 0; }
    /// Read without blocking: bytes copied, 0 when the peer closed, -1 when nothing is pending.
    int read(uint8_t* buf, size_t maxLen);

    /// The connected peer's IPv4 address, which a second channel back to it is addressed by.
    bool peerIPv4(uint8_t out[4]) const;
    /// Write every byte, blocking until it is sent, which an HTTP response needs.
    bool write(const uint8_t* data, size_t len);
    // The caller advances its own offset and calls again, streaming across ticks without blocking.
    /// Write what the socket accepts now: the count written, 0 when full, -1 on error.
    int writeSome(const uint8_t* data, size_t len);

    /// Close it, which the destructor also does.
    void close();

private:
    int fd_ = -1;
};

/// A listening TCP socket: what the web UI and the preview stream are served from.
class TcpServer {
public:
    /// A server that is not yet listening.
    TcpServer() = default;
    /// Stop listening.
    ~TcpServer();

    /// Servers are not copied; a listening socket has one owner.
    TcpServer(const TcpServer&) = delete;
    /// Servers are not copy-assigned, for the same reason.
    TcpServer& operator=(const TcpServer&) = delete;

    /// Listen on a port.
    bool open(uint16_t port);
    /// Take the next pending connection without blocking; an invalid one means none waited.
    TcpConnection accept();
    /// Stop listening.
    void close();

private:
    int fd_ = -1;
};

/// Restart the device: a hardware reset on ESP32, a process exit on desktop.
[[noreturn]] void reboot();

// RMT WS2812 output: the driver encodes symbols, the platform owns only the peripheral.

/// One configured RMT TX channel; the driver never inspects `impl`.
struct RmtWs2812Handle { void* impl = nullptr; };

/// Configure one RMT TX channel on `gpio`, where `invert` suits an inverting level-shifter.
bool rmtWs2812Init(RmtWs2812Handle& h, uint8_t gpio, uint32_t resolutionHz, bool invert);

/// The tick resolution granted, which the driver converts its timings against; 0 before init.
uint32_t rmtWs2812Resolution(const RmtWs2812Handle& h) MM_NONBLOCKING;

/// Transmit one frame as the channel-ordered wire bytes the strip expects.
bool rmtWs2812Transmit(RmtWs2812Handle& h, const uint8_t* wire, size_t byteCount);

/// Set the symbols a 0 and a 1 bit expand to; the timing control rewrites these between frames.
bool rmtWs2812SetBitTiming(RmtWs2812Handle& h, uint32_t sym0, uint32_t sym1);

/// Block until this channel's transmit completes; false on timeout, the frame still clocking out.
bool rmtWs2812Wait(RmtWs2812Handle& h, uint32_t timeoutMs);

/// Release the channel.
void rmtWs2812Deinit(RmtWs2812Handle& h);

/// Capture up to `maxSymbols` pulse durations on `gpio`, for the loopback self-test alone.
size_t rmtWs2812RxCapture(uint8_t gpio, uint32_t resolutionHz,
                          uint32_t* outSymbols, size_t maxSymbols, uint32_t timeoutMs);


/// What a loopback self-test observed.
///
/// @moreinfo The verdict says why it failed rather than only that it did: an empty capture is a different fault from a full one that decodes wrong.
/// Without those numbers both collapse into the same "bad bit 0 of 0" and isolate nothing.
struct RmtLoopbackResult {
    bool jumperDetected = false;  ///< the plain-GPIO continuity pre-check passed
    bool pass = false;            ///< every captured bit matched what was sent
    uint8_t sent[3] = {};         ///< the per-light test pattern transmitted
    uint8_t got[3] = {};          ///< the light holding the first mismatch, light 0 when clean
    uint32_t bitsChecked = 0;     ///< WS2812 bits verified, 24 for the short test
    uint32_t firstBadBit = 0;     ///< the first wrong bit, or bitsChecked when all pass
    uint32_t capturedSymbols = 0; ///< symbols captured, which should reach bitsChecked
    int8_t rxIdleLevel = -1;      ///< RX level after the capture window, -1 when unknown
    uint32_t txWallUs = 0;        ///< wall time of the first timed transmit
    uint32_t txExpectUs = 0;      ///< expected wire time; far below the wall time means a stalled transfer
};
/// Drive a known pattern out `txGpio` and verify it on `rxGpio`, which the user jumpers together.
RmtLoopbackResult rmtWs2812Loopback(uint8_t txGpio, uint8_t rxGpio);

/// The same test over a whole real frame, clocked back to back as the render loop does.
RmtLoopbackResult rmtWs2812LoopbackFrame(uint8_t txGpio, uint8_t rxGpio,
                                         uint16_t lights, uint8_t channels);

// i80 parallel WS2812 output: one pre-encoded frame in a DMA buffer the platform keeps internal.

/// One configured i80 bus with its one or two DMA frame buffers.
struct I80Ws2812Handle { void* impl = nullptr; };

/// Stands for a line this board does not wire.
constexpr uint16_t kBusPinUnset = 0xFFFF;
/// Create the bus on `dataPins`, plus the `wrGpio` pixel clock and `dcGpio` that WS2812 strands ignore.
bool i80Ws2812Init(I80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                   uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes,
                   bool wantSecondBuffer, uint8_t clockMultiplier = 1);
/// Why the last init failed when the backend knows more than that it did; null when it did not.
const char* i80Ws2812LastError();
/// Whether the peripheral this backend shares is free to claim right now.
bool i80Ws2812SharedBusFree();

/// The DMA frame buffer the driver encodes into; buffer 1 is null where it did not fit.
uint8_t* i80Ws2812Buffer(const I80Ws2812Handle& h, uint8_t buffer);
/// The capacity both buffers share, which is the driver's grow-only check; 0 before init.
size_t i80Ws2812BufferCapacity(const I80Ws2812Handle& h);

/// Start the autonomous transfer of one buffer and return; pair it with a wait on that same buffer.
bool i80Ws2812Transmit(I80Ws2812Handle& h, uint8_t buffer, size_t bytes);

/// Block until that buffer's transfer finishes; false on timeout, the DMA perhaps still reading it.
bool i80Ws2812Wait(I80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs);

/// How long the last completed transfer took, which is the pure wire time; 0 before the first.
uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle& h);

/// Release the bus and its buffers.
void i80Ws2812Deinit(I80Ws2812Handle& h);

/// Transmit the caller's real frame on a private bus and verify every captured bit.
RmtLoopbackResult i80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                    uint16_t wrGpio, uint16_t dcGpio, uint16_t rxGpio,
                                    const uint8_t* frame, size_t frameBytes,
                                    size_t dataBytes, uint8_t rowBits,
                                    uint8_t clockMultiplier = 1);

// MoonI80: our own DMA driver, because esp_lcd re-arms per transaction and caps a frame at one block.

/// One configured MoonI80 bus, in whole-frame or ring mode.
struct MoonI80Ws2812Handle { void* impl = nullptr; };

/// Fill one drained ring buffer: the platform owns the ring, the domain owns the encode.
using MoonI80EncodeFn = void (*)(void* user, uint8_t* dst, uint32_t firstRow, uint32_t rowCount,
                                 bool closeFrame, bool needsPrefill);

/// Bring the bus up in whole-frame mode, on i80Ws2812Init's contract.
bool moonI80Ws2812Init(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                       uint16_t wrGpio, size_t bufferBytes,
                       bool wantSecondBuffer, uint8_t clockMultiplier = 1);

/// Lights per DMA buffer, wall-verified at 256 lights across 16 strands.
constexpr uint8_t kRingRowsDefault = 7;
/// Buffers the DMA circulates, sized to the measured pool knee.
constexpr uint8_t kRingBufsDefault = 16;
/// The per-slice zero-pad ceiling in µs, which stretches the refill deadline without ending the frame.
constexpr uint8_t kRingPadMaxUs = 120;
/// The most bytes one GDMA descriptor node carries.
constexpr size_t kRingNodeMaxBytes = 4095;
/// The deepest ring pool, which keeps every slice encoded before arming even at 48 by 256.
constexpr uint8_t kRingBufsMax = 64;
/// The shallowest ring pool.
constexpr uint8_t kRingBufsMin = 2;

/// Bring the bus up in ring mode, streaming a frame too big to hold; false falls back to whole-frame.
bool moonI80Ws2812InitRing(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                           uint16_t wrGpio, size_t rowBytes, uint32_t totalRows,
                           uint32_t rowsPerBuf, uint8_t ringBufs, uint8_t padUs,
                           uint8_t clockMultiplier, MoonI80EncodeFn encode, void* user);

/// Start one frame on the ring: prime the buffers, fire the DMA, and let the refill run behind it.
bool moonI80Ws2812TransmitRing(MoonI80Ws2812Handle& h);
/// Set the '595 shift-clock prescale off the 80 MHz bus, taking effect on the next bus build.
void moonI80SetShiftClockDiv(uint8_t div);
/// Prime a sub-range of the pool, so two cores can prime disjoint ranges at once.
void moonI80Ws2812PrimeRange(MoonI80Ws2812Handle& h, uint8_t bufLo, uint8_t bufHi);
/// Arm the ring once everything is primed.
bool moonI80Ws2812ArmRing(MoonI80Ws2812Handle& h);

/// Whether this handle came up as a ring, which tells the driver which transmit to call.
bool moonI80Ws2812IsRing(const MoonI80Ws2812Handle& h);

/// Would a whole frame of `bytes` fit internal DMA RAM now, leaving the network reserve?
bool moonI80Ws2812InternalFits(size_t bytes);
/// The DMA frame buffer the driver encodes into.
uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle& h, uint8_t buffer);
/// The capacity both buffers share; 0 before init.
size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle& h);
/// Start the autonomous transfer of one whole-frame buffer.
bool moonI80Ws2812Transmit(MoonI80Ws2812Handle& h, uint8_t buffer, size_t bytes);
/// Block until that buffer's transfer finishes; false on timeout, as i80Ws2812Wait describes.
bool moonI80Ws2812Wait(MoonI80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs);
/// How long the last completed transfer took; 0 before the first.
uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle& h);

/// What the ring has been doing, for a read-only control; every field is 0 on a whole-frame handle.
///
/// @moreinfo Best-effort volatile reads without a lock, so this is a diagnostic rather than a contract.
struct MoonI80RingStats {
    bool     isRing = false;     ///< whether this handle runs a ring
    uint32_t nSlices = 0;        ///< slices a frame takes, the light count over rowsPerBuf
    uint32_t ringBufs = 0;       ///< pool size; buffers are reused once nSlices passes it
    uint32_t eofTotal = 0;       ///< lifetime end-of-frame interrupts
    uint32_t doneGiven = 0;      ///< lifetime frame completions
    uint32_t lastDrain = 0;      ///< the drain count the last interrupt saw, which should reach nSlices
    uint32_t numItems = 0;       ///< descriptor pool capacity
    uint32_t consumedItems = 0;  ///< nodes the mount loop used, which equals numItems when sized right
    uint32_t descErr = 0;        ///< descriptor errors, where anything above 0 means the chain was corrupted
    uint32_t maxEncodeUs = 0;    ///< worst refill-encode time, the producer's jitter
    uint32_t avgEncodeUs = 0;    ///< average refill-encode time, which decides whether the ring keeps up
    uint32_t maxIsrGapUs = 0;    ///< worst gap between interrupts, which is the drain deadline
    uint32_t late = 0;           ///< slices refilled after their drain began, each one stale on the wire
    uint32_t itemsPerBuf = 0;    ///< descriptor nodes a ring buffer takes, 1 since the clamp
    int32_t  termNodeDiag = -1;  ///< the mount-time terminator node, -1 on a lapping chain
    uint32_t cacheOffDefers = 0; ///< interrupts that refilled nothing because the flash cache was off
    uint32_t cacheOffMaxRun = 0; ///< worst run of those, which is how many buffers drained un-refilled
    uint32_t stallAbandons = 0;  ///< frames the wait backstop finalized after the DMA self-terminated
};
/// Read the ring's counters.
MoonI80RingStats moonI80Ws2812RingStats(const MoonI80Ws2812Handle& h);

/// Release the bus and its buffers.
void moonI80Ws2812Deinit(MoonI80Ws2812Handle& h);
/// Verify a real frame on a private bus, optionally riding the ring the render path uses.
RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                        uint16_t wrGpio, uint16_t rxGpio,
                                        const uint8_t* frame, size_t frameBytes,
                                        size_t dataBytes, uint8_t rowBits,
                                        uint8_t clockMultiplier = 1,
                                        uint32_t ringRows = 0, uint32_t ringBufs = 0,
                                        bool useRing = false);

/// Verify what the live pipeline is already clocking, building no bus and leaving it running.
RmtLoopbackResult ws2812LoopbackRide(uint16_t rxGpio, const uint8_t* sent, uint8_t sentLen,
                                     size_t dataBytes, uint8_t rowBits, uint8_t clockMultiplier);

// Parlio WS2812 output: the data GPIOs directly, any lane count, and the clock generated internally.

/// One configured Parlio TX unit with its one or two DMA frame buffers.
struct ParlioWs2812Handle { void* impl = nullptr; };

/// Create a Parlio TX unit on `dataPins` clocked at the WS2812 slot rate.
bool parlioWs2812Init(ParlioWs2812Handle& h, const uint16_t* dataPins,
                      uint8_t laneCount, uint32_t pclkHz, size_t bufferBytes,
                      bool wantSecondBuffer);

/// The DMA frame buffer the driver encodes into; buffer 1 is null where it did not fit.
uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle& h, uint8_t buffer);
/// The capacity both buffers share; 0 before init.
size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle& h);

/// The most bytes Parlio sends in one transfer: a hardware ceiling, so it needs no handle.
size_t parlioMaxTransferBytes();

/// Start the autonomous transfer of one buffer; pair it with a wait on that same buffer.
bool parlioWs2812Transmit(ParlioWs2812Handle& h, uint8_t buffer, size_t bytes);

/// Block until that buffer's transfer finishes; false on timeout, as i80Ws2812Wait describes.
bool parlioWs2812Wait(ParlioWs2812Handle& h, uint8_t buffer, uint32_t timeoutMs);

/// How long the last completed transfer took, the pure wire time; 0 before the first.
uint32_t parlioWs2812LastTransmitUs(const ParlioWs2812Handle& h);

/// Release the unit and its buffers.
void parlioWs2812Deinit(ParlioWs2812Handle& h);

/// Verify a real frame on a private Parlio unit, on i80Ws2812Loopback's contract.
RmtLoopbackResult parlioWs2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                       uint16_t rxGpio, const uint8_t* frame,
                                       size_t frameBytes, size_t dataBytes,
                                       uint8_t rowBits);

// HUB75 panel output: a panel is scanned rather than addressed, one bit plane at a time.

/// The pins one HUB75 port needs; `e` serves 1/32-scan panels alone.
///
/// @moreinfo Every pin defaults to unset because a soldered line must never be guessed.
/// A default would pick the user's wiring, and on an S3 could land on PSRAM or a strapping pin.
struct Hub75Pins {
    uint16_t r1 = 0xFFFF, g1 = 0xFFFF, b1 = 0xFFFF;   ///< upper half-panel color
    uint16_t r2 = 0xFFFF, g2 = 0xFFFF, b2 = 0xFFFF;   ///< lower half-panel color
    uint16_t a = 0xFFFF, b = 0xFFFF, c = 0xFFFF;      ///< row address, 1/8 scan
    uint16_t d = 0xFFFF;                              ///< and 1/16 scan
    uint16_t e = 0xFFFF;                              ///< and 1/32 scan
    uint16_t clk = 0xFFFF, lat = 0xFFFF, oe = 0xFFFF; ///< shift clock, latch, blank
    /// The panel's shift registers sample on the falling clock edge rather than the rising one. Some chips do, and driven on the wrong edge every pixel lands one column over.
    bool clkFalling = false;
};

/// One running HUB75 port.
struct Hub75Handle { void* impl = nullptr; };

/// Which silicon block drives a HUB75 port.
enum class Hub75Backend : uint8_t { LcdCam = 0, Parlio = 1 };

/// Is this backend present on this silicon and able to carry a frame of `frameBytes`?
bool hub75BackendAvailable(Hub75Backend backend, size_t frameBytes);

/// The label this backend shows in the UI, stable across builds.
const char* hub75BackendLabel(Hub75Backend backend);

/// Bring up a HUB75 port on `backend` and allocate its frame buffer.
bool hub75Init(Hub75Handle& h, Hub75Backend backend, const Hub75Pins& pins,
               uint16_t width, uint16_t height, uint8_t scanRate, uint8_t bitDepth);

/// Why the last hub75Init failed, or null when it did not.
const char* hub75LastError();

/// The DMA frame buffer the driver encodes into; null before a successful init.
uint8_t* hub75Buffer(const Hub75Handle& h) MM_NONBLOCKING;
/// That buffer's capacity; 0 before a successful init.
size_t   hub75BufferCapacity(const Hub75Handle& h) MM_NONBLOCKING;

/// Arm the scan, which then runs forever from the same buffer.
bool hub75Start(Hub75Handle& h);

/// Measured refresh in Hz from the completed-scan counter; 0 before the first scan.
uint16_t hub75RefreshHz(const Hub75Handle& h) MM_NONBLOCKING;

/// Which backend this handle runs, for the status line; null before a successful init.
const char* hub75Backend(const Hub75Handle& h);

/// Release the port and its buffer.
void hub75Deinit(Hub75Handle& h);

// I2S audio input: two seams only, the read and the FFT, with everything between them domain code.

/// Configure the audio codec over I2C, where the board has one; true when there is nothing to do.
bool audioCodecInit(CodecType type, const AudioCodecPins& pins, uint32_t sampleRate);

/// Release the codec.
void audioCodecDeinit();

/// One live audio input: an I2S channel on a board, an OS capture device on desktop.
struct AudioMicHandle { void* impl = nullptr; };

/// Whether this target has any live audio, which is what makes the analysis path run.
constexpr bool hasAudioInput = hasI2sMic || hasAudioCapture;

/// The OS capture devices, entry 0 being the default; 0 where the target has none.
size_t audioCaptureDevices(const char* const** optionsOut);

/// Open one capture device as mono samples at `sampleRate`, which the backend resamples to.
bool audioCaptureInit(AudioMicHandle& h, uint8_t deviceIndex, uint32_t sampleRate);

/// How the microphone speaks, which decides its pin count and how the peripheral is configured.
enum class MicMode : uint8_t { I2sStd = 0, Pdm = 1 };

/// Bring up an I2S channel reading the mic on these pins, as 24-bit mono samples.
bool audioMicInit(AudioMicHandle& h, uint16_t wsPin, uint16_t sdPin,
                  uint16_t sckPin, int16_t mclkPin, uint32_t sampleRate,
                  MicMode mode = MicMode::I2sStd);

/// Read up to `maxSamples` samples, answering the count; cheap enough for the render tick.
size_t audioMicRead(AudioMicHandle& h, int32_t* out, size_t maxSamples);
/// Whether the I2S instance a PDM microphone needs is free right now.
bool audioMicSharedBusFree(MicMode mode);

/// Release the input.
void audioMicDeinit(AudioMicHandle& h);

/// Fill `outMag` with the magnitude bins of `n` windowed samples, `n` being a power of two.
void audioFft(const float* windowed, size_t n, float* outMag);

// --- USB video capture (UVC): an HDMI grabber presenting itself as a webcam ---------------------
// MJPEG off the wire, decoded by the target's JPEG hardware, and ESP32-P4 only: every other target links a stub whose init fails, which VideoService reports as a status.

/// Opaque per-device state, owned by the platform between init and deinit.
struct VideoCaptureHandle { void* impl = nullptr; };

// MJPEG only, since nothing else is decodable here, so there is no format field.
/// One row of what the attached device advertises, so the UI offers real choices rather than guesses.
struct VideoCaptureFormat {
    /// Frame width in pixels.
    uint16_t width = 0;
    /// Frame height in pixels.
    uint16_t height = 0;
    /// Frames per second the device offers at that size.
    uint8_t fps = 0;
};

// A grabber lists its modes largest first, so a cap that is too low hides exactly the cheap ones.
/// Rows kept per device.
constexpr size_t kVideoCaptureMaxFormats = 64;

// Learned when a device enumerates, so it survives a failed videoCaptureInit, which is exactly when it is worth reading.
/// Fill `out` with up to `max` of those rows and return how many were written; 0 means no device has been seen yet.
size_t videoCaptureFormats(VideoCaptureFormat* out, size_t max);

// A consumer caching the list compares this instead of copying, so noticing a hotplug costs one load.
/// Bumped whenever that list is rewritten, which is when a device enumerates; 0 until then.
uint32_t videoCaptureFormatGeneration();

// Width, height and fps are requests rather than promises. A device that goes away is not followed: a reconnect bumps the generation, and the caller answers with a deinit/init pair.
/// Claim the first UVC device on the bus and stream MJPEG, false when nothing is attached or no MJPEG format matches.
bool videoCaptureInit(VideoCaptureHandle& h, uint16_t width, uint16_t height, uint8_t fps);

// The buffer belongs to the platform, allocated in init and freed in deinit, and the decoder never writes into the one most recently returned. See videoCaptureInit's appendix.
/// Newest decoded frame as RGB888, held until the next call that RETURNS A FRAME, or nullptr when none arrived.
const uint8_t* videoCaptureFrame(VideoCaptureHandle& h, uint16_t& width, uint16_t& height) MM_NONBLOCKING;

/// Release the device and free its buffers. The caller drops the frame it borrowed first.
void videoCaptureDeinit(VideoCaptureHandle& h);

// A drop in ones is normal, and a climbing count is a fault worth naming, since in the picture it is only a stutter.
/// Why frames did not reach the renderer, cumulative since boot.
struct VideoCaptureStats {
    /// Frames that reached a slot.
    uint32_t decoded = 0;
    /// Arrived while the previous frame was still waiting to be decoded.
    uint32_t busy = 0;
    /// Every decode buffer still held by the renderer.
    uint32_t noSlot = 0;
    /// Not a readable JPEG: a mis-detected payload stride lands here.
    uint32_t infoFail = 0;
    /// Larger than the buffers sized at open.
    uint32_t oversize = 0;
    /// The decoder refused a bitstream whose header it had accepted.
    uint32_t decodeFail = 0;
};
/// The counters above, read atomically enough for a once-a-second status line.
VideoCaptureStats videoCaptureStats();

// I2C bus diagnostics: the standard i2cdetect operation, domain-neutral rather than audio-specific.

/// The bus could not be opened, which is distinct from a scan that found nothing.
inline constexpr size_t kI2cBusUnavailable = static_cast<size_t>(-1);

/// Scan the bus on these pins, writing the addresses that answer into `out` and returning the count.
size_t i2cScan(uint16_t sda, uint16_t scl, uint8_t* out, size_t maxOut);

// --- GPIO as a role: read a switch or drive a line, the module owning debouncing ---------------

/// The internal pull to enable on an input; a mechanical switch needs one, a driven pin does not.
enum class GpioPull : uint8_t { None = 0, Up, Down };

/// Configure one GPIO as an input; false where the chip has no usable input there.
bool gpioInputBegin(uint8_t gpio, GpioPull pull);

/// Read a configured pin, true being HIGH; an unconfigured pin reads false.
bool gpioRead(uint8_t gpio);

/// Drive one GPIO as a push-pull output, configuring it on first use; false where it has no driver.
bool gpioWrite(uint8_t gpio, bool high);

/// Make gpioRead answer `level` for one pin.
void setTestGpioLevel(uint8_t gpio, bool level);
/// Drop every injected level.
void clearTestGpioLevel();

/// Read one ADC pin as a raw count; false where the chip has no ADC there or the read failed.
bool adcRead(uint8_t gpio, uint16_t& raw);

/// The full-scale count `adcRead` reports here, so a caller scales without knowing the chip.
uint16_t adcMaxCount();

/// Read one ADC pin as millivolts; false where `adcRead` would fail, or the chip carries no calibration.
bool adcReadMv(uint8_t gpio, uint16_t& mv);

/// Make adcReadMv answer `mv` for one pin.
void setTestAdcMv(uint8_t gpio, uint16_t mv);

/// Make adcRead answer `raw` for one pin, so a pedal's mapping is host-testable.
void setTestAdcValue(uint8_t gpio, uint16_t raw);
/// Drop every injected count.
void clearTestAdcValue();

/// Poll the IR receiver for a remote frame, answering true once per fresh code.
bool irRead(uint16_t pin, uint32_t& codeOut);

/// Release the IR channel, freeing its pin; irRead reopens lazily, so this is safe at any time.
void irStop();

/// Open or confirm the IR channel and report whether it is live, which irRead cannot distinguish.
bool irChannelReady(uint16_t pin);

/// @}

} // namespace mm::platform
