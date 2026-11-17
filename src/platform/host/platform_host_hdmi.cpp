#include "platform/platform.h"

/// The desktop half of the HDMI seam: one output loop over each platform's screen (screen.h), the kernel's display interface on Linux and a full-screen window on a second display on Windows and macOS, and a stand-in recording what the driver hands over, which the tests run against.
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
/// ## Linux: straight to the kernel
///
/// DRM/KMS through the kernel's own ioctls and uapi headers, the stable ABI libdrm wraps, so neither the build nor the package gains a library: the no-SDK rule the audio and NDI paths follow. Every list the kernel returns comes in two calls, the counts first and then the arrays, as libdrm does it.
///
/// The first connected HDMI connector is shown, drawn into the back one of two buffers that flip on the vertical blank; a driver where the flip fails draws into the shown buffer instead, tearing rather than going dark.
///
/// While no monitor is up the ask re-probes the connectors; while one is up it reads the state the kernel's hot-plug interrupt keeps, so a running monitor's EDID is not re-read every second.
///
/// ## Linux: the console takes the screen back
///
/// Without a desktop the first program to open the card sets its mode, and closing the card hands the screen back to the text console. With a desktop running, the desktop holds the screen and the link reports Down, retried each second.
///
/// ## Windows and macOS: a second display
///
/// The output is a display beside the one the desktop runs on, as a sending card or a projector is: a borderless window covers it, its mode changed for as long as the output runs and given back when it stops. With one display only there is no monitor to show on, so the desktop is never covered.
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

#ifdef __linux__
    #include <cerrno>

    #include <drm/drm.h>
    #include <drm/drm_mode.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/ioctl.h>
    #include <sys/mman.h>
    #include <unistd.h>
#endif

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

// The ESP32's sizes first, so the default and the driver's mode control read alike everywhere, then the sizes a desktop runs beyond the ESP32's lanes: 1440p, the ultrawide 1440p and 4K. A display without the size keeps its own mode instead.
constexpr HdmiMode kHdmiModes[] = {{800, 600, 60},   {1024, 768, 60},  {1280, 720, 60},  {1280, 800, 60},
                                   {1920, 1080, 60}, {2560, 1440, 60}, {3440, 1440, 60}, {3840, 2160, 60}};
constexpr size_t kModeCount = sizeof(kHdmiModes) / sizeof(kHdmiModes[0]);

// --- The stand-in ---
bool hdmiStandIn_ = false;   // set by the first test hook: @xref{the-tests-run-on-the-stand-in|the tests}
int hdmiOpenMode_ = -1;
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
    std::thread thread;
    std::atomic<bool> run{false};
    std::atomic<uint8_t> link{static_cast<uint8_t>(HdmiLink::NoMonitor)};
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
    const uint32_t id = screen::find(o.up ? o.display : 0);
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

bool outputOpen(uint8_t index) {
    outputClose();
    if (!screen::open()) return false;
    static bool atExitRegistered = false;   // a reboot exits with the thread running, which would otherwise end the process in std::terminate
    if (!atExitRegistered) atExitRegistered = std::atexit(outputClose) == 0;
    o.modeIndex = index;
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

// --- Linux: DRM/KMS ---
#ifdef __linux__
namespace screen {
namespace {

// One scanout buffer: the kernel's dumb buffer, the framebuffer naming it, and its mapping.
struct Scanout {
    uint32_t handle = 0;
    uint32_t fb = 0;
    uint32_t pitch = 0;
    uint64_t bytes = 0;
    uint8_t* map = nullptr;
};

// One connector with its modes, encoders and properties.
struct Connector {
    drm_mode_get_connector info{};
    std::vector<drm_mode_modeinfo> modes;
    std::vector<uint32_t> encoders;
    std::vector<uint32_t> props;
    std::vector<uint64_t> values;
};

struct Kms {
    int fd = -1;
    Connector found;                 // the connector find() returned, for readEdid() and up()
    uint32_t connector = 0;
    uint32_t crtc = 0;
    drm_mode_modeinfo timing{};      // the mode the monitor runs
    Scanout buf[2];
    int shown = 0;                   // the buffer on screen
    bool flips = true;               // false once a flip failed: draw into the shown buffer
    bool flipPending = false;        // a flip asked for whose event has not been read
};
Kms k;

// libdrm's drmIoctl: a call the kernel interrupted is made again.
int drmIoctl(int fd, unsigned long request, void* arg) {
    int r;
    do r = ioctl(fd, request, arg);
    while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

template <typename T>
uint64_t ptr(T* p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); }

// The card's CRTCs and connectors; false for a card without mode setting, such as the Pi's 3D engine.
bool resources(int fd, std::vector<uint32_t>& crtcs, std::vector<uint32_t>& connectors) {
    for (int attempt = 0; attempt < 3; attempt++) {   // a monitor plugged in between the two calls grows the counts
        drm_mode_card_res res{};
        if (drmIoctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) return false;
        crtcs.assign(res.count_crtcs, 0);
        connectors.assign(res.count_connectors, 0);
        const uint32_t nCrtcs = res.count_crtcs, nConnectors = res.count_connectors;
        res = {};
        res.count_crtcs = nCrtcs;
        res.crtc_id_ptr = ptr(crtcs.data());
        res.count_connectors = nConnectors;
        res.connector_id_ptr = ptr(connectors.data());
        if (drmIoctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) return false;
        if (res.count_crtcs <= nCrtcs && res.count_connectors <= nConnectors) {
            crtcs.resize(res.count_crtcs);
            connectors.resize(res.count_connectors);
            return true;
        }
    }
    return false;
}

// Read a connector; `probe` makes the kernel ask the monitor again, where otherwise it answers from what it last knew.
bool readConnector(int fd, uint32_t id, bool probe, Connector& c) {
    for (int attempt = 0; attempt < 3; attempt++) {
        drm_mode_modeinfo current{};
        c.info = {};
        c.info.connector_id = id;
        if (!probe) { c.info.count_modes = 1; c.info.modes_ptr = ptr(&current); }   // a mode slot offered says "do not probe"
        if (drmIoctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c.info) != 0) return false;
        const uint32_t nModes = c.info.count_modes, nEncoders = c.info.count_encoders, nProps = c.info.count_props;
        c.modes.assign(nModes > 0 ? nModes : 1, drm_mode_modeinfo{});
        c.encoders.assign(nEncoders, 0);
        c.props.assign(nProps, 0);
        c.values.assign(nProps, 0);
        c.info = {};
        c.info.connector_id = id;
        c.info.count_modes = static_cast<uint32_t>(c.modes.size());
        c.info.modes_ptr = ptr(c.modes.data());
        c.info.count_encoders = nEncoders;
        c.info.encoders_ptr = ptr(c.encoders.data());
        c.info.count_props = nProps;
        c.info.props_ptr = ptr(c.props.data());
        c.info.prop_values_ptr = ptr(c.values.data());
        if (drmIoctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c.info) != 0) return false;
        if (c.info.count_modes <= c.modes.size() && c.info.count_encoders <= nEncoders && c.info.count_props <= nProps) {
            c.modes.resize(c.info.count_modes);
            c.encoders.resize(c.info.count_encoders);
            c.props.resize(c.info.count_props);
            c.values.resize(c.info.count_props);
            return true;
        }
    }
    return false;
}

bool isHdmi(const Connector& c) {
    return c.info.connector_type == DRM_MODE_CONNECTOR_HDMIA || c.info.connector_type == DRM_MODE_CONNECTOR_HDMIB;
}

bool progressive(const drm_mode_modeinfo& m) { return !(m.flags & DRM_MODE_FLAG_INTERLACE); }   // a frame drawn whole, not in fields

// The driver's size at the refresh nearest its rate; null where the monitor has no such size.
const drm_mode_modeinfo* sizeMode(const Connector& c, const HdmiMode& want) {
    const auto gap = [&](const drm_mode_modeinfo& x) { return x.vrefresh > want.hz ? x.vrefresh - want.hz : want.hz - x.vrefresh; };
    const drm_mode_modeinfo* best = nullptr;
    for (const drm_mode_modeinfo& m : c.modes)
        if (progressive(m) && m.hdisplay == want.width && m.vdisplay == want.height && (!best || gap(m) < gap(*best))) best = &m;
    return best;
}

// The monitor's preferred mode, else its first.
const drm_mode_modeinfo* ownMode(const Connector& c) {
    for (const drm_mode_modeinfo& m : c.modes)
        if (progressive(m) && (m.type & DRM_MODE_TYPE_PREFERRED)) return &m;
    return c.modes.empty() ? nullptr : &c.modes[0];
}

bool pickMode(const Connector& c, const HdmiMode& want, drm_mode_modeinfo& out) {
    const drm_mode_modeinfo* m = sizeMode(c, want);
    if (!m) m = ownMode(c);
    if (!m) return false;
    out = *m;
    return true;
}

// A CRTC the connector reaches: the one its encoder drives now, else the first its encoders can drive.
uint32_t pickCrtc(const Connector& c) {
    std::vector<uint32_t> crtcs, connectors;
    if (!resources(k.fd, crtcs, connectors)) return 0;
    if (c.info.encoder_id) {
        drm_mode_get_encoder enc{};
        enc.encoder_id = c.info.encoder_id;
        if (drmIoctl(k.fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id) return enc.crtc_id;
    }
    for (uint32_t id : c.encoders) {
        drm_mode_get_encoder enc{};
        enc.encoder_id = id;
        if (drmIoctl(k.fd, DRM_IOCTL_MODE_GETENCODER, &enc) != 0) continue;
        for (size_t i = 0; i < crtcs.size() && i < 32; i++)
            if (enc.possible_crtcs & (1u << i)) return crtcs[i];
    }
    return 0;
}

void destroyScanout(Scanout& s) {
    if (s.map) munmap(s.map, s.bytes);
    if (s.fb) { uint32_t fb = s.fb; drmIoctl(k.fd, DRM_IOCTL_MODE_RMFB, &fb); }
    if (s.handle) { drm_mode_destroy_dumb d{}; d.handle = s.handle; drmIoctl(k.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d); }
    s = Scanout{};
}

// A dumb buffer of the mode's size in XRGB8888, named as a framebuffer and mapped.
bool createScanout(Scanout& s, uint16_t w, uint16_t h) {
    drm_mode_create_dumb create{};
    create.width = w;
    create.height = h;
    create.bpp = 32;
    if (drmIoctl(k.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) return false;
    s.handle = create.handle;
    s.pitch = create.pitch;
    s.bytes = create.size;
    drm_mode_fb_cmd fb{};
    fb.width = w;
    fb.height = h;
    fb.pitch = s.pitch;
    fb.bpp = 32;
    fb.depth = 24;
    fb.handle = s.handle;
    if (drmIoctl(k.fd, DRM_IOCTL_MODE_ADDFB, &fb) != 0) { destroyScanout(s); return false; }
    s.fb = fb.fb_id;
    drm_mode_map_dumb map{};
    map.handle = s.handle;
    if (drmIoctl(k.fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) { destroyScanout(s); return false; }
    void* p = mmap(nullptr, s.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, k.fd, static_cast<off_t>(map.offset));
    if (p == MAP_FAILED) { destroyScanout(s); return false; }
    s.map = static_cast<uint8_t*>(p);
    std::memset(s.map, 0, s.bytes);
    return true;
}

// Show buffer `b` on the connector in the picked mode.
bool setCrtc(int b) {
    drm_mode_crtc set{};
    set.crtc_id = k.crtc;
    set.fb_id = k.buf[b].fb;
    set.set_connectors_ptr = ptr(&k.connector);
    set.count_connectors = 1;
    set.mode = k.timing;
    set.mode_valid = 1;
    return drmIoctl(k.fd, DRM_IOCTL_MODE_SETCRTC, &set) == 0;
}

bool hasHdmiConnector(int fd) {
    std::vector<uint32_t> crtcs, connectors;
    if (!resources(fd, crtcs, connectors)) return false;
    for (uint32_t id : connectors) {
        Connector c;
        if (readConnector(fd, id, false, c) && isHdmi(c)) return true;
    }
    return false;
}

}  // namespace

// The first card under /dev/dri with an HDMI connector.
bool open() {
    for (int i = 0; i < 16; i++) {
        char path[24];
        std::snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        const int fd = ::open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        if (hasHdmiConnector(fd)) { k.fd = fd; return true; }
        ::close(fd);
    }
    return false;
}

void close() {
    if (k.fd >= 0) { ::close(k.fd); k.fd = -1; }   // @xref{linux-the-console-takes-the-screen-back|the console}
}

// The connected HDMI connector, the one shown kept first so a second monitor does not take its place.
uint32_t find(uint32_t current) {
    std::vector<uint32_t> crtcs, connectors;
    if (!resources(k.fd, crtcs, connectors)) return 0;
    uint32_t found = 0;
    for (uint32_t id : connectors) {
        Connector c;
        if (!readConnector(k.fd, id, current == 0, c) || !isHdmi(c) || c.info.connection != 1) continue;   // 1: connected
        if (!found || id == current) { found = id; k.found = std::move(c); }
    }
    return found;
}

// The connector's EDID property, reduced by edid.h.
bool readEdid(uint32_t /*display*/, const edid::Size* sizes, size_t n, edid::Monitor& out) {
    const Connector& c = k.found;
    for (size_t i = 0; i < c.props.size(); i++) {
        drm_mode_get_property prop{};
        prop.prop_id = c.props[i];
        if (drmIoctl(k.fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) != 0 || std::strcmp(prop.name, "EDID") != 0) continue;
        drm_mode_get_blob blob{};
        blob.blob_id = static_cast<uint32_t>(c.values[i]);
        if (!blob.blob_id || drmIoctl(k.fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) != 0 || blob.length < edid::kBlockBytes) return false;
        std::vector<uint8_t> data(blob.length);
        blob.data = ptr(data.data());
        if (drmIoctl(k.fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) != 0) return false;
        const uint8_t* ext = data.size() >= 2 * edid::kBlockBytes ? data.data() + edid::kBlockBytes : nullptr;
        return edid::parse(data.data(), ext, sizes, n, out);
    }
    return false;
}

bool up(uint32_t display, const HdmiMode& want, HdmiMode& running) {
    k.connector = display;
    k.crtc = pickCrtc(k.found);
    if (!k.crtc || !pickMode(k.found, want, k.timing)) { down(); return false; }
    for (Scanout& s : k.buf)
        if (!createScanout(s, k.timing.hdisplay, k.timing.vdisplay)) { down(); return false; }
    k.shown = 0;
    k.flips = true;
    k.flipPending = false;
    if (!setCrtc(0)) { down(); return false; }   // another program holds the screen, a desktop most often
    running = {k.timing.hdisplay, k.timing.vdisplay, static_cast<uint8_t>(k.timing.vrefresh)};
    std::printf("HDMI: %ux%u %u Hz on connector %u\n", static_cast<unsigned>(k.timing.hdisplay),
                static_cast<unsigned>(k.timing.vdisplay), static_cast<unsigned>(k.timing.vrefresh), static_cast<unsigned>(display));
    return true;
}

Surface back() {
    const Scanout& s = k.buf[k.flips ? 1 - k.shown : k.shown];
    return {s.map, s.pitch, k.timing.hdisplay, k.timing.vdisplay};
}

// Read the flip's event within `ms`: only flip events are asked for, so their content says nothing more.
void awaitFlip(int ms) {
    pollfd pfd{k.fd, POLLIN, 0};
    if (poll(&pfd, 1, ms) <= 0) return;
    char events[256];
    (void)!read(k.fd, events, sizeof(events));
    k.flipPending = false;
}

void present() {
    if (!k.flips) return;
    if (k.flipPending) awaitFlip(100);
    if (k.flipPending) return;   // the last flip still waits for its blank, a monitor waking from sleep: this frame waits too
    const int target = 1 - k.shown;
    drm_mode_crtc_page_flip flip{};
    flip.crtc_id = k.crtc;
    flip.fb_id = k.buf[target].fb;
    flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
    if (drmIoctl(k.fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) != 0) {
        if (errno == EBUSY) return;   // a flip still queued, so the next frame tries again
        k.flips = false;   // @xref{linux-straight-to-the-kernel|tearing rather than dark}
        if (setCrtc(target)) k.shown = target;
        return;
    }
    k.flipPending = true;
    k.shown = target;   // on screen from the next blank, whether or not its event arrives in time
    awaitFlip(100);
}

// Release the screen: the buffers go, and with the last framebuffer on it the kernel turns the CRTC off.
void down() {
    for (Scanout& s : k.buf) destroyScanout(s);
    k.connector = 0;
    k.crtc = 0;
}

void pump() {}

}  // namespace screen
#endif  // __linux__

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
    char device[CCHDEVICENAME] = {};
    bool modeChanged = false;
};
Win w;

constexpr const char* kClassName = "MoonLightHdmi";

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {   // uncovered: the last frame again
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (w.memory) BitBlt(dc, 0, 0, w.width, w.height, w.memory, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;   // the frame covers it all
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// The id of a display device: FNV-1a over its name, never 0.
uint32_t displayId(const char* device) {
    uint32_t hash = 2166136261u;
    for (const char* c = device; *c; c++) hash = (hash ^ static_cast<uint8_t>(*c)) * 16777619u;
    return hash ? hash : 1;
}

struct Pick {
    uint32_t current;
    uint32_t found = 0;
    char device[CCHDEVICENAME] = {};
};

BOOL CALLBACK pickMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM user) {
    Pick& p = *reinterpret_cast<Pick*>(user);
    MONITORINFOEXA info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(monitor, &info) || (info.dwFlags & MONITORINFOF_PRIMARY)) return TRUE;   // the desktop's own display is never covered
    const uint32_t id = displayId(info.szDevice);
    if (!p.found || id == p.current) { p.found = id; std::strncpy(p.device, info.szDevice, sizeof(p.device) - 1); }
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

uint32_t find(uint32_t current) {
    Pick p{current};
    EnumDisplayMonitors(nullptr, nullptr, pickMonitor, reinterpret_cast<LPARAM>(&p));
    if (p.found) std::memcpy(w.device, p.device, sizeof(w.device));
    return p.found;
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
    if (!EnumDisplayDevicesA(w.device, 0, &monitor, 0)) return false;
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
    if (w.window) { DestroyWindow(w.window); w.window = nullptr; }
    if (w.memory) { SelectObject(w.memory, w.previous); DeleteDC(w.memory); w.memory = nullptr; }
    if (w.dib) { DeleteObject(w.dib); w.dib = nullptr; }
    w.pixels = nullptr;
    if (w.modeChanged) { ChangeDisplaySettingsExA(w.device, nullptr, nullptr, 0, nullptr); w.modeChanged = false; }   // back to the registry's mode
}

void pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
}

}  // namespace screen
#endif  // _WIN32

// --- A build without a screen backend, such as GCC's on macOS: no display to show on ---
#if !defined(__linux__) && !defined(_WIN32) && !defined(MM_MAC_SCREEN)
namespace screen {
bool open() { return false; }
void close() {}
uint32_t find(uint32_t) { return 0; }
bool readEdid(uint32_t, const edid::Size*, size_t, edid::Monitor&) { return false; }
bool up(uint32_t, const HdmiMode&, HdmiMode&) { return false; }
Surface back() { return {}; }
void present() {}
void down() {}
void pump() {}
}  // namespace screen
#endif

// --- The seam ---

size_t hdmiModes(const HdmiMode** out) MM_NONBLOCKING {
    if (out) *out = kHdmiModes;
    return kModeCount;
}

bool hdmiOpen(uint8_t index) {
    if (index >= kModeCount) return false;
    if (!hdmiStandIn_) return outputOpen(index);
    hdmiOpenMode_ = index;
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
    out = hdmiActive_.width ? hdmiActive_ : kHdmiModes[hdmiOpenMode_];
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

// A desktop has no bridge to read back; the stand-in reports what a test set.
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
size_t hdmiTestFrameCount() { return hdmiFrames_; }
uint16_t hdmiTestFrameWidth() { return hdmiW_; }
uint16_t hdmiTestFrameHeight() { return hdmiH_; }
bool hdmiTestFrameFit() { return hdmiFit_; }
const uint8_t* hdmiTestFrameData() { return hdmiFrameBytes_ ? hdmiFrame_ : nullptr; }
void hdmiTestClear() {
    useStandIn();
    hdmiFrames_ = 0; hdmiFrameBytes_ = 0; hdmiW_ = hdmiH_ = 0; hdmiOpenMode_ = -1; hdmiLink_ = HdmiLink::NoMonitor; hdmiMonitorRead_ = false; hdmiBridgeRead_ = false; hdmiActive_ = {};
}

}  // namespace mm::platform
