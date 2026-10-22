/// USB video capture on the ESP32-P4: the UVC stream and the JPEG decode behind the capture seam.

#include "platform/platform.h"
#include "platform/usbvideo_frames.h"   // the hardware-free half: headers, mode choice, the shrink

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CONFIG_MM_USB_VIDEO)

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cstring>
#include <new>   // std::nothrow: the format list is optional, a failed install says so

namespace mm::platform {
namespace {

constexpr char kTag[] = "usbvideo";
// A grabber lists its modes largest first, so a cap that is too low hides exactly the cheap ones.
constexpr size_t kMaxFormats = 64;

struct Capture {
    uvc_host_stream_hdl_t stream = nullptr;
    jpeg_decoder_handle_t jpeg = nullptr;
    /// The frame the UVC callback handed over, or null; exchanged so the callback never blocks.
    std::atomic<uvc_host_frame_t*> pending{nullptr};
    SemaphoreHandle_t wake = nullptr;   ///< the UVC callback to whoever awaits a frame
    /// Decoded RGB888 at the negotiated size, from jpeg_alloc_decoder_mem for its DMA alignment.
    uint8_t* rgb = nullptr;
    size_t rgbCap = 0;
    uint16_t negW = 0, negH = 0;   ///< the size the device agreed to stream
    /// Set by the event callback when the driver pauses the stream, cleared by the capture task that restarts it.
    std::atomic<bool> restart{false};
    /// One log per fault per open, beside the counters they explain, so a re-open names a recurring fault again.
    bool saidInfo = false, saidOversize = false, saidDecode = false, saidStride = false, saidShrink = false;
};

/// A seqlock whose generation is odd during a write, so a reader seeing it move across the copy retries.
struct FormatBank {
    std::atomic<uint16_t> width[kMaxFormats];
    std::atomic<uint16_t> height[kMaxFormats];
    std::atomic<uint8_t> fps[kMaxFormats];
    std::atomic<uint32_t> interval[kMaxFormats];   ///< as published, since `fps` is rounded for the UI
    std::atomic<size_t> count{0};
};

/// What the capture keeps while it is installed, about 3 KB: on the heap, so a board that never selects the capture holds none of it.
struct Bus {
    uvc_host_frame_info_t frameList[kMaxFormats];   // too big for the driver task's stack
    FormatBank bank;
    usbvideo::Mode snapModes[kMaxFormats];   ///< the bank's last snapshot, taken on the render thread only
    uint32_t snapIntervals[kMaxFormats];
};
Bus* bus = nullptr;   // allocated by the install, freed by videoCaptureShutdown
// Only grows, so a snapshot of one device can never be confirmed by another's; odd while a write is in progress.
std::atomic<uint32_t> formatGen{0};
// The device the bank describes, so the open reaches THAT one rather than whichever the driver finds first.
std::atomic<uint8_t> formatDevAddr{UVC_HOST_ANY_DEV_ADDR};
constexpr uint8_t kStreamIndex = 0;   // only a device's first streaming function is ever opened

bool hostReady = false;
bool uvcReady = false;
std::atomic<bool> pumpRunning{false};   // until the pump has uninstalled the host library
bool claimed = false;   // one owner at a time, as a device is claimed once

// Counted on the UVC task, read from the render tick: relaxed is enough for a diagnostic.
std::atomic<uint32_t> statDecoded{0}, statDropped{0}, statFailed{0};

// usb_host_lib_handle_events() is where enumeration and the port state machine run, and it blocks. The UVC driver is the only client, so its uninstall ends the pump, which uninstalls the library, as the MIDI host does.
void pumpTask(void*) {
    bool noClients = false;
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            noClients = true;
            // Power the port off first: a grabber still on it is enumerated again while the stack is torn down, and the uninstall aborts.
            usb_host_lib_set_root_port_power(false);
            if (usb_host_device_free_all() == ESP_OK) break;   // nothing left to free
        }
        if ((flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) && noClients) break;
    }
    if (usb_host_uninstall() != ESP_OK) ESP_LOGE(kTag, "USB host did not uninstall");
    pumpRunning.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

// Installed while the capture is the source, so the format list fills as a device enumerates and an open fails fast; videoCaptureShutdown removes it.
bool ensureUsbHost() {
    if (hostReady) return true;
    if (pumpRunning.load(std::memory_order_acquire)) return false;   // the last shutdown is still tearing down
    if (!bus) bus = new (std::nothrow) Bus();
    if (!bus) {
        ESP_LOGE(kTag, "no memory for the format list");
        return false;
    }
    usb_host_config_t hostCfg = {};
    hostCfg.intr_flags = ESP_INTR_FLAG_LOWMED;
    if (usb_host_install(&hostCfg) != ESP_OK) {
        ESP_LOGE(kTag, "usb_host_install failed");
        return false;
    }
    // Core 1, with the UVC driver and the capture task, so no USB work lands on the render core.
    pumpRunning.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(pumpTask, "usbpump", 4 * 1024, nullptr, 4, nullptr, 1) != pdPASS) {
        pumpRunning.store(false, std::memory_order_release);
        ESP_LOGE(kTag, "no USB event task");
        usb_host_uninstall();
        return false;
    }
    hostReady = true;
    return true;
}

// Runs on the UVC driver task, so the ordinary API is safe; it only hands the frame over.
bool onFrame(const uvc_host_frame_t* frame, void* ctx) {
    auto* cap = static_cast<Capture*>(ctx);
    uvc_host_frame_t* expected = nullptr;
    // Take the slot only if free. Returning false keeps the frame, so the loser must return true or the driver runs dry.
    if (!cap->pending.compare_exchange_strong(expected, const_cast<uvc_host_frame_t*>(frame))) {
        statDropped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    xSemaphoreGive(cap->wake);
    return false;
}

// One row per (resolution, rate) pair: a device listing a resolution once with several intervals would otherwise offer only its default.
void addAdvertised(const uvc_host_frame_info_t& info, uint32_t interval, size_t& n) {
    if (interval == 0) return;
    if (n >= kMaxFormats) {
        // Named here rather than on the descriptor count, which a device's rates multiply before this fills.
        ESP_LOGW(kTag, "mode list full at %u rates; the rest are not offered", static_cast<unsigned>(kMaxFormats));
        return;
    }
    const uint16_t w = static_cast<uint16_t>(info.h_res), h = static_cast<uint16_t>(info.v_res);
    const uint8_t fps = usbvideo::fpsFrom(interval);
    bus->bank.width[n].store(w, std::memory_order_relaxed);
    bus->bank.height[n].store(h, std::memory_order_relaxed);
    bus->bank.fps[n].store(fps, std::memory_order_relaxed);
    bus->bank.interval[n].store(interval, std::memory_order_relaxed);
    n++;
    ESP_LOGI(kTag, "offers MJPEG %ux%u @ %u fps", w, h, fps);
}

// A continuous-range device names no rate of its own, so each rate a picture asks for becomes a rung.
void addRange(const uvc_host_frame_info_t& info, size_t& n) {
    for (uint8_t rate : usbvideo::kOfferedRates) {
        const uint32_t rung = usbvideo::rungFor(rate, info.interval_min, info.interval_max, info.interval_step);
        if (rung) addAdvertised(info, rung, n);
    }
}

// Every MJPEG row of the read frame list, expanded one row per rate, answering how many were kept.
size_t publishMjpegRows(size_t count) {
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        const uvc_host_frame_info_t& info = bus->frameList[i];
        if (info.format != UVC_VS_FORMAT_MJPEG) continue;   // nothing else is decodable here
        if (info.interval_type == 0) {   // a continuous range: the rungs a picture asks for, not only its ends
            addRange(info, n);
            continue;
        }
        const uint8_t rates = info.interval_type < CONFIG_UVC_INTERVAL_ARRAY_SIZE ? info.interval_type
                                                                                  : CONFIG_UVC_INTERVAL_ARRAY_SIZE;
        for (uint8_t j = 0; j < rates; j++) addAdvertised(info, info.interval[j], n);
    }
    return n;
}

// Runs on the UVC driver task when a device enumerates, before a stream is opened, which is what makes the list available even when the open then fails.
void onDriverEvent(const uvc_host_driver_event_data_t* event, void*) {
    if (event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) return;
    if (event->device_connected.uvc_stream_index != kStreamIndex) return;

    const uint8_t devAddr = event->device_connected.dev_addr;
    const size_t offered = event->device_connected.frame_info_num;
    if (offered > kMaxFormats) ESP_LOGW(kTag, "device offers %u modes; keeping the first %u",
                                        static_cast<unsigned>(offered), static_cast<unsigned>(kMaxFormats));
    std::memset(bus->frameList, 0, sizeof bus->frameList);   // a short list must not read the last device's rows
    size_t count = kMaxFormats;
    if (uvc_host_get_frame_list(devAddr, kStreamIndex,
                                reinterpret_cast<uvc_host_frame_info_t(*)[]>(bus->frameList), &count) != ESP_OK) {
        ESP_LOGW(kTag, "device connected but its frame list could not be read");
        return;
    }

    formatGen.fetch_add(1, std::memory_order_release);   // odd: a rewrite is in progress
    formatDevAddr.store(devAddr, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const size_t n = publishMjpegRows(count);
    bus->bank.count.store(n, std::memory_order_relaxed);
    formatGen.fetch_add(1, std::memory_order_release);   // even again: the list is settled
}

// Every way a stream dies or starves. A paused stream never restarts, so it is named rather than left to read as healthy; an unplug empties the bank so the next open fails fast instead of waiting.
void onEvent(const uvc_host_stream_event_data_t* event, void* ctx) {
    switch (event->type) {
        case UVC_HOST_DEVICE_DISCONNECTED:
            ESP_LOGW(kTag, "capture device disconnected");
            // A rewrite to an empty list: the generation moves, so the Picture service closes the stream and the port recycles for a replug.
            formatGen.fetch_add(1, std::memory_order_release);
            formatDevAddr.store(UVC_HOST_ANY_DEV_ADDR, std::memory_order_relaxed);
            bus->bank.count.store(0, std::memory_order_relaxed);
            formatGen.fetch_add(1, std::memory_order_release);
            break;
        case UVC_HOST_TRANSFER_ERROR:
            statFailed.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGW(kTag, "transfer error: the stream is paused, and the capture task restarts it");
            // The restart runs on the capture task: starting a stream from the driver's own callback would re-enter it.
            if (auto* cap = static_cast<Capture*>(ctx)) cap->restart.store(true, std::memory_order_release);
            break;
        case UVC_HOST_FRAME_BUFFER_OVERFLOW:
            statFailed.fetch_add(1, std::memory_order_relaxed);
            break;
        case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
            statDropped.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

// RGB888 bytes for a w x h JPEG, both axes padded to the widest MCU, which is what sizes the buffer.
size_t decodedBytes(uint16_t w, uint16_t h) {
    return static_cast<size_t>(usbvideo::alignUp(w, 16)) * usbvideo::alignUp(h, 16) * 3u;
}

// Warn on the FIRST of each kind only: a drop repeats at frame rate, and the counters carry the rate.
void warnOnce(bool& said, const char* what) {
    if (said) return;
    said = true;
    ESP_LOGW(kTag, "%s - first occurrence; see the Picture status for the running count", what);
}

// The decoder takes no scale factor and writes only into its own aligned buffer. So a frame lands there full size, then averages down into the caller's, which is where the fit to its ceiling happens.
bool decodeAndShrink(Capture& cap, uvc_host_frame_t* frame, const Sink& sink) {
    const size_t len = usbvideo::stripPayloadHeaders(frame->data, frame->data_len);
    // Dimensions from the bitstream, not from the request: a device may negotiate something else. Read here rather than by the decoder's header call, which allocates per frame.
    usbvideo::JpegShape shape;
    if (!usbvideo::jpegShape(frame->data, len, shape)) {
        statFailed.fetch_add(1, std::memory_order_relaxed);
        warnOnce(cap.saidInfo, "frame is not readable JPEG (payload stride mis-detected?)");
        return false;
    }
    const uint16_t w = shape.width, h = shape.height;
    if (decodedBytes(w, h) > cap.rgbCap) {
        statFailed.fetch_add(1, std::memory_order_relaxed);
        warnOnce(cap.saidOversize, "frame exceeds the buffer sized at open");
        return false;
    }
    // The picture is exactly the size asked for, as the test pattern's is. The ranges below tile the frame whatever the ratio, so a 4:3 source fills a wider picture rather than leaving it short.
    if (!sink.width || !sink.height) {
        statFailed.fetch_add(1, std::memory_order_relaxed);
        warnOnce(cap.saidShrink, "the picture has no room for a frame: raise its size");
        return false;
    }
    jpeg_decode_cfg_t decodeCfg = {};
    decodeCfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
    decodeCfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
    decodeCfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t outSize = 0;
    if (jpeg_decoder_process(cap.jpeg, &decodeCfg, frame->data, len, cap.rgb, cap.rgbCap, &outSize)
        != ESP_OK) {
        statFailed.fetch_add(1, std::memory_order_relaxed);
        warnOnce(cap.saidDecode, "the decoder refused a bitstream whose header it accepted");
        return false;
    }
    const size_t stride = usbvideo::strideFor(w, h, shape.mcuW, shape.mcuH, outSize);
    if (!stride) {
        statFailed.fetch_add(1, std::memory_order_relaxed);
        warnOnce(cap.saidStride, "the decoder wrote a size its sampling does not explain");
        return false;
    }
    usbvideo::areaAverage(cap.rgb, stride, w, h, sink);
    statDecoded.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// --- Open and close ------------------------------------------------------------------------------

// Installed once, like the host library: the format list belongs to the bus rather than to one open.
bool ensureUvcHost() {
    if (uvcReady) return true;
    uvc_host_driver_config_t driverCfg = {};
    driverCfg.driver_task_stack_size = 4 * 1024;
    driverCfg.driver_task_priority = 5;
    driverCfg.xCoreID = 1;   // core 1, with the pump and the capture task
    driverCfg.create_background_task = true;
    driverCfg.event_cb = onDriverEvent;   // fills the format bank as soon as a device enumerates
    if (uvc_host_install(&driverCfg) != ESP_OK) {
        ESP_LOGE(kTag, "uvc_host_install failed");
        return false;
    }
    uvcReady = true;
    return true;
}

bool createJpeg(Capture& cap) {
    jpeg_decode_engine_cfg_t jpegCfg = {};
    // The timeout aborts the 2D-DMA mid-frame, and a 1080p RGB frame takes about 34 ms to write, so the bound is an order above that.
    jpegCfg.timeout_ms = 200;
    if (jpeg_new_decoder_engine(&jpegCfg, &cap.jpeg) == ESP_OK) return true;
    ESP_LOGE(kTag, "no JPEG decoder engine");
    return false;
}

uint8_t snapDevAddr = UVC_HOST_ANY_DEV_ADDR;   ///< the device the snapshot describes, read under the same hold

// One snapshot of the bank under its seqlock, so a mode is never chosen from a half-rewritten list. Taken on the render thread in init, the only caller, so the snapshot needs no lock of its own.
size_t snapshotModes() {
    for (int tries = 0; tries < 8; tries++) {
        const uint32_t before = formatGen.load(std::memory_order_acquire);
        if (before & 1u) continue;   // a rewrite in flight
        size_t n = bus->bank.count.load(std::memory_order_relaxed);
        if (n == 0) return 0;   // no device listed
        if (n > kMaxFormats) n = kMaxFormats;
        for (size_t i = 0; i < n; i++) {
            bus->snapModes[i].width = bus->bank.width[i].load(std::memory_order_relaxed);
            bus->snapModes[i].height = bus->bank.height[i].load(std::memory_order_relaxed);
            bus->snapModes[i].fps = bus->bank.fps[i].load(std::memory_order_relaxed);
            bus->snapIntervals[i] = bus->bank.interval[i].load(std::memory_order_relaxed);
        }
        snapDevAddr = formatDevAddr.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (formatGen.load(std::memory_order_relaxed) == before) return n;   // no write overlapped
    }
    return 0;
}

// The snapshot's own interval as the float the driver compares against; 0 asks for the device default.
float exactFpsFor(size_t modes, uint16_t w, uint16_t h, uint8_t fps) {
    for (size_t i = 0; i < modes; i++)
        if (bus->snapModes[i].width == w && bus->snapModes[i].height == h && bus->snapModes[i].fps == fps
            && bus->snapIntervals[i])
            return 10000000.0f / static_cast<float>(bus->snapIntervals[i]);
    return 0.0f;
}

bool openStream(Capture& cap, size_t modes, uint16_t width, uint16_t height, uint8_t fps) {
    uvc_host_stream_config_t streamCfg = {};
    streamCfg.event_cb = onEvent;
    streamCfg.frame_cb = onFrame;
    streamCfg.user_ctx = &cap;
    streamCfg.usb.dev_addr = snapDevAddr;
    streamCfg.usb.vid = UVC_HOST_ANY_VID;
    streamCfg.usb.pid = UVC_HOST_ANY_PID;
    streamCfg.usb.uvc_stream_index = kStreamIndex;
    streamCfg.vs_format.h_res = width;
    streamCfg.vs_format.v_res = height;
    // The device's own interval, not the rounded fps: the driver matches within 0.0001 fps, so a 59.94 mode never matches a requested 60.
    streamCfg.vs_format.fps = exactFpsFor(modes, width, height, fps);
    streamCfg.vs_format.format = UVC_VS_FORMAT_MJPEG;
    // urb_size left at 0 (4x MPS), the driver's default. number_of_urbs has none: 0 is malloc(0).
    streamCfg.advanced.number_of_urbs = 4;
    streamCfg.advanced.number_of_frame_buffers = 3;
    streamCfg.advanced.frame_size = 0;   // 0 takes dwMaxVideoFrameSize from the negotiation: the device's own figure, not a guess
    streamCfg.advanced.frame_heap_caps = MALLOC_CAP_SPIRAM;   // keep the internal heap for USB and WiFi

    // A device has enumerated by now, so the driver has only to claim it, and half a second bounds a device that went away since the snapshot.
    if (uvc_host_stream_open(&streamCfg, pdMS_TO_TICKS(500), &cap.stream) != ESP_OK) {
        ESP_LOGW(kTag, "no UVC device offering MJPEG %ux%u", width, height);
        return false;
    }
    return true;
}

// Sized from what the device agreed to, so no frame needs more room than the buffer has.
bool sizeBuffer(Capture& cap) {
    uvc_host_stream_format_t got = {};
    if (uvc_host_stream_format_get(cap.stream, &got) != ESP_OK) {
        ESP_LOGE(kTag, "cannot read the negotiated format");
        return false;
    }
    ESP_LOGI(kTag, "streaming MJPEG %ux%u @ %.1f fps", got.h_res, got.v_res, got.fps);
    cap.negW = static_cast<uint16_t>(got.h_res);
    cap.negH = static_cast<uint16_t>(got.v_res);
    jpeg_decode_memory_alloc_cfg_t memCfg = {};
    memCfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    cap.rgb = static_cast<uint8_t*>(
        jpeg_alloc_decoder_mem(decodedBytes(cap.negW, cap.negH), &memCfg, &cap.rgbCap));
    if (cap.rgb) return true;
    cap.rgbCap = 0;
    ESP_LOGE(kTag, "no memory for the %ux%u decode buffer", cap.negW, cap.negH);
    return false;
}

// The counters the status reads, zeroed per open.
void resetStats() {
    statDecoded.store(0, std::memory_order_relaxed);
    statDropped.store(0, std::memory_order_relaxed);
    statFailed.store(0, std::memory_order_relaxed);
}

// In this order on purpose: the decode engine and the signal first, then the stream, then its buffer.
bool bringUp(Capture& cap, size_t modes, uint16_t width, uint16_t height, uint8_t fps) {
    cap.wake = xSemaphoreCreateBinary();
    return cap.wake && createJpeg(cap) && openStream(cap, modes, width, height, fps) && sizeBuffer(cap)
           && uvc_host_stream_start(cap.stream) == ESP_OK;
}

}   // namespace

bool videoCaptureInit(VideoCaptureHandle& handle, uint16_t width, uint16_t height, uint8_t fps) {
    if (handle.impl) return true;
    if (claimed) return false;   // one owner at a time, as the seam documents
    if (!ensureUsbHost() || !ensureUvcHost()) return false;
    // No device has enumerated, so there is nothing to open: failing here keeps a board without a grabber from stalling on the open's timeout.
    const size_t modes = snapshotModes();
    if (!modes) return false;
    auto* cap = new Capture();
    handle.impl = cap;   // every failure below unwinds through videoCaptureDeinit
    resetStats();   // per open, as the seam documents them, so a re-open inherits no losses
    // Nothing advertised covers the picture, so the open would spend its whole timeout on a mode no device offers.
    if (!usbvideo::pickMode(bus->snapModes, modes, width, height, fps)) {
        ESP_LOGW(kTag, "no advertised mode covers %ux%u", width, height);
        videoCaptureDeinit(handle);
        return false;
    }
    if (bringUp(*cap, modes, width, height, fps)) {
        claimed = true;
        return true;
    }
    videoCaptureDeinit(handle);
    return false;
}

bool videoCaptureAwaitFrame(VideoCaptureHandle& handle, uint32_t timeoutMs) {
    auto* cap = static_cast<Capture*>(handle.impl);
    if (!cap) return false;
    // A transfer error leaves the stream paused, and only a start brings frames back.
    if (cap->restart.exchange(false, std::memory_order_acq_rel)
        && uvc_host_stream_start(cap->stream) != ESP_OK)
        ESP_LOGW(kTag, "the paused stream refused to restart");
    // A token left by a frame already taken would otherwise return at once with nothing to decode.
    if (cap->pending.load(std::memory_order_acquire)) {
        xSemaphoreTake(cap->wake, 0);
        return true;
    }
    if (xSemaphoreTake(cap->wake, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
    return cap->pending.load(std::memory_order_acquire) != nullptr;
}

bool videoCaptureDecodeInto(VideoCaptureHandle& handle, uint8_t* dst, uint16_t maxWidth, uint16_t maxHeight,
                            uint16_t& outWidth, uint16_t& outHeight, uint8_t floor) {
    auto* cap = static_cast<Capture*>(handle.impl);
    if (!cap) return false;
    uvc_host_frame_t* frame = cap->pending.exchange(nullptr, std::memory_order_acq_rel);
    if (!frame) return false;
    const bool ok = decodeAndShrink(*cap, frame, Sink{dst, maxWidth, maxHeight, floor});
    uvc_host_frame_return(cap->stream, frame);   // returned either way, or the driver runs out
    if (ok) { outWidth = maxWidth; outHeight = maxHeight; }   // the sampler tiles the frame, so the picture is the size asked for
    return ok;
}

uint32_t videoCaptureFormatGeneration() MM_NONBLOCKING {
    if (!bus || bus->bank.count.load(std::memory_order_relaxed) == 0) return 0;   // no device listed
    return formatGen.load(std::memory_order_acquire);
}

void videoCaptureDeinit(VideoCaptureHandle& handle) {
    auto* cap = static_cast<Capture*>(handle.impl);
    if (!cap) return;
    // Stop the stream first, so no frame lands mid-teardown; the caller's task has stopped calling in.
    if (cap->stream) uvc_host_stream_stop(cap->stream);
    if (uvc_host_frame_t* frame = cap->pending.exchange(nullptr)) uvc_host_frame_return(cap->stream, frame);
    if (cap->stream) uvc_host_stream_close(cap->stream);
    if (cap->jpeg) jpeg_del_decoder_engine(cap->jpeg);
    if (cap->wake) vSemaphoreDelete(cap->wake);
    free(cap->rgb);   // free(nullptr) is a no-op, so a failed open is fine
    delete cap;
    handle.impl = nullptr;
    claimed = false;   // the device is free for the next open
}

void videoCaptureShutdown() {
    if (claimed) return;   // a capture is open: its owner deinits first
    if (uvcReady) {
        if (uvc_host_uninstall() != ESP_OK) {
            ESP_LOGW(kTag, "the UVC driver did not uninstall");
            return;
        }
        uvcReady = false;
    }
    // The pump sees its only client gone, frees the device and uninstalls the library; a stopped port does that within milliseconds.
    for (int i = 0; i < 200 && pumpRunning.load(std::memory_order_acquire); i++) vTaskDelay(pdMS_TO_TICKS(5));
    if (pumpRunning.load(std::memory_order_acquire)) {
        ESP_LOGW(kTag, "the USB host is still tearing down");
        return;   // the bank stays until the next shutdown finds the pump gone
    }
    hostReady = false;
    delete bus;
    bus = nullptr;
}

VideoCaptureStats videoCaptureStats() MM_NONBLOCKING {
    VideoCaptureStats s;
    s.decoded = statDecoded.load(std::memory_order_relaxed);
    s.dropped = statDropped.load(std::memory_order_relaxed);
    s.failed = statFailed.load(std::memory_order_relaxed);
    return s;
}

}   // namespace mm::platform

#else   // no High-Speed USB host, no JPEG decoder, or the capture left out of this image

namespace mm::platform {

// An image without the capture reports no device, which the Picture service shows as a status.
uint32_t videoCaptureFormatGeneration() MM_NONBLOCKING { return 0; }
bool videoCaptureInit(VideoCaptureHandle&, uint16_t, uint16_t, uint8_t) { return false; }
bool videoCaptureAwaitFrame(VideoCaptureHandle&, uint32_t) { return false; }
bool videoCaptureDecodeInto(VideoCaptureHandle&, uint8_t*, uint16_t, uint16_t, uint16_t&, uint16_t&,
                            uint8_t) { return false; }
void videoCaptureDeinit(VideoCaptureHandle&) {}
void videoCaptureShutdown() {}
VideoCaptureStats videoCaptureStats() MM_NONBLOCKING { return VideoCaptureStats{}; }

}   // namespace mm::platform

#endif
