/// @defgroup platform_esp32_es8311 The audio codec bring-up
/// The control half of the microphone path on boards whose part is analog behind a codec.
///
/// The read itself stays with the other audio seam; this file only brings the codec up so it streams its converter onto the bus that read then drains.
///
/// @moreinfo
///
/// ## The domain code is unchanged
///
/// It calls the codec init, a no-op on a board with a direct microphone, before the microphone init, and reads samples as always.
/// The driver is the vendor's own managed component, built for the chips whose boards carry one.
/// The codec is a device on the board's I2C bus, which platform_esp32_i2c.cpp opens and owns.
/// Everything else gets an inert stub that reports success, having nothing to bring up, so the one call works everywhere.

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

// esp_codec_dev is pulled for the S31 and the P4 (idf_component.yml rule), so the implementation is gated on its presence and the file compiles on every other target.
#if SOC_I2S_SUPPORTED && __has_include("esp_codec_dev.h")
#define MM_HAS_ES8311 1
#endif

#if MM_HAS_ES8311

#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

#include <new>   // std::nothrow

namespace mm::platform {

namespace {

const char* ES_TAG = "mm_es8311";

// The codec device and its interfaces, kept alive between init and deinit: once opened it keeps streaming, and the I2S read drains it.
struct CodecState {
    const audio_codec_ctrl_if_t* ctrl = nullptr;
    const audio_codec_if_t*   codec   = nullptr;
    esp_codec_dev_handle_t    dev     = nullptr;
};

// esp_codec_dev_new refuses a null data interface (#128); these no-ops satisfy it, where audio_codec_new_i2s_data linked code that crashed the S31 at boot.
int noDataOpen(const audio_codec_data_if_t*, void*, int) { return ESP_CODEC_DEV_OK; }
bool noDataIsOpen(const audio_codec_data_if_t*) { return true; }
int noDataEnable(const audio_codec_data_if_t*, esp_codec_dev_type_t, bool) { return ESP_CODEC_DEV_OK; }
int noDataSetFmt(const audio_codec_data_if_t*, esp_codec_dev_type_t, esp_codec_dev_sample_info_t*) { return ESP_CODEC_DEV_OK; }
int noDataTransfer(const audio_codec_data_if_t*, uint8_t*, int) { return ESP_CODEC_DEV_NOT_SUPPORT; }
int noDataClose(const audio_codec_data_if_t*) { return ESP_CODEC_DEV_OK; }
const audio_codec_data_if_t kNoDataIf = {noDataOpen, noDataIsOpen, noDataEnable, noDataSetFmt, noDataTransfer, noDataTransfer, noDataClose};

CodecState* g_codec = nullptr;

// Tear down a partially- or fully-built CodecState in reverse order.
void deinitState(CodecState* st) {
    if (!st) return;
    if (st->dev) { esp_codec_dev_close(st->dev); esp_codec_dev_delete(st->dev); }
    if (st->codec) audio_codec_delete_codec_if(st->codec);
    if (st->ctrl) audio_codec_delete_ctrl_if(st->ctrl);   // detaches the codec from the shared bus
    delete st;
}

}  // namespace

i2c_master_bus_handle_t i2cBusHandle();   // platform_esp32_i2c.cpp: the board's bus, which the codec attaches to

bool audioCodecInit(CodecType type, uint8_t i2cAddr, uint32_t sampleRate) {
    if (type == CodecType::None) return true;     // direct-mic board: nothing to do
    if (type != CodecType::Es8311) return false;  // unknown codec for this build
    if (!i2cBusHandle()) { ESP_LOGE(ES_TAG, "no I2C bus open for the codec"); return false; }

    audioCodecDeinit();   // idempotent: a re-init (pin/rate change) rebuilds cleanly
    auto* st = new (std::nothrow) CodecState();
    if (!st) return false;

    // Only the codec's registers are ours to set over I2C: audioMicInit owns the I2S channel that reads what it streams.
    audio_codec_i2c_cfg_t i2cCtrlCfg = {};
    i2cCtrlCfg.port = I2C_NUM_0;
    // esp_codec_dev wants the 8-bit write address: the bare 0x18 reaches 0x0C, and every write NACKs (TouchMyLight, #128).
    i2cCtrlCfg.addr = static_cast<uint8_t>(i2cAddr << 1);
    i2cCtrlCfg.bus_handle = i2cBusHandle();
    st->ctrl = audio_codec_new_i2c_ctrl(&i2cCtrlCfg);
    if (!st->ctrl) { ESP_LOGE(ES_TAG, "codec i2c ctrl failed"); deinitState(st); return false; }

    es8311_codec_cfg_t es8311Cfg = {};
    es8311Cfg.ctrl_if = st->ctrl;
    es8311Cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC;   // record / mic only
    es8311Cfg.use_mclk = true;                            // the microphone's mclkPin clocks the codec
    es8311Cfg.mclk_div = 256;                             // MCLK = 256 * sample_rate (the standard
                                                          // I2S ratio; the codec's coeff table is keyed on it, 0 fails "configure rate").
    es8311Cfg.pa_pin = -1;                                // mic path needs no power amp
    st->codec = es8311_codec_new(&es8311Cfg);
    if (!st->codec) { ESP_LOGE(ES_TAG, "es8311_codec_new failed"); deinitState(st); return false; }

    esp_codec_dev_cfg_t devCfg = {};
    devCfg.codec_if = st->codec;
    devCfg.data_if = &kNoDataIf;
    devCfg.dev_type = ESP_CODEC_DEV_TYPE_IN;              // input (mic) device
    st->dev = esp_codec_dev_new(&devCfg);
    if (!st->dev) { ESP_LOGE(ES_TAG, "esp_codec_dev_new failed"); deinitState(st); return false; }

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = sampleRate;
    fs.channel = 1;
    fs.bits_per_sample = 16;
    if (esp_codec_dev_open(st->dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(ES_TAG, "esp_codec_dev_open failed");
        deinitState(st);
        return false;
    }
    esp_codec_dev_set_in_gain(st->dev, 30.0f);            // mic gain (dB), a reasonable default

    g_codec = st;
    return true;
}

void audioCodecDeinit() {
    if (g_codec) { deinitState(g_codec); g_codec = nullptr; }
}

}  // namespace mm::platform

#else  // !MM_HAS_ES8311 — no codec on this target: inert stub.

#include <new>

namespace mm::platform {
bool audioCodecInit(CodecType type, uint8_t, uint32_t) {
    // No codec: there's nothing to configure, so a None request succeeds and any codec request fails (a board asking for a codec this build can't drive).
    return type == CodecType::None;
}
void audioCodecDeinit() {}
}  // namespace mm::platform

#endif  // MM_HAS_ES8311
