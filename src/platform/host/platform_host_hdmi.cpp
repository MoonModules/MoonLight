#include "platform/platform.h"

/// The host half of the HDMI seam: one output loop over each platform's screen (screen.h), the kernel's display interface on Linux and a full-screen window on a second display on Windows and macOS, or on the host's own when asked, and a stand-in recording what the driver hands over, which the tests run against.
///
/// @technical
///
/// ## A thread owns the screen
///
/// Finding the display, setting its mode and reading its EDID each take milliseconds, so none runs on the render tick. The tick hands a frame over through the frame slot every backend shares (`hdmi_handover.h`); the output thread scales it into the screen's back buffer, nearest neighbor with bars where it fits, and shows it.
///
/// ## Hot-plug is polled
///
/// Once a second the thread asks the screen for its display and keeps the one it shows while that stays connected. Unplugged, the display is released; plugged in, set up again in the mode of the driver's size at the nearest refresh, or the display's own mode where it lists no such size.
///
/// ## Windows and macOS: a second display
///
/// The output is a display beside the one the host runs on, as a sending card or a projector is: a borderless window covers it, its mode changed and the display kept awake for as long as the output runs, both given back when it stops.
///
/// With one display only there is no monitor to show on, unless the output was opened with `ownScreen`: the window then covers the host's own display, and the desktop is back when the output stops or Esc is pressed on it, which the driver answers by switching `ownScreen` off. A second display, plugged in, takes the output from it.
///
/// The EDID comes from where each system keeps it: Windows in the monitor device's registry key, macOS in the I/O registry on the display's port. A built-in panel has none.
///
/// ## The tests run on the stand-in
///
/// Any test hook switches the seam to the stand-in for the rest of the process. The unit tests' main calls one before every case and the scenario runner's main before its first, so neither the module contract test, which prepares every driver, nor a scenario reaches a real screen.

#include "platform/edid.h"
#include "platform/hdmi_handover.h"
#include "screen.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>


#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <setupapi.h>
#endif

namespace mm::platform {

namespace {

// The ESP32's sizes first, so the mode control reads alike everywhere, then a host's larger ones. Last the display's own mode, all zero: no display lists it, so every backend leaves the display as it is, as for any size it lacks.
constexpr HdmiMode kHdmiModes[] = {{800, 600, 60},   {1024, 768, 60},  {1280, 720, 60},  {1280, 800, 60},
                                   {1920, 1080, 60}, {2560, 1440, 60}, {3440, 1440, 60}, {3840, 2160, 60},
                                   {0, 0, 0}};
constexpr size_t kModeCount = sizeof(kHdmiModes) / sizeof(kHdmiModes[0]);
static_assert(hdmiDefaultMode == kModeCount - 1 && kHdmiModes[hdmiDefaultMode].width == 0, "the default is the display's own mode, the list's last");
constexpr HdmiMode kStandInOwnMode = {1920, 1080, 60};   // what the stand-in's display runs when a test names none

// --- The stand-in ---
bool hdmiStandIn_ = false;   // set by the first test hook: @xref{the-tests-run-on-the-stand-in|the tests}
int hdmiOpenMode_ = -1;
bool hdmiOwnScreen_ = false;
bool hdmiDismissed_ = false;
HdmiLink hdmiLink_ = HdmiLink::NoMonitor;
edid::Monitor hdmiMonitor_;
bool hdmiMonitorRead_ = false;
HdmiBridgeState hdmiBridge_{};
bool hdmiBridgeRead_ = false;
HdmiMode hdmiActive_{};   // a test's display mode, 0 wide for the mode opened
size_t hdmiFrames_ = 0;
uint16_t hdmiW_ = 0, hdmiH_ = 0;
bool hdmiFit_ = false;
// Fixed, since the write is on the render path and records without allocating; a test's wall is small.
uint8_t hdmiFrame_[16 * 1024];
size_t hdmiFrameBytes_ = 0;

// --- The output loop ---

struct Output {
    uint8_t modeIndex = 0;   // the driver's choice in kHdmiModes
    bool ownScreen = false;  // whether the host's own display may be covered
    std::thread thread;
    std::atomic<bool> run{false};
    std::atomic<uint8_t> link{static_cast<uint8_t>(HdmiLink::NoMonitor)};
    std::atomic<bool> dismissed{false};   // Esc on the host's own screen, until the driver takes it
    std::atomic<uint64_t> active{0};   // the mode the display runs, packed by pack(), 0 while none is up
    Seqlock<MonitorReport> monitor;
    // The frame the tick hands over, grown by the thread so the tick never allocates.
    FrameSlot slot;
    std::atomic<size_t> frameWant{0};
    std::vector<uint8_t> frame;
    uint16_t frameW = 0, frameH = 0;
    bool frameFit = true;
    // The display while one is up.
    bool up = false;
    uint32_t display = 0;
    std::vector<uint32_t> row;    // one scaled row, built in cached memory since a screen buffer may read back slowly
    std::vector<uint16_t> cols;   // the source column of each picture column
};
Output o;

constexpr uint64_t pack(const HdmiMode& m) { return uint64_t{m.width} | uint64_t{m.height} << 16 | uint64_t{m.hz} << 32; }
constexpr HdmiMode unpack(uint64_t v) {
    return {static_cast<uint16_t>(v), static_cast<uint16_t>(v >> 16), static_cast<uint8_t>(v >> 32)};
}

void publishMonitor(const edid::Monitor& m, bool read) { o.monitor.write({m, read}); }

void releaseDisplay() {
    if (o.up) screen::down();
    o.up = false;
    o.display = 0;
    o.active.store(0, std::memory_order_relaxed);
}

// Once a second: follow the display coming, going or changing.
void pollDisplay() {
    const uint32_t id = screen::find(o.up ? o.display : 0, o.ownScreen);
    if (!id) {
        releaseDisplay();
        publishMonitor(edid::Monitor{}, false);
        o.link.store(static_cast<uint8_t>(HdmiLink::NoMonitor));
        return;
    }
    if (o.up && id == o.display) return;
    releaseDisplay();
    edid::Size sizes[kModeCount];
    for (size_t m = 0; m < kModeCount; m++) sizes[m] = {kHdmiModes[m].width, kHdmiModes[m].height};
    edid::Monitor monitor;
    publishMonitor(monitor, screen::readEdid(id, sizes, kModeCount, monitor));
    HdmiMode running{};
    o.up = screen::up(id, kHdmiModes[o.modeIndex], running);
    if (o.up) { o.display = id; o.active.store(pack(running), std::memory_order_relaxed); }
    o.link.store(static_cast<uint8_t>(o.up ? HdmiLink::Monitor : HdmiLink::Down));
}

// Grow the frame to what the tick last asked for, while the tick cannot be writing it.
void growFrame() {
    const size_t want = o.frameWant.load();
    if (want <= o.frame.size()) return;
    if (!o.slot.claimIdle()) return;
    try { o.frame.resize(want); } catch (const std::bad_alloc&) {}   // the tick keeps asking; a smaller layer fits again
    o.slot.release();
}

// Where a w x h frame lands on a W x H screen: all of it when stretched, the largest same-shaped box when it fits.
struct Placement {
    uint32_t x0, y0, width, height;
};
Placement place(uint32_t W, uint32_t H, uint32_t w, uint32_t h, bool fit) {
    uint32_t outW = W, outH = H;
    if (fit) {
        if (W * h <= H * w) outH = h * W / w; else outW = w * H / h;   // whichever side the screen limits
        outW = outW ? outW : 1;
        outH = outH ? outH : 1;
    }
    return {(W - outW) / 2, (H - outH) / 2, outW, outH};
}

// Frame row `sy` scaled into o.row at the placement, XRGB8888, the bars either side left black.
void scaleRow(uint32_t sy, const Placement& at) {
    const uint8_t* src = o.frame.data() + static_cast<size_t>(sy) * o.frameW * 3;
    for (uint32_t x = 0; x < at.width; x++) {
        const uint8_t* p = src + o.cols[x] * 3;
        o.row[at.x0 + x] = (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2];
    }
}

// Scale the frame into the screen's back buffer, nearest neighbor with bars where it fits, and show it.
void show() {
    const screen::Surface dst = screen::back();
    const uint32_t W = dst.width, H = dst.height, w = o.frameW, h = o.frameH;
    if (!dst.pixels || W * H == 0 || w * h == 0) return;   // each side fits 16 bits, so the products cannot wrap
    const Placement at = place(W, H, w, h, o.frameFit);
    if (screen::presentScaled(o.frame.data(), static_cast<uint16_t>(w), static_cast<uint16_t>(h), static_cast<uint16_t>(at.x0),
                              static_cast<uint16_t>(at.y0), static_cast<uint16_t>(at.width), static_cast<uint16_t>(at.height))) return;   // the display scales: one pass over the frame, no screen-sized copy
    o.row.assign(W, 0);
    o.cols.resize(at.width);
    for (uint32_t x = 0; x < at.width; x++) o.cols[x] = static_cast<uint16_t>(x * w / at.width);
    uint32_t built = UINT32_MAX;   // the source row o.row holds
    for (uint32_t y = 0; y < H; y++) {
        uint32_t* line = reinterpret_cast<uint32_t*>(dst.pixels + static_cast<size_t>(y) * dst.pitch);
        if (y < at.y0 || y >= at.y0 + at.height) { std::memset(line, 0, W * 4); continue; }
        const uint32_t sy = (y - at.y0) * h / at.height;
        if (sy != built) { scaleRow(sy, at); built = sy; }
        std::memcpy(line, o.row.data(), W * 4);
    }
    screen::present();
}

void runOutput() {
    using Clock = std::chrono::steady_clock;
    Clock::time_point lastPoll{};
    while (o.run.load()) {
        screen::pump();
        if (screen::takeDismiss()) o.dismissed.store(true);
        if (Clock::now() - lastPoll >= std::chrono::seconds(1)) { pollDisplay(); lastPoll = Clock::now(); }
        growFrame();
        if (o.up && o.slot.claimShow()) { show(); o.slot.release(); }
        else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    releaseDisplay();
    screen::close();
}

void outputClose() {
    o.run.store(false);
    if (o.thread.joinable()) o.thread.join();
    // The frame goes once the tick is not writing it; the slot then stays taken, so a late write is refused until the next open.
    while (!o.slot.claimUnlessWriting()) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // a write in flight is one memcpy
    std::vector<uint8_t>().swap(o.frame);
    publishMonitor(edid::Monitor{}, false);
    o.link.store(static_cast<uint8_t>(HdmiLink::NoMonitor));
}

bool outputOpen(uint8_t index, bool ownScreen) {
    outputClose();
    if (!screen::open()) return false;
    static bool atExitRegistered = false;   // a reboot exits with the thread running, which would otherwise end the process in std::terminate
    if (!atExitRegistered) atExitRegistered = std::atexit(outputClose) == 0;
    o.modeIndex = index;
    o.ownScreen = ownScreen;
    o.dismissed.store(false);
    o.frameWant.store(0);
    o.slot.release();
    o.run.store(true);
    o.thread = std::thread(runOutput);
    return true;
}

bool outputWrite(const uint8_t* rgb, uint16_t w, uint16_t h, bool fit) MM_NONBLOCKING {
    if (!o.run.load(std::memory_order_acquire) || !o.slot.claimWrite()) return false;
    const size_t bytes = static_cast<size_t>(w) * h * 3;
    if (bytes > o.frame.size()) {
        o.frameWant.store(bytes, std::memory_order_relaxed);   // the thread grows it; this frame is dropped
        o.slot.release();
        return false;
    }
    std::memcpy(o.frame.data(), rgb, bytes);
    o.frameW = w;
    o.frameH = h;
    o.frameFit = fit;
    o.slot.publish();
    return true;
}

bool outputMonitor(edid::Monitor& out) MM_NONBLOCKING {
    MonitorReport r;
    if (!o.monitor.read(r) || !r.read) return false;   // a torn copy waits for the next second's read
    out = r.monitor;
    return true;
}

void useStandIn() {
    if (!hdmiStandIn_) outputClose();
    hdmiStandIn_ = true;
}

}  // namespace

// --- Which display, the one rule every desktop backend asks ---

namespace screen {
uint32_t pick(const Display* list, size_t n, uint32_t current, bool own) {
    uint32_t other = 0, self = 0;
    for (size_t i = 0; i < n; i++) {
        if (list[i].own) { self = list[i].id; continue; }
        if (!other || list[i].id == current) other = list[i].id;
    }
    return other ? other : own ? self : 0;
}
}  // namespace screen

// --- Linux: DRM/KMS, in platform_host_hdmi_linux.cpp ---

// --- Windows: a full-screen window on a second display ---
#ifdef _WIN32
namespace screen {
namespace {

struct Win {
    bool classRegistered = false;
    HWND window = nullptr;
    HDC memory = nullptr;            // holds the DIB section, blitted to the window
    HBITMAP dib = nullptr;
    HGDIOBJ previous = nullptr;
    uint8_t* pixels = nullptr;
    uint16_t width = 0, height = 0;
    char device[CCHDEVICENAME] = {};   // the display that is up: set by up(), so down() gives back the one it changed
    bool modeChanged = false;
    bool own = false;               // the device is the host's own display
    char found[CCHDEVICENAME] = {};    // the display find() last picked, which up() takes
    bool foundOwn = false;
    bool dismissed = false;         // Esc pressed on it, until the loop takes it
    bool escHeld = false;           // Esc is registered as a hot key
};
Win w;

constexpr const char* kClassName = "MoonLightHdmi";
constexpr int kEscHotKey = 1;   // this thread's id for the Esc hot key

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {   // uncovered: the last frame again
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (w.memory) BitBlt(dc, 0, 0, w.width, w.height, w.memory, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;   // the frame covers it all
    if (msg == WM_CLOSE) { w.dismissed = w.own; return 0; }   // the system's close (Alt+F4) asks for the desktop back, as Esc does; the window stays until the driver stops the output
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// The id of a display device: FNV-1a over its name, never 0.
uint32_t displayId(const char* device) {
    uint32_t hash = 2166136261u;
    for (const char* c = device; *c; c++) hash = (hash ^ static_cast<uint8_t>(*c)) * 16777619u;
    return hash ? hash : 1;
}

// The connected displays, as the picking rule reads them, each with the device name Windows knows it by.
struct Listing {
    Display display[16];
    char device[16][CCHDEVICENAME];
    size_t count = 0;
};

BOOL CALLBACK listMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM user) {
    Listing& l = *reinterpret_cast<Listing*>(user);
    MONITORINFOEXA info{};
    info.cbSize = sizeof(info);
    if (l.count == sizeof(l.display) / sizeof(l.display[0]) || !GetMonitorInfoA(monitor, &info)) return TRUE;
    l.display[l.count] = {displayId(info.szDevice), (info.dwFlags & MONITORINFOF_PRIMARY) != 0};
    std::snprintf(l.device[l.count], sizeof(l.device[0]), "%s", info.szDevice);
    l.count++;
    return TRUE;
}

struct Bounds {
    const char* device;
    RECT rect{};
    bool found = false;
};

BOOL CALLBACK boundsOf(HMONITOR monitor, HDC, LPRECT, LPARAM user) {
    Bounds& b = *reinterpret_cast<Bounds*>(user);
    MONITORINFOEXA info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoA(monitor, &info) && std::strcmp(info.szDevice, b.device) == 0) { b.rect = info.rcMonitor; b.found = true; return FALSE; }
    return TRUE;
}

// The device's mode for the driver's size at the nearest refresh, switched to for as long as the output runs.
void setMode(const HdmiMode& want) {
    DEVMODEA best{};
    bool found = false;
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(w.device, i, &dm); i++) {
        if (dm.dmPelsWidth != want.width || dm.dmPelsHeight != want.height || dm.dmBitsPerPel != 32) continue;
        const auto gap = [&](const DEVMODEA& x) { return x.dmDisplayFrequency > want.hz ? x.dmDisplayFrequency - want.hz : want.hz - x.dmDisplayFrequency; };
        if (!found || gap(dm) < gap(best)) { best = dm; found = true; }
    }
    if (!found) return;   // the display keeps its own mode
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    w.modeChanged = ChangeDisplaySettingsExA(w.device, &best, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL;   // temporary: the registry keeps the user's mode
}

}  // namespace

bool open() {
    if (!w.classRegistered) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = kClassName;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        w.classRegistered = RegisterClassA(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    return w.classRegistered;
}

void close() {}

uint32_t find(uint32_t current, bool own) {
    Listing l;
    EnumDisplayMonitors(nullptr, nullptr, listMonitor, reinterpret_cast<LPARAM>(&l));
    const uint32_t id = pick(l.display, l.count, current, own);
    for (size_t i = 0; id && i < l.count; i++) {
        if (l.display[i].id != id) continue;
        std::memcpy(w.found, l.device[i], sizeof(w.found));
        w.foundOwn = l.display[i].own;
    }
    return id;
}

namespace {

// The monitor's driver key, the last part of its device id, MONITOR\<id>\<driver key>; null where the id has none.
const char* driverKey(const char* deviceId) {
    const char* p = std::strchr(deviceId, '\\');
    if (p) p = std::strchr(p + 1, '\\');
    return p ? p + 1 : nullptr;
}

// The device in the monitor class whose driver key is `driver`.
bool findMonitorDevice(HDEVINFO set, const char* driver, SP_DEVINFO_DATA& device) {
    device.cbSize = sizeof(device);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &device); i++) {
        char key[128] = {};
        if (SetupDiGetDeviceRegistryPropertyA(set, &device, SPDRP_DRIVER, nullptr, reinterpret_cast<BYTE*>(key), sizeof(key) - 1, nullptr)
            && _stricmp(key, driver) == 0) return true;
    }
    return false;
}

// The EDID value in a monitor device's registry key, reduced to `sizes`.
bool edidOf(HDEVINFO set, SP_DEVINFO_DATA& device, const edid::Size* sizes, size_t n, edid::Monitor& out) {
    HKEY reg = SetupDiOpenDevRegKey(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (reg == INVALID_HANDLE_VALUE) return false;
    uint8_t data[4 * edid::kBlockBytes];
    DWORD bytes = sizeof(data);
    const bool read = RegQueryValueExA(reg, "EDID", nullptr, nullptr, data, &bytes) == ERROR_SUCCESS && bytes >= edid::kBlockBytes;
    RegCloseKey(reg);
    return read && edid::parse(data, bytes >= 2 * edid::kBlockBytes ? data + edid::kBlockBytes : nullptr, sizes, n, out);
}

}  // namespace

// The monitor's EDID, which Windows keeps in the monitor device's registry key: its driver key, from the display device, finds that device in the monitor class.
bool readEdid(uint32_t, const edid::Size* sizes, size_t n, edid::Monitor& out) {
    static constexpr GUID kMonitorClass = {0x4d36e96e, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};   // GUID_DEVCLASS_MONITOR
    DISPLAY_DEVICEA monitor{};
    monitor.cb = sizeof(monitor);
    if (!EnumDisplayDevicesA(w.found, 0, &monitor, 0)) return false;   // read before up(), so of the display find() picked
    const char* driver = driverKey(monitor.DeviceID);
    if (!driver) return false;
    HDEVINFO set = SetupDiGetClassDevsA(&kMonitorClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA device{};
    const bool parsed = findMonitorDevice(set, driver, device) && edidOf(set, device, sizes, n, out);
    SetupDiDestroyDeviceInfoList(set);
    return parsed;
}

bool up(uint32_t /*display*/, const HdmiMode& want, HdmiMode& running) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // on the output thread, which sizes and creates the window: the display's own pixels, not scaled ones
    std::memcpy(w.device, w.found, sizeof(w.device));   // the display find() picked; the one that was up is down by now
    w.own = w.foundOwn;
    setMode(want);
    Bounds b{w.device};
    EnumDisplayMonitors(nullptr, nullptr, boundsOf, reinterpret_cast<LPARAM>(&b));
    if (!b.found) { down(); return false; }
    w.width = static_cast<uint16_t>(b.rect.right - b.rect.left);
    w.height = static_cast<uint16_t>(b.rect.bottom - b.rect.top);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w.width;
    bi.bmiHeader.biHeight = -static_cast<LONG>(w.height);   // top-down rows, as the scaler writes them
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;   // B, G, R, X in memory: XRGB8888
    void* bits = nullptr;
    w.dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    w.memory = CreateCompatibleDC(nullptr);
    if (!w.dib || !w.memory) { down(); return false; }
    w.previous = SelectObject(w.memory, w.dib);
    w.pixels = static_cast<uint8_t*>(bits);
    w.window = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, "MoonLight", WS_POPUP | WS_VISIBLE,
                               b.rect.left, b.rect.top, w.width, w.height, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!w.window) { down(); return false; }
    DEVMODEA now{};
    now.dmSize = sizeof(now);
    const bool rate = EnumDisplaySettingsA(w.device, ENUM_CURRENT_SETTINGS, &now);
    running = {w.width, w.height, static_cast<uint8_t>(rate ? now.dmDisplayFrequency : 0)};
    // Esc as a hot key while the host's own display is covered: the keyboard's focus stays with whatever had it, MoonLight's console among them, so a hot key is what reaches MoonLight.
    w.escHeld = w.own && RegisterHotKey(nullptr, kEscHotKey, 0, VK_ESCAPE);
    if (w.own && !w.escHeld) std::printf("HDMI: another program holds Esc, so switch ownScreen off from a browser to get the desktop back\n");
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);   // a show has no keyboard to keep the screen saver and display sleep away; down() gives them back
    std::printf("HDMI: %ux%u window on %s\n", static_cast<unsigned>(w.width), static_cast<unsigned>(w.height), w.device);
    return true;
}

Surface back() { return {w.pixels, static_cast<uint32_t>(w.width) * 4, w.width, w.height}; }

void present() {
    HDC dc = GetDC(w.window);
    BitBlt(dc, 0, 0, w.width, w.height, w.memory, 0, 0, SRCCOPY);
    ReleaseDC(w.window, dc);
}

void down() {
    if (w.escHeld) { UnregisterHotKey(nullptr, kEscHotKey); w.escHeld = false; }   // Esc is every program's again
    SetThreadExecutionState(ES_CONTINUOUS);   // the system's own idle timers again
    if (w.window) { DestroyWindow(w.window); w.window = nullptr; }
    if (w.memory) { SelectObject(w.memory, w.previous); DeleteDC(w.memory); w.memory = nullptr; }
    if (w.dib) { DeleteObject(w.dib); w.dib = nullptr; }
    w.pixels = nullptr;
    if (w.modeChanged) { ChangeDisplaySettingsExA(w.device, nullptr, nullptr, 0, nullptr); w.modeChanged = false; }   // back to the registry's mode
}

void pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_HOTKEY && static_cast<int>(msg.wParam) == kEscHotKey) { w.dismissed = w.own; continue; }   // posted to this thread, which registered it
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

bool takeDismiss() {
    const bool was = w.dismissed;   // set by pump and by windowProc, both on this thread
    w.dismissed = false;
    return was;
}

// The window takes a screen-sized buffer, so the loop scales into it.
bool presentScaled(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) { return false; }

}  // namespace screen
#endif  // _WIN32

// --- A build without a screen backend, such as GCC's on macOS: no display to show on ---
#if !defined(__linux__) && !defined(_WIN32) && !defined(MM_MAC_SCREEN)
namespace screen {
bool open() { return false; }
void close() {}
uint32_t find(uint32_t, bool) { return 0; }
bool readEdid(uint32_t, const edid::Size*, size_t, edid::Monitor&) { return false; }
bool up(uint32_t, const HdmiMode&, HdmiMode&) { return false; }
Surface back() { return {}; }
bool presentScaled(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) { return false; }
void present() {}
void down() {}
void pump() {}
bool takeDismiss() { return false; }
}  // namespace screen
#endif

// --- The seam ---

size_t hdmiModes(const HdmiMode** out) MM_NONBLOCKING {
    if (out) *out = kHdmiModes;
    return kModeCount;
}

bool hdmiOpen(uint8_t index, bool ownScreen) {
    if (index >= kModeCount) return false;
    if (!hdmiStandIn_) return outputOpen(index, ownScreen);
    hdmiOpenMode_ = index;
    hdmiOwnScreen_ = ownScreen;
    return true;
}

void hdmiClose() {
    if (!hdmiStandIn_) { outputClose(); return; }
    hdmiOpenMode_ = -1; hdmiFrames_ = 0; hdmiFrameBytes_ = 0;   // a closed output keeps no frames, so a test sees a restart
}

bool hdmiWrite(const uint8_t* rgb, uint16_t w, uint16_t h, bool fit) MM_NONBLOCKING {
    if (!rgb || w == 0 || h == 0) return false;
    if (!hdmiStandIn_) return outputWrite(rgb, w, h, fit);
    const size_t bytes = static_cast<size_t>(w) * h * 3;
    if (hdmiOpenMode_ < 0 || bytes > sizeof(hdmiFrame_)) return false;
    std::memcpy(hdmiFrame_, rgb, bytes);
    hdmiFrameBytes_ = bytes;
    hdmiW_ = w; hdmiH_ = h; hdmiFit_ = fit;
    hdmiFrames_++;
    return true;
}

bool hdmiActiveMode(HdmiMode& out) MM_NONBLOCKING {
    if (!hdmiStandIn_) {
        const uint64_t v = o.active.load(std::memory_order_relaxed);
        if (!v) return false;
        out = unpack(v);
        return true;
    }
    if (hdmiOpenMode_ < 0 || hdmiLink_ != HdmiLink::Monitor) return false;
    out = hdmiActive_.width ? hdmiActive_ : kHdmiModes[hdmiOpenMode_].width ? kHdmiModes[hdmiOpenMode_] : kStandInOwnMode;
    return true;
}

HdmiLink hdmiLink() MM_NONBLOCKING {
    if (!hdmiStandIn_) return static_cast<HdmiLink>(o.link.load(std::memory_order_relaxed));
    return hdmiOpenMode_ >= 0 ? hdmiLink_ : HdmiLink::NoMonitor;
}

bool hdmiMonitor(edid::Monitor& out) MM_NONBLOCKING {
    if (!hdmiStandIn_) return outputMonitor(out);
    if (hdmiOpenMode_ < 0 || hdmiLink_ != HdmiLink::Monitor || !hdmiMonitorRead_) return false;
    out = hdmiMonitor_;
    return true;
}

bool hdmiTakeDismiss() MM_NONBLOCKING {
    if (!hdmiStandIn_) return o.dismissed.exchange(false, std::memory_order_relaxed);
    const bool was = hdmiDismissed_;
    hdmiDismissed_ = false;
    return was;
}

// A host has no bridge to read back; the stand-in reports what a test set.
bool hdmiBridge(HdmiBridgeState& out) MM_NONBLOCKING {
    if (!hdmiStandIn_ || hdmiOpenMode_ < 0 || hdmiLink_ == HdmiLink::Down || !hdmiBridgeRead_) return false;
    out = hdmiBridge_;
    return true;
}

void setTestHdmiLink(HdmiLink link) { useStandIn(); hdmiLink_ = link; }
void setTestHdmiBridge(const HdmiBridgeState& state) { useStandIn(); hdmiBridge_ = state; hdmiBridgeRead_ = true; }
void setTestHdmiMonitor(const edid::Monitor& monitor) { useStandIn(); hdmiMonitor_ = monitor; hdmiMonitorRead_ = true; }
void setTestHdmiActiveMode(const HdmiMode& mode) { useStandIn(); hdmiActive_ = mode; }
int hdmiTestOpenMode() { return hdmiOpenMode_; }
bool hdmiTestOwnScreen() { return hdmiOwnScreen_; }
void setTestHdmiDismiss() { useStandIn(); hdmiDismissed_ = true; }
size_t hdmiTestFrameCount() { return hdmiFrames_; }
uint16_t hdmiTestFrameWidth() { return hdmiW_; }
uint16_t hdmiTestFrameHeight() { return hdmiH_; }
bool hdmiTestFrameFit() { return hdmiFit_; }
const uint8_t* hdmiTestFrameData() { return hdmiFrameBytes_ ? hdmiFrame_ : nullptr; }
void hdmiTestClear() {
    useStandIn();
    hdmiFrames_ = 0; hdmiFrameBytes_ = 0; hdmiW_ = hdmiH_ = 0; hdmiOpenMode_ = -1; hdmiOwnScreen_ = false; hdmiDismissed_ = false; hdmiLink_ = HdmiLink::NoMonitor; hdmiMonitorRead_ = false; hdmiBridgeRead_ = false; hdmiActive_ = {};
}

}  // namespace mm::platform
