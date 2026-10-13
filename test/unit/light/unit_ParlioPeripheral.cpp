/// @module ParlioPeripheral
/// @also ParallelLedDriver, Drivers, Correction

#include "doctest.h"
#include "host_bus.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/drivers/ParlioPeripheral.h"
#include "light/layers/Buffer.h"
#include "unit/core/conditional_controls.h"  // shared conditional-control helpers

#include <cstring>

// Host-side half of the Parlio driver: lane slicing (the shared PinList semantics), the frame-byte arithmetic (latch pad, 64-byte alignment, RGBW growth), and the parse-error/recovery shape. The hardware half (TX unit init, DMA transmit) is inert on the host, desktop stubs return false/nullptr, and is proven on the P4. The encoder itself is shared with the LCD driver and is covered by unit_ParallelSlots.cpp, so it isn't re-tested here.
//
// The one behavioral difference from the LCD driver pinned below: Parlio has NO exactly-8-pins rule, 1..8 lanes are all valid (it takes the data GPIOs directly, no all-lanes-required i80 bus).
//
// mm::ParallelLedDriver is the ONE registered driver; this file drives it with an injected mm::ParlioPeripheral backend, the backend this header defines and registers under the "Parlio" peripheral label.

namespace {

void wire(mm::ParallelLedDriver& d, mm::ParlioPeripheral& peripheral, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights) {
    mm::test::wireParallel(d, peripheral, src, corr, lights, "20,21,22,23,24,25,26,27");
}

} // namespace

// Three lanes (Parlio accepts any 1..8 count) slice the buffer consecutively; the frame is sized by the LONGEST lane.
TEST_CASE("ParlioPeripheral slices lanes and sizes the frame by the longest") {
    mm::ParlioPeripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "36,37,38");
    std::strcpy(d.ledsPerPin, "50,20,20");
    wire(d, peripheral, src, corr, 90);

    REQUIRE(d.laneCount() == 3);
    CHECK(d.laneLightCount(0) == 50);
    CHECK(d.laneLightCount(1) == 20);
    CHECK(d.laneLightCount(2) == 20);
    CHECK(d.laneStart(0) == 0);
    CHECK(d.laneStart(1) == 50);
    CHECK(d.laneStart(2) == 70);
    CHECK(d.maxLaneLights() == 50);
    CHECK(d.frameBytes() == mm::test::expectParallelFrame(50, 3));
}

// Empty ledsPerPin (the default) splits evenly over the 8 lanes, shared PinList semantics, same as the RMT/LCD drivers.
TEST_CASE("ParlioPeripheral even split over 8 lanes") {
    mm::ParlioPeripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 256);         // ledsPerPin empty (default) = even split

    REQUIRE(d.laneCount() == 8);
    CHECK(d.laneLightCount(0) == 32);
    CHECK(d.laneLightCount(7) == 32);
    CHECK(d.maxLaneLights() == 32);
    CHECK(d.frameBytes() == mm::test::expectParallelFrame(32, 3));
}

// The Parlio-vs-LCD difference: 1..8 pins are ALL valid (no exactly-8 rule).
TEST_CASE("ParlioPeripheral accepts any lane count from 1 to 8") {
    mm::Correction corr;
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    for (const char* pinList : {"36", "36,37", "36,37,38,39,40", "36,37,38,39,40,41,42,43"}) {
        mm::ParlioPeripheral peripheral;
        mm::ParallelLedDriver d;
        mm::Buffer src;
        std::strcpy(d.pins, pinList);
        wire(d, peripheral, src, corr, 64);
        // count the commas+1 to know the expected lane count
        uint8_t expected = 1;
        for (const char* p = pinList; *p; p++) if (*p == ',') expected++;
        CHECK(d.laneCount() == expected);
        // Success now carries a neutral info readout ("driving N of M lights"), not a null status, assert it's info (not an error) and names the consumption.
        CHECK(d.severity() != mm::MoonModule::Severity::Error);
        CHECK(d.status() != nullptr);
        CHECK(std::strstr(d.status(), "driving") != nullptr);
    }
}

// 9..16 pins are accepted (Parlio drives 1..16, the 16-bit bus); more than 16 is rejected.
TEST_CASE("ParlioPeripheral accepts 9..16 pins, rejects more than 16") {
    mm::ParlioPeripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    {   // 12 pins → 12 lanes, valid (the 16-bit bus)
        std::strcpy(d.pins, "1,2,3,4,5,6,7,8,9,10,11,12");
        wire(d, peripheral, src, corr, 64);
        CHECK(d.laneCount() == 12);
        CHECK(d.severity() != mm::MoonModule::Severity::Error);   // valid → info, not error
    }
    {   // 17 pins → rejected (over the 16-lane cap)
        mm::ParlioPeripheral peripheral2;
        mm::ParallelLedDriver d2;
        std::strcpy(d2.pins, "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17");
        wire(d2, peripheral2, src, corr, 64);
        CHECK(d2.laneCount() == 0);
        CHECK(d2.status() != nullptr);
    }
}

// A 16-lane (>8) config uses the 16-bit bus, so each slot is 2 bytes: the frame is DOUBLE the byte size of the same per-lane lights at ≤8 lanes. Pins the slotBytes threading through frameBytesFor (including the doubled latch pad).
TEST_CASE("ParlioPeripheral 16-lane frame doubles the byte size (16-bit bus)") {
    mm::Buffer src;
    mm::Correction corr;
    {   // 8 lanes × 50 lights → 8-bit bus (slotBytes = 1)
        mm::ParlioPeripheral peripheral;
        mm::ParallelLedDriver d;
        std::strcpy(d.pins, "1,2,3,4,5,6,7,8");
        std::strcpy(d.ledsPerPin, "50,50,50,50,50,50,50,50");
        wire(d, peripheral, src, corr, 400);
        REQUIRE(d.laneCount() == 8);
        CHECK(d.maxLaneLights() == 50);
        CHECK(d.frameBytes() == mm::test::expectParallelFrame(50, 3, 1));
    }
    {   // 16 lanes × 50 lights → 16-bit bus (slotBytes = 2), same per-lane lights, DOUBLE bytes
        mm::ParlioPeripheral peripheral;
        mm::ParallelLedDriver d;
        std::strcpy(d.pins, "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16");
        std::strcpy(d.ledsPerPin, "50,50,50,50,50,50,50,50,50,50,50,50,50,50,50,50");
        wire(d, peripheral, src, corr, 800);
        REQUIRE(d.laneCount() == 16);
        CHECK(d.maxLaneLights() == 50);
        CHECK(d.frameBytes() == mm::test::expectParallelFrame(50, 3, 2));   // 16-bit slots → 2 bytes/slot
        // Sanity: the 16-bit frame is ~double the 8-bit one (modulo the 64-byte rounding).
        CHECK(d.frameBytes() > mm::test::expectParallelFrame(50, 3, 1));
    }
}

// An RGB→RGBW preset toggle grows the frame (32 vs 24 slot bytes per light).
TEST_CASE("ParlioPeripheral frame grows on RGBW preset") {
    mm::ParlioPeripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.ledsPerPin, "50,50");
    wire(d, peripheral, src, corr, 100);
    CHECK(d.frameBytes() == mm::test::expectParallelFrame(50, 3));

    // The driver owns its Correction, so mutate that copy (not the external one).
    mm::test::rebuildFromPreset(d.correctionForTest(), 255, mm::test::PresetOrder::GRBW);
    d.onCorrectionChanged();
    CHECK(d.frameBytes() == mm::test::expectParallelFrame(50, 4));
}

// Parlio single-transfer hardware ceiling (PARLIO_LL_TX_MAX_BITS_PER_FRAME = 0x7FFFF bits = 65535 bytes on P4/S3/most targets): the peripheral clocks the WHOLE frame out in one transaction, so a per-lane strand whose frameBytes exceeds this is rejected by parlioWs2812Init (fixed, was a silent tx failure). The ceiling is a **byte** limit (65535 bytes/lane), so the equivalent LIGHT count depends on channels-per-light: WS2812 encodes 24 slot-bytes per channel, plus a ~864-byte per-lane latch pad. So the max lights/lane is ~ (65535 − 864) / (channels × 24): **897 for RGB (3ch)**, ~673 for RGBW (4ch), ~538 for RGBCCT (5ch), wider fixtures fit fewer lights per one-shot transfer. This pins the boundary in host-visible frameBytes terms for the RGB and RGBW cases. The reject itself is hardware-only (the host bus allocates but enforces no Parlio transfer ceiling), verified on the P4 (LEDs burn at 8×896 RGB/lane; the driver reports a status error above the ceiling). Catches the ceiling shifting if the encoding changes. Mirrors the platform constant.
TEST_CASE("ParlioPeripheral frame at the Parlio single-transfer ceiling (byte limit, channel-relative)") {
    constexpr size_t kParlioMaxTransferBytes = 0x7FFFF / 8;   // 65535, matches platform_esp32_parlio.cpp
    mm::ParlioPeripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    // 896 RGB lights/lane, the HW-tested config, FITS one transfer.
    std::strcpy(d.pins, "20,21,22,23,24,25,26,27");
    std::strcpy(d.ledsPerPin, "896,896,896,896,896,896,896,896");
    wire(d, peripheral, src, corr, 896 * 8);
    CHECK(d.maxLaneLights() == 896);
    CHECK(d.frameBytes() == mm::test::expectParallelFrame(896, 3));
    CHECK(d.frameBytes() <= kParlioMaxTransferBytes);          // fits one Parlio transfer
    // The exact RGB (3ch) boundary: 897 fits, 898 overflows.
    CHECK(mm::test::expectParallelFrame(897, 3) <= kParlioMaxTransferBytes);
    CHECK(mm::test::expectParallelFrame(898, 3) > kParlioMaxTransferBytes);
    // RGBW (4ch) fits FEWER lights per one-shot transfer, the ceiling is bytes, not lights: ~673.
    CHECK(mm::test::expectParallelFrame(673, 4) <= kParlioMaxTransferBytes);
    CHECK(mm::test::expectParallelFrame(674, 4) > kParlioMaxTransferBytes);
}

// A bad pin list idles the driver with the parse literal in the status; fixing it recovers.
TEST_CASE("ParlioPeripheral bad pins → status error → recovery") {
    mm::ParlioPeripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "36,nope");
    wire(d, peripheral, src, corr, 64);

    CHECK(d.laneCount() == 0);
    CHECK(d.frameBytes() == 0);
    CHECK(d.status() != nullptr);

    std::strcpy(d.pins, "36,37");
    d.applyState();
    CHECK(d.laneCount() == 2);
    // Recovered: the parse error is cleared, replaced by the neutral consumption info.
    CHECK(d.severity() != mm::MoonModule::Severity::Error);
    CHECK(std::strstr(d.status() ? d.status() : "", "driving") != nullptr);
}

// Pins now default UNSET (the "default only when it cannot do harm" rule, the strand is user-soldered). A fresh, unconfigured driver idles, never grabbing a GPIO. (wire() back-fills empty pins for the slicing cases, so this one wires the buffer directly to keep pins empty.)
TEST_CASE("ParlioPeripheral with the empty default pins idles cleanly") {
    mm::ParlioPeripheral peripheral;
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

// A 0×0×0 grid is a clean idle: zero counts, zero frame, no crash.
TEST_CASE("ParlioPeripheral tolerates a zero-light buffer") {
    mm::ParlioPeripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 0);

    CHECK(d.laneCount() == 8);       // the default 8 pins parse fine
    CHECK(d.maxLaneLights() == 0);
    CHECK(d.frameBytes() == 0);
    d.tick();                        // must be a no-op, not a crash
    CHECK(true);
}

// tick() is crash-safe across single-pin / multi-pin / pre-init configs (the transmit path is gated out on the host; this pins the reachable contract).
TEST_CASE("ParlioPeripheral tick is crash-safe for every pin configuration") {
    mm::Correction corr;
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);

    SUBCASE("single pin, populated grid") {
        mm::ParlioPeripheral peripheral;
        mm::ParallelLedDriver d; mm::Buffer src;
        std::strcpy(d.pins, "36");
        wire(d, peripheral, src, corr, 64);
        d.tick();
    }
    SUBCASE("multi-pin even split") {
        mm::ParlioPeripheral peripheral;
        mm::ParallelLedDriver d; mm::Buffer src;
        std::strcpy(d.pins, "36,37,38");
        wire(d, peripheral, src, corr, 90);
        REQUIRE(d.laneCount() == 3);
        d.tick();
    }
    SUBCASE("tick before any buffer is wired") {
        mm::ParlioPeripheral peripheral;
        mm::ParallelLedDriver d;
        d.setPeripheralForTest(&peripheral);
        d.defineControls();
        d.tick();
    }
    CHECK(true);
}

// setup/release cycles leave no residue (status clean, ASAN-checked heap).
TEST_CASE("ParlioPeripheral setup/release is repeatable") {
    mm::test::checkSetupReleaseRepeatable<mm::ParlioPeripheral>("20,21,22,23,24,25,26,27", 8);
}

// loopbackRxPin is bound always, visible only while loopbackTest is on.
TEST_CASE("ParlioPeripheral loopbackRxPin tracks the loopbackTest toggle") {
    mm::ParlioPeripheral peripheral;
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

// loopbackTxPin (optional lane-0 TX override) is bound always, hidden until the test is on, same conditional-control contract as loopbackRxPin. The override's lane-0 substitution is hardware-only (parlioLanes==0 on desktop); the visibility contract is host-testable here via the shared helper (toggles loopbackTest both ways and asserts the control stays bound while flipping visibility).
TEST_CASE("ParlioPeripheral loopbackTxPin tracks the loopbackTest toggle") {
    mm::test::checkLoopbackTxPinTracksTest<mm::ParlioPeripheral>();
}

// The host bus is REAL MEMORY, not a refusal: `busInit` used to return false on desktop, so every bus assertion was unreachable off-device and the driver's encode path only ever ran on hardware. The contract is identical for all three peripherals, so it lives in one place.
TEST_CASE("ParlioPeripheral allocates a real host bus the driver can encode into") {
    mm::test::checkHostBusAllocates<mm::ParlioPeripheral>();
}

TEST_CASE("ParlioPeripheral gives the host bus two distinct buffers when asked") {
    mm::test::checkHostBusDoubleBuffer<mm::ParlioPeripheral>();
}

TEST_CASE("ParlioPeripheral counts the DMA buffers in its memory readout") {
    mm::test::checkHostBusCountedInHeapReadout<mm::ParlioPeripheral>();
}
