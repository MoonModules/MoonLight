#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared driver body + LedPeripheral
#include "platform/platform_leds.h"   // spiWs2812Init, spiWs2812Buffer, spiWs2812BufferCapacity

namespace mm {

/// Output driver: parallel WS2812B over a general-purpose SPI host, its data lines the lanes: up to four, or eight where the host runs octal. The shared body (slicing, encode, single-shot DMA) lives in ParallelLedDriver.
///
/// @technical
///
/// The DMA streams a whole frame, so the output keeps no deadline mid-frame, whatever else holds the interrupts off. The frame is the driver's byte slots, packed in the platform to the host's line width; the platform half says how.
class SpiPeripheral : public LedPeripheral {
public:
    /// SPI data lines this chip's hosts drive; 0 makes the orchestrator's guards hold the driver inert.
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return platform::spiLanes; }
    /// Whether the bus width must round to a power of two. Up to four strands any count rides the next SPI width, spare lines carrying zeros; past four it takes all eight.
    bool powerOfTwoBus() const override { return false; }
    /// Whether the loopback needs the full operational bus width. It does not.
    bool loopbackFullWidth() const override { return false; }
    /// The peripheral block this backend claims, for the sibling guard.
    LedHwBlock hwBlock() const override { return LedHwBlock::Spi; }
    /// What the status says when the bus will not come up.
    const char* initFailMsg() const override { return "SPI init failed: 1 to 4 strands or 8, pins, memory, or every SPI host taken"; }

    /// The WS2812 slot rate the other backends use, 375 ns, which the SPI clock divides to from 80 MHz (/30).
    static constexpr uint32_t kClockHz = 2'666'666;

    /// Whether a 74HCT595 pin expander can ride this bus. It cannot: it needs a latch and a clock line.
    bool supportsPinExpander() const override { return false; }

    /// Open a free SPI host and its DMA buffers for `frameBytes`, clocked at kClockHz.
    bool busInit(size_t frameBytes, bool wantSecondBuffer) override {
        return platform::spiWs2812Init(spi_, owner_->laneList(), owner_->laneCount(), kClockHz, frameBytes, wantSecondBuffer);
    }
    /// DMA buffer `i` (0 or 1) the orchestrator encodes into; 1 is null in single-buffer mode.
    uint8_t* busBuffer(uint8_t i) override        { return platform::spiWs2812Buffer(spi_, i); }
    /// The per-buffer byte capacity, fixed at bus creation.
    size_t   busCapacity() const override         { return platform::spiWs2812BufferCapacity(spi_); }
    /// The largest frame of byte slots one transfer carries: the hardware's packed ceiling, times the slots a packed byte holds.
    size_t   dmaBudgetBytes() const override {
        const size_t cap = platform::spiMaxTransferBytes();
        return cap ? cap * 8 / platform::spiLinesFor(owner_->laneCount()) : 0;
    }
    /// Pack buffer `i`'s slots and start its transfer, returning whether it began.
    bool  busTransmit(uint8_t i, size_t bytes) override { return platform::spiWs2812Transmit(spi_, i, bytes); }
    /// Block up to `ms` for buffer `i`'s in-flight transfer to complete.
    bool  busWait(uint8_t i, uint32_t ms) override      { return platform::spiWs2812Wait(spi_, i, ms); }
    /// The most recent transfer's wire time (us): the WS2812 output floor.
    uint32_t busLastTransmitUs() const override         { return platform::spiWs2812LastTransmitUs(spi_); }
    /// Release the SPI host and its DMA buffers.
    void     busDeinit() override                 { platform::spiWs2812Deinit(spi_); }

    /// No private-bus loopback: the driver's intrusive loopback verifies the live wire on any backend.
    platform::RmtLoopbackResult busLoopback(const uint8_t*, size_t, size_t, uint8_t) override { return {}; }

private:
    platform::SpiWs2812Handle spi_;
};

/// Register the SPI backend; ParallelLedDriver drives it via `peripheral`.
inline const bool kSpiPeripheralRegistered =
    ParallelLedDriver::registerPeripheral("SPI", []() -> LedPeripheral* { return new SpiPeripheral(); });

} // namespace mm
