#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared driver body + LedPeripheral
#include "platform/platform.h"


namespace mm {

/// A `LedPeripheral` backend: parallel WS2812B on the LCD_CAM peripheral, driven by our own DMA code. Same peripheral, pins and wire contract as the `i80` backend, differing underneath, and it buys a streamed frame and a 74HCT595 expander driving 8 strands per GPIO.
///
/// The wiring and the peripheral comparison are on the drivers page.
///
/// @attribution hpwit
///
/// @technical
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
class MoonI80Peripheral : public LedPeripheral, public platform::InternalRamObject {   // the refill interrupt reads it, so internal RAM, as ParallelLedDriver
public:
    /// The ring's live counters, once a second; `ab` counts lost frames, 0 on a healthy ring.
    char ringDbgStr_[192] = "—";
    bool shortOfLead_ = false;   ///< ringAuto stopped at the internal floor before the lead the ring needs

    // --- LedPeripheral descriptors ---

    /// LCD_CAM lanes on this chip; 0 leaves the driver inert through the orchestrator's guards.
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return platform::lcdLanes; }
    /// The i80 bus width is 8 or 16, a hardware fact of the peripheral. The pin count stays free, as the orchestrator rounds the bus up and parks spare lanes on WR.
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
    uint16_t clockPinForBus() const override { return static_cast<uint16_t>(owner_->clockPin); }
    /// A spare lane needs no pad: this backend routes only the GPIOs it is handed.
    bool spareLanesNeedPad() const override { return false; }

    /// Bind the bus controls; WR is bound always but shown only in expander mode.
    void addBusControls(ControlList& controls) override {
        controls.addPin("clockPin", owner_->clockPin);
        controls.setHidden(controls.count() - 1, !owner_->pinExpanderMode());
    }

    /// What ringAuto picks: rows per buffer, buffers, and whether the floor cut the lead short.
    struct RingDepth { uint8_t rows; uint8_t bufs; bool shortOfLead; };
    /// The depth the measured lead needs, no more than one buffer per slice, and never past the internal floor WiFi and HTTP need.
    static RingDepth autoDepth(size_t rowBytes, uint32_t totalRows, size_t freeInternal) {
        if (rowBytes == 0) rowBytes = 1;   // no row to size by: the depth falls to its limits rather than dividing by zero
        const uint32_t maxRows = static_cast<uint32_t>(platform::kRingNodeMaxBytes / rowBytes);
        const uint8_t rows = static_cast<uint8_t>(maxRows < platform::kRingAutoRows ? (maxRows ? maxRows : 1) : platform::kRingAutoRows);
        const uint32_t slices = (totalRows + rows - 1u) / rows;
        const size_t bufBytes = static_cast<size_t>(rows) * rowBytes;
        // The lead counts only past the buffers the refill keeps clear and the slice it is writing.
        uint32_t bufs = (platform::kRingLeadRows + rows - 1u) / rows + platform::kRingRefillGap + 1u;
        if (bufs > slices + 1u) bufs = slices + 1u;   // a buffer per slice and one spare: nothing races the wire
        if (bufs > platform::kRingBufsMax) bufs = platform::kRingBufsMax;
        const size_t budget = freeInternal > platform::kInternalFreeFloor ? freeInternal - platform::kInternalFreeFloor : 0;
        const uint32_t byRam = static_cast<uint32_t>(budget / bufBytes);
        const bool shortOfLead = bufs > byRam;
        if (shortOfLead) bufs = byRam;
        return {rows, static_cast<uint8_t>(bufs >= platform::kRingBufsMin ? bufs : platform::kRingBufsMin), shortOfLead};
    }

    /// This backend streams the ring, so the driver shows the ring's settings.
    bool supportsRing() const override { return true; }

    /// Bind the ring's instrument below the driver's ring settings.
    void addRingControls(ControlList& controls) override {
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
        std::snprintf(ringDbgStr_, sizeof(ringDbgStr_), "sn%c lv%c sl%u/bf%u co%u cr%u ab%u dn%u ld%u lt%u tx%u ipb%u ci%u tn%d de%u enc%u ea%u sg%u se%u tw%u ts%u tp%u gap%u@%u ml%d c%u",
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
                      static_cast<unsigned>(s.maxIsrGapUs), static_cast<unsigned>(s.maxGapAtSlice),
                      static_cast<int>(s.minLead), static_cast<unsigned>(s.isrCore));
                      // The field legend is on the LED drivers page.
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

    /// Refuse a bus configuration that cannot work, naming the reason; in direct mode WR never leaves the peripheral, so a shared GPIO is harmless there.
    const char* validateBusFatal() const override {
        if (owner_->pinExpanderMode()) {
            // Unset would route WR to GPIO 65535, so reject it before busInit reaches the pad.
            if (owner_->clockPin < 0) return "the 74HCT595 expander needs a clockPin (its shift clock)";
            if (owner_->latchPin >= 0 && owner_->latchPin == owner_->clockPin)
                return "latchPin is on clockPin (WR) — the latch needs its own GPIO";
        }
        return nullptr;
    }
    /// Refuse a data lane that shares WR's GPIO, which would emit the clock as pixel data.
    const char* validateBusPins(const uint16_t* lanes, uint8_t n) const override {
        if (!owner_->pinExpanderMode()) return nullptr;
        for (uint8_t i = 0; i < n; i++)
            if (lanes[i] == static_cast<uint16_t>(owner_->clockPin)) return "a data pin is on clockPin (WR)";
        return nullptr;
    }

    /// Create the bus and its DMA buffers, scaling the pixel clock to the slot's shift width.
    bool busInit(size_t frameBytes, bool /*wantSecondBuffer*/) override {
        platform::moonI80SetShiftClockDiv(owner_->shiftOverclock ? 3 : 4);   // ON = 26.67 MHz, OFF = 20 MHz
        // Forced single-buffer here too, so a caller that forgets the gate cannot reach the race.
        return platform::moonI80Ws2812Init(bus_, owner_->busPinList(), owner_->busPinCount(),
                                           static_cast<uint16_t>(owner_->clockPin), frameBytes,
                                           /*wantSecondBuffer=*/false, owner_->busClockMultiplier());
    }
    /// Tear the bus down.
    void busDeinit() override { platform::moonI80Ws2812Deinit(bus_); }
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
                                               static_cast<uint16_t>(owner_->clockPin),
                                               static_cast<uint16_t>(owner_->loopbackRxPin),
                                               frame, frameBytes, dataBytes, rowBits,
                                               owner_->busClockMultiplier(),
                                               owner_->ringRows, owner_->ringBufs, owner_->useRing);
    }

    /// Whether reinit should build a ring for this config rather than the whole-frame path. A silent fallback would hide the active path, so the switch says what runs.
    bool wantsRing() const override {
        if (!owner_->pinExpanderMode()) return false;   // direct mode never rings (drives PSRAM fine)
        return owner_->useRing;
    }

    /// Bring the bus up as a streaming ring, so a frame too big for internal RAM never forms.
    bool busInitRing(size_t rowBytes, uint32_t totalRows) override {
        shortOfLead_ = false;
        if (owner_->ringAuto && rowBytes > 0) {
            const RingDepth d = autoDepth(rowBytes, totalRows, platform::freeInternalHeap());
            owner_->ringRows = d.rows;
            owner_->ringBufs = d.bufs;
            shortOfLead_ = d.shortOfLead;
        }
        platform::moonI80SetShiftClockDiv(owner_->shiftOverclock ? 3 : 4);   // ON = 26.67 MHz, OFF = 20 MHz
        const bool ok = platform::moonI80Ws2812InitRing(bus_, owner_->busPinList(), owner_->busPinCount(),
                                               static_cast<uint16_t>(owner_->clockPin), rowBytes, totalRows,
                                               owner_->ringRows, owner_->ringBufs, owner_->ringPadUs, owner_->busClockMultiplier(),
                                               &MoonI80Peripheral::ringEncodeTrampoline, this);
        return ok;
    }
    /// Send one frame: prime the pool, fire the DMA, let the end-of-buffer ISR refill behind it. The prime runs on this core alone: a helper priming half on core 0 preempted the effects there and cost 12 fps.
    bool busTransmitRing() override { return platform::moonI80Ws2812TransmitRing(bus_); }
    /// The ring's regime for the status suffix: primed before arming, or lapping behind the DMA.
    const char* busRingMode() const override {
        const platform::MoonI80RingStats s = platform::moonI80Ws2812RingStats(bus_);
        if (!s.isRing) return nullptr;
        if (shortOfLead_) return "lapping, short of RAM: frames may glitch";
        return s.nSlices <= s.ringBufs ? "primed" : "lapping";
    }
    /// Whether the bus came up as a ring, reporting what the platform built rather than asked.
    bool busIsRing() const MM_NONBLOCKING override { return platform::moonI80Ws2812IsRing(bus_); }

    /// The platform's encode seam: recover `this` and encode one slice into the ring buffer, constants first, since a recycled buffer holds a different slice each time it drains.
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
        const bool prefill = owner->hot_.shift && (needsPrefill || !owner->uniformLaneCounts());
        if (owner->hot_.slotBytes == 1) {
            if (prefill) owner->prefillShiftRows<uint8_t>(outCh, dst, first, count);
            owner->encodeRows<uint8_t>(outCh, dst, first, count, closeFrame);
        } else {
            if (prefill) owner->prefillShiftRows<uint16_t>(outCh, dst, first, count);
            owner->encodeRows<uint16_t>(outCh, dst, first, count, closeFrame);
        }
    }

    /// WR is part of the bus identity, so a change to it rebuilds the bus: not a data-pin edit.
    void recordBusPins() override { lastClockPin_ = owner_->clockPin; }
    /// Whether the extra bus pins still match the live bus, WR being bus identity here.
    bool extraBusPinsCurrent() const override { return lastClockPin_ == owner_->clockPin; }

private:

    /// The platform-side bus handle; opaque here, so no LCD_CAM type escapes the platform layer.
    platform::MoonI80Ws2812Handle bus_;
    /// The clockPin the bus was last built with, so a change to the control can force a rebuild.
    int8_t lastClockPin_ = -1;
};

/// Registers the peripheral once at static-init; the one ParallelLedDriver drives it via the `peripheral` control.
inline const bool kMoonI80PeripheralRegistered =
    ParallelLedDriver::registerPeripheral("LCD-MM", []() -> LedPeripheral* { return new MoonI80Peripheral(); });

} // namespace mm
