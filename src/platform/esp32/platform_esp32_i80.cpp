/// The peripheral half of the whole-frame parallel driver.
///
/// The driver above applies correction and encodes every light into the frame buffer; this file owns the bus, the buffer, transmit and wait.
///
/// @technical
///
/// ## One transfer, no refill deadline
///
/// The whole frame is pre-encoded into one buffer and sent as one gapless stream. Once started no processor work remains until the completion callback, so there is no refill deadline for the radio to make it miss. That is the deliberate difference from the interrupt-refilled rings in the lineage this design studied.
///
/// ## One seam, two silicon backends
///
/// The SDK component exposes one public interface and routes it to whichever peripheral the chip has. Both do whole-frame chained transfers with the same control lines and bus widths, so this file's body is generic and serves both, and one driver runs on all three chips. The guard is therefore the broad capability macro rather than the narrow one, precisely so the classic backend compiles here too.
///
/// ## The clock is set by the slot duration
///
/// The WS2812 slot duration sets the clock; an elegant divider is secondary. An expander shifts each slot out over several bus words, so the slot is that multiplier divided by the pixel clock, and the slot IS the zero pulse, capped near 380 ns on newer revisions.
///
/// An exact divider that yields an out-of-spec slot reads zeros as ones (scattered full-brightness pixels, washed-out white), so the divider is a means rather than the goal. The rate used is an exact divide and lands the pulse inside both windows.
///
/// ## Adjust the clock upward, never down
///
/// A lower rate lengthens the slot and pushes the pulse further past its maximum, so adjust the clock upward and check the buffer can carry the rate. A wider cascade needs a rate outside the exact divides of the bus resolution; two pins beat two cascaded registers.
///
/// ## The expander frame will not mount, and the reason is open
///
/// With the expander's eightfold frame every descriptor mount fails, silently: the transmit call returns success, the mount fails later in the interrupt, and the driver waits out a timeout per frame. Six hypotheses are ruled out by measurement; the open one is the vendor's statement that with write-back off a descriptor stays owned by the engine. See the driver analysis under the shift-register work.
///
/// ## Where the frame lives, per mode
///
/// Direct mode prefers external memory: the frame is large, its clock is easy to sustain from there, and it stays out of scarce internal memory. Expander mode prefers internal, an empirical preference: with the eightfold frame external the strands flicker and the mounts fail in their thousands, while internal renders smooth.
///
/// It is a stopgap rather than the destination, internal memory capping an expander display below what direct mode already reaches. External memory is what makes large displays possible and the driver must get back to it, so it stays the fallback. A frame too big for internal still runs, at reduced quality, where refusing would leave the lights dark.
///
/// ## The classic chip's i80 is its audio peripheral
///
/// That chip has two instances and the output bus always takes the second: only the first carries the pulse-density converters, so a microphone of that kind can live nowhere else, and the output bus is the one consumer that can always yield. The component picks the first free instance, so the second is claimed by holding the first across bus creation and releasing it straight after.

#include "platform/platform.h"
#include "platform/loopback_verify.h"   // the loopback pattern and its checks

#include "sdkconfig.h"
#include "soc/soc_caps.h"

// SOC_LCD_I80_SUPPORTED: the generic esp_lcd i80 API, backed by LCD_CAM on the S3/P4 and by the I2S peripheral on the classic ESP32, both selected by IDF's own CMake. The body below is backend-agnostic.
#if SOC_LCD_I80_SUPPORTED

#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "esp_log.h"
#include "esp_timer.h"   // esp_timer_get_time — ISR-safe wire-time stamp
#include "esp_heap_caps.h"
#include "platform/esp32/platform_esp32_heap.h"   // allocCaps, freeCaps: MoonLight's blocks, counted
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstring>
#include <functional>  // the transmit callback passed to the shared frame loopback
#include <new>      // std::nothrow
#if !SOC_LCDCAM_I80_LCD_SUPPORTED
#include "esp_private/i2s_platform.h"   // the I2S0 occupancy probe (the same API esp_lcd's I2S backend uses)
#endif

namespace mm::platform {

// Defined in platform_esp32_rmt.cpp, the plain-GPIO continuity pre-check the RMT loopback uses; the wire question is identical here.
namespace detail { bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio); }

namespace {

static const char* I80_TAG = "mm_i80";

// The lcd_cmd passed to esp_lcd_panel_io_tx_color: LCD_CAM takes -1 (data only); the classic I2S backend always runs a command phase (see lcd_cmd_bits in createState), so it takes 0, a harmless byte that completes the command poll.
#if SOC_LCDCAM_I80_LCD_SUPPORTED
constexpr int kI80Cmd = -1;
#else
constexpr int kI80Cmd = 0;
#endif

// Three slots per bit, one slot HIGH for a zero: @xref{the-clock-is-set-by-the-slot-duration|why this rate and not the lineage's usual one}.
constexpr uint32_t kPclkHz = 2'666'666;

// The pixel clock with an expander fitted, an exact divide so the silent round-down stays harmless: @xref{the-clock-is-set-by-the-slot-duration|the whole window, and the direction to adjust it}.
constexpr uint32_t kShiftPclkHz = 26'666'666;   // prescale 3 of 80 MHz -> 300 ns WS2812 slots


// Two frame buffers for the deferred-wait double buffer: the driver encodes the next frame into one while the engine clocks the other out; the second is null when memory is short. A queue of started indices routes each in-order completion.
struct I80State : platform::InternalRamObject {
    esp_lcd_i80_bus_handle_t bus = nullptr;
    esp_lcd_panel_io_handle_t io = nullptr;
    SemaphoreHandle_t done[2] = {nullptr, nullptr};
    uint8_t* buf[2] = {nullptr, nullptr};
    size_t cap = 0;   // shared per-buffer capacity (both buffers equal)
    // In-order completion FIFO of enqueued buffer indices (0/1): enqueue pushes at head around tx_color, the ISR pops at tail; at most one entry per buffer.
    volatile uint8_t fifo[2] = {0, 0};
    volatile uint8_t fifoHead = 0;   // next write slot (mod 2)
    volatile uint8_t fifoTail = 0;   // next read slot (mod 2)
    // Wire-time KPI: the start timestamp of the oldest in-flight transfer (paired with the FIFO) and the last measured duration.
    volatile int64_t txStartUs[2] = {0, 0};
    volatile uint32_t lastTransmitUs = 0;
};

// The completion callback: pop the oldest enqueued index (transfers complete in order), record the wire duration and release that buffer's waiter; it runs in interrupt context, so it is resident in instruction memory.
bool IRAM_ATTR i80DoneCb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* user) {
    auto* st = static_cast<I80State*>(user);
    const uint8_t slot = st->fifoTail;
    const uint8_t b = st->fifo[slot] & 1u;
    const int64_t now = esp_timer_get_time();
    st->lastTransmitUs = static_cast<uint32_t>(now - st->txStartUs[slot]);
    st->fifoTail = (st->fifoTail + 1u) & 1u;
    // A buffer queued behind this one starts the instant this transfer ends, so its true start is stamped here (the transmit call skipped it while the wire was busy); this keeps its frameTime free of this transfer's remaining wire time.
    if (st->fifoTail != st->fifoHead) st->txStartUs[st->fifoTail] = now;
    BaseType_t high = pdFALSE;
    xSemaphoreGiveFromISR(st->done[b], &high);
    return high == pdTRUE;
}

void destroyState(I80State* st) {
    if (!st) return;
    if (st->io) esp_lcd_panel_io_del(st->io);
    if (st->bus) esp_lcd_del_i80_bus(st->bus);
    for (auto* b : st->buf) if (b) freeCaps(b);
    for (auto* s : st->done) if (s) vSemaphoreDelete(s);
    delete st;
}

// One bus + IO device + DMA buffer(s). `wantSecond` allocates the double-buffer's second frame buffer (best effort, null when memory is short). Shared by the runtime init and the loopback's private bus (one transfer).
I80State* createState(const uint16_t* dataPins, uint8_t laneCount,
                      uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes, bool wantSecond,
                      uint8_t clockMultiplier = 1) {
    auto* st = new (std::nothrow) I80State();
    if (!st) return nullptr;

    esp_lcd_i80_bus_config_t busCfg = {};
    busCfg.dc_gpio_num = static_cast<gpio_num_t>(dcGpio);
    busCfg.wr_gpio_num = static_cast<gpio_num_t>(wrGpio);
    busCfg.clk_src = LCD_CLK_SRC_DEFAULT;
    // Bus width is 8 or 16, derived from the lane count (<=8 gives 8, 9..16 gives 16); the domain driver guarantees exactly 8 or 16 real data pins.
    const size_t busWidth = laneCount <= 8 ? 8 : 16;
    busCfg.bus_width = busWidth;
    // The i80 layer rejects an NC data pin, so every data line up to bus_width is a real GPIO: unused lanes park on the WR ghost pin (hpwit's trick), where WR toggles harmlessly, and the domain driver clears their activeMask so they idle.
    for (size_t i = 0; i < ESP_LCD_I80_BUS_WIDTH_MAX; i++) {
        busCfg.data_gpio_nums[i] = (i < busWidth) ? static_cast<gpio_num_t>(wrGpio)
                                                  : GPIO_NUM_NC;
    }
    for (uint8_t i = 0; i < laneCount && i < busWidth; i++) {
        busCfg.data_gpio_nums[i] = static_cast<gpio_num_t>(dataPins[i]);
    }
    busCfg.max_transfer_bytes = bufferBytes;
    busCfg.dma_burst_size = 64;
    for (uint8_t i = 0; i < laneCount && i < busWidth; i++) gpioTake(dataPins[i]);
    gpioTake(wrGpio);
    gpioTake(dcGpio);
    if (esp_lcd_new_i80_bus(&busCfg, &st->bus) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }

    // One done-semaphore per buffer (buf[1]'s is created only when its buffer allocates).
    st->done[0] = xSemaphoreCreateBinary();
    if (!st->done[0]) {
        destroyState(st);
        return nullptr;
    }

    esp_lcd_panel_io_i80_config_t ioCfg = {};
    ioCfg.cs_gpio_num = GPIO_NUM_NC;    // no chip select — we own the bus
    // A '595 expander shifts each WS2812 slot out over 8 bus words, so the bus clocks 8x faster; kShiftPclkHz is an exact divide of the 80 MHz bus resolution because esp_lcd silently rounds an inexact pclk DOWN into a wrong waveform.
    ioCfg.pclk_hz = (clockMultiplier > 1) ? kShiftPclkHz : kPclkHz;
    // Depth two lets the deferred-wait tick hand over the next frame while the current one drains; expander mode drops to one, a narrowing that leaves the cause open: @xref{the-expander-frame-will-not-mount-and-the-reason-is-open|what was ruled out}.
    ioCfg.trans_queue_depth = (clockMultiplier > 1) ? 1 : 2;
    ioCfg.on_color_trans_done = i80DoneCb;
    ioCfg.user_ctx = st;
    // LCD_CAM skips the command phase (width zero); the classic backend always runs one and busy-waits on it, which a zero-length buffer leaves unsignaled until a watchdog reset, so it gets a one-byte phase in the idle gap inside the latch window.
#if SOC_LCDCAM_I80_LCD_SUPPORTED
    ioCfg.lcd_cmd_bits = 0;
#else
    ioCfg.lcd_cmd_bits = 8;
#endif
    ioCfg.lcd_param_bits = 0;
    ioCfg.flags.pclk_idle_low = 1;      // WR rests LOW like the data lines
    if (esp_lcd_new_panel_io_i80(st->bus, &ioCfg, &st->io) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }

    // The draw buffer, placed per mode: @xref{where-the-frame-lives-per-mode|the measurement behind the preference}. The external attempt compiles in only on chips whose engine reaches it, and the buffer is zeroed so the latch pad holds the lines LOW.
#if SOC_LCDCAM_I80_LCD_SUPPORTED
    // Only the LCD_CAM backend can reach PSRAM, so the preference exists only here; the classic ESP32's i80 is the I2S peripheral, whose DMA reaches internal RAM only, and takes the internal-only path below.
    const bool pinExpanderMode = clockMultiplier > 1;
    if (!pinExpanderMode)
        st->buf[0] = static_cast<uint8_t*>(adoptCaps(esp_lcd_i80_alloc_draw_buffer(
            st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM)));
    if (!st->buf[0])
#endif
        st->buf[0] = static_cast<uint8_t*>(adoptCaps(esp_lcd_i80_alloc_draw_buffer(
            st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));
#if SOC_LCDCAM_I80_LCD_SUPPORTED
    // Shift mode wants internal RAM; when the frame is too big for it, PSRAM is used and the flicker remains until the frame fits or the real fix lands.
    if (!st->buf[0] && pinExpanderMode) {
        ESP_LOGW(I80_TAG, "shift frame (%u B) does not fit internal DMA RAM — using PSRAM; "
                          "expect stalled transfers. Reduce lights per strand.", (unsigned)bufferBytes);
        st->buf[0] = static_cast<uint8_t*>(adoptCaps(esp_lcd_i80_alloc_draw_buffer(
            st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM)));
    }
#endif
    if (!st->buf[0]) {
        destroyState(st);
        return nullptr;
    }
    std::memset(st->buf[0], 0, bufferBytes);
    st->cap = bufferBytes;

    // The second buffer is allocated on request, so the common path pays for one frame; when it is short of memory the driver runs single-buffered.
    if (wantSecond) {
        st->done[1] = xSemaphoreCreateBinary();
        if (st->done[1]) {
            // The second buffer follows the first's policy, so the back buffer lands where the front one did; external memory spares the scarce internal heap, so that branch skips the reserve check.
#if SOC_LCDCAM_I80_LCD_SUPPORTED
            if (!pinExpanderMode)
                st->buf[1] = static_cast<uint8_t*>(adoptCaps(esp_lcd_i80_alloc_draw_buffer(
                    st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM)));
#endif
            // The internal fallback is taken only if it leaves the reserve intact, since a nice-to-have second buffer must leave the network stack its memory; otherwise the driver degrades to one buffer.
            if (!st->buf[1]
                && heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
                       >= bufferBytes + HEAP_RESERVE) {
                st->buf[1] = static_cast<uint8_t*>(adoptCaps(esp_lcd_i80_alloc_draw_buffer(
                    st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));
            }
            if (st->buf[1]) {
                std::memset(st->buf[1], 0, bufferBytes);
            } else {
                // No room for the second buffer, drop its unused semaphore, stay single-buffer.
                vSemaphoreDelete(st->done[1]);
                st->done[1] = nullptr;
            }
        }
    }
    return st;
}

} // namespace

namespace {
const char* s_lastError = nullptr;   // set by i80Ws2812Init on a refusal it can name; cold path
// Whether that refusal was CONTENTION (another module holds the instance) rather than a config fault: only contention can clear on its own, so only it earns a retry, and a bad pin set stays refused.
bool s_refusedForContention = false;
}

const char* i80Ws2812LastError() { return s_lastError; }

bool i80Ws2812SharedBusFree() {
#if !SOC_LCDCAM_I80_LCD_SUPPORTED
    // Meaningful only after THIS backend was refused for contention; probing is the acquire/release pair esp_lcd itself uses, so it is safe to call repeatedly.
    if (!s_refusedForContention) return false;
    if (i2s_platform_acquire_occupation(I2S_CTLR_HP, 1, "mm_i80_probe") != ESP_OK) return false;
    i2s_platform_release_occupation(I2S_CTLR_HP, 1);
    return true;
#else
    return false;   // LCD_CAM: the i80 bus shares nothing
#endif
}

bool i80Ws2812Init(I80Ws2812Handle& h, const uint16_t* dataPinsIn, uint8_t laneCount,
                   uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes,
                   bool wantSecondBuffer, uint8_t clockMultiplier) {
    s_lastError = nullptr;
    s_refusedForContention = false;
    if (!dataPinsIn || laneCount == 0 || bufferBytes == 0 || clockMultiplier == 0) return false;
    if (laneCount > ESP_LCD_I80_BUS_WIDTH_MAX) return false;
    uint16_t dataPins[ESP_LCD_I80_BUS_WIDTH_MAX];
    std::memcpy(dataPins, dataPinsIn, laneCount * sizeof(uint16_t));
#if !SOC_LCDCAM_I80_LCD_SUPPORTED
    // The output bus always takes the second instance: @xref{the-classic-chips-i80-is-its-audio-peripheral|why the split is fixed rather than chosen}.
    if (i2s_platform_acquire_occupation(I2S_CTLR_HP, 1, "mm_i80_probe") != ESP_OK) {
        s_lastError = "I2S1 is in use: on the classic ESP32 the parallel LED bus is an I2S "
                      "peripheral, and it drives from instance 1";
        s_refusedForContention = true;
        return false;
    }
    i2s_platform_release_occupation(I2S_CTLR_HP, 1);
    const bool parked = i2s_platform_acquire_occupation(I2S_CTLR_HP, 0, "mm_i80_park") == ESP_OK;
    struct ParkGuard {   // release instance 0 on EVERY path out of this function
        bool on;
        ~ParkGuard() { if (on) i2s_platform_release_occupation(I2S_CTLR_HP, 0); }
    } parkGuard{parked};
    // An unset write strobe (and the spare lanes parked on it) routes to a bonded input-only pad, which the matrix leaves undriven and which spends no usable pin; the data-command line needs a real pad because software toggles it every transfer.
    constexpr uint16_t kWrSink = 36;
    if (wrGpio == kBusPinUnset) wrGpio = kWrSink;
    for (uint8_t i = 0; i < laneCount; i++) if (dataPins[i] == kBusPinUnset) dataPins[i] = wrGpio;
    if (dcGpio == kBusPinUnset) {
        s_lastError = "dcPin (DC) needs a real GPIO: the i80 bus toggles it in software every frame";
        return false;
    }
#else
    // LCD_CAM (S3 / P4 / S31): both control lines need a real pad. The driver refuses an unset one before calling here; this is the backstop that keeps an invalid number away from the ROM.
    if (wrGpio == kBusPinUnset || dcGpio == kBusPinUnset) {
        s_lastError = "clockPin (WR) and dcPin (DC) need a real GPIO on this chip";
        return false;
    }
#endif
    // The expander's eightfold frame fits only in external memory, which the classic backend lacks, so it is refused here with a clean failure.
#if !SOC_LCDCAM_I80_LCD_SUPPORTED
    if (clockMultiplier > 1) return false;
#endif
    // Pre-check that the buffer can land in either region before building the bus: a frame that fits only externally (the wide-bus case) passes; the reserve floor guards internal memory only, and when both regions are full the driver idles with a status.
    const bool fitsInternal =
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
            >= bufferBytes + HEAP_RESERVE;
    // External capacity is queried on that capability ALONE, since every registered heap lacks the combination of it and the transfer capability, so a combined query reports zero; the allocation passes both. The same check guards the allocation.
#if SOC_LCDCAM_I80_LCD_SUPPORTED
    const bool fitsPsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= bufferBytes;
#else
    const bool fitsPsram = false;
#endif
    if (!fitsInternal && !fitsPsram) return false;
    I80State* st = createState(dataPins, laneCount, wrGpio, dcGpio, bufferBytes, wantSecondBuffer,
                               clockMultiplier);
    if (!st) return false;
    h.impl = st;
    return true;
}

uint8_t* i80Ws2812Buffer(const I80Ws2812Handle& h, uint8_t buffer) {
    auto* st = static_cast<I80State*>(h.impl);
    return (st && buffer < 2) ? st->buf[buffer] : nullptr;
}

size_t i80Ws2812BufferCapacity(const I80Ws2812Handle& h) {
    auto* st = static_cast<I80State*>(h.impl);
    return st ? st->cap : 0;
}

bool i80Ws2812Transmit(I80Ws2812Handle& h, uint8_t buffer, size_t bytes) {
    auto* st = static_cast<I80State*>(h.impl);
    if (!st || !st->io || buffer >= 2 || !st->buf[buffer] || bytes == 0 || bytes > st->cap) return false;
    // Push onto the completion queue BEFORE enqueuing, so a fast callback pops a populated slot; push and pop touch different slots in flight, which makes it lock-free. The transmit blocks, so it runs outside a critical section.
    const uint8_t slot = st->fifoHead;
    const bool wireIdle = (st->fifoHead == st->fifoTail);   // nothing in flight → this one starts NOW
    st->fifo[slot] = buffer;
    // Stamp the wire-time start only when the wire is IDLE (enqueue equals hardware start); a queued transfer is stamped by the done-callback when the hardware starts it, which keeps the predecessor's wire time out of this buffer's duration.
    if (wireIdle) st->txStartUs[slot] = esp_timer_get_time();
    st->fifoHead = (st->fifoHead + 1u) & 1u;
    // lcd_cmd = -1: no command phase, the transfer is one continuous GDMA data stream, gapless at the pclk rate.
    const esp_err_t err = esp_lcd_panel_io_tx_color(st->io, kI80Cmd, st->buf[buffer], bytes);
    if (err != ESP_OK) {
        // Enqueue failed, unwind the FIFO push so the ISR count stays balanced. Safe: a failed enqueue produced no transfer, so no done-callback will pop this slot.
        st->fifoHead = (st->fifoHead + 1u) & 1u;
    }
    return err == ESP_OK;
}

bool i80Ws2812Wait(I80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs) {
    auto* st = static_cast<I80State*>(h.impl);
    if (!st || buffer >= 2 || !st->done[buffer]) return true;   // nothing to wait on = not in flight
    // Report whether the transfer completed. After a timeout the DMA may still read this buffer, so the caller keeps it marked in-flight rather than re-encoding into it, which would hand a live DMA a half-rewritten buffer.
    return xSemaphoreTake(st->done[buffer], pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle& h) {
    auto* st = static_cast<I80State*>(h.impl);
    return st ? st->lastTransmitUs : 0;
}

void i80Ws2812Deinit(I80Ws2812Handle& h) {
    auto* st = static_cast<I80State*>(h.impl);
    if (!st) return;
    destroyState(st);
    h.impl = nullptr;
}

// The loopback self-test: a private full-width bus transmits the caller's real frame (full size, real descriptor chain and latch pad, back to back like the render loop), and a receive channel captures and verifies every bit.

// The capture + bit-verify half is shared with the Parlio loopback in detail::captureAndVerifyFrame (platform_esp32_rmt.cpp); only the i80 transmit differs. Declared here so this TU can call it (same pattern as loopbackJumperOk).
namespace detail {
void captureAndVerifyFrame(uint16_t rxGpio, size_t frameBytes, size_t dataBytes,
                           uint8_t rowBits, uint32_t pclkHz, bool pinExpanderMode, const char* tag,
                           const std::function<void()>& transmitOnce,
                           RmtLoopbackResult& r, bool rideMode = false,
                           uint32_t* rxSymbols = nullptr);
// Pre-allocate the capture buffer, one contiguous internal block, so a caller can take it before its own allocations fragment the heap; ownership transfers regardless of outcome.
uint32_t* allocLoopbackCapture(size_t dataBytes);
}

RmtLoopbackResult i80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                    uint16_t wrGpio, uint16_t dcGpio, uint16_t rxGpio,
                                    const uint8_t* frame, size_t frameBytes,
                                    size_t dataBytes, uint8_t rowBits,
                                    uint8_t clockMultiplier) {
    RmtLoopbackResult r = detail::loopbackResult();
    if (!detail::loopbackFrameValid(dataPins, laneCount, frame, frameBytes, dataBytes, rowBits) || clockMultiplier == 0)
        return r;
    const uint16_t txGpio = dataPins[0];   // lane 0 carries the pattern
    const bool pinExpanderMode = clockMultiplier > 1;

    if (pinExpanderMode) {
        // Skip the continuity pre-check, which expects the receive pin to follow the transmit one directly (a bare jumper; through an expander the input raises an output only at a latch); the bit-verify validates the whole chain.
        r.jumperDetected = true;
    } else {
        r.jumperDetected = detail::loopbackJumperOk(static_cast<uint8_t>(txGpio),
                                                    static_cast<uint8_t>(rxGpio));
        if (!r.jumperDetected) return r;
    }

    // The continuity check above reset txGpio's GPIO matrix route; bus creation re-claims it.
    I80State* st = createState(dataPins, laneCount, wrGpio, dcGpio, frameBytes,
                               /*wantSecond=*/false,    // one transfer — single buffer
                               clockMultiplier);        // shift mode → the kShiftPclkHz bus clock
    if (!st) {
        ESP_LOGE(I80_TAG, "loopback: private bus creation failed");
        return r;
    }
    std::memcpy(st->buf[0], frame, frameBytes);   // loopback uses buffer 0 only (single transfer)

    // The i80-specific transmit: ship one frame from buffer 0 and wait for its done-callback; the FIFO/semaphore bookkeeping matches the runtime path, and capture, cadence and bit-verify are the shared helper.
    auto transmitOnce = [st, frameBytes]() {
        // Loopback self-test path (not the render hot path): surface a failed enqueue or a done-callback timeout instead of letting it show up only as a later capture mismatch (same handling as the Parlio sibling).
        st->fifo[st->fifoHead] = 0;
        st->fifoHead = (st->fifoHead + 1u) & 1u;
        const esp_err_t err = esp_lcd_panel_io_tx_color(st->io, kI80Cmd, st->buf[0], frameBytes);
        if (err != ESP_OK) {
            st->fifoHead = (st->fifoHead + 1u) & 1u;   // unwind the push
            ESP_LOGE(I80_TAG, "loopback: tx enqueue failed (%s)", esp_err_to_name(err));
            return;
        }
        if (xSemaphoreTake(st->done[0], pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(I80_TAG, "loopback: tx done-callback timed out");
    };
    // The rate passed is the SLOT rate the strand sees, which sets the verifier's pulse threshold and expected duration; in expander mode several bus words fill one slot, so the rate is the bus rate divided by the multiplier.
    const uint32_t slotHz = (clockMultiplier > 1) ? (kShiftPclkHz / clockMultiplier) : kPclkHz;
    detail::captureAndVerifyFrame(rxGpio, frameBytes, dataBytes, rowBits, slotHz, clockMultiplier > 1,
                                  I80_TAG, transmitOnce, r);
    destroyState(st);
    return r;
}

} // namespace mm::platform

#else  // !SOC_LCD_I80_SUPPORTED — inert stubs so a chip with no i80 (LCD_CAM or I2S) links

namespace mm::platform {

bool i80Ws2812Init(I80Ws2812Handle&, const uint16_t*, uint8_t, uint16_t, uint16_t,
                   size_t, bool, uint8_t) {
    return false;
}
const char* i80Ws2812LastError() { return nullptr; }
bool i80Ws2812SharedBusFree() { return false; }
uint8_t* i80Ws2812Buffer(const I80Ws2812Handle&, uint8_t) { return nullptr; }
size_t i80Ws2812BufferCapacity(const I80Ws2812Handle&) { return 0; }
bool i80Ws2812Transmit(I80Ws2812Handle&, uint8_t, size_t) { return false; }
bool i80Ws2812Wait(I80Ws2812Handle&, uint8_t, uint32_t) { return true; }
uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle&) { return 0; }
void i80Ws2812Deinit(I80Ws2812Handle&) {}
RmtLoopbackResult i80Ws2812Loopback(const uint16_t*, uint8_t, uint16_t, uint16_t,
                                    uint16_t, const uint8_t*, size_t, size_t, uint8_t,
                                    uint8_t) {
    return {};
}

} // namespace mm::platform

#endif  // SOC_LCD_I80_SUPPORTED
