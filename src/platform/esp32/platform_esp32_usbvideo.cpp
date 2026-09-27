/// USB video capture: the peripheral half of VideoService, owning the UVC stream and the JPEG decode.
///
/// @moreinfo
///
/// An HDMI grabber presents itself as a UVC webcam. See src/core/services/VideoService.h for the module that drives this.
/// MJPEG, because uncompressed does not fit: 640x480 YUY2 at 60 fps is 37 MB/s against a USB 2.0 host's ~24.6 MB/s.
///
/// ## Three threads
///
/// The USB host library is a per-application singleton, installed on first use and left running, with its event loop on a task of its own.
/// uvc_host_stream_open() blocks waiting for an enumeration that loop drives, so it cannot share a thread with init.
/// Decoding runs on a task of its own too, since jpeg_decoder_process() blocks and the render tick is MM_NONBLOCKING.
/// So videoCaptureFrame only reads an index, and the frame it names was decoded earlier by decoderTask.
/// A frame arriving while one is still pending is dropped, the newest being the only one worth having.
///
/// ## Who owns the buffers
///
/// The buffers are allocated in videoCaptureInit and freed in videoCaptureDeinit, both on the caller's thread.
/// Nothing in between touches the set, so no task can reallocate while another reads.
/// A device that goes away is not chased from here: its return re-enumerates, bumping the format generation, and the caller re-inits on that.
///
/// ## Bulk payload headers
///
/// The header stride is not exposed by the driver, so stripPayloadHeaders reads it off the first header, which can only sit 12 bytes before a packet end.
/// A candidate is confirmed by the header that must follow it one stride on, since entropy-coded bytes pass the field checks about once per 2^18 tries.

#include "platform/platform.h"

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4)

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace mm::platform {
namespace {

constexpr char kTag[] = "usbvideo";

constexpr int kSlots = 3;
static_assert(kSlots >= 3, "freeSlot() needs a spare beyond the published and the in-use slot");

struct Capture {
    uvc_host_stream_hdl_t stream = nullptr;
    jpeg_decoder_handle_t jpeg = nullptr;

    // The frame the UVC callback handed over, or null. Exchanged rather than assigned so the callback never blocks and never overwrites one the decoder is already reading.
    std::atomic<uvc_host_frame_t*> pending{nullptr};

    TaskHandle_t decoder = nullptr;
    SemaphoreHandle_t wake = nullptr;    // callback -> decoder
    SemaphoreHandle_t stopped = nullptr; // decoder -> deinit
    std::atomic<bool> running{false};

    // Decoded RGB888, from jpeg_alloc_decoder_mem for its cache-line and 2D-DMA alignment: a plain malloc shows up as intermittent corruption, not an error. Written once, in init, before the decoder task exists; read-only from then on.
    uint8_t* rgb[kSlots] = {};
    size_t rgbCap = 0;
    uint16_t width[kSlots] = {};
    uint16_t height[kSlots] = {};
    // ONE word, because reading which slot is newest and claiming it must be a single step, or the decoder could decode into a buffer being displayed. Packed (published+1) << 8 | (inUse+1), where 0 in a field means none.
    std::atomic<uint16_t> slots{0};

    static int pubOf(uint16_t s) { return static_cast<int>(s >> 8) - 1; }
    static int useOf(uint16_t s) { return static_cast<int>(s & 0xFF) - 1; }
    static uint16_t pack(int pub, int use) {
        return static_cast<uint16_t>(((pub + 1) << 8) | ((use + 1) & 0xFF));
    }
};

// What the attached device advertises. File scope rather than inside Capture because it is learned from the driver event, which fires before a stream exists and outlives a failed open.
constexpr size_t kMaxFormats = kVideoCaptureMaxFormats;
uvc_host_frame_info_t frameList[kMaxFormats]; // file scope: too big for the driver task's stack

// A seqlock whose generation is odd during a write, so a reader that sees it move across the copy retries. The payload is atomic rather than plain bytes, since two threads touching a non-atomic object concurrently is a data race.
struct FormatBank {
    std::atomic<uint16_t> width[kMaxFormats];
    std::atomic<uint16_t> height[kMaxFormats];
    std::atomic<uint8_t> fps[kMaxFormats];
    std::atomic<uint32_t> interval[kMaxFormats]; // as published: `fps` is rounded for the UI
    std::atomic<size_t> count{0};
};
FormatBank formatBank;
std::atomic<uint32_t> formatGen{0}; // 0 = nothing published yet; odd = a write in progress
// The device the bank describes, so the open reaches THAT one and not whichever the driver finds first when two are attached. 0 (UVC_HOST_ANY_DEV_ADDR) until a device has enumerated.
std::atomic<uint8_t> formatDevAddr{UVC_HOST_ANY_DEV_ADDR};
// Only the first streaming function of a device is ever opened, so only its list is published: a device exposing several would otherwise describe one function while another gets opened.
constexpr uint8_t kStreamIndex = 0;

bool hostReady = false;
bool uvcReady = false;

// usb_host_lib_handle_events() is where enumeration and the port state machine actually run, and it blocks. Nothing else may drive it, so this task owns it for the life of the application.
void pumpTask(void*) {
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

// Installed once and never uninstalled: the library is a singleton the whole application shares, and tearing it down on a source switch only risks leaving it un-reinstallable.
bool ensureUsbHost() {
    if (hostReady) return true;
    usb_host_config_t hostCfg = {};
    hostCfg.intr_flags = ESP_INTR_FLAG_LOWMED;
    if (usb_host_install(&hostCfg) != ESP_OK) {
        ESP_LOGE(kTag, "usb_host_install failed");
        return false;
    }
    // Priority 4 is below the UVC driver task, which consumes what this one produces. Unpinned, since the render loop is fixed to core 0, so leaving placement to the scheduler keeps USB off it.
    if (xTaskCreatePinnedToCore(pumpTask, "usbpump", 4 * 1024, nullptr, 4, nullptr, tskNO_AFFINITY) !=
        pdPASS) {
        ESP_LOGE(kTag, "no USB event task");
        usb_host_uninstall();
        return false;
    }
    hostReady = true;
    return true;
}

// Counted on the UVC and decode tasks, read from the render tick: relaxed is enough for a diagnostic.
std::atomic<uint32_t> statDecoded{0}, statBusy{0}, statInfoFail{0}, statOversize{0}, statNoSlot{0},
    statDecodeFail{0};

// Runs on the UVC driver task, so the ordinary FreeRTOS API is safe. It still only hands the frame over, since decoding here would stall the task that collects isochronous packets.
bool onFrame(const uvc_host_frame_t* frame, void* ctx) {
    auto* cap = static_cast<Capture*>(ctx);
    uvc_host_frame_t* expected = nullptr;
    // Take the slot only if it is free. Returning false keeps the frame, so the loser of this race must return true to hand it straight back or the driver runs out of buffers.
    if (!cap->pending.compare_exchange_strong(expected, const_cast<uvc_host_frame_t*>(frame))) {
        statBusy.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    xSemaphoreGive(cap->wake);
    return false;
}

// UVC states a rate as dwFrameInterval, a period in 100 ns ticks, so one second is 10 million of them. Rounded rather than truncated: 59.94 fps is a real rate and reads better as 60 than 59.
uint8_t fpsFrom(uint32_t interval) {
    constexpr uint32_t kTicksPerSecond = 10000000;
    return static_cast<uint8_t>((kTicksPerSecond + interval / 2) / interval);
}

// One dropdown row per (resolution, rate) pair. A device that does 320x240 at both 30 and 60 lists the resolution ONCE with several intervals, so without this expansion only its default is reachable from the UI.
void addAdvertised(const uvc_host_frame_info_t& info, uint32_t interval, size_t& n) {
    if (n >= kMaxFormats || interval == 0) return;
    const uint16_t w = static_cast<uint16_t>(info.h_res), h = static_cast<uint16_t>(info.v_res);
    const uint8_t fps = fpsFrom(interval);
    formatBank.width[n].store(w, std::memory_order_relaxed);
    formatBank.height[n].store(h, std::memory_order_relaxed);
    formatBank.fps[n].store(fps, std::memory_order_relaxed);
    formatBank.interval[n].store(interval, std::memory_order_relaxed);
    n++;
    ESP_LOGI(kTag, "offers MJPEG %ux%u @ %u fps", w, h, fps);
}

// Runs on the UVC driver task when a device enumerates, before any stream is opened. That is what makes the list available even when the open then fails on an unsupported resolution.
void onDriverEvent(const uvc_host_driver_event_data_t* event, void*) {
    if (event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) return;
    if (event->device_connected.uvc_stream_index != kStreamIndex) return; // never opened, so not listed

    const uint8_t devAddr = event->device_connected.dev_addr;
    size_t count = kMaxFormats;
    if (uvc_host_get_frame_list(devAddr, kStreamIndex, reinterpret_cast<uvc_host_frame_info_t(*)[]>(frameList),
                                &count) != ESP_OK) {
        ESP_LOGW(kTag, "device connected but its frame list could not be read");
        return;
    }
    if (count > kMaxFormats) count = kMaxFormats; // it reports what it NEEDS, not what it wrote

    // Bracket the rewrite in an odd generation, so a reader can tell it overlapped one.
    formatGen.fetch_add(1, std::memory_order_release);
    formatDevAddr.store(devAddr, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        const uvc_host_frame_info_t& info = frameList[i];
        if (info.format != UVC_VS_FORMAT_MJPEG) continue; // nothing else is decodable here
        if (info.interval_type == 0) { // a continuous range: offer both ends, fastest first
            addAdvertised(info, info.interval_min, n);
            if (info.interval_max != info.interval_min) addAdvertised(info, info.interval_max, n);
            continue;
        }
        const uint8_t rates = info.interval_type < CONFIG_UVC_INTERVAL_ARRAY_SIZE
                                  ? info.interval_type
                                  : CONFIG_UVC_INTERVAL_ARRAY_SIZE;
        for (uint8_t j = 0; j < rates; j++) addAdvertised(info, info.interval[j], n);
    }
    formatBank.count.store(n, std::memory_order_relaxed);
    formatGen.fetch_add(1, std::memory_order_release); // even again: the list is settled
}

// Runs on the UVC driver task, which pauses the stream before this fires, so the renderer sees a gap and its stale timeout takes it from there. Nothing to unwind: the caller re-inits on the next generation.
void onEvent(const uvc_host_stream_event_data_t* event, void*) {
    if (event->type == UVC_HOST_DEVICE_DISCONNECTED) ESP_LOGW(kTag, "capture device disconnected");
}

// RGB888 bytes the decoder writes for a w x h JPEG: both axes padded to the 16-pixel MCU (jpeg_decoder_process, note 2). Sized w*h*3, 800x600 overruns by 19200 bytes and every frame fails.
size_t decodedBytes(uint16_t w, uint16_t h) {
    const size_t aw = (static_cast<size_t>(w) + 15) & ~static_cast<size_t>(15);
    const size_t ah = (static_cast<size_t>(h) + 15) & ~static_cast<size_t>(15);
    return aw * ah * 3;
}

// One set of slots per open, sized from the format the device agreed to. Called from init only, before the decoder task exists, so nothing can be reading a slot while it is (re)written.
bool allocSlots(Capture& cap, uint16_t w, uint16_t h) {
    const size_t need = decodedBytes(w, h);
    jpeg_decode_memory_alloc_cfg_t memCfg = {};
    memCfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    for (int i = 0; i < kSlots; i++) {
        size_t got = 0;
        cap.rgb[i] = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(need, &memCfg, &got));
        if (!cap.rgb[i]) {
            cap.rgbCap = 0; // a partial set is no set: decode() must not write into a missing slot
            return false;   // deinit frees what was allocated
        }
        cap.rgbCap = got; // every slot gets the same request, so the same rounded-up size
    }
    return true;
}

// -1 when every slot is spoken for: unreachable while kSlots is 3, but returning a real index
// anyway would hand the decoder a buffer the render thread is reading. Corruption with no error is worse than a dropped frame.
int freeSlot(const Capture& cap) {
    const uint16_t s = cap.slots.load(std::memory_order_acquire);
    const int pub = Capture::pubOf(s), use = Capture::useOf(s);
    for (int i = 0; i < kSlots; i++)
        if (i != pub && i != use) return i;
    return -1;
}

// UVC payload header (UVC 1.5 2.4.3.3): bLength 12 when PTS and SCR are present, EOH set, SCR's top 5 reserved bits zero. `pts` pins later headers to the frame's first one: PTS is constant across one frame's payloads.
constexpr size_t kPayloadHeaderLen = 12;
constexpr size_t kBulkMps = 512; // high-speed bulk

bool isPayloadHeader(const uint8_t* h, size_t avail, const uint8_t* pts) {
    constexpr uint8_t kEoh = 0x80, kErr = 0x40, kScr = 0x08, kPts = 0x04, kEof = 0x02;
    return avail >= kPayloadHeaderLen && h[0] == kPayloadHeaderLen &&
           (h[1] & (kEoh | kErr | kScr | kPts | kEof)) == (kEoh | kScr | kPts) && !(h[11] & 0xF8) &&
           (!pts || memcmp(h + 2, pts, 4) == 0);
}

// Drops the payload headers usb_host_uvc leaves inside a bulk frame. uvc_bulk.c strips one only after a short transfer. Where the payload is a multiple of the packet size, every header after the first stays in at stride `payload`. See @moreinfo.
size_t stripPayloadHeaders(uint8_t* d, size_t len) {
    size_t hdr = 0;
    for (size_t i = kBulkMps - kPayloadHeaderLen; !hdr && i + kPayloadHeaderLen < len; i += kBulkMps) {
        if (!isPayloadHeader(d + i, len - i, nullptr)) continue;
        const size_t next = 2 * i + kPayloadHeaderLen;
        if (next + kPayloadHeaderLen > len || isPayloadHeader(d + next, len - next, d + i + 2)) hdr = i;
    }
    if (!hdr) return len;
    const size_t payload = hdr + kPayloadHeaderLen;
    uint8_t pts[4]; // copied: the compaction overwrites the header it came from
    memcpy(pts, d + hdr + 2, sizeof pts);
    size_t w = hdr, r = hdr;
    while (isPayloadHeader(d + r, len - r, pts)) {
        r += kPayloadHeaderLen;
        const size_t run = std::min(payload - kPayloadHeaderLen, len - r);
        memmove(d + w, d + r, run);
        w += run;
        r += run;
    }
    memmove(d + w, d + r, len - r); // whatever follows a missing header is left as delivered
    return w + (len - r);
}

// Warn on the FIRST of each kind only. A drop repeats at frame rate, so logging every one buries the log and costs more than the fault; the counters carry the rate.
void warnOnce(bool& said, const char* what) {
    if (said) return;
    said = true;
    ESP_LOGW(kTag, "%s - first occurrence; see the Video status for the running count", what);
}

void decode(Capture& cap, uvc_host_frame_t* frame) {
    static bool saidInfo = false, saidOversize = false, saidNoSlot = false, saidDecode = false;
    const size_t len = stripPayloadHeaders(frame->data, frame->data_len);
    // Dimensions from the bitstream, not from the request: a device may negotiate something else.
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(frame->data, len, &info) != ESP_OK) {
        statInfoFail.fetch_add(1, std::memory_order_relaxed);
        warnOnce(saidInfo, "frame is not readable JPEG (payload stride mis-detected?)");
        return;
    }
    if (decodedBytes(static_cast<uint16_t>(info.width), static_cast<uint16_t>(info.height)) > cap.rgbCap) {
        statOversize.fetch_add(1, std::memory_order_relaxed);
        warnOnce(saidOversize, "frame exceeds the buffers sized at open");
        ESP_LOGW(kTag, "frame %ux%u exceeds the buffers sized at open", info.width, info.height);
        return;
    }

    const int slot = freeSlot(cap);
    if (slot < 0) { // drop the frame rather than write over one being read
        statNoSlot.fetch_add(1, std::memory_order_relaxed);
        warnOnce(saidNoSlot, "renderer holds every slot, dropping the newest frame");
        return;
    }

    jpeg_decode_cfg_t decodeCfg = {};
    decodeCfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
    decodeCfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
    decodeCfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t outSize = 0;
    if (jpeg_decoder_process(cap.jpeg, &decodeCfg, frame->data, len, cap.rgb[slot], cap.rgbCap, &outSize) !=
        ESP_OK) {
        statDecodeFail.fetch_add(1, std::memory_order_relaxed);
        warnOnce(saidDecode, "the decoder refused a bitstream whose header it accepted");
        return;
    }
    statDecoded.fetch_add(1, std::memory_order_relaxed);

    cap.width[slot] = static_cast<uint16_t>(info.width);
    cap.height[slot] = static_cast<uint16_t>(info.height);
    // Preserve whatever the renderer claimed while the decode ran. Pixels and dimensions are written first, and the release makes them visible to whoever acquires this.
    uint16_t cur = cap.slots.load(std::memory_order_relaxed);
    while (!cap.slots.compare_exchange_weak(cur, Capture::pack(slot, Capture::useOf(cur)),
                                            std::memory_order_release, std::memory_order_relaxed)) {
    }
}

// The blocking half, kept off the render tick. Waits on a finite timeout rather than forever so `running` is seen without the callback having to signal.
void decoderTask(void* arg) {
    auto* cap = static_cast<Capture*>(arg);
    while (cap->running.load()) {
        if (xSemaphoreTake(cap->wake, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (uvc_host_frame_t* frame = cap->pending.exchange(nullptr)) {
            decode(*cap, frame);
            uvc_host_frame_return(cap->stream, frame);
        }
    }
    xSemaphoreGive(cap->stopped);
    vTaskDelete(nullptr);
}

// Priority 6 puts it above the UVC driver task: a decode that runs late holds the only free frame buffer, which is what starves the driver.
bool startDecoder(Capture& cap) {
    cap.running = true;
    if (xTaskCreatePinnedToCore(decoderTask, "usbjpeg", 4 * 1024, &cap, 6, &cap.decoder, tskNO_AFFINITY) ==
        pdPASS)
        return true;
    cap.running = false;
    ESP_LOGE(kTag, "no decode task");
    return false;
}

// Installed once, like the host library above it: the format list belongs to the bus, not to one open. Reinstalling per open would re-enumerate the attached device and bump the format generation, which is the very signal the caller re-inits on.
bool ensureUvcHost() {
    if (uvcReady) return true;
    uvc_host_driver_config_t driverCfg = {};
    driverCfg.driver_task_stack_size = 4 * 1024;
    driverCfg.driver_task_priority = 5;
    driverCfg.xCoreID = tskNO_AFFINITY;
    driverCfg.create_background_task = true;
    driverCfg.event_cb = onDriverEvent; // fills the format bank as soon as a device enumerates
    if (uvc_host_install(&driverCfg) != ESP_OK) {
        ESP_LOGE(kTag, "uvc_host_install failed");
        return false;
    }
    uvcReady = true;
    return true;
}

bool createJpeg(Capture& cap) {
    jpeg_decode_engine_cfg_t jpegCfg = {};
    // 200, not 40: the timeout aborts the 2D-DMA mid-frame, and writing 6.2 MB of 1080p RGB into PSRAM alone takes ~34 ms. At 40 ms only 14% of intact frames survived.
    jpegCfg.timeout_ms = 200;
    if (jpeg_new_decoder_engine(&jpegCfg, &cap.jpeg) == ESP_OK) return true;
    ESP_LOGE(kTag, "no JPEG decoder engine");
    return false;
}

bool createSignals(Capture& cap) {
    cap.wake = xSemaphoreCreateBinary();
    cap.stopped = xSemaphoreCreateBinary();
    if (cap.wake && cap.stopped) return true;
    ESP_LOGE(kTag, "no semaphores");
    return false;
}

// The published interval of a row as the float the driver compares against; 0 (device default) when the row is unknown, so the request still opens at that resolution.
float exactFpsFor(uint16_t w, uint16_t h, uint8_t fps) {
    const size_t n = formatBank.count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n && i < kMaxFormats; i++) {
        if (formatBank.width[i].load(std::memory_order_relaxed) != w) continue;
        if (formatBank.height[i].load(std::memory_order_relaxed) != h) continue;
        if (formatBank.fps[i].load(std::memory_order_relaxed) != fps) continue;
        const uint32_t iv = formatBank.interval[i].load(std::memory_order_relaxed);
        if (iv) return 10000000.0f / static_cast<float>(iv);
    }
    return 0.0f;
}

bool openStream(Capture& cap, uint16_t width, uint16_t height, uint8_t fps) {
    uvc_host_stream_config_t streamCfg = {};
    streamCfg.event_cb = onEvent;
    streamCfg.frame_cb = onFrame;
    streamCfg.user_ctx = &cap;
    // The device whose formats are on offer, once one has enumerated. Any device before that, and its enumeration bumps the generation, so the caller comes back and opens it by address.
    streamCfg.usb.dev_addr = formatDevAddr.load(std::memory_order_relaxed);
    streamCfg.usb.vid = UVC_HOST_ANY_VID;
    streamCfg.usb.pid = UVC_HOST_ANY_PID;
    streamCfg.usb.uvc_stream_index = kStreamIndex;
    streamCfg.vs_format.h_res = width;
    streamCfg.vs_format.v_res = height;
    // The device's own interval, not the rounded fps: the driver matches within 0.0001 fps, so a 59.94 mode never matches a requested 60.
    streamCfg.vs_format.fps = exactFpsFor(width, height, fps);
    streamCfg.vs_format.format = UVC_VS_FORMAT_MJPEG;
    // urb_size left at 0 (4x MPS): the driver's default, and every urb is internal SRAM. number_of_urbs has no default: 0 is malloc(0), and the open fails with ESP_ERR_NO_MEM.
    streamCfg.advanced.number_of_urbs = 4;
    streamCfg.advanced.number_of_frame_buffers = 3;
    // frame_size 0 means dwMaxVideoFrameSize, the UNCOMPRESSED size: 3 x 4.1 MB at 1080p for MJPEG frames of 40-76 KB. Half of it still leaves an order of magnitude of headroom.
    streamCfg.advanced.frame_size = static_cast<size_t>(width) * height / 2;
    streamCfg.advanced.frame_heap_caps = MALLOC_CAP_SPIRAM; // keep the internal heap for USB and WiFi

    // Wait rather than fail: the host enumerates asynchronously, so a device plugged in at boot is usually not ready when this runs. The driver takes ticks, not milliseconds.
    if (uvc_host_stream_open(&streamCfg, pdMS_TO_TICKS(3000), &cap.stream) != ESP_OK) {
        ESP_LOGW(kTag, "no UVC device offering MJPEG %ux%u", width, height);
        return false;
    }
    uvc_host_desc_print(cap.stream); // the format list, for the bench
    return true;
}

// Sized from what the device agreed to, not from what we asked for, so no frame can arrive needing more room than the slots have.
bool sizeBuffers(Capture& cap) {
    uvc_host_stream_format_t got = {};
    if (uvc_host_stream_format_get(cap.stream, &got) != ESP_OK) {
        ESP_LOGE(kTag, "cannot read the negotiated format");
        return false;
    }
    ESP_LOGI(kTag, "streaming MJPEG %ux%u @ %.1f fps", got.h_res, got.v_res, got.fps);
    if (allocSlots(cap, static_cast<uint16_t>(got.h_res), static_cast<uint16_t>(got.v_res))) return true;
    ESP_LOGE(kTag, "no memory for %d decode buffers", kSlots);
    return false;
}

} // namespace

bool videoCaptureInit(VideoCaptureHandle& handle, uint16_t width, uint16_t height, uint8_t fps) {
    if (handle.impl) return true;
    if (!ensureUsbHost() || !ensureUvcHost()) return false;
    auto* cap = new Capture();
    handle.impl = cap; // every failure below unwinds through videoCaptureDeinit

    // In this order on purpose. The decoder task starts LAST, once every buffer it can reach exists, and the stream after it, so the first frame finds a task to wake.
    const bool ok = createJpeg(*cap) && createSignals(*cap) && openStream(*cap, width, height, fps) &&
                    sizeBuffers(*cap) && startDecoder(*cap) && uvc_host_stream_start(cap->stream) == ESP_OK;
    if (!ok) videoCaptureDeinit(handle);
    return ok;
}

// Hot path: an index load and two field reads. Everything expensive already happened on decoderTask, which is what lets the render tick stay MM_NONBLOCKING.
const uint8_t* videoCaptureFrame(VideoCaptureHandle& handle, uint16_t& width,
                                 uint16_t& height) MM_NONBLOCKING {
    auto* cap = static_cast<Capture*>(handle.impl);
    if (!cap) return nullptr;
    // Read and claim in one step. The loop runs again only if the decoder published meanwhile, and then hands back that newer frame.
    uint16_t cur = cap->slots.load(std::memory_order_acquire);
    int slot;
    do {
        slot = Capture::pubOf(cur);
        // Consecutive publishes land in different slots, so an unchanged one means no new frame.
        if (slot < 0 || slot == Capture::useOf(cur)) return nullptr;
    } while (!cap->slots.compare_exchange_weak(cur, Capture::pack(slot, slot),
                                               std::memory_order_acq_rel, std::memory_order_acquire));
    width = cap->width[slot];
    height = cap->height[slot];
    return cap->rgb[slot];
}

uint32_t videoCaptureFormatGeneration() { return formatGen.load(std::memory_order_acquire); }

size_t videoCaptureFormats(VideoCaptureFormat* out, size_t max) {
    for (;;) {
        const uint32_t before = formatGen.load(std::memory_order_acquire);
        if (before == 0) return 0;      // nothing published yet
        if (before & 1u) continue;      // a write is in progress; let it finish
        size_t n = formatBank.count.load(std::memory_order_relaxed);
        if (n > kMaxFormats) n = kMaxFormats; // a torn read of a rewrite in flight
        if (n > max) n = max;
        for (size_t i = 0; i < n; i++) {
            out[i].width = formatBank.width[i].load(std::memory_order_relaxed);
            out[i].height = formatBank.height[i].load(std::memory_order_relaxed);
            out[i].fps = formatBank.fps[i].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (formatGen.load(std::memory_order_relaxed) == before) return n; // no write overlapped
    }
}

void videoCaptureDeinit(VideoCaptureHandle& handle) {
    auto* cap = static_cast<Capture*>(handle.impl);
    if (!cap) return;
    // Stop the stream first so no frame lands mid-teardown, then the task that would decode it. The join is unbounded on purpose, and the decoder's own 40 ms timeout bounds how long it takes.
    if (cap->stream) uvc_host_stream_stop(cap->stream);
    if (cap->decoder) {
        cap->running = false;
        xSemaphoreGive(cap->wake); // it may be parked on this
        xSemaphoreTake(cap->stopped, portMAX_DELAY);
    }
    if (uvc_host_frame_t* frame = cap->pending.exchange(nullptr)) uvc_host_frame_return(cap->stream, frame);
    if (cap->stream) uvc_host_stream_close(cap->stream);
    if (cap->jpeg) jpeg_del_decoder_engine(cap->jpeg);
    if (cap->wake) vSemaphoreDelete(cap->wake);
    if (cap->stopped) vSemaphoreDelete(cap->stopped);
    for (uint8_t* buf : cap->rgb) free(buf); // free(nullptr) is a no-op, so a partial set is fine
    delete cap;
    handle.impl = nullptr;
}

VideoCaptureStats videoCaptureStats() {
    VideoCaptureStats s;
    s.decoded = statDecoded.load(std::memory_order_relaxed);
    s.busy = statBusy.load(std::memory_order_relaxed);
    s.infoFail = statInfoFail.load(std::memory_order_relaxed);
    s.oversize = statOversize.load(std::memory_order_relaxed);
    s.noSlot = statNoSlot.load(std::memory_order_relaxed);
    s.decodeFail = statDecodeFail.load(std::memory_order_relaxed);
    return s;
}

} // namespace mm::platform

#else // every other target: no High-Speed USB host, no JPEG decoder

namespace mm::platform {

bool videoCaptureInit(VideoCaptureHandle&, uint16_t, uint16_t, uint8_t) { return false; }
size_t videoCaptureFormats(VideoCaptureFormat*, size_t) { return 0; }
uint32_t videoCaptureFormatGeneration() { return 0; }
const uint8_t* videoCaptureFrame(VideoCaptureHandle&, uint16_t&, uint16_t&) MM_NONBLOCKING { return nullptr; }
void videoCaptureDeinit(VideoCaptureHandle&) {}
VideoCaptureStats videoCaptureStats() { return {}; }

} // namespace mm::platform

#endif
