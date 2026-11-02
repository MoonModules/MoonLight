#include "platform/platform.h"

#if defined(CONFIG_MM_HDMI) && defined(CONFIG_IDF_TARGET_ESP32P4)

/// The platform half of the HDMI seam: the MIPI-DSI bus, the LT8912B bridge on the board's I2C bus, and the frame scaled into the DPI framebuffer.
///
/// @attribution Espressif, Olimex, troyhacks
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
/// ## The EDID is the feedback
///
/// HDMI carries the picture one way; the monitor answers only on its hot-plug line and with its EDID, which the bridge passes through at the DDC address 0x50, at DDC's 100 kHz.
///
/// The task reads both blocks once per hot-plug, after the stream is up since the channel is live only then, and edid.h reduces them to the name, the preferred size and which of the output's modes the monitor lists. The link reports Down while the bridge is not set up, so a board without one says so rather than "no monitor".
///
/// ## Closing releases everything
///
/// A closed output holds nothing: the task, the panel and its framebuffer, the handed-over frame, the PPA client and the PHY's LDO all go, so a deleted driver costs no memory and no tick, and a reopened one starts as a cold boot does.
///
/// A chip reset drops the PHY's power and the stream with it, where a software restart would keep both and leave the bridge locked onto a stream that stops mid-frame: IDF's shutdown handler closes the output before `esp_restart`, so both restarts start alike.
///
/// ## Open
///
/// About one boot in four still shows noise, a software restart more often than a chip reset, a running picture can turn to noise, and the output reopened on a running board (the driver disabled and enabled, or removed and added) comes up without a signal; a reset of the board clears it.
///
/// The suspect is the bridge's clock synthesizer, whose read-back word (CEC page 0x0c to 0x0f) sits on the programmed value, pixel clock times 0x16C16, in a clean picture and off it in noise. The driver's `bridge` readout shows both; acting on them is open work, since resetting the receiver on a live link drops the signal.

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
#include "platform/edid.h"
#include "platform/hdmi_handover.h"
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
constexpr uint8_t kDdcAddr = 0x50;         // the monitor's EDID through the bridge: @xref{the-edid-is-the-feedback|the EDID}
constexpr uint32_t kDdcHz = 100000;        // DDC's rate; a monitor's EEPROM is slower than the bridge
constexpr uint8_t kEdidTries = 3;          // reads per hot-plug before giving up on a monitor without one
constexpr uint8_t kDownAfterPolls = 4;     // hot-plug reads that fail in a row before the bridge counts as gone
constexpr uint8_t kRegDds = 0x0d;          // the synthesizer read-back's middle bytes, which settle; 0x0c and 0x0f wander: @xref{open|open}
constexpr uint8_t kRegSync = 0x9c;         // the main page's measured input sync: h low, h high, v low, v high
constexpr uint32_t kDdsPerMhz = 0x16C16;   // the synthesizer word per MHz of pixel clock, the vendor's constant
constexpr uint32_t kHotPlugPollMs = 500;
constexpr int kBridgeI2cTimeoutMs = 50;   // a transfer stalled on the shared bus fails rather than hanging the task: @xref{a-task-owns-the-bridge|the task}
constexpr uint8_t kRegHpd = 0xc1;         // the main page's status register: bit 7 is hot-plug detect
constexpr uint32_t kStreamSettleMs = 100;  // the stream running before the receiver is reset onto it

struct Hdmi {
    esp_ldo_channel_handle_t ldo = nullptr;
    esp_lcd_dsi_bus_handle_t bus = nullptr;
    esp_lcd_panel_io_handle_t io[3] = {};
    esp_lcd_panel_io_handle_t ddc = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    ppa_client_handle_t ppa = nullptr;
    uint8_t* fb = nullptr;
    size_t fbBytes = 0;
    uint8_t mode = 0;
    TaskHandle_t task = nullptr;
    std::atomic<bool> run{false};
    std::atomic<bool> exited{true};
    std::atomic<uint8_t> link{static_cast<uint8_t>(HdmiLink::NoMonitor)};
    Seqlock<MonitorReport> monitor;
    std::atomic<uint16_t> dds{0};    // the synthesizer read-back as last read, 0 before the first
    std::atomic<uint32_t> sync{0};   // the measured sync, v in the high half and h in the low
    // The frame the tick hands over, grown by the task so the tick never allocates.
    FrameSlot slot;
    std::atomic<size_t> frameWant{0};
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
    if (g.ddc) { esp_lcd_panel_io_del(g.ddc); g.ddc = nullptr; }
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

// The on-chip LDO that powers the DSI PHY, acquired on the first bring-up and kept across the task's rebuilds.
bool powerPhy() {
    if (g.ldo) return true;
    esp_ldo_channel_config_t ldo = {};
    ldo.chan_id = kPhyLdoChannel;
    ldo.voltage_mv = kPhyLdoMillivolts;
    if (esp_ldo_acquire_channel(&ldo, &g.ldo) != ESP_OK) { ESP_LOGE(TAG, "DSI PHY power failed"); return false; }
    return true;
}

// One register-style device on the board's I2C bus: a one-byte address, then the data.
bool openIo(uint8_t addr, uint32_t hz, esp_lcd_panel_io_handle_t& out) {
    esp_lcd_panel_io_i2c_config_t io = {};
    io.dev_addr = addr;
    io.scl_speed_hz = hz;
    io.transaction_timeout_ms = kBridgeI2cTimeoutMs;
    io.control_phase_bytes = 1;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    io.flags.disable_control_phase = 1;
    return esp_lcd_new_panel_io_i2c(i2cBusHandle(), &io, &out) == ESP_OK;
}

// The bridge's three register pages, and the monitor's EDID behind it.
bool openBridgeIo() {
    for (int i = 0; i < 3; i++)
        if (!openIo(kBridgeAddr[i], kBridgeI2cHz, g.io[i])) { ESP_LOGE(TAG, "bridge I2C failed"); return false; }
    if (!openIo(kDdcAddr, kDdcHz, g.ddc)) { ESP_LOGE(TAG, "DDC I2C failed"); return false; }
    return true;
}

// Publish what the task knows of the monitor, or that it knows nothing.
void publishMonitor(const edid::Monitor* m) { g.monitor.write({m ? *m : edid::Monitor{}, m != nullptr}); }

// The monitor's EDID, both blocks, reduced to the modes the output runs: @xref{the-edid-is-the-feedback|the EDID}.
bool readEdid() {
    uint8_t base[edid::kBlockBytes], ext[edid::kBlockBytes];
    if (esp_lcd_panel_io_rx_param(g.ddc, 0x00, base, sizeof(base)) != ESP_OK) return false;
    const bool hasExt = base[126] > 0 && esp_lcd_panel_io_rx_param(g.ddc, 0x80, ext, sizeof(ext)) == ESP_OK;
    edid::Size sizes[kModeCount];
    for (size_t i = 0; i < kModeCount; i++) sizes[i] = {kModes[i].width, kModes[i].height};
    edid::Monitor m;
    if (!edid::parse(base, hasExt ? ext : nullptr, sizes, kModeCount, m)) return false;
    publishMonitor(&m);
    ESP_LOGI(TAG, "monitor %s prefers %ux%u, lists 0x%02x", m.name, m.preferredW, m.preferredH, static_cast<unsigned>(m.lists));
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

// What the hot-plug polls carry between them.
struct Poll {
    bool up = false;             ///< the panel is set up
    bool sawNoMonitor = false;   ///< a monitor that appears after this gets the bridge set up for it
    uint8_t retryIn = 0;         ///< polls until a failed bring-up is tried again
    uint8_t unread = 0;          ///< hot-plug reads that failed in a row
    uint8_t edidTries = 0;       ///< EDID reads made for the monitor connected now
};

// Bytes `n` of one bridge page from `reg` up, each read on its own as the vendor reads them; false on a failed read.
bool readRegs(esp_lcd_panel_io_handle_t io, uint8_t reg, uint8_t* out, uint8_t n) {
    for (uint8_t i = 0; i < n; i++)
        if (esp_lcd_panel_io_rx_param(io, reg + i, &out[i], 1) != ESP_OK) return false;
    return true;
}

// The bridge's synthesizer word and measured sync, for the readout: @xref{open|open}.
void readBridgeState() {
    uint8_t d[2], s[4];
    if (!readRegs(g.io[1], kRegDds, d, 2) || !readRegs(g.io[0], kRegSync, s, 4)) return;
    if (d[0] && d[1]) g.dds.store(static_cast<uint16_t>(d[0] << 8 | d[1]));   // a live counter: a read caught mid-update gives zeros, and keeps the last one
    g.sync.store(static_cast<uint32_t>(s[3] << 8 | s[2]) << 16 | (s[1] << 8 | s[0]));
}

// One hot-plug poll: a failed bring-up is tried again every few seconds, a monitor that comes back gets a fresh panel (@xref{hot-plug-rebuilds-the-panel|hot-plug}) and its EDID read, and a bridge that stops answering reports the link down.
void pollHotPlug(Poll& p) {
    if (!p.up) {
        g.link.store(static_cast<uint8_t>(HdmiLink::Down));
        if (++p.retryIn < 10) return;
        p.retryIn = 0;
        tearDownPanel();
        p.up = bringUpPanel();
        return;
    }
    const int hpd = readHpd();
    if (hpd < 0) {
        if (++p.unread >= kDownAfterPolls) {
            g.link.store(static_cast<uint8_t>(HdmiLink::Down));
            publishMonitor(nullptr);
            p.edidTries = 0;   // the bridge coming back reads the EDID afresh
        }
        return;
    }
    p.unread = 0;
    if (hpd == 0) {
        if (!p.sawNoMonitor) publishMonitor(nullptr);
        p.sawNoMonitor = true;
        p.edidTries = 0;
    } else if (p.sawNoMonitor) {
        p.sawNoMonitor = false;
        tearDownPanel();
        p.up = bringUpPanel();
    }
    if (p.up && hpd == 1 && !g.monitor.written().read && p.edidTries < kEdidTries) { p.edidTries++; readEdid(); }
    if (p.up) readBridgeState();
    g.link.store(static_cast<uint8_t>(p.up && hpd == 1 ? HdmiLink::Monitor : HdmiLink::NoMonitor));
}

// Grow the frame to what the tick last asked for, while the tick cannot be writing it: the PPA reads it by DMA, so cache-line aligned.
void growFrame() {
    const size_t want = g.frameWant.load(std::memory_order_relaxed);
    if (want <= g.frameCap || !g.slot.claimIdle()) return;
    heap_caps_free(g.frame);
    g.frameCap = (want + 127) & ~size_t(127);
    g.frame = static_cast<uint8_t*>(heap_caps_aligned_alloc(128, g.frameCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
    if (!g.frame) g.frameCap = 0;   // the tick keeps asking; a smaller layer fits again
    g.slot.release();
}

void hdmiTask(void*) {
    Poll p;
    p.up = bringUpPanel();
    TickType_t lastPoll = xTaskGetTickCount();
    while (g.run.load()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kHotPlugPollMs));
        if (!g.run.load()) break;
        const TickType_t now = xTaskGetTickCount();
        if (now - lastPoll >= pdMS_TO_TICKS(kHotPlugPollMs)) { lastPoll = now; pollHotPlug(p); }
        growFrame();
        if (p.up && g.slot.claimShow()) { show(); g.slot.release(); }
    }
    tearDownPanel();
    publishMonitor(nullptr);
    g.dds.store(0);
    g.link.store(static_cast<uint8_t>(HdmiLink::NoMonitor));
    g.exited.store(true);
    vTaskDelete(nullptr);
}

}  // namespace

size_t hdmiModes(const HdmiMode** out) MM_NONBLOCKING {
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
    static bool shutdownRegistered = false;   // before a software restart, as a chip reset leaves it: @xref{closing-releases-everything|closing}
    if (!shutdownRegistered) shutdownRegistered = esp_register_shutdown_handler(hdmiClose) == ESP_OK;
    g.mode = index;
    g.frameWant.store(0, std::memory_order_relaxed);
    g.slot.release();
    g.link.store(static_cast<uint8_t>(HdmiLink::NoMonitor));
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

// Everything the output holds goes with it: @xref{closing-releases-everything|closing}.
void hdmiClose() {
    if (!g.exited.load()) {
        g.run.store(false);
        if (g.task) xTaskNotifyGive(g.task);
        // A bring-up in progress runs to its end first: seconds, with the vendor's delays and the I2C timeouts.
        for (int i = 0; i < 500 && !g.exited.load(); i++) vTaskDelay(pdMS_TO_TICKS(10));
        if (!g.exited.load()) return;   // still running: it keeps what it uses, and the next close frees it
        g.task = nullptr;
    }
    while (!g.slot.claimUnlessWriting()) vTaskDelay(1);   // a write in flight is one memcpy; the slot then stays taken until the next open
    heap_caps_free(g.frame);
    g.frame = nullptr;
    g.frameCap = 0;
    if (g.ppa) { ppa_unregister_client(g.ppa); g.ppa = nullptr; }
    if (g.ldo) { esp_ldo_release_channel(g.ldo); g.ldo = nullptr; }
}

bool hdmiWrite(const uint8_t* rgb, uint16_t w, uint16_t h, bool fit) MM_NONBLOCKING {
    if (!rgb || w == 0 || h == 0 || !g.run.load() || !g.slot.claimWrite()) return false;
    const size_t bytes = static_cast<size_t>(w) * h * 3;
    if (bytes > g.frameCap) {
        g.frameWant.store(bytes, std::memory_order_relaxed);   // the task grows it; this frame is dropped
        g.slot.release();
        if (g.task) xTaskNotifyGive(g.task);
        return false;
    }
    std::memcpy(g.frame, rgb, bytes);
    g.frameW = w;
    g.frameH = h;
    g.frameFit = fit;
    g.slot.publish();
    if (g.task) xTaskNotifyGive(g.task);
    return true;
}

HdmiLink hdmiLink() MM_NONBLOCKING { return static_cast<HdmiLink>(g.link.load()); }

// The bridge runs the mode it was opened in, its timings being fixed per mode.
bool hdmiActiveMode(HdmiMode& out) MM_NONBLOCKING {
    if (hdmiLink() != HdmiLink::Monitor) return false;
    out = kModes[g.mode];
    return true;
}

bool hdmiBridge(HdmiBridgeState& out) MM_NONBLOCKING {
    const uint32_t dds = g.dds.load(), sync = g.sync.load();
    if (!dds || hdmiLink() == HdmiLink::Down) return false;
    const uint32_t want = mm_hdmi_dpi[g.mode].dpi_clock_freq_mhz * kDdsPerMhz;
    out = {static_cast<uint16_t>(dds), static_cast<uint16_t>(want >> 8),
           static_cast<uint16_t>(sync & 0xffff), static_cast<uint16_t>(sync >> 16)};
    return true;
}

bool hdmiMonitor(edid::Monitor& out) MM_NONBLOCKING {
    MonitorReport r;
    if (!g.monitor.read(r) || !r.read) return false;   // a torn copy waits for the next second's read
    out = r.monitor;
    return true;
}

}  // namespace mm::platform

#endif  // CONFIG_MM_HDMI && CONFIG_IDF_TARGET_ESP32P4
