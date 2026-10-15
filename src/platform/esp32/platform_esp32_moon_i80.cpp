/// Parallel WS2812 output over the S3 and P4 LCD_CAM i80 peripheral, sequenced by our own DMA.
///
/// This file owns the peripheral alone: the registers, the GDMA channel and its descriptor chain, the frame buffers, transmit and wait.
/// The driver above it does the domain work, and the esp_lcd sibling beside it is the behavioral reference this matches function for function.
///
/// @technical
///
/// ## Why this exists next to the esp_lcd backend
///
/// That component re-arms the peripheral on every transaction, resetting the bus and busy-waiting before each one.
/// An LCD panel does not care, but WS2812 is one unbroken self-clocked bit stream, so a mid-frame reset garbles everything after it.
/// A frame split across several transactions therefore cannot be sent gaplessly at any chunk size, which forces the whole frame into one.
/// That is what caps the sibling: the DMA must stream an entire frame from one contiguous reachable block.
///
/// ## The hardware never demanded it
///
/// The peripheral has no data-length register, and the SDK's own comment says the data phase is controlled by the buffer length.
/// So it clocks out exactly what the DMA feeds it and stops when the chain ends.
/// One start over an arbitrarily long descriptor chain is a single gapless stream across as many buffers as we like.
/// This backend takes that, built on the SDK's own hardware and link-list interfaces rather than raw registers, which keeps it a recognizable construct.
///
/// ## Both implementations ship
///
/// The esp_lcd one is the reference and this one is the measured alternative, selected as a module swap in the interface.
/// The guard is the narrow capability macro rather than the sibling's broad one, since this pokes registers the classic chip does not have.
/// Its i80 is a different peripheral with a different register file entirely, so the stubs are inert there and the build list stays unconditional.
///
/// ## The shift clock window
///
/// What constrains the clock is the WS2812 slot duration rather than the elegance of the divider, the slot being the zero pulse.
/// An expander shifts each slot out over several bus words, so the slot is that multiplier divided by the pixel clock.
/// The specified zero pulse runs to 380 ns on newer revisions and the one pulse from 580 to 1000 ns.
/// Three settings were taken to the wall, and the default is the middle one.
///
/// ```
/// div 3  26.67 MHz  zero 300 ns, bit 900 ns   strict spec, but marginal strands scramble
/// div 4  20.00 MHz  zero 400 ns, bit 1200 ns  the reliability point, verified on two rigs
/// div 5  16.00 MHz  zero 500 ns               crosses the threshold: every bit reads one, all white
/// ```
///
/// Div 3 is the overclock for short-wired rigs chasing the higher ceiling, measured at 151 frames against 118.
/// Long runs under capacitive load cannot track it, and raising the drive strength does not help, both measured: it is bandwidth-bound rather than edge-bound.
/// Div 4 gives the expander a third more shift margin, which is why hpwit ships his own driver near that rate.
/// Calibration is per wall: sweep up from the default until marginal strands are clean, and all white means one step too far.
///
/// ## The ring's geometry is runtime
///
/// The lights per buffer and the pool depth arrive as parameters, because the optimum is a measurement rather than a derivation.
/// Memory is the only axis wanting a small buffer, and three others want it big.
///
/// ```
/// memory          lights x depth x row bytes; flat only at one light per buffer
/// per-call cost   the encode seam's fixed overhead, amortized over the buffer
/// interrupt rate  one per buffer: 25.6k/s at one light, 1.6k/s at sixteen
/// lap-time runway how long a preemption may last before the DMA laps a refilling buffer
/// ```
///
/// A busy interrupt on the first core starves the network stack, measured as a 19 ms encode killing ethernet on the LC16.
/// Bench history worth keeping: at sixteen lights per buffer the pool only ever held about twelve, which caps that geometry near 240 lights per strand.
/// A depth of two broke transport outright, the loopback failing at the first bit, because our chain runs without the owner gate that scheme relies on.
///
/// ## The scatter above eight slices, resolved
///
/// It read as an unknown mechanism at the time and was two knowable per-hardware faults the ring counters are structurally blind to.
/// The inter-slice pad latched the strand, a LOW gap over the reset threshold repainting the first lights of every slice.
/// And the producer needed headroom over the consumer, which the near-prime pool provides.
/// With the automatic geometry and a latch-safe pad, the full wall streams clean, verified.
///
/// The lasting lesson is that without the owner check there is no handshake, so a torn or short read raises no error at all.
/// Timings look healthy and the error count stays zero, so a correct geometry drawn in scattered dots reads as a latch fault rather than a data fault.
/// Do not trust a ring counter for that class: the wall, or the loopback bit-verify, is the instrument.
///
/// ## Where the frame lives is decided by the pixel clock
///
/// Measured on one board with one variable changed, which is the cleanest result of the whole investigation.
/// In direct mode a frame in external memory streams fine; in expander mode it never completes, at any size.
/// So it is neither the memory nor the frame size: the transfer engine cannot sustain an external read at the expander's clock.
/// An expander is serial, so each slot shifts out over eight bus words and the bus runs ten times faster, which is exactly the rate that memory cannot feed.
///
/// This backend is what proved it, and is why it was built.
/// The sibling failed here with thousands of descriptor-mount errors that pointed hard at its own descriptor handling.
/// This one removes that mechanism entirely, owning the chain and mounting it once, and the mount errors are gone while the transfer still never completes.
/// So that storm was a symptom rather than the cause, killed by a controlled experiment with a working control condition.
///
/// Hence internal memory first in expander mode and external first otherwise, which is what the measurement says rather than a workaround inherited from the sibling.
/// External stays the fallback there, so a frame too big to fit still drives badly rather than refusing to start.
///
/// ## The interrupt does the encode, and must never touch flash
///
/// The handler runs in the transfer interrupt and its ring branch calls the domain encode, so the whole chain is resident in instruction memory.
/// The channel is registered cache-safe, a deliberate shipped hardening: the interrupt fires at the wire rate, so a cache miss inside it would blow the refill deadline.
/// Being cache-safe, it may fire while the flash cache is disabled by a write, so the slice encode must not touch anything flash-resident.
/// The refill defers when the cache is off and the batch catches up afterwards.
/// Do not remove that guard or move the encode back to flash: either reintroduces the measured cache-error panic.
///
/// ## Why completion is reported on written, not drained
///
/// The hardware ends the frame, the terminator sitting on the final slice, so there is no software stop and no post-frame interrupt to catch.
/// Completion is therefore reported the instant the last slice is written.
/// From there the frame's fate is sealed: the engine drains the rest at the exact wire rate and stops itself.
/// That dodges the coalescing trap which wedged the older drain-keyed stop, where two late interrupts latch into one and the firing that would have observed the end never arrives.
/// A drain-gated completion then deadlocks, the halted engine firing nothing further, measured as flicker and then no output.
///
/// A stall-truncated frame cannot be reported as a clean completion: reaching that branch means an interrupt fired, and one fires only while the engine runs.
/// A stall long enough to truncate is one where the engine halted at the frontier.
/// The handler never runs again for that frame, so its completion comes from the backstop instead.
///
/// ## Only the data lines reach a pad
///
/// The routing fabric is not a broadcast: a peripheral signal never connected to a pad stays inside the peripheral.
/// So the lanes a board does not use, and both bus control lines, cost no pins at all.
/// The peripheral clocks every lane whatever the pin count, but those past the board's count go nowhere.
/// Which the sibling component cannot do: it rejects an unconnected data pin and must park spares on a real ghost pin.
///
/// The data-command line separates command from data on a panel, a concept a strand does not have, and it is nailed to a constant level in every phase.
/// The write strobe must still be generated, being the clock that shifts each bus word out and drives an expander, but only a shift register consumes it.
/// A strand is self-clocked, so in direct mode it ignores the strobe entirely and that needs no pad either.
/// A direct-mode board therefore spends its pins on strands alone, which is the budget the hand-rolled drivers have always had.
///
/// ## The chain is ours, so the owner check protects nothing
///
/// The sibling component leaves owner-checking on, which makes a mount walk from the start and refuse at the first descriptor the engine still owns.
/// That is the mount failure which makes its path unusable with a large expanded frame.
/// We own the chain outright and rebuild it from scratch on every transmit, so the check protects nothing and only fails.
///
/// ## The frontier-terminated chain
///
/// The pool is a fixed set of node runs, one per buffer, mounted linear with a spare tail the arm re-links, and it is not a loop.
/// The chain always ends at a terminator sitting on the last written slice: the arm plants it at the prime edge and the interrupt advances it as it refills.
/// So the engine can never run past written data. Reaching the end of the encoded slices it halts there, lines idle low, and the strand latches a partially-updated frame.
///
/// That replaces the earlier looping chain, where a deferred refill let the engine lap the pool and re-clock stale slices as bright garbage.
/// It is also why the original linear chain stalled beyond a certain length: it revisited buffers with no live terminator to advance.
/// So the engine reached a node it could not pass and halted mid-frame.
/// A moving frontier is the fix that both a fixed terminator and a bare loop lacked.
///
/// Every node carries rows and nothing else, since the bit stream must flow continuously from buffer to buffer.
/// A trailing low pad on any mid-chain buffer is a gap long enough to latch the strand, which is the scrambled-image fault.
/// The reset gap is not in a buffer at all: it is the idle time after the halt, held by the next arm.
///
/// ## One buffer is one descriptor node
///
/// A buffer larger than a node's maximum spans several, and the per-buffer mount and re-link interaction then breaks the self-terminating end.
/// On the bench the terminator sat correctly on its node yet the engine stopped several nodes earlier, while the identical logic was clean at one node per buffer.
/// Clamping the buffer to fit one node deletes that class of bug instead of patching it, and small buffers are the direction the large walls want anyway.
///
/// ## Descriptor write-back stays off
///
/// With it on, the engine clears each descriptor's owner bit as it consumes the node.
/// On a chain whose buffers are refilled behind it, that leaves the engine gating on bits it cleared.
/// It then halts politely once it reaches a node it now thinks the processor owns.
/// That is the measured symptom of a clean stop after a number of interrupts with no error recorded.
/// The refill rewrites buffer contents and never the descriptor, so it never re-arms a bit; the fix is to never let the hardware clear them.
///
/// ## Where the frame lives is decided by the pixel clock
///
/// Measured on one board with one variable changed, and the cleanest result of the whole investigation.
/// Same board, same memory, same chain, same driver: in direct mode a frame in external memory streams fine, and in expander mode it never completes at any size.
/// So it is neither the memory nor the frame size: the engine cannot sustain an external read at the expander's clock.
/// An expander is serial, so each slot shifts out over eight bus words and the bus must run ten times faster, which is exactly the rate that memory cannot feed.
///
/// This backend is what proved it, which is why it was built.
/// The sibling failed here with thousands of descriptor-mount errors that pointed hard at its own handling.
/// This one removes that mechanism entirely and the errors are gone, yet the transfer still never completes.
/// So that storm was a symptom rather than the cause, killed by a controlled experiment with a working control condition.
///
/// Hence internal memory first in expander mode and external first otherwise, which caps the expander on this path at what fits internally.
/// Above that the driver does not use this path at all: it builds the streaming ring, which never materializes the frame.
///
/// ## Only the optional allocation is reserve-guarded
///
/// The reserve protects the network heap from an OPTIONAL allocation, and the frame itself is not optional.
/// Refusing it to keep the reserve intact would decline to drive the lights at all.
/// That degrades the essential thing to protect a nice-to-have, which inverts the policy.
///
/// With an expander there is no external fallback at all, internal or nothing.
/// Measured: a frame placed externally reports no output and burns a fifth of a second per tick timing out, while the same frame internally drives fine.
/// So such a fallback does not degrade, it WEDGES: init still succeeds, the driver reports driving, and every tick times out. Returning nothing instead surfaces the failure through the normal path.
///
/// ## Largest block, never total free
///
/// The whole-frame path allocates in one contiguous piece, so what matters is the largest free block: a fragmented heap can report megabytes free with no run big enough.
/// Using the total was a bug: at sixteen strands a 144 KB expander frame reported as fitting, so the ring was not chosen.
/// The contiguous allocation of those 144 KB then failed, fell back to external memory, and stalled at the expander clock.
/// The ring's own check deliberately uses free SIZE instead, since it makes many small allocations and needs no single run.
///
/// ## The wire is waited for, not refused
///
/// The peripheral holds exactly one transfer and the caller may legitimately hand over the next while one is still clocking out.
/// There is no queue to absorb it, and reprogramming the chain mid-walk would garble the frame, but refusing a busy bus would be wrong.
/// The double buffer's whole design is that the driver waits only on the buffer it is about to encode into.
/// At that point the other's transfer is quite legitimately still on the wire, and refusing there would drop every second frame.
/// Waiting here costs nothing that design was buying: its win is that the next encode overlapped the previous wire time, which has already happened by the time this is called.
/// What is left is the wire itself, which is serial on any design, the strand receiving one frame at a time.
///
/// ## The loopback follows the same rule the render path does
///
/// An expander test frame at the full light count lands in external memory and stalls at that clock on the whole-frame path.
/// The loopback would then time out and report nothing captured, blaming the transport for what is really a placement problem, and never testing the encode at all.
/// So when the frame will not fit internally it streams through the RING instead, exactly as the render path does, and the bit-verify then validates the actual ring output.
///
/// The ring's encode seam is a slice producer while the loopback already holds the whole pre-encoded frame.
/// So its encode is a copy of the matching slice out of that frame.
/// The row count must come from strand-side units: dividing by bus bytes mixes units and would build a ring for a fraction of the frame.
///
/// ## Two stall backstops, one recovery
///
/// On the ring path a cache-off window can outlast the pool's lead: the handler defers and the engine halts at the frontier.
/// A halted engine fires nothing further, so the completion never comes and the wait times out.
/// That is the designed benign outcome, the strand holding a partially-updated frame rather than replaying stale slices.
/// The frame must still be finalized, though, so the render thread proceeds.
/// It finalizes only once elapsed time proves the engine cannot still be mid-frame and the refills never reached the end, since a completed frame reports from the handler instead.
///
/// On the whole-frame path the completion interrupt is a latch that can very rarely be lost.
/// Two firings coalesce, or one races the next frame's reset, and the busy flag is left stuck.
/// Without recovery the bus wedges permanently, every later transmit blocking its full timeout and the driver's retry re-arming into the same state.
/// Here the condition is simply that the wait timed out with a transfer in flight, and the shared stop-and-clear does the rest.

#include "platform/platform.h"
#include "platform/loopback_verify.h"   // the loopback pattern and its checks

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_LCDCAM_I80_LCD_SUPPORTED

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_private/cache_utils.h"   // spi_flash_cache_enabled — the cache-safe ISR's defer guard
#include "esp_clk_tree.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"   // SIG_GPIO_OUT_IDX — detaches a matrix route (destroyState)
#include "esp_rom_sys.h"     // esp_rom_delay_us — the DMA-to-FIFO settle before lcd_ll_start
#include "esp_timer.h"       // esp_timer_get_time — ISR-safe wire-time stamp
#include "driver/gpio.h"
#include "soc/io_mux_reg.h"  // PIN_FUNC_GPIO — the IO-MUX function the matrix routes through
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "hal/gdma_channel.h"          // SOC_GDMA_TRIG_PERIPH_LCD0 / _BUS (the GDMA_MAKE_TRIGGER operands)
#include "hal/lcd_hal.h"               // lcd_hal_context_t / lcd_hal_init
#include "hal/lcd_ll.h"                // the LCD_CAM low-level register API
#include "hal/lcd_periph.h"            // soc_lcd_i80_signals — the GPIO-matrix signal indices
#include "esp_private/gdma.h"          // channel alloc / connect / strategy / transfer / start
#include "esp_private/gdma_link.h"     // the descriptor link list (esp_lcd uses exactly this)
#include "esp_private/esp_dma_utils.h" // esp_dma_calculate_node_count
#include "esp_private/gpio.h"          // gpio_func_sel — the IO-MUX select esp_lcd's GPIO setup uses
#include "esp_private/periph_ctrl.h"   // PERIPH_RCC_ACQUIRE_ATOMIC / PERIPH_RCC_ATOMIC
#include "esp_private/esp_clk_tree_common.h"  // esp_clk_tree_enable_src — power the LCD clock source

#include <atomic>      // std::atomic_thread_fence — orders the frontier splice's two descriptor writes
#include <cstring>
#include <functional>  // the transmit callback passed to the shared frame loopback
#include <new>         // std::nothrow

namespace mm::platform {

// The plain-GPIO continuity pre-check, shared with the RMT loopback: the wire question is identical.
namespace detail { bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio); }

namespace {

static const char* MOON_I80_TAG = "mm_moon_i80";

/// Three slots per bit at 375 ns, one slot HIGH for a zero: @xref{the-shift-clock-window|why 375 and not 416}.
constexpr uint32_t kPclkHz = 2'666'666;

/// The pixel clock with an expander fitted: @xref{the-shift-clock-window|the window and its three wall verdicts}.
constexpr uint8_t  kShiftClockDivDefault = 4;   // 80 MHz / 4 = 20 MHz
constexpr uint32_t kShiftBusResolutionHz = 80'000'000;   // PLL160M / kClockPreScale — the prescale base
// The live shift-clock prescale, a file-static because it is one global tuning knob rather than a per-call parameter.
uint8_t g_shiftClockDiv = kShiftClockDivDefault;

// The reset LOW, held as idle time between a frame's stop and the next arm rather than as clocked zeros, so it holds at any geometry for no extra memory.
constexpr int64_t kResetLowUs = 350;

// Flush slices past the close slice; file-scope so the completion and the backstop agree on the frame's last one.
constexpr uint32_t kTailBufs = 1;

// The group divider, restated rather than included because its header is private; kept identical to the sibling's so both produce the same waveform.
constexpr uint32_t kClockPreScale = 2;

// Max bytes one descriptor carries, so a full frame's chain costs a few hundred bytes: the chain is free.
constexpr size_t kDmaNodeMaxBytes = kRingNodeMaxBytes;

// The bus index; both chips have exactly one, and this backend owns it for the frame's duration.
constexpr int kBusId = 0;

// Ring mode: a closed chain over a few small internal buffers refilled as the DMA drains them, so it never reads external memory at the shift clock. That is the bound the whole-frame path hits once a frame no longer fits internally. See platform.h.

// Runtime geometry, both exposed as controls: @xref{the-rings-geometry-is-runtime|the four axes} and @xref{the-scatter-above-eight-slices-resolved|the scatter}. The shared bounds live in platform.h; here they size ring[] and gate the depth check.

// Backstop for a transmit arriving while the previous frame is still on the wire; it bounds a wedged peripheral so a broken DMA cannot hang the render thread.
constexpr uint32_t kWireFreeTimeoutMs = 200;

// The frame buffers, the peripheral, and the chain that streams one into the other.
// Two buffers for the deferred-wait double buffer, the second null when its allocation did not fit, each with its own completion signal.
// Transfers complete in start order and the event carries no token, so a two-slot queue of started indices routes each signal to the buffer that finished.
// There is no transaction queue, so a transmit programs the chain directly and a second one in flight is a caller error, rejected rather than queued.
struct MoonI80State {
    lcd_hal_context_t hal = {};
    gdma_channel_handle_t dma = nullptr;
    gdma_link_list_handle_t link = nullptr;
    SemaphoreHandle_t done[2] = {nullptr, nullptr};
    // Signals the wire is free, kept separate from the per-buffer ones the driver waits on: one producer and one consumer, so a binary semaphore fits exactly.
    SemaphoreHandle_t wireFree = nullptr;
    uint8_t* buf[2] = {nullptr, nullptr};
    size_t cap = 0;             // shared per-buffer capacity (both buffers equal)
    size_t busWidth = 8;        // 8 or 16 data lines
    uint32_t prescale = 1;      // pixel-clock prescale off the 80 MHz bus resolution
    bool clockAcquired = false; // the PERIPH_RCC bus-clock reference this state holds
    // The routes established at setup, kept so teardown can undo them: a deleted driver must leave no signal pointing at a freed peripheral.
    uint16_t routedPins[16] = {};   // data GPIOs routed to the bus (first `routedPinCount`)
    uint8_t  routedPinCount = 0;
    int32_t  routedWrGpio = -1;      // WR GPIO if routed (shift mode), else -1
    // In-order completion queue of started buffer indices, pushed by the transmit and popped by the interrupt; never more than one entry per buffer.
    volatile uint8_t fifo[2] = {0, 0};
    volatile uint8_t fifoHead = 0;   // next write slot (mod 2)
    volatile uint8_t fifoTail = 0;   // next read slot (mod 2)
    // Wire-time measurement: each in-flight transfer's start stamp and the last duration, paired with the queue so it tracks the right transfer.
    volatile int64_t txStartUs[2] = {0, 0};
    volatile uint32_t lastTransmitUs = 0;
    // When the peripheral last stopped, which is when the strand starts idling LOW and the reset begins. The next arm waits that out, so the reset is real at any geometry.
    volatile int64_t lastStopUs = 0;
    volatile bool busy = false;      // a transfer is clocking out right now

    // --- Ring mode. Null/zero on a whole-frame handle; populated only by moonI80Ws2812InitRing. ------
    bool isRing = false;
    uint8_t* ring[kRingBufsMax] = {};   // the internal-RAM slice buffers the DMA loops over (first `ringBufs` used)
    uint8_t  ringBufs = 0;           // pool depth in use — the live count; kRingBufsMax is only the array bound
    uint32_t rowsPerBuf = 0;         // rows one ring buffer holds (the last SLICE may be shorter)
    uint32_t totalRows = 0;          // strand length in rows — the frame ends after this many
    size_t   linkItemCap = 0;        // descriptor-pool capacity — the mount loop must not exceed it (IDF wraps silently)
    uint32_t consumedItems = 0;      // descriptor items the mount loop actually used (diagnostic; == linkItemCap when sized right)
    // Descriptor nodes per buffer, since one larger than a node's maximum spans several.
    uint8_t  itemsPerBuf = 1;
    // The node the prime-only path mounts terminated, so the DMA ends the frame itself rather than a mid-frame stop racing the prefetcher. Diagnostic only.
    int32_t  termNode = -1;
    // Each buffer's ACTUAL last node, taken from the mount rather than computed: the mount may allocate a different count than predicted, and arithmetic terminated a buffer early.
    int32_t  bufLastNode[kRingBufsMax] = {};
    // Whether a buffer's constants were cleared, so a uniform-lane encoder can skip about a third of the refill cost on one whose constants still stand.
    bool     bufNeedsPrefill[kRingBufsMax] = {};
    size_t   ringRowBytes = 0;       // encoded bytes per row (encode writes rowsPerBuf × this per buffer)
    MoonI80EncodeFn   encode = nullptr;   // the domain's slice encoder (platform.h seam)
    void*             encodeUser = nullptr;
    // The clock oracle: the drain position is a function of TIME rather than of interrupt arrivals, since the DMA free-runs at the exact bus rate.
    // The interrupt is a latch and not a queue, so two under load coalesce into one firing and any per-firing counter undercounts.
    // Deriving the refill target from elapsed time makes a coalesced interrupt change only when work happens, never what gets written.
    volatile int64_t  armUs = 0;          // esp_timer time at gdma_start — the oracle's epoch, per frame
    uint32_t          sliceNs = 0;        // one slice's wire duration incl. the pad (bytes × 37.5 ns + padUs)
    // The refill cursor as a slice index, the mount order fixing which buffer each slice lives in; advanced in batches toward the writable window.
    volatile uint32_t lastWrittenSlice = 0;   // highest slice index already encoded (or zero-filled) this frame
    // One shared zero block every pad node points at. A LOW gap under the latch threshold reads as a pause, stretching the refill deadline at a linear cost in frame time.
    uint8_t* zeroPad = nullptr;
    size_t   zeroPadBytes = 0;
    uint8_t  padUs = 0;
    // Cache facts hoisted out of the refill, the line size being a per-pool constant: querying it per refill was a flash call inside the interrupt.
    size_t   ringCacheLine = 0;
    uint32_t          nSlices = 0;        // total slices in the frame = ceil(totalRows / rowsPerBuf)
    volatile uint32_t drainCount = 0;     // DIAGNOSTIC ONLY: the oracle position the last EOF observed
    // Lifetime interrupt and completion counts, best-effort rather than a contract: they tell a stalled refill apart from no interrupts at all.
    volatile uint32_t dbgEofTotal = 0;
    volatile uint32_t dbgDoneGiven = 0;
    volatile uint32_t dbgLastDrain = 0;
    // Deferred refills and their worst consecutive run: the interrupt skips a refill whenever the flash cache is off, and a write outlasting the pool's lead corrupts a frame.
    volatile uint32_t dbgCacheOffDefers = 0;
    volatile uint32_t dbgCacheOffRun = 0;      // current consecutive-defer streak
    volatile uint32_t dbgCacheOffMaxRun = 0;   // worst streak seen (≈ how many buffers the DMA could lap)
    // Frames the backstop finalized after the DMA halted at the frontier during a stall: each is a partially-updated frame, the benign outcome the terminator exists to produce.
    volatile uint32_t dbgStallAbandons = 0;
    // A descriptor-error count, which turns an otherwise silent halt into a visible signal: above zero at a stall means the descriptor pool was corrupted.
    volatile uint32_t dbgDescErr = 0;
    // The worst refill time against the worst gap between interrupts: the one is the deadline the other must beat, which tells a pace problem from a cursor problem.
    volatile uint32_t dbgMaxEncodeUs = 0;
    // The average is the pace number and the maximum above is the jitter one; conflating them cost a day. Divided at readout, so the interrupt does no division.
    volatile uint32_t dbgEncSumUs = 0;
    volatile uint32_t dbgEncCount = 0;
    volatile uint32_t dbgEncAvgUs = 0;   // LAST FRAME's average, latched at frame end — the readout target
                                         // (reading sum/count mid-frame races the per-frame reset: the 1 s KPI tick correlates with the frame cycle and kept landing in the freshly-reset window, reading a false 0)
    volatile uint32_t dbgMaxIsrGapUs = 0;
    volatile int64_t  dbgLastEofUs = 0;
    // The scatter meter: slices written after their drain had begun, each stale on the wire. A clean soak reads zero, whether or not the eye catches it.
    volatile uint32_t dbgLate = 0;
};

// The descriptor-error callback, registered so a fetch fault is counted rather than ignored.
bool IRAM_ATTR moonI80DescErrCb(gdma_channel_handle_t, gdma_event_data_t*, void* user) {
    auto* st = static_cast<MoonI80State*>(user);
    st->dbgDescErr = st->dbgDescErr + 1u;
    return false;
}

// Forward declaration: the interrupt's ring branch refills the drained buffer by calling this.
void encodeRingSlice(MoonI80State* st, uint8_t slot, uint32_t firstRow, uint32_t count);
// The shared slice fill; the attribute goes on the definition only, since repeating it here conflicts the section.
bool fillSlice(MoonI80State* st, uint8_t slot, uint32_t sliceIdx);

// The node a buffer ends at, which is where the frontier terminator rests and where an inter-buffer link leaves from.
inline int ringTailNode(const MoonI80State* st, uint8_t b) {
    return st->bufLastNode[b] + (st->zeroPad ? 1 : 0);
}

// The completion callback: the chain reached its end node, so pop the oldest started index, record the wire duration and release that buffer's waiter.
// The event fires when the last bytes reach the peripheral's queue rather than the pins, which suits this contract: a wait gates reusing a buffer rather than reading the output.
// Resident in instruction memory, and the encode chain with it: @xref{the-interrupt-does-the-encode-and-must-never-touch-flash|the guard that must not be removed}.
bool IRAM_ATTR moonI80EofCb(gdma_channel_handle_t, gdma_event_data_t*, void* user) {
    auto* st = static_cast<MoonI80State*>(user);

    // Ring mode, in two shapes. When every slice was encoded before arming, only the terminator carries the end mark, so the one interrupt that arrives IS the frame's end.
    // When the frame laps the pool, every data node carries it and the handler is driven by elapsed time.
    // Not by its own arrival count, the interrupt being a latch where two coalesce into one firing.
    // Each firing refills every slice the writable window allows and advances the terminator, so a coalesced interrupt changes only when work happens, never what gets written.
    if (st->isRing) {
        BaseType_t high = pdFALSE;
        // The cache-off guard, the pattern the vendor's own cache-safe handlers use: @xref{the-interrupt-does-the-encode-and-must-never-touch-flash|why deferring is free}.
        if (!spi_flash_cache_enabled()) {
            // Cache off: this firing refills NOTHING and does NOT advance the frontier. Count it + track the consecutive-defer streak (buffers drained un-refilled while the write holds). If the write outlasts the pool's lead, the DMA reaches the frontier NULL and HALTS, the wait backstop then finalizes the held frame (dbgStallAbandons). No stale-slice replay: the frontier is the guard.
            st->dbgCacheOffDefers = st->dbgCacheOffDefers + 1u;
            st->dbgCacheOffRun = st->dbgCacheOffRun + 1u;
            if (st->dbgCacheOffRun > st->dbgCacheOffMaxRun) st->dbgCacheOffMaxRun = st->dbgCacheOffRun;
            return false;
        }
        st->dbgCacheOffRun = 0;                          // a live firing ends the streak
        st->dbgEofTotal = st->dbgEofTotal + 1u;         // ISR INSTRUMENTATION (diagnostic)
        const int64_t eofNow = esp_timer_get_time();
        if (st->dbgLastEofUs != 0) {
            const uint32_t gap = static_cast<uint32_t>(eofNow - st->dbgLastEofUs);
            if (gap > st->dbgMaxIsrGapUs) st->dbgMaxIsrGapUs = gap;
        }
        st->dbgLastEofUs = eofNow;

        const bool primeOnly = st->nSlices <= st->ringBufs;
        if (!primeOnly && st->busy) {
            // The oracle: slices whose drain has COMPLETED. sliceNs is exact (bytes × 37.5 ns + pad), so the only error source is esp_timer resolution, microseconds against a >100 µs slice.
            const uint32_t drainPos = static_cast<uint32_t>(
                ((eofNow - st->armUs) * 1000) / st->sliceNs);
            st->drainCount = drainPos;      // DIAGNOSTIC (dbgLastDrain mirrors the old counter's slot)
            st->dbgLastDrain = drainPos;

            // Batch refill toward the writable window: a slice's previous occupant is provably drained once the drain position has passed it, and a safety margin sets the edge.
            // Capped per firing to bound the handler's duration, and capped batches still converge since interrupts keep arriving.
            // The pool is the jitter buffer, so only the average encode must beat the deadline.
            constexpr uint32_t kLead = 2;
            constexpr uint32_t kBatchMax = 4;
            // kTailBufs (file scope): one pure-zero flush slice past the frame-close slice, so a stop firing on the second-to-last EOF (oracle floor at a slice boundary) can only truncate zeros, never the frame-close latch (which streams a whole slice earlier).
            const uint32_t windowEnd = drainPos + st->ringBufs - kLead;
            // The frame's last writes are the frame-close slice (index nSlices: zeros + the closing latch word) and kTailBufs pure-zero flush slices. The frontier terminator then RESTS on the final node as the frame's hardware end, the DMA cannot pass a NULL, so nothing past the frame is ever re-read (no lap of zero-fill needed beyond it).
            const uint32_t lastSlice = st->nSlices + kTailBufs;
            for (uint32_t n = 0; n < kBatchMax; n++) {
                const uint32_t s = st->lastWrittenSlice + 1u;
                if (s > windowEnd || s > lastSlice) break;
                const uint32_t slot = s % st->ringBufs;
                // Stale-on-the-wire detector: the drain of slice s begins at drainPos == s, so filling at or after that moment means the wire already clocked (part of) the OLD contents.
                if (drainPos >= s) st->dbgLate = st->dbgLate + 1u;
                const int64_t encStart = esp_timer_get_time();   // REUSE-RACE INSTRUMENTATION (diagnostic)
                const bool realEncode = fillSlice(st, static_cast<uint8_t>(slot), s);
                // Time only REAL encodes (fillSlice==true), the `ea`/max pace numbers must reflect the refill that has to beat the slice deadline, not the cheap past-frame zero-fills.
                if (realEncode) {
                    const uint32_t encUs = static_cast<uint32_t>(esp_timer_get_time() - encStart);
                    if (encUs > st->dbgMaxEncodeUs) st->dbgMaxEncodeUs = encUs;
                    st->dbgEncSumUs = st->dbgEncSumUs + encUs;    // average = sum/count at readout
                    st->dbgEncCount = st->dbgEncCount + 1u;
                }
                // The moving terminator: the chain always ends at the last written slice, the new frontier terminated first and the old one then extended into it.
                // In that order, with a fence between the two writes, there is never an instant where the chain runs past written data into a stale link.
                // A stalled refill therefore halts the engine at the frontier and the strand latches a partially-updated frame, instead of lapping the pool and re-clocking stale slices as bright garbage.
                // The link edit writes through a non-cacheable alias and needs no sync, and it runs only on the cache-on path.
                gdma_link_concat(st->link, ringTailNode(st, static_cast<uint8_t>(slot)), nullptr, 0);
                std::atomic_thread_fence(std::memory_order_release);
                gdma_link_concat(st->link, ringTailNode(st, static_cast<uint8_t>((s - 1u) % st->ringBufs)),
                                 st->link, st->bufLastNode[slot]);
                st->lastWrittenSlice = s;
            }

            // Frame end, reported on written rather than drained: @xref{why-completion-is-reported-on-written-not-drained|the coalescing trap this dodges}.
            // The engine is not stopped here, the terminator halting it, and stopping now would truncate the still-draining wire.
            // Fires exactly once per frame, the block being gated on the busy flag that the completion clears.
            if (st->lastWrittenSlice >= lastSlice) {
                st->busy = false;
                // Latch this frame's encode average and reset the window, at frame END, when every refill has landed, so a readout never sees a half-filled (or just-reset) window.
                st->dbgEncAvgUs = st->dbgEncCount ? st->dbgEncSumUs / st->dbgEncCount : 0;
                st->dbgEncSumUs = 0;
                st->dbgEncCount = 0;
                // The wire finishes a whole frame's duration after the arm, and the reset begins there rather than at this earlier written moment.
                // The next arm waits out the real tail before re-arming, and the frame carries its flush slice too.
                // Omitting that would start the reset a slice early and release the barrier while it is still draining.
                const uint32_t frameWireUs = static_cast<uint32_t>(
                    (static_cast<uint64_t>(st->nSlices + kTailBufs) * st->sliceNs) / 1000u);
                st->lastStopUs = st->armUs + frameWireUs;
                st->lastTransmitUs = frameWireUs;
                st->dbgDoneGiven = st->dbgDoneGiven + 1u;
                xSemaphoreGiveFromISR(st->done[0], &high);   // the ring reports completion on slot 0
            }
        } else if (primeOnly && st->busy) {
            // The terminator's EOF, the frame's one interrupt. The chain has self-terminated at NULL.
            st->drainCount = st->nSlices;
            st->dbgLastDrain = st->nSlices;
            st->busy = false;
            st->lastStopUs = eofNow;
            st->lastTransmitUs = static_cast<uint32_t>(eofNow - st->txStartUs[0]);
            st->dbgDoneGiven = st->dbgDoneGiven + 1u;
            xSemaphoreGiveFromISR(st->done[0], &high);
        }
        return high == pdTRUE;
    }

    // Whole-frame mode: pop the oldest started buffer, record its wire time, release its waiter. Guard on a non-empty FIFO: if the wait-timeout backstop already drained this entry (fifoTail == fifoHead) and the lost EOF then arrives late, popping would read a stale slot, hand out a spurious `done`, and desync tail past head, so a firing against a structurally empty FIFO is dropped.
    if (st->fifoTail == st->fifoHead) return false;
    const uint8_t slot = st->fifoTail;
    const uint8_t b = st->fifo[slot] & 1u;
    const int64_t now = esp_timer_get_time();
    st->lastTransmitUs = static_cast<uint32_t>(now - st->txStartUs[slot]);
    st->fifoTail = (st->fifoTail + 1u) & 1u;
    st->busy = false;
    BaseType_t high = pdFALSE;
    xSemaphoreGiveFromISR(st->done[b], &high);
    BaseType_t highWire = pdFALSE;
    xSemaphoreGiveFromISR(st->wireFree, &highWire);   // release a transmit blocked on the wire
    return (high == pdTRUE) || (highWire == pdTRUE);
}

void destroyState(MoonI80State* st) {
    if (!st) return;
    // GDMA teardown FIRST: the refill now runs in the EOF ISR, so stopping + deleting the channel (which disables the interrupt) is what guarantees no further refill fires into a buffer about to be freed. gdma_stop halts the engine; gdma_del_channel detaches the ISR. After this no EOF handler can run, so the buffer frees below are safe.
    if (st->dma) {
        gdma_stop(st->dma);
        gdma_disconnect(st->dma);
        gdma_del_channel(st->dma);
    }
    if (st->link) gdma_del_link_list(st->link);
    // Detach the GPIO-matrix routes configureGpio established, so a deleted driver leaves no data/WR signal pointing at this (now torn-down) peripheral. Route each back to plain GPIO (SIG_GPIO_OUT_IDX = no peripheral). Safe after the GDMA barrier above, nothing is clocking these pins any more, and a route that was never made (routedPinCount 0, routedWrGpio -1) is simply skipped.
    for (uint8_t i = 0; i < st->routedPinCount; i++)
        esp_rom_gpio_connect_out_signal(st->routedPins[i], SIG_GPIO_OUT_IDX, false, false);
    if (st->routedWrGpio >= 0)
        esp_rom_gpio_connect_out_signal(static_cast<uint16_t>(st->routedWrGpio), SIG_GPIO_OUT_IDX,
                                        false, false);
    if (st->hal.dev) {
        lcd_ll_stop(st->hal.dev);
        PERIPH_RCC_ATOMIC() {
            lcd_ll_enable_clock(st->hal.dev, false);
        }
    }
    // Release the bus-clock reference taken in createState (the peripheral powers down when the last holder, us or an esp_lcd bus, lets go). Mirrors esp_lcd_del_i80_bus, esp_lcd_panel_io_i80.c:305.
    if (st->clockAcquired) {
        PERIPH_RCC_ACQUIRE_ATOMIC(soc_lcd_i80_signals[kBusId].module, ref_count) {
            if (ref_count == 0) lcd_ll_enable_bus_clock(kBusId, false);
        }
    }
    for (auto* b : st->buf) if (b) heap_caps_free(b);
    for (auto* b : st->ring) if (b) heap_caps_free(b);   // ring buffers (null on a whole-frame handle)
    if (st->zeroPad) heap_caps_free(st->zeroPad);        // the shared inter-buffer pad block
    for (auto* s : st->done) if (s) vSemaphoreDelete(s);
    if (st->wireFree) vSemaphoreDelete(st->wireFree);
    delete st;
}

// Bring the peripheral up for a pure data phase, replicating the sibling's bus creation and clock selection minus everything a strand frame does not use. No peripheral interrupt, since the transfer completion is ours, and no transaction queue, format buffer, power lock or sleep retention.
bool initPeripheral(MoonI80State* st, uint32_t pclkHz) {
    // Bus resolution = source clock / the group prescale. Ask the clock tree rather than assuming 160 MHz, so a future default-source change can't silently retune the WS2812 waveform.
    uint32_t srcHz = 0;
    if (esp_clk_tree_enable_src(static_cast<soc_module_clk_t>(LCD_CLK_SRC_DEFAULT), true) != ESP_OK)
        return false;
    if (esp_clk_tree_src_get_freq_hz(static_cast<soc_module_clk_t>(LCD_CLK_SRC_DEFAULT),
                                     ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &srcHz) != ESP_OK)
        return false;
    const uint32_t resolutionHz = srcHz / kClockPreScale;   // 80 MHz with the PLL160M default
    const uint32_t prescale = resolutionHz / pclkHz;
    // The prescale is an integer register field: an inexact pclk rounds DOWN into a LONGER slot, and a too-long "0" pulse is read as a "1" (the max-white washout). Both supported rates divide 80 MHz exactly, so reject anything that doesn't rather than emit a wrong waveform.
    if (prescale == 0 || prescale > LCD_LL_PCLK_DIV_MAX) return false;
    st->prescale = prescale;

    // Power the peripheral's APB/bus clock. Reference-counted: esp_lcd may hold the same peripheral for the sibling driver, so the reset only fires for the first holder.
    PERIPH_RCC_ACQUIRE_ATOMIC(soc_lcd_i80_signals[kBusId].module, ref_count) {
        if (ref_count == 0) {
            lcd_ll_enable_bus_clock(kBusId, true);
            lcd_ll_reset_register(kBusId);
        }
    }
    st->clockAcquired = true;

    lcd_hal_init(&st->hal, kBusId);
    lcd_cam_dev_t* dev = st->hal.dev;

    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_clock(dev, true);
        lcd_ll_select_clk_src(dev, LCD_CLK_SRC_DEFAULT);
        // Integer division only (0/0 for the fractional part), a fractional group divider adds clock jitter, which on a self-clocked WS2812 stream is bit error.
        lcd_ll_set_group_clock_coeff(dev, static_cast<int>(kClockPreScale), 0, 0);
    }

    lcd_ll_reset(dev);
    lcd_ll_fifo_reset(dev);

    // No LCD interrupt is installed at all: esp_lcd needs one to dispatch its transaction queue, we do not (the GDMA EOF is the only completion event this driver has). Mask the peripheral's interrupts and clear anything the previous owner left pending, so a stale TRANS_DONE can't fire into an ISR that isn't ours.
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_interrupt(dev, LCD_LL_EVENT_I80, false);
    }
    lcd_ll_clear_interrupt_status(dev, UINT32_MAX);

    lcd_ll_enable_rgb_mode(dev, false);        // i80 (command/data) mode, not RGB timing mode
    lcd_ll_enable_color_convert(dev, false);   // no RGB/YUV conversion — the frame is raw slot words
    lcd_ll_set_dma_read_stride(dev, st->busWidth);   // bytes the FIFO pulls per bus word
    lcd_ll_set_data_wire_width(dev, st->busWidth);   // data lines actually driven
    // "output always on" is what makes the data-phase length come from the DMA chain rather than a cycle count, the property this whole backend is built on (esp_lcd_panel_io_i80.c:226).
    lcd_ll_enable_output_always_on(dev, true);
    lcd_ll_set_swizzle_mode(dev, LCD_LL_SWIZZLE_AB2BA);  // mode select only; the swizzle stays OFF below
    lcd_ll_enable_swizzle(dev, false);              // byte order as encoded — ParallelSlots writes bus words directly
    lcd_ll_reverse_dma_data_bit_order(dev, false);  // bit L of the word IS data line L (the encoder's contract)
    lcd_ll_swap_dma_data_byte_order(dev, false);

    lcd_ll_set_pixel_clock_prescale(dev, prescale);
    lcd_ll_set_clock_idle_level(dev, false);   // WR rests LOW, like the data lines (pclk_idle_low)
    lcd_ll_set_pixel_clock_edge(dev, false);   // data latched on the rising edge
    // DC is a peripheral-mandated line the strands ignore; park it LOW in every phase so it cannot toggle a neighboring strand if a board wires it to one.
    lcd_ll_set_dc_level(dev, /*idle=*/false, /*cmd=*/false, /*dummy=*/false, /*data=*/false);
    return true;
}

// Route the peripheral's signals onto real pins: @xref{only-the-data-lines-reach-a-pad|why the control lines and spare lanes cost none}. So the data-command line is never routed, and the strobe only when an expander needs it on a pin.
void configureGpio(MoonI80State* st, const uint16_t* dataPins, uint8_t laneCount, uint16_t wrGpio,
                   bool routeWr) {
    const uint8_t n = laneCount < 16 ? laneCount : 16;
    for (size_t i = 0; i < laneCount; i++) {
        gpio_func_sel(static_cast<gpio_num_t>(dataPins[i]), PIN_FUNC_GPIO);
        esp_rom_gpio_connect_out_signal(dataPins[i], soc_lcd_i80_signals[kBusId].data_sigs[i],
                                        false, false);
        // Maximum drive strength on every routed pin, data and the expander's latch alike.
        // Strong pads mean sharp edges, which is what the register needs to sample cleanly over real strand wiring.
        // Marginal strands lose the most margin and the strongest edge buys it back.
        gpio_set_drive_capability(static_cast<gpio_num_t>(dataPins[i]), GPIO_DRIVE_CAP_3);
        if (i < n) st->routedPins[i] = dataPins[i];   // recorded for teardown (see destroyState)
    }
    st->routedPinCount = n;
    if (routeWr) {
        gpio_func_sel(static_cast<gpio_num_t>(wrGpio), PIN_FUNC_GPIO);
        esp_rom_gpio_connect_out_signal(wrGpio, soc_lcd_i80_signals[kBusId].wr_sig, false, false);
        gpio_set_drive_capability(static_cast<gpio_num_t>(wrGpio), GPIO_DRIVE_CAP_3);   // SRCLK edge
        st->routedWrGpio = wrGpio;
    }
}

// The transfer channel and its descriptor chain, replicating the sibling's setup with two deliberate differences.
// Owner-checking is off: @xref{the-chain-is-ours-so-the-owner-check-protects-nothing|why it only fails here}.
// And the completion callback is the engine's own rather than a peripheral interrupt.
bool initDma(MoonI80State* st, size_t bufferBytes) {
    gdma_channel_alloc_config_t chanCfg = {};
    // The S3's LCD hangs off the AHB GDMA, the P4's off the AXI one, and the descriptor alignment differs with the bus. Same selection esp_lcd makes (esp_lcd_panel_io_i80.c:19-27), including its `defined(...) &&` guard, on a chip with no AXI GDMA the AXI symbol is simply absent, and an unguarded `== SOC_GDMA_BUS_AXI` would compare against the preprocessor's 0 and match.
#if defined(SOC_GDMA_BUS_AXI) && (SOC_GDMA_TRIG_PERIPH_LCD0_BUS == SOC_GDMA_BUS_AXI)
    constexpr size_t kDescAlign = 8;
    if (gdma_new_axi_channel(&chanCfg, &st->dma, nullptr) != ESP_OK) return false;
#else
    constexpr size_t kDescAlign = 4;
    if (gdma_new_ahb_channel(&chanCfg, &st->dma, nullptr) != ESP_OK) return false;
#endif
    if (gdma_connect(st->dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0)) != ESP_OK) return false;

    gdma_strategy_config_t strategy = {};
    strategy.auto_update_desc = true;
    strategy.owner_check = false;   // see (1) above — we own the chain
    if (gdma_apply_strategy(st->dma, &strategy) != ESP_OK) return false;

    gdma_transfer_config_t transfer = {};
    transfer.max_data_burst_size = 64;   // the burst esp_lcd's callers ask for; keeps PSRAM reads efficient
    transfer.access_ext_mem = true;      // the frame usually lives in PSRAM (LCD_CAM GDMA reaches it)
    if (gdma_config_transfer(st->dma, &transfer) != ESP_OK) return false;

    size_t intAlign = 0, extAlign = 0;
    if (gdma_get_alignment_constraints(st->dma, &intAlign, &extAlign) != ESP_OK) return false;
    const size_t bufAlign = intAlign > extAlign ? intAlign : extAlign;

    gdma_link_list_config_t linkCfg = {};
    linkCfg.item_alignment = kDescAlign;
    linkCfg.num_items = esp_dma_calculate_node_count(bufferBytes, bufAlign, kDmaNodeMaxBytes);
    linkCfg.flags.check_owner = false;   // see (1)
    if (gdma_new_link_list(&linkCfg, &st->link) != ESP_OK) return false;

    gdma_tx_event_callbacks_t cbs = {};
    cbs.on_trans_eof = moonI80EofCb;
    return gdma_register_tx_event_callbacks(st->dma, &cbs, st) == ESP_OK;
}

// Allocate one DMA-capable frame buffer, honouring the GDMA's alignment constraints (both the address and the length must be a cache-line multiple on the P4 / on PSRAM, or the cache write-back before a transfer would touch neighboring allocations).
uint8_t* allocFrame(MoonI80State* st, size_t bufferBytes, bool psram) {

    size_t intAlign = 0, extAlign = 0;
    gdma_get_alignment_constraints(st->dma, &intAlign, &extAlign);
    const size_t align = psram ? extAlign : intAlign;
    const uint32_t caps = MALLOC_CAP_8BIT | MALLOC_CAP_DMA
                        | (psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
    // Round the size up to the alignment too, gdma_link_mount_buffers checks the LENGTH against the same constraint, and a short tail would fail the mount. The pad is zeroed, so it just extends the frame's trailing latch gap (the lines already idle LOW there).
    const size_t size = align > 1 ? ((bufferBytes + align - 1) / align) * align : bufferBytes;
    return static_cast<uint8_t*>(heap_caps_aligned_calloc(align ? align : 4, 1, size, caps));
}

// One peripheral + GDMA chain + frame buffer(s). `wantSecond` allocates the async double-buffer's second frame buffer (best-effort, null if it won't fit); false allocates buffer 0 only. Shared by the runtime init and the loopback (which passes false, one transfer).
MoonI80State* createState(const uint16_t* dataPins, uint8_t laneCount,
                          uint16_t wrGpio, size_t bufferBytes, bool wantSecond,
                          uint8_t clockMultiplier) {
    auto* st = new (std::nothrow) MoonI80State();
    if (!st) return nullptr;

    // Bus width is power-of-two only (8 or 16), derived from the lane count: ≤8 → 8, 9..16 → 16.
    st->busWidth = laneCount <= 8 ? 8 : 16;

    // An expander shifts each slot out over several bus words, so the bus must clock proportionally faster to keep the slot inside its window. The shift clock is the bus resolution over the runtime divider, and the init recomputes the exact prescale from the clock tree, so this need only be the intended rate.
    const uint32_t pclkHz = (clockMultiplier > 1) ? (kShiftBusResolutionHz / g_shiftClockDiv) : kPclkHz;
    if (!initPeripheral(st, pclkHz) || !initDma(st, bufferBytes)) {
        destroyState(st);
        return nullptr;
    }
    // WR reaches a pad only when a '595 needs it as the shift clock; a direct-mode strand ignores it, and DC never reaches a pad at all. See configureGpio.
    configureGpio(st, dataPins, laneCount, wrGpio, /*routeWr=*/clockMultiplier > 1);

    st->done[0] = xSemaphoreCreateBinary();
    st->wireFree = xSemaphoreCreateBinary();
    if (!st->done[0] || !st->wireFree) {
        destroyState(st);
        return nullptr;
    }

    // Where the frame lives is decided by the pixel clock rather than its size: @xref{where-the-frame-lives-is-decided-by-the-pixel-clock|the measurement}. The frame itself is not reserve-guarded, unlike the optional second buffer: @xref{only-the-optional-allocation-is-reserve-guarded|why, and why an expander has no external fallback at all}.
    const bool pinExpanderMode = clockMultiplier > 1;
    st->buf[0] = allocFrame(st, bufferBytes, /*psram=*/!pinExpanderMode);
    if (!st->buf[0] && !pinExpanderMode) st->buf[0] = allocFrame(st, bufferBytes, /*psram=*/false);
    if (!st->buf[0]) {
        destroyState(st);
        return nullptr;
    }
    st->cap = bufferBytes;

    // The second buffer, only when asked, arming double buffering if it fits and leaving the driver single-buffered if not.
    // The reserve guards the INTERNAL attempt, whichever that is: the preference order flips with the mode.
    // So binding the guard to a fixed branch would put it on the wrong one half the time.
    // A small helper keeps the rule with the thing it guards.
    if (wantSecond) {
        st->done[1] = xSemaphoreCreateBinary();
        if (st->done[1]) {
            auto tryAlloc = [&](bool psram) -> uint8_t* {
                if (!psram
                    && heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
                           < bufferBytes + HEAP_RESERVE) {
                    return nullptr;   // internal, and it would eat the reserve — refuse
                }
                return allocFrame(st, bufferBytes, psram);
            };
            // Same rule as buf[0]: with a pin expander it is internal-or-nothing. A PSRAM buf[1] is worse than no second buffer at all, tickAsync would alternate a working internal buf[0] with a PSRAM buf[1] that never completes, giving exactly one frame at boot and then ~207 ms per tick forever. Refusing it leaves buf[1] null, which the driver reads as "run single-buffered".
            st->buf[1] = tryAlloc(/*psram=*/!pinExpanderMode);
            if (!st->buf[1] && !pinExpanderMode) st->buf[1] = tryAlloc(/*psram=*/false);
            if (!st->buf[1]) {
                vSemaphoreDelete(st->done[1]);
                st->done[1] = nullptr;
            }
        }
    }
    return st;
}

// Program the chain and start the ONE gapless transaction. This is the whole point of the backend: esp_lcd's per-transaction reset happens exactly once here, at the START of the single transfer that carries the entire frame, never between chunks of it.
bool startTransfer(MoonI80State* st, uint8_t buffer, size_t bytes) {
    lcd_cam_dev_t* dev = st->hal.dev;

    // Write the encoded frame back from cache to physical memory, the GDMA reads DRAM/PSRAM, not the CPU's cache. (esp_lcd does this inside tx_color, esp_lcd_panel_io_i80.c:576.)
    if (esp_cache_get_line_size_by_addr(st->buf[buffer]) > 0) {
        esp_cache_msync(st->buf[buffer], bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    // Mount the WHOLE frame in ONE call: the link list splits it across as many 4095-byte nodes as it needs and chains them, so the DMA walks the frame end to end without CPU involvement. mark_eof fires our completion callback on the last node; mark_final NULLs its next-pointer so the DMA stops there instead of wrapping to the head.
    gdma_buffer_mount_config_t mount = {};
    mount.buffer = st->buf[buffer];
    mount.length = bytes;
    mount.flags.mark_eof = true;
    mount.flags.mark_final = GDMA_FINAL_LINK_TO_NULL;
    if (gdma_link_mount_buffers(st->link, 0, &mount, 1, nullptr) != ESP_OK) return false;

    // Data phase only: no command cycles, no dummy cycles, `data_cycles = 1` is a boolean ENABLE, the peripheral clocks whatever the DMA chain feeds it and stops when the chain ends ("Number of data phase cycles are controlled by DMA buffer length", esp_lcd_panel_io_i80.c:778).
    lcd_ll_set_phase_cycles(dev, /*cmd=*/0, /*dummy=*/0, /*data=*/1);
    lcd_ll_set_blank_cycles(dev, 1, 1);
    lcd_ll_reset(dev);
    lcd_ll_fifo_reset(dev);   // discard any FIFO residue from the previous frame

    // Start the transfer engine first, the peripheral only consuming once data has reached its queue. The settle is the sibling's, shortened: at these clocks the queue fills far faster than one word period, so this is ample and keeps the inter-frame gap short.
    if (gdma_start(st->dma, gdma_link_get_head_addr(st->link)) != ESP_OK) return false;
    esp_rom_delay_us(1);
    lcd_ll_start(dev);
    return true;
}

// --- Ring mode ---------------------------------------------------------------------------------------

// Encode one slice straight into the internal buffer the engine is about to read.
// A ring buffer holds rows and nothing else, the reset coming from stopping the peripheral, so a pad written here would land past the allocation.
// The cache sync is a no-op for internal memory but kept for symmetry, and for correctness if a buffer ever lands cache-mapped.
// The chain lives in instruction memory for throughput: flash-resident code shares one cache between both cores, which the render core's churn evicts between firings.
void IRAM_ATTR encodeRingSlice(MoonI80State* st, uint8_t slot, uint32_t firstRow, uint32_t count) {
    // Hand the encoder the buffer-lifecycle fact its prefill-skip hangs on, and consume it: after this call the buffer's constants are laid (or were already), until a memset invalidates them again.
    const bool needsPrefill = st->bufNeedsPrefill[slot];
    st->bufNeedsPrefill[slot] = false;
    st->encode(st->encodeUser, st->ring[slot], firstRow, count, /*closeFrame=*/false, needsPrefill);
    // ringCacheLine is a per-pool constant hoisted to createRingState: the per-refill line-size query was a flash call inside the ISR (illegal on a cache-safe channel, and a needless icache miss before).
    if (st->ringCacheLine > 0) {
        esp_cache_msync(st->ring[slot], static_cast<size_t>(count) * st->ringRowBytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
}

// The one place the slice-fill rule lives, called both by the prime and by the interrupt's refill.
// A buffer holding a real slice gets its rows encoded, and one past the frame's slices is fully zeroed so the loop clocks clean.
// The first past-frame buffer carries the closing latch word at its head, one more latch so the register's final slot presents before the reset.
// True when it ran a real encode rather than a zero fill, since the interrupt times only those and cheap fills would dilute the pace number.
bool IRAM_ATTR fillSlice(MoonI80State* st, uint8_t slot, uint32_t sliceIdx) {
    const uint32_t firstRow = sliceIdx * st->rowsPerBuf;
    if (firstRow < st->totalRows) {
        uint32_t count = st->rowsPerBuf;
        bool shortSlice = false;
        if (firstRow + count >= st->totalRows) {
            count = st->totalRows - firstRow;
            if (count < st->rowsPerBuf) {
                std::memset(st->ring[slot] + static_cast<size_t>(count) * st->ringRowBytes, 0,
                            static_cast<size_t>(st->rowsPerBuf - count) * st->ringRowBytes);
                shortSlice = true;
            }
        }
        encodeRingSlice(st, slot, firstRow, count);
        // AFTER the encode (it consumed this use's prefill flag): the tail memset erased those rows' constants, so the buffer's next full use must re-prefill.
        if (shortSlice) st->bufNeedsPrefill[slot] = true;
        return true;
    }
    std::memset(st->ring[slot], 0, static_cast<size_t>(st->rowsPerBuf) * st->ringRowBytes);
    st->bufNeedsPrefill[slot] = true;   // zero-filled: constants gone until re-prefilled
    if (sliceIdx == st->nSlices)
        st->encode(st->encodeUser, st->ring[slot], st->totalRows, 0,
                   /*closeFrame=*/true, /*needsPrefill=*/false);
    return false;
}

// The barrier between priming and draining. Completion is written-gated, so it leads the wire's actual drain by most of the frame.
// A lapping frame's LAST slices live in its FIRST buffers, exactly the ones the next prime rewrites first, so priming early repaints the bottom rows and no counter sees it.
// Holding the prime until the deterministic wire end closes that race at its only entry point, waiting nothing in the common case.
// Do not instead gate completion on the drain, which deadlocks: a frontier-halted engine fires no completing interrupt.
void waitWireDrained(MoonI80State* st) {
    if (st->lastStopUs == 0) return;   // first-ever frame: no prior wire to drain
    const int64_t now = esp_timer_get_time();
    if (now < st->lastStopUs) esp_rom_delay_us(static_cast<uint32_t>(st->lastStopUs - now));
}

// Prime a range of buffers, each independent, so two cores can prime disjoint ranges concurrently. No latch pad, the reset coming from the stop; each caller takes the wire barrier itself, which is idempotent in parallel and keeps the barrier a per-call contract.
void primeRingRange(MoonI80State* st, uint8_t bufLo, uint8_t bufHi) {
    waitWireDrained(st);
    if (bufHi > st->ringBufs) bufHi = st->ringBufs;
    for (uint8_t primed = bufLo; primed < bufHi; primed++)
        fillSlice(st, primed, primed);   // first lap: buffer index IS the slice index
}

// Arm the primed ring: peripheral setup, the timed WS2812-reset guard, the oracle epoch, gdma_start. Every buffer must be primed (primeRingRange over the whole pool, serial or fork-joined) BEFORE this runs; the caller owns that ordering (the driver's join is the fence).
bool armRingTransfer(MoonI80State* st) {
    lcd_cam_dev_t* dev = st->hal.dev;
    st->drainCount = 0;
    st->busy = true;
    // Priming wrote slices 0..ringBufs-1 (real ones encoded, the rest zero-filled), the batch refill's cursor continues from there.
    st->lastWrittenSlice = st->ringBufs - 1u;

    // When the frame laps, rebuild the chain linear and terminate it at the prime frontier before every arm, so it ends exactly where written data ends.
    // Re-linking each frame rather than once at mount is what lets the terminator travel: the previous frame left it wherever its last refill put it.
    // A chain that never laps is self-terminated at mount and needs no re-link.
    if (st->nSlices > st->ringBufs) {
        // The previous frame's DMA halted itself at the frontier NULL; force it fully stopped before we rewrite its descriptors so no in-flight prefetch reads a half-edited link. gdma_start(head) below re-arms from the head cleanly. (First arm: the channel is idle already, a no-op stop.)
        gdma_stop(st->dma);
        for (uint8_t b = 0; b + 1u < st->ringBufs; b++)
            gdma_link_concat(st->link, ringTailNode(st, b), st->link, st->bufLastNode[b + 1u]);
        gdma_link_concat(st->link, ringTailNode(st, st->ringBufs - 1u), nullptr, 0);   // frontier NULL
    }

    lcd_ll_set_phase_cycles(dev, /*cmd=*/0, /*dummy=*/0, /*data=*/1);
    lcd_ll_set_blank_cycles(dev, 1, 1);
    lcd_ll_reset(dev);
    lcd_ll_fifo_reset(dev);

    // Guarantee the reset by holding the arm until the strand has idled long enough since the last frame stopped.
    // At normal rates the render loop's own gap already exceeds it, so this waits nothing in the common case and busy-waits only the remainder on back-to-back frames.
    // Sizing the reset by TIME rather than by tail-buffer count is what makes a small pool render at all.
    if (st->lastStopUs != 0) {
        const int64_t lowSoFar = esp_timer_get_time() - st->lastStopUs;
        if (lowSoFar < kResetLowUs) esp_rom_delay_us(static_cast<uint32_t>(kResetLowUs - lowSoFar));
    }

    st->txStartUs[0] = esp_timer_get_time();
    st->armUs = st->txStartUs[0];   // the clock oracle's epoch: drain position = (now − armUs) / sliceNs
    if (gdma_start(st->dma, gdma_link_get_head_addr(st->link)) != ESP_OK) {
        st->busy = false;
        return false;
    }
    esp_rom_delay_us(1);
    lcd_ll_start(dev);
    return true;
}

// The serial combo (prime everything, then arm), the single-core path, and what the dual-core driver falls back to when its helper is unavailable.
bool startRingTransfer(MoonI80State* st) {
    primeRingRange(st, 0, st->ringBufs);
    return armRingTransfer(st);
}


// GDMA channel + the link list the ring circulates. Mirrors initDma (channel alloc / connect / strategy / transfer / EOF callback); the chain itself is described where it is mounted, in createRingState.
bool initRingDma(MoonI80State* st) {
    gdma_channel_alloc_config_t chanCfg = {};
    // The deadline race runs at interrupt-dispatch granularity, so this interrupt takes a priority no render-thread interrupt can delay, and a cache-safe registration so a flash write delays nothing. The whole handler chain is resident in instruction memory, which is what makes that registration legal.
    chanCfg.intr_priority = 3;
    chanCfg.flags.isr_cache_safe = true;
#if defined(SOC_GDMA_BUS_AXI) && (SOC_GDMA_TRIG_PERIPH_LCD0_BUS == SOC_GDMA_BUS_AXI)
    constexpr size_t kDescAlign = 8;
    if (gdma_new_axi_channel(&chanCfg, &st->dma, nullptr) != ESP_OK) return false;
#else
    constexpr size_t kDescAlign = 4;
    if (gdma_new_ahb_channel(&chanCfg, &st->dma, nullptr) != ESP_OK) return false;
#endif
    if (gdma_connect(st->dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0)) != ESP_OK) return false;

    gdma_strategy_config_t strategy = {};
    // Descriptor write-back stays off: @xref{descriptor-write-back-stays-off|the polite halt it otherwise causes}. The owner check stays off too, since an owner gate is never wanted here at all.
    strategy.auto_update_desc = false;
    strategy.owner_check = false;
    if (gdma_apply_strategy(st->dma, &strategy) != ESP_OK) return false;

    gdma_transfer_config_t transfer = {};
    transfer.max_data_burst_size = 64;
    transfer.access_ext_mem = false;   // ring buffers are INTERNAL — the whole point (no PSRAM at the shift clock)
    if (gdma_config_transfer(st->dma, &transfer) != ESP_OK) return false;

    size_t intAlign = 0, extAlign = 0;
    if (gdma_get_alignment_constraints(st->dma, &intAlign, &extAlign) != ESP_OK) return false;

    st->nSlices = (st->totalRows + st->rowsPerBuf - 1) / st->rowsPerBuf;

    // The frontier-terminated chain: @xref{the-frontier-terminated-chain|why it is not a loop, and what every node carries}.
    const size_t rowsOnlyBytes = static_cast<size_t>(st->rowsPerBuf) * st->ringRowBytes;
    const size_t itemsPerBuf = esp_dma_calculate_node_count(rowsOnlyBytes, intAlign, kDmaNodeMaxBytes);
    st->itemsPerBuf = static_cast<uint8_t>(itemsPerBuf);   // the ISR splices the self-terminating NULL by node index
    // One extra node per buffer when the inter-buffer zero-pad is on (each pad node re-mounts the SAME shared zero block; zeroPadBytes ≤ kDmaNodeMaxBytes by the kRingPadMaxUs bound, so one node each).
    const size_t padItems = st->zeroPad ? st->ringBufs : 0;
    const size_t numItems = itemsPerBuf * st->ringBufs + padItems;

    st->linkItemCap = numItems;

    gdma_link_list_config_t linkCfg = {};
    linkCfg.item_alignment = kDescAlign;
    linkCfg.num_items = numItems;
    linkCfg.flags.check_owner = false;
    if (gdma_new_link_list(&linkCfg, &st->link) != ESP_OK) return false;

    gdma_tx_event_callbacks_t cbs = {};
    cbs.on_trans_eof = moonI80EofCb;
    cbs.on_descr_err = moonI80DescErrCb;   // B1-DISCRIMINATOR (diagnostic): catch the silent descriptor-fetch halt
    return gdma_register_tx_event_callbacks(st->dma, &cbs, st) == ESP_OK;
}

// Bring up the peripheral + N internal ring buffers + the linear chain. The peripheral setup mirrors createState (same clock, GPIO routing, DC/WR handling), only the DMA + buffers differ. Returns null (and the caller falls back to the whole-frame path) if any internal buffer or the chain won't fit.
MoonI80State* createRingState(const uint16_t* dataPins, uint8_t laneCount, uint16_t wrGpio,
                              size_t rowBytes, uint32_t totalRows,
                              uint32_t rowsPerBuf, uint8_t ringBufs, uint8_t padUs,
                              uint8_t clockMultiplier, MoonI80EncodeFn encode, void* user) {
    auto* st = new (std::nothrow) MoonI80State();
    if (!st) return nullptr;
    st->isRing = true;
    st->busWidth = laneCount <= 8 ? 8 : 16;
    st->ringRowBytes = rowBytes;
    st->totalRows = totalRows;
    st->padUs = padUs;
    // One buffer is one descriptor node, a structural rule enforced here: @xref{one-buffer-is-one-descriptor-node|what spanning several broke}. The clamp has a floor of one row, since a single row larger than a node cannot ring at all and the driver falls back to whole-frame.
    const uint32_t maxRowsPerNode = rowBytes ? static_cast<uint32_t>(kDmaNodeMaxBytes / rowBytes) : 1u;
    const uint32_t rowsClamped = rowsPerBuf > maxRowsPerNode ? (maxRowsPerNode ? maxRowsPerNode : 1u)
                                                             : rowsPerBuf;
    // The geometry, before initRingDma, it derives nSlices and the descriptor-pool size from both.
    st->rowsPerBuf = rowsClamped;
    st->ringBufs = ringBufs;

    st->encode = encode;
    st->encodeUser = user;
    st->cap = st->rowsPerBuf * rowBytes;   // reported buffer capacity (one ring buffer — ROWS ONLY, no pad)

    // Shift mode: the '595 SRCLK = the 80 MHz bus resolution / the runtime shiftClockDiv (default 4 = 20 MHz). initPeripheral recomputes the exact prescale from the clock tree, so this only needs to be the intended rate. Direct mode is unaffected (kPclkHz).
    const uint32_t pclkHz = (clockMultiplier > 1) ? (kShiftBusResolutionHz / g_shiftClockDiv) : kPclkHz;
    // The clock oracle's tick: one slice's exact wire duration in ns. The bus clocks busWidth/8 bytes per pclk, so ns = bytes × 8e9 / (busWidth × pclkHz); the inter-buffer pad (below) extends every slice's slot by padUs. Exact integer math, the oracle's only error source is esp_timer resolution (µs).
    const uint64_t sliceWireNs = (static_cast<uint64_t>(st->rowsPerBuf) * rowBytes * 8u * 1'000'000'000ull)
                                 / (static_cast<uint64_t>(st->busWidth) * pclkHz);
    st->sliceNs = static_cast<uint32_t>(sliceWireNs);   // + the pad below, once it is actually mounted
    // The geometry initRingDma sizes the pool from, computed here too because the pad decision needs it.
    st->nSlices = (st->totalRows + st->rowsPerBuf - 1u) / st->rowsPerBuf;
    // The shared pad block, used only where there is a refill deadline to stretch: one block every pad node references, sized in bus time and aligned. Zeros on the expander bus read as a strand-level pause, and the bound keeps that pause far under the latch threshold.
    if (padUs > 0 && st->nSlices > st->ringBufs) {
        const uint64_t padBytes64 = (static_cast<uint64_t>(padUs) * pclkHz * st->busWidth) / (8u * 1'000'000ull);
        st->zeroPadBytes = static_cast<size_t>(padBytes64) & ~size_t{7};
        if (st->zeroPadBytes > 0) {
            st->zeroPad = allocFrame(st, st->zeroPadBytes, /*psram=*/false);
            if (!st->zeroPad) {   // pad requested but unfittable: fail loud, caller falls back
                ESP_LOGE(MOON_I80_TAG, "ring init failed: zero-pad alloc (%u B)", (unsigned)st->zeroPadBytes);
                destroyState(st);
                return nullptr;
            }
            // The oracle's tick includes the pad's EXACT wire time (the mounted bytes, not the requested µs, they differ by the alignment round-down).
            st->sliceNs += static_cast<uint32_t>(
                (static_cast<uint64_t>(st->zeroPadBytes) * 8u * 1'000'000'000ull)
                / (static_cast<uint64_t>(st->busWidth) * pclkHz));
        } else {
            st->zeroPadBytes = 0;
        }
    }
    if (!initPeripheral(st, pclkHz)) {
        ESP_LOGE(MOON_I80_TAG, "ring init failed: peripheral (LCD_CAM) setup");
        destroyState(st);
        return nullptr;
    }
    if (!initRingDma(st)) {
        ESP_LOGE(MOON_I80_TAG, "ring init failed: GDMA channel/descriptor setup");
        destroyState(st);
        return nullptr;
    }
    configureGpio(st, dataPins, laneCount, wrGpio, /*routeWr=*/clockMultiplier > 1);

    st->done[0] = xSemaphoreCreateBinary();   // the render thread's frame-complete wait (given on the last slice)
    if (!st->done[0]) {
        ESP_LOGE(MOON_I80_TAG, "ring init failed: done semaphore");
        destroyState(st);
        return nullptr;
    }

    // N internal ring buffers, each one slice + the latch pad (the LAST slice appends the pad; sizing every buffer to hold it keeps them uniform and lets any buffer be the last one).
    const size_t bufBytes = static_cast<size_t>(st->rowsPerBuf) * rowBytes;
    for (uint8_t i = 0; i < st->ringBufs; i++) {
        st->ring[i] = allocFrame(st, bufBytes, /*psram=*/false);
        if (!st->ring[i]) {
            ESP_LOGE(MOON_I80_TAG, "ring init failed: buffer %u/%u alloc (%u B, free DMA %u B)",
                     (unsigned)i, (unsigned)st->ringBufs, (unsigned)bufBytes,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            destroyState(st);
            return nullptr;
        }
        st->bufNeedsPrefill[i] = true;   // fresh (zeroed) buffer: no constants laid yet
    }
    // Cache-line size is a per-pool constant (all buffers come from the same internal-DMA heap), hoisted here so the refill never makes the (flash-resident) query inside the now cache-safe ISR.
    st->ringCacheLine = esp_cache_get_line_size_by_addr(st->ring[0]);

    // Mount the chain, self-terminating from creation rather than spliced at runtime, which raced the still-walking engine and was index-fragile.
    // A geometry whose frame fits the pool has a fixed length and a fixed terminator, the zero tail past the last real slice, so re-arming is a plain start.
    // One that laps can have no fixed terminator, so it is mounted linear and the arm plants the frontier: @xref{the-frontier-terminated-chain|the rule}.
    const bool primeOnly = st->nSlices <= st->ringBufs;
    const uint32_t termBuf = primeOnly
        ? (st->nSlices < st->ringBufs ? st->nSlices : st->ringBufs - 1u)   // the zero reset-tail buffer
        : st->ringBufs;   // sentinel "none" for the lapping case (no buffer gets LINK_TO_NULL)
    const size_t rowsOnly = static_cast<size_t>(st->rowsPerBuf) * rowBytes;   // node length: rows, NO pad
    // Mount up to and including the terminator, then stop: each mount re-links the previous node to the one it mounts.
    // So mounting one more would overwrite the end we just set and the chain would loop forever.
    // The engine never reaches buffers past the terminator, so leaving them unmounted is correct.
    const uint8_t mountCount = primeOnly ? static_cast<uint8_t>(termBuf + 1u) : st->ringBufs;
    // With a pad, one pad node is interleaved after every data node, each re-mounting the SAME shared zero block, which is only more mount calls.
    // The pad reads as a strand-level pause after each slice, stretching the refill deadline by its wire time.
    // The end mark stays on the data nodes, whose drain is the one that needs an interrupt.
    const bool padded = !primeOnly && st->zeroPad != nullptr;
    int idx = 0;
    bool mountOk = true;
    for (uint8_t b = 0; b < mountCount && mountOk; b++) {
        const bool last = (b == mountCount - 1);
        gdma_buffer_mount_config_t mount = {};
        mount.buffer = st->ring[b];
        mount.length = rowsOnly;   // rows only — continuous stream, no inter-buffer LOW gap (see initRingDma)
        // When the whole frame is primed before arming, the interrupt fires ONCE per frame, on the terminator alone.
        // Counting per-buffer completions would be unsound, the status being a latch: two coalesce while the handler is delayed and the count undercounts.
        // Completion then never fires and the driver gives up, measured as every large configuration dying within a few dozen frames.
        // A lapping geometry keeps per-buffer interrupts, its handler genuinely refilling per drain.
        mount.flags.mark_eof = primeOnly ? (b == termBuf) : true;
        // Prime-only: the reset-tail buffer self-terminates (NULL); every other node → next (DEFAULT).
        // Lapping: every node → next (DEFAULT). The lapping chain never loops to HEAD, armRingTransfer re-links it linear and plants the frontier NULL at the prime edge, and the ISR advances that NULL.
        // A DEFAULT link on the last mounted node is a don't-care: armRingTransfer overwrites it before arm.
        mount.flags.mark_final = (b == termBuf) ? GDMA_FINAL_LINK_TO_NULL : GDMA_FINAL_LINK_TO_DEFAULT;
        int endIdx = 0;
        if (idx >= static_cast<int>(st->linkItemCap)) mountOk = false;
        else if (gdma_link_mount_buffers(st->link, idx, &mount, 1, &endIdx) != ESP_OK) mountOk = false;
        else st->bufLastNode[b] = endIdx;   // buffer b's real last node (kept for the lapping ISR splice)
        idx = endIdx + 1;
        if (padded && mountOk) {
            gdma_buffer_mount_config_t pad = {};
            pad.buffer = st->zeroPad;
            pad.length = st->zeroPadBytes;
            pad.flags.mark_eof = false;   // the pad's drain is dead time; the data node's EOF drives the ISR
            // Every pad node → next (DEFAULT). The last one is a don't-care too, armRingTransfer re-links the chain (through each buffer's tail node = its pad, ringTailNode) and plants the frontier.
            pad.flags.mark_final = GDMA_FINAL_LINK_TO_DEFAULT;
            (void)last;
            int padEnd = 0;
            if (idx >= static_cast<int>(st->linkItemCap)) mountOk = false;
            else if (gdma_link_mount_buffers(st->link, idx, &pad, 1, &padEnd) != ESP_OK) mountOk = false;
            idx = padEnd + 1;
        }
    }
    st->consumedItems = static_cast<uint32_t>(idx);   // diagnostic: exposed via moonI80Ws2812RingStats
    // SELF-TERM DIAG (temp): the actual mount-time NULL terminator node, for the ringDbg readout.
    st->termNode = primeOnly ? st->bufLastNode[termBuf] : -1;
    if (!mountOk) { destroyState(st); return nullptr; }

    // No refill task: the EOF ISR encodes the next slice inline as each buffer drains (see moonI80EofCb). That is the reuse-race fix, an interrupt-priority refill always beats the DMA lapping into a reused buffer, which a lower-priority task could lose to wake latency at >8 slices (≥192 lights/strand).
    return st;
}

// Abandon the in-flight transfer and return the peripheral to a clean idle, the one recovery both backstops share.
// A lost interrupt leaves the busy flag stuck with nothing to clear it, and the bus then wedges permanently.
// So this stops the peripheral and the engine, clears the flag, and marks the strand as idling now, holding the next arm's reset window.
// The condition and any mode-specific residue stay at the call sites, so the two paths cannot drift in how they leave the hardware.
void finalizeStalledTransfer(MoonI80State* st) {
    lcd_ll_stop(st->hal.dev);
    gdma_stop(st->dma);
    st->busy = false;
    st->lastStopUs = esp_timer_get_time();
    st->dbgStallAbandons = st->dbgStallAbandons + 1u;
}

} // namespace

bool moonI80Ws2812Init(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                       uint16_t wrGpio, size_t bufferBytes,
                       bool wantSecondBuffer, uint8_t clockMultiplier) {
    if (!dataPins || laneCount == 0 || bufferBytes == 0 || clockMultiplier == 0) return false;
    // Pre-check that the frame can land somewhere before touching the peripheral, which is fine when either region fits, the reserve guarding only internal memory.
    // The external query uses that capability alone, no registered heap carrying both it and the transfer one.
    // A combined query would report nothing even where the engine reaches it perfectly well.
    // Largest block rather than total free: @xref{largest-block-never-total-free|the bug the total caused}.
    const bool fitsInternal =
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
            >= bufferBytes + HEAP_RESERVE;
    const bool fitsPsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= bufferBytes;
    if (!fitsInternal && !fitsPsram) return false;
    MoonI80State* st = createState(dataPins, laneCount, wrGpio, bufferBytes,
                                   wantSecondBuffer, clockMultiplier);
    if (!st) return false;
    h.impl = st;
    return true;
}

bool moonI80Ws2812InitRing(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                           uint16_t wrGpio, size_t rowBytes, uint32_t totalRows,
                           uint32_t rowsPerBuf, uint8_t ringBufs, uint8_t padUs,
                           uint8_t clockMultiplier, MoonI80EncodeFn encode, void* user) {
    if (!dataPins || laneCount == 0 || rowBytes == 0 || totalRows == 0 || clockMultiplier == 0
        || !encode) return false;
    // The geometry is a user control, so clamp it here rather than trusting the caller: a 0 would divide by zero in the nSlices math, and a depth past the array bound would overrun `ring[]`. Depth 2 is the hard floor (see kRingBufsMax's note: IDF's 2-buffer bounce scheme relies on an owner gate this chain does not run).
    if (rowsPerBuf == 0 || ringBufs < 2 || ringBufs > kRingBufsMax) {
        ESP_LOGE(MOON_I80_TAG, "ring init rejected: rowsPerBuf=%u ringBufs=%u (bounds %u..%u)",
                 (unsigned)rowsPerBuf, (unsigned)ringBufs, (unsigned)kRingBufsMin, (unsigned)kRingBufsMax);
        return false;
    }
    if (padUs > kRingPadMaxUs) padUs = kRingPadMaxUs;   // the latch-threshold bound (see platform.h)
    // The pool must fit internal memory while leaving the reserve, which is the whole reason the ring exists.
    // Rows only: a pad inside a buffer would be allocated, encoded, synced and never clocked, and sizing every buffer with one pushed the pool past the heap.
    // Free SIZE here rather than the largest block, the ring making many small allocations: @xref{largest-block-never-total-free|the distinction}.
    // The row clamp is applied before the fit arithmetic, so the pre-check prices the geometry actually built.
    const uint32_t maxRowsPerNode = static_cast<uint32_t>(kDmaNodeMaxBytes / rowBytes);
    const uint32_t rowsEffective = rowsPerBuf > maxRowsPerNode ? (maxRowsPerNode ? maxRowsPerNode : 1u)
                                                              : rowsPerBuf;
    const size_t bufBytes = static_cast<size_t>(rowsEffective) * rowBytes;
    // The pad block is ~3.5 KB at the ceiling, priced in so a pad-tight heap fails here, not mid-create.
    const size_t padBytes = padUs ? (static_cast<size_t>(padUs) * 27u * 2u) : 0;   // upper bound, 16-bit bus
    const size_t need = bufBytes * ringBufs + padBytes + HEAP_RESERVE;
    const size_t freeDma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (freeDma < need) {   // fall back to whole-frame (or the loopback's pool step-down)
        ESP_LOGW(MOON_I80_TAG, "ring init rejected: need %u B internal DMA (bufs=%u x %u B + reserve), free %u B",
                 (unsigned)need, (unsigned)ringBufs, (unsigned)bufBytes, (unsigned)freeDma);
        return false;
    }
    MoonI80State* st = createRingState(dataPins, laneCount, wrGpio, rowBytes, totalRows,
                                       rowsEffective, ringBufs, padUs, clockMultiplier, encode, user);
    if (!st) return false;
    h.impl = st;
    return true;
}

// Set the expander's shift-clock divider for the NEXT init: @xref{the-shift-clock-window|the three settings and their wall verdicts}. It takes effect on the next bus rebuild, which the driver's own control triggers, and is clamped to the valid range.
void moonI80SetShiftClockDiv(uint8_t div) {
    if (div < 3) div = 3;                      // 80/3 = 26.67 MHz is the fastest in-spec-T0H rate
    if (div > LCD_LL_PCLK_DIV_MAX) div = LCD_LL_PCLK_DIV_MAX;
    g_shiftClockDiv = div;
}

void moonI80Ws2812PrimeRange(MoonI80Ws2812Handle& h, uint8_t bufLo, uint8_t bufHi) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || !st->isRing || st->busy) return;   // never prime under a live transfer
    primeRingRange(st, bufLo, bufHi);
}

bool moonI80Ws2812ArmRing(MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || !st->isRing || st->busy) return false;
    return armRingTransfer(st);
}

bool moonI80Ws2812TransmitRing(MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || !st->isRing) return false;
    // Serial on the wire like the whole-frame path: a strand receives one frame at a time. If a previous ring frame is still clocking out, its completion gives done[0]; the driver waits on slot 0 before calling here again (tickRing), so busy should already be clear, guard anyway.
    if (st->busy) return false;
    return startRingTransfer(st);
}

bool moonI80Ws2812IsRing(const MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    return st && st->isRing;
}

bool moonI80Ws2812InternalFits(size_t bytes) {
    // Does a whole frame fit internal memory as ONE contiguous block? @xref{largest-block-never-total-free|why the total is the wrong question}.
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t freeTotal = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    return largest >= bytes && freeTotal >= bytes + HEAP_RESERVE;
}

uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle& h, uint8_t buffer) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st) return nullptr;
    // A ring handle has no whole-frame buf[]; the domain never encodes into a ring buffer itself (the platform's refill task does, via the encode seam). But the base uses busBuffer(0) as its non-null "inited" sentinel (dmaBuf_) and busBuffer(1) to detect double-buffer mode. So report ring[0] for slot 0 (a real, non-null pointer → inited) and null for slot 1 (a ring is never the double-buffer).
    if (st->isRing) return buffer == 0 ? st->ring[0] : nullptr;
    return buffer < 2 ? st->buf[buffer] : nullptr;
}

size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    return st ? st->cap : 0;
}

bool moonI80Ws2812Transmit(MoonI80Ws2812Handle& h, uint8_t buffer, size_t bytes) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || buffer >= 2 || !st->buf[buffer] || bytes == 0 || bytes > st->cap) return false;
    // Wait for the wire here rather than refusing a busy bus: @xref{the-wire-is-waited-for-not-refused|why refusing would drop every second frame}. A stale token is drained first, since the previous frame's completion gives one unconditionally and it can sit unconsumed, so the wait below is for THIS frame.
    xSemaphoreTake(st->wireFree, 0);
    if (st->busy) {
        // Block on the dedicated wire-free signal rather than the in-flight buffer's own, which belongs to the driver and would make its wait miss if consumed here. The completion gives both. The bound is a backstop against a wedged peripheral rather than a policy, the driver's own frame-derived timeout governing a stalled bus.
        if (xSemaphoreTake(st->wireFree, pdMS_TO_TICKS(kWireFreeTimeoutMs)) != pdTRUE) return false;
    }

    // Push the buffer onto the completion FIFO BEFORE starting the hardware, so a fast EOF (which pops it) can never fire before its slot is populated. The push touches slot `fifoHead`; the ISR only ever reads slot `fifoTail`, and with one transfer in flight those are different slots, that disjointness is what makes the push safe without a lock.
    const uint8_t slot = st->fifoHead;
    st->fifo[slot] = buffer;
    // The transfer starts on the wire immediately (we waited above for any predecessor), so stamp the wire-time KPI right here, no deferred stamping is needed, which is a small simplification over the esp_lcd sibling's queued path.
    st->txStartUs[slot] = esp_timer_get_time();
    st->fifoHead = (st->fifoHead + 1u) & 1u;
    st->busy = true;

    if (!startTransfer(st, buffer, bytes)) {
        // Failed to arm, unwind the FIFO push so the ISR count stays balanced. Safe: no transfer started, so no EOF will pop this slot.
        st->fifoHead = (st->fifoHead + 1u) & 1u;
        st->busy = false;
        return false;
    }
    return true;
}

bool moonI80Ws2812Wait(MoonI80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || buffer >= 2 || !st->done[buffer]) return true;   // nothing to wait on = not in flight
    // Report whether the transfer actually completed. On a timeout the DMA may still be reading this buffer, so the caller must keep it marked in-flight rather than re-encoding into it, handing a live DMA a half-rewritten buffer is exactly the frame corruption the timeout is meant to avoid.
    if (xSemaphoreTake(st->done[buffer], pdMS_TO_TICKS(timeoutMs)) == pdTRUE) return true;

    // The ring's stall backstop: @xref{two-stall-backstops-one-recovery|the window that outlasts the pool's lead}. The next arm re-links the chain and plants a fresh frontier, so no residual state carries over.
    if (st->isRing && st->busy && st->nSlices > st->ringBufs
        && st->lastWrittenSlice < st->nSlices + kTailBufs) {   // kTailBufs (file scope), not a bare +1
        const int64_t frameWireUs = (static_cast<int64_t>(st->nSlices) * st->sliceNs) / 1000;
        if (esp_timer_get_time() - st->armUs >= frameWireUs) {
            finalizeStalledTransfer(st);   // the shared stop-and-clear; the next arm re-links the chain
            // Latch whatever this frame's refills managed, so the ea readout isn't stuck at a half window.
            st->dbgEncAvgUs = st->dbgEncCount ? st->dbgEncSumUs / st->dbgEncCount : st->dbgEncAvgUs;
            st->dbgEncSumUs = 0;
            st->dbgEncCount = 0;
        }
    }

    // The whole-frame path's stall backstop: @xref{two-stall-backstops-one-recovery|the lost interrupt this recovers from}. Its own residue is draining the completion queue, so the abandoned entry cannot be popped by a late interrupt against the next frame.
    if (!st->isRing && st->busy) {
        finalizeStalledTransfer(st);   // stop LCD + GDMA FIRST, so no EOF can fire during the drain below
        st->fifoTail = st->fifoHead;   // then drop the un-completed entry; the ISR guard ignores a late EOF
    }
    return false;   // the caller keeps the buffer in-flight for this frame; the next frame arms fresh
}

uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    return st ? st->lastTransmitUs : 0;
}

MoonI80RingStats moonI80Ws2812RingStats(const MoonI80Ws2812Handle& h) {
    MoonI80RingStats s;
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || !st->isRing) return s;
    s.isRing        = true;
    s.nSlices       = st->nSlices;
    s.ringBufs      = st->ringBufs;
    s.eofTotal      = st->dbgEofTotal;
    s.cacheOffDefers = st->dbgCacheOffDefers;
    s.cacheOffMaxRun = st->dbgCacheOffMaxRun;
    s.stallAbandons = st->dbgStallAbandons;
    s.doneGiven     = st->dbgDoneGiven;
    s.lastDrain     = st->dbgLastDrain;
    s.numItems      = static_cast<uint32_t>(st->linkItemCap);
    s.consumedItems = st->consumedItems;
    s.descErr       = st->dbgDescErr;
    s.maxEncodeUs   = st->dbgMaxEncodeUs;
    s.avgEncodeUs   = st->dbgEncAvgUs;
    s.maxIsrGapUs   = st->dbgMaxIsrGapUs;
    s.itemsPerBuf   = st->itemsPerBuf;
    s.termNodeDiag  = st->termNode;
    s.late          = st->dbgLate;
    return s;
}

void moonI80Ws2812Deinit(MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st) return;
    destroyState(st);
    h.impl = nullptr;
}

// The loopback self-test: a private full-width setup on the driver's real pins transmits the caller's real frame, exactly like the render loop.
// A receive channel captures it off the jumpered pin and verifies every bit.
// A short synthetic burst would miss exactly the failures a real frame hits, so the test sends the genuine article.
// The capture and verify half is shared with the sibling loopbacks; only the transmit differs.

namespace detail {
void captureAndVerifyFrame(uint16_t rxGpio, size_t frameBytes, size_t dataBytes,
                           uint8_t rowBits, uint32_t pclkHz, bool pinExpanderMode, const char* tag,
                           const std::function<void()>& transmitOnce,
                           RmtLoopbackResult& r, bool rideMode = false,
                           uint32_t* rxSymbols = nullptr);
// Pre-allocate the capture buffer, one contiguous internal block, so a caller can take it before its own allocations fragment the heap. Ownership transfers regardless of outcome, and passing nothing on failure is fine: the helper retries and reports it.
uint32_t* allocLoopbackCapture(size_t dataBytes);
}

RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                        uint16_t wrGpio, uint16_t rxGpio,
                                        const uint8_t* frame, size_t frameBytes,
                                        size_t dataBytes, uint8_t rowBits,
                                        uint8_t clockMultiplier,
                                        uint32_t ringRows, uint32_t ringBufs,
                                        bool useRingArg) {
    RmtLoopbackResult r = detail::loopbackResult();
    if (!detail::loopbackFrameValid(dataPins, laneCount, frame, frameBytes, dataBytes, rowBits) || clockMultiplier == 0)
        return r;
    const uint16_t txGpio = dataPins[0];   // lane 0 carries the pattern
    const bool pinExpanderMode = clockMultiplier > 1;

    if (pinExpanderMode) {
        // Skip the continuity pre-check, which expects the receive pin to follow the transmit one directly.
        // True of a bare jumper and false through an expander, where raising the input raises no output until a latch.
        // It would report no jumper on perfectly good wiring, and the bit-verify is the stronger check anyway, validating the whole chain.
        r.jumperDetected = true;
    } else {
        r.jumperDetected = detail::loopbackJumperOk(static_cast<uint8_t>(txGpio),
                                                    static_cast<uint8_t>(rxGpio));
        if (!r.jumperDetected) return r;
    }

    // The loopback follows the same rule the render path does: @xref{the-loopback-follows-the-same-rule-the-render-path-does|why, and what its encode becomes}.
    // It rides the ring when the driver asked for it, the render path being on the ring so the self-test must be too.
    // Or when the frame would overflow internal memory.
    const bool useRing = pinExpanderMode && (useRingArg || !moonI80Ws2812InternalFits(frameBytes));
    const uint8_t sb = laneCount <= 8 ? 1 : 2;
    // rowBytes = outCh(=rowBits/8) × 8 × 3 × slotBytes × outputsPerPin(=clockMultiplier in shift mode).
    const size_t loopRowBytes = static_cast<size_t>(rowBits) * 3u * sb * clockMultiplier;
    // The row count is the strand's light count and must come from strand-side units: @xref{the-loopback-follows-the-same-rule-the-render-path-does|why bus bytes mix units}.
    const uint32_t rowStrandBytes = static_cast<uint32_t>(rowBits) * 3u;
    const uint32_t loopRows = rowStrandBytes ? static_cast<uint32_t>(dataBytes / rowStrandBytes) : 0;

    // Grab the capture buffer FIRST: it is the loopback's one big contiguous DMA block (~4 B per WS2812 bit), and the ring init below fragments internal RAM with its per-buffer nodes, in that order the capture alloc fails on a busy heap and the test dies as "no capture" before transmitting a bit. Largest-first fixes it; ownership passes to captureAndVerifyFrame.
    uint32_t* rxSymbols = detail::allocLoopbackCapture(dataBytes);

    MoonI80State* st = nullptr;
    // The copy-slice encoder: the frame is already encoded, so a slice is a straight copy out of it, its geometry riding in the seam's own user pointer. Rows only, like every ring slice, so the pre-built frame's trailing pad is not copied: a ring buffer holds rows and nothing else, and the pad has nowhere to go.
    struct LoopCopyCtx { const uint8_t* frame; size_t rowBytes; uint32_t rows; size_t slotBytes; };
    LoopCopyCtx ctx{frame, loopRowBytes, loopRows, sb};
    if (useRing && loopRows > 0) {
        auto copySlice = [](void* user, uint8_t* dst, uint32_t firstRow, uint32_t count, bool close,
                            bool /*needsPrefill*/) {   // a full memcpy re-writes constants + data alike
            auto* c = static_cast<LoopCopyCtx*>(user);
            if (count == 0) {
                // The frame-close call: the pre-built frame's pad HEAD is exactly the latch-only word (encodeWs2812ShiftLatchPad wrote it there), copy that one word.
                if (close) std::memcpy(dst, c->frame + static_cast<size_t>(c->rows) * c->rowBytes, c->slotBytes);
                return;
            }
            std::memcpy(dst, c->frame + static_cast<size_t>(firstRow) * c->rowBytes,
                        static_cast<size_t>(count) * c->rowBytes);
        };
        MoonI80Ws2812Handle h;
        // Prefer the driver's LIVE geometry, so the self-test streams through the same ring the render path is tuned to.
        // A scattered margin then shows as a bit fault at the same slice boundary the eyes see on the wall.
        // Zero takes the platform default, so a caller that does not care still works, and the depth steps down on a memory-tight heap.
        const uint32_t loopRingRows = ringRows ? ringRows : kRingRowsDefault;
        // The live depth when memory allows, else stepping down until the init's gate accepts.
        // The capture buffer already holds several bytes per bit of the same pool, and both do not fit a tight board.
        // A shallower ring still verifies the same chain, only the refill margin differing.
        // The copy refill is far faster than a render encode, so even a minimal pool streams clean.
        uint32_t tryBufs = ringBufs ? ringBufs : kRingBufsDefault;
        while (tryBufs >= kRingBufsMin) {
            if (moonI80Ws2812InitRing(h, dataPins, laneCount, wrGpio, loopRowBytes, loopRows,
                                      loopRingRows, tryBufs, /*padUs=*/0, clockMultiplier, copySlice,
                                      &ctx)) {
                st = static_cast<MoonI80State*>(h.impl);
                break;
            }
            tryBufs /= 2;
        }
    }
    // Whole-frame fallback (direct mode, or a frame that fits internal): the original private bus + copy.
    if (!st) {
        // The continuity check above reset txGpio's GPIO matrix route; createState re-claims it.
        st = createState(dataPins, laneCount, wrGpio, frameBytes, /*wantSecond=*/false, clockMultiplier);
        if (!st) {
            ESP_LOGE(MOON_I80_TAG, "loopback: private peripheral setup failed");
            heap_caps_free(rxSymbols);   // ownership never reached captureAndVerifyFrame
            return r;
        }
        std::memcpy(st->buf[0], frame, frameBytes);   // loopback uses buffer 0 only (single transfer)
    }
    const bool loopIsRing = st->isRing;

    // Ship one frame and wait for its completion, everything else being the shared helper. This is the runtime path's bookkeeping minus the handle indirection, surfacing a failed arm or a timeout rather than letting either show up only as a later capture mismatch.
    auto transmitOnce = [st, frameBytes, loopIsRing]() {
        bool armed;
        if (loopIsRing) {
            armed = startRingTransfer(st);
        } else {
            st->fifo[st->fifoHead] = 0;
            st->txStartUs[st->fifoHead] = esp_timer_get_time();
            st->fifoHead = (st->fifoHead + 1u) & 1u;
            st->busy = true;
            armed = startTransfer(st, 0, frameBytes);
            if (!armed) {
                st->fifoHead = (st->fifoHead + 1u) & 1u;   // unwind the push
                st->busy = false;
            }
        }
        if (!armed) { ESP_LOGE(MOON_I80_TAG, "loopback: tx arm failed"); return; }
        if (xSemaphoreTake(st->done[0], pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(MOON_I80_TAG, "loopback: tx EOF timed out");
    };
    // The rate passed here is the SLOT rate the strand sees, which sets the verifier's pulse threshold and expected duration.
    // So it must describe the strand rather than the bus: in expander mode several bus words fill one slot, so the rate divides by that multiplier.
    // Passing the bus rate sized the window far too short and decoded nothing on a working strand.
    // It is derived from the runtime divider rather than a constant, or a non-default one makes the capture expect the wrong widths.
    const uint32_t slotHz = pinExpanderMode
        ? (kShiftBusResolutionHz / g_shiftClockDiv / clockMultiplier) : kPclkHz;
    detail::captureAndVerifyFrame(rxGpio, frameBytes, dataBytes, rowBits, slotHz, pinExpanderMode,
                                  MOON_I80_TAG, transmitOnce, r, /*rideMode=*/false, rxSymbols);
    destroyState(st);
    return r;
}

} // namespace mm::platform

#else  // !SOC_LCDCAM_I80_LCD_SUPPORTED — inert stubs so a chip without LCD_CAM links

namespace mm::platform {

bool moonI80Ws2812Init(MoonI80Ws2812Handle&, const uint16_t*, uint8_t, uint16_t, uint16_t,
                       size_t, bool, uint8_t) {
    return false;
}
bool moonI80Ws2812InitRing(MoonI80Ws2812Handle&, const uint16_t*, uint8_t, uint16_t, size_t,
                           uint32_t, uint32_t, uint8_t, uint8_t, uint8_t, MoonI80EncodeFn, void*) {
    return false;
}
bool moonI80Ws2812TransmitRing(MoonI80Ws2812Handle&) { return false; }
void moonI80SetShiftClockDiv(uint8_t) {}
void moonI80Ws2812PrimeRange(MoonI80Ws2812Handle&, uint8_t, uint8_t) {}
bool moonI80Ws2812ArmRing(MoonI80Ws2812Handle&) { return false; }
bool moonI80Ws2812IsRing(const MoonI80Ws2812Handle&) { return false; }
bool moonI80Ws2812InternalFits(size_t) { return false; }
uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle&, uint8_t) { return nullptr; }
size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle&) { return 0; }
bool moonI80Ws2812Transmit(MoonI80Ws2812Handle&, uint8_t, size_t) { return false; }
bool moonI80Ws2812Wait(MoonI80Ws2812Handle&, uint8_t, uint32_t) { return true; }
uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle&) { return 0; }
MoonI80RingStats moonI80Ws2812RingStats(const MoonI80Ws2812Handle&) { return {}; }
void moonI80Ws2812Deinit(MoonI80Ws2812Handle&) {}
RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t*, uint8_t, uint16_t, uint16_t,
                                        uint16_t, const uint8_t*, size_t, size_t, uint8_t,
                                        uint8_t, uint32_t, uint32_t, bool) {
    return {};
}

} // namespace mm::platform

#endif  // SOC_LCDCAM_I80_LCD_SUPPORTED
