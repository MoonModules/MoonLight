#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared driver body + LedPeripheral
#include "platform/platform.h"

namespace mm {

/// Output driver: parallel WS2812B by one DMA channel writing each slot into a GPIO port, paced by a timer; the Teensy 4.1's path. The shared body (slicing, encode, loopback) lives in ParallelLedDriver.
///
/// Prior art: OctoWS2811 and ObjectFLED (MIT), which drive WS2812 from Teensy GPIO by timer-paced DMA: architecture studied, never copied.
class GpioDmaPeripheral : public LedPeripheral {
public:
    /// The lanes this chip provides; 0 makes the orchestrator's guards hold the driver inert.
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return platform::gpioDmaLanes; }
    /// The slots are bytes or halfwords, so the bus width rounds to 8 or 16.
    bool powerOfTwoBus() const override { return true; }
    /// Lanes are the pins named, so a spare slot bit drives nothing and needs no pad.
    bool spareLanesNeedPad() const override { return false; }
    /// The self-test builds a one-lane bus.
    bool loopbackFullWidth() const override { return false; }
    /// The peripheral block this backend claims, for the sibling guard.
    LedHwBlock hwBlock() const override { return LedHwBlock::GpioDma; }
    /// What the status says when the bus will not come up.
    const char* initFailMsg() const override { return "GPIO DMA init failed: lanes need pins of one port (Teensy 4.1: 0, 1, 14-27, 38-41), or memory ran out"; }
    /// One buffer: the widened copy already lets encode and output overlap.
    bool supportsDoubleBuffer() const override { return false; }
    /// The largest frame the bus carries: the descriptor chain's slots, and the heap for the frame with its port words.
    size_t dmaBudgetBytes() const override {
        const size_t slotBytes = owner_ ? owner_->slotBytes() : 1;
        constexpr size_t kReserve = 16 * 1024;   // headroom for what allocates after this query
        const size_t block = platform::maxAllocBlock();   // 0 is a heap without a reported bound
        const size_t chain = platform::gpioDmaMaxSlots * slotBytes;
        if (block == 0) return chain;
        // Each slot costs its encoded bytes plus a 4-byte port word, both from the one heap.
        const size_t memory = block > kReserve ? (block - kReserve) * slotBytes / (slotBytes + 4) : 1;
        return chain < memory ? chain : memory;
    }
    /// No '595 expander on this bus yet.
    bool supportsPinExpander() const override { return false; }

    /// Create the bus and its frame buffer.
    bool busInit(size_t frameBytes, bool /*wantSecondBuffer*/) override {
        return platform::gpioDmaWs2812Init(bus_, owner_->busPinList(), owner_->busPinCount(), frameBytes);
    }
    /// The frame buffer the orchestrator encodes into; buffer 1 does not exist.
    uint8_t* busBuffer(uint8_t i) override { return i == 0 ? platform::gpioDmaWs2812Buffer(bus_) : nullptr; }
    /// The frame buffer's byte capacity.
    size_t busCapacity() const override { return platform::gpioDmaWs2812BufferCapacity(bus_); }
    /// The frame buffer and the wider copy the transfer reads, both of which the memory readout shows.
    size_t busHeapBytes() override { return platform::gpioDmaWs2812HeapBytes(bus_); }
    /// Start the transfer of the first `bytes` of the frame buffer.
    bool busTransmit(uint8_t i, size_t bytes) override { return i == 0 && platform::gpioDmaWs2812Transmit(bus_, bytes, waitMs_); }
    // The DMA reads the widened copy, so the frame buffer is free the moment the transfer starts.
    /// Report the frame buffer free and keep the driver's budget: the wait for the wire happens in the next transmit, after the encode it overlapped.
    bool busWait(uint8_t /*i*/, uint32_t ms) override { waitMs_ = ms; return true; }
    /// The most recent transfer's wire time (us): the WS2812 output floor.
    uint32_t busLastTransmitUs() const override { return platform::gpioDmaWs2812LastTransmitUs(bus_); }
    /// Release the bus and its buffers.
    void busDeinit() override { platform::gpioDmaWs2812Deinit(bus_); }

    /// Run the loopback self-test on a private one-lane bus.
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) override {
        return platform::gpioDmaWs2812Loopback(owner_->laneList(), owner_->laneCount(),
                                               static_cast<uint16_t>(owner_->loopbackRxPin),
                                               frame, frameBytes, dataBytes, rowBits);
    }

private:
    platform::GpioDmaWs2812Handle bus_;
    uint32_t waitMs_ = 100;   // the driver's wait budget, handed over at each busWait
};

/// Register the GPIO-DMA backend; ParallelLedDriver drives it via `peripheral`.
inline const bool kGpioDmaPeripheralRegistered =
    ParallelLedDriver::registerPeripheral("GPIO-DMA", []() -> LedPeripheral* { return new GpioDmaPeripheral(); });

} // namespace mm
