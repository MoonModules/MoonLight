#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared driver body + LedPeripheral
#include "platform/platform.h"

#include <atomic>   // the parallel-snapshot helper join flags

namespace mm {

/// A `LedPeripheral` backend: parallel WS2812B on the LCD_CAM peripheral, driven by our own DMA code. Same peripheral, pins and wire contract as the `i80` backend, differing underneath, and it buys a streamed frame and a 74HCT595 expander driving 8 strands per GPIO.
///
/// The wiring and the peripheral comparison are on the drivers page.
///
/// @attribution hpwit
///
/// @moreinfo
///
/// ## Why our own DMA driver
///
/// `esp_lcd` re-arms the peripheral on every transaction, resetting it mid-stream, which suits an addressed LCD panel. WS2812 is one unbroken self-clocked bit stream, so a whole frame belongs in one contiguous DMA-reachable block, which is the cap this backend exists to lift.
///
/// The LCD peripheral clocks what the DMA feeds it and stops when the chain ends, so one `gdma_start()` over an arbitrarily long descriptor chain is a single gapless stream. Built on IDF's HAL and GDMA APIs, one level below `esp_lcd` rather than at the registers.
///
/// ## What streaming costs
///
/// The whole-frame path runs free once armed; the ring carries a deadline. Its refill runs from the end-of-buffer interrupt and must beat the wire at 28.8 µs per light at 20 MHz, which is why `useRing` is a switch.
///
/// @card MoonI80Peripheral.png
class MoonI80Peripheral : public LedPeripheral {
public:
    // Pins default to UNSET: they are user-soldered, so a default would be a guess.

    /// WR, the pixel clock, needed only by a 74HCT595 expander and unrouted otherwise.
    int8_t clockPin = 10;

    // Whole-frame stays as the A/B reference, not as a fallback the ring degrades into.
    /// Stream the frame as a ring of small internal buffers, or send it whole from one block.
    bool useRing = true;

    /// Derive the ring geometry and write it into the controls, so the user sees real numbers.
    bool ringAuto = true;

    /// Lights per DMA buffer: the ring's grain, and the RAM against interrupt-rate lever.
    uint8_t ringRows = platform::kRingRowsDefault;

    // Depth buys jitter absorption, so only the AVERAGE refill must beat the slice deadline.
    /// How many buffers the DMA circulates; more depth costs more internal DMA RAM.
    uint8_t ringBufs = platform::kRingBufsDefault;

    /// Inter-buffer zero-pad in µs, buying refill deadline at a linear frame-time cost.
    uint8_t ringPadUs = 0;

    // Turn this OFF if any panel scrambles: a loaded '595 cannot always sample that fast.
    /// Overclock the '595 shift clock from 20 MHz to 26.67 MHz, for short-wired rigs.
    bool shiftOverclock = false;

    // Public like every control's backing member: the framework reads it by pointer.
    /// Backing store for the read-only `ringDbg` control, the ring's one-line instrument.
    char ringDbgStr_[176] = "—";

    // --- LedPeripheral descriptors ---

    /// LCD_CAM lanes on this chip; 0 leaves the driver inert through the orchestrator's guards.
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return platform::lcdLanes; }
    // The PIN count stays free: the orchestrator rounds the bus up and parks spare lanes on WR.
    /// The i80 bus width is 8 or 16, which is a hardware fact of the peripheral.
    bool powerOfTwoBus() const override { return true; }
    /// The loopback carries its pattern on lane 0 of a full-width bus, not a private one.
    bool loopbackFullWidth() const override { return true; }
    /// Claims the LcdCam block, which the esp_lcd i80 backend also uses, so only one runs.
    LedHwBlock hwBlock() const override { return LedHwBlock::LcdCam; }
    /// Status text when the bus will not come up, so the cause is on screen not in a log.
    const char* initFailMsg() const override { return "LCD-MM: bus init failed, check pins / memory"; }
    /// The expander needs a streaming backend, so it runs wherever this backend does.
    bool supportsPinExpander() const override { return platform::hasLcdCam; }

    /// No async double-buffer: the two-buffer completion handshake races and wedges the bus.
    bool supportsDoubleBuffer() const override { return false; }

    /// The GPIO the orchestrator pads spare bus lanes with, which only shift mode routes.
    uint16_t clockPinForBus() const override { return static_cast<uint16_t>(clockPin); }
    /// A spare lane needs no pad: this backend routes only the GPIOs it is handed.
    bool spareLanesNeedPad() const override { return false; }

    /// Bind the bus controls; WR is bound always but shown only in expander mode.
    void addBusControls(ControlList& controls) override {
        controls.addPin("clockPin", clockPin);
        controls.setHidden(controls.count() - 1, !owner_->pinExpanderMode());
    }

    // A separate hook so clockPin and latchPin stay together as one wiring pair in the UI.
    /// Bind the output path, the ring's geometry and its instrument.
    void addRingControls(ControlList& controls) override {
        // Placed below the wiring pair it belongs to; the fix for '595 corruption is OFF.
        controls.addControl("shiftOverclock", shiftOverclock);
        controls.setHidden(controls.count() - 1, !owner_->pinExpanderMode());
        controls.setAdvanced(controls.count() - 1);   // a '595 clock tuning knob: expert only
        // This picks the PATH; ringSnapshot tunes how the ring reads its source. They compose.
        controls.addControl("useRing", useRing);
        controls.setHidden(controls.count() - 1, !owner_->pinExpanderMode());
        // Lives on the orchestrator; the control binds it here through the reference accessor.
        controls.addControl("ringSnapshot", owner_->ringSnapshotRef());
        controls.setHidden(controls.count() - 1, !wantsRing());
        // Gating on wantsRing() is safe: it reads plain members, so it resolves before boot wiring.
        controls.addControl("ringAuto", ringAuto);
        controls.setHidden(controls.count() - 1, !wantsRing());
        // Dev tuning: ringAuto derives these for the end user, so the manual knobs are expert-only.
        controls.addControl("ringRows", ringRows, 1, 64);
        controls.setHidden(controls.count() - 1, !wantsRing());
        controls.setAdvanced(controls.count() - 1);
        controls.addControl("ringBufs", ringBufs, platform::kRingBufsMin, platform::kRingBufsMax);
        controls.setHidden(controls.count() - 1, !wantsRing());
        controls.setAdvanced(controls.count() - 1);
        controls.addControl("ringPadUs", ringPadUs, 0, platform::kRingPadMaxUs);
        controls.setHidden(controls.count() - 1, !wantsRing());
        controls.setAdvanced(controls.count() - 1);
        // Diagnosable by polling /api/state, which is more reliable than scraping serial.
        controls.addReadOnly("ringDbg", ringDbgStr_, sizeof(ringDbgStr_));
        controls.setHidden(controls.count() - 1, !wantsRing());
        controls.setAdvanced(controls.count() - 1);
    }

    /// Refresh the ringDbg diagnostic string once a second, leaving it alone off the ring.
    void refreshBusKpi() override {
        const platform::MoonI80RingStats s = platform::moonI80Ws2812RingStats(bus_);
        if (!s.isRing) return;
        // Read-and-clear each window: a free-running sum wraps in ~20 s and garbles the averages.
        const uint32_t segGather = ParallelLedDriver::dbgSegGatherCy;
        const uint32_t segEmit = ParallelLedDriver::dbgSegEmitCy;
        const uint32_t segRows = ParallelLedDriver::dbgSegRows;
        ParallelLedDriver::dbgSegGatherCy = 0;
        ParallelLedDriver::dbgSegEmitCy = 0;
        ParallelLedDriver::dbgSegRows = 0;
        // The ISR reads one of these per byte, so a PSRAM snapshot costs a measured 4 to 8x.
        const uint8_t* snap = owner_->snapshotBuf();
        const Buffer* src = owner_->sourceBuffer();
        const char snapWhere = snap
            ? (platform::ptrIsPsram(snap) ? 'P' : 'I') : '-';
        const char liveWhere = (src && src->data())
            ? (platform::ptrIsPsram(src->data()) ? 'P' : 'I') : '-';
        std::snprintf(ringDbgStr_, sizeof(ringDbgStr_), "sn%c lv%c sl%u/bf%u co%u cr%u ab%u dn%u ld%u lt%u tx%u ipb%u ci%u tn%d de%u enc%u ea%u sg%u se%u tw%u ts%u tp%u gap%u",
                      snapWhere, liveWhere,
                      static_cast<unsigned>(s.nSlices), static_cast<unsigned>(s.ringBufs),
                      static_cast<unsigned>(s.cacheOffDefers), static_cast<unsigned>(s.cacheOffMaxRun),
                      static_cast<unsigned>(s.stallAbandons),
                      static_cast<unsigned>(s.doneGiven), static_cast<unsigned>(s.lastDrain),
                      static_cast<unsigned>(s.late),
                      static_cast<unsigned>(platform::moonI80Ws2812LastTransmitUs(bus_)),
                      static_cast<unsigned>(s.itemsPerBuf), static_cast<unsigned>(s.consumedItems),
                      static_cast<int>(s.termNodeDiag), static_cast<unsigned>(s.descErr),
                      static_cast<unsigned>(s.maxEncodeUs), static_cast<unsigned>(s.avgEncodeUs),
                      static_cast<unsigned>(segRows ? segGather / segRows : 0),   // avg gather cycles/row (last window)
                      static_cast<unsigned>(segRows ? segEmit / segRows : 0),     // avg emit cycles/row (last window)
                      static_cast<unsigned>(ParallelLedDriver::dbgTickWaitUs),   // wire-wait µs
                      static_cast<unsigned>(ParallelLedDriver::dbgTickSnapUs),   // snapshot µs
                      static_cast<unsigned>(ParallelLedDriver::dbgTickPrimeUs),  // prime µs
                      static_cast<unsigned>(s.maxIsrGapUs));
                      // The field legend is on the drivers page; `lt` is the one to watch.
    }
    /// Which controls need the bus rebuilt rather than re-encoded when they change.
    bool busControlTriggersBuild(const char* name) const override {
        return std::strcmp(name, "clockPin") == 0
            || std::strcmp(name, "useRing") == 0      // path switch: rebuild the bus on the new path
            || std::strcmp(name, "ringAuto") == 0     // re-derive (or stop deriving) the geometry
            || std::strcmp(name, "ringRows") == 0     // geometry: buffers are sized and the chain mounted
            || std::strcmp(name, "ringBufs") == 0     // at build time, so a change is a rebuild
            || std::strcmp(name, "ringPadUs") == 0    // the pad is a mounted DMA node: same rebuild
            || std::strcmp(name, "ringSnapshot") == 0  // the snapshot buffer is (de)allocated at build time
            || std::strcmp(name, "shiftOverclock") == 0; // the peripheral clock is set at bus (re)build
    }

    // In direct mode WR never leaves the peripheral, so a shared GPIO is harmless there.
    /// Refuse a bus configuration that cannot work, naming the reason.
    const char* validateBusFatal() const override {
        if (owner_->pinExpanderMode()) {
            // Unset would route WR to GPIO 65535, so reject it before busInit reaches the pad.
            if (clockPin < 0) return "the 74HCT595 expander needs a clockPin (its shift clock)";
            if (owner_->latchPin >= 0 && owner_->latchPin == clockPin)
                return "latchPin is on clockPin (WR) — the latch needs its own GPIO";
        }
        return nullptr;
    }
    /// Refuse a data lane that shares WR's GPIO, which would emit the clock as pixel data.
    const char* validateBusPins(const uint16_t* lanes, uint8_t n) const override {
        if (!owner_->pinExpanderMode()) return nullptr;
        for (uint8_t i = 0; i < n; i++)
            if (lanes[i] == static_cast<uint16_t>(clockPin)) return "a data pin is on clockPin (WR)";
        return nullptr;
    }

    /// Create the bus and its DMA buffers, scaling the pixel clock to the slot's shift width.
    bool busInit(size_t frameBytes, bool /*wantSecondBuffer*/) override {
        platform::moonI80SetShiftClockDiv(shiftOverclock ? 3 : 4);   // ON = 26.67 MHz, OFF = 20 MHz
        // Forced single-buffer here too, so a caller that forgets the gate cannot reach the race.
        return platform::moonI80Ws2812Init(bus_, owner_->busPinList(), owner_->busPinCount(),
                                           static_cast<uint16_t>(clockPin), frameBytes,
                                           /*wantSecondBuffer=*/false, owner_->busClockMultiplier());
    }
    /// Tear the bus down, stopping this backend's own snapshot helper before it goes away.
    void busDeinit() override { stopSnapHelper(); platform::moonI80Ws2812Deinit(bus_); }
    /// The DMA buffer the encoder writes slice `i` into.
    uint8_t* busBuffer(uint8_t i) override { return platform::moonI80Ws2812Buffer(bus_, i); }
    /// How many bytes one DMA buffer holds.
    size_t busCapacity() const override { return platform::moonI80Ws2812BufferCapacity(bus_); }
    /// Start the DMA over buffer `i`, returning false when the bus refuses it.
    bool busTransmit(uint8_t i, size_t bytes) override { return platform::moonI80Ws2812Transmit(bus_, i, bytes); }
    /// Wait out buffer `i`'s transmission, returning false on timeout.
    bool busWait(uint8_t i, uint32_t ms) override { return platform::moonI80Ws2812Wait(bus_, i, ms); }
    /// The measured wire time of the last frame, in µs.
    uint32_t busLastTransmitUs() const override { return platform::moonI80Ws2812LastTransmitUs(bus_); }

    /// Drive a frame and capture the wire back, so the self-test verifies what was emitted.
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) override {
        return platform::moonI80Ws2812Loopback(owner_->busPinList(), owner_->busPinCount(),
                                               static_cast<uint16_t>(clockPin),
                                               static_cast<uint16_t>(owner_->loopbackRxPin),
                                               frame, frameBytes, dataBytes, rowBits,
                                               owner_->busClockMultiplier(),
                                               ringRows, ringBufs, useRing);
    }

    // No auto-router: a silent fallback made the ACTIVE path invisible. The switch says what runs.
    /// Whether reinit should build a ring for this config rather than the whole-frame path.
    bool wantsRing() const override {
        if (!owner_->pinExpanderMode()) return false;   // direct mode never rings (drives PSRAM fine)
        return useRing;
    }

    /// Bring the bus up as a streaming ring, so a frame too big for internal RAM never forms.
    bool busInitRing(size_t rowBytes, uint32_t totalRows) override {
        // The RAM budget takes free internal MINUS a reserve, since WiFi and HTTP need their share.
        if (ringAuto && rowBytes > 0) {
            const uint32_t maxRows = static_cast<uint32_t>(platform::kRingNodeMaxBytes / rowBytes);
            ringRows = static_cast<uint8_t>(maxRows > 64 ? 64 : (maxRows ? maxRows : 1));
            const uint32_t slices = (totalRows + ringRows - 1u) / ringRows;
            const size_t bufBytes = static_cast<size_t>(ringRows) * rowBytes;
            const size_t freeInt = platform::freeInternalHeap();
            constexpr size_t kReserve = 64 * 1024;   // leave WiFi/HTTP/stacks their internal share
            // A deep pool is the whole win: each buffer removes one slice from the ISR's load.
            const size_t budget = freeInt > kReserve ? (freeInt - kReserve) : 0;
            const uint32_t byRam = bufBytes ? static_cast<uint32_t>(budget / bufBytes) : 0;
            uint32_t bufs = slices + 1u;             // prime-only when it fits: the ideal
            if (bufs > platform::kRingBufsMax) bufs = platform::kRingBufsMax;
            if (bufs > byRam) bufs = byRam;
            ringBufs = static_cast<uint8_t>(bufs >= platform::kRingBufsMin ? bufs : platform::kRingBufsMin);
        }
        platform::moonI80SetShiftClockDiv(shiftOverclock ? 3 : 4);   // ON = 26.67 MHz, OFF = 20 MHz
        const bool ok = platform::moonI80Ws2812InitRing(bus_, owner_->busPinList(), owner_->busPinCount(),
                                               static_cast<uint16_t>(clockPin), rowBytes, totalRows,
                                               ringRows, ringBufs, ringPadUs, owner_->busClockMultiplier(),
                                               &MoonI80Peripheral::ringEncodeTrampoline, this);
        // Spawned here on the cold path, so kick and join only ever notify.
        if (ok) ensureSnapHelper();
        return ok;
    }
    // Ring buffers are independent, so the prime fork-joins across both cores when the split runs.
    /// Send one frame: prime the pool, fire the DMA, let the end-of-buffer ISR refill behind it.
    bool busTransmitRing() override {
        if (snapHelperReady() && ringBufs >= 2) {
            // Each prime waits out the previous wire end, so neither half repaints a draining buffer.
            const uint8_t half = static_cast<uint8_t>(ringBufs / 2);
            primeLo_ = 0; primeHi_ = half;
            helperKick();                                             // core 0: buffers [0, half)
            platform::moonI80Ws2812PrimeRange(bus_, half, ringBufs);   // this core: [half, ringBufs)
            helperJoin();                                              // fence: every buffer primed
            return platform::moonI80Ws2812ArmRing(bus_);
        }
        return platform::moonI80Ws2812TransmitRing(bus_);
    }
    /// The ring's regime for the status suffix: primed before arming, or lapping behind the DMA.
    const char* busRingMode() const override {
        const platform::MoonI80RingStats s = platform::moonI80Ws2812RingStats(bus_);
        if (!s.isRing) return nullptr;
        return s.nSlices <= s.ringBufs ? "primed" : "lapping";
    }
    /// Whether the bus came up as a ring, reporting what the platform built rather than asked.
    bool busIsRing() const MM_NONBLOCKING override { return platform::moonI80Ws2812IsRing(bus_); }

    // Constants FIRST, then data: a recycled buffer holds a different slice each time it drains.
    /// The platform's encode seam: recover `this` and encode one slice into the ring buffer.
    static void MM_RAMFUNC ringEncodeTrampoline(void* user, uint8_t* dst, uint32_t firstRow, uint32_t rowCount,
                                                bool closeFrame, bool needsPrefill) {
        auto* self = static_cast<MoonI80Peripheral*>(user);
        ParallelLedDriver* owner = self->owner_;
        const uint8_t outCh = owner->correction().outChannels;
        const auto first = static_cast<nrOfLightsType>(firstRow);
        const auto count = static_cast<nrOfLightsType>(rowCount);
        if (rowCount == 0) {
            // The latch-only word, which presents the register's final slot on the strand.
            if (closeFrame) owner->encodeFrameClose(dst);
            return;
        }
        // A per-refill prefill was ~1/3 of the ISR cost, so it runs only when the constants are gone.
        const bool prefill = owner->pinExpanderMode() && (needsPrefill || !owner->uniformLaneCounts());
        if (owner->slotBytes() == 1) {
            if (prefill) owner->prefillShiftRows<uint8_t>(outCh, dst, first, count);
            owner->encodeRows<uint8_t>(outCh, dst, first, count, closeFrame);
        } else {
            if (prefill) owner->prefillShiftRows<uint16_t>(outCh, dst, first, count);
            owner->encodeRows<uint16_t>(outCh, dst, first, count, closeFrame);
        }
    }

    /// WR is part of the bus identity, so a change to it rebuilds the bus: not a data-pin edit.
    void recordBusPins() override { lastClockPin_ = clockPin; }
    /// Whether the extra bus pins still match the live bus, WR being bus identity here.
    bool extraBusPinsCurrent() const override { return lastClockPin_ == clockPin; }

    // The SNAPSHOT is deliberately NOT forked: doing so saturated core 0 and hung the board.

    /// Whether the fork-join should engage: the helper is up and this caller runs on core 1.
    bool snapHelperReady() const override {
        return snapHelper_.impl != nullptr && !snapHelperBroken_ && platform::currentCore() == 1;
    }


    /// Publish this frame's prime job and wake the helper; always paired with `helperJoin`.
    void helperKick() {
        if (!snapHelper_.impl) return;
        // Strict ordering keeps the bounds race-free; this park-wait stops a lost or doubled notify.
        const uint32_t t0 = platform::millis();
        while (!snapHelperParked_.load(std::memory_order_acquire)) {
            if (platform::millis() - t0 > kSnapJoinTimeoutMs) { snapHelperBroken_ = true; runHelperJob(); return; }
            platform::yield();
        }
        snapHelperParked_.store(false, std::memory_order_release);   // consume the park; helper re-sets it
        snapHelperDone_.store(false, std::memory_order_release);
        platform::notifyTask(snapHelper_);
    }

    // A timeout latches the helper broken and runs serially, so a wedged worker degrades.
    /// Block until the helper's half is primed; the fence that makes the fork-join safe.
    void helperJoin() {
        if (!snapHelper_.impl) return;
        // A degraded kick already did the work serially, so there is nothing to wait for.
        if (snapHelperBroken_) return;
        // A real-time deadline, not a spin count: yield() may return instantly and burn hot.
        const uint32_t t0 = platform::millis();
        while (!snapHelperDone_.load(std::memory_order_acquire)) {
            if (platform::millis() - t0 > kSnapJoinTimeoutMs) {
                // Self-heal: the jobs are idempotent, so even a late helper write is identical.
                runHelperJob();
                snapHelperBroken_ = true;
                return;
            }
            platform::yield();
        }
    }

    /// Prime the buffer range the kick published, on the helper or serially as the fallback.
    void runHelperJob() { platform::moonI80Ws2812PrimeRange(bus_, primeLo_, primeHi_); }

    /// Bring the helper task up on core 0, the ring's tick having core 1 under the split.
    void ensureSnapHelper() {
        if (snapHelper_.impl) return;
        snapHelperBroken_ = false;   // a rebuild gets a fresh chance
        snapHelperStop_.store(false, std::memory_order_release);
        // Seeded true, so the first kick does not wait on a park signal the task has yet to emit.
        snapHelperParked_.store(true, std::memory_order_release);
        snapHelperDone_.store(true, std::memory_order_release);
        platform::spawnPinnedTask(snapHelper_, "mmSnap", &MoonI80Peripheral::snapHelperTramp, this,
                                  4096, 5, /*core=*/0);
    }
    /// Tear the helper down, so the worker cannot outlive the bus it primes into.
    void stopSnapHelper() {
        if (!snapHelper_.impl) return;
        snapHelperStop_.store(true, std::memory_order_release);
        platform::stopPinnedTask(snapHelper_);
    }

    /// The worker's entry point: park, run the published job, mark done, repeat until stopped.
    static void snapHelperTramp(void* user) {
        auto* self = static_cast<MoonI80Peripheral*>(user);
        while (!self->snapHelperStop_.load(std::memory_order_acquire)) {
            // Announced BEFORE blocking, closing the window where done is set but the park is not.
            self->snapHelperParked_.store(true, std::memory_order_release);
            if (!platform::waitNotify(self->snapHelper_, 100)) continue;   // re-check stop on timeout
            if (self->snapHelperStop_.load(std::memory_order_acquire)) break;
            self->runHelperJob();                                           // this core-0 frame's half
            self->snapHelperDone_.store(true, std::memory_order_release);
        }
    }

private:
    /// Join deadline for one prime half; at this point the helper is broken rather than slow.
    static constexpr uint32_t kSnapJoinTimeoutMs = 100;
    /// The core-0 worker that primes the bottom half of the ring pool.
    platform::WorkerTask snapHelper_{};
    /// Stored by the helper when its job is finished; starts true so the first join returns.
    std::atomic<bool> snapHelperDone_{true};
    /// Asks the helper to exit its wait loop; set once by `stopSnapHelper` at teardown.
    std::atomic<bool> snapHelperStop_{false};
    /// Whether the helper is parked, which the kick waits for before mutating the bounds.
    std::atomic<bool> snapHelperParked_{true};
    /// Self-heal latch: one timed-out join disables the helper and the prime runs serial.
    bool snapHelperBroken_ = false;
    /// The buffer range [lo, hi) the helper primes this frame: written only between join and kick.
    uint8_t primeLo_ = 0, primeHi_ = 0;

    /// The platform-side bus handle; opaque here, so no LCD_CAM type escapes the platform layer.
    platform::MoonI80Ws2812Handle bus_;
    /// The clockPin the bus was last built with, so a change to the control can force a rebuild.
    int8_t lastClockPin_ = -1;
};

// Registered once at static-init; the one ParallelLedDriver drives it via the `peripheral` control.
inline const bool kMoonI80PeripheralRegistered =
    ParallelLedDriver::registerPeripheral("LCD-MM", []() -> LedPeripheral* { return new MoonI80Peripheral(); });

} // namespace mm
