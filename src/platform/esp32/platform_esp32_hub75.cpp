/// @defgroup platform_esp32_hub75 HUB75 panel output
/// The peripheral half of the panel driver: the bus, the frame buffer, and the continuous scan.
///
/// The driver above applies correction and encodes the rendered frame into bit planes; no domain logic lives here.
///
/// @moreinfo
///
/// ## The output is continuous, not one-shot
///
/// That is the design difference from every other output seam in this tree.
/// A strand latches a frame and holds it, while a panel holds nothing: it displays only while it is being clocked.
/// So the controller re-scans the same buffer forever and the driver writes the next frame into it between scans.
/// There is no per-frame transmit call at all; the start arms the loop once and from then on the panel is lit by the transfer engine.
///
/// ## Keeping a discrete-frame interface scanning
///
/// The SDK's transaction model is built for discrete frames, so the scan is kept continuous the producer and consumer way.
/// A refill task queues the same buffer again the moment the previous scan completes, and the peripheral's own interrupt starts it.
/// Nothing re-arms from interrupt context, because the transmit call blocks once the transaction pool is exhausted, and the completion callback runs before the finished slot is recycled.
/// Called from there it always blocks, and a blocking queue wait in an interrupt is a watchdog panic, verified on the bench on every board preset.
///
/// ## Why the narrow capability guard
///
/// A panel port needs fourteen pins on one bus, which rules the classic chip out.
/// So there is no backend to write for it, and the broad macro would only compile dead code onto it.
///
/// ## One scan in flight, which is not a choice
///
/// Each transmit call blocks in task context until its scan finishes and recycles its slot, then queues the same buffer again.
/// And the interrupt starts it at once because the completion is still pending.
/// The gap is one task wake, microseconds against a scan of a millisecond or more, and it is uniform: a fraction of a percent of brightness rather than a flicker.
///
/// A depth of two is not available: the bus owns ONE descriptor list, and queueing a second scan mounts it while the first still owns the descriptors, which fails outright.
/// The same failure can appear once or twice right after start while the engine hands the first scan's descriptors back, after which the interrupt restarts the list as it stands.
/// Which already holds this frame, so the panel loses nothing.
///
/// The refill task's priority sits above the encode task, since a refill that lost the processor for a whole scan would leave the panel dark for that long.
/// It never deletes itself, so teardown can always delete it by handle without racing a self-delete.
///
/// ## Both control lines must be real pins
///
/// A panel has no command phase, so the data-command line signals nothing, but the layer validates it before configuring anything and rejects the whole bus otherwise.
/// Leaving it unset failed every board preset identically with a message that reads like a pin conflict and is not one.
/// So it points at the write strobe, which costs no extra pin since the command phase is never asserted.
/// The layer likewise rejects an unconnected data pin, so the lanes the layout does not use park on the strobe, where it toggles harmlessly with no panel line attached.
///
/// ## The frame must be internal memory
///
/// The peripheral keeps clocking while the transfer engine falls behind on external reads, signals completion with descriptors it still owns, and every following mount then fails.
/// One chip forecloses it anyway, its external region carrying no transfer capability at all.
/// So a frame that does not fit is refused with the depth to lower, which is a panel that says why rather than one that scans torn.

#include "platform/platform.h"
// The wire format, for the frame size: one home for it, shared with the driver that encodes.
#include "light/drivers/Hub75Slots.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#define MM_HUB75_LCDCAM  SOC_LCDCAM_I80_LCD_SUPPORTED
#define MM_HUB75_PARLIO   (SOC_PARLIO_SUPPORTED && SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH >= 16)

#if MM_HUB75_LCDCAM || MM_HUB75_PARLIO

#if MM_HUB75_LCDCAM
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "hal/lcd_periph.h"      // soc_lcd_i80_signals: the OE lane's GPIO-matrix signal
#endif
#if MM_HUB75_PARLIO
#include "driver/parlio_tx.h"
#include "hal/parlio_periph.h"   // soc_parlio_signals: the same, for Parlio
#endif
#include "esp_rom_gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstring>
#include <new>

namespace mm::platform {

namespace {

// The parallel path's proven shift clock, an exact divide of the bus resolution: the component silently rounds an inexact rate into a wrong waveform.
constexpr uint32_t kPclkHz = 20'000'000;

// Parlio's ceiling, PER_FRAME's 0x7FFFF bits, is 65,535 bytes at any bus width (as platform_esp32_parlio.cpp records), which makes a large wall an LCD_CAM job.
constexpr size_t kParlioMaxTransferBytes = 0x7FFFF / 8;

const char* g_lastError = nullptr;

// The OE bit means lit and the panel's OE is active low, so the pin is re-routed inverted and an idle bus is dark (Hub75Slots.h, "The OE bit means lit").
constexpr uint8_t kOeLane = Hub75Layout{}.oe;
void invertOe(gpio_num_t oe, int signal) { esp_rom_gpio_connect_out_signal(oe, signal, true, false); }

struct Hub75State {
    Hub75Backend backend = Hub75Backend::LcdCam;
#if MM_HUB75_LCDCAM
    esp_lcd_i80_bus_handle_t bus = nullptr;
    esp_lcd_panel_io_handle_t io = nullptr;
    TaskHandle_t refill = nullptr;         // keeps the transaction queue full, see hub75RefillTask
    std::atomic<bool> refillParked{false}; // the refill task has stopped touching this state
#endif
#if MM_HUB75_PARLIO
    parlio_tx_unit_handle_t parlio = nullptr;
#endif
    uint8_t* frame = nullptr;
    size_t   frameBytes = 0;
    // ATOMIC: the ISR callbacks read this to decide whether to touch `frame`, and teardown clears it from a task. A plain bool orders nothing between the two, so a callback could pass the check and then read a freed frame.
    std::atomic<bool> running{false};
    // Scans counted by the completion callback, so refresh is measured rather than calculated; atomic, as it crosses from the interrupt to the status tick.
    std::atomic<uint32_t> scans{0};
    uint32_t windowStartUs = 0;
    std::atomic<uint16_t> refreshHz{0};
};

// A panel is lit only while clocked: Parlio re-sends on its own, so its callback only counts, and LCD_CAM has a task queue each next scan.
#if MM_HUB75_LCDCAM
bool IRAM_ATTR hub75DoneCb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
    auto* st = static_cast<Hub75State*>(ctx);
    if (!st || !st->running.load(std::memory_order_acquire)) return false;
    st->scans.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// One scan in flight at a time: @xref{one-scan-in-flight-which-is-not-a-choice|why a second cannot be queued}.
constexpr size_t kLcdQueueDepth = 1;

void hub75RefillTask(void* arg) {
    auto* st = static_cast<Hub75State*>(arg);
    while (st->running.load(std::memory_order_acquire)) {
        if (esp_lcd_panel_io_tx_color(st->io, -1, st->frame, st->frameBytes) != ESP_OK) break;
    }
    st->refillParked.store(true, std::memory_order_release);
    vTaskSuspend(nullptr);
}
#endif

#if MM_HUB75_PARLIO
// Parlio counts only: the unit loops the buffer on its own, so re-arming here would queue a second transmission against a transfer that never ends.
bool IRAM_ATTR hub75ParlioDoneCb(parlio_tx_unit_handle_t,
                                 const parlio_tx_done_event_data_t*, void* ctx) {
    auto* st = static_cast<Hub75State*>(ctx);
    if (!st || !st->running.load(std::memory_order_acquire)) return false;
    st->scans.fetch_add(1, std::memory_order_relaxed);
    return false;
}
#endif

void destroyState(Hub75State* st) {
    if (!st) return;
    // Order matters: clear the flag, stop the peripheral (which disables its callback and waits for an in-flight one), and only then free the frame the callback reads.
    st->running.store(false, std::memory_order_release);
#if MM_HUB75_LCDCAM
    if (st->refill) {
        // The refill task parks after the current scan and must be gone before the delete; the wait is bounded, since a leaked handle beats a hung teardown.
        bool parked = false;
        for (int i = 0; i < 100 && !parked; i++) {
            parked = st->refillParked.load(std::memory_order_acquire);
            if (!parked) vTaskDelay(1);
        }
        vTaskDelete(st->refill);
        st->refill = nullptr;
        if (!parked) st->io = nullptr;
    }
    if (st->io) esp_lcd_panel_io_del(st->io);
    if (st->bus) esp_lcd_del_i80_bus(st->bus);
#endif
#if MM_HUB75_PARLIO
    if (st->parlio) {
        parlio_tx_unit_disable(st->parlio);
        parlio_del_tx_unit(st->parlio);
    }
#endif
    if (st->frame) heap_caps_free(st->frame);
    delete st;
}

/// Every panel line in the bit order the layout declares; false when one is unset, such a port being dark rather than degraded.
bool buildPinOrder(const Hub75Pins& p, uint8_t scanRate, gpio_num_t* out, size_t& count) {
    constexpr uint16_t kUnset = 0xFFFF;
    // Bits 0-5: color. Bits 8-12: address. 13: latch. 14: OE. The gap at 6-7 is deliberate, because it matches Hub75Layout's defaults, so encoder and bus agree without either knowing the other.
    const uint16_t order[15] = {
        p.r1, p.g1, p.b1, p.r2, p.g2, p.b2,
        kUnset, kUnset,               // 6, 7: unused by the default layout
        p.a, p.b, p.c, p.d, p.e,
        p.lat, p.oe,
    };
    // How many address lines this panel's scan rate actually uses: a 1/8 panel leaves D and E unwired, and demanding them would refuse a perfectly good port.
    const uint8_t addrBits = (scanRate > 16) ? 5 : (scanRate > 8) ? 4 : 3;

    count = 0;
    for (size_t bit = 0; bit < 15; bit++) {
        const bool isAddr = (bit >= 8 && bit <= 12);
        const bool needed = !isAddr || (bit - 8) < addrBits;
        if (!needed || order[bit] == kUnset) {
            // An unneeded or unwired line still occupies its bus position: the encoder puts data at fixed bit offsets, so a hole cannot shift the lines above it. NC parks it.
            out[bit] = GPIO_NUM_NC;
            count = bit + 1;
            continue;
        }
        out[bit] = static_cast<gpio_num_t>(order[bit]);
        count = bit + 1;
    }

    // The lines without which nothing lights.
    const uint16_t required[] = {p.r1, p.g1, p.b1, p.r2, p.g2, p.b2,
                                 p.a, p.b, p.c, p.clk, p.lat, p.oe};
    for (uint16_t pin : required) {
        if (pin == kUnset) return false;
    }
    if (addrBits >= 4 && p.d == kUnset) return false;
    if (addrBits >= 5 && p.e == kUnset) return false;
    return true;
}

}  // namespace

const char* hub75LastError() { return g_lastError; }

const char* hub75BackendLabel(Hub75Backend backend) {
    return backend == Hub75Backend::Parlio ? "Parlio" : "LCD_CAM";
}

bool hub75BackendAvailable(Hub75Backend backend, size_t frameBytes) {
    if (backend == Hub75Backend::LcdCam) {
#if MM_HUB75_LCDCAM
        // LCD_CAM has no transfer-size cap of its own: the frame size is a memory question (internal DRAM first, see the allocation in hub75Init).
        (void)frameBytes;
        return true;
#else
        return false;
#endif
    }
#if MM_HUB75_PARLIO
    // Parlio's single-shot transfer caps at 65,535 bytes, width-invariant. Offering it for a frame that cannot fit would be a choice that fails at init, so the ceiling is part of "available".
    return frameBytes <= kParlioMaxTransferBytes;
#else
    (void)frameBytes;
    return false;
#endif
}

bool hub75Init(Hub75Handle& h, Hub75Backend backend, const Hub75Pins& pins,
               uint16_t width, uint16_t height, uint8_t scanRate, uint8_t bitDepth) {
    hub75Deinit(h);
    g_lastError = nullptr;

    if (width == 0 || height == 0 || scanRate == 0) {
        g_lastError = "set the panel size and scan rate";
        return false;
    }
    // A real panel is 1/8, 1/16 or 1/32, and buildPinOrder derives the address width from it, so a config file's 3 would ask for an address no panel has.
    if (scanRate != 8 && scanRate != 16 && scanRate != 32) {
        g_lastError = "scan rate must be 1/8, 1/16 or 1/32";
        return false;
    }
    if (height % scanRate != 0 || ((height / scanRate) % 2) != 0) {
        g_lastError = "the scan rate does not divide the panel height into row pairs";
        return false;
    }

    gpio_num_t busPins[16] = {};
    size_t busCount = 0;
    if (!buildPinOrder(pins, scanRate, busPins, busCount)) {
        g_lastError = "a HUB75 line is unset: every color, address and control pin is required";
        return false;
    }

    // The size comes from the encoder, which is the one home for the wire format. Recomputing it here is how the two silently disagreed: this must be the exact buffer hub75Encode writes, or a correct encode overruns the DMA frame.
    mm::Hub75Geometry geo;
    geo.width = width; geo.height = height; geo.scanRate = scanRate; geo.bitDepth = bitDepth;
    if (!geo.valid()) {
        g_lastError = "the panel size, scan rate or bit depth (2 to 6) cannot be encoded";
        return false;
    }
    const size_t frameBytesPre = geo.frameBytes();
    if (!hub75BackendAvailable(backend, frameBytesPre)) {
        // Named, never substituted: a user keeping LCD_CAM free for strips must hear that the panel does not fit Parlio.
        g_lastError = (backend == Hub75Backend::Parlio)
            ? "this panel is too big for Parlio (65,535 byte limit): use LCD_CAM or lower the depth"
            : "this chip has no LCD_CAM";
        return false;
    }

    auto* st = new (std::nothrow) Hub75State();
    if (!st) {
        g_lastError = "out of memory";
        return false;
    }
    st->backend = backend;

    if (backend == Hub75Backend::Parlio) {
#if MM_HUB75_PARLIO
        parlio_tx_unit_config_t cfg = {};
        cfg.clk_src = PARLIO_CLK_SRC_DEFAULT;
        cfg.data_width = 16;              // the layout puts OE at bit 14
        cfg.clk_in_gpio_num = GPIO_NUM_NC;
        cfg.valid_gpio_num = GPIO_NUM_NC;
        cfg.clk_out_gpio_num = static_cast<gpio_num_t>(pins.clk);
        // Parlio ACCEPTS an NC data lane where the i80 layer rejects one, so the layout's holes (bits 6, 7 and 15) stay unparked here. The i80 branch below has to park them on WR instead.
        for (size_t i = 0; i < 16; i++) {
            cfg.data_gpio_nums[i] = (i < busCount) ? busPins[i] : GPIO_NUM_NC;
        }
        cfg.output_clk_freq_hz = kPclkHz;
        cfg.trans_queue_depth = 1;
        cfg.max_transfer_size = frameBytesPre;
        // The edge the panel's chips sample on: rising for most, falling for some, and the wrong one puts every pixel one column over.
        cfg.shift_edge = pins.clkFalling ? PARLIO_SHIFT_EDGE_NEG : PARLIO_SHIFT_EDGE_POS;
        if (parlio_new_tx_unit(&cfg, &st->parlio) != ESP_OK) {
            g_lastError = "could not claim the Parlio unit: is another driver using it?";
            destroyState(st);
            return false;
        }
        invertOe(busPins[kOeLane], soc_parlio_signals[0].tx_units[0].data_sigs[kOeLane]);
        parlio_tx_event_callbacks_t cbs = {};
        cbs.on_trans_done = hub75ParlioDoneCb;
        parlio_tx_unit_register_event_callbacks(st->parlio, &cbs, st);
        parlio_tx_unit_enable(st->parlio);
#else
        g_lastError = "this chip has no Parlio";
        destroyState(st);
        return false;
#endif
    } else {
#if MM_HUB75_LCDCAM
    // The bus is 16 bits wide because the layout puts OE at bit 14: the color and control lines do not fit in 8. A board is free to re-map them into the low byte (Hub75Layout allows it), but the default wiring is the one this must support.
    esp_lcd_i80_bus_config_t busCfg = {};
    // The data-command line rides the write strobe and must be a real pin: @xref{both-control-lines-must-be-real-pins|why leaving it unset fails the whole bus}.
    busCfg.dc_gpio_num = static_cast<gpio_num_t>(pins.clk);
    busCfg.wr_gpio_num = static_cast<gpio_num_t>(pins.clk);   // WR IS the panel's shift clock
    busCfg.clk_src = LCD_CLK_SRC_DEFAULT;
    busCfg.bus_width = 16;
    // Every lane up to the bus width must be a real pin, so the ones the layout leaves as holes park on the strobe: @xref{both-control-lines-must-be-real-pins|the same reason}.
    for (size_t i = 0; i < ESP_LCD_I80_BUS_WIDTH_MAX; i++) {
        busCfg.data_gpio_nums[i] = (i < 16) ? static_cast<gpio_num_t>(pins.clk) : GPIO_NUM_NC;
    }
    for (size_t i = 0; i < busCount && i < 16; i++) {
        if (busPins[i] != GPIO_NUM_NC) {
            busCfg.data_gpio_nums[i] = busPins[i];
        }
    }

    busCfg.max_transfer_bytes = frameBytesPre;
    busCfg.dma_burst_size = 64;
    const esp_err_t busErr = esp_lcd_new_i80_bus(&busCfg, &st->bus);
    if (busErr != ESP_OK) {
        // Name the reason: a refused pin returns ESP_ERR_INVALID_ARG, which a generic "in use" message would hide behind a conflict that does not exist.
        g_lastError = (busErr == ESP_ERR_INVALID_ARG)
            ? "the LCD bus refused a pin: check the board's GPIO map"
            : (busErr == ESP_ERR_NOT_FOUND)
                ? "no free LCD bus: another driver is using it"
                : "the LCD bus would not start";
        destroyState(st);
        return false;
    }

    invertOe(busPins[kOeLane], soc_lcd_i80_signals[0].data_sigs[kOeLane]);

    esp_lcd_panel_io_i80_config_t ioCfg = {};
    ioCfg.cs_gpio_num = GPIO_NUM_NC;
    ioCfg.pclk_hz = kPclkHz;
    ioCfg.trans_queue_depth = kLcdQueueDepth;   // why 1: see hub75RefillTask
    ioCfg.on_color_trans_done = hub75DoneCb;
    ioCfg.user_ctx = st;
    ioCfg.lcd_cmd_bits = 0;     // LCD_CAM: tx_color(-1) skips the command phase entirely
    ioCfg.lcd_param_bits = 0;
    ioCfg.flags.pclk_idle_low = 1;
    // Same edge choice as the Parlio branch: the display "writes" (samples) on the falling edge when set.
    ioCfg.flags.pclk_active_neg = pins.clkFalling ? 1 : 0;
    if (esp_lcd_new_panel_io_i80(st->bus, &ioCfg, &st->io) != ESP_OK) {
        g_lastError = "could not configure the LCD device";
        destroyState(st);
        return false;
    }
#else
        g_lastError = "this chip has no LCD_CAM";
        destroyState(st);
        return false;
#endif
    }

    // Internal transfer-capable memory and only that: @xref{the-frame-must-be-internal-memory|why external fails at this clock}. A single panel at this depth is a few tens of kilobytes and four are a few times that, both internal-sized.
    st->frame = static_cast<uint8_t*>(heap_caps_calloc(1, frameBytesPre, MALLOC_CAP_DMA));
    if (!st->frame) {
        g_lastError = "the panel frame does not fit in memory: lower the bit depth";
        destroyState(st);
        return false;
    }
    st->frameBytes = frameBytesPre;
    st->windowStartUs = static_cast<uint32_t>(esp_timer_get_time());

    h.impl = st;
    return true;
}

uint8_t* hub75Buffer(const Hub75Handle& h) MM_NONBLOCKING {
    return h.impl ? static_cast<Hub75State*>(h.impl)->frame : nullptr;
}

size_t hub75BufferCapacity(const Hub75Handle& h) MM_NONBLOCKING {
    return h.impl ? static_cast<Hub75State*>(h.impl)->frameBytes : 0;
}

bool hub75Start(Hub75Handle& h) {
    auto* st = static_cast<Hub75State*>(h.impl);
    if (!st || !st->frame) return false;
    if (st->running.load(std::memory_order_acquire)) return true;   // starting twice would queue two
    st->running.store(true, std::memory_order_release);
    bool ok = false;
    if (st->backend == Hub75Backend::Parlio) {
#if MM_HUB75_PARLIO
        parlio_transmit_config_t xcfg = {};
        // Repeat forever until the unit is disabled: this is what keeps the panel clocked without an ISR re-arming it, and it is why the Parlio done callback only counts.
        xcfg.flags.loop_transmission = 1;
        // Parlio counts in BITS, the one API difference from the LCD_CAM path.
        ok = st->parlio && parlio_tx_unit_transmit(st->parlio, st->frame,
                                                   st->frameBytes * 8, &xcfg) == ESP_OK;
#endif
    } else {
#if MM_HUB75_LCDCAM
        // The first scan is queued here, so the refill task starts by blocking on it rather than finding the bus idle.
        ok = st->io && esp_lcd_panel_io_tx_color(st->io, -1, st->frame, st->frameBytes) == ESP_OK;
        if (ok) {
            st->refillParked.store(false, std::memory_order_release);
            ok = xTaskCreate(&hub75RefillTask, "mmHub75", 4096, st, 6, &st->refill) == pdPASS;
            if (!ok) st->refill = nullptr;
        }
#endif
    }
    if (!ok) {
        st->running.store(false, std::memory_order_release);
        g_lastError = "the panel scan would not start";
    }
    return ok;
}

uint16_t hub75RefreshHz(const Hub75Handle& h) MM_NONBLOCKING {
    auto* st = static_cast<Hub75State*>(h.impl);
    if (!st || !st->running.load(std::memory_order_acquire)) return 0;
    // Averaged over a ~1 s window, recomputed on read. Cheap enough for a 1 Hz status poll, and it keeps the ISR down to one increment.
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t elapsed = now - st->windowStartUs;
    if (elapsed >= 1'000'000u) {
        const uint32_t scans = st->scans.exchange(0, std::memory_order_relaxed);
        st->refreshHz.store(static_cast<uint16_t>(
            (static_cast<uint64_t>(scans) * 1'000'000u) / elapsed),
            std::memory_order_relaxed);
        st->windowStartUs = now;
    }
    return st->refreshHz.load(std::memory_order_relaxed);
}

const char* hub75Backend(const Hub75Handle& h) {
    auto* st = static_cast<Hub75State*>(h.impl);
    return st ? hub75BackendLabel(st->backend) : nullptr;
}

void hub75Deinit(Hub75Handle& h) {
    destroyState(static_cast<Hub75State*>(h.impl));
    h.impl = nullptr;
}

}  // namespace mm::platform

#else   // no LCD_CAM i80 on this chip

namespace mm::platform {

// Inert on silicon without the peripheral (the classic ESP32). The driver reports the cause, the same allocate-and-degrade rule every other output seam follows.
const char* hub75LastError() { return "HUB75 needs an ESP32-S3, P4 or S31"; }
bool hub75BackendAvailable(Hub75Backend, size_t) { return false; }
const char* hub75BackendLabel(Hub75Backend backend) {
    return backend == Hub75Backend::Parlio ? "Parlio" : "LCD_CAM";
}
bool hub75Init(Hub75Handle&, Hub75Backend, const Hub75Pins&, uint16_t, uint16_t,
               uint8_t, uint8_t) {
    return false;
}
uint8_t* hub75Buffer(const Hub75Handle&) MM_NONBLOCKING { return nullptr; }
size_t hub75BufferCapacity(const Hub75Handle&) MM_NONBLOCKING { return 0; }
bool hub75Start(Hub75Handle&) { return false; }
uint16_t hub75RefreshHz(const Hub75Handle&) MM_NONBLOCKING { return 0; }
const char* hub75Backend(const Hub75Handle&) { return nullptr; }
void hub75Deinit(Hub75Handle&) {}

}  // namespace mm::platform

#endif  // SOC_LCDCAM_I80_LCD_SUPPORTED
