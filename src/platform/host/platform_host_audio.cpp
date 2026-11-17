/// The vendored single-header backend, compiled exactly once here.
///
/// @technical
///
/// ## Why it is vendored
///
/// One permissively licensed header replaces three system backends and device enumeration, linking the system audio frameworks at run time, so the build needs no vendor kits. It stays untouched, outside the quality gates.
///
/// GCC cannot parse Apple's block syntax in those framework headers, so that compile-proof build falls back to the null backend; the shipped clang build keeps CoreAudio.
#if defined(__APPLE__) && defined(__GNUC__) && !defined(__clang__)
    #define MA_NO_COREAUDIO
#endif

// Shrink the build to the capture core: no codecs, no waveform generation, no engine.
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

// Ours below the include: the library's device thread pushes samples into a lock-free ring that the render thread pops; the ring documents overflow and sizing.

#include <chrono>
#include <thread>   // the read's wait

#include "platform/platform.h"
#include "core/util/SpscRing.h"

#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <unistd.h>   // getuid: a process Android's shell started
#endif

namespace mm::platform {

namespace {

// One pipeline: AudioService is a single seat. 4096 samples ≈ 186 ms at 22050 Hz, above one tick's use and short enough to bound latency.
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

// miniaudio's own backend order, except in a process Android's shell started (`adb shell`, uid 2000): lacking an app identity, it makes AAudio crash, so it runs with no sound device.
static ma_result initContext(ma_context* ctx) {
#ifdef __ANDROID__
    constexpr uid_t kAndroidShellUid = 2000;   // AID_SHELL, from a header the NDK does not ship
    if (::getuid() == kAndroidShellUid) return MA_NO_BACKEND;
#endif
    return ma_context_init(nullptr, 0, nullptr, ctx);
}

// The context picks its backend when it is created, on Linux PulseAudio or PipeWire where a server answers and ALSA otherwise, so a server started after MoonLight is found by creating it anew, free while no device holds it.
bool freshContext() {
    if (deviceOpen_) return true;   // the open device holds this context
    if (ctxReady_) { ma_context_uninit(&ctx_); ctxReady_ = false; }
    if (initContext(&ctx_) != MA_SUCCESS) return false;
    ctxReady_ = true;
    return true;
}

// miniaudio device thread -> ring. s32 frames, mono (configured in audioCaptureInit); a full ring drops the newest block (see SpscRing).
void captureCallback(ma_device* /*dev*/, void* /*out*/, const void* in, ma_uint32 frames) {
    if (in == nullptr) return;
    ring_.push(static_cast<const int32_t*>(in), frames);
}

}  // namespace

bool audioCodecInit(CodecType /*type*/, uint8_t /*i2cAddr*/, uint32_t /*sampleRate*/, uint8_t /*gainDb*/) {
    return true;   // no codec hardware on a host; nothing to bring up
}
void audioCodecDeinit() {}

/// The list entry past the default called `name`, as the list truncates names, or 0 where there is none.
static size_t indexNamed(const char* name) {
    for (size_t k = 1; k < deviceCount_; k++)
        if (std::strncmp(namePtrs_[k], name, kNameLen - 1) == 0) return k;
    return 0;
}

/// What the OS lists and a reader cannot pick: ALSA's bit bucket, `null`, offered as a capture device, and a name already listed, as ALSA names `default` and `sysdefault` alike.
static bool unlistable(const ma_device_info& info) {
    if (ctx_.backend == ma_backend_alsa && std::strcmp(info.id.alsa, "null") == 0) return true;
    return indexNamed(info.name) != 0;
}

/// The context a list is read through: a fresh one, or, while a device holds the context, `scratch`, so a server started since shows in the list; null where neither comes up. The pick recreates the context before it opens anything.
static ma_context* listContext(ma_context& scratch) {
    if (deviceOpen_) return initContext(&scratch) == MA_SUCCESS ? &scratch : nullptr;
    return freshContext() ? &ctx_ : nullptr;
}

/// Say the backend once per change on stderr, the diagnostics stream, so a terminal shows which one the host holds (Null: no audio libraries) and mm_tempo's stdout stays its data.
static void sayBackend(ma_backend backend) {
    static ma_backend said = ma_backend_null;
    static bool once = false;
    if (once && said == backend) return;
    std::fprintf(stderr, "audio: %s backend, %u capture device(s)\n", ma_get_backend_name(backend), static_cast<unsigned>(deviceCount_ - 1));
    said = backend;
    once = true;
}

/// Append what the OS lists past entry 0, the default.
static void listDevices() {
    static ma_context scratch;   // kilobytes, so static rather than a stack frame the render thread pays for
    ma_context* use = listContext(scratch);
    if (!use) return;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(use, nullptr, nullptr, &infos, &count) != MA_SUCCESS) count = 0;
    for (ma_uint32 i = 0; i < count && deviceCount_ < kMaxDevices; i++) {
        if (unlistable(infos[i])) continue;
        // Truncation to kNameLen is intentional (long device names get cut for the dropdown); the explicit precision states it, which GCC's -Wformat-truncation requires under -Werror.
        std::snprintf(nameBuf_[deviceCount_], kNameLen, "%.63s", infos[i].name);
        namePtrs_[deviceCount_] = nameBuf_[deviceCount_];
        deviceIds_[deviceCount_] = infos[i].id;
        deviceCount_++;
    }
    sayBackend(use->backend);
    if (use == &scratch) ma_context_uninit(&scratch);
}

const char* audioCaptureBackend() MM_NONBLOCKING { return ctxReady_ ? ma_get_backend_name(ctx_.backend) : "none"; }

size_t audioCaptureDevices(const char* const** optionsOut) {
    std::snprintf(nameBuf_[0], kNameLen, "default");
    namePtrs_[0] = nameBuf_[0];
    deviceCount_ = 1;
    listDevices();
    if (optionsOut) *optionsOut = namePtrs_;
    return deviceCount_;
}

/// The id of the device called `name` in the context's own list, as the list truncates names, valid until the context goes; null where it lists none such.
static const ma_device_id* findDevice(const char* name) {
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&ctx_, nullptr, nullptr, &infos, &count) != MA_SUCCESS) return nullptr;
    for (ma_uint32 i = 0; i < count; i++)
        if (std::strncmp(infos[i].name, name, kNameLen - 1) == 0) return &infos[i].id;
    return nullptr;
}

/// The device entry `deviceIndex` names, on a fresh context: null for the OS default, else its id in the context's list; false for a stale index past the list or a name the fresh context lacks, which fail loudly rather than open another device.
static bool resolvePick(uint8_t deviceIndex, const ma_device_id*& id) {
    id = nullptr;
    if (deviceIndex && deviceIndex >= deviceCount_) return false;
    char want[kNameLen] = "default";
    if (deviceIndex) std::snprintf(want, kNameLen, "%s", namePtrs_[deviceIndex]);   // the name at that index, as the service remembers it; the card's list stays as the card saw it
    if (!freshContext()) return false;
    if (deviceIndex) id = findDevice(want);
    return !deviceIndex || id != nullptr;
}

/// Fill `cfg` for a mono s32 capture at `sampleRate` from the device `id`, the OS default where null: full-scale s32 is the seam's 24-bit-left-justified regime, and miniaudio converts from whatever the hardware runs natively.
static void captureConfig(ma_device_config& cfg, const ma_device_id* id, uint32_t sampleRate) {
    cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.pDeviceID = id;
    cfg.capture.format = ma_format_s32;
    cfg.capture.channels = 1;
    cfg.sampleRate = sampleRate;
    cfg.dataCallback = captureCallback;
}

bool audioCaptureInit(AudioMicHandle& h, uint8_t deviceIndex, uint32_t sampleRate) {
    if (deviceOpen_) { audioMicDeinit(h); }
    const ma_device_id* id = nullptr;
    if (!resolvePick(deviceIndex, id)) return false;
    ma_device_config cfg;
    captureConfig(cfg, id, sampleRate);

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


// The host captures from an OS device, which shares no peripheral with anything: never retries.
bool audioMicSharedBusFree(MicMode) { return false; }

size_t audioMicRead(AudioMicHandle& /*h*/, int32_t* out, size_t maxSamples, uint32_t waitMs) {
    if (!deviceOpen_ || out == nullptr) return 0;
    size_t got = ring_.pop(out, maxSamples);
    // An empty ring waits for the capture thread in 1 ms steps, the read an audio task blocks in.
    for (uint32_t waited = 0; got == 0 && waited < waitMs; waited++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        got = ring_.pop(out, maxSamples);
    }
    return got;
}

void audioMicDeinit(AudioMicHandle& h) {
    if (deviceOpen_) {
        ma_device_uninit(&device_);   // stops the device thread, then frees it
        deviceOpen_ = false;
    }
    h.impl = nullptr;
}

// The pin-based I2S init cannot exist on a host; capture goes through audioCaptureInit. Kept failing (not asserting) so shared code degrades.
bool audioMicInit(AudioMicHandle& /*h*/, uint16_t /*wsPin*/, uint16_t /*sdPin*/,
                  uint16_t /*sckPin*/, int16_t /*mclkPin*/, uint32_t /*sampleRate*/,
                  MicMode /*mode*/) {
    return false;
}

}  // namespace mm::platform
