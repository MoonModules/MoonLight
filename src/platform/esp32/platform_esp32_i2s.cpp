/// The peripheral half of the audio feature: two seams and nothing else.
///
/// The module above does the level maths, the windowing and the band mapping; this file reads samples off the channel and runs the transform.
///
/// @technical
///
/// ## What the microphone is
///
/// A digital part in the standard framing, its data left-justified in a wider slot, mono: the channel reads the one slot the part's own pin selects.
/// It is self-clocked from the bit clock and needs no master clock pin.
/// The slot is configured wide and the raw samples are handed to the domain code, which shifts the value down itself.
/// Everything is under the capability guard with inert stubs elsewhere, and the module never calls in on a chip without it.
///
/// ## The transform scratch is allocated, not reserved
///
/// As a plain array it was eight kilobytes of internal memory reserved from boot on every board.
/// That is a few percent of the pool the network stack and every task stack share.
/// That included the many boards with no microphone fitted, where nothing ever reads it, and a module that is not used should cost nothing.
/// Now a board without audio pays only for the pointer, and one with audio allocates on its first analyzed frame and keeps it for the process.
/// So nothing allocates per frame on the audio path.
///
/// ## The gain from the alternate microphone is a conversion, not a boost
///
/// That receiver delivers narrower samples than the standard path, so each is scaled to the same full scale the rest of the chain assumes.
/// Which maps one range onto the other exactly and can never clip.
///
/// A larger gain is tempting and wrong.
/// The level meter measures a raw magnitude in decibels against a window.
/// So a quiet signal reads below that window and the level sits at nothing until the floor is lowered to meet it.
/// That is a WINDOW problem, and scaling up to fix it costs headroom: at a much larger factor the clip point falls below this part's own quiet-room noise floor.
/// The microphone then clipped continuously, and clipping is broadband, so every band showed noise and the onset detector fired in a silent room.
/// Set the display window with the floor control, never with this constant.
///
/// The scaling multiplies rather than shifts, since a narrow sample promotes to a signed type where shifting a negative value or into the sign bit is undefined.
/// While the multiply is defined across the whole range.

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_I2S_SUPPORTED

#include "driver/i2s_std.h"
#if SOC_I2S_SUPPORTS_PDM_RX
#include "driver/i2s_pdm.h"          // the two-wire onboard mics (QuinLED Dig-Next-2 and friends)
#include "esp_private/i2s_platform.h" // the shared-instance probe (see audioMicSharedBusFree)
#endif
#include "esp_heap_caps.h"   // heap_caps_malloc — the FFT scratch, internal RAM only
#include "esp_log.h"
#include "dsps_fft2r.h"

#include <cmath>
#include <cstring>
#include <new>      // std::nothrow

namespace mm::platform {

namespace {

const char* I2S_TAG = "mm_i2s";

struct MicState {
    i2s_chan_handle_t rx = nullptr;
    bool pdm = false;          ///< PDM reads 16-bit samples; the std path reads 32
    /// Staging for the read, per channel rather than shared, since two microphones would otherwise collide and a static one races anything but the render loop.
    int16_t stage[256] = {};
};

// The library's transform works in place on an interleaved complex array, so one scratch is sized to the largest block, and its tables initialize lazily on first use. Allocated on the first transform rather than reserved from boot: @xref{the-transform-scratch-is-allocated-not-reserved|what it cost as a static array}.
constexpr size_t kMaxFftN = 1024;
float* g_fftBuf = nullptr;
bool   g_fftReady = false;

bool ensureFftInit() {
    if (g_fftReady) return true;
    // dsps_fft2r_init_fc32(NULL, …) uses the library's built-in max-size twiddle table, no caller allocation, initialized once for the process.
    if (dsps_fft2r_init_fc32(nullptr, CONFIG_DSP_MAX_FFT_SIZE) != ESP_OK) {
        ESP_LOGE(I2S_TAG, "esp-dsp FFT init failed");
        return false;
    }
    // Internal RAM, not PSRAM: esp-dsp's assembly kernels run per audio block and a PSRAM scratch would put a cache miss in the middle of every butterfly.
    g_fftBuf = static_cast<float*>(
        heap_caps_malloc(kMaxFftN * 2 * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!g_fftBuf) {
        ESP_LOGE(I2S_TAG, "FFT scratch alloc failed (%u B internal)",
                 static_cast<unsigned>(kMaxFftN * 2 * sizeof(float)));
        return false;   // audioFft zero-fills its output, so the caller degrades to silence
    }
    g_fftReady = true;
    return true;
}

}  // namespace

namespace {
// Set when audioMicInit failed because another module held the I2S instance, cleared on every attempt. Only contention can clear on its own, so only it earns the once-a-second retry: keyed on "the mic is down" instead, a board with no microphone wired would re-init forever.
bool s_micRefusedForContention = false;
}

bool audioMicInit(AudioMicHandle& h, uint16_t wsPin, uint16_t sdPin,
                  uint16_t sckPin, int16_t mclkPin, uint32_t sampleRate, MicMode mode) {
    s_micRefusedForContention = false;
    auto* st = new (std::nothrow) MicState();
    if (!st) return false;

    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chanCfg, nullptr, &st->rx) != ESP_OK) {
        // No free instance: something else (the parallel LED bus) holds it, and that can clear.
        s_micRefusedForContention = true;
        delete st;
        return false;
    }

    if (mode == MicMode::Pdm) {
#if SOC_I2S_SUPPORTS_PDM_RX
        // A PDM part sends one bit per clock and the peripheral decimates it to PCM, so there are only two wires: the clock the ESP32 drives, and the data line. `wsPin` carries the clock (it is the pin the board wires to the mic's CLK) and `sdPin` the data; `sckPin` and `mclkPin` have no meaning here.
        i2s_pdm_rx_config_t pdmCfg = {
            .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sampleRate),
            .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .clk = static_cast<gpio_num_t>(wsPin),
                .din = static_cast<gpio_num_t>(sdPin),
                .invert_flags = { .clk_inv = false },
            },
        };
        if (i2s_channel_init_pdm_rx_mode(st->rx, &pdmCfg) != ESP_OK
            || i2s_channel_enable(st->rx) != ESP_OK) {
            i2s_del_channel(st->rx);
            delete st;
            return false;
        }
        // 16-bit samples, where the std path reads 32. audioMicRead widens them on the way out so the domain code sees one sample format whatever the part is.
        st->pdm = true;
        h.impl = st;
        return true;
#else
        // The chip has no PDM receiver. Fail rather than quietly configure a standard-mode channel on two pins, which would read noise and look like a wiring fault.
        ESP_LOGE(I2S_TAG, "PDM microphone requested, but this chip has no PDM receiver");
        i2s_del_channel(st->rx);
        delete st;
        return false;
#endif
    }

    // The standard framing, mono, the part putting its data in ONE slot chosen by its own select pin and leaving the other empty. The bench part is wired for the left slot; a microphone reading silence with sound present is filling the other one, so flip this.
    i2s_std_slot_config_t slotCfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    slotCfg.slot_mask = I2S_STD_SLOT_LEFT;
    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
        .slot_cfg = slotCfg,
        .gpio_cfg = {
            // MCLK: unused for a self-clocked MEMS mic (INMP441); driven on the given pin for a codec that needs a master clock (the ES8311). −1 = none.
            .mclk = mclkPin < 0 ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(mclkPin),
            .bclk = static_cast<gpio_num_t>(sckPin),
            .ws   = static_cast<gpio_num_t>(wsPin),
            .dout = I2S_GPIO_UNUSED,         // input only
            .din  = static_cast<gpio_num_t>(sdPin),
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(st->rx, &stdCfg) != ESP_OK
        || i2s_channel_enable(st->rx) != ESP_OK) {
        if (st->rx) i2s_del_channel(st->rx);
        delete st;
        return false;
    }

    h.impl = st;
    return true;
}

size_t audioMicRead(AudioMicHandle& h, int32_t* out, size_t maxSamples) {
    auto* st = static_cast<MicState*>(h.impl);
    if (!st || !st->rx || !out || maxSamples == 0) return 0;
    size_t bytesRead = 0;
    // Non-blocking, since this runs in the render tick and must not wait for the transfer to fill: it drains whatever is already held and returns at once.
    // A full block takes longer than one tick, so a single read returns a partial one and the module accumulates them across ticks.
    // A timeout still copies what was ready and reports the count, so that count is used whatever the return code.
    if (st->pdm) {
        // The alternate receiver delivers narrower samples, staged here and widened on the way out, so the domain code sees one format either way.
        // A FIXED staging buffer rather than a split of the caller's, whose remaining room shrinks as its block fills.
        // Halving that reached zero and reported no samples forever, one short of a complete block.
        // The scaling is a conversion rather than a boost: @xref{the-gain-from-the-alternate-microphone-is-a-conversion-not-a-boost|why a larger one clips continuously}.
        constexpr size_t kStage = sizeof(st->stage) / sizeof(st->stage[0]);
        const size_t want = maxSamples < kStage ? maxSamples : kStage;
        if (want == 0) return 0;
        i2s_channel_read(st->rx, st->stage, want * sizeof(int16_t), &bytesRead, 0 /* non-blocking */);
        const size_t got = bytesRead / sizeof(int16_t);
        constexpr int32_t kPdmGain = 65536;   // int16 full scale -> int32 full scale, see above
        for (size_t i = 0; i < got; i++) out[i] = static_cast<int32_t>(st->stage[i]) * kPdmGain;
        return got;
    }
    i2s_channel_read(st->rx, out, maxSamples * sizeof(int32_t), &bytesRead,
                     0 /* ms — non-blocking */);
    return bytesRead / sizeof(int32_t);
}

bool audioMicSharedBusFree(MicMode mode) {
#if CONFIG_IDF_TARGET_ESP32
    // Only after a CONTENTION refusal. Without this the probe answers "free" on any board whose instance 0 is simply idle, so a mic that is down for its OWN reasons (no part wired, wrong pins) would re-init once a second forever, allocating and logging on the render thread.
    if (!s_micRefusedForContention) return false;
    // Only the classic ESP32 shares: its parallel LED bus IS an I2S peripheral. That bus always takes instance 1 (see platform_esp32_i80.cpp), so audio always has instance 0, which is also the only instance a PDM microphone can use. This is the mirror of the bus's own probe: it matters when something else holds 0, and the mic recovers once that clears.
    (void)mode;
    if (i2s_platform_acquire_occupation(I2S_CTLR_HP, 0, "mm_mic_probe") != ESP_OK) return false;
    i2s_platform_release_occupation(I2S_CTLR_HP, 0);
    return true;
#else
    return false;   // every other chip drives parallel LEDs from LCD_CAM, so nothing contends
#endif
}

void audioMicDeinit(AudioMicHandle& h) {
    auto* st = static_cast<MicState*>(h.impl);
    if (!st) return;
    if (st->rx) {
        i2s_channel_disable(st->rx);
        i2s_del_channel(st->rx);
    }
    delete st;
    h.impl = nullptr;
}

void audioFft(const float* windowed, size_t n, float* outMag) {
    if (!windowed || !outMag || n == 0 || n > kMaxFftN) return;
    if (!ensureFftInit()) {
        for (size_t k = 0; k < n / 2; k++) outMag[k] = 0.0f;
        return;
    }
    // Pack the real input into the interleaved complex scratch (imag = 0).
    for (size_t i = 0; i < n; i++) {
        g_fftBuf[2 * i] = windowed[i];
        g_fftBuf[2 * i + 1] = 0.0f;
    }
    dsps_fft2r_fc32(g_fftBuf, static_cast<int>(n));
    dsps_bit_rev_fc32(g_fftBuf, static_cast<int>(n));
    // Magnitudes of the first n/2 bins: sqrt(re^2 + im^2).
    for (size_t k = 0; k < n / 2; k++) {
        const float re = g_fftBuf[2 * k];
        const float im = g_fftBuf[2 * k + 1];
        outMag[k] = std::sqrt(re * re + im * im);
    }
}

}  // namespace mm::platform

#else  // !SOC_I2S_SUPPORTED — inert stubs so any I2S-less target links

namespace mm::platform {

bool audioMicInit(AudioMicHandle&, uint16_t, uint16_t, uint16_t, int16_t, uint32_t, MicMode) {
    return false;
}
size_t audioMicRead(AudioMicHandle&, int32_t*, size_t) { return 0; }
void audioMicDeinit(AudioMicHandle&) {}
bool audioMicSharedBusFree(MicMode) { return false; }   // no I2S: nothing to contend for
void audioFft(const float*, size_t, float*) {}


}  // namespace mm::platform

#endif  // SOC_I2S_SUPPORTED

// OS capture devices are a desktop concept (hasAudioCapture == false on every ESP32 target). Deliberately OUTSIDE the SOC_I2S_SUPPORTED split: shared code references these from discarded `if constexpr (hasAudioCapture)` branches, which still require a definition to link (ODR) on I2S and I2S-less chips alike.
namespace mm::platform {
size_t audioCaptureDevices(const char* const** optionsOut) {
    if (optionsOut) *optionsOut = nullptr;
    return 0;
}
bool audioCaptureInit(AudioMicHandle& /*h*/, uint8_t /*deviceIndex*/, uint32_t /*sampleRate*/) {
    return false;
}
}  // namespace mm::platform
