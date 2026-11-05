/// The peripheral half of the audio feature: the read and nothing else.
///
/// The module above does the level maths and the bands; this file reads samples off the channel.
///
/// @technical
///
/// ## What the microphone is
///
/// A digital part, standard framing, data left-justified in a wider slot, mono, self-clocked: the channel reads the slot its select pin picks, and the domain code shifts the raw samples down. Inert stubs cover chips without I2S.
///
/// ## The gain from the alternate microphone is a conversion, not a boost
///
/// That receiver delivers narrower samples, so each is scaled to the full scale the chain assumes: an exact range map that never clips.
///
/// A larger gain is wrong: the learned dynamics window follows the signal's level, so a quiet input needs no boost. At a much larger factor the clip point falls below the part's noise floor and clipping fires the onset detector in a silent room.

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_I2S_SUPPORTED

#include "driver/i2s_std.h"
#if SOC_I2S_SUPPORTS_PDM_RX
#include "driver/i2s_pdm.h"          // the two-wire onboard mics (QuinLED Dig-Next-2 and friends)
#include "esp_private/i2s_platform.h" // the shared-instance probe (see audioMicSharedBusFree)
#endif
#include "esp_log.h"

#include <cstring>
#include <new>      // std::nothrow

namespace mm::platform {

namespace {

const char* I2S_TAG = "mm_i2s";

struct MicState {
    i2s_chan_handle_t rx = nullptr;
    bool pdm = false;          ///< PDM reads 16-bit samples; the std path reads 32
    /// Read staging, per channel: a shared or static buffer collides between two microphones.
    int16_t stage[256] = {};
};


// Set when audioMicInit failed because another module held the I2S instance, cleared on every attempt. Only contention clears on its own, so only it earns the once-a-second retry.
bool s_micRefusedForContention = false;
}

// The receive channel with a DMA ring of 60 ms at any rate, so a busy core delays the audio task's read without an overflow; the default holds 30 ms at 48 kHz.
static i2s_chan_config_t micChannelConfig(uint32_t sampleRate) {
    i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    constexpr uint32_t kRingMs = 60;
    const uint32_t descs = (sampleRate * kRingMs / 1000 + cfg.dma_frame_num - 1) / cfg.dma_frame_num;
    if (descs > cfg.dma_desc_num) cfg.dma_desc_num = descs;
    return cfg;
}

bool audioMicInit(AudioMicHandle& h, uint16_t wsPin, uint16_t sdPin,
                  uint16_t sckPin, int16_t mclkPin, uint32_t sampleRate, MicMode mode) {
    s_micRefusedForContention = false;
    auto* st = new (std::nothrow) MicState();
    if (!st) return false;

    i2s_chan_config_t chanCfg = micChannelConfig(sampleRate);
    if (i2s_new_channel(&chanCfg, nullptr, &st->rx) != ESP_OK) {
        // No free instance: something else (the parallel LED bus) holds it, and that can clear.
        s_micRefusedForContention = true;
        delete st;
        return false;
    }

    if (mode == MicMode::Pdm) {
#if SOC_I2S_SUPPORTS_PDM_RX
        // PDM has two wires: `wsPin` carries the clock the ESP32 drives (the mic's CLK) and `sdPin` the data; `sckPin` and `mclkPin` are unused.
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

    // Standard framing, mono: the part fills ONE slot, chosen by its select pin. Wired for the left slot here; silence with sound present means the other slot, so flip this.
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

size_t audioMicRead(AudioMicHandle& h, int32_t* out, size_t maxSamples, uint32_t waitMs) {
    auto* st = static_cast<MicState*>(h.impl);
    if (!st || !st->rx || !out || maxSamples == 0) return 0;
    size_t bytesRead = 0;
    // `waitMs` 0 drains what is held and returns at once; a wait blocks until samples arrive. A timeout still copies what was ready, so the count is used whatever the return code.
    if (st->pdm) {
        // Narrow samples stage in a FIXED buffer (a split of the caller's shrinks to zero), widened per buffer until drained. @xref{the-gain-from-the-alternate-microphone-is-a-conversion-not-a-boost|why a larger gain clips}.
        constexpr size_t kStage = sizeof(st->stage) / sizeof(st->stage[0]);
        constexpr int32_t kPdmGain = 65536;   // int16 full scale -> int32 full scale, see above
        size_t total = 0;
        while (total < maxSamples) {
            const size_t want = maxSamples - total < kStage ? maxSamples - total : kStage;
            i2s_channel_read(st->rx, st->stage, want * sizeof(int16_t), &bytesRead, total == 0 ? waitMs : 0);   // only the first part waits
            const size_t got = bytesRead / sizeof(int16_t);
            for (size_t i = 0; i < got; i++) out[total + i] = static_cast<int32_t>(st->stage[i]) * kPdmGain;
            total += got;
            if (got < want) break;   // drained
        }
        return total;
    }
    i2s_channel_read(st->rx, out, maxSamples * sizeof(int32_t), &bytesRead, waitMs);
    return bytesRead / sizeof(int32_t);
}

bool audioMicSharedBusFree(MicMode mode) {
#if CONFIG_IDF_TARGET_ESP32
    // Only after a CONTENTION refusal: otherwise a mic down for its OWN reasons (no part wired, wrong pins) re-inits once a second forever on the render thread.
    if (!s_micRefusedForContention) return false;
    // Only the classic ESP32 shares: its parallel LED bus is an I2S peripheral on instance 1 (see platform_esp32_i80.cpp), so audio has instance 0, the only one a PDM mic can use. Mirror of the bus's own probe.
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

}  // namespace mm::platform

#else  // !SOC_I2S_SUPPORTED — inert stubs so any I2S-less target links

namespace mm::platform {

bool audioMicInit(AudioMicHandle&, uint16_t, uint16_t, uint16_t, int16_t, uint32_t, MicMode) {
    return false;
}
size_t audioMicRead(AudioMicHandle&, int32_t*, size_t, uint32_t) { return 0; }
void audioMicDeinit(AudioMicHandle&) {}
bool audioMicSharedBusFree(MicMode) { return false; }   // no I2S: nothing to contend for


}  // namespace mm::platform

#endif  // SOC_I2S_SUPPORTED

// OS capture is desktop only (hasAudioCapture == false on ESP32). Outside the SOC_I2S_SUPPORTED split: discarded `if constexpr (hasAudioCapture)` branches still need a definition to link.
namespace mm::platform {
size_t audioCaptureDevices(const char* const** optionsOut) {
    if (optionsOut) *optionsOut = nullptr;
    return 0;
}
bool audioCaptureInit(AudioMicHandle& /*h*/, uint8_t /*deviceIndex*/, uint32_t /*sampleRate*/) {
    return false;
}
}  // namespace mm::platform
