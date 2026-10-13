/// @defgroup platform_esp32_rmt RMT WS2812 output
/// The peripheral half of the symbol-based LED driver.
///
/// The driver above applies correction and hands down the wire bytes; this file owns the channel, the bit expansion, transmit and wait, and the receive side the loopback test uses.
///
/// @moreinfo
///
/// ## The wire-byte path
///
/// Each byte becomes eight symbols on the way to the peripheral, most significant first, using the two bit shapes the timing call programs.
/// Where the peripheral has its own memory access the SDK's encoder does it; on the classic chip the high-priority refill does it inline.
/// So the caller keeps three or four bytes per light rather than thirty-two bytes for each byte of it.
/// And a long strand no longer outgrows the internal memory the refill is restricted to.
///
/// ## The high-priority refill
///
/// The SDK driver keeps everything about the channel and is bypassed for the transmit itself.
/// Its interrupt runs at a level every critical section masks, and that is what let a refill arrive late.
/// The interrupt source on the second core is rerouted by hand to a high-priority vector, which the allocator refuses as special.
/// And this code plays each frame out of the channel's memory one half-block at a time, straight from the driver's frame buffer.
///
/// It lives at file scope outside every namespace, since the assembly bridge calls the handler by its plain name and the peripheral memory is a linker symbol.
/// Both need external linkage an anonymous namespace would silently remove.
/// Everything the handler touches is in internal memory, so it also runs through a cache-off window, which a flash write opens on both cores.
///
/// ## The channel is created on the second core
///
/// A channel's refill interrupt binds to whichever core creates it, and initialization is reached from the main task, which is pinned to the first core alongside the radio.
/// So the driver ticked on one core while its interrupt lived with the radio on the other.
/// And every radio burst above the interrupt's ceiling delayed a refill past its deadline.
/// A chip without transfer hardware keeps clocking the stale block, so the strand shows a few wrong lights at any light count and any transmit power.
///
/// On the bench, more memory blocks softened it, raising the priority did nothing, and halving the lights changed nothing.
/// The vendor's own maintainer names this exact fix: create the channel on the core the radio is not on.
/// A pinned one-shot task rather than the inter-processor call, whose task has a small stack while channel creation allocates and installs an interrupt.
/// Teardown needs no counterpart, the free hopping to the allocating core itself, and a chip with transfer hardware gains nothing but loses nothing either, so the hop is unconditional.
///
/// ## Re-routing after every channel creation
///
/// Creating a channel routes the source back to the driver's own vector each time, and a config change re-creates the channel.
/// A once-only guard let the second initialization hand the threshold events to the driver's handler, which has no transaction and dereferences nothing.
/// A boot loop some ten seconds in, when the network coming up triggered the second sweep.
///
/// ## A timed-out transfer is left alone
///
/// Cancelling one on the classic chip while it is still active triggers a watchdog panic, which is a worse failure than a dropped frame.
/// It self-heals: the next tick re-encodes and transmits again, and if the channel is still busy the call fails and the driver skips waiting on it.
/// The result still matters, though, because a timeout leaves the frame in flight and re-encoding would rewrite bytes the peripheral is still clocking out.
/// Which shows as a few lights in the wrong color rather than a dropped frame.
///
/// ## Riding a live pipeline means re-arming until a whole frame lands
///
/// Each capture takes ONE run of pulses ending at the next long gap, which is the strand's reset.
/// With a controlled transmit the run starts at the frame start, so one arm yields the whole frame.
/// Riding a free-running pipeline, an arm lands mid-frame and captures only the tail.
/// So it re-arms until a run long enough arrives, meaning one that happened to land at or before a frame start.
/// The pipeline transmits continuously, so a full frame is caught within a few arms, and the budget is bounded so a dead wire still times out rather than looping forever.
/// That budget must fit INSIDE the outer ceiling, or the caller frees the capture buffer while this task is still writing into it.

#include "platform/platform.h"
#include "platform/loopback_verify.h"   // the bit-verify every loopback shares

#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"
#include "driver/gpio.h"   // continuity pre-check in the loopback self-test
#include "soc/soc_caps.h"  // SOC_RMT_MEM_WORDS_PER_CHANNEL (64 classic, 48 S3)
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"   // ets_delay_us for the reset gap

#include "esp_heap_caps.h"   // capture buffer alloc for the shared frame loopback
#include "esp_timer.h"       // timed first transmit
#include "esp_log.h"
#include "esp_cpu.h"
#if CONFIG_IDF_TARGET_ESP32
// The level-5 refill path (rmt_hi_vector.S): the classic ESP32 has no RMT DMA, so the refill interrupt is the whole timing story. These are the pieces that path drives directly.
#include "esp_rom_sys.h"          // esp_rom_route_intr_matrix
#include "esp_memory_utils.h"     // esp_ptr_internal: the ISR may only read internal RAM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"        // vTaskDelay in the polled wait
#include "hal/rmt_ll.h"
#include "soc/rmt_struct.h"       // RMT
#include "soc/gpio_struct.h"      // GPIO.func_out_sel_cfg: which channel a pin got
#include "soc/gpio_sig_map.h"     // RMT_SIG_OUT0_IDX
#include "soc/interrupts.h"       // ETS_RMT_INTR_SOURCE
#include "soc/dport_reg.h"        // the interrupt matrix map registers, read back after routing
#endif

#include <cstdlib>
#include <cstring>
#include <functional>  // the transmit callback the shared frame loopback takes
#include <new>      // std::nothrow

#if CONFIG_IDF_TARGET_ESP32
// The high-priority refill: @xref{the-high-priority-refill|why it bypasses the driver's own interrupt}.
struct RmtHiChannel {
    const uint8_t* cur = nullptr;    // next WIRE BYTE to expand
    const uint8_t* end = nullptr;    // one past the last
    uint32_t sym0 = 0;               // the symbol a 0 bit expands to
    uint32_t sym1 = 0;               // ... and a 1 bit
    uint16_t half = 0;               // symbols per half-block (the threshold)
    uint16_t offset = 0;             // where the next half goes: 0 or `half`
    volatile bool busy = false;      // a frame is on the wire
};
static RmtHiChannel s_hi[RMT_LL_CHANS_PER_INST];

// RMTMEM is a linker-provided address; the IDF types it in a private header, so the same layout is declared here: 8 channels of 64 words, contiguous, which is what lets a channel that owns several blocks be addressed as one run past its own 64.
struct RmtHiMem { struct { volatile uint32_t data32[SOC_RMT_MEM_WORDS_PER_CHANNEL]; } chan[RMT_LL_CHANS_PER_INST]; };
extern "C" RmtHiMem RMTMEM;
extern "C" void ld_include_rmt_hi_vector();   // forces the .S object to link (the vector symbol is weak elsewhere)

// Copy the next half-block for `ch`. Runs at level 5: no RTOS, no logging, no cache-dependent memory. A frame shorter than the remaining half ends with a zero symbol, which the peripheral treats as end-of-transmission and raises TX_DONE on.
static void IRAM_ATTR rmtHiFill(uint8_t ch) {
    RmtHiChannel& c = s_hi[ch];
    volatile uint32_t* dst = &RMTMEM.chan[ch].data32[c.offset];
    uint32_t n = c.half;
    // Expand the wire bytes here rather than copying symbols a caller pre-expanded, which keeps the resident buffer at a few bytes per light instead of thirty-two per byte.
    // That buffer must be internal memory, which a classic chip has too little of for the expanded form.
    // So a long strand fell back to external memory and the transmit refused every frame.
    // The work per half-block is a shift and a select per bit, well inside the deadline.
    const uint32_t s0 = c.sym0, s1 = c.sym1;
    while (n >= 8 && c.cur != c.end) {
        uint8_t data = *c.cur++;
        for (uint8_t bit = 0; bit < 8; bit++) { *dst++ = (data & 0x80u) ? s1 : s0; data = static_cast<uint8_t>(data << 1); }
        n -= 8;
    }
    if (n) *dst = 0;                                   // end marker inside this half
    c.offset = static_cast<uint16_t>(c.offset ? 0 : c.half);
}

// The C half of the level-5 handler. Called from rmt_hi_vector.S with the register file saved and a private stack; must return promptly and must clear what it handles, the interrupt is level-triggered.
extern "C" void IRAM_ATTR rmtHiIsr(void*) {
    const uint32_t st = RMT.int_st.val;
    for (uint8_t ch = 0; ch < RMT_LL_CHANS_PER_INST; ch++) {
        const uint32_t thres = RMT_LL_EVENT_TX_THRES(ch), done = RMT_LL_EVENT_TX_DONE(ch);
        if (st & thres) {
            rmt_ll_clear_interrupt_status(&RMT, thres);
            if (s_hi[ch].busy) rmtHiFill(ch);
        }
        if (st & done) {
            rmt_ll_clear_interrupt_status(&RMT, done);
            s_hi[ch].busy = false;
            rmt_ll_enable_interrupt(&RMT, thres | done, false);
        }
    }
}

// Which peripheral channel the IDF handed this GPIO: the matrix records the output signal, and the RMT signals are consecutive from RMT_SIG_OUT0_IDX. The driver keeps the id private.
static uint8_t rmtHiChannelOf(uint8_t gpio) {
    const uint32_t sig = GPIO.func_out_sel_cfg[gpio].func_sel;
    return (sig >= RMT_SIG_OUT0_IDX && sig < RMT_SIG_OUT0_IDX + RMT_LL_CHANS_PER_INST)
           ? static_cast<uint8_t>(sig - RMT_SIG_OUT0_IDX) : 0xFF;
}

// Route this core's interrupt source to the high-priority vector and enable it, on the second core because the enable register is per core.
// The driver's own handler never fires here again, which is intended, nothing calling its transmit any more.
// Called after EVERY channel creation: @xref{re-routing-after-every-channel-creation|what a once-only guard cost}.
static void rmtHiRouteOnThisCore() {
#if defined(CONFIG_ESP_SYSTEM_CHECK_INT_LEVEL_5) || defined(CONFIG_BTDM_CTRL_HLI)
#error "level 5 is taken on this config (system check or Bluetooth HLI); the RMT refill needs it free"
#endif
    (void)&ld_include_rmt_hi_vector;
    constexpr uint32_t kVector = 26;   // level 5, "special" in the descriptor table, free here
    esp_rom_route_intr_matrix(esp_cpu_get_core_id(), ETS_RMT_INTR_SOURCE, kVector);
    esp_cpu_intr_enable(1u << kVector);
    // Read the routing back: the matrix map for this core's RMT source, and this core's INTENABLE. The IDF's esp_intr_enable re-programs the map (intr_alloc.c), so a later call on the driver's own handle would silently undo this; the readback is what proves it held.
    const uint32_t mapReg = esp_cpu_get_core_id() == 0
        ? DPORT_PRO_RMT_INTR_MAP_REG : DPORT_APP_RMT_INTR_MAP_REG;
    ESP_LOGI("rmt", "level-5 refill: RMT source routed to vector %lu on core %d; map reads %lu, INTENABLE 0x%08lx",
             static_cast<unsigned long>(kVector), static_cast<int>(esp_cpu_get_core_id()),
             static_cast<unsigned long>(DPORT_REG_READ(mapReg)),
             static_cast<unsigned long>(esp_cpu_intr_get_enabled_mask()));
}
#endif  // CONFIG_IDF_TARGET_ESP32

namespace mm::platform {

namespace {

// Per-channel peripheral state, hidden behind RmtWs2812Handle::impl so the domain header never sees an ESP type. One TX channel + the copy encoder it streams symbols through, both allocated once at init.
struct RmtTxState {
    rmt_channel_handle_t channel = nullptr;
    rmt_encoder_handle_t encoder = nullptr;
    uint32_t resolutionHz = 0;
    bool     timed = false;       // rmtWs2812SetBitTiming succeeded: a transmit before it would clock all-zero symbols
    uint32_t sym0 = 0, sym1 = 0;  // bit shapes, set live by rmtWs2812SetBitTiming (every chip:
                                  // the bytes encoder takes them, and so does the level-5 refill)
#if CONFIG_IDF_TARGET_ESP32
    uint8_t  channelId = 0xFF;    // the peripheral channel the IDF gave us, read back from the GPIO matrix
    uint16_t blockSymbols = 0;    // symbols the channel's memory holds (64 per block)
#endif
};


} // namespace

// The channel is created on the second core, which is the whole point of the detour below: @xref{the-channel-is-created-on-the-second-core|the measurement behind it}.
namespace {
struct RmtInitJob {
    RmtTxState* st;
    uint8_t gpio;
    uint32_t resolutionHz;
    bool invert;
    bool ok;
};

void rmtInitOnThisCore(void* arg) {
    auto* job = static_cast<RmtInitJob*>(arg);
    RmtTxState* st = job->st;
    rmt_tx_channel_config_t txCfg = {};
    txCfg.gpio_num = static_cast<gpio_num_t>(job->gpio);
    txCfg.clk_src = RMT_CLK_SRC_DEFAULT;
    txCfg.resolution_hz = job->resolutionHz;
    txCfg.trans_queue_depth = 4;
    txCfg.flags.invert_out = job->invert ? 1 : 0;
    // One memory block per channel at the chip's own size, since a hardcoded one is rejected elsewhere, so every channel stays available to a fully populated board. The block is the refill deadline, and extra blocks only softened a deadline the high-priority refill removes entirely, so they bought nothing and lost pins.
    txCfg.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
    if (rmt_new_tx_channel(&txCfg, &st->channel) != ESP_OK) { job->ok = false; return; }

    // The SDK's own byte encoder, which expands each wire byte as it feeds the peripheral, so the caller keeps only the few bytes per light the correction produces.
    // The copy encoder it replaces made the caller pre-expand every bit first, which is the buffer that outgrew internal memory.
    // The timings here are placeholders, the driver's control being live and rewriting them before each frame.
    rmt_bytes_encoder_config_t bytesCfg = {};
    bytesCfg.flags.msb_first = 1;          // WS2812 clocks the most significant bit first
    if (rmt_new_bytes_encoder(&bytesCfg, &st->encoder) != ESP_OK) {
        rmt_del_channel(st->channel); st->channel = nullptr;
        job->ok = false; return;
    }
    if (rmt_enable(st->channel) != ESP_OK) {
        rmt_del_encoder(st->encoder); st->encoder = nullptr;
        rmt_del_channel(st->channel); st->channel = nullptr;
        job->ok = false; return;
    }
    ESP_LOGI("rmt", "channel on GPIO %u created on core %d (%lu-symbol block)",
             static_cast<unsigned>(job->gpio), static_cast<int>(esp_cpu_get_core_id()),
             static_cast<unsigned long>(txCfg.mem_block_symbols));
#if CONFIG_IDF_TARGET_ESP32
    st->channelId = rmtHiChannelOf(job->gpio);
    st->blockSymbols = static_cast<uint16_t>(txCfg.mem_block_symbols);
    if (st->channelId != 0xFF) {
        rmt_ll_tx_enable_wrap(&RMT, st->channelId, true);      // one global bit on this chip
        rmt_ll_tx_set_limit(&RMT, st->channelId, st->blockSymbols / 2);
        rmtHiRouteOnThisCore();
        ESP_LOGI("rmt", "level-5 refill on channel %u, %u-symbol halves",
                 static_cast<unsigned>(st->channelId), static_cast<unsigned>(st->blockSymbols / 2));
    } else {
        ESP_LOGW("rmt", "could not read the channel back from the GPIO matrix; IDF transmit path");
    }
#endif
    job->ok = true;
}
}  // namespace

bool rmtWs2812Init(RmtWs2812Handle& h, uint8_t gpio, uint32_t resolutionHz, bool invert) {
    auto* st = new (std::nothrow) RmtTxState();
    if (!st) return false;

    RmtInitJob job{st, gpio, resolutionHz, invert, false};
    WorkerTask hop;
    // Priority above the render loop so the one-shot runs at once; 8 KB matches the encode task. If the spawn fails (single core, no memory) the init runs inline on this core, which is the pre-fix behavior rather than no channel at all.
    if (spawnPinnedTask(hop, "mmRmtInit", &rmtInitOnThisCore, &job, 8192, 6, 1)) {
        stopPinnedTask(hop);          // joins: the fn already returned, this only reaps the task
    } else {
        rmtInitOnThisCore(&job);
    }
    if (!job.ok) { delete st; return false; }

    st->resolutionHz = resolutionHz;
    h.impl = st;
    return true;
}

uint32_t rmtWs2812Resolution(const RmtWs2812Handle& h) MM_NONBLOCKING {
    auto* st = static_cast<RmtTxState*>(h.impl);
    return st ? st->resolutionHz : 0;
}

bool rmtWs2812SetBitTiming(RmtWs2812Handle& h, uint32_t sym0, uint32_t sym1) {
    auto* st = static_cast<RmtTxState*>(h.impl);
    if (!st || !st->encoder) return false;
    // The `timing` control is live (400 kHz WS2811, 800 kHz, custom ns), so the encoder's bit shapes are rewritten rather than fixed at init.
    rmt_bytes_encoder_config_t cfg = {};
    static_assert(sizeof(rmt_symbol_word_t) == sizeof(uint32_t), "symbol word is one 32-bit word");
    std::memcpy(&cfg.bit0, &sym0, sizeof(uint32_t));
    std::memcpy(&cfg.bit1, &sym1, sizeof(uint32_t));
    cfg.flags.msb_first = 1;
    // Commit the shapes only once the encoder took them, so a transmit can never run on stale or zero symbols: `timed` is what rmtWs2812Transmit checks.
    if (rmt_bytes_encoder_update_config(st->encoder, &cfg) != ESP_OK) return false;
    st->sym0 = sym0; st->sym1 = sym1; st->timed = true;
    return true;
}

bool rmtWs2812Transmit(RmtWs2812Handle& h, const uint8_t* wire, size_t byteCount) {
    auto* st = static_cast<RmtTxState*>(h.impl);
    if (!st || !wire || byteCount == 0 || !st->timed) return false;   // no bit shapes yet: refuse, not garbage

#if CONFIG_IDF_TARGET_ESP32
    if (st->channelId != 0xFF) {
        // The level-5 path expands the bytes itself (rmtHiFill). Those bytes must be internal RAM: the refill runs with the flash cache possibly off, where a PSRAM read is a fault rather than a stall. At 3-4 bytes per light that is a few KB even for a long strand, so unlike the pre-expanded symbol form this does not outgrow internal RAM: issue #94.
        if (!esp_ptr_internal(wire)) return false;
        RmtHiChannel& c = s_hi[st->channelId];
        if (c.busy) return false;
        const uint8_t ch = st->channelId;
        c.cur = wire; c.end = wire + byteCount;
        c.sym0 = st->sym0; c.sym1 = st->sym1;
        // The expander consumes whole BYTES (8 symbols each), so a half-block that is not a multiple of 8 would leave 1..7 symbols unfilled and end the frame early with no diagnostic. True for every chip today; asserted so a mem_block_symbols change says so.
        static_assert(SOC_RMT_MEM_WORDS_PER_CHANNEL % 16 == 0,
                      "half-block must be a multiple of 8 symbols: rmtHiFill expands whole bytes");
        c.half = st->blockSymbols / 2; c.offset = 0;
        c.busy = true;
        rmt_ll_tx_reset_pointer(&RMT, ch);
        rmt_ll_clear_interrupt_status(&RMT, RMT_LL_EVENT_TX_THRES(ch) | RMT_LL_EVENT_TX_DONE(ch));
        rmtHiFill(ch);                     // both halves primed before the start
        rmtHiFill(ch);
        rmt_ll_enable_interrupt(&RMT, RMT_LL_EVENT_TX_THRES(ch) | RMT_LL_EVENT_TX_DONE(ch), true);
        rmt_ll_tx_start(&RMT, ch);
        return true;
    }
#endif
    rmt_transmit_config_t txCfg = {};
    txCfg.loop_count = 0;   // single shot, no hardware loop

    // The bytes encoder expands each byte to eight symbols as it feeds the peripheral, so the wire bytes go straight out. This only *starts* the transfer: channels started back-to-back clock out concurrently, which is what makes a multi-pin frame cost the longest strand instead of the sum. The caller pairs this with rmtWs2812Wait and owns the inter-frame latch.
    return rmt_transmit(st->channel, st->encoder, wire, byteCount, &txCfg) == ESP_OK;
}

bool rmtWs2812Wait(RmtWs2812Handle& h, uint32_t timeoutMs) {
    auto* st = static_cast<RmtTxState*>(h.impl);
    if (!st) return true;
    // A finite timeout, so a wedged transfer cannot hang the render tick forever; even the longest realistic frame clocks out well inside it. A timed-out transfer is deliberately left alone rather than canceled: @xref{a-timed-out-transfer-is-left-alone|why, and what the result costs}.
#if CONFIG_IDF_TARGET_ESP32
    if (st->channelId != 0xFF) {
        // TX_DONE clears `busy` from the level-5 handler. Polled with a yield, not a semaphore: the handler runs where no RTOS call is allowed, so it cannot signal one.
        const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeoutMs) * 1000;
        // Spin first and yield only if the frame is genuinely long, since the shortest sleep is a whole scheduler tick.
        // A frame clocking out in a fraction of a millisecond still cost the full tick, measured flat on the bench whatever the light or lane count.
        // That is the scheduler rather than the wire, and a frame here is bounded and short.
        // So busy-waiting to about one tick keeps the processor for the case that is over in microseconds, while still yielding on a long strand.
        const int64_t spinUntil = esp_timer_get_time() + 10000;   // ~1 scheduler tick
        while (s_hi[st->channelId].busy) {
            const int64_t now = esp_timer_get_time();
            if (now > deadline) return false;
            if (now > spinUntil) vTaskDelay(1);   // long frame: hand the core back
        }
        return true;
    }
#endif
    return rmt_tx_wait_all_done(st->channel, timeoutMs) == ESP_OK;
}

void rmtWs2812Deinit(RmtWs2812Handle& h) {
    auto* st = static_cast<RmtTxState*>(h.impl);
    if (!st) return;
#if CONFIG_IDF_TARGET_ESP32
    if (st->channelId != 0xFF) {
        rmt_ll_enable_interrupt(&RMT, RMT_LL_EVENT_TX_THRES(st->channelId) | RMT_LL_EVENT_TX_DONE(st->channelId), false);
        s_hi[st->channelId].busy = false;
    }
#endif
    if (st->channel) {
        rmt_disable(st->channel);
        rmt_del_channel(st->channel);
    }
    if (st->encoder) rmt_del_encoder(st->encoder);
    delete st;
    h.impl = nullptr;
}

// Loopback capture, an on-device test only: a one-shot receive channel on the jumpered pin, returning how many symbols landed for the test to decode and compare.

namespace {

// done-callback hands the received symbol count to the waiting capture call via a 1-deep queue. IRAM so it survives a cache-disabled window.
struct RxDone { size_t numSymbols; };

bool IRAM_ATTR rmtRxDoneCb(rmt_channel_handle_t, const rmt_rx_done_event_data_t* edata,
                           void* user) {
    QueueHandle_t q = static_cast<QueueHandle_t>(user);
    RxDone d = { edata->num_symbols };
    BaseType_t high = pdFALSE;
    xQueueSendFromISR(q, &d, &high);
    return high == pdTRUE;
}

} // namespace

size_t rmtWs2812RxCapture(uint8_t gpio, uint32_t resolutionHz,
                          uint32_t* outSymbols, size_t maxSymbols, uint32_t timeoutMs) {
    if (!outSymbols || maxSymbols == 0) return 0;

    rmt_rx_channel_config_t rxCfg = {};
    rxCfg.gpio_num = static_cast<gpio_num_t>(gpio);
    rxCfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rxCfg.resolution_hz = resolutionHz;
    // The receive channel's block must be even and at least one hardware block, whose size differs per chip.
    // So a hardcoded one would silently claim part of a second channel's memory.
    // The capture buffer itself is separate and may be smaller.
    size_t memBlock = static_cast<size_t>(maxSymbols);
    if (memBlock < SOC_RMT_MEM_WORDS_PER_CHANNEL) memBlock = SOC_RMT_MEM_WORDS_PER_CHANNEL;
    if (memBlock & 1) memBlock++;
#if SOC_RMT_SUPPORT_DMA
    // A capture larger than one hardware block (whole-frame captures, e.g. the LCD loopback's full-frame check) uses the DMA backend, which can stream an arbitrarily large mem_block. Caller's buffer must then be DMA-capable internal RAM.
    rxCfg.flags.with_dma = maxSymbols > SOC_RMT_MEM_WORDS_PER_CHANNEL;
#else
    // Without transfer hardware, asking for more than one channel's memory silently claims a neighbor's and then fails to allocate, so this caps at a single channel. A whole-frame check on such a chip must therefore use a frame that fits one, which the frame loopback sizes itself to.
    if (memBlock > SOC_RMT_MEM_WORDS_PER_CHANNEL)
        memBlock = SOC_RMT_MEM_WORDS_PER_CHANNEL;
#endif
    rxCfg.mem_block_symbols = memBlock;

    rmt_channel_handle_t rxChan = nullptr;
    if (rmt_new_rx_channel(&rxCfg, &rxChan) != ESP_OK) return 0;

    QueueHandle_t q = xQueueCreate(1, sizeof(RxDone));
    if (!q) { rmt_del_channel(rxChan); return 0; }

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = rmtRxDoneCb;
    rmt_rx_register_event_callbacks(rxChan, &cbs, q);

    // Accept WS2812 pulse widths: anything from a fraction of T0H up to well past a bit cell, so glitches are filtered but real 0/1 pulses pass.
    rmt_receive_config_t rcfg = {};
    rcfg.signal_range_min_ns = 100;       // shorter than any real WS2812 edge
    rcfg.signal_range_max_ns = 100000;    // longer than a bit cell; ends the frame

    size_t got = 0;
    if (rmt_enable(rxChan) == ESP_OK) {
        // Once enabled, the channel must be disabled before delete, even if rmt_receive or the wait fails, or rmt_del_channel rejects it.
        if (rmt_receive(rxChan, outSymbols, maxSymbols * sizeof(uint32_t), &rcfg) == ESP_OK) {
            RxDone d = {};
            if (xQueueReceive(q, &d, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
                got = d.numSymbols;
            }
        }
        rmt_disable(rxChan);
    }

    vQueueDelete(q);
    rmt_del_channel(rxChan);
    return got;
}


// The loopback self-test, runnable from the live firmware: send a known pattern, capture it back on the jumpered pin, decode and compare. The symbol build is inlined here, being two shapes, so this layer stays self-contained and the domain keeps no dependency on it.

namespace {

constexpr uint32_t kLoopbackResHz = 40'000'000;  // 25 ns/tick, same as the driver
constexpr uint16_t kT0H = 14, kT1H = 28, kPeriod = 50;  // 350/700/1250 ns in ticks

} // namespace

namespace detail {

// Plain-GPIO continuity check: drive tx, read rx. Separates "wire wrong" from "RMT/LCD wrong" so a failed jumper is reported clearly. Shared with the LCD loopback in platform_esp32_i80.cpp (declared there), hence not anonymous.
bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio) {
    gpio_set_direction(static_cast<gpio_num_t>(txGpio), GPIO_MODE_OUTPUT);
    gpio_set_direction(static_cast<gpio_num_t>(rxGpio), GPIO_MODE_INPUT);
    gpio_set_pull_mode(static_cast<gpio_num_t>(rxGpio), GPIO_PULLDOWN_ONLY);
    gpio_set_level(static_cast<gpio_num_t>(txGpio), 1);
    ets_delay_us(2000);
    int hi = gpio_get_level(static_cast<gpio_num_t>(rxGpio));
    gpio_set_level(static_cast<gpio_num_t>(txGpio), 0);
    ets_delay_us(2000);
    int lo = gpio_get_level(static_cast<gpio_num_t>(rxGpio));
    gpio_reset_pin(static_cast<gpio_num_t>(txGpio));
    gpio_reset_pin(static_cast<gpio_num_t>(rxGpio));
    // Log the raw levels, this one line is a genuine bench HAL diagnostic (it pinned the MHC-WLED P4 shield loopback: hi=0 lo=0 = no signal path, hi=1 lo=1 = the Rx pin is externally pulled up, hi=1 lo=0 = clean). Only runs when the loopback self-test is invoked (off the hot path), so it costs nothing in normal operation.
    ESP_LOGI("mm_loopback", "continuity tx=%u->rx=%u: hi=%d lo=%d (want hi=1 lo=0)",
             txGpio, rxGpio, hi, lo);
    return hi == 1 && lo == 0;
}

// The shared capture and bit-verify for both parallel loopbacks, which differ only in their transmit call and private state type while everything else was identical.
// The caller has already done the jumper pre-check and built its bus, and passes a transmit-and-wait callback plus what is needed to size the capture.
// The capture buffer is one symbol per bit plus slack, aligned and internal: the single biggest contiguous block needed.
// Which is why a caller may allocate it first and hand it in.
uint32_t* allocLoopbackCapture(size_t dataBytes) {
    const size_t capMax = dataBytes / 3 + 16;
    return static_cast<uint32_t*>(heap_caps_aligned_alloc(
        64, capMax * sizeof(uint32_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
}

void captureAndVerifyFrame(uint16_t rxGpio, size_t frameBytes, size_t dataBytes,
                           uint8_t rowBits, uint32_t pclkHz, bool pinExpanderMode, const char* tag,
                           const std::function<void()>& transmitOnce,
                           RmtLoopbackResult& r, bool rideMode, uint32_t* rxSymbols) {
    // The decode threshold is derived from the strand's slot rate rather than fixed.
    // A zero is high for one slot and a one for two, so the midpoint separates them at any rate.
    // A hardcoded threshold sat above the expander's one-bit and decoded every one as zero, and the verdict then blamed the transport for a decode fault.
    constexpr uint32_t kCapResHz = 40'000'000;
    const uint16_t slotTicks = static_cast<uint16_t>(kCapResHz / pclkHz);
    const uint16_t threshTicks = static_cast<uint16_t>(slotTicks + slotTicks / 2);
    const size_t kBits = dataBytes / 3;
    const size_t capMax = kBits + 16;
    if (!rxSymbols) rxSymbols = allocLoopbackCapture(dataBytes);   // caller didn't pre-allocate
    if (!rxSymbols) {
        ESP_LOGE(tag, "loopback: capture buffer alloc failed (%u B)",
                 (unsigned)(capMax * sizeof(uint32_t)));
        return;
    }

    struct Cap {
        uint8_t rxGpio; uint32_t* buf; size_t max; size_t need;
        bool ride; volatile size_t got = 0; volatile bool done = false;
    };
    // HEAP, not stack: the rx task holds this pointer, and the wedged-task exit below returns while the task may still be running, a stack Cap would then be a use-after-return. Heap lets that path leak the context alongside rxSymbols (the deliberate failure mode) instead of dangling it.
    auto* cap = new (std::nothrow) Cap{static_cast<uint8_t>(rxGpio), rxSymbols, capMax, kBits, rideMode};
    if (!cap) { heap_caps_free(rxSymbols); return; }
    // Re-arm until a whole frame lands: @xref{riding-a-live-pipeline-means-re-arming-until-a-whole-frame-lands|why one arm is not enough here}.
    auto rxTask = [](void* arg) {
        auto* c = static_cast<Cap*>(arg);
        // The retry budget fits inside the outer ceiling below, or the caller frees the buffer while this task is still writing into it. A live pipeline delivers a frame within milliseconds, so the per-arm wait is already generous slack and a dead wire exhausts the budget in bounded time.
        const int attempts = c->ride ? 40 : 1;
        const uint32_t perArmTimeoutMs = c->ride ? 100 : 1000;
        for (int a = 0; a < attempts; a++) {
            const size_t g = rmtWs2812RxCapture(c->rxGpio, kCapResHz, c->buf, c->max, perArmTimeoutMs);
            if (g > c->got) c->got = g;           // keep the fullest capture seen
            if (g >= c->need) break;              // a complete frame — stop
        }
        c->done = true;
        vTaskDelete(nullptr);
    };
    const bool taskStarted = xTaskCreate(rxTask, "lblb", 4096, cap, 5, nullptr) == pdPASS;
    if (taskStarted) {
        vTaskDelay(pdMS_TO_TICKS(50));
        // First transmit timed, the wall time of a known byte count confirms the granted pixel clock matches the configured slot rate (the bus driver doesn't expose the granted clock directly).
        {
            const int64_t t0 = esp_timer_get_time();
            transmitOnce();
            const int64_t dt = esp_timer_get_time() - t0;
            r.txWallUs = static_cast<uint32_t>(dt);
            // Expected wire time from the STRAND's view (unit-safe at any bus width / fan-out): kBits WS2812 bits × 3 slots each ÷ the slot rate. frameBytes ÷ pclkHz would mix units, frameBytes counts BUS bytes while pclkHz here is the slot rate, which overstates the expectation 8× in shift mode.
            r.txExpectUs = static_cast<uint32_t>(kBits * 3ull * 1000000ull / pclkHz);
            ESP_LOGI(tag, "loopback: %u bytes in %lld us (expect ~%u us at %u Hz slot rate)",
                     (unsigned)frameBytes, (long long)dt,
                     (unsigned)r.txExpectUs, (unsigned)pclkHz);
        }
        // Back-to-back frames, exactly the render loop's transmit/wait cadence.
        for (int i = 0; i < 100 && !cap->done; i++) transmitOnce();
        // Wait for the capture task. Ride mode re-arms internally (each arm returns in ~1 frame when the pipeline is live), so give it a longer ceiling than the controlled-transmit path, a live frame is caught in well under this, and a dead wire still ends when the task exhausts its bounded retries.
        const int waitTicks = rideMode ? 600 : 200;   // ×10 ms = 6 s (ride) / 2 s (controlled)
        for (int i = 0; i < waitTicks && !cap->done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    r.capturedSymbols = static_cast<uint32_t>(cap->got);
    r.rxIdleLevel = static_cast<int8_t>(gpio_get_level(static_cast<gpio_num_t>(rxGpio)));
    ESP_LOGI(tag, "loopback: rx captured %u symbols (need %u), idle rx level=%d",
             (unsigned)cap->got, (unsigned)kBits, (int)r.rxIdleLevel);

    if (cap->done && cap->got >= kBits) {
        // Verify EVERY bit of the frame against the per-row pattern (r.sent[], zero-padded for RGBW rows), not just the first light.
        const LoopbackBitStats st = verifyLoopbackBits(rxSymbols, kBits, cap->got, rowBits, threshTicks,
                                                       pinExpanderMode, r);
        ESP_LOGI(tag, "loopback: high ticks — 0-bits %u..%u, 1-bits %u..%u (25ns/tick, threshold %u)",
                 (unsigned)st.minHigh[0], (unsigned)st.maxHigh[0], (unsigned)st.minHigh[1], (unsigned)st.maxHigh[1],
                 (unsigned)threshTicks);
        if (!r.pass) {
            ESP_LOGE(tag, "loopback: first bad bit %u (light %u, bit-in-row %u), %u/%u bits bad; row0 got %02x%02x%02x exp %02x%02x%02x",
                     (unsigned)st.mismatch, (unsigned)(st.mismatch / rowBits),
                     (unsigned)(st.mismatch % rowBits), (unsigned)st.mismatchCount, (unsigned)kBits,
                     r.got[0], r.got[1], r.got[2], r.sent[0], r.sent[1], r.sent[2]);
        }
    }
    // NEVER free what the rx task may still touch, the capture buffer it writes AND the Cap context it reads. The retry budget is sized under the wait ceiling above, so cap->done is normally long set by here; this drains the residue if the scheduler starved the task. If it STILL hasn't finished (an RMT-driver wedge), leaking both is the correct failure, a use-after-free under a running task is not.
    for (int i = 0; taskStarted && !cap->done && i < 500; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (taskStarted && !cap->done) {
        ESP_LOGE(tag, "loopback: rx task never finished — leaking the capture buffer instead of freeing under it");
        return;   // rxSymbols and cap both stay allocated, deliberately
    }
    heap_caps_free(rxSymbols);
    delete cap;
}

} // namespace detail

// The intrusive loopback, driver-agnostic: no bus and no transmit of our own.
// The live pipeline already clocking frames past the receive pin, so this only arms the capture and lets one land.
// It shares the capture and verify helper with every family's own loopback, so the decode and verdict are identical whatever drove the wire.
// And it costs no extra memory, which is why it exists beside the private-bus one.
RmtLoopbackResult ws2812LoopbackRide(uint16_t rxGpio, const uint8_t* sent, uint8_t sentLen,
                                     size_t dataBytes, uint8_t rowBits, uint8_t clockMultiplier) {
    RmtLoopbackResult r;
    if (!sent || sentLen == 0 || sentLen > 3 || dataBytes < 3 || rowBits < 8 || clockMultiplier == 0)
        return r;
    for (uint8_t i = 0; i < sentLen; i++) r.sent[i] = sent[i];   // the per-light pattern to verify
    r.jumperDetected = true;   // proven by the bit-verify itself, not a plain-GPIO continuity pre-check
    // The STRAND's slot rate (what the RX sees), from the WS2812 physical timing every family shares: direct slots at kSlotHz; an expander fits `clockMultiplier` bus words per slot, so the slot rate is the fast bus clock ÷ multiplier. Same values the per-family loopbacks derive from their own kPclkHz/kShiftPclkHz, canonical WS2812 timing, so the driver-agnostic ride carries them here rather than taking a family's.
    constexpr uint32_t kSlotHz = 2'666'666;         // direct-mode WS2812 slot (375 ns)
    constexpr uint32_t kShiftBusHz = 26'666'666;    // expander bus clock (300 ns slot at ÷8)
    const bool pinExpanderMode = clockMultiplier > 1;
    const uint32_t slotHz = pinExpanderMode ? (kShiftBusHz / clockMultiplier) : kSlotHz;
    auto noTransmit = []() {};  // the render loop is the transmitter
    detail::captureAndVerifyFrame(rxGpio, dataBytes, dataBytes, rowBits, slotHz, pinExpanderMode,
                                  "ws2812-ride", noTransmit, r, /*rideMode=*/true, /*rxSymbols=*/nullptr);
    return r;
}

RmtLoopbackResult rmtWs2812Loopback(uint8_t txGpio, uint8_t rxGpio) {
    RmtLoopbackResult r = detail::loopbackResult();

    r.jumperDetected = detail::loopbackJumperOk(txGpio, rxGpio);
    if (!r.jumperDetected) return r;   // no point running RMT through a dead wire

    // Build 24 symbols (3 bytes × 8 bits, MSB-first) for the pattern.
    const uint32_t sym0 = static_cast<uint32_t>(kT0H) | (1u << 15)
                        | (static_cast<uint32_t>(kPeriod - kT0H) << 16);
    const uint32_t sym1 = static_cast<uint32_t>(kT1H) | (1u << 15)
                        | (static_cast<uint32_t>(kPeriod - kT1H) << 16);
    constexpr size_t kBits = 24;
    const uint8_t txWire[3] = { r.sent[0], r.sent[1], r.sent[2] };

    RmtWs2812Handle tx;
    if (!rmtWs2812Init(tx, txGpio, kLoopbackResHz, /*invert=*/false)) return r;
    rmtWs2812SetBitTiming(tx, sym0, sym1);

    // RX must be listening while we transmit; run the (blocking) capture in a task and resend the short frame until the receiver latches one or we give up.
    constexpr size_t kCapMax = kBits + 8;
    static uint32_t rxSymbols[kCapMax];
    // Pass rxGpio through the arg struct (the task fn is a plain C pointer, no captures).
    struct Cap { uint8_t rxGpio; volatile size_t got = 0; volatile bool done = false; } cap{rxGpio};
    auto rxTask = [](void* arg) {
        auto* c = static_cast<Cap*>(arg);
        c->got = rmtWs2812RxCapture(c->rxGpio, kLoopbackResHz, rxSymbols, kCapMax, 1000);
        c->done = true;
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(rxTask, "rmtlb", 4096, &cap, 5, nullptr) == pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        for (int i = 0; i < 50 && !cap.done; i++) {
            rmtWs2812Transmit(tx, txWire, sizeof(txWire));
            rmtWs2812Wait(tx, 1000);
            ets_delay_us(300);   // inter-frame latch
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        for (int i = 0; i < 200 && !cap.done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    rmtWs2812Deinit(tx);

    if (cap.done && cap.got >= kBits) {
        // Decode the first 24 captured symbols → bytes (HIGH closer to T1H = 1).
        for (size_t b = 0; b < kBits; b++) {
            uint16_t high = static_cast<uint16_t>(rxSymbols[b] & 0x7FFF);
            uint8_t bit = (high >= ((kT0H + kT1H) / 2)) ? 1 : 0;
            r.got[b / 8] = static_cast<uint8_t>((r.got[b / 8] << 1) | bit);
        }
        r.pass = (r.got[0] == r.sent[0] && r.got[1] == r.sent[1] && r.got[2] == r.sent[2]);
    }
    r.bitsChecked = static_cast<uint32_t>(kBits);
    r.firstBadBit = r.pass ? static_cast<uint32_t>(kBits) : 0;
    return r;
}

// The whole-frame variant: transmit a real frame back to back and verify the whole capture, one fixed pattern repeated for every light and padded for a fourth channel.
// Unlike the short burst above, this drives the sustained transfer path and a long wire under whatever the radio is doing.
// So it catches the frame-rate corruption and interference the short test is blind to.
RmtLoopbackResult rmtWs2812LoopbackFrame(uint8_t txGpio, uint8_t rxGpio,
                                         uint16_t lights, uint8_t channels) {
    RmtLoopbackResult r = detail::loopbackResult();
    if (lights == 0 || channels < 3 || channels > 4) return r;

    r.jumperDetected = detail::loopbackJumperOk(txGpio, rxGpio);
    if (!r.jumperDetected) return r;

    const uint32_t sym0 = static_cast<uint32_t>(kT0H) | (1u << 15)
                        | (static_cast<uint32_t>(kPeriod - kT0H) << 16);
    const uint32_t sym1 = static_cast<uint32_t>(kT1H) | (1u << 15)
                        | (static_cast<uint32_t>(kPeriod - kT1H) << 16);
    const uint8_t bitsPerLight = static_cast<uint8_t>(channels * 8);
#if !SOC_RMT_SUPPORT_DMA
    // Without transfer hardware the capture holds at most one channel's symbols, so the verified frame is capped to whole lights within that block. The frame is still transmitted back to back, which is the stress that exposes interference; only the verified part is a prefix.
    const uint16_t maxLights =
        static_cast<uint16_t>(SOC_RMT_MEM_WORDS_PER_CHANNEL / bitsPerLight);
    if (lights > maxLights) lights = maxLights ? maxLights : 1;
#endif
    const size_t kBits = static_cast<size_t>(lights) * bitsPerLight;

    // One real frame's worth of WIRE BYTES, DMA-capable internal RAM (the same place the driver's own frame buffer lives). Off the hot path: a control-driven self-test.
    const size_t txBytes = static_cast<size_t>(lights) * channels;
    auto* txWire = static_cast<uint8_t*>(heap_caps_malloc(
        txBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    const size_t capMax = kBits + 16;
    auto* rxSymbols = static_cast<uint32_t*>(heap_caps_aligned_alloc(
        64, capMax * sizeof(uint32_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!txWire || !rxSymbols) {
        heap_caps_free(txWire);
        heap_caps_free(rxSymbols);
        return r;
    }
    size_t s = 0;
    for (uint16_t light = 0; light < lights; light++)
        for (uint8_t ch = 0; ch < channels; ch++)
            txWire[s++] = ch < 3 ? r.sent[ch] : 0x00;

    RmtWs2812Handle tx;
    if (!rmtWs2812Init(tx, txGpio, kLoopbackResHz, /*invert=*/false)) {
        heap_caps_free(txWire);
        heap_caps_free(rxSymbols);
        return r;
    }
    rmtWs2812SetBitTiming(tx, sym0, sym1);

    struct Cap {
        uint8_t rxGpio; uint32_t* buf; size_t max;
        volatile size_t got = 0; volatile bool done = false;
    } cap{rxGpio, rxSymbols, capMax};
    auto rxTask = [](void* arg) {
        auto* c = static_cast<Cap*>(arg);
        c->got = rmtWs2812RxCapture(c->rxGpio, kLoopbackResHz, c->buf, c->max, 1000);
        c->done = true;
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(rxTask, "rmtlbf", 4096, &cap, 5, nullptr) == pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        // Back-to-back frames, the render loop's cadence. The capture latches one whole frame; we keep resending so it can't miss the window.
        for (int i = 0; i < 100 && !cap.done; i++) {
            rmtWs2812Transmit(tx, txWire, txBytes);
            rmtWs2812Wait(tx, 1000);
            ets_delay_us(300);   // inter-frame WS2812 latch
        }
        for (int i = 0; i < 200 && !cap.done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    rmtWs2812Deinit(tx);

    if (cap.done && cap.got >= kBits) {
        size_t mismatch = SIZE_MAX;
        for (size_t b = 0; b < kBits; b++) {
            const uint16_t high = static_cast<uint16_t>(rxSymbols[b] & 0x7FFF);
            const uint8_t bit = (high >= ((kT0H + kT1H) / 2)) ? 1 : 0;
            const uint8_t pos = static_cast<uint8_t>(b % bitsPerLight);
            const uint8_t expByte = (pos / 8u) < 3 ? r.sent[pos / 8u] : 0x00;
            const uint8_t exp = (expByte >> (7 - (pos & 7))) & 1u;
            if (bit != exp && mismatch == SIZE_MAX) mismatch = b;
        }
        r.pass = (mismatch == SIZE_MAX);
        r.bitsChecked = static_cast<uint32_t>(kBits);
        r.firstBadBit = (mismatch == SIZE_MAX) ? static_cast<uint32_t>(kBits)
                                               : static_cast<uint32_t>(mismatch);
        // got[] = the light holding the first mismatch (light 0 when clean).
        const size_t badLight = (mismatch == SIZE_MAX) ? 0 : mismatch / bitsPerLight;
        const size_t lightStart = badLight * bitsPerLight;
        for (size_t b = 0; b < 24 && lightStart + b < cap.got; b++) {
            const uint8_t bit = ((rxSymbols[lightStart + b] & 0x7FFF)
                                 >= ((kT0H + kT1H) / 2)) ? 1 : 0;
            r.got[b / 8] = static_cast<uint8_t>((r.got[b / 8] << 1) | bit);
        }
    }
    heap_caps_free(txWire);
    heap_caps_free(rxSymbols);
    return r;
}

} // namespace mm::platform
