/// The peripheral half of the parallel driver on the P4.
///
/// The driver above shares its encoder with the other parallel backend, one bus byte per slot; this file owns the transmit unit, the buffer, transmit and wait.
///
/// @technical
///
/// ## The same one-transfer design, on a simpler peripheral
///
/// The whole frame is pre-encoded into one buffer and sent as one autonomous transfer, so the processor is idle from start until the completion callback. The peripheral takes the data pins directly and generates the pixel clock internally, so the bus is always eight lanes wide and an unused lane stays undriven.
///
/// ## One transaction means a hard frame ceiling
///
/// This peripheral clocks the whole buffer out in one transaction, so a frame must fit its single-transfer register. A unit created oversized fails EVERY transmit, because the vendor checks the configured maximum rather than the payload, and the output goes silently dark; the driver rejects up front with a clear status.
///
/// The limit counts buffer bytes at either bus width: one light is its channels times eight bits times three slots. A slot is one byte on the narrow bus (seventy-two bytes for three channels, a few hundred lights a lane) and two bytes on the wide bus. Higher totals need the backlogged chunked-transfer work.
//
// Compiles on every ESP32 chip: everything is under SOC_PARLIO_SUPPORTED with inert stubs otherwise; the driver never calls in (platform::parlioLanes == 0).

#include "platform/platform.h"
#include "platform/loopback_verify.h"   // the loopback pattern and its checks

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_PARLIO_SUPPORTED

#include "driver/parlio_tx.h"
#include "driver/gpio.h"        // gpio_num_t / GPIO_NUM_NC for the unit's pin map
#include "esp_log.h"
#include "esp_timer.h"   // esp_timer_get_time — ISR-safe wire-time stamp
#include "esp_heap_caps.h"
#include "platform/esp32/platform_esp32_heap.h"   // allocCaps, freeCaps: MoonLight's blocks, counted
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstring>
#include <functional>  // the transmit callback passed to the shared frame loopback

namespace mm::platform {

namespace {

static const char* PAR_TAG = "mm_parlio";

// Parlio data_width is a power of two derived from the lane count: up to 8 lanes give an 8-bit bus (uint8 slots, bit L = lane L), 9..16 a 16-bit bus (uint16 slots); unlike i80, Parlio accepts NC for an unused lane.
constexpr size_t kMaxBusWidth = 16;   // both peripherals' physical ceiling

// WS2812 slot rate (375 ns @ 2.67 MHz), same value the driver passes at init, the loopback creates its own private unit and needs the constant directly.
constexpr uint32_t kPclkHz = 2'666'666;

// Two frame buffers for the deferred-wait double buffer, the same shape the sibling backend uses; the second is null when memory is short. Transfers complete in order, so a two-slot queue routes each signal to the buffer that finished.
struct ParlioState : platform::InternalRamObject {
    parlio_tx_unit_handle_t unit = nullptr;
    SemaphoreHandle_t done[2] = {nullptr, nullptr};
    uint8_t* buf[2] = {nullptr, nullptr};
    size_t cap = 0;   // shared per-buffer capacity (both buffers equal)
    // Storage for the two done-semaphores. Static (not xSemaphoreCreateBinary) so the control block lives inside this internal-RAM struct, where the cache-safe ISR can reach it.
    StaticSemaphore_t doneBuf[2] = {};
    volatile uint8_t fifo[2] = {0, 0};
    volatile uint8_t fifoHead = 0;
    volatile uint8_t fifoTail = 0;
    // Wire-time KPI (see the i80 driver): start stamp per in-flight transfer + last measured duration.
    volatile int64_t txStartUs[2] = {0, 0};
    volatile uint32_t lastTransmitUs = 0;
};

// Done-callback: pop the oldest enqueued buffer index (in-order completion), record the wire duration and release THAT buffer's waiter; esp_timer_get_time reads a hardware counter, so it is ISR-safe.
bool IRAM_ATTR parlioDoneCb(parlio_tx_unit_handle_t, const parlio_tx_done_event_data_t*,
                            void* user) {
    auto* st = static_cast<ParlioState*>(user);
    const uint8_t slot = st->fifoTail;
    const uint8_t b = st->fifo[slot] & 1u;
    const int64_t now = esp_timer_get_time();
    st->lastTransmitUs = static_cast<uint32_t>(now - st->txStartUs[slot]);
    st->fifoTail = (st->fifoTail + 1u) & 1u;
    // A buffer queued behind this one starts the instant this transfer ends, so its true start is stamped here (parlioWs2812Transmit skipped it while the wire was busy); this keeps the second buffer's frameTime honest.
    if (st->fifoTail != st->fifoHead) st->txStartUs[st->fifoTail] = now;
    BaseType_t high = pdFALSE;
    xSemaphoreGiveFromISR(st->done[b], &high);
    return high == pdTRUE;
}

void destroyState(ParlioState* st) {
    if (!st) return;
    if (st->unit) {
        parlio_tx_unit_disable(st->unit);
        parlio_del_tx_unit(st->unit);
    }
    for (auto* b : st->buf) if (b) freeCaps(b);
    for (auto* s : st->done) if (s) vSemaphoreDelete(s);
    delete st;
}

// One TX unit + DMA buffer(s). pclkHz is the WS2812 slot rate (2.67 MHz). `wantSecond` allocates the async double-buffer's second frame buffer (best-effort); false → buffer 0 only.
ParlioState* createState(const uint16_t* dataPins, uint8_t laneCount,
                         uint32_t pclkHz, size_t bufferBytes, bool wantSecond) {
    auto* st = new (std::nothrow) ParlioState();
    if (!st) return nullptr;

    parlio_tx_unit_config_t cfg = {};
    cfg.clk_src = PARLIO_CLK_SRC_DEFAULT;     // PLL_F160M → /60 = 2.67 MHz
    cfg.clk_in_gpio_num = GPIO_NUM_NC;        // internal clock, not external
    cfg.output_clk_freq_hz = pclkHz;
    const size_t busWidth = laneCount <= 8 ? 8 : 16;   // power-of-two, derived
    cfg.data_width = busWidth;
    for (size_t i = 0; i < kMaxBusWidth; i++) cfg.data_gpio_nums[i] = GPIO_NUM_NC;
    for (uint8_t i = 0; i < laneCount && i < busWidth; i++)
        cfg.data_gpio_nums[i] = static_cast<gpio_num_t>(dataPins[i]);
    cfg.clk_out_gpio_num = GPIO_NUM_NC;       // WS2812 ignores the clock line
    cfg.valid_gpio_num = GPIO_NUM_NC;
    // Queue depth 2 lets the tick enqueue the next frame's transfer while the current one drains; the driver waits before REUSING a buffer, so one transfer per buffer is outstanding, and single-buffer boards use buf[0] with harmless extra depth.
    cfg.trans_queue_depth = 2;
    cfg.max_transfer_size = bufferBytes;
    cfg.dma_burst_size = 64;
    cfg.shift_edge = PARLIO_SHIFT_EDGE_POS;   // shift data on the clock's rising edge
    cfg.bit_pack_order = PARLIO_BIT_PACK_ORDER_MSB;
    for (uint8_t i = 0; i < laneCount && i < busWidth; i++) gpioTake(dataPins[i]);
    if (parlio_new_tx_unit(&cfg, &st->unit) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }

    // Internal-RAM semaphore: the ISR gives it with the cache disabled (see createState).
    st->done[0] = xSemaphoreCreateBinaryStatic(&st->doneBuf[0]);
    if (!st->done[0]) { destroyState(st); return nullptr; }
    parlio_tx_event_callbacks_t cbs = {};
    cbs.on_trans_done = parlioDoneCb;
    if (parlio_tx_unit_register_event_callbacks(st->unit, &cbs, st) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }
    if (parlio_tx_unit_enable(st->unit) != ESP_OK) { destroyState(st); return nullptr; }

    // The draw buffer, external memory first with an internal fallback, with the cache alignment the external engine needs; the external request fails on this chip, so the largest internal block caps the wide bus until chunked transfers land.
    st->buf[0] = static_cast<uint8_t*>(allocCaps(
        bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
    // The internal fallback obeys the same reserve rule as the second buffer, which closes the hole the init gate leaves (external memory reports room, then the allocation fails) and spares the network stack.
    if (!st->buf[0]
        && heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
               >= bufferBytes + HEAP_RESERVE) {
        st->buf[0] = static_cast<uint8_t*>(allocCaps(
            bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL, 64));
    }
    if (!st->buf[0]) { destroyState(st); return nullptr; }
    std::memset(st->buf[0], 0, bufferBytes);
    st->cap = bufferBytes;

    // Second buffer for the async double-buffer, ONLY when asked (wantSecond); off by default, so the common path allocates exactly one buffer. Same allocate-and-degrade as the i80 driver: buf[1] null (won't-fit or not-wanted) means single-buffer mode.
    if (wantSecond) {
        st->done[1] = xSemaphoreCreateBinaryStatic(&st->doneBuf[1]);
        if (st->done[1]) {
            // PSRAM first (no internal-heap impact). Internal fallback ONLY if it leaves HEAP_RESERVE intact, the second buffer is a nice-to-have, so it must never eat the WiFi/HTTP reserve (see the i80 driver). Degrade to single-buffer otherwise.
            st->buf[1] = static_cast<uint8_t*>(allocCaps(
                bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
            if (!st->buf[1]
                && heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
                       >= bufferBytes + HEAP_RESERVE) {
                st->buf[1] = static_cast<uint8_t*>(allocCaps(
                    bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL, 64));
            }
            if (st->buf[1]) {
                std::memset(st->buf[1], 0, bufferBytes);
            } else {
                vSemaphoreDelete(st->done[1]);
                st->done[1] = nullptr;
            }
        }
    }
    return st;
}

}  // namespace

// The single-transfer ceiling: @xref{one-transaction-means-a-hard-frame-ceiling|the limit, and what a light really costs}.
inline constexpr size_t kParlioMaxTransferBytes = 0x7FFFF / 8;   // 65535 (buffer bytes, width-invariant)

bool parlioWs2812Init(ParlioWs2812Handle& h, const uint16_t* dataPins,
                      uint8_t laneCount, uint32_t pclkHz, size_t bufferBytes,
                      bool wantSecondBuffer) {
    if (!dataPins || laneCount == 0 || bufferBytes == 0) return false;
    // Reject a frame larger than one transaction can clock out, since the created unit would fail every transmit silently (see kParlioMaxTransferBytes); the driver reports a status, and the remedy is fewer lights per lane or the start/count window.
    if (bufferBytes > kParlioMaxTransferBytes) return false;
    // Can the frame be placed at all? This mirrors the allocation: external first, then internal. Gating on internal alone rejected frames external memory could hold, and the reserve applies to the internal path only.
    const bool fitsPsram = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM) >= bufferBytes;
    const bool fitsInternal = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
                              >= bufferBytes + HEAP_RESERVE;
    if (!fitsPsram && !fitsInternal) return false;
    ParlioState* st = createState(dataPins, laneCount, pclkHz, bufferBytes, wantSecondBuffer);
    if (!st) return false;
    h.impl = st;
    return true;
}

uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle& h, uint8_t buffer) {
    auto* st = static_cast<ParlioState*>(h.impl);
    return (st && buffer < 2) ? st->buf[buffer] : nullptr;
}

size_t parlioMaxTransferBytes() { return kParlioMaxTransferBytes; }

size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle& h) {
    auto* st = static_cast<ParlioState*>(h.impl);
    return st ? st->cap : 0;
}

bool parlioWs2812Transmit(ParlioWs2812Handle& h, uint8_t buffer, size_t bytes) {
    auto* st = static_cast<ParlioState*>(h.impl);
    if (!st || buffer >= 2 || !st->buf[buffer] || bytes == 0 || bytes > st->cap) return false;
    parlio_transmit_config_t xcfg = {};
    xcfg.idle_value = 0;   // lines rest LOW between/after the frame (the latch)
    // Push onto the completion queue before enqueuing; push and pop touch different slots in flight, which makes it lock-free. The transmit blocks on an internal queue, so it runs outside a critical section (a blocking call with interrupts off panics).
    const uint8_t slot = st->fifoHead;
    const bool wireIdle = (st->fifoHead == st->fifoTail);   // nothing in flight → this one starts NOW
    st->fifo[slot] = buffer;
    // Stamp the start only when the wire is idle, where enqueuing IS the hardware start; otherwise the callback stamps it as it completes the predecessor, which keeps that transfer's remaining time out of this buffer's measurement.
    if (wireIdle) st->txStartUs[slot] = esp_timer_get_time();
    st->fifoHead = (st->fifoHead + 1u) & 1u;
    // payload length is in BITS; the buffer is bytes × 8 lanes-worth of slots.
    const esp_err_t err = parlio_tx_unit_transmit(st->unit, st->buf[buffer], bytes * 8, &xcfg);
    if (err != ESP_OK) st->fifoHead = (st->fifoHead + 1u) & 1u;   // unwind the push (no transfer → no pop)
    return err == ESP_OK;
}

bool parlioWs2812Wait(ParlioWs2812Handle& h, uint8_t buffer, uint32_t timeoutMs) {
    auto* st = static_cast<ParlioState*>(h.impl);
    if (!st || buffer >= 2 || !st->done[buffer]) return true;   // nothing to wait on = not in flight
    // Wait on the specific buffer's done-semaphore (the ISR gives it via the completion FIFO) and REPORT the outcome: on a timeout the DMA may still be reading this buffer, so the caller must not re-encode into it (see i80Ws2812Wait).
    return xSemaphoreTake(st->done[buffer], pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

uint32_t parlioWs2812LastTransmitUs(const ParlioWs2812Handle& h) {
    auto* st = static_cast<ParlioState*>(h.impl);
    return st ? st->lastTransmitUs : 0;
}

void parlioWs2812Deinit(ParlioWs2812Handle& h) {
    auto* st = static_cast<ParlioState*>(h.impl);
    if (!st) return;
    destroyState(st);
    h.impl = nullptr;
}

// The loopback self-test: a private transmit unit on the driver's data pins sends the caller's real frame back to back while a receive channel captures and verifies every bit; it is the sibling's loopback with this transmit (length in bits).

// loopbackJumperOk + captureAndVerifyFrame live in platform_esp32_rmt.cpp (the shared continuity check and the shared capture+bit-verify all three loopback rigs reuse); declared here so this TU can call them.
namespace detail {
bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio);
void captureAndVerifyFrame(uint16_t rxGpio, size_t frameBytes, size_t dataBytes,
                           uint8_t rowBits, uint32_t pclkHz, bool pinExpanderMode, const char* tag,
                           const std::function<void()>& transmitOnce,
                           RmtLoopbackResult& r, bool rideMode = false,
                           uint32_t* rxSymbols = nullptr);
// Pre-allocate the capture buffer, one contiguous internal block, so a caller can take it before its own allocations fragment the heap; ownership transfers regardless of outcome.
uint32_t* allocLoopbackCapture(size_t dataBytes);
}

RmtLoopbackResult parlioWs2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                       uint16_t rxGpio, const uint8_t* frame,
                                       size_t frameBytes, size_t dataBytes,
                                       uint8_t rowBits) {
    RmtLoopbackResult r = detail::loopbackResult();
    if (!detail::loopbackFrameValid(dataPins, laneCount, frame, frameBytes, dataBytes, rowBits)) return r;
    const uint16_t txGpio = dataPins[0];   // lane 0 carries the pattern

    r.jumperDetected = detail::loopbackJumperOk(static_cast<uint8_t>(txGpio),
                                                static_cast<uint8_t>(rxGpio));
    if (!r.jumperDetected) return r;

    // The continuity check above reset txGpio's GPIO matrix route; the TX unit creation below re-claims it.
    ParlioState* st = createState(dataPins, laneCount, kPclkHz, frameBytes,
                                  /*wantSecond=*/false);   // one transfer — single buffer
    if (!st) {
        ESP_LOGE(PAR_TAG, "loopback: private TX unit creation failed");
        return r;
    }
    std::memcpy(st->buf[0], frame, frameBytes);   // loopback uses buffer 0 only (single transfer)

    // The Parlio-specific transmit: ship one frame from buffer 0 (length in BITS, not bytes) and wait for its done-callback. FIFO/semaphore bookkeeping matches the runtime path. Everything else (capture, cadence, bit-verify) is the shared helper.
    parlio_transmit_config_t xcfg = {};
    xcfg.idle_value = 0;   // lines rest LOW between frames (the latch)
    auto transmitOnce = [st, frameBytes, &xcfg]() {
        // Loopback self-test path (not the render hot path): a failed enqueue or a done-callback timeout would otherwise be silent and just surface later as a capture mismatch, log it so the real cause is visible in the verdict.
        st->fifo[st->fifoHead] = 0;
        st->fifoHead = (st->fifoHead + 1u) & 1u;
        const esp_err_t err = parlio_tx_unit_transmit(st->unit, st->buf[0],
                                                      frameBytes * 8, &xcfg);
        if (err != ESP_OK) {
            st->fifoHead = (st->fifoHead + 1u) & 1u;   // unwind the push
            ESP_LOGE(PAR_TAG, "loopback: tx enqueue failed (%s)", esp_err_to_name(err));
            return;   // nothing to wait for
        }
        if (xSemaphoreTake(st->done[0], pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(PAR_TAG, "loopback: tx done-callback timed out");
    };
    detail::captureAndVerifyFrame(rxGpio, frameBytes, dataBytes, rowBits, kPclkHz, /*pinExpanderMode=*/false,
                                  PAR_TAG, transmitOnce, r);
    destroyState(st);
    return r;
}

}  // namespace mm::platform

#else  // !SOC_PARLIO_SUPPORTED — inert stubs so classic ESP32 / S3 link

namespace mm::platform {

bool parlioWs2812Init(ParlioWs2812Handle&, const uint16_t*, uint8_t, uint32_t, size_t, bool) {
    return false;
}
uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle&, uint8_t) { return nullptr; }
size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle&) { return 0; }
size_t parlioMaxTransferBytes() { return 0; }   // no Parlio here → no bound (the budget-0 contract)
bool parlioWs2812Transmit(ParlioWs2812Handle&, uint8_t, size_t) { return false; }
bool parlioWs2812Wait(ParlioWs2812Handle&, uint8_t, uint32_t) { return true; }
uint32_t parlioWs2812LastTransmitUs(const ParlioWs2812Handle&) { return 0; }
void parlioWs2812Deinit(ParlioWs2812Handle&) {}
RmtLoopbackResult parlioWs2812Loopback(const uint16_t*, uint8_t, uint16_t,
                                       const uint8_t*, size_t, size_t, uint8_t) {
    return {};
}

}  // namespace mm::platform

#endif  // SOC_PARLIO_SUPPORTED
