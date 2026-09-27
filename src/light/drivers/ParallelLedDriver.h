#pragma once

#include "light/drivers/DriverBase.h"        // DriverBase, Correction
#include "light/drivers/LedPeripheral.h"     // runtime peripheral strategy (Parlio / i80 / MoonI80)
#include "light/drivers/ParallelSlots.h"        // encodeWs2812ParallelSlots (shared encoder)
#include "light/drivers/LedDriverConfig.h"
#include "light/drivers/PinList.h"         // parsePinList / assignCounts (shared)
#include "platform/platform.h"

#include <numeric>  // std::gcd: the snapshot split's cache-line alignment

namespace mm {

/// The registered parallel WS2812B driver: up to 16 strands clocking out at once, one GPIO lane each, fed consecutive slices of the source buffer over whichever peripheral the control selects. Backends: I80Peripheral.h, MoonI80Peripheral.h, ParlioPeripheral.h.
///
/// The whole frame is encoded up front and shipped as one autonomous transfer. So there is no CPU deadline while it is on the wire. The encode is a fused correct and transpose, per row (ParallelSlots.h). Vocabulary: strand, lane, slot, row, under
/// @xref{terminology|More info → Terminology}.
/// @card ParallelLedDriver.png
///
/// @moreinfo
///
/// ## Why single-shot
///
/// Encoding the whole frame first removes the deadline: a driver refilling as the DMA drains must beat the clock every time, and a WiFi interrupt garbles the rest. MoonI80Peripheral's ring gives this up to hold a frame too big for memory.
///
/// ## Terminology
///
/// A **strand** is one chain of LEDs. A **lane** is one bus data line: in the normal case, one GPIO driving one strand. A **slot** is one WS2812 bit on the wire. A **row** is one light across every strand at once, so a frame is `maxLaneLights` rows.
///
/// ## The frame transpose (correct + transpose, per row)
///
/// The source holds each light's bytes together; the wire needs each bus WORD to carry one bit of EVERY strand at once. The encoder turns 8 lights on their side, an 8x8 bit matrix transpose, writing one word per slot fused with the correction. A Parlio bus word and an i80 bus word have the same meaning.
class ParallelLedDriver : public DriverBase {
public:
    /// This driver prices its own frame, so the shared path leaves the limiter to it.
    bool limitsCurrent() const override { return true; }

    /// Test-only: borrow a mock backend, dropping any existing one. The caller keeps ownership.
    void setPeripheralForTest(LedPeripheral* p) {
        if (peripheral_ && peripheralOwned_) { peripheral_->busDeinit(); delete peripheral_; }
        else if (peripheral_) peripheral_->busDeinit();
        peripheral_ = p;
        peripheralOwned_ = false;   // the test owns a stack/heap mock; the orchestrator must not delete it
        peripheralActiveReg_ = 0xFF;
        if (p) p->attach(this);
    }

    /// Recompute the heap total for the module readout; a test with no alloc site asks for it.
    void publishHeapBytesForTest() { publishHeapBytes(); }

    // `#if`-gated per chip in main.cpp, so the Select offers the linked-and-supported subset.
    using PeripheralFactory = LedPeripheral* (*)();
    struct PeripheralEntry { const char* label; PeripheralFactory make; };
    /// How many backends can register, sized to the most any one chip links in.
    static constexpr uint8_t kMaxPeripherals = 4;
    static inline PeripheralEntry peripheralRegistry_[kMaxPeripherals] = {};
    /// How many of the registry's slots are filled, so registration stops at the cap.
    static inline uint8_t peripheralRegistryCount_ = 0;
    /// Register a backend factory under a UI label, once per linked backend at static-init.
    static bool registerPeripheral(const char* label, PeripheralFactory make) {
        if (!label || !make || peripheralRegistryCount_ >= kMaxPeripherals) return false;
        for (uint8_t i = 0; i < peripheralRegistryCount_; i++)
            if (std::strcmp(peripheralRegistry_[i].label, label) == 0) return false;
        peripheralRegistry_[peripheralRegistryCount_++] = {label, make};
        return true;
    }

    // On desktop no backend links, so peripheral_ stays null and the driver idles.
    /// A fresh driver takes the GRB preset (WS2812 wiring) and the first backend this chip supports.
    ParallelLedDriver() {
        this->setDefaultPresetName("GRB");
        selectDefaultPeripheral(nullptr);
    }

    // The ownership flag decides, so the delete lives once in the base rather than per subclass.
    /// Free the owned backend; a test-borrowed mock is left alone.
    ~ParallelLedDriver() override {
        if (peripheral_ && peripheralOwned_) delete peripheral_;
        peripheral_ = nullptr;
    }

    // The bus width follows the pin count: up to 8 pins an 8-bit bus, 9 to 16 a 16-bit one.
    /// Max parallel lanes: the peripheral's 16 data lines. Strands can exceed it, see kMaxStrands.
    static constexpr uint8_t kMaxLanes = 16;

    // 64 because the active-strand mask is a uint64_t; parseConfig refuses more rather than dropping.
    /// Max strands: every data line fanned out through its '595 chain. Direct mode uses kMaxLanes.
    static constexpr uint8_t kMaxStrands = 64;

    // Small on purpose: a big frame exceeds the RMT-RX capture depth and reads as "bad bit 0".
    /// Light count the loopback self-test drives, or the strand length when that is smaller.
    static constexpr nrOfLightsType kLoopbackTestLights = 256;

    // A token is a pin or an inclusive range, mixing freely: "20-22,35,38-40". Parsers in PinList.h.
    /// Comma-separated GPIO list, one parallel lane per pin, each fed a slice of this driver's window.
    char pins[64] = "";
    // A lane is a STRAND: with the expander each pin fans out to 8, so entry N is strand N.
    /// Comma-separated lights per lane; the remainder splits evenly over the rest. Empty splits all.
    char ledsPerPin[kMaxStrands * 5 + 1] = "";

    // Toggling rebuilds the bus; a buffer that will not fit degrades to synchronous by itself.
    /// Encode the next frame while the current clocks out, so a tick costs max(encode, wire).
    bool doubleBuffer = true;
    // Live, not a prepare trigger: it changes only what tickRing does per frame. Ring path only.
    /// Freeze the source each frame so the ring's off-thread refill reads an immutable copy.
    bool ringSnapshot = true;
    // A persistent mode: while on it re-runs on every relevant change; off clears the verdict.
    /// On-device loopback self-test: transmit a known pattern and bit-verify the capture.
    bool     loopbackTest = false;
    // Lets the bench loopback run on a dedicated jumper pin without re-typing the operational `pins`.
    /// TX override for the self-test; unset (-1) falls back to lane 0.
    int8_t   loopbackTxPin = -1;
    // Walking this 0..N-1 until the test passes identifies the tapped strand empirically.
    /// Which strand carries the test pattern, shift mode only; direct mode always uses lane 0.
    uint8_t  loopbackStrand = 0;
    // OFF tests a replica, and on the ring path it re-allocates the whole pool.
    /// Loopback mode: ON rides the live pipeline, OFF rebuilds a private bus for the test.
    bool     loopbackIntrusive = false;
    // With a '595 the jumper comes off a 5 V register output and MUST be level-shifted.
    /// Jumper this to the TX lane for the self-test; unset (-1) by default.
    int8_t   loopbackRxPin = -1;

    // A checkbox, not a fan-out number: 8 is the '595's own width. Limits: the driver page.
    /// Is a 74HCT595 shift-register expander fitted? Each data pin then drives 8 strands.
    bool     pinExpander = false;
    // A DATA lane: it takes a bit of every bus word, so it cannot share a data pin or clockPin.
    /// The 74HCT595 latch (RCLK), pulsed once the shifted byte is in. Unset (-1) until wired.
    int8_t   latchPin = -1;

    // Gated here rather than by mutating `pinExpander`, so the saved value survives a round trip.
    /// Is the shift-register expander engaged, after the peripheral's own capability?
    bool pinExpanderMode() const { return pinExpander && peripheral_ && peripheral_->supportsPinExpander(); }
    /// Strands per physical data pin: 1 direct, or the '595's width through the expander.
    uint8_t outputsPerPin() const { return pinExpanderMode() ? kPinExpanderOutputs : uint8_t(1); }

    // Above the `peripheral` selector is invariant, below is what that peripheral supports.
    /// Bind the controls: the invariant block, the peripheral selector, then that backend's own.
    void defineDriverControls() override {
        addWindowControls();   // start / count: the slice of the shared buffer this driver outputs
        controls_.addText("pins", pins, sizeof(pins));
        controls_.addText("ledsPerPin", ledsPerPin, sizeof(ledsPerPin));
        // The pure output floor, independent of render load: the headroom the pipeline has left.
        controls_.addReadOnly("frameTime", frameTimeStr_, sizeof(frameTimeStr_));
        // The live backend must agree with peripheralSel_ BEFORE the backend surfaces its controls.
        buildPeripheralOptions();
        ensurePeripheralMatchesSelection();
        controls_.addSelect("peripheral", peripheralSel_, peripheralOptions_, peripheralOptionCount_);
        // Bound even when hidden, so the saved value survives a single-buffer-only peripheral.
        controls_.addControl("doubleBuffer", doubleBuffer);
        controls_.setHidden(controls_.count() - 1, !(peripheral_ && peripheral_->supportsDoubleBuffer()));
        // Peripheral-specific: MoonI80 routes WR only for a '595, i80 always needs a real WR and DC.
        if (peripheral_) peripheral_->addBusControls(controls_);
        // Before the ring cluster, which depends on it: a switch sits above what it controls.
        controls_.addControl("pinExpander", pinExpander);
        controls_.setHidden(controls_.count() - 1, !(peripheral_ && peripheral_->supportsPinExpander()));
        controls_.addPin("latchPin", latchPin);
        controls_.setHidden(controls_.count() - 1, !pinExpanderMode());
        // MoonI80 only, and gated on the expander, so it sits under the switch that governs it.
        if (peripheral_) peripheral_->addRingControls(controls_);
        // A bring-up instrument rather than a casual setting, so the whole cluster is expert-only.
        controls_.addControl("loopbackTest", loopbackTest);
        controls_.setAdvanced(controls_.count() - 1);
        // Always bound, shown only in test mode: the conditional-control shape.
        controls_.addPin("loopbackTxPin", loopbackTxPin);
        // Direct mode only: in shift mode loopbackStrand decides, so this would only mislead.
        controls_.setHidden(controls_.count() - 1, !loopbackTest || pinExpanderMode());
        controls_.setAdvanced(controls_.count() - 1);
        controls_.addPin("loopbackRxPin", loopbackRxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        controls_.setAdvanced(controls_.count() - 1);
        // Lets the jumper come off ANY '595 output, including a spare that drives no panel.
        controls_.addControl("loopbackStrand", loopbackStrand, 0, kMaxStrands - 1);
        controls_.setHidden(controls_.count() - 1, !loopbackTest || !pinExpanderMode());
        controls_.setAdvanced(controls_.count() - 1);
        // Intrusive: ride the live pipeline instead of a private replica (see the member doc).
        controls_.addControl("loopbackIntrusive", loopbackIntrusive);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        controls_.setAdvanced(controls_.count() - 1);
    }

    // doubleBuffer, pinExpander and latchPin decide an allocation only bus init can change.
    /// Which control changes re-parse and re-init the bus live, through the prepare sweep.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "pins") == 0 || std::strcmp(name, "ledsPerPin") == 0
            || std::strcmp(name, "doubleBuffer") == 0
            || std::strcmp(name, "pinExpander") == 0 || std::strcmp(name, "latchPin") == 0
            || isWindowControl(name)
            || (peripheral_ && peripheral_->busControlTriggersBuild(name));   // clockPin/dcPin on i80
    }

    /// React to a control change off the render loop; loopbackTest re-runs while it is on.
    void onControlChanged(const char* name) override {
        // The rebuild already swapped it: a second swap dangles every backend-owned control.
        if (std::strcmp(name, "peripheral") == 0) {
            ensurePeripheralMatchesSelection();
            parseConfig();
            reinit();
            DriverBase::onControlChanged(name);
            return;
        }
        const bool isTestControl = std::strcmp(name, "loopbackTest") == 0;
        // A test left running across such an edit would report a verdict for a config that is gone.
        const bool isPinControl  = std::strcmp(name, "pins") == 0
                                || std::strcmp(name, "pinExpander") == 0
                                || std::strcmp(name, "latchPin") == 0
                                || std::strcmp(name, "loopbackTxPin") == 0
                                || std::strcmp(name, "loopbackRxPin") == 0;
        if (isTestControl && !loopbackTest) {
            // Re-derive rather than clear: a config or init error must survive the toggle.
            clearFailBuf();
            clearStatus();
            parseConfig();
            reinit();
        } else if (loopbackTest && (isTestControl || isPinControl || isTestParamControl(name))) {
            // This runs BEFORE the prepare sweep, so refresh or the test uses stale pins.
            if (isPinControl) { parseConfig(); reinit(); }
            runLoopbackSelfTest();
        }
        // Without this chain a correction-control edit never reaches this driver's LUT.
        DriverBase::onControlChanged(name);
    }

    /// Loopback parameters: no bus rebuild, but a change re-runs a running test.
    static bool isTestParamControl(const char* name) {
        return std::strcmp(name, "loopbackStrand") == 0
            || std::strcmp(name, "loopbackIntrusive") == 0;
    }

    /// One-time wiring: parse the lane lists. The bus acquire lives in prepare().
    void setup() override { parseConfig(); }
    /// Deinit the bus, then clear the shared fail and config-error state.
    void release() override {
        deinit();   // drains in-flight first, so the ring's refill has stopped reading the snapshot
        freeSnapshot();
        DriverBase::release();   // frees the correction scratch, clears failBuf_ + configErr_
    }

    // The caller has already drained any in-flight refill, so no ISR reads it.
    /// Free the ring snapshot buffer, clear the encode source, refresh the memory readout.
    void freeSnapshot() {
        if (snapshotBuf_) { platform::free(snapshotBuf_); snapshotBuf_ = nullptr; snapshotCap_ = 0; this->publishHeapBytes(); }
        encodeSrc_ = nullptr;
    }

    // No enabled() check: applyState routes a disabled driver to release() instead.
    /// Pure build: re-parse the lanes and re-init the bus, off the hot path.
    void prepare() override {
        // BEFORE parseConfig, which reallocates state the ring's refill task reads on its own core.
        drainInFlight();
        parseConfig();
        reinit();
    }

    // The gate stops a double reinit: rebuilding twice left the ring's GDMA EOF interrupt dead.
    /// Re-init when a channel-count change resizes the frame, and only then.
    void onCorrectionChanged() override {
        if (!effectivelyEnabled()) return;
        const size_t before = frameBytes_;
        drainInFlight();
        parseConfig();
        if (frameBytes_ != before) reinit();   // rebuild only on a real frame-size change (see above)
    }

    /// Point the driver at the source frame buffer and re-parse the lane config.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; parseConfig(); }

    // Which path runs is decided by whether a second DMA buffer was allocated, not by the flag.
    /// Per-tick output: correct and transpose each row into the DMA buffer, then ship one transfer.
    // Reported as blocking deliberately: hiding it would make the hot-path report worthless.
    void tick() MM_NONBLOCKING override {
        if (!peripheral_ || peripheral_->lanesAvailable() == 0) return;  // no backend / inert off this chip
        // Loopback owns the peripheral exclusively: it tears the bus down and rebuilds it.
        if (loopbackTest) return;
        if (!inited_ || !dmaBuf_ || !sourceBuffer_ || !sourceBuffer_->data()
            || laneCount_ == 0 || maxLaneLights_ == 0) return;
        const uint8_t outCh = correction_.outChannels;
        if (outCh == 0) return;
        // Idle rather than overrun when the scratch is missing or pending a shrunk realloc.
        if (!wire_ || wireCap_ < static_cast<size_t>(kMaxStrands) * outCh * platform::kMaxCores) return;

        // The ring holds no whole frame, so the capacity guard below does not apply to it.
        if (peripheral_->busIsRing()) { tickRing(outCh); return; }
        // Reported in LIGHTS PER LANE, the control the user sets, rather than failing silently.
        if (frameBytes_ > peripheral_->busCapacity()) {
            reportOverCapacity(outCh, peripheral_->busCapacity());
            return;
        }

        // The allocation decides, not the flag, so a stale flag cannot route a single buffer async.
        if (peripheral_->busBuffer(1)) tickAsync(outCh);   // double-buffer (doubleBuffer ON)
        else                           tickSync(outCh);    // synchronous (doubleBuffer OFF / no 2nd buf)
    }

    /// Price the frame against the budget before encodeRows forks across both cores, so both halves read a `limit` that is already settled.
    void measureFrame() {
        if (!sourceBuffer_ || !sourceBuffer_->data()) return;
        const uint8_t* src = encodeSrc_ ? encodeSrc_ : sourceBuffer_->data();
        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        nrOfLightsType lights = 0;
        for (uint8_t lane = 0; lane < laneCount_; lane++) lights += laneCounts_[lane];
        correction_.measure(src + static_cast<size_t>(winStart_) * srcCh, srcCh, lights);
    }

    // One DMA buffer, no alternation, no deferred-wait bookkeeping, zero added latency.
    /// Blocking path: encode, send, wait out the wire, so a tick costs encode plus wire.
    void tickSync(uint8_t outCh) {
        if (busGaveUp()) return;
        // A timed-out wait leaves the DMA reading: re-wait rather than encode over a live transfer.
        if (!busWaitIfBusy(0)) return;
        uint8_t* buf = peripheral_->busBuffer(0);
        if (!buf) return;
        measureFrame();
        // Branch on the BUS WIDTH, not the strand count: 48 strands on 6 pins is still 8-bit.
        if (slotBytes() == 1) encodeRows<uint8_t>(outCh, buf);
        else                  encodeRows<uint16_t>(outCh, buf);
        // Wait only when the transfer started: a failed transmit gives no done-callback.
        if (peripheral_->busTransmit(0, frameBytes_)) {
            inFlight_[0] = true;
            busWaitIfBusy(0);   // synchronous: wait it out here; clears the flag on completion
        } else if (deadFrames_ < kDeadFramesBeforeGiveUp) {
            deadFrames_++;      // a REFUSED frame is as dead as a wedged one: see busWaitIfBusy
        }
    }

    // Costs the second DMA buffer and one frame of output latency.
    /// Deferred-wait path: encode N+1 while N clocks out, so a tick costs max(encode, wire).
    void tickAsync(uint8_t outCh) {
        if (busGaveUp()) return;
        // A timed-out wait skips the frame, so a wedged DMA idles rather than emits garbage.
        if (!busWaitIfBusy(active_)) return;
        // 2. Fused per-ROW encode into buffer `active_`, one branch on the bus width (see encodeRows).
        uint8_t* buf = peripheral_->busBuffer(active_);
        if (!buf) return;
        measureFrame();
        // Branch on the BUS WIDTH, not the strand count: 48 strands on 6 pins is still 8-bit.
        if (slotBytes() == 1) encodeRows<uint8_t>(outCh, buf);
        else                  encodeRows<uint16_t>(outCh, buf);
        // 3. Kick this buffer's DMA and return WITHOUT waiting; flip to the other buffer for next tick.
        if (peripheral_->busTransmit(active_, frameBytes_)) {
            inFlight_[active_] = true;
            active_ ^= 1;
        } else if (deadFrames_ < kDeadFramesBeforeGiveUp) {
            deadFrames_++;      // a REFUSED frame is as dead as a wedged one: see busWaitIfBusy
        }
    }

    // ASYNC per tick: blocking here starves the refill task past its deadline on the same core.
    /// Streaming-ring path: wait, arm the next frame, and return with the wire still running.
    void tickRing(uint8_t /*outCh*/) {
        if (busGaveUp()) return;
        // Bench diagnostic: the ring tick's wire-wait, snapshot and prime segments, via ringDbg.
        const uint32_t tkW0 = platform::cycleCount();
        // A strand receives one frame at a time, so the previous must finish before the next starts.
        if (!busWaitIfBusy(0)) return;
        const uint32_t tkW1 = platform::cycleCount();
        // The refill encodes off the render thread, so it needs an immutable copy.
        if (ringSnapshot) { if (!snapshotSourceForRing()) return; }
        else              encodeSrc_ = nullptr;   // OFF: encodeRows reads the live sourceBuffer_
        const uint32_t tkW2 = platform::cycleCount();
        measureFrame();
        if (peripheral_->busTransmitRing()) {
            inFlight_[0] = true;   // kicked; DO NOT wait here: the next tick waits, freeing the core now
        } else if (deadFrames_ < kDeadFramesBeforeGiveUp) {
            deadFrames_++;         // a REFUSED frame is as dead as a wedged one: see busWaitIfBusy
        }
        const uint32_t tkW3 = platform::cycleCount();
        constexpr uint32_t kCyPerUs = 240;   // S3 at 240 MHz
        dbgTickWaitUs = (tkW1 - tkW0) / kCyPerUs;
        dbgTickSnapUs = (tkW2 - tkW1) / kCyPerUs;
        dbgTickPrimeUs = (tkW3 - tkW2) / kCyPerUs;
    }

    /// Microseconds the ring tick spent waiting for the peripheral, for `ringDbg`.
    static inline volatile uint32_t dbgTickWaitUs = 0;
    /// Microseconds the ring tick spent snapshotting the frame, for `ringDbg`.
    static inline volatile uint32_t dbgTickSnapUs = 0;
    /// Microseconds the ring tick spent priming buffers, for `ringDbg`.
    static inline volatile uint32_t dbgTickPrimeUs = 0;

    // The pure WS2812 output floor: the render loop can never beat it.
    /// Refresh the `frameTime` KPI once a second: the measured wire time and its fps ceiling.
    void tick1s() MM_NONBLOCKING override {
        if (!peripheral_) return;
        // A bus refused to a sibling comes back on its own, retried at most once per second.
        if (!inited_ && laneCount_ > 0 && peripheral_->busContentionCleared()) reinit();
        const uint32_t us = peripheral_->busLastTransmitUs();
        if (us == 0) std::snprintf(frameTimeStr_, sizeof(frameTimeStr_), "—");
        else std::snprintf(frameTimeStr_, sizeof(frameTimeStr_), "%u µs (%u fps max)",
                           static_cast<unsigned>(us), static_cast<unsigned>(1000000u / us));
        peripheral_->refreshBusKpi();   // per-backend extra read-only KPIs (MoonI80's ring diagnostic); base no-op
    }

    // False means it timed out and the DMA may still be reading, so leave the buffer alone.
    /// Wait for buffer `i` if a transfer is in flight; false means it wedged. A no-op when idle.
    bool busWaitIfBusy(uint8_t i) {
        if (!inFlight_[i]) return true;
        if (!peripheral_ || !peripheral_->busWait(i, waitBudgetMs())) {
        // A bus missing many times its own wire time is broken: stop spending the render thread.
            if (deadFrames_ < kDeadFramesBeforeGiveUp) deadFrames_++;
            return false;   // still in flight: do not reuse the buffer
        }
        deadFrames_ = 0;   // a completed transfer clears the strike count: the bus is alive again
        inFlight_[i] = false;
        // parseConfig, not clearStatus: only parseConfig restores the "driving N of M" line.
        if (gaveUpReported_) { gaveUpReported_ = false; parseConfig(); }
        return true;
    }

    // A never-delivering bus eats the CPU WiFi needs, so the driver gives up: dark but reachable.
    /// How long a transfer gets before it counts as dead: derived from the frame, never a constant.
    void reportOverCapacity(uint8_t outCh, size_t cap) {
        if (overCapReported_) return;
        overCapReported_ = true;
        const uint8_t opp     = outputsPerPin();
        const size_t  rowBytes = rowBytesFor(outCh, slotBytes(), opp);
        const size_t  pad     = padBytesFor(slotBytes(), opp);
        // Count DOWN through frameBytesFor: the frame is 64-byte rounded, so a division overshoots.
        unsigned fits = 0;
        if (rowBytes && cap > pad) {
            fits = static_cast<unsigned>((cap - pad) / rowBytes);
            while (fits > 0 && frameBytesFor(static_cast<nrOfLightsType>(fits), outCh,
                                             slotBytes(), opp) > cap) fits--;
        }
        std::snprintf(statusBuf_, sizeof(statusBuf_),
                      "too many lights per pin: %u exceeds this peripheral's %u — lower ledsPerPin",
                      static_cast<unsigned>(maxLaneLights_), fits);
        setStatus(statusBuf_, Severity::Error);
    }

    /// Whether the bus has failed for long enough to stop trying, reported once.
    bool busGaveUp() {
        if (deadFrames_ < kDeadFramesBeforeGiveUp) return false;
        if (!gaveUpReported_) {
            gaveUpReported_ = true;
            setStatus("no LED output — the driver is not sending frames; check pins and LED count",
                      Severity::Error);
        }
        // One transfer every kGiveUpRetryTicks: a completed one lifts the give-up, a failed one keeps it.
        if (++giveUpRetry_ >= kGiveUpRetryTicks) {
            giveUpRetry_ = 0;
            deadFrames_ = kDeadFramesBeforeGiveUp - 1;   // one strike below the threshold: let this frame try
            // Abandon the wedged transfer so the retry arms a fresh one instead of re-waiting it.
            inFlight_[0] = inFlight_[1] = false;
            // The retry has not succeeded yet, so the status stays honest until a frame completes.
            return false;
        }
        return true;
    }

    // Four times the wire time, floored at 20 ms and capped at 100 ms.
    /// How long a transfer gets before it counts as dead, derived from the frame, never a constant.
    uint32_t waitBudgetMs() const {
        constexpr uint32_t kSlotNs = 375;   // WS2812 wire slot; 3 per bit (ParallelSlots.h)
        const uint32_t bits = static_cast<uint32_t>(maxLaneLights_) * correction_.outChannels * 8u;
        const uint32_t wireMs = (bits * 3u * kSlotNs) / 1'000'000u;
        const uint32_t budget = wireMs * 4u;
        return budget < 20u ? 20u : (budget > 100u ? 100u : budget);
    }

    // Cannot skip on a timeout: the teardown itself cancels the transfer.
    /// Block until nothing reads the DMA buffers or the snapshot: the barrier a live resize needs.
    void drainInFlight() {
        if (!busWaitIfBusy(0)) inFlight_[0] = false;   // deinit below cancels the wedged transfer
        if (!busWaitIfBusy(1)) inFlight_[1] = false;
    }

    /// Prefill both DMA buffers' shift-mode constants; a no-op in direct mode.
    void prefillShiftConstantsIfNeeded() {
        if (!peripheral_ || !pinExpanderMode() || !inited_) return;
        const uint8_t outCh = correction_.outChannels;
        if (outCh == 0 || maxLaneLights_ == 0) return;
        for (uint8_t i = 0; i < 2; i++) {
            uint8_t* buf = peripheral_->busBuffer(i);
            if (!buf) continue;   // buffer 1 is null in single-buffer mode
            if (slotBytes() == 1) prefillShiftFrame<uint8_t>(outCh, buf);
            else                  prefillShiftFrame<uint16_t>(outCh, buf);
        }
    }

    // Prefilled in RUNS: a strand ending mid-frame must stop asserting or it flashes white.
    /// Write the shift-mode frame constants into every DMA buffer, once, off the hot path.
    template <class Slot>
    void prefillShiftFrame(uint8_t outCh, uint8_t* dst) {
        prefillShiftRows<Slot>(outCh, dst, 0, maxLaneLights_);
    }

    // Written dst-relative, so a ring slice gets the same constants the whole-frame buffer would.
    /// Prefill the shift constants for a row range; `rowCount == 0` means to the end.
    template <class Slot>
    void MM_RAMFUNC prefillShiftRows(uint8_t outCh, uint8_t* dst, nrOfLightsType firstRow, nrOfLightsType rowCount) {
        auto* out = reinterpret_cast<Slot*>(dst);
        const size_t slotsPerRow = static_cast<size_t>(outCh) * 8 * 3 * outputsPerPin();
        const nrOfLightsType lastRow = (rowCount == 0 || firstRow + rowCount > maxLaneLights_)
                                           ? maxLaneLights_
                                           : static_cast<nrOfLightsType>(firstRow + rowCount);
        nrOfLightsType row = firstRow;
        while (row < lastRow) {
            // 32-bit halves: `1ULL << lane` with a runtime lane is a library call on Xtensa.
            uint32_t mLo = 0, mHi = 0;
            nrOfLightsType runEnd = lastRow;
            for (uint8_t lane = 0; lane < laneCount_; lane++) {
                if (row < laneCounts_[lane]) {
                    if (lane < 32) mLo |= uint32_t(1) << lane;
                    else           mHi |= uint32_t(1) << (lane - 32);
                    if (laneCounts_[lane] < runEnd) runEnd = laneCounts_[lane];   // this strand ends first
                }
            }
            const uint64_t mask = mLo | (static_cast<uint64_t>(mHi) << 32);   // constant shift: cheap
            if (runEnd <= row) runEnd = lastRow;   // no strand ends ahead in range: one run to lastRow
            const uint32_t rows = static_cast<uint32_t>(runEnd - row);
            // dst-relative: row `firstRow` is at out+0, so offset by (row - firstRow).
            prefillWs2812ShiftConstants<Slot>(mask, physPins_, latchBit_, outputsPerPin(), outCh, rows,
                                              out + static_cast<size_t>(row - firstRow) * slotsPerRow);
            row = runEnd;
        }
    }

    // The ring encodes one slice straight into the buffer the DMA is about to read.
    /// Encode a row range into `dst`, dst-relative; only the last slice may set `closeFrame`.
    template <class Slot>
    void MM_RAMFUNC encodeRows(uint8_t outCh, uint8_t* dst,
                    nrOfLightsType firstRow = 0, nrOfLightsType rowCount = 0,
                    bool closeFrame = true) {
        const nrOfLightsType lastRow = (rowCount == 0 || firstRow + rowCount > maxLaneLights_)
                                           ? maxLaneLights_
                                           : static_cast<nrOfLightsType>(firstRow + rowCount);
        // Correction runs HERE, fused into the gather: the snapshot is a raw copy, not pre-corrected.
        const uint8_t* src = encodeSrc_ ? encodeSrc_ : sourceBuffer_->data();
        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        auto* out = reinterpret_cast<Slot*>(dst);
        // This core's own scratch slice: the prime fork-join runs encodeRows on both cores at once.
        uint8_t* const wire = wire_ + static_cast<size_t>(platform::currentCore()) * kMaxStrands * outCh;
        std::memset(wire, 0, static_cast<size_t>(kMaxStrands) * outCh);
        const size_t stride = outCh;
        const bool shift = pinExpanderMode();
        // Bench diagnostic: gather-and-mask against transpose-and-emit, via ringDbg's sg/se.
        uint32_t segT0 = platform::cycleCount();
        // 64-bit because the expander drives more strands than the bus is wide.
        for (nrOfLightsType row = firstRow; row < lastRow; row++) {
            // 32-bit halves: a runtime `1ULL << lane` is an Xtensa library call, 48 per row.
            uint32_t maskLo32 = 0, maskHi32 = 0;
            for (uint8_t lane = 0; lane < laneCount_; lane++) {
                if (row >= laneCounts_[lane]) continue;   // short strand: idle LOW
                if (lane < 32) maskLo32 |= uint32_t(1) << lane;
                else           maskHi32 |= uint32_t(1) << (lane - 32);
                // The source holds RAW srcCh bytes either way, so correction runs here per light.
                correction_.apply(src + (winStart_ + laneStart_[lane] + row) * srcCh,
                                  wire + lane * stride, srcCh);
            }
            const uint64_t mask = maskLo32 | (static_cast<uint64_t>(maskHi32) << 32);   // constant shift: cheap
            if (shift) {
                // DATA WORDS ONLY: the pulse-start and tail are frame constants, prefilled once.
                const uint32_t segT1 = platform::cycleCount();   // TEMP DIAGNOSTIC
                encodeWs2812ShiftData<Slot>(wire, mask, physPins_, latchBit_, outputsPerPin(), outCh, out);
                const uint32_t segT2 = platform::cycleCount();   // TEMP DIAGNOSTIC
                dbgSegGatherCy += segT1 - segT0;   // memset+mask+gather (since segT0 / previous row's end)
                dbgSegEmitCy   += segT2 - segT1;   // the transpose+emit
                dbgSegRows     += 1;
                segT0 = segT2;                     // next row's gather starts here
                out += static_cast<size_t>(outCh) * 8 * 3 * outputsPerPin();
            } else {
                encodeWs2812ParallelSlots<Slot>(wire, static_cast<Slot>(mask), outCh, out);
                out += static_cast<size_t>(outCh) * 8 * 3;   // 3 slots × 8 bits × channels, in Slot elements
            }
        }
        // Without the latch word a strand whose last wire byte is odd holds HIGH and never resets.
        if (shift && closeFrame) encodeWs2812ShiftLatchPad<Slot>(latchBit_, out);
    }

    /// Write only the frame-closing latch word, which presents the register's final slot.
    void MM_RAMFUNC encodeFrameClose(uint8_t* dst) {
        if (!pinExpanderMode()) return;
        if (slotBytes() == 1) encodeWs2812ShiftLatchPad<uint8_t>(latchBit_, reinterpret_cast<uint8_t*>(dst));
        else                  encodeWs2812ShiftLatchPad<uint16_t>(latchBit_, reinterpret_cast<uint16_t*>(dst));
    }

    // Matches the private loopback bus's width, so the DMA stride is right.
    /// Build the direct-mode self-test frame: the known pattern on lane 0, every other lane idle.
    template <class Slot>
    void encodeLoopbackFrame(uint8_t* frame, const uint8_t* wire, uint8_t outCh,
                             nrOfLightsType lights) {
        auto* out = reinterpret_cast<Slot*>(frame);
        for (nrOfLightsType row = 0; row < lights; row++) {
            encodeWs2812ParallelSlots<Slot>(wire, Slot(1), outCh, out);
            out += static_cast<size_t>(outCh) * 8 * 3;
        }
    }

    // A '595 shifts MSB-first, so strand 0 appears on the register's LAST output, Q7.
    /// Build the expander self-test frame, latch and all, so the capture proves the real wire.
    template <class Slot>
    void encodeLoopbackFrameShift(uint8_t* frame, const uint8_t* wire, uint8_t outCh,
                                  nrOfLightsType lights) {
        auto* out = reinterpret_cast<Slot*>(frame);
        // A spare output nothing else loads is the ideal tap.
        const uint8_t s = (loopbackStrand < kMaxStrands) ? loopbackStrand : uint8_t{0};
        const uint64_t mask = uint64_t(1) << s;
        // The shift encoder indexes by strand, so point strand `s`'s slot at that one light.
        for (nrOfLightsType row = 0; row < lights; row++) {
            encodeWs2812ShiftSlots<Slot>(wire, mask, physPins_, latchBit_,
                                         outputsPerPin(), outCh, out);
            out += static_cast<size_t>(outCh) * 8 * 3 * outputsPerPin();
        }
        encodeWs2812ShiftLatchPad<Slot>(latchBit_, out);   // close the frame: see encodeRows
    }

    /// Test-only accessors: pin the lane slicing and frame-size arithmetic on the host.
    uint8_t laneCount() const { return laneCount_; }
    /// Which DMA buffer this tick encodes into, 0 or 1. Test-only.
    uint8_t activeForTest() const { return active_; }
    /// Is buffer `i`'s DMA transfer outstanding (awaiting its wait)? Test-only.
    bool inFlightForTest(uint8_t i) const { return inFlight_[i]; }
    /// Lights on lane `i` (0 if out of range). Test-only.
    nrOfLightsType laneLightCount(uint8_t i) const { return i < laneCount_ ? laneCounts_[i] : 0; }
    // Best-effort counters: NOT volatile, which is the wrong tool and deprecated on a compound assign.
    /// Cycles spent gathering a row, accumulated for `ringDbg`.
    static inline uint32_t dbgSegGatherCy = 0;
    /// Cycles spent transposing and emitting a row, accumulated for `ringDbg`.
    static inline uint32_t dbgSegEmitCy = 0;
    /// How many rows the two counters above cover, so `ringDbg` can average them.
    static inline uint32_t dbgSegRows = 0;

    // The prefill-skip needs a constant per-row mask; an empty lane is in no mask at all.
    /// Are all POPULATED strands the same length? Gates the ring's prefill-skip.
    bool MM_RAMFUNC uniformLaneCounts() const {
        nrOfLightsType ref = 0;
        for (uint8_t i = 0; i < laneCount_; i++) {
            if (laneCounts_[i] == 0) continue;   // empty lane: never in any mask
            if (ref == 0) ref = laneCounts_[i];
            else if (laneCounts_[i] != ref) return false;
        }
        return true;
    }
    /// First light index of lane `i`'s slice (0 if out of range). Test-only.
    nrOfLightsType laneStart(uint8_t i) const { return i < laneCount_ ? laneStart_[i] : 0; }
    /// Length of the longest lane: the frame's row count. Test-only.
    nrOfLightsType maxLaneLights() const { return maxLaneLights_; }
    /// Total DMA frame size in bytes (rows + latch pad). Test-only.
    size_t frameBytes() const { return frameBytes_; }

    // The back-pointer's read side: what CRTP inheritance gave a derived class for free.
    /// Bus-bit index of the latch line, shift mode only; a backend reads it through the owner.
    uint8_t latchBit() const { return latchBit_; }
    /// The parsed physical data GPIOs, bus-width bound, for a backend's encode trampoline.
    const uint16_t* laneList() const { return laneList_; }
    /// The live output Correction, which a backend's encode and prefill helpers read through.
    const Correction& correction() const { return correction_; }
    // addControl binds a bool by reference, so the control needs the member's address.
    /// Mutable reference to the ring-snapshot knob, for a backend's addRingControls to bind.
    bool& ringSnapshotRef() { return ringSnapshot; }
    /// The ring snapshot buffer, null off the ring path; a backend's KPI reads its residency.
    const uint8_t* snapshotBuf() const { return snapshotBuf_; }
    /// The wired source buffer (null before setSourceBuffer): same KPI-residency use as snapshotBuf().
    const Buffer* sourceBuffer() const { return sourceBuffer_; }

protected:
    // Re-points by backend, not by index, so a filtered list cannot select a different one.
    void buildPeripheralOptions() {
        // Remember which backend is selected now (by label) so we can re-find it after filtering.
        const char* current = (peripheralSel_ < peripheralOptionCount_) ? peripheralOptions_[peripheralSel_]
                                                                        : nullptr;
        peripheralOptionCount_ = 0;
        for (uint8_t i = 0; i < peripheralRegistryCount_ && peripheralOptionCount_ < kMaxPeripherals; i++) {
            LedPeripheral* probe = peripheralRegistry_[i].make();
            const bool usable = probe && probe->lanesAvailable() > 0;
            delete probe;
            if (!usable) continue;
            peripheralOptions_[peripheralOptionCount_] = peripheralRegistry_[i].label;
            peripheralIndex_[peripheralOptionCount_] = i;
            peripheralOptionCount_++;
        }
        if (peripheralOptionCount_ == 0) {   // no usable backend on this chip (desktop): a single inert row
            peripheralOptions_[0] = "(none)";
            peripheralIndex_[0] = 0;
            peripheralOptionCount_ = 1;
        }
        // Re-point the Select at the same backend it named before (labels are stable), else clamp.
        uint8_t sel = 0;
        if (current)
            for (uint8_t k = 0; k < peripheralOptionCount_; k++)
                if (std::strcmp(peripheralOptions_[k], current) == 0) { sel = k; break; }
        peripheralSel_ = sel;
    }

    // Only the selected peripheral costs memory; a bad index leaves peripheral_ null and idles.
    void swapPeripheral(uint8_t k) {
        // Quiesce the encode worker first: it dereferences peripheral_ every tick.
        if (peripheral_) MoonModule::notifyQuiesceRender();
        deinit();                                   // stop any in-flight transfer on the old bus first
        if (peripheral_) {
            peripheral_->busDeinit();
            if (peripheralOwned_) delete peripheral_;   // never delete a test-borrowed mock
            peripheral_ = nullptr;
        }
        peripheralActiveReg_ = 0xFF;
        peripheralOwned_ = false;
        if (k >= peripheralOptionCount_) return;
        const uint8_t reg = peripheralIndex_[k];
        if (reg >= peripheralRegistryCount_) return;
        peripheral_ = peripheralRegistry_[reg].make();
        if (peripheral_) { peripheral_->attach(this); peripheralActiveReg_ = reg; peripheralOwned_ = true; }
    }

    // A board naming an unavailable default still gets the first usable backend, so it works.
    void selectDefaultPeripheral(const char* label) {
        buildPeripheralOptions();
        peripheralSel_ = 0;   // fallback: the first usable backend (a board naming an absent default
                              // still gets a working driver, and `label == nullptr` means "first").
        if (label)
            for (uint8_t k = 0; k < peripheralOptionCount_; k++)
                if (std::strcmp(peripheralOptions_[k], label) == 0) { peripheralSel_ = k; break; }
        swapPeripheral(peripheralSel_);
    }

    // A no-op when they already agree, so it is cheap on every rebuild.
    void ensurePeripheralMatchesSelection() {
        // Never swap a test-borrowed mock out for a registry backend.
        if (peripheral_ && !peripheralOwned_) return;
        const uint8_t wantReg = (peripheralSel_ < peripheralOptionCount_) ? peripheralIndex_[peripheralSel_]
                                                                          : 0xFF;
        if (peripheral_ && peripheralActiveReg_ == wantReg) return;   // already correct
        swapPeripheral(peripheralSel_);
    }

    // RTTI-free: every module answers hwBlock(), and a non-parallel driver returns None.
    bool siblingClaimsBlock() const {
        if (!peripheral_) return false;
        const LedHwBlock mine = peripheral_->hwBlock();
        if (mine == LedHwBlock::None) return false;
        const MoonModule* p = parent();
        if (!p) return false;
        for (uint8_t i = 0; i < p->childCount(); i++) {
            const MoonModule* sib = p->child(i);
            if (sib == this || !sib || !sib->enabled()) continue;
            // role() confirms it, so this static_cast needs no RTTI.
            if (sib->role() != ModuleRole::Driver) continue;
            if (static_cast<const DriverBase*>(sib)->hwBlock() == mine) return true;
        }
        return false;
    }

public:
    // Gated on inited_, so the claim means "the bus is up" rather than "a backend is selected".
    /// The hardware block this driver is DRIVING, for the sibling claim guard.
    LedHwBlock hwBlock() const override {
        return (inited_ && peripheral_) ? peripheral_->hwBlock() : LedHwBlock::None;
    }

protected:


    /// The runtime backend; null only before the constructor wires one, so every use guards.
    LedPeripheral* peripheral_ = nullptr;

    // peripheralOptions_ is a stable member array because addSelect borrows the pointer.
    uint8_t peripheralSel_ = 0;
    const char* peripheralOptions_[kMaxPeripherals] = {};
    uint8_t peripheralIndex_[kMaxPeripherals] = {};
    uint8_t peripheralOptionCount_ = 0;
    uint8_t peripheralActiveReg_ = 0xFF;   // registry index the LIVE peripheral_ came from (0xFF = none)
    bool peripheralOwned_ = false;         // does the orchestrator own peripheral_ (delete it): false
                                           // for a test-borrowed mock (setPeripheralForTest)

    Buffer* sourceBuffer_ = nullptr;

    LedDriverConfig cfg_;
    bool inited_ = false;
    uint8_t* dmaBuf_ = nullptr;          // platform-owned buffer 0; cached as the "inited" flag
                                         // the per-tick encode target, alternating 0/1
    uint8_t active_ = 0;                 // which DMA buffer this tick encodes into (0/1); stays 0
                                         // in single-buffer mode (no second buffer allocated)
    bool inFlight_[2] = {};              // is buffer i's DMA transfer outstanding (awaiting its wait)
    // Dead transfers, not the healthy `stallUs` wait: 0 on any working bus.
    uint8_t deadFrames_ = 0;
    bool gaveUpReported_ = false;        // one status write per give-up, not one per tick
    // A member because setStatus keeps the POINTER: stack storage would dangle on return.
    /// One buffer for every formatted status this driver reports.
    char statusBuf_[96] = {};
    bool overCapReported_ = false;
    static constexpr uint8_t kDeadFramesBeforeGiveUp = 8;
    // About a second between retries: rare enough not to starve the network, often enough to feel live.
    uint16_t giveUpRetry_ = 0;
    static constexpr uint16_t kGiveUpRetryTicks = 50;
    // One slice PER CORE: a fixed 4-byte stride here boot-looped the SE16 on a 5-channel correction.
    char frameTimeStr_[40] = "—";             // read-only `frameTime` KPI text (refreshed in tick1s); sized
                                         // the worst case plus the 3-byte UTF-8 micro sign
    uint16_t laneList_[kMaxLanes] = {};              // physical data GPIOs (bus width bound)
    uint16_t busPinBuf_[kMaxLanes] = {};             // data pins + latch: the list busInit() builds from
    nrOfLightsType laneCounts_[kMaxStrands] = {};    // per-STRAND light count (expander bound)
    nrOfLightsType laneStart_[kMaxStrands] = {};     // per-STRAND offset into the window
    nrOfLightsType winStart_ = 0;   // first source-buffer light this driver reads (the window)
    nrOfLightsType winLen_ = 0;     // window length (lights), clamped to the buffer
    uint8_t laneCount_ = 0;      // STRAND lanes driven = physPins_ × outputsPerPin() (expanded)
    uint8_t physPins_ = 0;       // physical data GPIOs (== laneCount_ in direct mode)
    uint8_t latchBit_ = 0;       // bus-bit index of the latch line (shift mode only)
    nrOfLightsType maxLaneLights_ = 0;
    size_t frameBytes_ = 0;

    // Only THIS DRIVER'S WINDOW, biased by -winStart_ so encodeRows' index is unchanged.
    uint8_t* snapshotBuf_ = nullptr;      // driver-owned copy of the source WINDOW (ring only)
    size_t   snapshotCap_ = 0;            // allocated capacity, grows to fit the window

    // The DMA buffers count: what a user needs is what choosing this driver costs.
    /// This driver's heap: the scratch, the ring snapshot, and the peripheral's DMA buffers.
    size_t driverHeapBytes() const override {
        size_t dma = 0;
        if (peripheral_) {
            const size_t cap = peripheral_->busCapacity();
            // Count the buffers that EXIST: a readout trusting the request reports unspent memory.
            dma = cap;
            if (peripheral_->busBuffer(1)) dma += cap;
        }
        return DriverBase::driverHeapBytes() + snapshotCap_ + dma;
    }
    // The snapshot is serial; only the ring prime still forks to the core-0 helper.
    const uint8_t* snapCopySrc_ = nullptr;
    uint8_t  snapCopyCh_ = 0;
    const uint8_t* encodeSrc_ = nullptr;  // when non-null, encodeRows reads this (bias-corrected) instead of sourceBuffer_

    // Grow-only and off the hot path, so the render thread only ever memcpys into it.
    /// Resize the ring snapshot to hold this driver's whole window.
    bool ensureSnapshotCap() {
        if (!sourceBuffer_) return true;   // sized on the first build that has a source; harmless if absent
        // Sized by the SOURCE channel count: the copy is raw, with correction fused into the gather.
        const size_t srcCh = sourceBuffer_->channelsPerLight();
        const size_t bytes = static_cast<size_t>(winLen_) * (srcCh ? srcCh : correction_.outChannels);
        if (bytes == 0 || snapshotCap_ >= bytes) return true;
        // INTERNAL RAM first: PSRAM latency costs ~10% of a refill, but slow beats dark.
        uint8_t* grown = static_cast<uint8_t*>(platform::allocInternal(bytes));
        if (!grown) grown = static_cast<uint8_t*>(platform::alloc(bytes));
        if (!grown) return false;
        if (snapshotBuf_) platform::free(snapshotBuf_);
        snapshotBuf_ = grown;
        snapshotCap_ = bytes;
        this->publishHeapBytes();   // the snapshot grew: refresh the memory readout
        return true;
    }

    // The bias pointer is only dereferenced past winStart_, so it never reads out of bounds.
    /// Freeze this driver's window into the snapshot and point the ring's encode at it.
    bool snapshotSourceForRing() {
        encodeSrc_ = nullptr;
        if (!sourceBuffer_ || !sourceBuffer_->data() || !snapshotBuf_) return false;
        const size_t srcCh = sourceBuffer_->channelsPerLight();
        const size_t outCh = correction_.outChannels;
        if (srcCh == 0 || outCh == 0) return false;
        // The source can have shrunk since reinit, which is the hazard this snapshot guards.
        const nrOfLightsType count = sourceBuffer_->count();
        if (winStart_ >= count) return false;
        nrOfLightsType winLights = (winStart_ + winLen_ > count)
                                       ? static_cast<nrOfLightsType>(count - winStart_)
                                       : winLen_;
        // Against the SOURCE stride: outCh would drop the tail whenever an RGB source feeds RGBW.
        if (static_cast<size_t>(winLights) * srcCh > snapshotCap_)
            winLights = static_cast<nrOfLightsType>(snapshotCap_ / srcCh);   // never overrun the sized buffer
        if (winLights == 0) return false;
        // The ISR refill reads a frozen frame while the render loop overwrites the live buffer.
        snapCopySrc_ = sourceBuffer_->data() + static_cast<size_t>(winStart_) * srcCh;
        snapCopyCh_ = static_cast<uint8_t>(srcCh);
        // SERIAL always: forking the copy saturated core 0 and hung the board. The prime still forks.
        copyRange(0, winLights);
        // Raw source bytes, since correction runs later in encodeRows, uniformly.
        if (patternHoldStrand_ >= 0 && static_cast<uint8_t>(patternHoldStrand_) < laneCount_) {
            const uint8_t lane = static_cast<uint8_t>(patternHoldStrand_);
            uint8_t patSrc[8] = {};   // pattern in SOURCE channels (corrected downstream like every light)
            // A fixture with srcCh > 8 would otherwise memcpy past the pattern buffer.
            const size_t patCh = srcCh < sizeof(patSrc) ? srcCh : sizeof(patSrc);
            for (size_t ch = 0; ch < patCh; ch++)
                patSrc[ch] = ch < 3 ? kPatternRGB_[ch] : uint8_t{0};
            const nrOfLightsType laneRows = laneCounts_[lane];
            for (nrOfLightsType row = 0; row < laneRows; row++) {
                if (static_cast<nrOfLightsType>(laneStart_[lane] + row) >= winLights) break;
                std::memcpy(snapshotBuf_ + (static_cast<size_t>(laneStart_[lane]) + row) * srcCh,
                            patSrc, patCh);
            }
        }
        // Bias so encodeRows' unchanged index addresses into the windowed copy.
        encodeSrc_ = snapshotBuf_ - static_cast<size_t>(winStart_) * srcCh;
        return true;
    }

    // MM_RAMFUNC because both cores run it: keep it out of the shared flash cache.
    /// Copy lights [lo, hi) of the window into the snapshot: a raw memcpy at srcCh stride.
    void MM_RAMFUNC copyRange(nrOfLightsType lo, nrOfLightsType hi) {
        const size_t ch = snapCopyCh_;
        if (hi > lo)
            std::memcpy(snapshotBuf_ + static_cast<size_t>(lo) * ch, snapCopySrc_ + static_cast<size_t>(lo) * ch,
                        static_cast<size_t>(hi - lo) * ch);
    }


    // Each half then owns whole cache lines, so the two cores never share one (false sharing).
    /// Split the window near the middle, rounded so the second half starts on a cache line.
    static nrOfLightsType snapLineAlignedHalf(nrOfLightsType winLights, size_t chStride) {
        if (chStride == 0) return winLights / 2;
        // 64 / gcd(64, stride) lights: a plain 64/stride rounds down and misses the line boundary.
        const auto lightsPerAlign = static_cast<nrOfLightsType>(64 / std::gcd(size_t{64}, chStride));
        nrOfLightsType half = winLights / 2;
        half -= half % lightsPerAlign;                                // round DOWN to a line boundary
        if (half == 0) half = lightsPerAlign;                         // never give the helper nothing
        return half < winLights ? half : winLights / 2;               // tiny window: plain (unaligned) halve
    }
    // Which strand carries the pinned pattern (-1 = off), and the bytes the bit-verify expects.
    int16_t patternHoldStrand_ = -1;
    uint8_t kPatternRGB_[3] = {0xA5, 0x00, 0xFF};

    /// The two wirings that exist: strands on the GPIOs, or a 74HCT595 per GPIO.
    uint16_t busPins_[kMaxLanes] = {};   // data pins the live bus/unit was built
                                         // a pin change rebuilds even when the buffer fits
    uint8_t  busLaneCount_ = 0;          // lane count the live bus was built with:
                                         // a lane-count change keeps frameBytes_ but needs a rebuild

    /// The pin-list parser's cap: the peripheral's lane count when narrower, else kMaxLanes.
    uint8_t maxLanesForTarget() const {
        const uint8_t avail = peripheral_ ? peripheral_->lanesAvailable() : 0;
        return (avail > 0 && avail < kMaxLanes) ? avail : kMaxLanes;
    }

    // The one source of the row geometry, so the ring's slice size cannot drift from the frame's.
    static size_t rowBytesFor(uint8_t outCh, uint8_t slotBytes, uint8_t outPerPin) {
        return static_cast<size_t>(outCh) * 24 * slotBytes * outPerPin;
    }
    // Scaled by slotBytes and outPerPin: the shift bus clocks faster, so an unscaled pad is short.
    static size_t padBytesFor(uint8_t slotBytes, uint8_t outPerPin) {
        return static_cast<size_t>(800 + 64) * slotBytes * outPerPin;
    }

    static size_t frameBytesFor(nrOfLightsType maxLights, uint8_t outCh, uint8_t slotBytes,
                                uint8_t outPerPin = 1) {
        if (maxLights == 0 || outCh == 0 || outPerPin == 0) return 0;
        const size_t bytes = static_cast<size_t>(maxLights) * rowBytesFor(outCh, slotBytes, outPerPin)
                             + padBytesFor(slotBytes, outPerPin);
        return (bytes + 63) & ~static_cast<size_t>(63);
    }

    // 0 means no bound. A bounded driver checks this before busInit and idles with a clear status.
    static bool frameFitsDmaBudget(size_t frameBytes, size_t budgetBytes) {
        return budgetBytes == 0 || frameBytes <= budgetBytes;
    }

    // A peripheral fact, independent of the strand count: the driver rounds up and pads the spares.
    uint8_t busWidthPins() const {
        // Data pins, plus the latch lane when a '595 expander is in use.
        const uint8_t needed = static_cast<uint8_t>(physPins_ + (pinExpanderMode() ? 1 : 0));
        return needed <= 8 ? uint8_t{8} : uint8_t{16};
    }

    // Data pins, then the latch in shift mode, then every spare lane parked on WR.
public:
    // Public because a backend reaches it through owner_; the rest of this block stays protected.
    /// The bus-geometry accessors a backend builds its bus from.
    const uint16_t* busPinList() {
        const uint8_t width = busPinCount();
        const uint16_t clockPin = peripheral_ ? peripheral_->clockPinForBus() : laneList_[0];
        for (uint8_t i = 0; i < width && i < kMaxLanes; i++) {
            if (i < physPins_)                        busPinBuf_[i] = laneList_[i];   // data
            else if (pinExpanderMode() && i == latchBit_)   busPinBuf_[i] = static_cast<uint16_t>(latchPin);
            else                                      busPinBuf_[i] = clockPin;
        }
        return busPinBuf_;
    }
    // Only a backend that cannot leave a lane unconnected needs the spare lanes padded.
    /// How many lanes the PERIPHERAL is handed.
    uint8_t busPinCount() const {
        const uint8_t width = busWidthPins();
        if (peripheral_ && !peripheral_->spareLanesNeedPad()) {
            const uint8_t real = static_cast<uint8_t>(physPins_ + (pinExpanderMode() ? 1 : 0));
            return real < width ? real : width;
        }
        return width;
    }
    /// How much faster the bus clocks per slot: a '595 needs 8 shift cycles for the same 375 ns.
    uint8_t busClockMultiplier() const { return outputsPerPin(); }
    /// Bytes per bus slot, keyed on the PHYSICAL pin count rather than the lane count.
    uint8_t slotBytes() const { return busWidthPins() > 8 ? 2 : 1; }
protected:

    // Sized to outCh rather than a fixed 4, which is what lets any channel count lay out safely.
    void prepareWire(uint8_t outCh) {
        // Sized for STRANDS and one slice per core: the ring's prime runs encodeRows on both.
        ensureWire(static_cast<size_t>(kMaxStrands) * (outCh ? outCh : 1) * platform::kMaxCores);
    }

    // Off the hot path; on error the driver idles with the parse literal in its status.
    bool parseConfig() {
        laneCount_ = 0;
        physPins_ = 0;
        maxLaneLights_ = 0;
        frameBytes_ = 0;
        uint8_t n = 0;
        const char* err = parsePinList(pins, laneList_, maxLanesForTarget(), n);
        // Degrades to direct SILENTLY: the toggle is hidden there, so an error would be unfixable.
        if (!err && pinExpanderMode() && latchPin < 0) err = "the 74HCT595 expander needs a latchPin";
        // The BUS width is a peripheral fact and the PIN COUNT a board fact; the driver reconciles.
        if (peripheral_ && peripheral_->powerOfTwoBus()) {
            const uint8_t maxData = static_cast<uint8_t>(kMaxLanes - (pinExpanderMode() ? 1 : 0));
            if (!err && (n == 0 || n > maxData))
                err = pinExpanderMode() ? "shift mode needs 1..15 data pins (one per populated 74HCT595)"
                                  : "i80 bus needs 1..16 pins";
        }
        // The latch drives its own bus bit, so a data pin there would carry the latch waveform.
        if (!err && pinExpanderMode()) {
            for (uint8_t i = 0; i < n; i++)
                if (laneList_[i] == static_cast<uint16_t>(latchPin)) {
                    err = "latchPin collides with a data pin";
                    break;
                }
        }
        // Checked before the per-lane warnings: a broken bus is worse than a garbled lane.
        if (!err && peripheral_) err = peripheral_->validateBusFatal();
        // A WARNING, not a blocker: parking WR or DC on an unused lane is a legitimate choice.
        const char* warn = (err || !peripheral_) ? nullptr : peripheral_->validateBusPins(laneList_, n);
        // From here on "lane" means a STRAND, and physPins_ holds the GPIO count.
        const uint8_t lanes = static_cast<uint8_t>(n * outputsPerPin());
        if (!err && lanes > kMaxStrands) err = "too many strands (pins × 8 through the expander)";
        if (!err) {
            // Distribute over this driver's window slice, not the whole buffer.
            const nrOfLightsType bufN = sourceBuffer_ ? sourceBuffer_->count() : 0;
            windowSlice(bufN, winStart_, winLen_);
            const char* clampWarn = nullptr;
            // A clamped lane drives its ceiling rather than choking a grid onto one line.
            err = assignCounts(ledsPerPin, lanes, winLen_, laneCounts_, kMaxWs2812LedsPerPin,
                               &clampWarn);
            if (clampWarn) warn = clampWarn;
        }
        if (err) {
            setConfigErr(err);
            return false;
        }
        physPins_ = n;
        laneCount_ = lanes;
        // The latch takes the bus bit above the data pins, which are bits 0..n-1.
        latchBit_ = n;
        nrOfLightsType start = 0;
        for (uint8_t i = 0; i < laneCount_; i++) {
            laneStart_[i] = start;
            start = static_cast<nrOfLightsType>(start + laneCounts_[i]);
            if (laneCounts_[i] > maxLaneLights_) maxLaneLights_ = laneCounts_[i];
        }
        const uint8_t outCh = correction_.outChannels;
        // slotBytes() now reflects the real BUS width: the x8 lands in the slot COUNT instead.
        frameBytes_ = frameBytesFor(maxLaneLights_, outCh, slotBytes(), outputsPerPin());
        overCapReported_ = false;   // a new geometry re-earns its verdict: lowering the count recovers
        // Stride outCh, so any channel count fits without the old fixed-4-byte overflow.
        prepareWire(outCh);
        clearConfigErr();
        // A lane clamped to the WS2812 ceiling still drives: Warning, not error (see RmtLed).
        setConfigWarn(warn);
        // Real consumption rather than a guess from grid x pins; an idle driver stays statusless.
        if (!warn && start > 0) setDrivingInfo(start, winLen_, outCh);
        return true;
    }

    // Bus and buffer are fused: max_transfer_bytes is fixed at creation, so both rebuild together.

    void reinit() {
        if (!peripheral_ || peripheral_->lanesAvailable() == 0) return;
        // One block per chip, so a sibling holding it means idle with a status rather than fight.
        if (siblingClaimsBlock()) {
            deinit();
            setConfigErr("peripheral already in use by another driver — pick a different one");
            return;
        }
        // A rebuild is the user fixing the setting, so a driver that gave up starts again.
        deadFrames_ = 0;
        gaveUpReported_ = false;
        giveUpRetry_ = 0;
        // Drain first: a rebuild must not race a transfer still reading the old buffer.
        drainInFlight();
        if (laneCount_ == 0 || frameBytes_ == 0) { deinit(); return; }
        // EXACT-match reuse: a fixed-size peripheral goes invalid when reused at another size.
        if (peripheral_->wantsRing()) {
            deinit();
            const uint8_t outCh = correction_.outChannels;
            // Rows only: a ring buffer carries no latch pad, since stopping the peripheral resets.
            const size_t rowBytes = rowBytesFor(outCh, slotBytes(), outputsPerPin());
            // Sized HERE, off the hot path: if it will not allocate, the ring cannot run at all.
            if (!ringSnapshot) freeSnapshot();
            if (peripheral_->busInitRing(rowBytes, static_cast<uint32_t>(maxLaneLights_))
                && (!ringSnapshot || ensureSnapshotCap())) {
                inited_ = true;
                dmaBuf_ = peripheral_->busBuffer(0);   // ring[0]: a real pointer, the "inited" sentinel
                for (uint8_t i = 0; i < kMaxLanes; i++) busPins_[i] = laneList_[i];
                busLaneCount_ = laneCount_;
                peripheral_->recordBusPins();
                if (status() == peripheral_->initFailMsg()) clearStatus();
                // The ring's regime is a platform fact that exists only after the build.
                if (const char* mode = peripheral_->busRingMode();
                    mode && status() && std::strncmp(status(), "driving ", 8) == 0) {
                    nrOfLightsType driven = 0;
                    for (uint8_t i = 0; i < laneCount_; i++) driven += laneCounts_[i];
                    if (driven > 0) setDrivingInfo(driven, winLen_, correction_.outChannels, mode);
                }
                return;
            }
            // busDeinit DIRECTLY: inited_ is false here, so deinit would leak the built ring.
            peripheral_->busDeinit();
            deinit();
        }

        // Whole-frame from here: free any leftover snapshot, which this path never reads.
        freeSnapshot();

        const bool haveSecond = peripheral_->busBuffer(1) != nullptr;
        // The one "want a second buffer?" decision: the toggle AND a peripheral that supports it.
        const bool wantSecond = doubleBuffer && peripheral_->supportsDoubleBuffer();
        if (inited_ && !peripheral_->busIsRing() && peripheral_->busCapacity() == frameBytes_
            && busPinsCurrent() && busLaneCount_ == laneCount_ && haveSecond == wantSecond) {
            // Clear stale latch-pad bytes in BOTH buffers (buffer 1 is null in single-buffer mode).
            std::memset(dmaBuf_, 0, peripheral_->busCapacity());
            if (uint8_t* b1 = peripheral_->busBuffer(1)) std::memset(b1, 0, peripheral_->busCapacity());
            prefillShiftConstantsIfNeeded();   // the zeroing above wiped them
            return;
        }
        deinit();
        // Pre-checked BEFORE busInit: the failing path busy-waits to a watchdog reset. COLD PATH.
        {
            const uint16_t* bus = busPinList();
            const uint8_t width = busPinCount();
            // The bus LANES here; validateBusFatal already owns the WR/DC pair.
            for (uint8_t i = 0; i < width && i < kMaxLanes; i++) {
                const uint16_t pin = bus[i];
                if (pin > 48) continue;                       // unset/NC: nothing routed
                // A pin the package lacks fails as silently as a flash pin, so both are refused.
                const char* why = platform::gpioRefusal(static_cast<uint8_t>(pin));
                if (!why) continue;
                std::snprintf(statusBuf_, sizeof(statusBuf_),
                              "GPIO %u %s - pick another pin", unsigned(pin), why);
                setStatus(statusBuf_, Severity::Error);
                deinit();
                return;
            }
        }
        if (const size_t budget = peripheral_->dmaBudgetBytes();
            !frameFitsDmaBudget(frameBytes_, budget)) {
            // Measured against the DECLARED budget, since no bus is up to report its capacity.
            reportOverCapacity(correction_.outChannels, budget);
            return;
        }
        // OFF costs exactly one DMA buffer and no async overhead.
        inited_ = peripheral_->busInit(frameBytes_, wantSecond);
        dmaBuf_ = inited_ ? peripheral_->busBuffer(0) : nullptr;
        if (inited_) {
            for (uint8_t i = 0; i < kMaxLanes; i++) busPins_[i] = laneList_[i];
            busLaneCount_ = laneCount_;
            peripheral_->recordBusPins();   // i80 also stores WR/DC; Parlio no-op
            prefillShiftConstantsIfNeeded();
        }
        if (!inited_) {
            clearFailBuf();
            setStatus(peripheral_->initFailMsg(), Severity::Error);
        } else if (status() == peripheral_->initFailMsg()) {
            clearStatus();
        }
    }

    void deinit() {
        if (!peripheral_ || peripheral_->lanesAvailable() == 0) return;
        // Freeing while a transfer reads is a use-after-free, so drain first; a no-op when idle.
        drainInFlight();
        if (inited_) peripheral_->busDeinit();
        inited_ = false;
        dmaBuf_ = nullptr;
        active_ = 0;              // next init starts on buffer 0
        inFlight_[0] = inFlight_[1] = false;
        busLaneCount_ = 0;
        // A stale encodeSrc_ surviving a ring-to-whole-frame switch would read a freed snapshot.
        encodeSrc_ = nullptr;
        // wire_ is NOT freed here: reinit calls this after parseConfig sized it. release() frees it.
    }

    bool busPinsCurrent() const {
        for (uint8_t i = 0; i < laneCount_; i++)
            if (busPins_[i] != laneList_[i]) return false;
        return peripheral_ && peripheral_->extraBusPinsCurrent();   // i80 also checks WR/DC
    }

    // Builds the REAL frame on a private bus and verifies every captured bit.

    // Driver-agnostic, so every family shares this one path, unlike busLoopback.
    /// Intrusive ride: arm the RX on the live wire and bit-verify, transmitting nothing.
    platform::RmtLoopbackResult busLoopbackRide(const uint8_t* sent, uint8_t sentLen,
                                                size_t dataBytes, uint8_t rowBits) {
        return platform::ws2812LoopbackRide(static_cast<uint16_t>(loopbackRxPin), sent, sentLen,
                                            dataBytes, rowBits, busClockMultiplier());
    }

    /// Intrusive loopback: verify the live pipeline's wire, building no bus and freeing no ring.
    void runIntrusiveLoopback(nrOfLightsType lights, uint8_t outCh) {
        if (loopbackRxPin < 0) {
            clearFailBuf();
            setStatus("loopback: set loopbackRxPin (jumper it to the tapped strand)", Severity::Status);
            return;
        }
        // The tapped strand: loopbackStrand in expander mode (any '595 output), else lane 0 (direct).
        const uint8_t strand = pinExpanderMode() && loopbackStrand < laneCount_ ? loopbackStrand
                                                                                : uint8_t{0};
        // Width-independent, like the private-bus path: the capture derives its bit count.
        const size_t dataBytes = static_cast<size_t>(lights) * outCh * 24;
        // No deinit and no alloc: the running pipeline keeps rendering while the pattern rides it.
        patternHoldStrand_ = static_cast<int16_t>(strand);
        platform::delayMs(40);   // a handful of 100 fps frames so the held pattern is on the wire before capture
        const uint8_t pat[3] = {kPatternRGB_[0], kPatternRGB_[1], kPatternRGB_[2]};
        const auto r = busLoopbackRide(pat, outCh < 3 ? outCh : uint8_t{3}, dataBytes,
                                       static_cast<uint8_t>(outCh * 8));
        patternHoldStrand_ = -1;   // release the hold: the strand returns to the live effect next frame
        // Report with the shared verdict formatter (same status strings as the private-bus path).
        reportLoopbackResult(r, outCh);
    }

    void runLoopbackSelfTest() {
        if (!peripheral_ || peripheral_->lanesAvailable() == 0) {
            clearFailBuf();
            setStatus("loopback: not supported on this platform", Severity::Warning);
            return;
        }
        if (laneCount_ == 0) {
            clearFailBuf();
            setStatus("loopback: no valid pins", Severity::Warning);
            return;
        }
        // An unset rxPin has nothing to capture on, and the cast would turn -1 into a bogus pin.
        if (loopbackRxPin < 0) {
            clearFailBuf();
            setStatus("loopback: set loopbackRxPin (jumper it to the TX pin)", Severity::Status);
            return;
        }
        const uint8_t outCh = correction_.outChannels;
        if (frameBytes_ == 0 || maxLaneLights_ == 0 || outCh == 0) {
            clearFailBuf();
            setStatus("loopback: no lights to encode", Severity::Warning);
            return;
        }
        // Capped to kLoopbackTestLights, which its declaration explains.
        const nrOfLightsType lights =
            maxLaneLights_ < kLoopbackTestLights ? maxLaneLights_ : kLoopbackTestLights;

        // Reaches the ring at zero extra RAM, which a private-bus rebuild cannot on a fragmented heap.
        if (loopbackIntrusive) { runIntrusiveLoopback(lights, outCh); return; }
        // Per-driver: the i80 loopback needs the full width, Parlio's 1-lane unit stays 8-bit.
        const uint8_t sb = peripheral_->loopbackFullWidth() ? slotBytes() : 1;
        // The expander multiplies the SLOT COUNT: omitting it transmits a truncated waveform.
        const uint8_t opp = outputsPerPin();
        const size_t perLightBytes = static_cast<size_t>(outCh) * 8 * 3 * sb * opp;
        // Shift mode writes one trailing latch word, so the buffer needs one Slot beyond the rows.
        const size_t testFrameBytes = static_cast<size_t>(lights) * perLightBytes
                                      + (pinExpanderMode() ? sb : 0);
        // The genuine transfer, so the test covers what the render loop sends. Off the hot path.
        auto* frame = static_cast<uint8_t*>(platform::alloc(testFrameBytes));
        if (!frame) {
            clearFailBuf();
            setStatus("loopback: out of memory", Severity::Error);
            return;
        }
        std::memset(frame, 0, testFrameBytes);
        // Indexed BY STRAND, and shift mode can use any strand, so size for the full range.
        const uint8_t patStrand = pinExpanderMode() && loopbackStrand < kMaxStrands ? loopbackStrand
                                                                              : uint8_t{0};
        const size_t wireBytes = static_cast<size_t>(patStrand + 1) * outCh;
        auto* wire = static_cast<uint8_t*>(platform::alloc(wireBytes));
        if (!wire) { platform::free(frame); clearFailBuf(); setStatus("loopback: out of memory", Severity::Error); return; }
        std::memset(wire, 0, wireBytes);
        uint8_t* pat = wire + static_cast<size_t>(patStrand) * outCh;   // the chosen strand's slot
        pat[0] = 0xA5; if (outCh > 1) pat[1] = 0x00; if (outCh > 2) pat[2] = 0xFF;   // R/G/B pattern
        // At the operational width; in shift mode strand 0 is that register's Q7, the pin to tap.
        if (pinExpanderMode()) {
            if (sb == 1) encodeLoopbackFrameShift<uint8_t>(frame, wire, outCh, lights);
            else         encodeLoopbackFrameShift<uint16_t>(frame, wire, outCh, lights);
        } else if (sb == 1) {
            encodeLoopbackFrame<uint8_t>(frame, wire, outCh, lights);
        } else {
            encodeLoopbackFrame<uint16_t>(frame, wire, outCh, lights);
        }
        platform::free(wire);
        // Width-INDEPENDENT: a wider bus fattens each slot but clocks the same bits out of lane 0.
        const size_t dataBytes = static_cast<size_t>(lights) * outCh * 24;
        deinit();   // free the live bus; the test builds its own on the data pins
        // DIRECT mode only: in shift mode the signal comes off a '595 output, not a GPIO.
        const uint16_t realLane0 = laneList_[0];
        if (loopbackTxPin >= 0 && !pinExpanderMode()) laneList_[0] = static_cast<uint16_t>(loopbackTxPin);
        const auto r = peripheral_->busLoopback(frame, testFrameBytes, dataBytes,
                                                static_cast<uint8_t>(outCh * 8));
        laneList_[0] = realLane0;
        platform::free(frame);
        // Result first, then reinit: an unusable driver matters more than a passed self-test.
        reportLoopbackResult(r, outCh);
        reinit();
    }

    // Names the fault CLASS, so an empty capture reads as wiring rather than a misleading bad bit.
    /// Turn a loopback result into a status string, shared by both test paths.
    void reportLoopbackResult(const platform::RmtLoopbackResult& r, uint8_t outCh) {
        if (!r.jumperDetected) {
            clearFailBuf();
            setStatus("loopback: jumper not detected", Severity::Warning);
        } else if (r.pass) {
            clearFailBuf();
            setStatus("loopback PASS", Severity::Status);
        } else if (failBufEnsure()) {
            if (r.bitsChecked == 0) {
                std::snprintf(failBuf_, kFailBufLen,
                              "no capture: %u sym idle=%d tx %u/%uus",
                              static_cast<unsigned>(r.capturedSymbols),
                              static_cast<int>(r.rxIdleLevel),
                              static_cast<unsigned>(r.txWallUs),
                              static_cast<unsigned>(r.txExpectUs));
            } else {
                const unsigned rowBits = static_cast<unsigned>(outCh) * 8u;
                const unsigned badLight = rowBits ? r.firstBadBit / rowBits : 0u;
                std::snprintf(failBuf_, kFailBufLen,
                              "loopback FAIL: bad bit %u/%u (light %u)",
                              static_cast<unsigned>(r.firstBadBit),
                              static_cast<unsigned>(r.bitsChecked), badLight);
            }
            setStatus(failBuf_, Severity::Error);
        } else {
            setStatus("loopback FAIL", Severity::Error);
        }
    }
};

} // namespace mm
