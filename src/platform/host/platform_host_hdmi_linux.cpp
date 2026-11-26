#include "platform/platform.h"

/// The Linux screen for the HDMI output: a connector, a mode and two buffers that flip on the vertical blank, or the primary plane scaled by the display.
///
/// The loop, the software scaler and the handover it serves live in platform_host_hdmi.cpp, where they are shared with Windows and macOS.
///
/// @technical
///
/// ## Linux: straight to the kernel
///
/// DRM/KMS through the kernel's own ioctls and uapi headers, the stable ABI libdrm wraps, so the build and the package stay library-free: the no-SDK rule the audio and NDI paths follow. Every list the kernel returns comes in two calls, the counts first and then the arrays, as libdrm does it.
///
/// The first connected HDMI connector is shown, drawn into the back one of two buffers that flip on the vertical blank; a driver where the flip fails draws into the shown buffer instead, tearing rather than going dark.
///
/// While no monitor is up the ask re-probes the connectors; while one is up it reads the state the kernel's hot-plug interrupt keeps, so a running monitor's EDID is not re-read every second.
///
/// ## Linux: the console takes the screen back
///
/// On a bare console the first program to open the card sets its mode, and closing the card hands the screen back to the text console. With a host running, the host holds the screen and the link reports Down, retried each second.
///
/// ## The display scales
///
/// A display controller scales a plane between its source and screen rectangles, as video players use it. The frame goes on the primary plane in a buffer the size of the layer, the CRTC's black background around it forming the bars: one pass over the frame a frame.
///
/// The primary is the plane sure to scale: an overlay may be a window that lacks a scaler, as the RK3566's Smart windows are, which takes the setting and stays dark; its primary, Esmart0-win0, scales.
///
/// A new frame size keeps the replaced buffers until one of the new size is on the plane, since removing the shown buffer turns the display off.
///
/// A display that refuses the scaling is told by the plane's first set, gets the screen-sized buffer back and keeps the software scaler.

#ifdef __linux__

#include "screen.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace mm::platform {

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

// The primary plane scaled by the display controller: two buffers the size of the frame, shown in turn.
struct Plane {
    uint32_t id = 0;       // the plane, 0 while none is found
    bool tried = false;    // looked for since the display came up
    bool works = false;    // the display scales on it; false falls back to scaling in software
    Scanout buf[2];
    Scanout old[2];        // the pair a resize replaced, removed once a new buffer is on the plane: removing the shown one turns the display off
    int next = 0;          // the buffer the next frame goes into
    uint16_t w = 0, h = 0; // the frame size the buffers hold
    bool shown = false;    // a frame-sized buffer is on the plane, so dropping it puts the screen's back
};

struct Kms {
    int fd = -1;
    Plane plane;
    Connector found;                 // the connector find() returned, for readEdid() and up()
    uint32_t connector = 0;
    uint32_t crtc = 0;
    drm_mode_modeinfo timing{};      // the mode the monitor runs
    Scanout buf[2];
    int shown = 0;                   // the buffer on screen
    bool flips = true;               // false once a flip failed: draw into the shown buffer
    bool rebindFailed = false;       // the busy-CRTC rebind failed and was logged; a success clears it
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

// The card's CRTCs and connectors; false for a render-only card, such as the Pi's 3D engine.
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

// The planes the card lists, the primary among them once the universal-planes cap is set.
std::vector<uint32_t> planes() {
    drm_set_client_cap cap{DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1};
    drmIoctl(k.fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);   // a kernel that lists every plane already refuses it, harmlessly
    drm_mode_get_plane_res res{};
    if (drmIoctl(k.fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res) != 0 || res.count_planes == 0) return {};
    std::vector<uint32_t> ids(res.count_planes);
    res.plane_id_ptr = ptr(ids.data());
    if (drmIoctl(k.fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res) != 0) return {};
    return ids;
}

// Plane `id`'s property `name` into `value`; false where it has none.
bool planeProp(uint32_t id, const char* name, uint64_t& value) {
    drm_mode_obj_get_properties props{};
    props.obj_id = id;
    props.obj_type = DRM_MODE_OBJECT_PLANE;
    if (drmIoctl(k.fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) != 0 || props.count_props == 0) return false;
    std::vector<uint32_t> ids(props.count_props);
    std::vector<uint64_t> values(props.count_props);
    props.props_ptr = ptr(ids.data());
    props.prop_values_ptr = ptr(values.data());
    if (drmIoctl(k.fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) != 0) return false;
    for (size_t i = 0; i < ids.size(); i++) {
        drm_mode_get_property p{};
        p.prop_id = ids[i];
        if (drmIoctl(k.fd, DRM_IOCTL_MODE_GETPROPERTY, &p) == 0 && std::strcmp(p.name, name) == 0) { value = values[i]; return true; }
    }
    return false;
}

// The primary plane scanning the CRTC in use, the full window with a scaler that already shows the screen, 0 for none.
uint32_t findPrimary() {
    for (uint32_t id : planes()) {
        drm_mode_get_plane p{};
        p.plane_id = id;
        uint64_t type = 0;
        if (drmIoctl(k.fd, DRM_IOCTL_MODE_GETPLANE, &p) == 0 && p.crtc_id == k.crtc && planeProp(id, "type", type) && type == 1) return id;   // 1: primary, as the kernel numbers plane types
    }
    return 0;
}

// Put the screen-sized buffer back on the primary plane and free the frame-sized ones.
void dropPlane() {
    Plane& pl = k.plane;
    // The software path draws into the screen's buffers from here on; a bind that fails is made again by the next frame's flip (rebindShown).
    if (pl.shown && !setCrtc(k.shown)) std::printf("HDMI: the screen's buffer did not bind (errno %d), the next frame binds it again\n", errno);
    for (Scanout& s : pl.buf) destroyScanout(s);
    for (Scanout& s : pl.old) destroyScanout(s);
    pl = Plane{};
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

bool takeDismiss() { return false; }   // the console's screen has no desktop under it

// The connected HDMI connector, the one shown kept first so a second monitor does not take its place.
uint32_t find(uint32_t current, bool /*own*/) {   // the console's screen is the output already
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
    if (!setCrtc(0)) { down(); return false; }   // another program holds the screen, a host most often
    running = {k.timing.hdisplay, k.timing.vdisplay, static_cast<uint8_t>(k.timing.vrefresh)};
    std::printf("HDMI: %ux%u %u Hz on connector %u\n", static_cast<unsigned>(k.timing.hdisplay),
                static_cast<unsigned>(k.timing.vdisplay), static_cast<unsigned>(k.timing.vrefresh), static_cast<unsigned>(display));
    return true;
}

// Give up on the plane for this display: the loop scales in software until the display comes up again.
bool planeRefused() {
    dropPlane();
    k.plane.tried = true;
    return false;
}

// The plane found and its two buffers sized to `w` × `h`; false where there is none or they cannot be made.
bool planeReady(uint16_t w, uint16_t h) {
    Plane& pl = k.plane;
    if (pl.tried && !pl.works) return false;
    if (!pl.tried) { pl.tried = true; pl.id = findPrimary(); pl.works = pl.id != 0; }
    if (!pl.works) return false;
    if (w == pl.w && h == pl.h) return true;
    for (int i = 0; i < 2; i++) {
        destroyScanout(pl.old[i]);   // a pair replaced before any frame of the last size went up
        pl.old[i] = pl.buf[i];
        pl.buf[i] = Scanout{};
    }
    if (!createScanout(pl.buf[0], w, h) || !createScanout(pl.buf[1], w, h)) return planeRefused();
    pl.w = w;
    pl.h = h;
    return true;
}

// An RGB frame into a scanout buffer as XRGB8888, row by row at the buffer's pitch.
void fillXrgb(const Scanout& s, const uint8_t* rgb, uint16_t w, uint16_t h) {
    for (uint16_t y = 0; y < h; y++) {
        uint32_t* line = reinterpret_cast<uint32_t*>(s.map + static_cast<size_t>(y) * s.pitch);
        const uint8_t* p = rgb + static_cast<size_t>(y) * w * 3;
        for (uint16_t x = 0; x < w; x++, p += 3) line[x] = 0xFF000000u | (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2];   // the X byte opaque, for a plane that reads it as alpha
    }
}

// The frame on the primary plane, scaled by the display onto the rectangle, the CRTC's background around it.
bool presentScaled(const uint8_t* rgb, uint16_t w, uint16_t h, uint16_t x0, uint16_t y0, uint16_t outW, uint16_t outH) {
    if (!planeReady(w, h)) return false;
    Plane& pl = k.plane;
    const Scanout& s = pl.buf[pl.next];
    fillXrgb(s, rgb, w, h);
    drm_mode_set_plane set{};
    set.plane_id = pl.id;
    set.crtc_id = k.crtc;
    set.fb_id = s.fb;
    set.crtc_x = x0;
    set.crtc_y = y0;
    set.crtc_w = outW;
    set.crtc_h = outH;
    set.src_w = uint32_t{w} << 16;   // the source rectangle in 16.16
    set.src_h = uint32_t{h} << 16;
    if (drmIoctl(k.fd, DRM_IOCTL_MODE_SETPLANE, &set) != 0) {   // no scaling on this plane, or no such size: scale in software from here on
        std::printf("HDMI: the display does not scale %ux%u onto %ux%u, so MoonLight scales it\n", unsigned{w}, unsigned{h}, unsigned{outW}, unsigned{outH});
        return planeRefused();
    }
    for (Scanout& old : pl.old) destroyScanout(old);   // the plane shows a buffer of the new size now
    if (!pl.shown) {
        pl.shown = true;
        std::printf("HDMI: the display scales %ux%u onto %ux%u on plane %u\n", unsigned{w}, unsigned{h}, unsigned{outW}, unsigned{outH}, unsigned{pl.id});
    }
    pl.next ^= 1;
    return true;
}

Surface back() {
    const Scanout& s = k.buf[k.flips ? 1 - k.shown : k.shown];
    return {s.map, s.pitch, k.timing.hdisplay, k.timing.vdisplay};
}

// Read the flip's event within `ms`: only flip events are asked for, so the event's arrival is the whole answer.
void awaitFlip(int ms) {
    pollfd pfd{k.fd, POLLIN, 0};
    if (poll(&pfd, 1, ms) <= 0) return;
    char events[256];
    (void)!read(k.fd, events, sizeof(events));
    k.flipPending = false;
}

// Bind the shown buffer to the CRTC again, logging the first failure of a run of them.
void rebindShown() {
    if (setCrtc(k.shown)) { k.rebindFailed = false; return; }
    if (k.rebindFailed) return;
    k.rebindFailed = true;
    std::printf("HDMI: binding the shown buffer again failed (errno %d)\n", errno);
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
        if (errno == EBUSY) {
            // With the last flip done, a busy CRTC has lost its framebuffer, as a dropped plane whose modeset failed leaves it: bind the shown buffer again, and the flips go on.
            rebindShown();
            return;
        }
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
    dropPlane();
    for (Scanout& s : k.buf) destroyScanout(s);
    k.connector = 0;
    k.crtc = 0;
}

void pump() {}

}  // namespace screen

}  // namespace mm::platform

#endif  // __linux__
