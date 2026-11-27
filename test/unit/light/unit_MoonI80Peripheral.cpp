/// @module MoonI80Peripheral
/// @also I80Peripheral, ParallelLedDriver

#include "doctest.h"
#include "host_bus.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/drivers/MoonI80Peripheral.h"
#include "light/layers/Buffer.h"
#include "unit/core/conditional_controls.h"  // controlIndex/setControlValue: clockPin is set through the control the backend declares
#include "platform/platform_heap.h"   // kInternalFreeFloor

#include <cstring>

// MoonI80Peripheral is the same LCD_CAM output as I80Peripheral on our own DMA code; the Mock-driver suites cover the orchestrator, so these cases pin its own parts: the LedPeripheral contract, LCD_CAM-only constants and bus-pin validation.

namespace {

// On the host `platform::lcdLanes` is 0, so the real backend reports supportsPinExpander() == false and the orchestrator drops pinExpander to direct mode; the shift-mode cases use this subclass, which reports the expander as supported.
struct ExpanderMoonI80 : mm::MoonI80Peripheral {
    bool supportsPinExpander() const override { return true; }
};

void wire(mm::ParallelLedDriver& d, mm::MoonI80Peripheral& peripheral, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights) {
    mm::wireParallel(d, peripheral, src, corr, lights, "1,2,4,5,6,7,8,9");
}

} // namespace

// The load-bearing difference from its sibling: MoonI80 programs LCD_CAM directly and claims no classic chip, so lanesAvailable() reads lcdLanes alone; it and the other backend virtuals are reachable on a bare instance.
TEST_CASE("MoonI80Peripheral is LCD_CAM-only — it does not claim the classic ESP32's I2S i80") {
    mm::MoonI80Peripheral peripheral;
    CHECK(peripheral.lanesAvailable() == mm::platform::lcdLanes);
    // The expander needs LCD_CAM silicon, `hasLcdCam`, rather than `lcdLanes > 0`: a lane count is how wide, the capability is which peripheral (the host sets a non-zero lane count so the driver runs against a memory bus).
    CHECK(peripheral.supportsPinExpander() == mm::platform::hasLcdCam);
}

// The i80 BUS is 8 or 16 bits wide whatever the pin count, so the base rounds it up (powerOfTwoBus()) and parks the unused lanes; the loopback cannot build a 1-lane private bus, so its test frame is encoded at the full operational width.
TEST_CASE("MoonI80Peripheral keeps the i80 bus rules: power-of-two bus, full-width loopback") {
    mm::MoonI80Peripheral peripheral;
    CHECK(peripheral.powerOfTwoBus());
    CHECK(peripheral.loopbackFullWidth());
}

// The bus control pins are a '595 cost, not an i80 cost: DC is held constant and WR only clocks a shift register, so in DIRECT mode `clockPin` may name a GPIO a strand also uses, since the signal stays inside the peripheral.
TEST_CASE("MoonI80Peripheral direct mode: clockPin is unrouted, so it cannot collide") {
    mm::MoonI80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "1,2,4,5,6,7,8,10");   // pin 10 IS the default clockPin (WR), fine here
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

// The '595 latch rides a DATA lane (WR is already the shift clock), so it must stay off WR to latch at all (a clash looks like a dead strip); clockPin is read and set through the control API defineDriverControls() binds, like the UI and persistence.
TEST_CASE("MoonI80Peripheral rejects a latchPin on WR") {
    ExpanderMoonI80 peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.setPeripheralForTest(&peripheral);
    d.defineControls();
    d.pinExpander = true;
    const int idx = mm::controlIndex(d, "clockPin");
    REQUIRE(idx >= 0);
    d.latchPin = *static_cast<int8_t*>(d.controls()[static_cast<uint8_t>(idx)].ptr);
    wire(d, peripheral, src, corr, 8 * 16);
    CHECK(d.severity() == mm::ParallelLedDriver::Severity::Error);
}

// The '595's shift clock IS WR, so shift mode needs clockPin on a real GPIO: an unset (-1) pin is caught as a config error instead of routing WR to GPIO 65535. Direct mode leaves WR unrouted, so the unset pin is fine there.
TEST_CASE("MoonI80Peripheral shift mode requires a clockPin; direct mode does not") {
    ExpanderMoonI80 peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.setPeripheralForTest(&peripheral);
    d.defineControls();
    d.pinExpander = true;
    d.latchPin = 12;
    mm::setControlValue<int8_t>(d, "clockPin", -1);   // unset
    wire(d, peripheral, src, corr, 8 * 16);
    CHECK(d.severity() == mm::ParallelLedDriver::Severity::Error);

    ExpanderMoonI80 peripheral2;
    mm::ParallelLedDriver d2;          // same unset clockPin, DIRECT mode → fine
    mm::Buffer src2;
    mm::Correction corr2;
    d2.setPeripheralForTest(&peripheral2);
    d2.defineControls();
    mm::setControlValue<int8_t>(d2, "clockPin", -1);
    std::strcpy(d2.pins, "1,2,4,5,6,7,8,9");
    wire(d2, peripheral2, src2, corr2, 8 * 16);
    CHECK(d2.severity() != mm::ParallelLedDriver::Severity::Error);
}

// Sanity: with a valid config the driver slices lanes and reports the lights it drives, like its sibling (the lane and frame arithmetic is the base's, covered in unit_I80LedDriver and the Mock suites).
TEST_CASE("MoonI80Peripheral drives a valid config like its sibling") {
    mm::MoonI80Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 8 * 32);   // 8 lanes × 32 lights
    CHECK(d.laneCount() == 8);
}

// ringAuto once took every internal byte above a 64 KB reserve, 28 buffers on shiffy, leaving 66 KB free; the ring needs only its lead over the wire.
TEST_CASE("MoonI80Peripheral ringAuto takes 3-row buffers for its lead, not the free RAM") {
    // shiffy: 585 bytes per row, 259 rows per frame.
    const auto d = mm::MoonI80Peripheral::autoDepth(585, 259, 200 * 1024);
    CHECK(d.rows == 3);
    CHECK(d.bufs == 21);   // 54 rows of lead over 3-row buffers, plus the 2 the refill keeps clear and the one it writes
    CHECK_FALSE(d.shortOfLead);
}

TEST_CASE("MoonI80Peripheral ringAuto takes fewer rows per buffer when a row nearly fills a node") {
    const auto d = mm::MoonI80Peripheral::autoDepth(1500, 259, 400 * 1024);   // 2 rows fill a 4 KB node
    CHECK(d.rows == 2);
}

TEST_CASE("MoonI80Peripheral ringAuto never takes more than one buffer per slice and a spare") {
    const auto d = mm::MoonI80Peripheral::autoDepth(585, 20, 200 * 1024);   // a small frame: 7 slices
    CHECK(d.bufs == 8);
    CHECK_FALSE(d.shortOfLead);
}

TEST_CASE("MoonI80Peripheral ringAuto stops at the internal floor and says the lead is short") {
    const size_t freeInternal = mm::platform::kInternalFreeFloor + 6 * 3 * 585;   // room for 6 buffers above the floor
    const auto d = mm::MoonI80Peripheral::autoDepth(585, 259, freeInternal);
    CHECK(d.bufs == 6);
    CHECK(d.shortOfLead);
}

// The ring's headroom: its refill's least lead over the most a ring of that depth holds, so a healthy ring of any depth reads near 100, a late slice 0, and a ring before its first interrupt none.
TEST_CASE("MoonI80Peripheral's ring headroom is the least lead over the most the ring holds, 0 on a late slice, none before a lead is measured") {
    CHECK(mm::MoonI80Peripheral::ringHeadroom(18, 21, false) == 100);   // 21 buffers: the refill gap and the draining slice leave 18
    CHECK(mm::MoonI80Peripheral::ringHeadroom(9, 21, false) == 50);
    CHECK(mm::MoonI80Peripheral::ringHeadroom(1, 4, false) == 100);     // a small ring at its most is healthy too
    CHECK(mm::MoonI80Peripheral::ringHeadroom(-1, 21, true) == 0);      // a late slice: behind, whatever the lead read
    CHECK(mm::MoonI80Peripheral::ringHeadroom(18, 21, true) == 0);
    CHECK_FALSE(mm::MoonI80Peripheral::ringHeadroom(-1, 21, false).has_value());   // no interrupt yet
}
