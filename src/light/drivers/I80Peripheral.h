#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared driver body + LedPeripheral
#include "platform/platform.h"

#include <cstdio>   // snprintf: the refusal message


namespace mm {

/// Output driver: parallel 8-or-16-lane WS2812B over the ESP-IDF esp_lcd i80 bus, the scale path on all three i80-capable ESP32 families. RMT gives a chip 4-8 channels; this gives 8-16 lanes for the wall time of one. The shared body lives in ParallelLedDriver and the wire format in ParallelSlots.h.
///
/// @attribution Adafruit, hpwit, FastLED
///
/// @technical
///
/// **One API, two peripherals.** ESP-IDF exposes one public i80 API and routes it to whatever the
/// silicon has: LCD_CAM on the S3, P4 and S31, and the I2S peripheral on the classic, its only route past 8 lanes. They are mutually exclusive per chip, so `lanesAvailable()` reads whichever lane count is non-zero. Named for the bus, since two peripherals serve it.
///
/// **The 3-slot-per-bit wire contract.** Each WS2812 bit becomes three bus slots at
/// 2.67 MHz, a 375 ns slot: every active lane HIGH, the data bits, then all LOW. A 1 is HIGH 750 ns and a 0 375 ns, approximating RMT's 700/350 rather than the lineage's ~416 ns. That stays under the 380 ns T0H max newer WS2812B revisions spec.
///
/// Both silicon paths do whole-frame chained DMA. So the classic I2S path is WiFi-underrun-immune by construction, and needs none of the ISR-refilled ring the lineage requires.
class I80Peripheral : public LedPeripheral {
public:
    // Unset until the user sets them; the S3 N16R8 bench is pins "1,2,4,5,6,7,8,9", rx 12.

    /// The i80 bus's write strobe, the pixel clock the peripheral mandates; in shift mode it is also the '595's shift clock, so it is wired to the expander. See the driver page.
    int8_t clockPin = platform::i2sLanes > 0 ? -1 : 10;
    /// The i80 bus's data/command select; unused by WS2812 but mandated by the peripheral.
    int8_t dcPin    = platform::i2sLanes > 0 ? 33 : 11;

    // --- LedPeripheral descriptors ---

    /// The number of i80 lanes this chip provides, 0 meaning no i80 bus here; the two backends are mutually exclusive per chip, so the sum picks whichever exists.
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return platform::lcdLanes + platform::i2sLanes; }
    /// Whether the bus width must round to a power of two, which on i80 it must; the bus rounds to 8 or 16 while the pin count itself is free.
    bool powerOfTwoBus() const override { return true; }

    /// The largest frame whole-frame DMA can carry, 0 meaning no bound; the classic's I2S DMA is internal-RAM only, and a failing alloc busy-waits to a reset.
    size_t dmaBudgetBytes() const override {
        if constexpr (platform::i2sLanes > 0) {
            const size_t block = platform::maxInternalAllocBlock();
            constexpr size_t kReserve = 16 * 1024;   // descriptors + headroom for allocs after this query
            constexpr size_t kMinBudget = 1;         // never 0 on the bounded path (0 == "no bound")
            return block > kReserve ? block - kReserve : kMinBudget;
        } else {
            return 0;   // LCD_CAM (S3/P4/S31): PSRAM DMA, no whole-frame ceiling
        }
    }

    /// Whether the loopback needs the full operational bus width, which on i80 it does, as there is no 1-lane private bus.
    bool loopbackFullWidth() const override { return true; }
    /// The peripheral block this backend claims, for the sibling guard; LcdCam is shared with MoonI80, which is why those two conflict.
    LedHwBlock hwBlock() const override {
        if constexpr (platform::i2sLanes > 0) return LedHwBlock::I2s;
        else return LedHwBlock::LcdCam;
    }
    /// Whether the shared peripheral instance this bus needs is free right now; this bus drives I2S instance 1, leaving instance 0, the only PDM one, for audio.
    bool busContentionCleared() const override {
        if constexpr (platform::i2sLanes > 0) return platform::i80Ws2812SharedBusFree();
        else return false;
    }

    /// What the status says when the bus will not come up.
    const char* initFailMsg() const override {
        // The backend's own reason first, else a line naming the peripheral the label names.
        if (const char* why = platform::i80Ws2812LastError()) return why;
        return (platform::i2sLanes > 0) ? "I2S-IDF: bus init failed, check pins / memory"
                                       : "LCD-IDF: bus init failed, check pins / memory";
    }

    /// The GPIO spare lanes park on, overriding the interface default: the "ghost pin" WR, which is already driven and already wired.
    uint16_t clockPinForBus() const override {
        return clockPin < 0 ? platform::kBusPinUnset : static_cast<uint16_t>(clockPin);
    }

    /// Bind the sacrificial WR (clockPin) and DC pins the i80 peripheral mandates.
    void addBusControls(ControlList& controls) override {
        controls.addPin("clockPin", clockPin);
        controls.addPin("dcPin", dcPin);   // never unset: esp_lcd toggles DC in software every transfer, and that aborts on a bare pad
    }
    /// Whether changing `name` needs the bus rebuilt: clockPin and dcPin do.
    bool busControlTriggersBuild(const char* name) const override {
        return std::strcmp(name, "clockPin") == 0 || std::strcmp(name, "dcPin") == 0;
    }

    /// Why the bus pins are fatally wrong, or null when they are usable. It is the error path, not validateBusPins' warnings, as a broken bus is not a warn-and-run.
    const char* validateBusFatal() const override {
        // An unguarded -1 becomes 65535 in busInit's cast and slips IDF's own `>= 0` test.
        if (platform::i2sLanes == 0 && clockPin < 0)
            return "clockPin (WR) is unset - the i80 bus needs a write-strobe GPIO on this chip";
        if (dcPin < 0) return "dcPin (DC) is unset - the i80 bus toggles it every frame, so it needs a real GPIO";
        if (clockPin >= 0 && clockPin == dcPin)
            return "clockPin (WR) and dcPin (DC) are the same GPIO - they must differ";
        // The lane sweep misses WR and DC, so the pair is refused here, in the platform's words.
        if (clockPin >= 0) {
            if (const char* why = platform::gpioRefusal(static_cast<uint8_t>(clockPin))) {
                std::snprintf(refusal_, sizeof(refusal_), "clockPin (WR) %s - pick another pin", why);
                return refusal_;
            }
        }
        if (dcPin >= 0) {
            if (const char* why = platform::gpioRefusal(static_cast<uint8_t>(dcPin))) {
                std::snprintf(refusal_, sizeof(refusal_), "dcPin (DC) %s - pick another pin", why);
                return refusal_;
            }
        }
        // The latch is a BUS LANE and needs its own GPIO: sharing WR latches every shift cycle.
        if (owner_->pinExpanderMode() && owner_->latchPin >= 0) {
            if (owner_->latchPin == clockPin)
                return "latchPin is on clockPin (WR) - the latch needs its own GPIO";
            if (owner_->latchPin == dcPin)
                return "latchPin is on dcPin (DC) - the latch needs its own GPIO";
        }
        return nullptr;
    }

    /// Which data lane collides with WR or DC, as a warning, or null when the set is clear. Parking WR or DC on an unused lane is valid, and only a driven lane shows garbage.
    const char* validateBusPins(const uint16_t* lanes, uint8_t n) const override {
        for (uint8_t i = 0; i < n; i++) {
            // clockPin/dcPin are int8_t (-1 = unset); only a real GPIO can collide.
            if (clockPin >= 0 && lanes[i] == static_cast<uint16_t>(clockPin))
                return "a LED pin is on clockPin (WR): that lane carries the clock, not pixels";
            if (dcPin >= 0 && lanes[i] == static_cast<uint16_t>(dcPin))
                return "a LED pin is on dcPin: that lane carries DC, not pixels";
        }
        return nullptr;
    }

    /// Whether a 74HCT595 pin expander can ride this bus; only LCD_CAM reaches PSRAM, and the x8 expander frame is 154 KB, which the classic cannot hold.
    bool supportsPinExpander() const override { return platform::hasLcdCam; }

    /// Create the i80 bus and its DMA buffers for `frameBytes` on the current lanes. In shift mode the orchestrator appends the latch to the pin list, as it is a bus lane.
    bool busInit(size_t frameBytes, bool wantSecondBuffer) override {
        return platform::i80Ws2812Init(i80_, owner_->busPinList(), owner_->busPinCount(),
                                       clockPinForBus(),
                                       dcPin < 0 ? platform::kBusPinUnset : static_cast<uint16_t>(dcPin),
                                       frameBytes, wantSecondBuffer, owner_->busClockMultiplier());
    }
    /// DMA buffer `i` (0 or 1) the orchestrator encodes into; buffer 1 is null in single-buffer mode, and both are busCapacity bytes.
    uint8_t* busBuffer(uint8_t i) override        { return platform::i80Ws2812Buffer(i80_, i); }
    /// The per-buffer byte capacity (fixed at bus creation; both buffers equal).
    size_t   busCapacity() const override         { return platform::i80Ws2812BufferCapacity(i80_); }
    /// Start the autonomous transfer of the first `bytes` of DMA buffer `i`, returning whether it began.
    bool  busTransmit(uint8_t i, size_t bytes) override { return platform::i80Ws2812Transmit(i80_, i, bytes); }
    /// Block up to `ms` for buffer `i`'s in-flight transfer to complete.
    bool  busWait(uint8_t i, uint32_t ms) override      { return platform::i80Ws2812Wait(i80_, i, ms); }
    /// The most recent DMA transfer's wire time (µs): the WS2812 output floor.
    uint32_t busLastTransmitUs() const override         { return platform::i80Ws2812LastTransmitUs(i80_); }
    /// Tear down the i80 bus and its DMA buffer.
    void     busDeinit() override                 { platform::i80Ws2812Deinit(i80_); }

    /// Run the loopback self-test on a full-width private bus; all 8 data GPIOs must be valid, so it rebuilds the full bus and uses lane 0.
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) override {
        // Built from the same pin list and pclk, so the test transmits what the render loop does.
        return platform::i80Ws2812Loopback(owner_->busPinList(), owner_->busPinCount(),
                                           static_cast<uint16_t>(clockPin),
                                           static_cast<uint16_t>(dcPin),
                                           static_cast<uint16_t>(owner_->loopbackRxPin),
                                           frame, frameBytes, dataBytes, rowBits,
                                           owner_->busClockMultiplier());
    }

    /// Store WR and DC alongside the data pins, so editing either rebuilds the bus.
    void recordBusPins() override { lastClockPin_ = clockPin; lastDcPin_ = dcPin; }
    /// Whether the live bus's WR and DC pins still match the controls, so a rebuild can be skipped.
    bool extraBusPinsCurrent() const override {
        return lastClockPin_ == clockPin && lastDcPin_ == dcPin;
    }

private:
    platform::I80Ws2812Handle i80_;
    int8_t lastClockPin_ = -1;
    int8_t lastDcPin_ = -1;
    mutable char refusal_[96] = {};   // one refusal message, formatted by the const validators
};

/// The peripheral picker label: it names the PERIPHERAL a user can look up, and `-IDF` says esp_lcd drives it.
inline constexpr const char* kI80Label = (platform::i2sLanes > 0) ? "I2S-IDF" : "LCD-IDF";
inline const bool kI80PeripheralRegistered =
    ParallelLedDriver::registerPeripheral(kI80Label, []() -> LedPeripheral* { return new I80Peripheral(); });

} // namespace mm
