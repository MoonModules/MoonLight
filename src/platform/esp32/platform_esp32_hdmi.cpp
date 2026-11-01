#include "platform/platform.h"

#if defined(CONFIG_MM_HDMI) && defined(CONFIG_IDF_TARGET_ESP32P4)

/// The platform half of the HDMI seam: the MIPI-DSI bus, the LT8912B bridge on the board's I2C bus, and the frame scaled into the DPI framebuffer.
///
/// @attribution Espressif, Olimex, Troy Hacks (WLED-MM)
///
/// @technical
///
/// ## The clocks follow the hardware
///
/// The DPI's pixel clock is 240 MHz divided by an integer, the only clocks the P4 makes exactly, and the bridge takes the pixels at the rate they leave the lanes, so the lane rate is the pixel clock times 24 bits over the two lanes. A mode off either rule shows noise: WLED-MM's finding, and the reason 1280x720 runs at 60 MHz.
///
/// ## A task owns the bridge
///
/// Setting the bridge up takes tens of milliseconds and every I2C transfer can wait on the codec sharing the bus, so neither runs on the render tick. The tick only hands over a frame through a four-state slot; the task, on core 1 with the rest of the output stage, scales it into the framebuffer with the PPA and polls hot-plug.
///
/// Each transfer to the bridge has a 50 ms timeout, since the vendor's default waits forever and one stalled transfer on the shared bus would hang the task for good.
///
/// ## The receiver is reset onto the stream
///
/// The vendor's init resets the bridge's MIPI receiver and its clock synthesizer before the DPI panel starts streaming, and on a cold chip they then settle on nothing, which shows as noise. The reset runs again 100 ms into the stream, as Linux's driver for this chip orders it.
///
/// ## Logging stalls the bring-up
///
/// IDF's USB-serial console waits up to 50 ms per write while the host is enumerated but nothing reads the port, the state of a board on a desk with no terminal open. The vendor's init logs at Info between its register writes, and with those stalls the bridge comes up without a signal, so its tag runs at Warn and this file's own line prints after the timed steps.
///
/// ## Hot-plug rebuilds the panel
///
/// A monitor connected or woken after boot is set up when the hot-plug detect rises. The vendor's init also starts the DPI panel, so it cannot run twice on one panel: the panel is deleted and created again. Nothing else restarts the link: a monitor takes seconds to lock onto a restarted signal, and some give up.
///
/// ## A software restart releases the output
///
/// A chip reset drops the PHY's power and the stream with it; a software restart keeps both, leaving the bridge locked onto a stream that stops mid-frame. IDF's shutdown handler tears the output down and releases the PHY's LDO before `esp_restart`, so both restarts start alike.
///
/// ## Open
///
/// About one boot in four still shows noise, a software restart more often than a chip reset, a running picture can turn to noise, and the output reopened on a running board (the driver disabled and enabled, or removed and added) comes up without a signal; a reset of the board clears it.
///
/// The suspect is the bridge's clock synthesizer, whose read-back word (CEC page 0x0c to 0x0f) sits on the programmed value, pixel clock times 0x16C16, in a clean picture and off it in noise. A watch on that word is open work, since resetting the receiver on a live link drops the signal.
///
/// A bridge that is absent or does not answer reads as "no monitor connected" on the driver's card; a state of its own for that is open work as well.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>

#include "driver/i2c_master.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_lt8912b.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
extern const esp_lcd_dpi_panel_config_t mm_hdmi_dpi[];
extern const esp_lcd_panel_lt8912b_video_timing_t mm_hdmi_timing[];
extern const size_t mm_hdmi_mode_count;
}

namespace mm::platform {

i2c_master_bus_handle_t i2cBusHandle();   // platform_esp32_i2c.cpp: the board's bus, which the bridge attaches to

namespace {

const char* const TAG = "hdmi";

// The order mm_hdmi_dpi and mm_hdmi_timing list.
constexpr HdmiMode kModes[] = {{800, 600, 60}, {1024, 768, 60}, {1280, 720, 60}, {1280, 800, 60}, {1920, 1080, 30}};
constexpr size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

constexpr int kPhyLdoChannel = 3;      // the on-chip LDO that powers the MIPI-DSI PHY
constexpr int kPhyLdoMillivolts = 2500;
constexpr uint8_t kLanes = 2;
constexpr uint8_t kBridgeAddr[3] = {LT8912B_IO_I2C_MAIN_ADDRESS, LT8912B_IO_I2C_CEC_ADDRESS, LT8912B_IO_I2C_AVI_ADDRESS};
constexpr uint32_t kBridgeI2cHz = 400000;
constexpr uint32_t kHotPlugPollMs = 500;
constexpr int kBridgeI2cTimeoutMs = 50;   // a transfer stalled on the shared bus fails rather than hanging the task: @xref{a-task-owns-the-bridge|the task}
constexpr uint8_t kRegHpd = 0xc1;         // the main page's status register: bit 7 is hot-plug detect
constexpr uint32_t kStreamSettleMs = 100;  // the stream running before the receiver is reset onto it

// The frame slot between the render tick and the task: free, being written, ready, being shown.
enum Slot : uint8_t { kFree, kWriting, kReady, kShowing };

struct Hdmi {
    esp_ldo_channel_handle_t ldo = nullptr;
    esp_lcd_dsi_bus_handle_t bus = nullptr;
    esp_lcd_panel_io_handle_t io[3] = {};
    esp_lcd_panel_handle_t panel = nullptr;
    ppa_client_handle_t ppa = nullptr;
    uint8_t* fb = nullptr;
    size_t fbBytes = 0;
    uint8_t mode = 0;
    TaskHandle_t task = nullptr;
    std::atomic<bool> run{false};
    std::atomic<bool> exited{true};
    std::atomic<bool> connected{false};
    // The frame the tick hands over.
    std::atomic<uint8_t> slot{kFree};
    uint8_t* frame = nullptr;
    size_t frameCap = 0;
    uint16_t frameW = 0, frameH = 0;
    bool frameFit = true;
    // What the framebuffer holds outside the picture, cleared when the picture's placement changes.
    uint16_t shownW = 0, shownH = 0;
    bool shownFit = true;
};
Hdmi g;

void tearDownPanel() {
    if (g.panel) { esp_lcd_panel_del(g.panel); g.panel = nullptr; }   // deletes the DPI panel and its framebuffer too
    g.fb = nullptr;
    g.fbBytes = 0;
    for (auto& io : g.io) if (io) { esp_lcd_panel_io_del(io); io = nullptr; }
    if (g.bus) { esp_lcd_del_dsi_bus(g.bus); g.bus = nullptr; }
}

void clearFramebuffer() {
    if (!g.fb) return;
    std::memset(g.fb, 0, g.fbBytes);
    esp_cache_msync(g.fb, g.fbBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    g.shownW = g.shownH = 0;
}

// The bridge's MIPI receiver and its clock synthesizer, reset again with the stream running: @xref{the-receiver-is-reset-onto-the-stream|the receiver}, and what stays @xref{open|open}.
bool resetReceiver() {
    const uint8_t seq[][2] = {{0x03, 0x7f}, {0x03, 0xff}, {0x05, 0xfb}, {0x05, 0xff}};
    vTaskDelay(pdMS_TO_TICKS(kStreamSettleMs));
    for (const auto& w : seq) {
        if (esp_lcd_panel_io_tx_param(g.io[0], w[0], &w[1], 1) != ESP_OK) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

// The on-chip LDO that powers the DSI PHY, acquired once and kept across rebuilds.
bool powerPhy() {
    if (g.ldo) return true;
    esp_ldo_channel_config_t ldo = {};
    ldo.chan_id = kPhyLdoChannel;
    ldo.voltage_mv = kPhyLdoMillivolts;
    if (esp_ldo_acquire_channel(&ldo, &g.ldo) != ESP_OK) { ESP_LOGE(TAG, "DSI PHY power failed"); return false; }
    return true;
}

// The bridge's three register pages on the board's I2C bus.
bool openBridgeIo() {
    for (int i = 0; i < 3; i++) {
        esp_lcd_panel_io_i2c_config_t io = {};
        io.dev_addr = kBridgeAddr[i];
        io.scl_speed_hz = kBridgeI2cHz;
        io.transaction_timeout_ms = kBridgeI2cTimeoutMs;
        io.control_phase_bytes = 1;
        io.lcd_cmd_bits = 8;
        io.lcd_param_bits = 8;
        io.flags.disable_control_phase = 1;
        if (esp_lcd_new_panel_io_i2c(i2cBusHandle(), &io, &g.io[i]) != ESP_OK) { ESP_LOGE(TAG, "bridge I2C failed"); return false; }
    }
    return true;
}

bool bringUpPanel() {
    esp_log_level_set("lt8912b", ESP_LOG_WARN);   // the vendor logs at Info between its register writes: @xref{logging-stalls-the-bring-up|a stalled log breaks the bring-up}
    if (!powerPhy()) return false;
    esp_lcd_dsi_bus_config_t bus = {};
    bus.bus_id = 0;
    bus.num_data_lanes = kLanes;
    bus.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus.lane_bit_rate_mbps = mm_hdmi_dpi[g.mode].dpi_clock_freq_mhz * 24.0f / kLanes;   // 24 bits per pixel over the lanes: @xref{the-clocks-follow-the-hardware|the clocks}
    if (esp_lcd_new_dsi_bus(&bus, &g.bus) != ESP_OK) { ESP_LOGE(TAG, "DSI bus failed"); return false; }
    if (!openBridgeIo()) return false;

    lt8912b_vendor_config_t vendor = {};
    vendor.video_timing = mm_hdmi_timing[g.mode];
    vendor.mipi_config.dsi_bus = g.bus;
    vendor.mipi_config.dpi_config = &mm_hdmi_dpi[g.mode];
    vendor.mipi_config.lane_num = kLanes;
    esp_lcd_panel_dev_config_t dev = {};
    dev.bits_per_pixel = 24;
    dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    dev.reset_gpio_num = GPIO_NUM_NC;   // the bridge's reset is not on a GPIO on these boards
    dev.vendor_config = &vendor;
    const esp_lcd_panel_lt8912b_io_t ios = {g.io[0], g.io[1], g.io[2]};
    if (esp_lcd_new_panel_lt8912b(&ios, &dev, &g.panel) != ESP_OK) { ESP_LOGE(TAG, "bridge panel failed"); return false; }
    if (esp_lcd_panel_reset(g.panel) != ESP_OK || esp_lcd_panel_init(g.panel) != ESP_OK) { ESP_LOGE(TAG, "bridge init failed"); return false; }

    void* fb = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(g.panel, 1, &fb) != ESP_OK || !fb) { ESP_LOGE(TAG, "no framebuffer"); return false; }
    g.fb = static_cast<uint8_t*>(fb);
    g.fbBytes = static_cast<size_t>(kModes[g.mode].width) * kModes[g.mode].height * 3;
    clearFramebuffer();
    if (!resetReceiver()) { ESP_LOGE(TAG, "receiver reset failed"); return false; }
    ESP_LOGI(TAG, "%ux%u at %u Hz", kModes[g.mode].width, kModes[g.mode].height, kModes[g.mode].hz);   // after the timed step, since a log can stall
    return true;
}

// The largest sixteenth at or below `s`, within the PPA's range, so the scaled picture always fits.
float ppaScale(float s) {
    s = std::floor(s * 16.0f) / 16.0f;
    return std::min(16.0f, std::max(1.0f / 16.0f, s));
}

void show() {
    const uint16_t W = kModes[g.mode].width, H = kModes[g.mode].height;
    const uint16_t w = g.frameW, h = g.frameH;
    if (!g.fb || !g.ppa || w == 0 || h == 0) return;
    float sx = ppaScale(static_cast<float>(W) / w), sy = ppaScale(static_cast<float>(H) / h);
    if (g.frameFit) sx = sy = std::min(sx, sy);
    const uint32_t outW = std::min<uint32_t>(W, static_cast<uint32_t>(w * sx));
    const uint32_t outH = std::min<uint32_t>(H, static_cast<uint32_t>(h * sy));
    if (w != g.shownW || h != g.shownH || g.frameFit != g.shownFit) {
        clearFramebuffer();   // the bars around a picture that moved or shrank
        g.shownW = w; g.shownH = h; g.shownFit = g.frameFit;
    }
    ppa_srm_oper_config_t op = {};
    op.in.buffer = g.frame;
    op.in.pic_w = w;
    op.in.pic_h = h;
    op.in.block_w = w;
    op.in.block_h = h;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
    op.out.buffer = g.fb;
    op.out.buffer_size = g.fbBytes;
    op.out.pic_w = W;
    op.out.pic_h = H;
    op.out.block_offset_x = (W - outW) / 2;
    op.out.block_offset_y = (H - outH) / 2;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x = sx;
    op.scale_y = sy;
    op.rgb_swap = true;   // the frame is R, G, B in memory, the framebuffer's RGB888 is B, G, R
    op.mode = PPA_TRANS_MODE_BLOCKING;
    if (ppa_do_scale_rotate_mirror(g.ppa, &op) != ESP_OK) ESP_LOGW(TAG, "scale failed");
}

// Hot-plug as 1 high, 0 low, -1 unread: a failed read says nothing about the monitor.
int readHpd() {
    uint8_t v = 0;
    if (esp_lcd_panel_io_rx_param(g.io[0], kRegHpd, &v, 1) != ESP_OK) return -1;
    return (v & 0x80) ? 1 : 0;
}

// One hot-plug poll: a failed bring-up is tried again every few seconds, and a monitor that comes back gets a fresh panel (@xref{hot-plug-rebuilds-the-panel|hot-plug}). Returns whether the panel is up.
bool pollHotPlug(bool up, bool& sawNoMonitor, uint8_t& retryIn) {
    if (!up) {
        if (++retryIn < 10) return false;
        retryIn = 0;
        tearDownPanel();
        return bringUpPanel();
    }
    const int hpd = readHpd();
    if (hpd == 0) sawNoMonitor = true;
    else if (hpd == 1 && sawNoMonitor) { sawNoMonitor = false; tearDownPanel(); up = bringUpPanel(); }
    if (hpd >= 0) g.connected.store(up && hpd == 1);
    return up;
}

void hdmiTask(void*) {
    bool up = bringUpPanel();
    bool sawNoMonitor = false;   // a monitor that appears after this gets the bridge set up for it
    uint8_t retryIn = 0;
    TickType_t lastPoll = xTaskGetTickCount();
    while (g.run.load()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kHotPlugPollMs));
        if (!g.run.load()) break;
        const TickType_t now = xTaskGetTickCount();
        if (now - lastPoll >= pdMS_TO_TICKS(kHotPlugPollMs)) { lastPoll = now; up = pollHotPlug(up, sawNoMonitor, retryIn); }
        uint8_t ready = kReady;
        if (up && g.slot.compare_exchange_strong(ready, kShowing)) { show(); g.slot.store(kFree); }
    }
    tearDownPanel();
    g.connected.store(false);
    g.exited.store(true);
    vTaskDelete(nullptr);
}

// Before a software restart: the output torn down and the PHY unpowered, as a chip reset leaves them: @xref{a-software-restart-releases-the-output|a software restart}.
void hdmiShutdown() {
    hdmiClose();
    if (g.ldo) { esp_ldo_release_channel(g.ldo); g.ldo = nullptr; }
}

}  // namespace

size_t hdmiModes(const HdmiMode** out) {
    if (out) *out = kModes;
    return mm_hdmi_mode_count == kModeCount ? kModeCount : 0;   // the two tables disagreeing is a build mistake: offer nothing
}

bool hdmiOpen(uint8_t index) {
    hdmiClose();
    if (!g.exited.load()) return false;   // the task is still inside a bring-up: a second task on one bridge is worse than no output this time
    if (index >= hdmiModes(nullptr) || !i2cBusHandle()) return false;
    if (!g.ppa) {
        ppa_client_config_t ppa = {};
        ppa.oper_type = PPA_OPERATION_SRM;
        ppa.max_pending_trans_num = 1;
        if (ppa_register_client(&ppa, &g.ppa) != ESP_OK) return false;
    }
    static bool shutdownRegistered = false;
    if (!shutdownRegistered) shutdownRegistered = esp_register_shutdown_handler(hdmiShutdown) == ESP_OK;
    g.mode = index;
    g.slot.store(kFree);
    g.run.store(true);
    g.exited.store(false);
    // On core 1 with the output stage, away from the render loop, HTTP and the network on core 0.
    if (xTaskCreatePinnedToCore(hdmiTask, "mmHdmi", 4096, nullptr, 2, &g.task, portNUM_PROCESSORS > 1 ? 1 : tskNO_AFFINITY) != pdPASS) {
        g.run.store(false);
        g.exited.store(true);
        return false;
    }
    return true;
}

void hdmiClose() {
    if (g.exited.load()) return;
    g.run.store(false);
    if (g.task) xTaskNotifyGive(g.task);
    // A bring-up in progress runs to its end first: seconds, with the vendor's delays and the I2C timeouts.
    for (int i = 0; i < 500 && !g.exited.load(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    g.task = nullptr;
}

bool hdmiWrite(const uint8_t* rgb, uint16_t w, uint16_t h, bool fit) MM_NONBLOCKING {
    if (!rgb || w == 0 || h == 0 || !g.run.load()) return false;
    uint8_t expect = kFree;
    if (!g.slot.compare_exchange_strong(expect, kWriting)) {
        expect = kReady;   // a frame not shown yet is replaced by the newer one
        if (!g.slot.compare_exchange_strong(expect, kWriting)) return false;   // the task is showing: drop this one
    }
    const size_t bytes = static_cast<size_t>(w) * h * 3;
    if (bytes > g.frameCap) {
        // Grown only, on the first frame of a larger layer: the PPA reads it by DMA, so cache-line aligned.
        heap_caps_free(g.frame);
        g.frameCap = (bytes + 127) & ~size_t(127);
        g.frame = static_cast<uint8_t*>(heap_caps_aligned_alloc(128, g.frameCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
        if (!g.frame) { g.frameCap = 0; g.slot.store(kFree); return false; }
    }
    std::memcpy(g.frame, rgb, bytes);
    g.frameW = w;
    g.frameH = h;
    g.frameFit = fit;
    g.slot.store(kReady);
    if (g.task) xTaskNotifyGive(g.task);
    return true;
}

bool hdmiConnected() MM_NONBLOCKING { return g.connected.load(); }

}  // namespace mm::platform

#endif  // CONFIG_MM_HDMI && CONFIG_IDF_TARGET_ESP32P4
