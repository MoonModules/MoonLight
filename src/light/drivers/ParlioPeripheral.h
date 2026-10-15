#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared driver body + LedPeripheral
#include "platform/platform.h"


namespace mm {

/// Output driver: parallel WS2812B over the ESP32-P4 Parlio (Parallel IO) TX peripheral, the P4's scale path and sibling of I80Peripheral. The shared body (slicing, encode, single-shot DMA, loopback) lives in ParallelLedDriver.
///
/// @attribution hpwit, FastLED
///
/// @moreinfo
///
/// Parlio is the simpler peripheral, so this backend adds less than the i80 one: it generates the pixel clock itself and its bus width IS the pin count. The user names only the pins that drive a strand.
///
/// Every control defaults to unset, because the strand is user-soldered and a hard-coded pin could drive one committed elsewhere. The P4-NANO bench uses pins 20-27 with loopbackRxPin 33, its clear GPIOs being 20-27, 32-33 and 39-48.
class ParlioPeripheral : public LedPeripheral {
public:
    /// Parlio lanes this chip provides; 0 makes the orchestrator's guards hold the driver inert.
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return platform::parlioLanes; }
    /// Whether the bus width must round to a power of two. It need not: 1 to 16 lanes are valid.
    bool powerOfTwoBus() const override { return false; }
    /// Whether the loopback needs the full operational bus width. It does not.
    bool loopbackFullWidth() const override { return false; }
    /// The peripheral block this backend claims, for the sibling guard.
    LedHwBlock hwBlock() const override { return LedHwBlock::Parlio; }
    /// What the status says when the bus will not come up.
    const char* initFailMsg() const override { return "Parlio init failed, check pins / memory"; }

    // The P4 Parlio's 160 MHz PLL divides to this exactly (/60); another rate needs that checked.
    /// The WS2812 slot rate: 375 ns at 2.67 MHz, the same slot the LCD backend uses.
    static constexpr uint32_t kClockHz = 2'666'666;

    /// No bus controls: Parlio has no sacrificial clock or DC pins.
    void addBusControls(ControlList&) override {}
    /// No extra bus controls, so none can trigger a rebuild.
    bool busControlTriggersBuild(const char*) const override { return false; }
    /// No extra pins to record (Parlio has no WR/DC).
    void recordBusPins() override {}
    /// No extra pins to track, so they are always current.
    bool extraBusPinsCurrent() const override { return true; }

    // A x8 fan-out frame is ~145 KB against a 65,535-byte transfer cap: chunked transfer, not a flag.
    /// Whether a 74HCT595 pin expander can ride this bus. On Parlio it cannot.
    bool supportsPinExpander() const override { return false; }

    /// Create the Parlio bus and its DMA buffers for `frameBytes`, clocked at kClockHz.
    bool busInit(size_t frameBytes, bool wantSecondBuffer) override {
        return platform::parlioWs2812Init(parlio_, owner_->laneList(), owner_->laneCount(),
                                          kClockHz, frameBytes, wantSecondBuffer);
    }
    /// DMA buffer `i` (0 or 1) the orchestrator encodes into; 1 is null in single-buffer mode.
    uint8_t* busBuffer(uint8_t i) override        { return platform::parlioWs2812Buffer(parlio_, i); }
    /// The per-buffer byte capacity (fixed at bus creation; both buffers equal).
    size_t   busCapacity() const override         { return platform::parlioWs2812BufferCapacity(parlio_); }
    // Asked, not hard-coded: reinit() refuses an oversized frame before the alloc fails (issue #44).
    /// The largest frame one transfer can carry.
    size_t   dmaBudgetBytes() const override      { return platform::parlioMaxTransferBytes(); }
    /// Start the autonomous transfer of the first `bytes` of DMA buffer `i`, returning whether it began.
    bool  busTransmit(uint8_t i, size_t bytes) override { return platform::parlioWs2812Transmit(parlio_, i, bytes); }
    /// Block up to `ms` for buffer `i`'s in-flight transfer to complete.
    bool  busWait(uint8_t i, uint32_t ms) override      { return platform::parlioWs2812Wait(parlio_, i, ms); }
    /// The most recent DMA transfer's wire time (us): the WS2812 output floor.
    uint32_t busLastTransmitUs() const override         { return platform::parlioWs2812LastTransmitUs(parlio_); }
    /// Tear down the Parlio bus and its DMA buffer.
    void     busDeinit() override                 { platform::parlioWs2812Deinit(parlio_); }

    /// Run the loopback self-test on a private 1-lane unit.
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) override {
        return platform::parlioWs2812Loopback(owner_->laneList(), owner_->laneCount(),
                                              static_cast<uint16_t>(owner_->loopbackRxPin),
                                              frame, frameBytes, dataBytes, rowBits);
    }

private:
    platform::ParlioWs2812Handle parlio_;
};

/// Register the Parlio backend, on P4 silicon only; ParallelLedDriver drives it via `peripheral`.
inline const bool kParlioPeripheralRegistered =
    ParallelLedDriver::registerPeripheral("Parlio", []() -> LedPeripheral* { return new ParlioPeripheral(); });

} // namespace mm
