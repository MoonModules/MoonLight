/// @module MoonI80Peripheral
/// @also I80Peripheral, ParallelLedDriver

#include "doctest.h"
#include "host_bus.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/drivers/MoonI80Peripheral.h"
#include "light/layers/Buffer.h"
#include "unit/core/conditional_controls.h"  // controlIndex/setControlValue — clockPin now lives on the backend

#include <cstring>

// MoonI80Peripheral is the SAME LCD_CAM output as I80Peripheral, on our own DMA code instead of esp_lcd. It is a thin ParallelLedDriver subclass whose constructor wires a MoonI80Peripheral backend (a runtime LedPeripheral strategy, not compile-time CRTP), so the orchestrator's whole body, lane slicing, frame sizing, the fused encode, the async double-buffer, the shift-register expander, the dead-frame guard, is ALREADY covered by the Mock-driver suites (unit_ParallelLedDriver_doublebuffer / _shiftregister) and by unit_I80LedDriver. Re-testing it here through a second concrete driver would assert the same base twice.
//
// So these cases pin only what is genuinely MoonI80's own:
//   - it satisfies the LedPeripheral contract (it instantiates and configures at all);
//   - the constants that DIFFER from its sibling, chiefly that it is LCD_CAM-only;
//   - its own bus-pin validation, and the fact that it needs FEWER pins than the sibling: no DC at
//     all, and WR only under the expander (owning the GPIO matrix is what buys that).
//
// The hardware half (our GDMA descriptor chain) is inert on the host, desktop stubs return false/nullptr and `lanesAvailable()` is 0, and is proven on the S3 bench.

namespace {

// On the host `platform::lcdLanes` is 0, so the real MoonI80Peripheral reports supportsPinExpander() == false, which the orchestrator now honors by SILENTLY dropping pinExpander back to direct mode (a peripheral that can't host the '595 has its expander toggle hidden, so an unfixable error would be wrong, see parseConfig). That degradation is correct on hardware but hides the SHIFT-MODE validation (WR collision, latch-on-WR, clockPin required) these cases exist to pin. So the shift-mode cases use this tiny subclass that reports the expander as supported, exactly as the real backend does on an LCD_CAM chip, the validation then runs on the host.
struct ExpanderMoonI80 : mm::MoonI80Peripheral {
    bool supportsPinExpander() const override { return true; }
};

void wire(mm::ParallelLedDriver& d, mm::MoonI80Peripheral& peripheral, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights) {
    mm::test::wireParallel(d, peripheral, src, corr, lights, "1,2,4,5,6,7,8,9");
}

} // namespace

// **The load-bearing difference from its sibling.** I80Peripheral runs on the i80 *bus*, LCD_CAM on
// the S3/P4 AND the I2S peripheral on the classic ESP32 (IDF's esp_lcd picks the backend). MoonI80 programs LCD_CAM directly, so it must NOT claim the classic chip: `lanesAvailable()` reads `lcdLanes` alone, without the `+ i2sLanes` its sibling adds. Getting this wrong would offer the driver on a chip whose peripheral it cannot drive.
//
// lanesAvailable()/kSupportsPinExpander/kPowerOfTwoBus/kLoopbackFullWidth moved from static constexpr on the driver to virtuals on the MoonI80Peripheral backend, a bare instance reaches them without a live driver's peripheral_ (protected on ParallelLedDriver).
TEST_CASE("MoonI80Peripheral is LCD_CAM-only — it does not claim the classic ESP32's I2S i80") {
    mm::MoonI80Peripheral peripheral;
    CHECK(peripheral.lanesAvailable() == mm::platform::lcdLanes);
    // The expander needs LCD_CAM silicon, `hasLcdCam`, not `lcdLanes > 0`. The two used to be the same test, until the host set a non-zero lane count so the driver would RUN there against a memory bus; a lane count is "how wide", the capability is "which peripheral".
    CHECK(peripheral.supportsPinExpander() == mm::platform::hasLcdCam);
}

// The i80 BUS is 8 or 16 bits wide whatever the pin count, so the base rounds it up (powerOfTwoBus()) and parks the lanes the board does not use. And the loopback cannot build a 1-lane private bus, so its test frame is encoded at the full operational width.
TEST_CASE("MoonI80Peripheral keeps the i80 bus rules: power-of-two bus, full-width loopback") {
    mm::MoonI80Peripheral peripheral;
    CHECK(peripheral.powerOfTwoBus());
    CHECK(peripheral.loopbackFullWidth());
}

// **The bus control pins are a '595 cost, not an i80 cost, and owning the DMA is what proves it.**
// DC exists so an LCD panel can separate command bytes from data bytes; a WS2812 strand has no such concept, and the peripheral holds DC at a constant level. WR is the pixel clock, which only a shift register consumes (as SRCLK), WS2812 is self-clocked, so a strand ignores it. esp_lcd mandates a valid GPIO for BOTH regardless (`wr_gpio_num >= 0 && dc_gpio_num >= 0`), which is why the sibling still spends two pins on them. This backend routes its own GPIO matrix, so it routes neither in direct mode: there is no dcPin at all, and clockPin reaches a pad only under the expander.
//
// The observable consequence, and what this case pins: in DIRECT mode `clockPin` may freely name a GPIO that a strand also uses, because the signal never leaves the peripheral. Rejecting that would forbid a working config to protect a signal nobody reads.
TEST_CASE("MoonI80Peripheral direct mode: clockPin is unrouted, so it cannot collide") {
    mm::MoonI80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "1,2,4,5,6,7,8,10");   // pin 10 IS the default clockPin (WR) — fine here
    wire(d, peripheral, src, corr, 8 * 16);
    CHECK(d.severity() != mm::ParallelLedDriver::Severity::Error);
    CHECK(d.laneCount() == 8);   // it drives all eight strands
}

// Under the expander WR IS routed (it clocks the '595s), so now it can collide, and a data lane sharing it is silent corruption: the matrix drives both signals onto the one pad and that strand emits the shift clock instead of pixel data.
TEST_CASE("MoonI80Peripheral shift mode: a data pin on clockPin (WR) is caught") {
    ExpanderMoonI80 peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.pinExpander = true;
    d.latchPin = 12;
    std::strcpy(d.pins, "1,2,10");   // pin 10 IS clockPin, and here WR is a real pad
    wire(d, peripheral, src, corr, 8 * 16);
    CHECK(d.severity() != mm::ParallelLedDriver::Severity::Status);   // it complains
}

// The '595 latch rides a DATA lane (the peripheral gives only one clock output, and WR is already the shift clock), so it must not land on WR, the latch would ride the shift clock itself and nothing would ever latch, which looks like a dead strip rather than a config error.
//
// clockPin now lives on the MoonI80Peripheral backend (not a MoonI80Peripheral member), so it is read/set through the control API defineDriverControls() binds, the same mechanism the UI and persistence use.
TEST_CASE("MoonI80Peripheral rejects a latchPin on WR") {
    ExpanderMoonI80 peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.setPeripheralForTest(&peripheral);
    d.defineControls();
    d.pinExpander = true;
    const int idx = mm::test::controlIndex(d, "clockPin");
    REQUIRE(idx >= 0);
    d.latchPin = *static_cast<int8_t*>(d.controls()[static_cast<uint8_t>(idx)].ptr);
    wire(d, peripheral, src, corr, 8 * 16);
    CHECK(d.severity() == mm::ParallelLedDriver::Severity::Error);
}

// The '595's shift clock IS WR, so shift mode needs clockPin on a real GPIO. Unset (-1) would route the peripheral's WR signal to GPIO 65535, catch it as a config error, not a bad pad write. Direct mode does not care (WR is unrouted there), so the same unset pin is fine without the expander.
TEST_CASE("MoonI80Peripheral shift mode requires a clockPin; direct mode does not") {
    ExpanderMoonI80 peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.setPeripheralForTest(&peripheral);
    d.defineControls();
    d.pinExpander = true;
    d.latchPin = 12;
    mm::test::setControlValue<int8_t>(d, "clockPin", -1);   // unset
    wire(d, peripheral, src, corr, 8 * 16);
    CHECK(d.severity() == mm::ParallelLedDriver::Severity::Error);

    ExpanderMoonI80 peripheral2;
    mm::ParallelLedDriver d2;          // same unset clockPin, DIRECT mode → fine
    mm::Buffer src2;
    mm::Correction corr2;
    d2.setPeripheralForTest(&peripheral2);
    d2.defineControls();
    mm::test::setControlValue<int8_t>(d2, "clockPin", -1);
    std::strcpy(d2.pins, "1,2,4,5,6,7,8,9");
    wire(d2, peripheral2, src2, corr2, 8 * 16);
    CHECK(d2.severity() != mm::ParallelLedDriver::Severity::Error);
}

// Sanity: with a valid config the driver is a working CRTP sibling, it slices lanes and reports the lights it drives, exactly like its sibling. (The lane/frame ARITHMETIC itself is the base's, and is covered once, in unit_I80LedDriver and the Mock suites.)
TEST_CASE("MoonI80Peripheral drives a valid config like its sibling") {
    mm::MoonI80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 8 * 32);   // 8 lanes × 32 lights
    CHECK(d.laneCount() == 8);
}
