#pragma once

#include <cstddef>
#include <cstdint>
#include "core/module/Control.h"          // ControlList: a backend appends its own controls into the shared list
#include "platform/platform.h"     // RmtLoopbackResult

namespace mm {

class ParallelLedDriver;   // the orchestrator; a backend reads shared state through this back-pointer

/// The physical peripheral block a backend drives; two backends on one block conflict.
enum class LedHwBlock : uint8_t { None = 0, LcdCam, I2s, Parlio, GpioDma };

/// A parallel-WS2812 output peripheral, behind a runtime strategy interface.
///
/// ParallelLedDriver owns the controls, lifecycle, tick and encode machinery, driving one peripheral chosen at runtime. The peripheral supplies the variant operations: bring the bus up, hand back its DMA buffer, transmit, tear down.
///
/// Not a hot-path boundary: every method here is called per frame or per reinit, never per light. The per-light encode writes into the raw buffer and never calls back into the peripheral.
///
/// It is the Strategy pattern, in the shape this project's other pluggable seams take.
class LedPeripheral {
public:
    /// Virtual, since the orchestrator owns every backend through this interface.
    virtual ~LedPeripheral() = default;

    /// Bind the peripheral to its orchestrator, which it reads its shared state through.
    void attach(ParallelLedDriver* owner) { owner_ = owner; }

    // --- Static descriptors: per-peripheral constants the orchestrator reads through the interface ---
    /// Parallel lanes this peripheral's silicon provides on the current chip (0 = not this chip).
    virtual uint8_t lanesAvailable() const MM_NONBLOCKING = 0;
    /// Can this peripheral host the 74HCT595 pin expander? (Needs a DMA that reaches PSRAM.)
    virtual bool supportsPinExpander() const = 0;
    /// Whether this peripheral can run the async double-buffer, overlapping encode and wire.
    virtual bool supportsDoubleBuffer() const { return true; }
    /// Does the bus width round up to a power of two (8/16), or is it the exact pin count?
    virtual bool powerOfTwoBus() const = 0;
    /// The status message when bus init fails on this peripheral.
    virtual const char* initFailMsg() const = 0;

    /// Whether a peripheral a previous init lost to a sibling is free again, so the driver retries.
    virtual bool busContentionCleared() const { return false; }
    /// Whether the loopback self-test must build a full-width bus rather than a single lane.
    virtual bool loopbackFullWidth() const = 0;
    /// The physical block this backend drives, which the claim guard refuses to double up.
    virtual LedHwBlock hwBlock() const = 0;

    /// Create the bus and its DMA buffers, optionally with the double-buffer's second frame.
    virtual bool busInit(size_t frameBytes, bool wantSecondBuffer) = 0;
    /// Tear down the bus and its DMA buffer(s).
    virtual void busDeinit() = 0;
    /// DMA buffer `i` (0/1) the encoder writes into; buffer 1 is null in single-buffer mode.
    virtual uint8_t* busBuffer(uint8_t i) = 0;
    /// Per-buffer byte capacity (fixed at bus creation; both buffers equal).
    virtual size_t busCapacity() const = 0;
    /// The heap this bus holds, which the driver's memory readout shows: by default each buffer that exists, at its capacity.
    virtual size_t busHeapBytes() { return busCapacity() * (busBuffer(1) ? 2 : 1); }
    /// Start the transfer of the first `bytes` of DMA buffer `i`, reporting whether it began.
    virtual bool busTransmit(uint8_t i, size_t bytes) = 0;
    /// Block up to `ms` for buffer `i`'s in-flight transfer to complete.
    virtual bool busWait(uint8_t i, uint32_t ms) = 0;
    /// The most recent DMA transfer's wire time (µs): the WS2812 output floor.
    virtual uint32_t busLastTransmitUs() const = 0;
    /// Run the loopback self-test on this peripheral (each builds its own bus, per loopbackFullWidth).
    virtual platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                                    size_t dataBytes, uint8_t rowBits) = 0;

    /// Bring the bus up as a streaming ring, returning false when it will not fit.
    virtual bool busInitRing(size_t /*rowBytes*/, uint32_t /*totalRows*/) { return false; }
    /// Send one frame on the ring (prime + arm + ISR refill). False if the ring isn't up.
    virtual bool busTransmitRing() { return false; }
    /// Is the live bus a streaming ring?
    virtual bool busIsRing() const MM_NONBLOCKING { return false; }
    /// Should reinit build a ring for the current config instead of the whole-frame path?
    virtual bool wantsRing() const { return false; }
    /// The ring's active-mode label for the status line, or nullptr for the whole-frame path.
    virtual const char* busRingMode() const { return nullptr; }
    /// Append this peripheral's ring controls into the shared list. Default: none.
    virtual void addRingControls(ControlList& /*controls*/) {}
    /// Refresh any peripheral-specific read-only KPIs (the ring diagnostic). Default: none.
    virtual void refreshBusKpi() {}
    /// Is the core-0 fork-join snapshot helper up (dual-core prime)? Default: no helper.
    virtual bool snapHelperReady() const { return false; }

    /// Append this peripheral's own bus-pin controls into the shared list.
    virtual void addBusControls(ControlList& /*controls*/) {}
    /// Does a change to control `name` require a bus rebuild (a bus pin changed)? Default: no.
    virtual bool busControlTriggersBuild(const char* /*name*/) const { return false; }
    /// Snapshot the current bus pins so extraBusPinsCurrent can detect a later change. Default: none.
    virtual void recordBusPins() {}
    /// Are the recorded bus pins still current (no un-applied change)? Default: always current.
    virtual bool extraBusPinsCurrent() const { return true; }
    /// A per-peripheral fatal validation (returns a status message) run before bus init. Default: ok.
    virtual const char* validateBusFatal() const { return nullptr; }
    /// A per-peripheral lane-pin validation (returns a warning): e.g. a data pin colliding with WR.
    virtual const char* validateBusPins(const uint16_t* /*lanes*/, uint8_t /*n*/) const { return nullptr; }
    /// The GPIO the bus parks spare lanes on, reached only where the bus rounds wider.
    virtual uint16_t clockPinForBus() const { return 0; }
    // False keeps a spare lane off the pin map, so it cannot drive a pad another peripheral owns.
    /// Whether a spare bus lane must be parked on a real GPIO.
    virtual bool spareLanesNeedPad() const { return true; }
    /// The whole-frame DMA byte budget; 0 means no bound, as on a PSRAM-capable peripheral.
    virtual size_t dmaBudgetBytes() const { return 0; }

protected:
    ParallelLedDriver* owner_ = nullptr;
};

} // namespace mm
