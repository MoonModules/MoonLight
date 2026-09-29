/// @defgroup platform_desktop_audio Desktop audio capture
/// The vendored single-header backend, compiled exactly once here.
///
/// @moreinfo
///
/// ## Why it is vendored
///
/// One permissively licensed header with no link dependencies replaces three hand-written system backends plus device enumeration.
/// It links the system audio frameworks itself at run time, which matches the rule that the build needs no vendor kits, as the raw-frame and video precedents already do.
/// The header lives untouched and is excluded from the quality gates; everything below the include is ours and stays warning-clean.

// Shrink the build to the capture core: no codecs, no waveform generation, no engine.
// One compiler cannot parse Apple's block syntax in those framework headers, and that build is a compile proof never shipped.
// Dropping the backend there leaves the null one, while the shipped binary is built with the other compiler and keeps it.
#if defined(__APPLE__) && defined(__GNUC__) && !defined(__clang__)
    #define MA_NO_COREAUDIO
#endif

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE

#if defined(_MSC_VER)
    #pragma warning(push, 1)
#elif defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wunused-parameter"
    #pragma GCC diagnostic ignored "-Wunused-function"
    #pragma GCC diagnostic ignored "-Wunused-variable"
    #pragma GCC diagnostic ignored "-Wsign-compare"
    #pragma GCC diagnostic ignored "-Wswitch"
    #pragma GCC diagnostic ignored "-Wdouble-promotion"
    #pragma GCC diagnostic ignored "-Wconversion"
    #pragma GCC diagnostic ignored "-Wfloat-conversion"
    #if defined(__clang__)
        #pragma GCC diagnostic ignored "-Wnullability-completeness"
        #pragma GCC diagnostic ignored "-Wnullability-extension"
        // Only local/newer clang knows this experimental group; CI's Apple clang errors on the unknown name under -Werror, so probe before naming it.
        #if __has_warning("-Wfunction-effects")
            #pragma GCC diagnostic ignored "-Wfunction-effects"
        #endif
    #endif
#endif

#define MINIAUDIO_IMPLEMENTATION
#include "vendor/miniaudio.h"

#if defined(_MSC_VER)
    #pragma warning(pop)
#elif defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif

// The capture backend proper: everything below is ours and only the include above is vendored.
// The library delivers samples on ITS own device thread, the callback pushes into a lock-free single-producer ring, and the module's polled read pops on the render thread.
// The overflow policy and the sizing reasoning live with that ring.

#include "platform/platform.h"
#include "core/util/SpscRing.h"

#include <cstdio>
#include <cstring>

namespace mm::platform {

namespace {

// One capture pipeline at a time (AudioService is a single active seat; a second concurrent device would race one ring). 4096 samples ≈ 186 ms at 22050 Hz, comfortably above one render tick's consumption, small enough to bound latency.
SpscRing<int32_t, 4096> ring_;

ma_context ctx_;
bool ctxReady_ = false;
ma_device device_;
bool deviceOpen_ = false;

// The enumeration cache the `device` Select borrows: stable storage owned here, entry 0 is always "default". Sized for a generous desk (more devices than kMaxDevices simply don't list; the default entry always works).
constexpr size_t kMaxDevices = 16;
constexpr size_t kNameLen = 64;
char nameBuf_[kMaxDevices][kNameLen];
const char* namePtrs_[kMaxDevices];
ma_device_id deviceIds_[kMaxDevices];   // parallel to nameBuf_ (entry 0 unused: default)
size_t deviceCount_ = 0;

bool ensureContext() {
    if (ctxReady_) return true;
    if (ma_context_init(nullptr, 0, nullptr, &ctx_) != MA_SUCCESS) return false;
    ctxReady_ = true;
    return true;
}

// miniaudio device thread -> ring. s32 frames, mono (configured in audioCaptureInit); a full ring drops the newest block (see SpscRing).
void captureCallback(ma_device* /*dev*/, void* /*out*/, const void* in, ma_uint32 frames) {
    if (in == nullptr) return;
    ring_.push(static_cast<const int32_t*>(in), frames);
}

}  // namespace

bool audioCodecInit(CodecType /*type*/, const AudioCodecPins& /*pins*/, uint32_t /*sampleRate*/) {
    return true;   // no codec hardware on a desktop host; nothing to bring up
}
void audioCodecDeinit() {}

size_t audioCaptureDevices(const char* const** optionsOut) {
    std::snprintf(nameBuf_[0], kNameLen, "default");
    namePtrs_[0] = nameBuf_[0];
    deviceCount_ = 1;
    if (ensureContext()) {
        ma_device_info* infos = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(&ctx_, nullptr, nullptr, &infos, &count) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count && deviceCount_ < kMaxDevices; i++) {
                // Truncation to kNameLen is intentional (long device names get cut for the dropdown); the explicit precision states it, which GCC's -Wformat-truncation requires under -Werror.
                std::snprintf(nameBuf_[deviceCount_], kNameLen, "%.63s", infos[i].name);
                namePtrs_[deviceCount_] = nameBuf_[deviceCount_];
                deviceIds_[deviceCount_] = infos[i].id;
                deviceCount_++;
            }
        }
    }
    if (optionsOut) *optionsOut = namePtrs_;
    return deviceCount_;
}

bool audioCaptureInit(AudioMicHandle& h, uint8_t deviceIndex, uint32_t sampleRate) {
    if (!ensureContext()) return false;
    if (deviceOpen_) { audioMicDeinit(h); }
    // Index 0 = OS default device (null id). A stale persisted index past the current list fails loudly here rather than opening some other device.
    if (deviceIndex >= deviceCount_ && deviceIndex != 0) {
        // The list may simply not have been enumerated yet this boot (e.g. a direct init).
        audioCaptureDevices(nullptr);
        if (deviceIndex >= deviceCount_) return false;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    if (deviceIndex > 0) cfg.capture.pDeviceID = &deviceIds_[deviceIndex];
    // s32 mono at the service's rate: full-scale s32 IS the seam's 24-bit-left-justified regime, and miniaudio converts/resamples from whatever the hardware runs natively.
    cfg.capture.format = ma_format_s32;
    cfg.capture.channels = 1;
    cfg.sampleRate = sampleRate;
    cfg.dataCallback = captureCallback;

    if (ma_device_init(&ctx_, &cfg, &device_) != MA_SUCCESS) return false;
    if (ma_device_start(&device_) != MA_SUCCESS) {
        ma_device_uninit(&device_);
        return false;
    }
    deviceOpen_ = true;
    h.impl = &device_;
    // Drain anything stale so the first block is live audio, not history.
    int32_t scratch[256];
    while (ring_.pop(scratch, 256) == 256) {}
    return true;
}


// The desktop captures from an OS device, which shares no peripheral with anything: never retries.
bool audioMicSharedBusFree(MicMode) { return false; }

size_t audioMicRead(AudioMicHandle& /*h*/, int32_t* out, size_t maxSamples) {
    if (!deviceOpen_ || out == nullptr) return 0;
    return ring_.pop(out, maxSamples);
}

void audioMicDeinit(AudioMicHandle& h) {
    if (deviceOpen_) {
        ma_device_uninit(&device_);   // stops the device thread, then frees it
        deviceOpen_ = false;
    }
    h.impl = nullptr;
}

// The pin-based I2S init cannot exist on a desktop host; capture goes through audioCaptureInit. Kept failing (not asserting) so shared code degrades.
bool audioMicInit(AudioMicHandle& /*h*/, uint16_t /*wsPin*/, uint16_t /*sdPin*/,
                  uint16_t /*sckPin*/, int16_t /*mclkPin*/, uint32_t /*sampleRate*/,
                  MicMode /*mode*/) {
    return false;
}

}  // namespace mm::platform
