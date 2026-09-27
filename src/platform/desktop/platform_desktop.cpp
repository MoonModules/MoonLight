/// @defgroup platform_desktop The desktop platform layer
/// Every platform seam on macOS, Windows and Linux, so the desktop build runs what a device runs.
///
/// Where a host has the facility it is real: sockets, the filesystem, executable memory, raw frames.
/// Where it has no silicon the seam is backed by ordinary memory instead of refused, which is what keeps the drivers above testable off device.
///
/// @moreinfo
///
/// ## A host bus is real memory, not a refusal
///
/// The parallel output seams used to return failure, so a driver body of some 2500 lines never executed off device.
/// It was not runnable, not testable, and invisible to every syntax-tree check.
/// So a bus is implemented against a heap buffer: init allocates, buffer hands back writable memory, transmit records the byte count, wait returns at once.
/// Everything above the seam is then the same code that runs on hardware, and only the hand-off is absent.
///
/// Deliberately not modelled: timing, wire protocol, pin state and loopback capture.
/// Those need silicon, and faking them would make a driver's self-test lie about hardware it never touched.
/// HUB75 is the exception that stays inert, having no host analogue worth faking, and its encoder is already tested on plain buffers.
///
/// ## The identity is stored, not read from a NIC
///
/// The address is generated once and kept beside the config, which is the pattern systemd uses for its machine identity.
/// It matters because the address is an identity rather than a diagnostic: the device name, the MQTT topic prefix and the Home Assistant identifier all derive from it.
/// A hardcoded value made every desktop instance the same one, so two desktops or a handful of containers were indistinguishable and fought over one entity.
/// Home Assistant requires that such an identifier survive container recreation, which storing it achieves and reading a host interface does not.
/// This Mac lists an internal management interface before its real one, and containers sharing a bridge can present related addresses.
///
/// The address is locally administered and unicast, the range set aside for addresses that are not vendor-assigned, so it cannot collide with real hardware.
/// Existing installs keep their identity: a tree with no identity file but with config is seeded with the historic value, so an upgrade never renames a device.
/// The write is atomic like every other config write, since a torn line would be rejected on the next start and the device would silently take a different identity.
///
/// ## The data root is per-user, not relative
///
/// A shipped binary is launched from a download folder, a menu shortcut or an installer's directory.
/// A relative root fails both ways.
/// It lands somewhere unwritable, so every save fails, or it makes the settings belong to that folder rather than to the user, so moving the executable loses them.
/// Both were seen on a Windows bench. Three sources are tried in order: an explicit override, a repo checkout, then the operating system's per-user application data.
///
/// A checkout is recognized by two markers together, because one of them is true in the root of every CMake project there is.
/// A developer whose shell sits in an unrelated one would otherwise get this project's settings written into that project's build directory.
/// It keys on the working directory rather than the executable's location, since that is the development loop this preserves and an installed copy is never launched that way.
///
/// ## The raw-frame driver is loaded, not linked
///
/// Windows has no kernel path for sending a raw layer-two frame: it takes a third-party driver, which ColorLight's own software also uses.
/// The library is resolved at run time rather than linked, and that is deliberate. This layer is a public dependency of the core, the application and both test binaries.
/// Linking it would make that vendor's kit a build requirement for continuous integration and for every contributor, to compile a path most of them never run.
/// Loading on demand means the binary builds and runs identically without it, and reports that raw send is unavailable instead of failing to link.
/// The whole surface is five functions, declared with the library's own signatures rather than by including its header, which would reintroduce the dependency this avoids.
///
/// ## Interface labels carry the negotiated speed
///
/// The lookup is necessarily per-operating-system, three interfaces in three units, which is what this layer is for.
/// The rendering is not, so it lives here once.
/// The label shape is a contract that the apply path and the driver's remap both parse, to recover the adapter's stable identity.
/// Two copies would be two chances to drift out of that agreement.
/// A speed of zero means the system would not state one, for a virtual adapter or a link that is down, and that appends nothing rather than a fabricated figure.
/// It appends only if the whole suffix fits, since a truncated speed reads worse than none and the label is what the selection persists by.
///
/// ## Config files are written owner-only
///
/// The standard open creates a file the process umask widens, so on a typical one it lands world-readable.
/// These are the config files, which hold network keys and broker passwords, and a desktop runs on a real machine with real other users.
/// A device is unaffected: its filesystem has no modes at all.
///
/// One platform gets an exclusive create with an explicit mode.
/// Exclusive, because a pre-existing file at one of these paths is either a crashed run's leftover or somebody else's, and inheriting its mode would defeat the point.
/// The other has no mode concept and its files inherit the directory's access list, which is that platform's own answer to the same question, so it keeps the plain open.
///
/// ## Address reuse means opposite things
///
/// On one platform the option lets a fresh socket claim a port left waiting from a closed connection, and never allows two live binds to overlap.
/// On the other its meaning is reversed: two live sockets can hold the same port, so a second bind succeeds where the retry logic expects a refusal.
/// That platform's default already matches the first one's behavior, so the option is simply not set there.
///
/// The outcome still is not uniform: on one system the option on a datagram socket bound to every address permits an overlapping bind, so a second one succeeds.
/// A test that needs a bind to fail must use the test override rather than holding the port.
///
/// ## What the allocation counter is for
///
/// Not a heap figure: a desktop has as much memory as it wants.
/// And the free-heap report stays at nothing because three call sites read that as unlimited and switch off gates that only mean something on a device.
/// What it is good for is the DELTA. Every buffer the system takes on purpose comes through these entry points.
/// So adding or removing a module moves the number by exactly what that module costs, on a laptop, in a second, with no board attached.
/// The process's own resident size cannot answer that, since the allocator, the compiler and the network buffers move it too, and a small layer would be lost in the noise.
/// The REQUESTED size is recorded rather than the allocator's rounded one, so a reported delta is the number the caller asked for.
///
/// ## The render sleeps to a frame budget
///
/// Yielding alone only offers the processor to another runnable thread.
/// So on an otherwise idle machine it returns at once and the caller spins a core flat out, reported from a bench as the process slowly eating more cycles.
/// Nothing consumes a desktop render faster than a display or a driver's own rate limit.
/// So a loop free-running at thousands of frames is spending a core to compute frames nobody reads.
/// The budget is far above any output rate we drive while leaving the processor idle in between.
/// And a tick that legitimately runs longer simply gets no sleep, so a heavy grid still runs as fast as it can.
///
/// ## The video runtime's structures are transcribed, not included
///
/// The runtime is resolved on demand and never linked, bundled, or its headers included, the same arrangement as the raw-frame driver and for the same licensing reason.
/// The user installs it; a machine without it builds and runs identically and reports the feature unavailable.
///
/// So the declarations are transcribed from the vendor's own public headers.
/// Getting a field's type or ORDER wrong is a silent crash or a skewed image rather than a compile error.
/// These are passed by pointer into a binary built against the real definitions.
/// They are quoted verbatim in the plan with their source, and must not be tidied.
///
/// ## Naming an adapter when the capture library cannot
///
/// That library's own description is sometimes absent, and then the adapter is unnameable: the only text left to match is a 49-character device path.
/// So the system's own description is found through the interface table, keyed on the identifier the device path already carries.
/// The negotiated speed rides in the label too, because the name alone does not say what a picker needs to know.
/// A wall wants the gigabit adapter, and a list of plausible names hides which entries are a dongle, a radio or a virtual switch.
/// An adapter whose speed the system will not state gets no suffix rather than a fabricated one.
///
/// ## The interface table, not the address list
///
/// An adapter bound to a virtual switch does not appear in the address list at all: the system reports the virtual one and hides the physical one the switch owns.
/// Measured here, where the capture library opens a device whose identifier is in no address-list row.
/// The interface table lists the physical one and carries the same identifier, so one exact key covers both a virtualized adapter and one described as nothing at all.
/// The description cannot do that, being absent on some and filter-suffixed on others.
///
/// ## Which adapters can carry panel frames
///
/// The interface type alone is not the test: measured on a bench, the virtual switch ports, every wide-area miniport, the bridge and the personal-area network all report the same type.
/// Whether it is a hardware interface is what separates them from an adapter with a socket on it.
/// A radio fails the type test instead, which is the right answer for a card that needs a wire.
///
/// ## The link query used to be a stub
///
/// It returned false on every host but one, which made the driver report no link while it drove a card perfectly.
/// The send path was implemented and only the state query was missing, so the health check contradicted the driver's own output.
/// Reported by a user driving a card from a small board.

#include "platform/platform.h"
#include "core/util/FirmwareImage.h"  // identify/moonBaseRejection: shared image vetting
#include "core/util/H264Bitstream.h"  // where the encoder's frames begin and end

#include <algorithm>
#include <chrono>
#include <bit>       // std::countr_zero, the radix-2 audioFft's bit-reversal
#include <cmath>     // cos/sin/sqrt for audioFft's twiddles and magnitudes
#include <numbers>   // std::numbers::pi_v, same kernel
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>      // the stored identity is generated once, see getMacAddress
#include <fstream>
#ifdef MM_HAVE_CURL
#include <curl/curl.h>   // https POST: the OS's own TLS, nothing vendored
#endif
#include <filesystem>
#include <string>
#ifndef _WIN32
#include <dlfcn.h>   // dlopen/dlsym — the NDI runtime is resolved on demand, never linked
#endif
#include <vector>   // HostBus frame buffers — the memory-backed parallel bus
#include <thread>
#include <deque>     // encoder frame queue between the render tick and the writer thread
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cerrno>

#ifdef _WIN32
// The handle stays an int in the shared header: the narrowing is well-defined for handles in the practical range, and is the standard pattern.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>     // _fileno, _commit (POSIX fileno/fsync equivalents)
#include <iphlpapi.h>   // GetIfTable2 — real link state + negotiated speed (ethLinkUp)
#include <netioapi.h>   // MIB_IF_ROW2: sees a NIC a Hyper-V vSwitch hides from GetAdaptersAddresses
#include <winhttp.h>    // https POST: Windows' own TLS, so the .exe needs no bundled library
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>      // getaddrinfo — hostname resolution for TcpConnection::connect
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>   // waitpid: reaping the spawned ffmpeg (encoderRunning/Stop)
#include <spawn.h>      // posix_spawn: fd-hygienic, thread-safe child creation (encoderStart)
extern char** environ;   // posix_spawnp wants the environment explicitly
#include <csignal>      // SIGPIPE ignore + SIGKILL: the encoder pipe's failure surface
#include <sys/mman.h>   // mmap/munmap for allocExec (executable pages)
#include <net/if.h>     // if_nametoindex / ifreq — naming the NIC for raw L2 send
#include <ifaddrs.h>    // getifaddrs: the raw-interface Select enumerates the host NICs
#ifdef __linux__
#include <netpacket/packet.h>   // sockaddr_ll — AF_PACKET raw frames (ethSendRaw)
#include <net/ethernet.h>       // ETH_P_ALL
#endif
#ifdef __APPLE__
#include <net/if_media.h>   // SIOCGIFMEDIA: the negotiated link rate, for the interface labels
#include <pthread.h>    // pthread_jit_write_protect_np — macOS arm64 W^X JIT toggle
#include <sys/ioctl.h>  // BIOCSETIF — binding a BPF device to an interface (ethSendRaw)
#include <net/bpf.h>
#endif
#endif

namespace mm::platform {

namespace {
/// Append the speed to an interface label in the one format every list uses: @xref{interface-labels-carry-the-negotiated-speed|why here, and why zero appends nothing}.
void appendLinkSpeed(char* out, size_t cap, unsigned mbps) {
    if (!out || mbps == 0) return;
    const size_t n = std::strlen(out);
    char speed[24];
    if (mbps >= 1000 && mbps % 1000 == 0)
        std::snprintf(speed, sizeof(speed), ", %u Gb", mbps / 1000);
    else if (mbps >= 1000)
        std::snprintf(speed, sizeof(speed), ", %u.%u Gb", mbps / 1000, (mbps % 1000) / 100);
    else
        std::snprintf(speed, sizeof(speed), ", %u Mb", mbps);
    if (n + std::strlen(speed) + 1 <= cap) std::snprintf(out + n, cap - n, "%s", speed);
}


// Small shims mapping two socket interfaces onto one surface, so each call site reads as plain code.
#ifdef _WIN32
// Cast to the unsigned handle type at the boundary, so the warning level does not fire at every call site.
inline SOCKET sock(int fd) { return static_cast<SOCKET>(fd); }
inline int close_sock(int fd) { return ::closesocket(sock(fd)); }
// Both the would-block and the timed-out cases translate to the same retry semantics the caller expects.
inline bool sockWouldBlock() {
    int err = ::WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT;
}
inline int open_sock(int domain, int type, int protocol) {
    SOCKET s = ::socket(domain, type, protocol);
    return (s == INVALID_SOCKET) ? -1 : static_cast<int>(s);
}
inline int make_nonblocking(int fd) {
    u_long mode = 1;
    return ::ioctlsocket(sock(fd), FIONBIO, &mode);
}
inline int make_blocking(int fd) {
    u_long mode = 0;
    return ::ioctlsocket(sock(fd), FIONBIO, &mode);
}
#else
inline int sock(int fd) { return fd; }
inline int close_sock(int fd) { return ::close(fd); }
inline bool sockWouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
inline int open_sock(int domain, int type, int protocol) {
    return ::socket(domain, type, protocol);
}
inline int make_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
inline int make_blocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}
#endif
#ifdef _WIN32
// The stack is initialized once per process by a static guard at library load, which covers the application and the test binaries alike.
struct WinsockInit {
    WinsockInit() {
        WSADATA d;
        ::WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WinsockInit() { ::WSACleanup(); }
};
static WinsockInit g_winsockInit;
#endif

}  // namespace

static auto startTime = std::chrono::steady_clock::now();
// Test-only clock override, zero meaning the real clock; atomic, since a test sets it from another thread.
static std::atomic<uint32_t> testNowMs{0};

void setTestNowMs(uint32_t ms) { testNowMs.store(ms, std::memory_order_relaxed); }

// The clock read allocates nothing and takes no lock, but the library does not say so, so the effect warning must assume the worst.
// Ask the compiler whether it HAS the warning rather than inferring it from a version number: an unknown pragma and an unknown warning group are both errors under our settings.
// A version test does not work here, since one vendor's compiler carries its own version line and reported a high major while predating the warning.
// Which is exactly how this reached the main branch.
#if defined(__clang__) && defined(__has_warning)
#  if __has_warning("-Wfunction-effects")
#    define MM_SUPPRESS_FUNCTION_EFFECTS 1
#  endif
#endif
#ifdef MM_SUPPRESS_FUNCTION_EFFECTS
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
#endif
uint32_t millis() MM_NONBLOCKING {
    uint32_t override_ = testNowMs.load(std::memory_order_relaxed);
    if (override_) return override_;
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
    );
}

// The OS thread identity as an integer. std::this_thread::get_id() is the portable spelling but is not convertible to an integer, so each platform's own call is used: GetCurrentThreadId on Windows, pthread_self elsewhere. The +1 guarantees a non-zero result so callers can treat 0 as "none".
uintptr_t currentThreadId() MM_NONBLOCKING {
#ifdef _WIN32
    return static_cast<uintptr_t>(GetCurrentThreadId()) + 1;
#else
    return reinterpret_cast<uintptr_t>(pthread_self()) + 1;
#endif
}

uint32_t micros() MM_NONBLOCKING {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count()
    );
}
#ifdef MM_SUPPRESS_FUNCTION_EFFECTS
#pragma clang diagnostic pop
#endif

// What this process has deliberately allocated, in bytes: @xref{what-the-allocation-counter-is-for|why it is a delta rather than a heap figure}.
std::atomic<size_t> g_allocatedBytes{0};
std::atomic<size_t> g_allocatedPeak{0};
std::atomic<uint32_t> g_allocCount{0};

namespace {
// The requested size, kept before the block; sized so the returned pointer keeps the alignment the allocator promised.
constexpr size_t kAllocHeader = 16;

void* trackedAlloc(size_t bytes) {
    void* raw = std::malloc(bytes + kAllocHeader);
    if (!raw) return nullptr;
    *static_cast<size_t*>(raw) = bytes;
    const size_t now = g_allocatedBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    // The peak is advisory, so a lost race costs a slightly low high-water mark rather than anything a caller depends on.
    if (now > g_allocatedPeak.load(std::memory_order_relaxed))
        g_allocatedPeak.store(now, std::memory_order_relaxed);
    g_allocCount.fetch_add(1, std::memory_order_relaxed);
    return static_cast<uint8_t*>(raw) + kAllocHeader;
}
}  // namespace

void* alloc(size_t bytes) {
    return trackedAlloc(bytes);
}

bool ptrIsPsram(const void* /*p*/) { return false; }   // desktop has no PSRAM

void* allocInternal(size_t bytes) {
    return trackedAlloc(bytes);   // desktop has one flat RAM: internal == ordinary
}

void free(void* ptr) {
    if (!ptr) return;
    void* raw = static_cast<uint8_t*>(ptr) - kAllocHeader;
    g_allocatedBytes.fetch_sub(*static_cast<size_t*>(raw), std::memory_order_relaxed);
    // Decremented, so the count is live blocks rather than allocations ever: otherwise it only climbs and reads as a leak.
    g_allocCount.fetch_sub(1, std::memory_order_relaxed);
    std::free(raw);
}

// Executable memory for emitted code. One platform allows a page to be writable or executable but never both, so the write happens later behind a per-thread toggle; the others allow one plain page.
void* allocExec(size_t bytes) {
    if (bytes == 0) return nullptr;
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    return p;   // VirtualAlloc returns nullptr on failure
#elif defined(__APPLE__)
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#else
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

void freeExec(void* ptr, size_t bytes) {
    if (!ptr) return;
#ifdef _WIN32
    (void)bytes;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    ::munmap(ptr, bytes);
#endif
}

void writeExec(void* dst, const void* src, size_t len) {
    if (!dst || !src || !len) return;
#if defined(_WIN32)
    // The page is already both writable and executable here, so a copy suffices, followed by an instruction-cache flush.
    std::memcpy(dst, src, len);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
#elif defined(__APPLE__)
    // Flip this thread's pages to writable, copy, flip back, then sync the instruction cache, which this architecture requires for fresh code.
    pthread_jit_write_protect_np(0);
    std::memcpy(dst, src, len);
    pthread_jit_write_protect_np(1);
    __builtin___clear_cache(static_cast<char*>(dst), static_cast<char*>(dst) + len);
#else
    // The page is plain memory here, so a copy suffices; the cache sync matters on one architecture and is a no-op on the other.
    std::memcpy(dst, src, len);
    __builtin___clear_cache(static_cast<char*>(dst), static_cast<char*>(dst) + len);
#endif
}

void yield() {
    // Hand the processor to another runnable thread, and really yield. The frame boundary polls this waiting for the encode worker, so a no-op would pin a core and starve the very worker it waits for.
    std::this_thread::yield();
}

void delayMs(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void delayUs(uint32_t us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

void pauseLoop() {
    // Sleep to a frame BUDGET rather than a fixed nap: @xref{the-render-sleeps-to-a-frame-budget|why yielding alone spins a core}.
    static constexpr auto kFrameBudget = std::chrono::microseconds(4000);
    static auto lastWake = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const auto spent = now - lastWake;
    if (spent < kFrameBudget) std::this_thread::sleep_for(kFrameBudget - spent);
    lastWake = std::chrono::steady_clock::now();
}

size_t allocatedBytes() { return g_allocatedBytes.load(std::memory_order_relaxed); }
size_t allocatedPeak()  { return g_allocatedPeak.load(std::memory_order_relaxed); }
uint32_t allocatedCount() { return g_allocCount.load(std::memory_order_relaxed); }

size_t freeHeap() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

size_t freeInternalHeap() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

// Test-only cap on the reported largest free block, so a test can force the paged fallback without an actually fragmented heap.
static std::atomic<size_t> testMaxBlock{0};
void setTestMaxAllocBlock(size_t bytes) { testMaxBlock.store(bytes, std::memory_order_relaxed); }

size_t maxAllocBlock() {
    return testMaxBlock.load(std::memory_order_relaxed); // 0 = unlimited
}

size_t maxInternalAllocBlock() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

size_t maxExecAllocBlock() {
    return 0;   // no distinct executable pool: pages are mapped per allocation
}

// No task system here, so the module shows only its own cost table; a test can inject a canned snapshot to exercise its rows and nesting on the host.
static const TaskInfo* g_testTasks = nullptr;
static size_t g_testTaskCount = 0;
static const char* g_testRenderTask = "";
void setTestTaskSnapshot(const TaskInfo* tasks, size_t count, const char* renderTask) {
    g_testTasks = tasks; g_testTaskCount = count; g_testRenderTask = renderTask ? renderTask : "";
}
size_t taskSnapshot(TaskInfo* out, size_t maxTasks) {
    if (!g_testTasks || !out) return 0;
    const size_t n = g_testTaskCount < maxTasks ? g_testTaskCount : maxTasks;
    for (size_t i = 0; i < n; i++) out[i] = g_testTasks[i];
    return n;
}
void currentTaskOnCore(int, char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
const char* renderTaskName() { return g_testRenderTask; }

// The worker seam, backed by a thread and a condition variable.
// The core pin is ignored, but the handoff is real, so the render and encode invariants are testable on an actual second thread.
// The wake is a single-slot latch, matching the device notification's one-pending-count semantics so a host test sees the same behavior.
namespace {
struct DesktopWorker {
    std::thread thread;
    std::mutex mtx;
    std::condition_variable cv;
    bool pending = false;   // a notify is waiting to be consumed (the single-slot latch)
    bool stop = false;
};
}  // namespace

bool spawnPinnedTask(WorkerTask& t, const char* /*name*/, WorkerFn fn, void* user,
                     size_t /*stackBytes*/, uint8_t /*priority*/, int /*core*/) {
    auto* w = new (std::nothrow) DesktopWorker();
    if (!w) return false;
    t.impl = w;
    w->thread = std::thread([fn, user] { fn(user); });   // the fn owns its loop until stop
    return true;
}

void notifyTask(WorkerTask& t) {
    auto* w = static_cast<DesktopWorker*>(t.impl);
    if (!w) return;
    { std::scoped_lock<std::mutex> lk(w->mtx); w->pending = true; }
    w->cv.notify_one();
}

bool waitNotify(WorkerTask& t, uint32_t timeoutMs) {
    auto* w = static_cast<DesktopWorker*>(t.impl);
    if (!w) return false;
    std::unique_lock<std::mutex> lk(w->mtx);
    const bool got = w->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                    [w] { return w->pending || w->stop; });
    if (!got) return false;         // timed out with no notify/stop
    w->pending = false;             // consume the single-slot latch
    return true;                    // woken by a notify OR stop; the fn re-checks its stop flag
}

void stopPinnedTask(WorkerTask& t) {
    auto* w = static_cast<DesktopWorker*>(t.impl);
    if (!w) return;
    { std::scoped_lock<std::mutex> lk(w->mtx); w->stop = true; }
    w->cv.notify_one();
    if (w->thread.joinable()) w->thread.join();
    delete w;
    t.impl = nullptr;
}

void taskWdtSubscribe() {}     // no watchdog on the host
void taskWdtUnsubscribe() {}   // no watchdog on the host
void taskWdtReset() {}         // no watchdog on the host


// A host has no pins to protect, so the map flags nothing, which is correct: there is no silicon to corrupt. A test can override one pin's capability to exercise the severity derivation, held in a small fixed table.
namespace {
struct GpioCapOverride { uint8_t gpio; GpioCapability cap; bool set; };
GpioCapOverride g_gpioCapOverrides[16] = {};
}  // namespace
GpioCapability gpioCapability(uint8_t gpio) {
    for (const auto& o : g_gpioCapOverrides)
        if (o.set && o.gpio == gpio) return o.cap;
    return GpioCapability{};
}

const char* gpioRefusal(uint8_t gpio) {
    const GpioCapability c = gpioCapability(gpio);
    if (!c.validGpio) return "does not exist on this chip package";
    if (c.reserved)   return "is wired to flash/PSRAM on this chip";
    return nullptr;
}

void setTestGpioCapability(uint8_t gpio, GpioCapability cap) {
    for (auto& o : g_gpioCapOverrides)
        if (!o.set || o.gpio == gpio) { o = {gpio, cap, true}; return; }
}
void clearTestGpioCapability() {
    for (auto& o : g_gpioCapOverrides) o.set = false;
}

// Live state: no real pins, so the map omits the live columns unless a test injects one.
namespace {
struct GpioLiveOverride { uint8_t gpio; GpioLiveState state; bool set; };
GpioLiveOverride g_gpioLiveOverrides[16] = {};
}  // namespace
GpioLiveState gpioLiveState(uint8_t gpio) {
    for (const auto& o : g_gpioLiveOverrides)
        if (o.set && o.gpio == gpio) return o.state;
    return GpioLiveState{};   // valid=false
}
void setTestGpioLiveState(uint8_t gpio, GpioLiveState state) {
    for (auto& o : g_gpioLiveOverrides)
        if (!o.set || o.gpio == gpio) { o = {gpio, state, true}; return; }
}
void clearTestGpioLiveState() {
    for (auto& o : g_gpioLiveOverrides) o.set = false;
}

size_t totalHeap() {
    return 0; // Not meaningful on desktop
}

size_t totalInternalHeap() {
    return 0; // Not meaningful on desktop
}

const char* macString() {
    static char buf[18] = {};
    if (buf[0] == 0) {
        uint8_t mac[6];
        getMacAddress(mac);
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return buf;
}

const char* chipModel() {
    // The real instruction set rather than the word desktop, since on a device this names the silicon and a category would leave every host indistinguishable. Compile-time, because a binary is built for one architecture; an emulated one reports what is actually running, which is the truthful answer.
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm32";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "desktop";   // an architecture nobody has built for yet; better vague than wrong
#endif
}

bool httpsAvailable() MM_NONBLOCKING {
#if defined(_WIN32) || defined(MM_HAVE_CURL)
    // Windows needs no libcurl: WinHTTP ships with the OS, so the send path is always compiled in.
    return true;
#else
    return false;   // built without libcurl: httpsPost can never succeed
#endif
}

bool httpsPost(const char* url, const char* body, uint32_t timeoutMs) {
#ifdef _WIN32
    // The system's own transport here, for the same reason the others use theirs: the certificate store the platform already ships, with nothing vendored and one link entry.
    if (!url || !*url) return false;

    // This interface takes the pieces of a URL separately, so its own splitter does the work and nothing is parsed by hand.
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, url, -1, nullptr, 0);
    if (wideLen <= 0) return false;
    std::wstring wideUrl(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wideUrl.data(), wideLen);

    wchar_t host[256] = {};
    wchar_t path[1024] = {};
    wchar_t query[1024] = {};
    URL_COMPONENTS parts = {};
    parts.dwStructSize      = sizeof(parts);
    parts.lpszHostName      = host;   parts.dwHostNameLength      = ARRAYSIZE(host);
    parts.lpszUrlPath       = path;   parts.dwUrlPathLength       = ARRAYSIZE(path);
    // The splitter separates the query from the path, so asking for it and re-joining keeps this general: a later caller's query would otherwise be dropped silently.
    parts.lpszExtraInfo     = query;  parts.dwExtraInfoLength     = ARRAYSIZE(query);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) return false;
    // Anything but https is a caller error rather than something to downgrade into cleartext.
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) return false;
    const std::wstring target = std::wstring(path) + query;

    HINTERNET session = WinHttpOpen(L"MoonLight", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    // The caller's one timeout applied to every phase, since a hung handshake must bound the same way a hung read does.
    const int t = static_cast<int>(timeoutMs);
    WinHttpSetTimeouts(session, t, t, t, t);

    bool ok = false;
    if (HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0)) {
        if (HINTERNET request = WinHttpOpenRequest(connection, L"POST", target.c_str(), nullptr,
                                                   WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   WINHTTP_FLAG_SECURE)) {
            // No redirect following: the endpoint is ours and answers directly, and following one would let a server move the body elsewhere.
            DWORD noRedirects = WINHTTP_DISABLE_REDIRECTS;
            WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
                             &noRedirects, sizeof(noRedirects));

            const char* payload = body ? body : "";
            const DWORD payloadLen = static_cast<DWORD>(std::strlen(payload));
            // The body rides with the request, which is what lets a one-shot post skip a separate write.
            if (WinHttpSendRequest(request,
                                   L"Content-Type: application/json\r\n",
                                   static_cast<DWORD>(-1),
                                   const_cast<char*>(payload), payloadLen, payloadLen, 0)
                && WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0, statusSize = sizeof(status);
                if (WinHttpQueryHeaders(request,
                                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                        WINHTTP_NO_HEADER_INDEX)) {
                    ok = status >= 200 && status < 300;
                }
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    // The response body is never read, matching the contract: the caller has nothing to do with it and the handles close either way.
    return ok;
#elif defined(MM_HAVE_CURL)
    if (!url || !*url) return false;

    // The library's global init is not thread-safe and must run once, so a function-local static does it: the first call initializes and every later one skips.
    static const bool inited = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    if (!inited) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeoutMs));
    // Both verifications are the defaults, set explicitly so a future edit cannot turn them off quietly: without them the transport proves nothing about who answered.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // No redirect following: the endpoint is ours, and following one would let a server move the body somewhere the caller never named.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);   // no SIGALRM in a threaded process

    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // Discard the body rather than buffer it, a callback returning the full size being how the library is told the data was consumed.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     +[](char*, size_t size, size_t nmemb, void*) -> size_t { return size * nmemb; });

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && status >= 200 && status < 300;
#else
    // Built without libcurl: the caller degrades visibly rather than pretending it sent something.
    (void)url; (void)body; (void)timeoutMs;
    return false;
#endif
}

const char* hostPlatform() {
    // The same names the release packaging uses, so a breakdown lines up with the downloads it came from rather than inventing a second vocabulary. A container reports as one rather than as its host system, since being a container is what changes how it behaves, and the system underneath is already visible elsewhere.
#if defined(__linux__)
    // The marker the runtime itself creates, checked once, since a process cannot move in or out of a container while it runs.
    static const bool inContainer = std::filesystem::exists("/.dockerenv");
    // The architecture rides along, since a container on a small board and one on a server are different deployments that the label alone made one slice.
  #if defined(__aarch64__)
    if (inContainer) return "docker-arm64";
  #else
    if (inContainer) return "docker-x64";
  #endif
  #if defined(__aarch64__)
    // Every small arm board lands here, and the breakdown exists to count them: naming the other architecture would report each one as a PC.
    return "linux-arm64";
  #else
    return "linux-x64";
  #endif
#elif defined(__APPLE__)
  #if defined(__aarch64__)
    return "macos-arm64";
  #else
    return "macos-x64";
  #endif
#elif defined(_WIN32)
  #if defined(_M_ARM64)
    return "windows-arm64";
  #else
    return "windows-x64";
  #endif
#else
    return "";
#endif
}

uint8_t currentCore() { return 0; }

uint32_t cycleCount() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* cpuInfo() {
    // Cores only: the host's clock speed has no portable query (and boosts dynamically anyway).
    static char buf[16] = {};
    if (!buf[0])
        std::snprintf(buf, sizeof(buf), "%u cores", std::thread::hardware_concurrency());
    return buf;
}

const char* hostIp() {
    // Resolve the outbound address: connecting a datagram socket sends nothing and merely selects the route, so the socket then names this host's address.
    static char ip[INET_ADDRSTRLEN] = {};
    if (ip[0]) return ip;
    int fd = open_sock(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "";
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);
    if (::connect(sock(fd), reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (::getsockname(sock(fd), reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
        }
    }
    close_sock(fd);
    return ip;
}

const char* sdkVersion() {
#ifdef __clang__
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#else
    return "unknown";
#endif
}

const char* coprocessorWifi() {
    return "";   // desktop has no WiFi co-processor
}

const char* psramType() {
    return "";   // desktop has no PSRAM
}

const char* resetReason() {
    // Desktop has no reset-reason concept; report a benign value the UI treats as "not crashed".
    return "OK";
}

void setLogLevel(LogLevel) {
    // The terminal takes every line here and the measurement gate reads the level directly, so there is nothing to apply to a platform logger.
}

size_t firmwareSize() { return 0; }
size_t firmwarePartition() { return 0; }
size_t flashChipSize() { return 0; }

// Rooted at fsRoot_, a PER-USER data directory: @xref{the-data-root-is-per-user-not-relative|the three sources and why not a relative one}.

namespace {

// The convention for per-user application data, empty when the environment names no home, which a bare service account really has.
std::filesystem::path userDataDir() {
#ifdef _WIN32
    // LOCALAPPDATA, not APPDATA: this is machine-local state and has no business roaming.
    if (const char* base = std::getenv("LOCALAPPDATA"); base && *base)
        return std::filesystem::path(base) / "projectMM";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / "Library" / "Application Support" / "projectMM";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "projectMM";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".local" / "share" / "projectMM";
#endif
    return {};
}

// Both markers together, and the working directory rather than the executable's: @xref{the-data-root-is-per-user-not-relative|why}.
std::filesystem::path defaultRoot() {
    if (const char* env = std::getenv("MM_DATA_DIR"); env && *env)
        return std::filesystem::path(env);
    std::error_code ec;
    if (std::filesystem::exists("CMakeLists.txt", ec) && !ec
        && std::filesystem::is_directory("moondeck", ec) && !ec)
        // A subfolder rather than the build tree itself, since rooting it there listed caches and archives beside the few directories a device actually has.
        return std::filesystem::path("build") / "fs";
    std::filesystem::path user = userDataDir();
    return user.empty() ? std::filesystem::path("build") : user;
}

std::filesystem::path fsRoot_{defaultRoot()};


// Map an API path onto the root, normalize it, and reject one that escapes; an empty result reads as failure, which callers already handle.
std::filesystem::path toFsPath(const char* path) {
    if (!path) return {};
    while (*path == '/') path++;  // strip any number of leading slashes
    std::filesystem::path candidate = (fsRoot_ / path).lexically_normal();
    std::filesystem::path rootNormal = fsRoot_.lexically_normal();
    // A prefix check on the normalized string, compared by iterator so a trailing separator cannot change the answer.
    auto [r, c] = std::mismatch(rootNormal.begin(), rootNormal.end(),
                                candidate.begin(), candidate.end());
    if (r != rootNormal.end()) return {};  // candidate diverges before consuming all of rootNormal
    return candidate;
}
}

void fsSetRoot(const char* path) {
    fsRoot_ = (path && *path) ? std::filesystem::path(path) : defaultRoot();
}

const char* fsRootPath() {
    // Refreshed per call rather than cached, so it cannot go stale when the root moves.
    static std::string cached;
    cached = fsRoot_.string();
    return cached.c_str();
}

// Open a file for writing, owner-only where the system has file modes: @xref{config-files-are-written-owner-only|why, and what each platform does}.
static FILE* openOwnerOnly(const char* path) {
#ifdef _WIN32
    return std::fopen(path, "wb");
#else
    ::unlink(path);                       // clear a leftover so O_EXCL cannot fail on our own file
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0600);
    if (fd < 0) return nullptr;
    FILE* f = ::fdopen(fd, "wb");
    if (!f) ::close(fd);                  // fdopen failure leaves the descriptor ours to release
    return f;
#endif
}

bool fsMount() {
    // No volume to mount, but a root that may not exist or may not be writable: establishing it here turns an unusable location into one line at startup.
    std::error_code ec;
    std::filesystem::create_directories(fsRoot_, ec);
    if (!std::filesystem::is_directory(fsRoot_, ec)) return false;
    // Existence does not imply writability, and creating a directory is silent about all three ways it can fail, so probe with the operation that actually matters. Owner-only through the same helper the config writes use, since a file this code creates should not be the loosest thing in the directory.
    const auto probe = fsRoot_ / ".mm-write-probe";
    std::error_code rm;
    std::filesystem::remove(probe, rm);
    FILE* f = openOwnerOnly(probe.string().c_str());
    if (!f) return false;
    std::fclose(f);
    std::filesystem::remove(probe, rm);
    return true;
}

void fsUnmount() {}

bool fsMkdir(const char* path) {
    std::error_code ec;
    std::filesystem::create_directories(toFsPath(path), ec);
    return !ec;
}

bool fsExists(const char* path) {
    std::error_code ec;
    return std::filesystem::exists(toFsPath(path), ec);
}

bool fsRemove(const char* path) {
    std::error_code ec;
    return std::filesystem::remove(toFsPath(path), ec);
}

int fsRead(const char* path, char* buf, size_t maxLen) {
    if (!buf || maxLen == 0) return -1;
    // The path's native character type differs per platform, so go via a string: one allocation per read, which these rare small reads can afford.
    FILE* f = std::fopen(toFsPath(path).string().c_str(), "rb");
    if (!f) return -1;
    size_t n = std::fread(buf, 1, maxLen - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return static_cast<int>(n);
}


bool fsWriteAtomic(const char* path, const char* data, size_t len) {
    auto target = toFsPath(path);
    auto tmp = target;
    tmp += ".tmp";

    FILE* f = openOwnerOnly(tmp.string().c_str());
    if (!f) return false;
    size_t written = std::fwrite(data, 1, len, f);
    if (written != len) {
        std::fclose(f);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::fflush(f);
#ifdef _WIN32
    int fd = ::_fileno(f);
    if (fd >= 0) ::_commit(fd);  // Windows equivalent of fsync
#else
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
#endif
    std::fclose(f);

    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

long fsSize(const char* path) {
    std::error_code ec;
    auto p = toFsPath(path);
    if (!std::filesystem::is_regular_file(p, ec)) return -1;
    const auto sz = std::filesystem::file_size(p, ec);
    return ec ? -1 : static_cast<long>(sz);
}

int fsReadAt(const char* path, long offset, char* buf, size_t len) {
    if (!buf) return -1;
    FILE* f = std::fopen(toFsPath(path).string().c_str(), "rb");
    if (!f) return -1;
    if (std::fseek(f, offset, SEEK_SET) != 0) { std::fclose(f); return -1; }
    const size_t n = std::fread(buf, 1, len, f);
    std::fclose(f);
    return static_cast<int>(n);   // 0 at EOF
}

bool fsWriteStream(const char* path, FsWriteSrc src, void* user) {
    if (!src) return false;
    auto target = toFsPath(path);
    auto tmp = target;
    tmp += ".tmp";

    FILE* f = openOwnerOnly(tmp.string().c_str());
    if (!f) return false;
    // Pull chunks from the source and write each straight through, fixed buffer, any file size. `abort` set by the source (a short/timed-out upload) means the data is incomplete → discard.
    char chunk[1024];
    bool ok = true, abort = false;
    for (;;) {
        const size_t got = src(chunk, sizeof(chunk), user, &abort);
        if (abort) { ok = false; break; }
        if (got == 0) break;                                    // clean end of stream
        if (std::fwrite(chunk, 1, got, f) != got) { ok = false; break; }
    }
    std::fflush(f);
#ifdef _WIN32
    int fd = ::_fileno(f);
    if (fd >= 0) ::_commit(fd);
#else
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
#endif
    std::fclose(f);

    std::error_code ec;
    if (!ok) { std::filesystem::remove(tmp, ec); return false; }
    std::filesystem::rename(tmp, target, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
}

void fsList(const char* dir, FsListCb cb, void* user) {
    if (!cb) return;
    std::error_code ec;
    auto p = toFsPath(dir);
    if (!std::filesystem::exists(p, ec)) return;
    for (auto& entry : std::filesystem::directory_iterator(p, ec)) {
        if (ec) break;
        // The filename's native character type differs per platform, so round-trip through a string for a portable view.
        std::string name = entry.path().filename().string();
        const bool isDir = entry.is_directory(ec);
        std::error_code sizeEc;
        const auto sz = isDir ? 0u : static_cast<uint32_t>(std::filesystem::file_size(entry.path(), sizeEc));
        cb(name.c_str(), isDir, sizeEc ? 0u : sz, user);
    }
}

size_t filesystemUsed() {
    // Sum of file sizes under ./.config/
    std::error_code ec;
    auto root = toFsPath("/.config");
    if (!std::filesystem::exists(root, ec)) return 0;
    size_t total = 0;
    for (auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

size_t filesystemTotal() {
    // Desktop has no fixed quota; report a notional 384 KB to match the 4MB ESP32 partition.
    return 384 * 1024;
}

// Network stubs (desktop has no WiFi/Ethernet hardware)

void setEthConfig(const EthPinConfig&) {}   // no eth on desktop; ethInit stubs false
void ethStop() {}                           // no eth on desktop
bool ethInit() { return false; }

// Raw-frame capture, the host half of the send seam: sending a real frame needs privileges no test should ask for, so the host records what the driver emitted instead. Fixed capacity and no allocation, since an unbounded recorder would turn a long run into unbounded memory; frames past the cap are counted rather than stored.
namespace {
// Sized for the largest frame sequence a test asserts over, and allocated on first capture rather than from boot.
// As a static array it cost a fifth of a megabyte in a shipped binary that may never record a frame.
// Never freed, since freeing would put the allocation back on a path that runs per frame.
constexpr size_t kEthTestMaxFrames = 132;
uint8_t (*ethTestFrames_)[kEthTestFrameMax] = nullptr;
size_t  ethTestLens_[kEthTestMaxFrames] = {};
size_t  ethTestCount_ = 0;
bool    ethTestSendFails_ = false;
uint16_t ethTestLinkSpeed_ = 1000;   // desktop reports gigabit unless a test says otherwise
int      ethRawClaims_ = 0;          // drivers holding the link for direct L2 use
uint32_t ethSendFails_ = 0;          // consecutive ethSendRaw failures (the streak)
uint32_t ethFailTotal_ = 0;          // cumulative since boot; what ethSendFailCounts reports
uint32_t ethRestarts_ = 0;           // ethRestartTx() calls, for the once-per-wedge test
bool     ethRestartFails_ = false;   // simulated recovery failure
// The bound raw socket, or -1 for capture mode (the default, and all any test sees).
int      ethRawFd_ = -1;
unsigned ethRawIfIndex_ = 0;         // Linux AF_PACKET needs the index; BPF binds by name
// The interface the raw sender bound to, so the link queries describe that one rather than whichever the host lists first.
char     ethRawIfName_[64] = {};

#ifdef _WIN32
// Npcap/WinPcap, resolved at RUN TIME rather than linked: @xref{the-raw-frame-driver-is-loaded-not-linked|why}.
struct PcapIf { PcapIf* next; char* name; char* description; /* remaining fields unused */ };
using PcapT = struct pcap;
// The library's send queue, a preformatted block the kernel transmits in one call; the layout must match exactly, so these fields are a binary interface rather than a convenience.
struct PcapSendQueue { unsigned maxlen; unsigned len; char* buffer; };
struct PcapTimeval { long tv_sec; long tv_usec; };            // Windows long is 32-bit
struct PcapPktHdr  { PcapTimeval ts; unsigned caplen; unsigned len; };
using PcapOpenLiveFn    = PcapT* (*)(const char*, int, int, int, char*);
using PcapSendPacketFn  = int (*)(PcapT*, const unsigned char*, int);
using PcapCloseFn       = void (*)(PcapT*);
using PcapFindAllDevsFn = int (*)(PcapIf**, char*);
using PcapFreeAllDevsFn = void (*)(PcapIf*);
using PcapQAllocFn      = PcapSendQueue* (*)(unsigned);
using PcapQQueueFn      = int (*)(PcapSendQueue*, const PcapPktHdr*, const unsigned char*);
using PcapQTransmitFn   = unsigned (*)(PcapT*, PcapSendQueue*, int);
using PcapQDestroyFn    = void (*)(PcapSendQueue*);

HMODULE           wpcapLib_ = nullptr;
PcapOpenLiveFn    pcapOpenLive_ = nullptr;
PcapSendPacketFn  pcapSendPacket_ = nullptr;
PcapCloseFn       pcapClose_ = nullptr;
PcapFindAllDevsFn pcapFindAllDevs_ = nullptr;
PcapFreeAllDevsFn pcapFreeAllDevs_ = nullptr;
PcapQAllocFn      pcapQAlloc_ = nullptr;
PcapQQueueFn      pcapQQueue_ = nullptr;
PcapQTransmitFn   pcapQTransmit_ = nullptr;
PcapQDestroyFn    pcapQDestroy_ = nullptr;
PcapT*            pcapHandle_ = nullptr;   // the open adapter, or null for capture mode
// The batch the send fills and the flush hands over, allocated once at bind time because the send path must not allocate. Null when the library is too old to offer it.
PcapSendQueue*    pcapQueue_ = nullptr;
// Sized for one wall frame with headroom, a one-time allocation on a machine that has just chosen to drive a wall.
constexpr unsigned kSendQueueBytes = 264u * (unsigned)(kEthTestFrameMax + sizeof(PcapPktHdr));
// The adapter identifier the handle belongs to, keyed on the one every device carries rather than the description, which some adapters do not report at all.
char              boundGuid_[40] = {};   // "{8BB7C86E-E3D1-4842-8333-DAD18FD0ADD5}" + NUL

/// Resolve wpcap.dll once. False when Npcap is not installed, which is an ordinary state.
bool wpcapLoad() {
    if (pcapSendPacket_) return true;
    if (!wpcapLib_) wpcapLib_ = ::LoadLibraryA("wpcap.dll");
    if (!wpcapLib_) return false;
    auto sym = [](HMODULE m, const char* n) {
        return reinterpret_cast<void*>(::GetProcAddress(m, n));
    };
    pcapOpenLive_    = reinterpret_cast<PcapOpenLiveFn>(sym(wpcapLib_, "pcap_open_live"));
    pcapSendPacket_  = reinterpret_cast<PcapSendPacketFn>(sym(wpcapLib_, "pcap_sendpacket"));
    pcapClose_       = reinterpret_cast<PcapCloseFn>(sym(wpcapLib_, "pcap_close"));
    pcapFindAllDevs_ = reinterpret_cast<PcapFindAllDevsFn>(sym(wpcapLib_, "pcap_findalldevs"));
    pcapFreeAllDevs_ = reinterpret_cast<PcapFreeAllDevsFn>(sym(wpcapLib_, "pcap_freealldevs"));
    // The batch interface is an optional extension absent from some builds; without it the sends stay one call per packet, which works and merely jitters.
    pcapQAlloc_    = reinterpret_cast<PcapQAllocFn>(sym(wpcapLib_, "pcap_sendqueue_alloc"));
    pcapQQueue_    = reinterpret_cast<PcapQQueueFn>(sym(wpcapLib_, "pcap_sendqueue_queue"));
    pcapQTransmit_ = reinterpret_cast<PcapQTransmitFn>(sym(wpcapLib_, "pcap_sendqueue_transmit"));
    pcapQDestroy_  = reinterpret_cast<PcapQDestroyFn>(sym(wpcapLib_, "pcap_sendqueue_destroy"));
    return pcapOpenLive_ && pcapSendPacket_ && pcapClose_ && pcapFindAllDevs_ && pcapFreeAllDevs_;
}

/// Case-insensitive substring test, so an adapter can be named the way Windows shows it.
bool containsNoCase(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return false;
    for (const char* h = haystack; *h; h++) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) ==
                           std::tolower(static_cast<unsigned char>(*b))) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

/// Case-insensitive equality, for two strings already in the same canonical form.
bool equalsNoCase(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b))) return false;
    }
    return !*a && !*b;
}

/// The `{GUID}` out of a pcap device name (`\Device\NPF_{8BB7C86E-...}`), braces included.
bool guidFromPcapName(const char* name, char* out, size_t cap) {
    if (!name || !out || cap == 0) return false;
    const char* open = std::strchr(name, '{');
    if (!open) return false;
    const char* close = std::strchr(open, '}');
    if (!close) return false;
    const size_t n = static_cast<size_t>(close - open) + 1;
    if (n >= cap) return false;
    std::memcpy(out, open, n);
    out[n] = '\0';
    return true;
}

/// A MIB row's GUID in the spelling pcap uses, so the two can be compared as text.
void guidToString(const GUID& g, char* out, size_t cap) {
    std::snprintf(out, cap, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                  g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

/// The interface-table row behind a capture device, matched on the identifier in its device name: @xref{naming-an-adapter-when-the-capture-library-cannot|why the label is built this way}.
const MIB_IF_ROW2* winRowForPcapName(const MIB_IF_TABLE2* table, const char* pcapName) {
    if (!table) return nullptr;
    char want[40];
    if (!guidFromPcapName(pcapName, want, sizeof(want))) return nullptr;
    for (ULONG i = 0; i < table->NumEntries; i++) {
        char have[40];
        guidToString(table->Table[i].InterfaceGuid, have, sizeof(have));
        if (equalsNoCase(have, want)) return &table->Table[i];
    }
    return nullptr;
}

/// Whether this adapter can carry panel frames, which only physical wired ones can: @xref{which-adapters-can-carry-panel-frames|why the type alone is not the test}.
bool winIsPanelCapableNic(const MIB_IF_ROW2* row) {
    return row && row->Type == IF_TYPE_ETHERNET_CSMACD
        && row->InterfaceAndOperStatusFlags.HardwareInterface;
}

bool winDescForPcapName(const MIB_IF_TABLE2* table, const char* pcapName, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    const MIB_IF_ROW2* row = winRowForPcapName(table, pcapName);
    if (!row) return false;
    // Narrow the wide description as we copy: an adapter description is ASCII.
    size_t n = 0;
    for (; n + 1 < cap && row->Description[n]; n++) {
        out[n] = static_cast<char>(row->Description[n]);
    }
    out[n] = '\0';
    if (n == 0) return false;

    // The same source and unknown-speed guard as the link query, converted here so the shared formatter takes one unit from every platform.
    const unsigned long long bps = row->TransmitLinkSpeed;
    if (bps == 0 || bps == ~0ULL) return true;
    appendLinkSpeed(out, cap, static_cast<unsigned>(bps / 1000000ULL));
    return true;
}
#endif  // _WIN32
}  // namespace

void getMacAddress(uint8_t mac[6]) {
    // A stored identity, generated once and kept beside the config: @xref{the-identity-is-stored-not-read-from-a-nic|why}. Cached per ROOT rather than per process, since the root is settable and a stale cache would describe the wrong install.
    static std::filesystem::path resolvedFor;
    static uint8_t cached[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    if (resolvedFor != fsRoot_) {
        resolvedFor = fsRoot_;
        for (int i = 0; i < 6; i++) cached[i] = 0;
        cached[0] = 0xDE; cached[1] = 0xAD; cached[2] = 0xBE;
        cached[3] = 0xEF; cached[4] = 0xCA; cached[5] = 0xFE;
        std::error_code ec;
        // The live root rather than the default one, so the identity follows the config it belongs to rather than the working directory.
        const std::filesystem::path file = fsRoot_ / ".config" / "identity";
        bool loaded = false;
        if (std::ifstream in(file); in) {
            unsigned b[6] = {};
            if (in >> std::hex >> b[0] >> b[1] >> b[2] >> b[3] >> b[4] >> b[5]) {
                bool sane = true;
                for (int i = 0; i < 6; i++) if (b[i] > 0xFF) sane = false;
                if (sane) { for (int i = 0; i < 6; i++) cached[i] = static_cast<uint8_t>(b[i]); loaded = true; }
            }
        }
        if (!loaded) {
            // No stored identity, so this is a fresh install or one predating the file; they are told apart by whether the tree already holds config. Reading that here is safe because this runs before the config load, so a fresh tree genuinely has none at this instant.
            bool existing = false;
            if (std::filesystem::is_directory(fsRoot_ / ".config", ec) && !ec) {
                for (const auto& e : std::filesystem::directory_iterator(fsRoot_ / ".config", ec)) {
                    if (e.path().extension() == ".json") { existing = true; break; }
                }
            }
            if (!existing) {
                std::random_device rd;
                for (int i = 0; i < 6; i++) cached[i] = static_cast<uint8_t>(rd() & 0xFF);
                cached[0] = static_cast<uint8_t>((cached[0] & 0xFC) | 0x02);   // locally administered, unicast
            }
            std::filesystem::create_directories(file.parent_path(), ec);
            // Atomic, like every other config write: a torn line would be rejected on the next start and the device would silently take a different identity.
            char line[24];
            const int n = std::snprintf(line, sizeof(line), "%02X %02X %02X %02X %02X %02X\n",
                                        cached[0], cached[1], cached[2], cached[3], cached[4], cached[5]);
            if (n > 0) (void)fsWriteAtomic("/.config/identity", line, static_cast<size_t>(n));
            // A write failure is not fatal for this run, since the address above serves. What it costs is persistence: a fresh install on a read-only mount then moves its name on every start.
        }
    }
    for (int i = 0; i < 6; i++) mac[i] = cached[i];
}

// Open a raw socket so a host drives panels for real, the same path a device takes; both systems need privileges, so an ordinary test run stays in capture mode.
bool ethBindRawInterface(const char* ifName) {
#ifdef _WIN32
    // Close any previous handle first, since the interface is a live control and a rebind must not leak the old adapter.
    if (pcapHandle_ && pcapClose_) { pcapClose_(pcapHandle_); }
    pcapHandle_ = nullptr;
    if (pcapQueue_ && pcapQDestroy_) { pcapQDestroy_(pcapQueue_); }
    pcapQueue_ = nullptr;
    boundGuid_[0] = '\0';
    if (!ifName || !ifName[0]) return true;   // explicit return to capture mode, as on POSIX
    if (!wpcapLoad()) return false;           // no Npcap installed: the driver reports it

    // Match the user's string against the device name and both descriptions, first hit wins.
    // A device name far outruns the control's width, so a substring of a description is the only spelling that fits.
    // The extra lookup is not a nicety, since some adapters report no description at all and nothing a user could type would match them.
    PcapIf* devs = nullptr;
    char err[256] = {};
    if (pcapFindAllDevs_(&devs, err) != 0 || !devs) return false;
    MIB_IF_TABLE2* table = nullptr;
    if (::GetIfTable2(&table) != NO_ERROR) table = nullptr;   // fall back to pcap's own text
    const PcapIf* hit = nullptr;
    for (const PcapIf* d = devs; d; d = d->next) {
        if (containsNoCase(d->name, ifName) || containsNoCase(d->description, ifName)) { hit = d; break; }
        char desc[256];
        if (winDescForPcapName(table, d->name, desc, sizeof(desc)) &&
            containsNoCase(desc, ifName)) { hit = d; break; }
    }
    if (table) ::FreeMibTable(table);
    if (!hit) { pcapFreeAllDevs_(devs); return false; }

    // This handle only ever sends, so capture stays non-promiscuous: otherwise it would cost interrupts for frames nothing reads.
    PcapT* h = pcapOpenLive_(hit->name, 65536, 0, 1, err);
    if (h) {
        // Keep the identifier the link query matches on. The description was the old key and fell back to a string no row can match, so a bound adapter reported no link forever.
        guidFromPcapName(hit->name, boundGuid_, sizeof(boundGuid_));
    }
    pcapFreeAllDevs_(devs);
    pcapHandle_ = h;
    // Allocate the batch here, off the hot path, so ethSendRaw only ever fills it.
    if (h && pcapQAlloc_ && pcapQQueue_ && pcapQTransmit_) pcapQueue_ = pcapQAlloc_(kSendQueueBytes);
    return h != nullptr;
#else
    if (ethRawFd_ >= 0) { ::close(ethRawFd_); ethRawFd_ = -1; }
    ethRawIfIndex_ = 0;
    ethRawIfName_[0] = '\0';
    if (!ifName || !ifName[0]) return true;   // explicit return to capture mode

#if defined(__linux__)
    const int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) return false;
    const unsigned idx = if_nametoindex(ifName);
    if (idx == 0) { ::close(fd); return false; }
    ethRawFd_ = fd;
    ethRawIfIndex_ = idx;
    std::snprintf(ethRawIfName_, sizeof(ethRawIfName_), "%s", ifName);
    return true;
#elif defined(__APPLE__)
    // BPF has no single device: open the first free /dev/bpfN, then bind it to the interface.
    for (int i = 0; i < 99; i++) {
        char dev[24];
        std::snprintf(dev, sizeof(dev), "/dev/bpf%d", i);
        const int fd = ::open(dev, O_RDWR);
        if (fd < 0) continue;              // busy or no permission — try the next
        ifreq ifr = {};
        std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifName);
        if (::ioctl(fd, BIOCSETIF, &ifr) < 0) { ::close(fd); return false; }
        // Write whole frames as given: otherwise the layer supplies its own source address over the fixed one the cards filter on, and the frames go out well-formed and ignored.
        unsigned hdrComplete = 1;
        if (::ioctl(fd, BIOCSHDRCMPLT, &hdrComplete) < 0) { ::close(fd); return false; }
        ethRawFd_ = fd;
        std::snprintf(ethRawIfName_, sizeof(ethRawIfName_), "%s", ifName);
        return true;
    }
    return false;
#else
    (void)ifName;
    return false;   // no raw-L2 path on this host OS
#endif
#endif  // _WIN32
}

// Send the frame on the bound interface, or record it when none is bound. The capture branch is what every unit test exercises; the raw branch is what makes a host a panel controller.
bool ethSendRaw(const uint8_t* frame, size_t len) MM_NONBLOCKING {
    if (!frame || len == 0) return false;
    if (ethTestSendFails_) { ethSendFails_++; ethFailTotal_++; return false; }   // simulated link-down / full ring

#ifdef _WIN32
    // A bound adapter sends for real; unbound, which every test is, falls through to the capture ring, so sending changes nothing a test observes.
    // Batched where the queue exists.
    // A wall frame is over a hundred packets that must land inside one window, and one call per packet measured a fivefold spread the cards show as stutter.
    if (pcapHandle_ && pcapQueue_ && pcapQQueue_) {
        PcapPktHdr hdr = {};
        hdr.caplen = static_cast<unsigned>(len);
        hdr.len    = static_cast<unsigned>(len);
        // The streak is not cleared here, since queuing says nothing about reaching the wire. Only the flush knows how many bytes went out, and clearing on enqueue would pin the streak at zero forever.
        if (pcapQQueue_(pcapQueue_, &hdr, frame) == 0) return true;
        // Queue full: flush and retry once, so an unexpectedly large wall degrades to two batches rather than dropping the rest of the frame.
        ethFlushRaw();
        if (pcapQQueue_(pcapQueue_, &hdr, frame) == 0) return true;
        ethSendFails_++; ethFailTotal_++;
        return false;
    }
    if (pcapHandle_ && pcapSendPacket_) {
        const int rc = pcapSendPacket_(pcapHandle_, frame, static_cast<int>(len));
        if (rc != 0) { ethSendFails_++; ethFailTotal_++; return false; }
        ethSendFails_ = 0;
        return true;
    }
#endif

#ifndef _WIN32
    if (ethRawFd_ >= 0) {
#if defined(__linux__)
        sockaddr_ll dst = {};
        dst.sll_family = AF_PACKET;
        dst.sll_ifindex = static_cast<int>(ethRawIfIndex_);
        dst.sll_halen = 6;
        std::memcpy(dst.sll_addr, frame, 6);   // destination MAC is the frame's first 6 bytes
        const ssize_t n = ::sendto(ethRawFd_, frame, len, 0,
                                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
#else
        const ssize_t n = ::write(ethRawFd_, frame, len);
#endif
        // Track failures on the real send path too, since a bound host is where frames actually reach a wire.
        if (n != static_cast<ssize_t>(len)) { ethSendFails_++; ethFailTotal_++; return false; }
        ethSendFails_ = 0;
        return true;
    }
#endif

    if (ethTestCount_ < kEthTestMaxFrames) {
        if (!ethTestFrames_) {
            ethTestFrames_ = static_cast<uint8_t(*)[kEthTestFrameMax]>(
                std::calloc(kEthTestMaxFrames, kEthTestFrameMax));
        }
        if (ethTestFrames_) {
            // Record the true length even when the copy is clipped, so an oversized frame is visible rather than silently short.
            const size_t copy = len < kEthTestFrameMax ? len : kEthTestFrameMax;
            std::memcpy(ethTestFrames_[ethTestCount_], frame, copy);
            ethTestLens_[ethTestCount_] = len;
        }
    }
    ethTestCount_++;
    ethSendFails_ = 0;
    return true;
}

uint32_t ethSendFailStreak() MM_NONBLOCKING { return ethSendFails_; }

// A host socket has no driver link state to refuse against, so every failure is the full-buffer case.
void ethSendFailCounts(uint32_t& linkDown, uint32_t& ringFull) MM_NONBLOCKING {
    linkDown = 0; ringFull = ethFailTotal_;
}

// Nothing to restart here, so clear the streak and let a test exercise the driver's recovery path.
bool ethRestartTx() {
    ethRestarts_++;
    if (ethRestartFails_) return false;
    ethSendFails_ = 0;
    return true;
}

void setTestEthRestartFails(bool fail) { ethRestartFails_ = fail; }

uint32_t ethRestartCountForTest() { return ethRestarts_; }

// Hand the batched burst to the kernel; a no-op wherever the send already gave each frame over, leaving nothing held back.
void ethFlushRaw() MM_NONBLOCKING {
#ifdef _WIN32
    if (!pcapHandle_ || !pcapQueue_ || !pcapQTransmit_ || pcapQueue_->len == 0) return;
    // Transmit at wire speed rather than replaying the queued timestamps, since the card wants the whole burst inside one window.
    const unsigned queued = pcapQueue_->len;
    const unsigned sent = pcapQTransmit_(pcapHandle_, pcapQueue_, 0);
    // The one place that knows the burst left, so it owns both ends of the streak. A short write is the failure, and a complete one the only honest reason to clear it.
    if (sent < queued) { ethSendFails_++; ethFailTotal_++; }
    else               { ethSendFails_ = 0; }
    // Reset for the next frame: the queue is a buffer, and transmit does not rewind it.
    pcapQueue_->len = 0;
#endif
}

void ethClaimRawL2(bool claim) {
    if (claim) ethRawClaims_++;
    else if (ethRawClaims_ > 0) ethRawClaims_--;
}

bool ethRawL2Claimed() MM_NONBLOCKING { return ethRawClaims_ > 0; }

// Link state and negotiated speed, describing the bound adapter where there is one.
// That matters rather than being a nicety: a card with no buffering tears the panel on a slow link while every frame still sends successfully.
// A hardcoded rate would make that warning inert, which is worse than absent. Elsewhere there is no peripheral to describe, so a test chooses what the driver sees.
#ifdef _WIN32
namespace {
/// The link state and speed for the bound adapter, matched through the interface table: @xref{the-interface-table-not-the-address-list|why not the address list}.
bool winAdapterLink(uint16_t& mbps) {
    mbps = 0;
    if (!boundGuid_[0]) return false;
    MIB_IF_TABLE2* table = nullptr;
    if (::GetIfTable2(&table) != NO_ERROR || !table) return false;
    bool up = false;
    for (ULONG i = 0; i < table->NumEntries; i++) {
        const MIB_IF_ROW2& row = table->Table[i];
        char have[40];
        guidToString(row.InterfaceGuid, have, sizeof(have));
        if (!equalsNoCase(have, boundGuid_)) continue;
        up = (row.OperStatus == IfOperStatusUp);
        const unsigned long long bps = row.TransmitLinkSpeed;
        mbps = (bps == 0 || bps == ~0ULL) ? 0
             : static_cast<uint16_t>((bps / 1000000ULL) > 65535ULL ? 65535ULL : (bps / 1000000ULL));
        break;
    }
    ::FreeMibTable(table);
    return up;
}
}  // namespace

bool ethLinkUp() MM_NONBLOCKING { uint16_t m = 0; return winAdapterLink(m); }
bool ethConnected() MM_NONBLOCKING { return ethLinkUp(); }
uint16_t ethLinkSpeedMbps() MM_NONBLOCKING {
    uint16_t m = 0;
    if (winAdapterLink(m) && m) return m;
    return ethTestLinkSpeed_;   // unbound, or a speed Windows would not state
}
#else
namespace {
/// The carrier state and speed for a named interface, the one place either question is asked of the system: @xref{the-link-query-used-to-be-a-stub|why both callers share it}.
bool posixIfLink(const char* ifname, uint16_t& mbps) MM_NONBLOCKING {
    mbps = 0;
    if (!ifname || !*ifname) return false;
#if defined(__linux__)
    // The kernel's own word for the carrier, which a virtual interface leaves unknown; only up counts.
    char path[128];
    std::snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifname);
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char state[16] = {};
    const bool read = std::fscanf(f, "%15s", state) == 1;
    std::fclose(f);
    if (!read || std::strcmp(state, "up") != 0) return false;
    // Speed rides along from the same sysfs tree, in Mbit. Absent or -1 where none is stated.
    std::snprintf(path, sizeof(path), "/sys/class/net/%s/speed", ifname);
    if (FILE* sf = std::fopen(path, "r")) {
        long v = 0;
        if (std::fscanf(sf, "%ld", &v) == 1 && v > 0) mbps = static_cast<uint16_t>(v);
        std::fclose(sf);
    }
    return true;
#elif defined(__APPLE__)
    // IFM_ACTIVE is the carrier bit; the negotiated rate is encoded as the media SUBTYPE.
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    ifmediareq req{};
    // A name too long to have bound is refused rather than truncated, which would describe the wrong adapter.
    if (std::strlen(ifname) >= sizeof(req.ifm_name)) { ::close(fd); return false; }
    std::memcpy(req.ifm_name, ifname, std::strlen(ifname) + 1);
    bool up = false;
    if (::ioctl(fd, SIOCGIFMEDIA, &req) == 0 && (req.ifm_status & IFM_ACTIVE)) {
        up = true;
        switch (IFM_SUBTYPE(req.ifm_active)) {
            case IFM_10_T:   mbps = 10;    break;
            case IFM_100_TX: mbps = 100;   break;
            case IFM_1000_T: mbps = 1000;  break;
            case IFM_2500_T: mbps = 2500;  break;
            case IFM_5000_T: mbps = 5000;  break;
            case IFM_10G_T:  mbps = 10000; break;
            default: break;
        }
    }
    ::close(fd);
    return up;
#else
    (void)ifname;
    return false;   // a host OS with no raw-L2 path: nothing to describe
#endif
}

/// The link state and speed for the interface the raw sender bound to, or nothing when none is: @xref{the-link-query-used-to-be-a-stub|what it used to report}.
bool posixAdapterLink(uint16_t& mbps) MM_NONBLOCKING {
    mbps = 0;
    if (!ethRawIfName_[0]) return false;   // capture mode, or a bind that failed
    return posixIfLink(ethRawIfName_, mbps);
}
}  // namespace

bool ethLinkUp() MM_NONBLOCKING { uint16_t m = 0; return posixAdapterLink(m); }
bool ethConnected() MM_NONBLOCKING { return ethLinkUp(); }
uint16_t ethLinkSpeedMbps() MM_NONBLOCKING {
    uint16_t m = 0;
    if (posixAdapterLink(m)) return m;   // bound and up: the OS's answer, 0 included
    // Only when nothing is bound: a real adapter that states no rate reports none, since inventing one is worse than admitting it is unknown.
    return ethTestLinkSpeed_;
}
#endif

size_t ethTestFrameCount() { return ethTestCount_; }
size_t ethTestFrameLength(size_t i) { return i < kEthTestMaxFrames ? ethTestLens_[i] : 0; }
const uint8_t* ethTestFrameData(size_t i) {
    return (ethTestFrames_ && i < kEthTestMaxFrames) ? ethTestFrames_[i] : nullptr;
}
void ethTestClearFrames() { ethTestCount_ = 0; ethSendFails_ = 0; ethFailTotal_ = 0; }
void setTestEthSendFails(bool fail) { ethTestSendFails_ = fail; }
void setTestEthLinkSpeed(uint16_t mbps) { ethTestLinkSpeed_ = mbps; }
void ethGetIPv4(uint8_t out[4]) MM_NONBLOCKING {
    // No real interface state, but the discovery module needs this host's address to scan from, so the outbound-route answer is reported as the wired one it reads first.
    out[0] = out[1] = out[2] = out[3] = 0;
    const char* ip = hostIp();
    if (ip && ip[0]) {
        // Parse the address with the system call already used here, since this layer does not include the core's parser.
        in_addr a{};
        if (inet_pton(AF_INET, ip, &a) == 1) {
            uint32_t n = a.s_addr;   // network byte order: octet 0 is the low byte
            out[0] = static_cast<uint8_t>(n & 0xff);
            out[1] = static_cast<uint8_t>((n >> 8) & 0xff);
            out[2] = static_cast<uint8_t>((n >> 16) & 0xff);
            out[3] = static_cast<uint8_t>((n >> 24) & 0xff);
        }
    }
}

// Test seam: no radio here, so the init reports none unless a test fakes one to drive the waiting path.
static std::atomic<bool> testWifiStaAvailable{false};
void setTestWifiStaAvailable(bool available) { testWifiStaAvailable.store(available, std::memory_order_relaxed); }
bool wifiStaInit(const char* /*ssid*/, const char* /*password*/) {
    return testWifiStaAvailable.load(std::memory_order_relaxed);
}
bool wifiStaConnected() MM_NONBLOCKING { return false; }
void wifiStaGetIPv4(uint8_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }
// Addressing is managed by the system here, so the setters are inert; the per-interface counter is what a host test pins the path on.
static std::atomic<uint32_t> testStaticApplies[2] = {};   // indexed by NetIface
void netSetStaticIPv4(NetIface iface, const uint8_t[4], const uint8_t[4],
                      const uint8_t[4], const uint8_t[4]) {
    testStaticApplies[static_cast<uint8_t>(iface)].fetch_add(1, std::memory_order_relaxed);
}
uint32_t testNetStaticApplyCount(NetIface iface) {
    return testStaticApplies[static_cast<uint8_t>(iface)].load(std::memory_order_relaxed);
}
void netSetDhcp(NetIface /*iface*/) {}
void setHostname(const char* /*name*/) {}   // no DHCP client on desktop
void wifiStaStop() {}
int wifiStaRssi() { return 0; }
void wifiStaBssid(uint8_t out[6]) { std::memset(out, 0, 6); }
int wifiStaChannel() { return 0; }

bool wifiApInit(const char* /*apName*/, const char* /*ip*/) { return false; }   // no AP on a host
bool wifiApConnected() { return false; }
void wifiApStop() {}
uint32_t wifiApClientCount() { return 0; }

// Host sockets work whatever the link predicates above say, and there is no initialization race, so this is always safe.
bool networkReady() { return true; }
int wifiTxPower() { return 0; }
// Zero is a successful no-op and anything else fails, there being no radio. The module passes its no-override sentinel through here to lift a prior cap, which is trivially true with no radio.
bool wifiSetTxPower(int8_t quarterDbm) { return quarterDbm == 0; }

bool mdnsInit(const char* /*deviceName*/) { return false; }
void mdnsStop() {}
void mdnsShutdown() {}
// Advertising is a device concern, so these are stubs; discovery itself is datagram presence and runs here too, testable over real loopback.

// No update partition here, and the route guards on the capability, so this stub exists for compile coverage only.
bool http_fetch_to_ota(const char* /*url*/,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut,
                       const char* /*fallbackUrl*/) {
    if (statusBuf && statusBufLen > 0) {
        std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    }
    if (bytesReadOut) *bytesReadOut = 0;
    if (bytesTotalOut) *bytesTotalOut = 0;
    return false;
}

bool otaWriteStream(FsWriteSrc /*src*/, void* /*user*/, size_t /*contentLen*/,
                    char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut) {
    // No OTA partition on desktop, call sites guard with `if constexpr (mm::platform::hasOta)`.
    if (statusBuf && statusBufLen > 0) std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    if (bytesReadOut) *bytesReadOut = 0;
    return false;
}

// No partitions on desktop: there is no recovery image and nothing to boot into.
bool otaHasMoonBase() { return false; }
bool otaBootMoonBase() { return false; }
bool otaRunningMoonBase() { return false; }
// No factory partition off-device, so nothing to read a version from.
bool otaMoonBaseVersion(char*, size_t) { return false; }
bool otaMoonBaseBuild(char*, size_t) { return false; }
bool otaMoonBaseSize(uint32_t*, uint32_t*) { return false; }
// No factory partition to install into off-device.
bool otaFetchMoonBaseUrl(const char*, char* statusBuf, size_t statusBufLen,
                         uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    if (statusBuf && statusBufLen > 0) std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    if (bytesReadOut) *bytesReadOut = 0;
    if (bytesTotalOut) *bytesTotalOut = 0;
    return false;
}

// No recovery partition here, so this installs nothing and consumes nothing.
// An earlier version read the caller's first chunk for a check whose real coverage is a unit test driving the vetting directly.
// Refusing without touching the source is the honest stub.
bool otaWriteMoonBase(FsWriteSrc, void*, size_t, char* statusBuf, size_t statusBufLen,
                      uint32_t* bytesReadOut) {
    if (statusBuf && statusBufLen > 0) std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    if (bytesReadOut) *bytesReadOut = 0;
    return false;
}

bool moonbaseStageInstallUrl(const char*) { return false; }
void moonbaseClearStagedUrl() {}

// An outbound request over the local network, blocking and bounded by a timeout: it builds into a stack buffer, sends, and returns the status and body.
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

    int fd = open_sock(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct CloseGuard { int f; ~CloseGuard() { close_sock(f); } } guard{fd};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return 0;

    // Bound the connect too, since a blocking one to an unreachable host hangs for tens of seconds. This shares a thread with the render loop, so it connects without blocking and waits for writability.
    if (make_nonblocking(fd) != 0) return 0;
    int cr = ::connect(sock(fd), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    // A connect that did not complete at once reports as in progress, under a different name per platform; anything else is a hard failure.
#ifdef _WIN32
    const bool inProgress = (cr != 0 && ::WSAGetLastError() == WSAEWOULDBLOCK);
#else
    const bool inProgress = (cr != 0 && errno == EINPROGRESS);
#endif
    if (cr != 0 && !inProgress) return 0;          // immediate hard failure
    if (cr != 0) {                                 // connect in progress — wait for writable
        fd_set wf; FD_ZERO(&wf); FD_SET(sock(fd), &wf);
        const uint32_t cms = remainingMs();
        timeval ctv{};
        ctv.tv_sec = static_cast<time_t>(cms / 1000);
        // Take the field's own type rather than a named one, which does not exist on every platform.
        ctv.tv_usec = static_cast<decltype(ctv.tv_usec)>((cms % 1000) * 1000);
        if (::select(static_cast<int>(sock(fd)) + 1, nullptr, &wf, nullptr, &ctv) <= 0) return 0;  // timeout / error
        int soerr = 0; socklen_t len = sizeof(soerr);
        ::getsockopt(sock(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
        if (soerr != 0) return 0;                  // connect failed
    }
    if (make_blocking(fd) != 0) return 0;          // back to blocking for the bounded send/recv

    // Bound the send and the read with the time left on the shared deadline, so every phase together stays within the caller's budget.
    const uint32_t sms = remainingMs();
#ifdef _WIN32
    DWORD tv = sms;
    ::setsockopt(sock(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(sock(fd), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(sms / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((sms % 1000) * 1000);
    ::setsockopt(sock(fd), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock(fd), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

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
    // Send the whole request, looping because a blocking send can return short under backpressure.
    for (int off = 0; off < n;) {
        auto w = ::send(sock(fd), req + off, n - off, 0);
        if (w > 0) off += static_cast<int>(w);
        else return 0;
    }

    // Read into the caller's buffer when they want the body, so they size it, and into a small local one otherwise, just far enough for the status line.
    char scratch[256];
    char* buf = body ? body : scratch;
    const size_t cap = body ? bodyLen : sizeof(scratch);
    if (cap < 16) return 0;
    int total = 0;
    while (total < static_cast<int>(cap - 1)) {
        auto r = ::recv(sock(fd), buf + total, cap - 1 - total, 0);
        if (r > 0) total += static_cast<int>(r);
        else break;   // closed or timeout
    }
    buf[total] = '\0';
    if (total < 12 || std::strncmp(buf, "HTTP/1.", 7) != 0) { if (body) body[0] = '\0'; return 0; }
    int status = std::atoi(buf + 9);   // "HTTP/1.1 NNN ..."
    if (body) {
        char* b = std::strstr(body, "\r\n\r\n");
        if (b) std::memmove(body, b + 4, std::strlen(b + 4) + 1);   // drop headers, keep just the body
        else body[0] = '\0';
    }
    return status;
}


// No serial provisioning path here, and the module gates on the capability, so this stub exists for compile coverage.
bool improvProvisioningInit(const ImprovDeviceInfo& /*info*/,
                            char* /*ssidOut*/, size_t /*ssidOutLen*/,
                            char* /*passwordOut*/, size_t /*passwordOutLen*/,
                            std::atomic<bool>* /*ready*/,
                            char* statusBuf, size_t statusBufLen,
                            uint8_t* /*txPowerOut*/,
                            std::atomic<bool>* /*txPowerReady*/,
                            char* /*opOut*/, size_t /*opOutLen*/,
                            std::atomic<bool>* /*opReady*/) {
    if (statusBuf && statusBufLen > 0) {
        std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    }
    return false;
}

void reboot() {
    // The device is the host process, so exit cleanly and let the supervisor restart it, which matches what the browser's reconnect expects.
    std::printf("platform::reboot() — exiting\n");
    std::fflush(stdout);
    // Exiting is the reboot here, there being no firmware to restart into; the thread-safety warning describes exactly the abrupt teardown a reboot models. NOLINTNEXTLINE(concurrency-mt-unsafe)
    std::exit(0);
}

// UdpSocket

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open() {
    if (fd_ >= 0) return true;
    fd_ = open_sock(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    // Allow broadcast sends, which the system otherwise refuses; it has no effect on the other kinds.
    const int on = 1;
    ::setsockopt(sock(fd_), SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&on), sizeof(on));
    return true;
}

bool UdpSocket::connect(const char* ip, uint16_t port) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return false;
    return ::connect(sock(fd_), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool UdpSocket::sendTo(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    return ::send(sock(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0) >= 0;
}

// Test override forcing a bind to fail, since relying on the system to refuse a port is not portable.
static std::atomic<bool> testBindFails{false};
void setTestBindFails(bool fail) { testBindFails.store(fail, std::memory_order_relaxed); }

bool UdpSocket::bind(uint16_t port) {
    if (fd_ < 0) return false;
    if (testBindFails.load(std::memory_order_relaxed)) return false;
    // The address-reuse option means opposite things per platform: @xref{address-reuse-means-opposite-things|the split, and what a test must do instead}.
#ifndef _WIN32
    int reuse = 1;
    ::setsockopt(sock(fd_), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(sock(fd_), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    // Non-blocking so the render loop's drain never stalls waiting for a packet.
    return make_nonblocking(fd_) == 0;
}

int UdpSocket::recvFrom(uint8_t* buf, size_t maxLen, uint8_t srcIp[4]) {
    if (fd_ < 0) return -1;
    sockaddr_in src{};
    socklen_t srcLen = sizeof(src);
    auto n = ::recvfrom(sock(fd_), reinterpret_cast<char*>(buf), static_cast<int>(maxLen), 0,
                        reinterpret_cast<sockaddr*>(&src), &srcLen);
    // 0-byte datagrams and would-block both mean "nothing usable pending".
    if (n <= 0) return -1;
    if (srcIp) std::memcpy(srcIp, &src.sin_addr.s_addr, 4);   // network order = octets
    return static_cast<int>(n);
}

// Join a multicast group so the bound socket receives its datagrams; letting the stack pick the interface is what a single-homed device wants.
bool UdpSocket::joinMulticast(const char* group) {
    if (fd_ < 0 || !group) return false;
    ip_mreq mreq{};
    if (::inet_pton(AF_INET, group, &mreq.imr_multiaddr) != 1) return false;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    return ::setsockopt(sock(fd_), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                        reinterpret_cast<const char*>(&mreq), sizeof(mreq)) == 0;
}

bool UdpSocket::sendToAddr(const uint8_t ip[4], uint16_t port,
                           const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::memcpy(&addr.sin_addr.s_addr, ip, 4);
    return ::sendto(sock(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                    reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) >= 0;
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        close_sock(fd_);
        fd_ = -1;
    }
}

// TcpConnection

TcpConnection::~TcpConnection() {
    close();
}

int TcpConnection::read(uint8_t* buf, size_t maxLen) {
    if (fd_ < 0) return -1;
    // The read behaves the same on both platforms, and each one's would-block result is translated to the same value for the caller.
    auto n = ::recv(sock(fd_), reinterpret_cast<char*>(buf), static_cast<int>(maxLen), 0);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0; // peer closed
    if (sockWouldBlock()) return -1; // read timed out, nothing available
    return 0; // error → treat as closed
}

// getpeername rather than a field captured at accept: an earlier copy outlives a reconnect.
bool TcpConnection::peerIPv4(uint8_t out[4]) const {
    if (fd_ < 0 || !out) return false;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
#ifdef _WIN32
    if (::getpeername(sock(fd_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
#else
    if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
#endif
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
    // Send every byte, since a response must arrive complete, bounded because this runs on the render thread and a stalled peer would otherwise block it forever. Two bounds, as on a device: progress resets the stall one so a slow but steady transfer finishes, while the total one keeps a trickling peer from holding the loop.
    constexpr uint32_t kWriteStallMs = 2000;
    constexpr uint32_t kWriteTotalMs = 8000;
    const uint32_t start = millis();
    uint32_t lastProgress = start;
    size_t sent = 0;
    while (sent < len) {
        auto n = ::send(sock(fd_), reinterpret_cast<const char*>(data + sent),
                        static_cast<int>(len - sent), 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            lastProgress = millis();
        } else if (sockWouldBlock()) {
            const uint32_t now = millis();
            if (now - lastProgress >= kWriteStallMs || now - start >= kWriteTotalMs)
                return false;   // stalled or crawling peer: close it, never hang the loop
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
#ifndef _WIN32
        } else if (errno == EINTR) {
            continue; // interrupted by signal, retry
#endif
        } else {
            return false;
        }
    }
    return true;
}

int TcpConnection::writeSome(const uint8_t* data, size_t len) {
    if (fd_ < 0) return -1;
    if (len == 0) return 0;
    // The accepted socket is permanently non-blocking, so a full send buffer surfaces as would-block and is reported as nothing sent; the caller advances its own offset.
    auto n = ::send(sock(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (sockWouldBlock()) return 0;         // buffer full — try later
#ifndef _WIN32
    if (errno == EINTR) return 0;           // interrupted — try later
#endif
    return -1;                              // real socket error
}


bool TcpConnection::connectStart(const char* host, uint16_t port) {
    if (!host || !host[0]) return false;
    close();

    // One bounded name lookup up front, the single unavoidable blocking step; the connect itself then proceeds without blocking.
    char portStr[6];
    std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host, portStr, &hints, &res) != 0 || !res) return false;
    struct AiGuard { addrinfo* p; ~AiGuard() { if (p) ::freeaddrinfo(p); } } aiGuard{res};

    int fd = open_sock(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) return false;
    if (make_nonblocking(fd) != 0) { close_sock(fd); return false; }
    int cr = ::connect(sock(fd), res->ai_addr, static_cast<int>(res->ai_addrlen));
#ifdef _WIN32
    const bool inProgress = (cr != 0 && ::WSAGetLastError() == WSAEWOULDBLOCK);
#else
    const bool inProgress = (cr != 0 && errno == EINPROGRESS);
#endif
    if (cr != 0 && !inProgress) { close_sock(fd); return false; }   // immediate hard failure
    fd_ = fd;   // in flight (or already connected) — connectPoll() resolves which
    return true;
}

TcpConnection::ConnectResult TcpConnection::connectPoll() {
    if (fd_ < 0) return ConnectResult::Failed;
    // A zero-timeout poll that never blocks, watching both writability and the exception set: a refused connect signals only through the latter on one platform.
    fd_set wf; FD_ZERO(&wf); FD_SET(sock(fd_), &wf);
    fd_set ef; FD_ZERO(&ef); FD_SET(sock(fd_), &ef);
    timeval zero{};   // 0s / 0us
    const int r = ::select(static_cast<int>(sock(fd_)) + 1, nullptr, &wf, &ef, &zero);
    if (r == 0) return ConnectResult::Pending;                       // neither writable nor errored yet
    if (r < 0)  { close(); return ConnectResult::Failed; }
    // SO_ERROR distinguishes a real connect from an errored one on both platforms.
    int soerr = 0; socklen_t len = sizeof(soerr);
    ::getsockopt(sock(fd_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
    if (soerr != 0) { close(); return ConnectResult::Failed; }
    return ConnectResult::Connected;                                 // socket stays non-blocking
}

void TcpConnection::close() {
    if (fd_ >= 0) {
        close_sock(fd_);
        fd_ = -1;
    }
}

// TcpServer

TcpServer::~TcpServer() {
    close();
}

bool TcpServer::open(uint16_t port) {
    if (fd_ >= 0) return true;
    fd_ = open_sock(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    int opt = 1;
    setsockopt(sock(fd_), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(sock(fd_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_sock(fd_);
        fd_ = -1;
        return false;
    }

    if (::listen(sock(fd_), 8) < 0) {
        close_sock(fd_);
        fd_ = -1;
        return false;
    }

    make_nonblocking(fd_);

    return true;
}

TcpConnection TcpServer::accept() {
    if (fd_ < 0) return TcpConnection();
#ifdef _WIN32
    SOCKET client = ::accept(sock(fd_), nullptr, nullptr);
    if (client == INVALID_SOCKET) return TcpConnection();
    int clientFd = static_cast<int>(client);
    // Non-blocking, since a blocking read on the single-loop server stalls the whole render loop.
    make_nonblocking(clientFd);
#else
    int clientFd = ::accept(fd_, nullptr, nullptr);
    if (clientFd < 0) return TcpConnection();
    // A non-blocking client socket, the server running from the single render loop.
    // A blocking read's timeout froze the whole loop whenever a request's bytes had not landed the instant it was accepted.
    // The request lands within about a millisecond and is read across a few rapid retries instead.
    make_nonblocking(clientFd);
#endif
    return TcpConnection(clientFd);
}

void TcpServer::close() {
    if (fd_ >= 0) {
        close_sock(fd_);
        fd_ = -1;
    }
}

// The symbol-based output on the host: accepted and counted rather than refused, since refusing made that driver inert off device. There is nothing to hand back, the driver owning the symbols, and the resolution is echoed so its timing arithmetic works on real numbers.
namespace {
struct HostRmt { uint32_t resolutionHz = 0; };
HostRmt* hostRmt(void*& impl) {
    if (!impl) impl = new HostRmt();
    return static_cast<HostRmt*>(impl);
}
}  // namespace

bool rmtWs2812Init(RmtWs2812Handle& h, uint8_t /*gpio*/, uint32_t resolutionHz,
                   bool /*invert*/) {
    // A zero resolution would make the driver divide by zero, so refuse it rather than hand back an unusable channel.
    if (resolutionHz == 0) return false;
    hostRmt(h.impl)->resolutionHz = resolutionHz;
    return true;
}
uint32_t rmtWs2812Resolution(const RmtWs2812Handle& h) MM_NONBLOCKING {
    return h.impl ? static_cast<HostRmt*>(h.impl)->resolutionHz : 0;
}
bool rmtWs2812Transmit(RmtWs2812Handle& h, const uint8_t* wire, size_t byteCount) {
    if (!h.impl || !wire || byteCount == 0) return false;
    return true;
}

bool rmtWs2812SetBitTiming(RmtWs2812Handle& h, uint32_t /*sym0*/, uint32_t /*sym1*/) {
    return h.impl != nullptr;   // no peripheral to program off-target
}
bool rmtWs2812Wait(RmtWs2812Handle& /*h*/, uint32_t /*timeoutMs*/) { return true; }
void rmtWs2812Deinit(RmtWs2812Handle& h) {
    delete static_cast<HostRmt*>(h.impl);
    h.impl = nullptr;
}
size_t rmtWs2812RxCapture(uint8_t /*gpio*/, uint32_t /*resolutionHz*/,
                          uint32_t* /*outSymbols*/, size_t /*maxSymbols*/,
                          uint32_t /*timeoutMs*/) {
    return 0;
}
RmtLoopbackResult rmtWs2812Loopback(uint8_t /*txGpio*/, uint8_t /*rxGpio*/) {
    return {};   // not supported off ESP32
}
RmtLoopbackResult rmtWs2812LoopbackFrame(uint8_t /*txGpio*/, uint8_t /*rxGpio*/,
                                         uint16_t /*lights*/, uint8_t /*channels*/) {
    return {};   // not supported off ESP32
}
RmtLoopbackResult ws2812LoopbackRide(uint16_t /*rxGpio*/, const uint8_t* /*sent*/, uint8_t /*sentLen*/,
                                     size_t /*dataBytes*/, uint8_t /*rowBits*/,
                                     uint8_t /*clockMultiplier*/) {
    return {};   // no RMT-RX capture off ESP32
}

// Parallel WS2812 buses on desktop, backed by real memory: @xref{a-host-bus-is-real-memory-not-a-refusal|what is and is not modelled}.
namespace {

/// One memory-backed parallel bus, shared by every parallel seam below: three peripherals for the same job, and off device that job is holding a frame.
struct HostBus {
    std::vector<uint8_t> buf[2];
    size_t capacity = 0;

    bool init(size_t bytes, bool wantSecond) {
        if (bytes == 0) return false;
        capacity = bytes;
        buf[0].assign(bytes, 0);
        if (wantSecond) buf[1].assign(bytes, 0);
        else            buf[1].clear();
        return true;
    }
    uint8_t* buffer(uint8_t i) {
        if (i > 1 || buf[i].empty()) return nullptr;
        return buf[i].data();
    }
    bool transmit(uint8_t i, size_t bytes) {
        if (i > 1 || buf[i].empty() || bytes > capacity) return false;
        return true;
    }
};

HostBus* hostBus(void*& impl) {
    if (!impl) impl = new HostBus();
    return static_cast<HostBus*>(impl);
}
void freeHostBus(void*& impl) {
    delete static_cast<HostBus*>(impl);
    impl = nullptr;
}

}  // namespace

const char* i80Ws2812LastError() { return nullptr; }   // the emulated bus never refuses for a cause
bool i80Ws2812SharedBusFree() { return false; }        // and shares no peripheral, so never retries
bool i80Ws2812Init(I80Ws2812Handle& h, const uint16_t* /*dataPins*/,
                   uint8_t /*laneCount*/, uint16_t /*wrGpio*/, uint16_t /*dcGpio*/,
                   size_t bufferBytes, bool wantSecondBuffer,
                   uint8_t /*clockMultiplier*/) {
    if (bufferBytes == 0) return false;   // refuse before allocating, as the RMT seam does
    return hostBus(h.impl)->init(bufferBytes, wantSecondBuffer);
}
uint8_t* i80Ws2812Buffer(const I80Ws2812Handle& h, uint8_t buffer) {
    return h.impl ? static_cast<HostBus*>(h.impl)->buffer(buffer) : nullptr;
}
size_t i80Ws2812BufferCapacity(const I80Ws2812Handle& h) {
    return h.impl ? static_cast<HostBus*>(h.impl)->capacity : 0;
}
bool i80Ws2812Transmit(I80Ws2812Handle& h, uint8_t buffer, size_t bytes) {
    return h.impl && static_cast<HostBus*>(h.impl)->transmit(buffer, bytes);
}
// True rather than false: the driver reads false as an incomplete previous frame and holds the next back, stalling a bus that is never busy.
bool i80Ws2812Wait(I80Ws2812Handle& /*h*/, uint8_t /*buffer*/, uint32_t /*timeoutMs*/) { return true; }
uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle& /*h*/) { return 0; }
void i80Ws2812Deinit(I80Ws2812Handle& h) { freeHostBus(h.impl); }
RmtLoopbackResult i80Ws2812Loopback(const uint16_t* /*dataPins*/, uint8_t /*laneCount*/,
                                    uint16_t /*wrGpio*/, uint16_t /*dcGpio*/,
                                    uint16_t /*rxGpio*/, const uint8_t* /*frame*/,
                                    size_t /*frameBytes*/, size_t /*dataBytes*/,
                                    uint8_t /*rowBits*/, uint8_t /*clockMultiplier*/) {
    return {};   // not supported off the S3
}

// Our own driver, on the same memory-backed bus as the family above; the ring path stays inert, so a driver that would stream on device runs whole-frame here.
bool moonI80Ws2812Init(MoonI80Ws2812Handle& h, const uint16_t* /*dataPins*/,
                       uint8_t /*laneCount*/, uint16_t /*wrGpio*/,
                       size_t bufferBytes, bool wantSecondBuffer,
                       uint8_t /*clockMultiplier*/) {
    if (bufferBytes == 0) return false;   // refuse before allocating, as the other seams do
    return hostBus(h.impl)->init(bufferBytes, wantSecondBuffer);
}
// Ring mode has no host equivalent, so it stays inert and a driver that would pick it on device runs whole-frame here.
bool moonI80Ws2812InitRing(MoonI80Ws2812Handle& /*h*/, const uint16_t* /*dataPins*/,
                           uint8_t /*laneCount*/, uint16_t /*wrGpio*/, size_t /*rowBytes*/,
                           uint32_t /*totalRows*/, uint32_t /*rowsPerBuf*/, uint8_t /*ringBufs*/,
                           uint8_t /*padUs*/, uint8_t /*clockMultiplier*/, MoonI80EncodeFn /*encode*/,
                           void* /*user*/) {
    return false;
}
bool moonI80Ws2812TransmitRing(MoonI80Ws2812Handle& /*h*/) { return false; }
void moonI80SetShiftClockDiv(uint8_t /*div*/) {}
void moonI80Ws2812PrimeRange(MoonI80Ws2812Handle& /*h*/, uint8_t /*bufLo*/, uint8_t /*bufHi*/) {}
bool moonI80Ws2812ArmRing(MoonI80Ws2812Handle& /*h*/) { return false; }
bool moonI80Ws2812IsRing(const MoonI80Ws2812Handle& /*h*/) { return false; }
bool moonI80Ws2812InternalFits(size_t /*bytes*/) { return false; }
uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle& h, uint8_t buffer) {
    return h.impl ? static_cast<HostBus*>(h.impl)->buffer(buffer) : nullptr;
}
size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle& h) {
    return h.impl ? static_cast<HostBus*>(h.impl)->capacity : 0;
}
bool moonI80Ws2812Transmit(MoonI80Ws2812Handle& h, uint8_t buffer, size_t bytes) {
    return h.impl && static_cast<HostBus*>(h.impl)->transmit(buffer, bytes);
}
bool moonI80Ws2812Wait(MoonI80Ws2812Handle& /*h*/, uint8_t /*buffer*/, uint32_t /*timeoutMs*/) { return true; }
uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle& /*h*/) { return 0; }
MoonI80RingStats moonI80Ws2812RingStats(const MoonI80Ws2812Handle& /*h*/) { return {}; }
void moonI80Ws2812Deinit(MoonI80Ws2812Handle& h) { freeHostBus(h.impl); }
RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t* /*dataPins*/, uint8_t /*laneCount*/,
                                        uint16_t /*wrGpio*/,
                                        uint16_t /*rxGpio*/, const uint8_t* /*frame*/,
                                        size_t /*frameBytes*/, size_t /*dataBytes*/,
                                        uint8_t /*rowBits*/, uint8_t /*clockMultiplier*/,
                                        uint32_t /*ringRows*/, uint32_t /*ringBufs*/,
                                        bool /*useRing*/) {
    return {};   // not supported off LCD_CAM
}
RmtLoopbackResult moonI80Ws2812LoopbackRide(uint16_t /*rxGpio*/, const uint8_t* /*sent*/,
                                            uint8_t /*sentLen*/, size_t /*dataBytes*/,
                                            uint8_t /*rowBits*/, uint8_t /*clockMultiplier*/) {
    return {};   // not supported off LCD_CAM
}

// The same memory-backed bus again: no silicon here, but the driver runs and its sizing is pinned by tests.
bool parlioWs2812Init(ParlioWs2812Handle& h, const uint16_t* /*dataPins*/,
                      uint8_t /*laneCount*/, uint32_t /*pclkHz*/, size_t bufferBytes,
                      bool wantSecondBuffer) {
    if (bufferBytes == 0) return false;   // refuse before allocating, as the other seams do
    return hostBus(h.impl)->init(bufferBytes, wantSecondBuffer);
}
uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle& h, uint8_t buffer) {
    return h.impl ? static_cast<HostBus*>(h.impl)->buffer(buffer) : nullptr;
}
size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle& h) {
    return h.impl ? static_cast<HostBus*>(h.impl)->capacity : 0;
}
// The bus is ordinary memory here, so there is no transfer ceiling to declare and zero is the contract for no bound.
size_t parlioMaxTransferBytes() { return 0; }
bool parlioWs2812Transmit(ParlioWs2812Handle& h, uint8_t buffer, size_t bytes) {
    return h.impl && static_cast<HostBus*>(h.impl)->transmit(buffer, bytes);
}
bool parlioWs2812Wait(ParlioWs2812Handle& /*h*/, uint8_t /*buffer*/, uint32_t /*timeoutMs*/) { return true; }
uint32_t parlioWs2812LastTransmitUs(const ParlioWs2812Handle& /*h*/) { return 0; }
void parlioWs2812Deinit(ParlioWs2812Handle& h) { freeHostBus(h.impl); }

// No panel on a desktop, and inert rather than emulated: this port has no analogue worth faking, and its encoder is already tested on plain buffers. So the init refuses with a cause and the driver reports it as it would on a chip without the silicon.
const char* hub75LastError() { return "HUB75 needs an ESP32-S3, P4 or S31"; }
bool hub75BackendAvailable(Hub75Backend /*backend*/, size_t /*frameBytes*/) { return false; }
const char* hub75BackendLabel(Hub75Backend backend) {
    return backend == Hub75Backend::Parlio ? "Parlio" : "LCD_CAM";
}
bool hub75Init(Hub75Handle& /*h*/, Hub75Backend /*backend*/, const Hub75Pins& /*pins*/,
               uint16_t /*width*/, uint16_t /*height*/, uint8_t /*scanRate*/,
               uint8_t /*bitDepth*/) {
    return false;
}
uint8_t* hub75Buffer(const Hub75Handle& /*h*/) MM_NONBLOCKING { return nullptr; }
size_t hub75BufferCapacity(const Hub75Handle& /*h*/) MM_NONBLOCKING { return 0; }
bool hub75Start(Hub75Handle& /*h*/) { return false; }
uint16_t hub75RefreshHz(const Hub75Handle& /*h*/) MM_NONBLOCKING { return 0; }
const char* hub75Backend(const Hub75Handle& /*h*/) { return nullptr; }
void hub75Deinit(Hub75Handle& /*h*/) {}
RmtLoopbackResult parlioWs2812Loopback(const uint16_t* /*dataPins*/, uint8_t /*laneCount*/,
                                       uint16_t /*rxGpio*/, const uint8_t* /*frame*/,
                                       size_t /*frameBytes*/, size_t /*dataBytes*/,
                                       uint8_t /*rowBits*/) {
    return {};   // not supported off the P4
}

// The codec and capture live in their own file: the codec succeeds with nothing to bring up, and the microphone reads the system capture device.

// USB video capture: no USB host on desktop, so init fails and VideoService's usb source reports "no capture device" while its other sources keep working.
bool videoCaptureInit(VideoCaptureHandle& /*h*/, uint16_t /*width*/, uint16_t /*height*/,
                      uint8_t /*fps*/) {
    return false;
}
size_t videoCaptureFormats(VideoCaptureFormat* /*out*/, size_t /*max*/) { return 0; }
uint32_t videoCaptureFormatGeneration() { return 0; }
const uint8_t* videoCaptureFrame(VideoCaptureHandle& /*h*/, uint16_t& /*width*/,
                                 uint16_t& /*height*/) MM_NONBLOCKING {
    return nullptr;
}
void videoCaptureDeinit(VideoCaptureHandle& /*h*/) {}
VideoCaptureStats videoCaptureStats() { return {}; }

// The textbook in-place radix-2 transform, the production kernel now that live capture runs blocks dozens of times a second on the render tick. The contract is unchanged and it is numerically equivalent to the direct form, pinned against one by a test.
void audioFft(const float* windowed, size_t n, float* outMag) {
    if (!windowed || !outMag || n == 0 || (n & (n - 1)) != 0) return;
    constexpr size_t kMaxN = 4096;
    if (n > kMaxN) return;
    static float re[kMaxN], im[kMaxN];   // scratch; render-thread only, like the ESP32 kernel's

    // Bit-reversal permutation while loading the input.
    const int bits = static_cast<int>(std::countr_zero(n));
    for (size_t i = 0; i < n; i++) {
        size_t r = 0;
        for (int b = 0; b < bits; b++) r |= ((i >> b) & 1u) << (bits - 1 - b);
        re[r] = windowed[i];
        im[r] = 0.0f;
    }

    // Butterflies: stages of doubling span, twiddles advanced per group.
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
        const float wRe = std::cos(ang), wIm = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            float curRe = 1.0f, curIm = 0.0f;
            for (size_t j = 0; j < len / 2; j++) {
                const size_t a = i + j, b = a + len / 2;
                const float tRe = re[b] * curRe - im[b] * curIm;
                const float tIm = re[b] * curIm + im[b] * curRe;
                re[b] = re[a] - tRe; im[b] = im[a] - tIm;
                re[a] += tRe;        im[a] += tIm;
                const float nRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = nRe;
            }
        }
    }

    for (size_t k = 0; k < n / 2; k++) outMag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]);
}

// No bus here, reported as unavailable rather than as an empty scan, which would mean a real bus that nothing answered on.
size_t i2cScan(uint16_t /*sda*/, uint16_t /*scl*/, uint8_t* /*out*/, size_t /*maxOut*/) {
    return kI2cBusUnavailable;
}

// No receiver here, so the seam is a no-op and the service still runs with its buttons working through the control path.
// A host has no pins either, so reads come from what a test injected.
// The button logic is ordinary code and gets tested here, leaving only the electrical half for the bench.
namespace {
// A flat table rather than a map: a pin number IS the index, there are at most 48 of them, and this allocates nothing.
constexpr uint8_t kMaxGpio = 48;
bool g_gpioLevel[kMaxGpio] = {};
}

bool gpioInputBegin(uint8_t gpio, GpioPull pull) {
    if (gpio >= kMaxGpio) return false;
    // The PULL sets the resting level, as it does on a board: a pull-up idles HIGH, a pull-down idles LOW. Without this every pin idled LOW, which an active-low button reads as HELD, so a desktop with no hardware reported a phantom press the moment a row named a pin. A test that wants a different level still calls setTestGpioLevel after this.
    g_gpioLevel[gpio] = (pull == GpioPull::Up);
    return true;
}

bool gpioRead(uint8_t gpio) { return gpio < kMaxGpio && g_gpioLevel[gpio]; }

bool gpioWrite(uint8_t gpio, bool high) {
    // A write is observable through gpioRead, so a test can drive a pin and read back what a module put there (a relay enable, MoonLive's write-then-read hello world).
    if (gpio >= kMaxGpio) return false;
    g_gpioLevel[gpio] = high;
    return true;
}

void setTestGpioLevel(uint8_t gpio, bool level) { if (gpio < kMaxGpio) g_gpioLevel[gpio] = level; }
void clearTestGpioLevel() { for (bool& b : g_gpioLevel) b = false; }

// --- ADC ---
// The desktop has no converter, so a read reports whatever a test injected. Same arrangement as the GPIO level above: a pedal's mapping, its min/max/invert and its smoothing are ordinary logic, and this is what lets all of it be pinned on the host with no hardware attached.
namespace {
uint16_t g_adcValue[kMaxGpio] = {};
// Millivolts are injected SEPARATELY from the raw count rather than derived from it. On a board the two are related by the chip's own eFuse curve, which a host cannot reproduce, so deriving one here would let a test pass against an arithmetic relationship that does not hold on hardware.
uint16_t g_adcMv[kMaxGpio] = {};
}

bool adcRead(uint8_t gpio, uint16_t& raw) {
    if (gpio >= kMaxGpio) return false;
    raw = g_adcValue[gpio];
    return true;
}

// The ESP32's 12-bit full scale, reported here too so a host test scales exactly as the board does: a mapping verified against 4095 on the desktop cannot then behave differently on a device.
uint16_t adcMaxCount() { return 4095; }

void setTestAdcValue(uint8_t gpio, uint16_t raw) { if (gpio < kMaxGpio) g_adcValue[gpio] = raw; }
void clearTestAdcValue() { for (uint16_t& v : g_adcValue) v = 0; for (uint16_t& v : g_adcMv) v = 0; }

bool adcReadMv(uint8_t gpio, uint16_t& mv) {
    if (gpio >= kMaxGpio) return false;
    mv = g_adcMv[gpio];
    return true;
}

void setTestAdcMv(uint8_t gpio, uint16_t mv) { if (gpio < kMaxGpio) g_adcMv[gpio] = mv; }

bool irRead(uint16_t /*pin*/, uint32_t& /*codeOut*/) { return false; }
void irStop() {}   // no IR hardware on desktop
bool irChannelReady(uint16_t /*pin*/) { return true; }   // no channel to fail on desktop


// Video output, resolved on demand and never linked: @xref{the-video-runtimes-structures-are-transcribed-not-included|why the declarations below must not be tidied}.
namespace {

using NdiSendInstance = void*;

// Processing.NDI.structs.h, NDIlib_video_frame_v2_t, verbatim field order.
struct NdiVideoFrameV2 {
    int         xres, yres;
    int         FourCC;                 // NDIlib_FourCC_video_type_e — an int-sized enum
    int         frame_rate_N, frame_rate_D;
    float       picture_aspect_ratio;
    int         frame_format_type;      // NDIlib_frame_format_type_e
    int64_t     timecode;
    uint8_t*    p_data;
    union { int line_stride_in_bytes; int data_size_in_bytes; };
    const char* p_metadata;
    int64_t     timestamp;
};

// Processing.NDI.Send.h, NDIlib_send_create_t, verbatim field order.
struct NdiSendCreate {
    const char* p_ndi_name;
    const char* p_groups;
    bool        clock_video, clock_audio;
};

// NDI_LIB_FOURCC('B','G','R','X'), X, not A: MoonLight has no alpha to send, and an ignored alpha channel is exactly what the X variants mean. Little-endian packing, as the macro builds it.
constexpr int kFourCCBgrx = 'B' | ('G' << 8) | ('R' << 16) | (static_cast<int>('X') << 24);
constexpr int kFrameFormatProgressive = 1;   // NDIlib_frame_format_type_progressive

using NdiInitFn       = bool (*)();
using NdiDestroyFn    = void (*)();
using NdiSendCreateFn = NdiSendInstance (*)(const NdiSendCreate*);
using NdiSendVideoFn  = void (*)(NdiSendInstance, const NdiVideoFrameV2*);
using NdiSendDestroyFn= void (*)(NdiSendInstance);

NdiInitFn        ndiInit_        = nullptr;
NdiDestroyFn     ndiDestroy_     = nullptr;
NdiSendCreateFn  ndiSendCreate_  = nullptr;
NdiSendVideoFn   ndiSendVideo_   = nullptr;
NdiSendDestroyFn ndiSendDestroy_ = nullptr;

// Test capture (platform.h § NDI test seam). Recording is OFF unless a test turns it on, so a desktop build with a real runtime behaves exactly as it would in production.
struct NdiCapturedFrame { uint16_t w, h; uint8_t fps; std::vector<uint8_t> rgb; };
NdiTestMode                  ndiTestMode_ = NdiTestMode::Off;
std::vector<NdiCapturedFrame> ndiCaptured_;
std::string                  ndiCapturedName_;

void*           ndiLib_    = nullptr;
NdiSendInstance ndiSender_ = nullptr;
std::string     ndiName_;                 // owned: NDIlib_send_create_t holds the pointer, not a copy
std::vector<uint8_t> ndiFrame_;           // BGRX staging, resized only on a geometry change

/// The runtime's file name per platform, tried in order and resolved through the search path the vendor's installer sets.
const char* const kNdiLibNames[] = {
#if defined(_WIN32)
    "Processing.NDI.Lib.x64.dll", "Processing.NDI.Lib.x86.dll",
#elif defined(__APPLE__)
    // NDI Tools for macOS ships the runtime INSIDE its app bundles rather than installing a system-wide dylib, so a plain name resolves nothing however complete the install is. The bundle paths are tried by name (verified to export the send API on a real NDI Tools install); `libndi_advanced` is the file NDI Tools ships, `libndi` the one bundled with Resolume.
    "libndi.dylib", "/usr/local/lib/libndi.dylib", "/opt/homebrew/lib/libndi.dylib",
    "/Applications/NDI Video Monitor.app/Contents/Frameworks/libndi_advanced.dylib",
    "/Applications/NDI Studio Monitor.app/Contents/Frameworks/libndi_advanced.dylib",
    "/Applications/NDI Discovery.app/Contents/Frameworks/libndi_advanced.dylib",
    "/Applications/Resolume Arena/libndi.dylib",
    "/Applications/Resolume Avenue/libndi.dylib",
#else
    "libndi.so.5", "libndi.so.6", "libndi.so",
#endif
};

bool ndiLoad() {
    if (ndiSendVideo_) return true;      // already resolved
    if (!ndiLib_) {
        for (const char* name : kNdiLibNames) {
#if defined(_WIN32)
            ndiLib_ = reinterpret_cast<void*>(::LoadLibraryA(name));
#else
            ndiLib_ = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL);
#endif
            if (ndiLib_) break;
        }
    }
    if (!ndiLib_) return false;
    auto sym = [](void* m, const char* n) -> void* {
#if defined(_WIN32)
        return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(m), n));
#else
        return ::dlsym(m, n);
#endif
    };
    ndiInit_         = reinterpret_cast<NdiInitFn>(sym(ndiLib_, "NDIlib_initialize"));
    ndiDestroy_      = reinterpret_cast<NdiDestroyFn>(sym(ndiLib_, "NDIlib_destroy"));
    ndiSendCreate_   = reinterpret_cast<NdiSendCreateFn>(sym(ndiLib_, "NDIlib_send_create"));
    ndiSendVideo_    = reinterpret_cast<NdiSendVideoFn>(sym(ndiLib_, "NDIlib_send_send_video_v2"));
    ndiSendDestroy_  = reinterpret_cast<NdiSendDestroyFn>(sym(ndiLib_, "NDIlib_send_destroy"));
    if (!ndiInit_ || !ndiSendCreate_ || !ndiSendVideo_ || !ndiSendDestroy_) {
        ndiSendVideo_ = nullptr;         // treat a partial resolve as absent
        return false;
    }
    // NDIlib_initialize returns false when the CPU is unsupported, a real "cannot use it" that must not read as "installed and working".
    if (!ndiInit_()) { ndiSendVideo_ = nullptr; return false; }
    return true;
}

}  // namespace

bool ndiAvailable() {
    if (ndiTestMode_ == NdiTestMode::ForceAvailable) return true;
    if (ndiTestMode_ == NdiTestMode::ForceMissing) return false;
    return ndiLoad();
}

bool ndiSenderOpen(const char* name) {
    if (ndiTestMode_ == NdiTestMode::ForceMissing) return false;
    if (ndiTestMode_ == NdiTestMode::ForceAvailable) { ndiCapturedName_ = (name && name[0]) ? name : "MoonLight"; return true; }
    if (!ndiLoad()) return false;
    ndiSenderClose();
    ndiName_ = (name && name[0]) ? name : "MoonLight";
    NdiSendCreate create{};
    create.p_ndi_name = ndiName_.c_str();   // the string must outlive the sender, hence ndiName_
    create.p_groups   = nullptr;
    // FALSE deliberately: clock_video makes send_send_video_v2 BLOCK to pace the caller, and this is called from the render thread which must never block. The driver already rate-limits to its fps control, so the pacing is ours to do.
    create.clock_video = false;
    create.clock_audio = false;             // no audio is sent
    ndiSender_ = ndiSendCreate_(&create);
    return ndiSender_ != nullptr;
}

void ndiSenderClose() {
    if (ndiTestMode_ != NdiTestMode::Off) { ndiCapturedName_.clear(); return; }
    if (ndiSender_) { ndiSendDestroy_(ndiSender_); ndiSender_ = nullptr; }
    ndiFrame_.clear();
    ndiFrame_.shrink_to_fit();
}

bool ndiSendFrame(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t fps) {
    if (!rgb || w == 0 || h == 0) return false;
    if (ndiTestMode_ == NdiTestMode::ForceAvailable) {
        // Record what the driver produced, tight RGB, exactly as handed over.
        NdiCapturedFrame f{w, h, fps, {}};
        f.rgb.assign(rgb, rgb + static_cast<size_t>(w) * h * 3);
        ndiCaptured_.push_back(std::move(f));
        return true;
    }
    if (!ndiSender_) return false;
    const size_t pixels = static_cast<size_t>(w) * h;
    ndiFrame_.resize(pixels * 4);           // no-op once warm; the only allocation, never per frame
    // RGB -> BGRX. The 4th byte is the ignored X, written once as 0xFF so a receiver that reads it as alpha sees opaque rather than transparent.
    for (size_t i = 0; i < pixels; ++i) {
        ndiFrame_[i * 4 + 0] = rgb[i * 3 + 2];
        ndiFrame_[i * 4 + 1] = rgb[i * 3 + 1];
        ndiFrame_[i * 4 + 2] = rgb[i * 3 + 0];
        ndiFrame_[i * 4 + 3] = 0xFF;
    }
    NdiVideoFrameV2 f{};
    f.xres = w;
    f.yres = h;
    f.FourCC = kFourCCBgrx;
    f.frame_rate_N = fps ? fps : 30;
    f.frame_rate_D = 1;
    f.picture_aspect_ratio = 0.0f;          // 0 = square pixels, which a light grid has
    f.frame_format_type = kFrameFormatProgressive;
    f.timecode = INT64_MAX;                 // NDIlib_send_timecode_synthesize: let NDI stamp it
    f.p_data = ndiFrame_.data();
    f.line_stride_in_bytes = static_cast<int>(w) * 4;
    ndiSendVideo_(ndiSender_, &f);          // synchronous, and clocked by clock_video above
    return true;
}

void setTestNdiMode(NdiTestMode mode) {
    ndiTestMode_ = mode;
    if (mode != NdiTestMode::ForceAvailable) { ndiCaptured_.clear(); ndiCapturedName_.clear(); }
}
size_t ndiTestFrameCount() { return ndiCaptured_.size(); }
uint16_t ndiTestFrameWidth(size_t i)  { return i < ndiCaptured_.size() ? ndiCaptured_[i].w : 0; }
uint16_t ndiTestFrameHeight(size_t i) { return i < ndiCaptured_.size() ? ndiCaptured_[i].h : 0; }
uint8_t  ndiTestFrameFps(size_t i)    { return i < ndiCaptured_.size() ? ndiCaptured_[i].fps : 0; }
const uint8_t* ndiTestFrameData(size_t i) {
    return i < ndiCaptured_.size() ? ndiCaptured_[i].rgb.data() : nullptr;
}
const char* ndiTestSenderName() { return ndiCapturedName_.c_str(); }
void ndiTestClearFrames() { ndiCaptured_.clear(); }


// The stream encoder: one spawned process with its input piped, its arguments from the driver.
// A writer thread does the blocking writes on every system while callers only enqueue whole frames into a fixed ring.
// So the render tick never touches the pipe and no per-system trickery is needed.
// A test seam mirrors the video one, so continuous integration never needs the encoder installed.

namespace {
// Threading model: encoderStart/Stop/Running are LIFECYCLE calls, made only from the render task (prepare/release/tick1s), so they need no lock among themselves. encoderWrite crosses threads (the encode worker) and the writer thread consumes: those three share encMutex_, which guards only the queue and the dead/stop flags, never a blocking write.
std::mutex encMutex_;
std::condition_variable encCv_;
// A fixed ring of REUSED frame slots, whole frames only (tearing is structurally out): the enqueue path must not heap-allocate per frame (assign() reuses each slot's capacity after the first lap), and 3 slots of burst absorption is the drop-newest boundary.
constexpr size_t kEncQueueMax = 3;
std::vector<uint8_t> encSlots_[kEncQueueMax];
size_t encHead_ = 0;    // slot the writer consumes next
size_t encCount_ = 0;   // filled slots
std::thread encWriter_;
bool encWriterStop_ = false;
bool encWriterDead_ = false;                  // the writer saw EPIPE/error: the process is gone
EncoderTestMode encTestMode_ = EncoderTestMode::Off;
constexpr int kEncTestWriteNormal = 1;   // sentinel: record normally (real results are 0/-1/len)
int encTestWriteResult_ = kEncTestWriteNormal;
std::vector<std::vector<uint8_t>> encCaptured_;
std::string encCapturedArgs_;

#ifdef _WIN32
HANDLE encProcess_ = nullptr;
HANDLE encStdin_ = nullptr;
HANDLE encStdout_ = nullptr;
#else
pid_t encPid_ = -1;
int encStdin_ = -1;
int encStdout_ = -1;
int esWake_[2] = {-1, -1};   // the reader waits on this beside the encoder's output
#endif

// --- The elementary stream, which RTSP ships and HLS has no use for -------------------------
// ffmpeg writes Annex B to stdout when the encoder runs without an output directory, and this thread cuts it into whole access units.
std::thread esReader_;
std::atomic<bool> esStop_{false};   // written by the render thread, read by the reader
std::mutex esMutex_;
std::vector<uint8_t> esFrame_;      // the published access unit, read under esMutex_
uint32_t esFrameSeq_ = 0;           // rises per published frame, so a taker can tell one from the next
uint32_t esTakenSeq_ = 0;
uint32_t esPts90_ = 0;
bool esKeyframe_ = false;
uint8_t esFps_ = 30;                // the rate the pts is advanced at, from the running config
constexpr uint32_t kRtpVideoClockHz = 90000;   // the RTP video clock the timestamps count in
}  // namespace


// Stop the child and the writer, deadlock-free: signal stop, TERM the child FIRST (a writer blocked in write() only reliably unblocks when the read side dies: EPIPE), join, then close stdin and reap with a short grace before SIGKILL. Called only from the render task.
static void stopEncoderProcess() {
    if (encTestMode_ != EncoderTestMode::Off) return;
    {
        std::lock_guard<std::mutex> lk(encMutex_);
        encWriterStop_ = true;
        // The ring counters are NOT reset here: the writer may be mid-write on the head slot, and a producer racing this stop must keep seeing that slot as occupied. encoderStart resets the ring under the lock after the join, when nothing can touch it.
        encCv_.notify_all();
    }
    esStop_ = true;
#ifdef _WIN32
    if (encProcess_) TerminateProcess(encProcess_, 0);
    if (encWriter_.joinable()) encWriter_.join();
    // CancelIoEx, never a close, while the reader is parked in ReadFile: closing a handle under a blocked read is undefined, and the value can be recycled onto another object.
    if (encStdout_ && esReader_.joinable()) CancelIoEx(encStdout_, nullptr);
    if (esReader_.joinable()) esReader_.join();
    if (encStdout_) { CloseHandle(encStdout_); encStdout_ = nullptr; }
    if (encStdin_) { CloseHandle(encStdin_); encStdin_ = nullptr; }
    if (encProcess_) { WaitForSingleObject(encProcess_, 500); CloseHandle(encProcess_); encProcess_ = nullptr; }
#else
    if (encPid_ >= 0) ::kill(encPid_, SIGTERM);
    if (encWriter_.joinable()) encWriter_.join();
    // Wake the reader through its OWN pipe: a signalled child holds the write end until reaped, so waiting for EOF deadlocks the render thread.
    if (esWake_[1] >= 0) { const char b = 1; (void)!::write(esWake_[1], &b, 1); }
    if (esReader_.joinable()) esReader_.join();
    if (esWake_[0] >= 0) { ::close(esWake_[0]); esWake_[0] = -1; }
    if (esWake_[1] >= 0) { ::close(esWake_[1]); esWake_[1] = -1; }
    if (encStdout_ >= 0) { ::close(encStdout_); encStdout_ = -1; }
    if (encStdin_ >= 0) { ::close(encStdin_); encStdin_ = -1; }
    if (encPid_ >= 0) {
        for (int i = 0; i < 20; i++) {                   // ~200 ms of graceful exit
            if (::waitpid(encPid_, nullptr, WNOHANG) == encPid_) { encPid_ = -1; break; }
            ::usleep(10 * 1000);
        }
        if (encPid_ >= 0) { ::kill(encPid_, SIGKILL); ::waitpid(encPid_, nullptr, 0); encPid_ = -1; }
    }
#endif
}

// Cut `pending` into whole access units, keeping the tail still arriving: core owns where a frame begins, this owns the buffering.
static void publishAccessUnits(std::vector<uint8_t>& pending, uint32_t& frames) {
    std::vector<size_t> starts;
    mm::h264::findAccessUnits(pending.data(), pending.size(), &starts);
    if (starts.size() < 2) return;      // a frame is whole only once the next one has begun

    for (size_t i = 0; i + 1 < starts.size(); i++) {
        const size_t from = starts[i], to = starts[i + 1];
        std::lock_guard<std::mutex> lk(esMutex_);
        esFrame_.assign(pending.begin() + static_cast<long>(from),
                        pending.begin() + static_cast<long>(to));
        esPts90_ = static_cast<uint32_t>(static_cast<uint64_t>(frames) *
                                         kRtpVideoClockHz / (esFps_ ? esFps_ : 30));
        esKeyframe_ = mm::h264::hasKeyframe(pending.data() + from, to - from);
        esFrameSeq_++;
        frames++;
    }
    pending.erase(pending.begin(), pending.begin() + static_cast<long>(starts.back()));
}

// Spawn `argv` with its stdin piped from us, and its stdout too where `captureStdout` asks: that is the elementary stream RTSP ships. This half is pure process plumbing.
static bool spawnEncoderProcess(const char* const argv[], bool captureStdout = false) {
    stopEncoderProcess();
    if (encTestMode_ != EncoderTestMode::Off) {
        encCapturedArgs_.clear();
        for (const char* const* a = argv; *a; a++) {
            if (!encCapturedArgs_.empty()) encCapturedArgs_ += ' ';
            encCapturedArgs_ += *a;
        }
        return encTestMode_ == EncoderTestMode::Record;
    }
#ifdef _WIN32
    // Anonymous pipe, our end non-inheritable; ffmpeg resolved via PATH by CreateProcess.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 4 * 1024 * 1024)) return false;
    SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE outRead = nullptr, outWrite = nullptr;
    if (captureStdout) {
        if (!CreatePipe(&outRead, &outWrite, &sa, 4 * 1024 * 1024)) {
            CloseHandle(readEnd); CloseHandle(writeEnd); return false;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    }
    // Quoted only where it needs to be: a bare `-` names ffmpeg's stdin and stdout, and quoting it hands the child a literal `"-"` it rejects. POSIX passes an array and never sees this.
    std::string cmd;
    for (const char* const* a = argv; *a; a++) {
        if (!cmd.empty()) cmd += ' ';
        const std::string arg(*a);
        const bool needsQuotes = arg.empty() ||
                                 arg.find_first_of(" \t\"") != std::string::npos;
        if (!needsQuotes) { cmd += arg; continue; }
        cmd += '"';
        for (const char c : arg) {
            if (c == '"') cmd += '\\';
            cmd += c;
        }
        cmd += '"';
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = readEnd;
    si.hStdOutput = captureStdout ? outWrite : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(readEnd);
    if (outWrite) CloseHandle(outWrite);       // the child holds it; ours would keep EOF away
    if (!ok) {
        CloseHandle(writeEnd);
        if (outRead) CloseHandle(outRead);
        return false;
    }
    CloseHandle(pi.hThread);
    encProcess_ = pi.hProcess;
    encStdin_ = writeEnd;
    encStdout_ = outRead;
#else
    // posix_spawn, not fork/exec: fork in a threaded process can deadlock on the allocator lock before exec, and a plain exec would leak every parent fd (the HTTP listen socket, the Art-Net/DDP ports) into a child that outlives a restart. Everything except the dup2'd stdin is closed in the child: CLOEXEC_DEFAULT on macOS, closefrom on glibc.
    int fds[2];
    if (::pipe(fds) != 0) return false;
    int outFds[2] = {-1, -1};
    if (captureStdout && ::pipe(outFds) != 0) { ::close(fds[0]); ::close(fds[1]); return false; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], 0);
    if (captureStdout) posix_spawn_file_actions_adddup2(&fa, outFds[1], 1);
    pid_t pid = -1;
    int rc;
#ifdef __APPLE__
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    rc = ::posix_spawnp(&pid, argv[0], &fa, &attr, const_cast<char* const*>(argv), environ);
    posix_spawnattr_destroy(&attr);
#else
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
    rc = ::posix_spawnp(&pid, argv[0], &fa, nullptr, const_cast<char* const*>(argv), environ);
#endif
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[0]);
    if (outFds[1] >= 0) ::close(outFds[1]);   // the child holds the write end; ours would keep EOF away
    if (rc != 0) {
        ::close(fds[1]);
        if (outFds[0] >= 0) ::close(outFds[0]);
        return false;
    }
    ::signal(SIGPIPE, SIG_IGN);             // a dead ffmpeg surfaces as EPIPE, not a signal
    encPid_ = pid;
    encStdin_ = fds[1];
    encStdout_ = outFds[0];
#endif
    // The writer thread does the BLOCKING writes: the render tick only ever enqueues, so an encoder that stops reading for a while (scheduler starvation under a free-running render loop stalled it >250 ms on the bench) costs queued-then-dropped frames, never a stalled tick, never a torn frame, and never a false death.
    {
        std::lock_guard<std::mutex> lk(encMutex_);   // producers may race this restart
        encWriterStop_ = false;
        encWriterDead_ = false;
        encHead_ = 0;
        encCount_ = 0;
    }
    encWriter_ = std::thread([] {
        for (;;) {
            const std::vector<uint8_t>* frame = nullptr;
            {
                std::unique_lock<std::mutex> lk(encMutex_);
                encCv_.wait(lk, [] { return encWriterStop_ || encCount_ > 0; });
                if (encWriterStop_) return;
                frame = &encSlots_[encHead_];   // the writer owns the head slot until it advances
            }
            size_t off = 0;
            while (off < frame->size()) {
#ifdef _WIN32
                DWORD wrote = 0;
                if (!WriteFile(encStdin_, frame->data() + off,
                               static_cast<DWORD>(frame->size() - off), &wrote, nullptr)) {
                    std::lock_guard<std::mutex> lk(encMutex_);
                    encWriterDead_ = true;
                    return;
                }
                off += wrote;
#else
                const ssize_t n = ::write(encStdin_, frame->data() + off, frame->size() - off);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    std::printf("encoder: pipe closed at %zu/%zu bytes\n", off, frame->size());
                    std::lock_guard<std::mutex> lk(encMutex_);
                    encWriterDead_ = true;   // EPIPE etc.: the process is gone
                    return;
                }
                off += static_cast<size_t>(n);
#endif
            }
            {
                std::lock_guard<std::mutex> lk(encMutex_);
                encHead_ = (encHead_ + 1) % kEncQueueMax;
                encCount_--;
            }
        }
    });

    // The elementary-stream reader, started only where the caller asked for stdout. It owns the splitting, so `rtspTakeFrame` is a copy under a lock and nothing more.
    if (captureStdout) {
        // A joinable thread assigned over terminates the process, and a run without stdout capture leaves this one unjoined.
        if (esReader_.joinable()) { esStop_ = true; esReader_.join(); }
        esStop_ = false;
#ifndef _WIN32
        if (esWake_[0] >= 0) { ::close(esWake_[0]); esWake_[0] = -1; }
        if (esWake_[1] >= 0) { ::close(esWake_[1]); esWake_[1] = -1; }
        // Without this pipe the stop path cannot reach a reader parked on a silent encoder, and the join would hold the render thread forever.
        if (::pipe(esWake_) != 0) {
            esWake_[0] = esWake_[1] = -1;
            return true;   // the encoder runs; only the frames this thread would publish are absent
        }
#endif
        {
            std::lock_guard<std::mutex> lk(esMutex_);
            esFrame_.clear();
            esFrameSeq_ = 0;
            esTakenSeq_ = 0;
            esPts90_ = 0;
        }
        // Captured BY VALUE: the stop path clears the globals while this thread runs, and reading them here would race that write. Windows wakes the reader with CancelIoEx and so has no wake pipe to capture.
#ifdef _WIN32
        const int wakeFd = -1;
#else
        const int wakeFd = esWake_[0];
#endif
        esReader_ = std::thread([out = encStdout_, wake = wakeFd] {
            std::vector<uint8_t> pending;     // bytes read but not yet a whole access unit
            uint8_t buf[16384];
            uint32_t frames = 0;
            for (;;) {
#ifdef _WIN32
                DWORD got = 0;
                // A canceled read reports ERROR_OPERATION_ABORTED, which is the stop path asking.
                if (!out || !ReadFile(out, buf, static_cast<DWORD>(sizeof(buf)), &got, nullptr) ||
                    got == 0) return;
                const size_t n = got;
#else
                // Both descriptors, so a stop is noticed even while the encoder sends nothing.
                if (out < 0) return;
                fd_set rd;
                FD_ZERO(&rd);
                FD_SET(out, &rd);
                if (wake >= 0) FD_SET(wake, &rd);
                const int maxFd = (wake > out ? wake : out) + 1;
                const int ready = ::select(maxFd, &rd, nullptr, nullptr, nullptr);
                if (ready < 0 && errno == EINTR) continue;
                if (ready < 0) return;
                if (wake >= 0 && FD_ISSET(wake, &rd)) return;   // asked to stop
                const ssize_t r = ::read(out, buf, sizeof(buf));
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) return;           // the child closed its stdout: the encoder is gone
                const size_t n = static_cast<size_t>(r);
#endif
                if (esStop_) return;
                pending.insert(pending.end(), buf, buf + n);
                publishAccessUnits(pending, frames);
            }
        });
    }
    return true;
}

// The ffmpeg invocation IS the desktop encode contract: raw RGB in at the grid size and rate, zerolatency x264 out, 1 s segments on a short rolling playlist (the live tuning that puts glass-to-glass at 2-5 s), segments deleted as they fall off it.
bool encoderStart(const EncoderConfig& cfg) {
    char geo[16], rate[8], gop[8], bv[12], out[192];
    std::snprintf(geo, sizeof(geo), "%ux%u", static_cast<unsigned>(cfg.width),
                  static_cast<unsigned>(cfg.height));
    std::snprintf(rate, sizeof(rate), "%u", static_cast<unsigned>(cfg.fps));
    std::snprintf(gop, sizeof(gop), "%u", static_cast<unsigned>(cfg.fps));
    std::snprintf(bv, sizeof(bv), "%uk", static_cast<unsigned>(cfg.bitrateKbit));
    // A null outDir asks for the elementary stream rather than a playlist: that is RTSP, which takes the frames itself.
    const bool elementary = (cfg.outDir == nullptr);
    if (!elementary) std::snprintf(out, sizeof(out), "%s/stream.m3u8", cfg.outDir);
    esFps_ = cfg.fps ? cfg.fps : 30;

    // Assembled by index so the software encoder's tuning flags stay off the hardware ones, which reject them, without duplicated slots.
    // The frame slots are sized HERE, off the render tick, since the write path would otherwise allocate on its first lap and must not allocate at all.
    // A failure here fails the start, where the driver already reports it, rather than throwing from a later write.
    const size_t frameBytes = static_cast<size_t>(cfg.width) * cfg.height * 3;
    // Stop FIRST, then resize. The previous writer thread reads a slot's data pointer in its blocking write loop WITHOUT encMutex_ held, so reserving under it is both a data race and, once a geometry or scale change grows frameBytes, a reallocation that frees the buffer the writer is still reading. spawnEncoderProcess stops again below; that call is then a no-op.
    stopEncoderProcess();
    try {
        for (auto& slot : encSlots_) slot.reserve(frameBytes);
    } catch (const std::bad_alloc&) {
        return false;
    }

    const char* encoder = cfg.encoderName ? cfg.encoderName : "libx264";
    const bool x264 = std::strcmp(encoder, "libx264") == 0;
    const char* argv[40];
    size_t i = 0;
    auto add = [&](const char* a) { if (i + 1 < sizeof(argv) / sizeof(argv[0])) argv[i++] = a; };
    for (const char* a : std::initializer_list<const char*>{
             "ffmpeg", "-hide_banner", "-loglevel", "error",
             "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", geo,
             "-r", rate, "-i", "-",
             "-c:v", encoder }) add(a);
    if (x264) { add("-preset"); add("veryfast"); add("-tune"); add("zerolatency"); }
    add("-g"); add(gop); add("-b:v"); add(bv);
    if (elementary) {
        // Annex B to stdout; `dump_extra` repeats the parameter sets per keyframe, so a client joining mid-stream decodes at once.
        for (const char* a : std::initializer_list<const char*>{
                 "-bsf:v", "dump_extra", "-f", "h264", "-" }) add(a);
    } else {
        for (const char* a : std::initializer_list<const char*>{
                 "-f", "hls", "-hls_time", "1", "-hls_list_size", "6",
                 "-hls_flags", "delete_segments+temp_file", out }) add(a);   // temp_file: the playlist lands by RENAME, never served half-written
    }
    argv[i] = nullptr;
    return spawnEncoderProcess(argv, elementary);
}

// ffmpeg writes the playlist and segments to disk itself, so there is nothing in RAM to serve and the HTTP server uses its normal file path.
bool hlsSegment(const char*, const uint8_t**, size_t*) { return false; }
void hlsSegmentRelease() {}
// The frame the reader published, copied out under the lock into a buffer that outlives the next read. The sequence is what says whether this caller has seen it.
bool rtspTakeFrame(EncodedFrame* out) {
    if (!out) return false;
    static std::vector<uint8_t> taken;        // the caller reads this after the lock drops
    std::lock_guard<std::mutex> lk(esMutex_);
    if (esFrameSeq_ == esTakenSeq_ || esFrame_.empty()) return false;
    esTakenSeq_ = esFrameSeq_;
    taken = esFrame_;
    out->nal      = taken.data();
    out->len      = taken.size();
    out->pts90    = esPts90_;
    out->keyframe = esKeyframe_;
    return true;
}

// The taker already owns its copy here, so there is nothing to hand back.
void rtspReleaseFrame() {}

// `dump_extra` above repeats the parameter sets on every keyframe, so a client joining at one decodes from it.
bool rtspParameterSets(EncodedFrame*, EncodedFrame*) { return false; }

int encoderWrite(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(encMutex_);
    if (encTestMode_ == EncoderTestMode::Record) {
        if (encTestWriteResult_ != kEncTestWriteNormal) return encTestWriteResult_;
        encCaptured_.emplace_back(data, data + len);
        return static_cast<int>(len);
    }
    if (encWriterDead_) return -1;
    if (encCount_ >= kEncQueueMax) return 0;   // encoder behind: drop-newest, stay live
    // assign() into the reused slot. The capacity was reserved by encoderStart, so this copies without allocating -- including the first lap, which is why the reserve is there.
    encSlots_[(encHead_ + encCount_) % kEncQueueMax].assign(data, data + len);
    encCount_++;
    encCv_.notify_one();
    return static_cast<int>(len);
}

bool encoderRunning() {
    if (encTestMode_ != EncoderTestMode::Off) return encTestMode_ == EncoderTestMode::Record;
    {
        std::lock_guard<std::mutex> lock(encMutex_);
        if (encWriterDead_) return false;
    }
#ifdef _WIN32
    if (!encProcess_) return false;
    return WaitForSingleObject(encProcess_, 0) == WAIT_TIMEOUT;
#else
    if (encPid_ < 0) return false;
    int status = 0;
    const pid_t r = ::waitpid(encPid_, &status, WNOHANG);
    if (r == encPid_) {
        std::printf("encoder: process exited (status %d)\n", status);
        encPid_ = -1;
        return false;
    }
    if (r < 0 && errno == ECHILD) { encPid_ = -1; return false; }   // reaped elsewhere: a stale
                                                                    // pid must never be SIGKILLed
    return true;   // running, or EINTR (a signal is not an exit)
#endif
}

void encoderStop() {
    stopEncoderProcess();
}

void setTestEncoderMode(EncoderTestMode mode) {
    encTestMode_ = mode;
    encTestWriteResult_ = kEncTestWriteNormal;
    if (mode == EncoderTestMode::Off) { encCaptured_.clear(); encCapturedArgs_.clear(); }
}
void setTestEncoderWriteResult(int result) { encTestWriteResult_ = result; }
size_t encoderTestFrameCount() { return encCaptured_.size(); }
size_t encoderTestFrameSize(size_t i) { return i < encCaptured_.size() ? encCaptured_[i].size() : 0; }
const uint8_t* encoderTestFrameData(size_t i) {
    return i < encCaptured_.size() ? encCaptured_[i].data() : nullptr;
}
const char* encoderTestArgs() { return encCapturedArgs_.c_str(); }
void encoderTestClearFrames() { encCaptured_.clear(); }


// Raw-interface enumeration for the driver's selection: labels for humans, bind names for the binder, the first entry always the capture-only row. One system lists through the capture library and labels by the adapter's friendly description, its device name being an identifier nobody recognizes; on the other the name IS the label.

namespace {
// FIXED storage, refilled in place: a Select's aux keeps pointing at these arrays across re-enumerations, so two panel-card instances rebuilding in one sweep can never dangle each other's option pointers (rows update under a stale aux, which is harmless; freed rows would not be). 16 NICs + the capture row cover any sane host.
constexpr size_t kRawIfMax = 17;
char rawIfLabels_[kRawIfMax][64];
char rawIfNames_[kRawIfMax][64];
const char* rawIfOptions_[kRawIfMax];
size_t rawIfCount_ = 0;
std::vector<std::string> rawIfTest_;   // test seam: label == bind name

void rawIfPush(const char* label, const char* name) {
    if (rawIfCount_ >= kRawIfMax) return;
    // 63 not 64: the Select apply path rejects labels that FILL its 64-byte buffer as overlong, so a row must persist at <= 62 chars or the pick dies on reboot.
    std::snprintf(rawIfLabels_[rawIfCount_], 63, "%s", label);
    std::snprintf(rawIfNames_[rawIfCount_], sizeof(rawIfNames_[0]), "%s", name);
    rawIfCount_++;
}
}  // namespace

void setTestRawInterfaces(const char* const* names, size_t count) {
    // The documented reset is (nullptr, 0), and `names + count` on a null pointer is undefined even when count is zero, so the reset is its own path rather than a degenerate range.
    if (!names || count == 0) { rawIfTest_.clear(); return; }
    rawIfTest_.assign(names, names + count);
}

size_t rawInterfaces(const char* const** optionsOut) {
    rawIfCount_ = 0;
    rawIfPush("none (capture only)", "");
    if (!rawIfTest_.empty()) {
        for (const auto& n : rawIfTest_) rawIfPush(n.c_str(), n.c_str());
    } else {
#ifdef _WIN32
        if (wpcapLoad()) {
            PcapIf* devs = nullptr;
            char err[256] = {};
            if (pcapFindAllDevs_(&devs, err) == 0 && devs) {
                MIB_IF_TABLE2* table = nullptr;
                if (::GetIfTable2(&table) != NO_ERROR) table = nullptr;
                for (const PcapIf* d = devs; d; d = d->next) {
                    // Show only what could carry panel frames, which the other branch does with a name blocklist while here the interface table answers it. Skipped only when that table could not be read, since otherwise every row would be filtered out and the picker would be an empty dead end on perfectly usable hardware.
                    if (table && !winIsPanelCapableNic(winRowForPcapName(table, d->name))) continue;
                    char desc[256] = {};
                    const char* label = nullptr;
                    if (winDescForPcapName(table, d->name, desc, sizeof(desc))) label = desc;
                    else if (d->description && d->description[0]) label = d->description;
                    else label = d->name;   // pcap reports no description for some adapters
                    char row[64];
                    std::snprintf(row, sizeof(row), "%s", label);
                    // Two identical adapters would collide as Select rows: suffix the device name's tail so each row stays a distinct, matchable label.
                    for (size_t i = 1; i < rawIfCount_; i++) {
                        if (std::strcmp(rawIfLabels_[i], row) == 0) {
                            const char* tail = d->name + (std::strlen(d->name) > 8 ? std::strlen(d->name) - 8 : 0);
                            std::snprintf(row, sizeof(row), "%s (%s)", label, tail);
                            break;
                        }
                    }
                    rawIfPush(row, d->name);
                }
                if (table) ::FreeMibTable(table);
                pcapFreeAllDevs_(devs);
            }
        }
#else
        // The OS's virtual plumbing can never reach a panel card and only buries the real NICs: loopback plus the well-known virtual prefixes (macOS: VPN tunnels, the AirDrop/AirPlay radios, Apple-silicon debug, bridges; Linux: container veths).
        static constexpr const char* kVirtualPrefixes[] = {
            "lo", "utun", "awdl", "llw", "anpi", "bridge", "gif", "stf", "ap", "pktap",
            "veth", "docker", "br-", "virbr",
        };
            // The negotiated speed, or nothing when the system will not state one, riding in the label for the same reason as the other branch.
            // The name alone does not say which entry is the fast adapter and which a tunnel.
            // The shared helper is the one place either the carrier or the rate is asked, and the label wants only the rate, so it discards the carrier.
            auto linkMbps = [](const char* ifname) -> unsigned {
                uint16_t mbps = 0;
                posixIfLink(ifname, mbps);
                return mbps;
            };

        auto isVirtual = [](const char* n) {
            for (const char* p : kVirtualPrefixes) {
                const size_t l = std::strlen(p);
                if (std::strncmp(n, p, l) == 0 && (n[l] == 0 || (n[l] >= '0' && n[l] <= '9')))
                    return true;
            }
            return false;
        };
        ifaddrs* addrs = nullptr;
        if (::getifaddrs(&addrs) == 0 && addrs) {
            for (const ifaddrs* a = addrs; a; a = a->ifa_next) {
                if (!a->ifa_name || !(a->ifa_flags & IFF_UP)) continue;
                if ((a->ifa_flags & IFF_LOOPBACK) || isVirtual(a->ifa_name)) continue;
                bool seen = false;
                for (size_t i = 1; i < rawIfCount_; i++)
                    if (std::strcmp(rawIfNames_[i], a->ifa_name) == 0) { seen = true; break; }
                if (seen) continue;   // getifaddrs lists one row per address family
                // Label carries the speed, bind name does not: the name is the adapter's identity and the speed changes when a link renegotiates (platform.h § raw interfaces). Same "NAME, N Gb" shape as the Windows branch.
                char label[64];
                std::snprintf(label, sizeof(label), "%s", a->ifa_name);
                appendLinkSpeed(label, sizeof(label), linkMbps(a->ifa_name));
                rawIfPush(label, a->ifa_name);
            }
            ::freeifaddrs(addrs);
        }
#endif
    }
    for (size_t i = 0; i < rawIfCount_; i++) rawIfOptions_[i] = rawIfLabels_[i];
    if (optionsOut) *optionsOut = rawIfOptions_;
    return rawIfCount_;
}

const char* rawInterfaceName(size_t i) {
    if (i == 0 || i >= rawIfCount_) return nullptr;   // row 0: capture only
    return rawIfNames_[i];
}

} // namespace mm::platform
