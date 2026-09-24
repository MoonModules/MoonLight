/// @module I80Peripheral
/// @also ParallelLedDriver, Drivers, Correction

#include "doctest.h"
#include "host_bus.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/drivers/I80Peripheral.h"
#include "light/layers/Buffer.h"
#include "unit/core/conditional_controls.h"  // shared conditional-control helpers

#include <cstring>

// Host-side half of the LCD driver: lane slicing (the shared PinList semantics), the frame-byte arithmetic (latch pad, 64-byte alignment, RGBW growth), and the parse-error/recovery shape. The hardware half (bus init, DMA transmit) is inert on the host, desktop stubs return false/nullptr, and is proven on the S3.
//
// mm::ParallelLedDriver is the ONE registered driver; this file drives it with an injected mm::I80Peripheral backend (the esp_lcd i80 bus), the backend this header defines and registers under the "i80" peripheral label.

namespace {

// The peripheral is declared BEFORE the driver in every case below (via this helper's parameter order + the caller's declaration order) so it outlives the driver, reverse destruction order tears the driver down first, matching setPeripheralForTest's borrow contract (see unit_ParallelLedDriver_doublebuffer.cpp's wire()).
void wire(mm::ParallelLedDriver& d, mm::I80Peripheral& peripheral, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights) {
    d.setPeripheralForTest(&peripheral);
    // Pins default to UNSET now (the "default only when it cannot do harm" rule, a user solders the strand to its own GPIOs), so a fresh driver idles until configured. These slicing/frame tests exercise the lane logic, not the default value, so the helper supplies the bench 8-pin set unless a case set its own pins first (the bad-pin / partial-bus cases do).
    if (d.pins[0] == '\0') std::strcpy(d.pins, "1,2,4,5,6,7,8,9");
    // allocate succeeds exactly when lights > 0 (the zero-grid case wires an empty buffer on purpose); a masked alloc failure would fail cases downstream.
    REQUIRE(src.allocate(lights, 3) == (lights > 0));
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);   // 3 out-channels
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

// frameBytes = maxLaneLights × outCh × 24 + 800 latch pad + 64 clock-tolerance slack, rounded up to 64 (mirrors ParallelLedDriver::frameBytesFor).
// `slotBytes` is the BUS width in bytes: 1 for the 8-bit bus (≤8 pins), 2 for the 16-bit bus (9..16).
// It scales both the slot stream and the latch pad, exactly as ParallelLedDriver::frameBytesFor does.
size_t expectFrame(mm::nrOfLightsType maxLights, uint8_t outCh, uint8_t slotBytes = 1) {
    if (maxLights == 0) return 0;
    const size_t raw = static_cast<size_t>(maxLights) * outCh * 24 * slotBytes + (800 + 64) * slotBytes;
    return (raw + 63) & ~static_cast<size_t>(63);
}

} // namespace

// Explicit counts slice the buffer consecutively; the frame is sized by the LONGEST lane. The bus always has all 8 lanes, unused strands take the 0-light remainder and idle LOW.
TEST_CASE("I80Peripheral slices lanes and sizes the frame by the longest") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.ledsPerPin, "50,20,20");   // lanes 3..7 share the remainder: 0
    wire(d, peripheral, src, corr, 90);

    REQUIRE(d.laneCount() == 8);
    CHECK(d.laneLightCount(0) == 50);
    CHECK(d.laneLightCount(1) == 20);
    CHECK(d.laneLightCount(2) == 20);
    CHECK(d.laneLightCount(3) == 0);
    CHECK(d.laneLightCount(7) == 0);
    CHECK(d.laneStart(0) == 0);
    CHECK(d.laneStart(1) == 50);
    CHECK(d.laneStart(2) == 70);
    CHECK(d.maxLaneLights() == 50);
    CHECK(d.frameBytes() == expectFrame(50, 3));
}

// Empty ledsPerPin splits evenly, same PinList semantics the RMT driver uses.
TEST_CASE("I80Peripheral even split over the default 8 lanes") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 256);   // default pins: 8 lanes

    REQUIRE(d.laneCount() == 8);
    CHECK(d.laneLightCount(0) == 32);
    CHECK(d.laneLightCount(7) == 32);
    CHECK(d.maxLaneLights() == 32);
    CHECK(d.frameBytes() == expectFrame(32, 3));
}

// An RGB→RGBW preset toggle grows the frame (32 vs 24 slot bytes per light).
TEST_CASE("I80Peripheral frame grows on RGBW preset") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.ledsPerPin, "50,50");   // lanes 2..7 idle
    wire(d, peripheral, src, corr, 100);
    CHECK(d.frameBytes() == expectFrame(50, 3));

    // The driver owns its Correction, so mutate that copy (not the external one).
    mm::test::rebuildFromPreset(d.correctionForTest(), 255, mm::test::PresetOrder::GRBW);
    d.onCorrectionChanged();
    CHECK(d.frameBytes() == expectFrame(50, 4));
}

// A whole-frame driver with a bounded DMA (the classic ESP32 i80 = internal-RAM-only I2S, no ring) must REFUSE cleanly when the frame won't fit, never choke the bus init (which can busy-wait to a watchdog reset on hardware). On desktop / PSRAM chips the budget is 0 (no bound), so the frame-fit gate NEVER triggers regardless of grid size: a large frame is never rejected for its size here (the classic-i80 branch is compiled out). Pins the "budget 0 = no bound" contract, the gate is inert off the classic chip, so this refactor changes nothing on every non-classic target. The hardware behavior (a too-big frame on the real classic i80 idles with the clear "over DMA" status) is proven on the Olimex.
TEST_CASE("I80Peripheral: the DMA-fit gate is inert off the classic i80 (budget 0)") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "1");            // one lane → the whole grid lands on it, a big frame
    std::strcpy(d.ledsPerPin, "");       // even-split all onto the one pin
    wire(d, peripheral, src, corr, 4096);            // a large grid, far past any real classic i80 DMA budget

    // The frame was computed (a real size), and the status is NOT the size-rejection message, desktop dmaBudgetBytes() is 0, so frameFitsDmaBudget() always passes. (The desktop bus stub is inert, so the status may be the plain init-fail, but never the size gate, that only fires on the classic.)
    CHECK(d.frameBytes() > 0);
    CHECK(std::strstr(d.status() ? d.status() : "", "over i80 DMA") == nullptr);
}

// frameFitsDmaBudget() is the pure predicate the classic-i80 gate leans on; its logic is compiled out on desktop (budget always 0), so exercise it directly with synthetic budgets. A FINITE budget rejects an oversized frame and accepts one that fits; a ZERO budget ("no bound", the LCD_CAM/PSRAM/desktop case) never rejects, whatever the frame size.
TEST_CASE("I80Peripheral::frameFitsDmaBudget rejects only over a finite budget") {
    // frameFitsDmaBudget is a protected static on ParallelLedDriver; a tiny subclass exposes it for the test without widening the production class surface.
    struct Expose : mm::ParallelLedDriver {
        using mm::ParallelLedDriver::frameFitsDmaBudget;
    };
    // finite budget: reject strictly-larger, accept equal-or-smaller
    CHECK_FALSE(Expose::frameFitsDmaBudget(1025, 1024));
    CHECK(Expose::frameFitsDmaBudget(1024, 1024));
    CHECK(Expose::frameFitsDmaBudget(512, 1024));
    // zero budget = no bound: never rejects, however large the frame
    CHECK(Expose::frameFitsDmaBudget(1u << 30, 0));
}

// A bad pin list idles the driver with the parse literal in the status; fixing it recovers.
TEST_CASE("I80Peripheral bad pins → status error → recovery") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "1,nope");
    wire(d, peripheral, src, corr, 64);

    CHECK(d.laneCount() == 0);
    CHECK(d.frameBytes() == 0);
    CHECK(d.status() != nullptr);

    std::strcpy(d.pins, "1,2,4,5,6,7,8,9");
    d.applyState();
    CHECK(d.laneCount() == 8);
    // Recovered: parse error cleared, replaced by the neutral consumption info.
    CHECK(d.severity() != mm::MoonModule::Severity::Error);
    CHECK(std::strstr(d.status() ? d.status() : "", "driving") != nullptr);
}

// Pins now default UNSET (the "default only when it cannot do harm" rule, the strand is user-soldered). A fresh, unconfigured driver idles, never grabbing the 8 data GPIOs on its own. (wire() back-fills empty pins for the slicing cases, so this one wires the buffer directly to keep pins empty.)
TEST_CASE("I80Peripheral with the empty default pins idles cleanly") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.setPeripheralForTest(&peripheral);
    REQUIRE(d.pins[0] == '\0');           // the empty default, not a bench guess
    REQUIRE(src.allocate(64, 3));
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();

    CHECK(d.laneCount() == 0);            // no lanes claimed
    CHECK(d.frameBytes() == 0);
    CHECK(d.status() != nullptr);         // "set pins" surfaced, not silent
    d.tick();                             // must be a no-op, not a crash
}

// **The BUS width is 8 or 16; the PIN COUNT is whatever the board drives.** They are different
// numbers and the driver reconciles them: the peripheral always clocks a full 8- or 16-bit word, but nothing requires every bit to reach a GPIO, busPinList() parks the lanes the board doesn't use on WR, where the peripheral already drives and no strand reads them. So a 5-pin board is 5 data lanes on an 8-bit bus, not a config error, and needs no fake "ghost" pins to pad the list out.
TEST_CASE("I80Peripheral drives any pin count; the bus rounds up around it") {
    mm::Buffer src;
    mm::Correction corr;
    {   // 3 pins → 3 lanes on the 8-bit bus, the spare 5 parked on WR.
        mm::I80Peripheral peripheral;
        mm::ParallelLedDriver d;
        std::strcpy(d.pins, "1,2,4");
        wire(d, peripheral, src, corr, 64);
        CHECK(d.laneCount() == 3);
        CHECK(d.severity() != mm::MoonModule::Severity::Error);
        // 64 lights over 3 lanes → the longest is 22. ≤8 pins → the 8-bit bus → 1-byte slots, which is what expectFrame() assumes, so a plain expectFrame match IS the "bus stayed 8-bit" assertion.
        CHECK(d.maxLaneLights() == 22);
        CHECK(d.frameBytes() == expectFrame(22, 3));
    }
    {   // 16 pins → the full 16-bit bus, nothing parked. clock/dc moved clear of the data set
        // (defaults 10/11 would collide with data pins 10/11 → the collision guard). clockPin/dcPin live on the I80Peripheral backend, so set them via the control API ParallelLedDriver binds (defineDriverControls -> peripheral_->addBusControls) rather than a direct member.
        mm::I80Peripheral peripheral;
        mm::ParallelLedDriver d;
        d.setPeripheralForTest(&peripheral);
        d.defineControls();
        mm::test::setControlValue<int8_t>(d, "clockPin", 20);
        mm::test::setControlValue<int8_t>(d, "dcPin", 21);
        std::strcpy(d.pins, "1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17");
        wire(d, peripheral, src, corr, 64);
        CHECK(d.laneCount() == 16);
        CHECK(d.severity() != mm::MoonModule::Severity::Error);
        CHECK(d.maxLaneLights() == 4);
        CHECK(d.frameBytes() == expectFrame(4, 3, /*slotBytes=*/2));   // 16 pins → the 16-bit bus
    }
    {   // 10 pins — the case the old "exactly 8 or 16" rule rejected outright. It is a perfectly good
        // config: 10 data lanes on the 16-bit bus, the other 6 parked. This is the point of the change.
        mm::I80Peripheral peripheral;
        mm::ParallelLedDriver d;
        d.setPeripheralForTest(&peripheral);
        d.defineControls();
        mm::test::setControlValue<int8_t>(d, "clockPin", 20);
        mm::test::setControlValue<int8_t>(d, "dcPin", 21);
        std::strcpy(d.pins, "1,2,4,5,6,7,8,9,12,13");
        wire(d, peripheral, src, corr, 64);
        CHECK(d.laneCount() == 10);
        CHECK(d.severity() != mm::MoonModule::Severity::Error);
        // 64 over 10 lanes → 6 each, the last taking the remainder (PinList's even-split rule) → 10.
        // >8 pins → the 16-bit bus → 2-byte slots.
        CHECK(d.maxLaneLights() == 10);
        CHECK(d.frameBytes() == expectFrame(10, 3, /*slotBytes=*/2));
    }
    // (0 pins → idles: covered by "I80Peripheral with the empty default pins idles cleanly" above.)
}

// WR and DC are lines the LEDs never read, so what an UNSET one costs is the chip's business: the classic ESP32 sinks it onto an input-only pad (no GPIO spent), the LCD_CAM chips need a real pad because an invalid number reaches the ROM's matrix routine. The desktop emulates the LCD_CAM backend, so here an unset WR idles the driver with a status that names the chip's rule.
TEST_CASE("I80Peripheral idles with a named status when DC is unset, on every chip") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.setPeripheralForTest(&peripheral);
    REQUIRE(src.allocate(64, 3));
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    d.defineControls();
    mm::test::setControlValue<int8_t>(d, "clockPin", 20);
    mm::test::setControlValue<int8_t>(d, "dcPin", -1);
    std::strcpy(d.pins, "1,2,4");
    wire(d, peripheral, src, corr, 64);
    CHECK(d.severity() == mm::MoonModule::Severity::Error);
    // DC is toggled in software every frame (esp_lcd calls gpio_set_level on it), which a pad with no output driver cannot do: the call fails, logs, and the log aborts. So unlike WR it can never be sunk, and the driver says why.
    CHECK(std::strstr(d.status() ? d.status() : "", "needs a real GPIO") != nullptr);
}

TEST_CASE("I80Peripheral on an LCD_CAM chip idles with a named status when WR is unset") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.setPeripheralForTest(&peripheral);
    REQUIRE(src.allocate(64, 3));
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    d.defineControls();
    mm::test::setControlValue<int8_t>(d, "clockPin", -1);
    mm::test::setControlValue<int8_t>(d, "dcPin", 21);
    std::strcpy(d.pins, "1,2,4");
    wire(d, peripheral, src, corr, 64);
    CHECK(d.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::strstr(d.status() ? d.status() : "", "needs a write-strobe GPIO") != nullptr);
}

// A pin the PACKAGE lacks fails the same silent way a flash pin does: the ESP32-PICO-V3-02 has no GPIO 18/23, and routing the i80 clock there wedged its flash cache with no panic. The platform knows the package; the driver must refuse by name before the peripheral touches the pad.
TEST_CASE("I80Peripheral refuses a WR/DC pin this chip package does not have") {
    mm::platform::GpioCapability absent;
    absent.validGpio = false;
    mm::platform::setTestGpioCapability(20, absent);
    {
        mm::I80Peripheral peripheral;
        mm::ParallelLedDriver d;
        mm::Buffer src;
        mm::Correction corr;
        d.setPeripheralForTest(&peripheral);
        REQUIRE(src.allocate(64, 3));
        mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
        d.defineControls();
        mm::test::setControlValue<int8_t>(d, "clockPin", 20);
        mm::test::setControlValue<int8_t>(d, "dcPin", 21);
        std::strcpy(d.pins, "1,2,4");
        wire(d, peripheral, src, corr, 64);
        CHECK(d.severity() == mm::MoonModule::Severity::Error);
        CHECK(std::strstr(d.status() ? d.status() : "", "does not exist on this chip package") != nullptr);
    }
    mm::platform::clearTestGpioCapability();
}

// A data lane on the same GPIO as the WR (clockPin) or DC pin is a WARNING, not a blocker: that lane carries the clock/DC waveform instead of pixel data, but on a board that wires all 8/16 lanes yet drives fewer strands, parking WR/DC on an unused data pin is a valid choice, so the driver still runs and flags a warning. clockPin/dcPin default to 10/11.
TEST_CASE("I80Peripheral warns (does not idle) when a data pin is on clockPin/dcPin") {
    mm::Buffer src;
    mm::Correction corr;
    {   // lane on GPIO 10 == default clockPin → warns but still drives all 8 lanes
        mm::I80Peripheral peripheral;
        mm::ParallelLedDriver d;
        std::strcpy(d.pins, "18,5,6,7,8,9,10,11");   // 10 == clockPin, 11 == dcPin
        wire(d, peripheral, src, corr, 64);
        CHECK(d.laneCount() == 8);                    // still built + driving
        REQUIRE(d.status() != nullptr);               // a warning is present
        CHECK(std::strstr(d.status(), "clockPin") != nullptr);
    }
    {   // move clock/dc clear of the data set → no warning
        mm::I80Peripheral peripheral;
        mm::ParallelLedDriver d;
        d.setPeripheralForTest(&peripheral);
        d.defineControls();
        mm::test::setControlValue<int8_t>(d, "clockPin", 12);
        mm::test::setControlValue<int8_t>(d, "dcPin", 13);
        std::strcpy(d.pins, "18,5,6,7,8,9,10,11");
        wire(d, peripheral, src, corr, 64);
        CHECK(d.laneCount() == 8);
        // No collision warning → the neutral consumption info instead (not a null status).
        CHECK(d.severity() != mm::MoonModule::Severity::Error);
        CHECK(std::strstr(d.status() ? d.status() : "", "driving") != nullptr);
    }
    {   // WR and DC on the SAME GPIO is a FATAL config error that IDLES the driver — the i80 bus
        // needs two distinct control lines, so this breaks the bus outright (unlike a data-lane collision, which only corrupts that one lane and is a warn-and-run). Routed through the error path (validateBusFatal), so the driver idles: laneCount 0, error severity.
        mm::I80Peripheral peripheral;
        mm::ParallelLedDriver d;
        d.setPeripheralForTest(&peripheral);
        d.defineControls();
        mm::test::setControlValue<int8_t>(d, "clockPin", 20);
        mm::test::setControlValue<int8_t>(d, "dcPin", 20);   // same as clockPin
        std::strcpy(d.pins, "1,2,3,4,5,6,7,8");
        wire(d, peripheral, src, corr, 64);
        REQUIRE(d.status() != nullptr);
        CHECK(std::strstr(d.status(), "same GPIO") != nullptr);
        CHECK(d.severity() == mm::MoonModule::Severity::Error);   // idles, not warn-and-run
        CHECK(d.laneCount() == 0);                                // driver did NOT build the bus
    }
    {   // An UNSET clockPin (-1) is FATAL on i80: the int8_t -1 casts to uint16_t 65535 and slips past
        // IDF's own wr/dc >= 0 guard, so validateBusFatal must reject it here or the bus inits on a garbage GPIO. Idles with a clear status, doesn't build.
        mm::I80Peripheral peripheral;
        mm::ParallelLedDriver d;
        d.setPeripheralForTest(&peripheral);
        d.defineControls();
        mm::test::setControlValue<int8_t>(d, "clockPin", -1);   // unset
        mm::test::setControlValue<int8_t>(d, "dcPin", 21);
        std::strcpy(d.pins, "1,2,3,4,5,6,7,8");
        wire(d, peripheral, src, corr, 64);
        REQUIRE(d.status() != nullptr);
        CHECK(std::strstr(d.status(), "clockPin") != nullptr);
        CHECK(d.severity() == mm::MoonModule::Severity::Error);
        CHECK(d.laneCount() == 0);
    }
    {   // Same for an unset dcPin (-1).
        mm::I80Peripheral peripheral;
        mm::ParallelLedDriver d;
        d.setPeripheralForTest(&peripheral);
        d.defineControls();
        mm::test::setControlValue<int8_t>(d, "clockPin", 20);
        mm::test::setControlValue<int8_t>(d, "dcPin", -1);      // unset
        std::strcpy(d.pins, "1,2,3,4,5,6,7,8");
        wire(d, peripheral, src, corr, 64);
        REQUIRE(d.status() != nullptr);
        CHECK(std::strstr(d.status(), "dcPin") != nullptr);
        CHECK(d.severity() == mm::MoonModule::Severity::Error);
        CHECK(d.laneCount() == 0);
    }
}

// A 0×0×0 grid is a clean idle: zero counts, zero frame (no pad for an empty frame), no crash.
TEST_CASE("I80Peripheral tolerates a zero-light buffer") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 0);

    CHECK(d.laneCount() == 8);       // pins parse fine
    CHECK(d.maxLaneLights() == 0);
    CHECK(d.frameBytes() == 0);
    d.tick();                        // must be a no-op, not a crash
    CHECK(true);
}

// setup/release cycles leave no residue (status clean, ASAN-checked heap).
TEST_CASE("I80Peripheral setup/release is repeatable") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.setPeripheralForTest(&peripheral);
    src.allocate(64, 3);
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    std::strcpy(d.pins, "1,2,4,5,6,7,8,9");   // pins now default UNSET
    d.defineControls();
    for (int cycle = 0; cycle < 4; cycle++) {
        d.setup();
        d.setSourceBuffer(&src);
        d.correctionForTest() = corr;
        d.applyState();
        REQUIRE(d.laneCount() == 8);
        d.release();
        CHECK(d.status() == nullptr);
    }
}

// loopbackRxPin is bound always, visible only while loopbackTest is on.
TEST_CASE("I80Peripheral loopbackRxPin tracks the loopbackTest toggle") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    d.setPeripheralForTest(&peripheral);
    d.defineControls();
    bool found = false;
    for (uint8_t i = 0; i < d.controls().count(); i++) {
        if (std::strcmp(d.controls()[i].name, "loopbackRxPin") == 0) {
            found = true;
            CHECK(d.controls()[i].hidden == true);   // test mode off by default
        }
    }
    CHECK(found);
}

// loopbackTxPin (optional lane-0 TX override) is bound always, hidden until the test is on, same conditional-control contract as loopbackRxPin. The override's lane-0 substitution is hardware-only (lcdLanes==0 on desktop); the visibility contract is host-testable here via the shared helper (toggles loopbackTest both ways and asserts the control stays bound while flipping visibility).
TEST_CASE("I80Peripheral loopbackTxPin tracks the loopbackTest toggle") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    d.setPeripheralForTest(&peripheral);
    d.defineControls();
    auto setTest = [&](bool on) {
        mm::test::setControlValue<bool>(d, "loopbackTest", on);
    };
    mm::test::checkConditionalControl(d, "loopbackTxPin", setTest, /*visibleWhenTrue=*/true);
}

// The pinExpander switch is HIDDEN where the silicon can't host the '595 (supportsPinExpander() false: the classic ESP32 i80 = the I2S peripheral, whose DMA can't read PSRAM, so the ×8 expander frame has nowhere to live). Desktop has lcdLanes==0 too, so the control is hidden here, the same compile-time gate the classic build takes. On the LCD_CAM chips (S3/P4) the flag is true and the control shows; that path is exercised on-device. Turning it on where unsupported only ever yields a config error, so not offering the switch is the honest UI. (Before this, the toggle was shown on every chip.)
//
// kSupportsPinExpander moved from a static constexpr on the driver to a virtual on the I80Peripheral backend (supportsPinExpander()), a bare I80Peripheral instance reaches it without needing a live driver's peripheral_ (which is protected).
TEST_CASE("I80Peripheral hides pinExpander where the chip can't host it") {
    mm::I80Peripheral peripheral;
    // Tied to the capability flag, not to a hard-coded value: the host now EMULATES LCD_CAM (so the expander path is reachable off-device), and a classic ESP32 still reports false. The control's visibility must track whatever the target says, which is what this pins.
    CHECK(peripheral.supportsPinExpander() == mm::platform::hasLcdCam);

    mm::ParallelLedDriver d;
    d.setPeripheralForTest(&peripheral);
    d.defineControls();
    bool found = false;
    for (uint8_t i = 0; i < d.controls().count(); i++) {
        if (std::strcmp(d.controls()[i].name, "pinExpander") == 0) {
            found = true;
            // Hidden exactly when the chip can't host the expander, ties the assertion to the flag rather than to the desktop's happens-to-be-unsupported value, so it stays correct on any target.
            CHECK(d.controls()[i].hidden == !peripheral.supportsPinExpander());
        }
    }
    CHECK(found);   // still BOUND (a saved value survives), just not shown
}

// The host bus is REAL MEMORY, not a refusal: `busInit` used to return false on desktop, so every bus assertion was unreachable off-device and the driver's encode path only ever ran on hardware. The contract is identical for all three peripherals, so it lives in one place.
TEST_CASE("I80Peripheral allocates a real host bus the driver can encode into") {
    mm::test::checkHostBusAllocates<mm::I80Peripheral>();
}

TEST_CASE("I80Peripheral gives the host bus two distinct buffers when asked") {
    mm::test::checkHostBusDoubleBuffer<mm::I80Peripheral>();
}

// --- Current limiting ---------------------------------------------------------------------------
// measureFrame() has to walk exactly the lights the encode will touch, so the total must be the SUM of the lanes rather than the buffer size or the longest lane. Under-counting is the dangerous direction.
TEST_CASE("ParallelLedDriver prices every lane, not just the longest") {
    mm::I80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.ledsPerPin, "50,20,20");
    wire(d, peripheral, src, corr, 90);

    // White everywhere: 90 lights x 3 channels x 8 mA = 2160 mA against a 1080 budget.
    std::memset(src.data(), 255, static_cast<size_t>(90) * 3);
    d.correctionForTest().budgetMa = 1080;
    d.tick();

    // Halved. Sizing the pass by the longest lane would price it at 1200 mA and set limit to 230, the under-counting direction that reports a frame safe while the supply sags.
    CHECK(d.correctionForTest().limit == 128);
}

// A driver that never measures must not offer the controls: see DriverBase::limitsCurrent.
TEST_CASE("ParallelLedDriver offers the current controls") {
    mm::ParallelLedDriver d;
    CHECK(d.limitsCurrent());
}

TEST_CASE("I80Peripheral counts the DMA buffers in its memory readout") {
    mm::test::checkHostBusCountedInHeapReadout<mm::I80Peripheral>();
}
