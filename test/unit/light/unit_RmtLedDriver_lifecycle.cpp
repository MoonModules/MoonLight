/// @module RmtLedDriver
/// @also Drivers, Correction

#include "doctest.h"
#include "light/drivers/RmtLedDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"
#include "unit/core/conditional_controls.h"  // shared conditional-control helpers

#include <cstring>  // std::strcpy (writing the pins text control directly)

// These pin the frame buffer's lifecycle, which once reached hardware: deinit() freed it, reinit() calls deinit(), so every rebuild left tick() no buffer and the strip went dark; buffer ownership is host-testable.

namespace {

// Wire a driver up to a source buffer + correction the way the Drivers container does, then run applyState (the sizing hook). Returns nothing; the caller inspects the driver.
void wire(mm::RmtLedDriver& d, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights = 64) {
    REQUIRE(src.allocate(lights, 3));   // a masked alloc failure would fail cases downstream
    mm::rebuildFromPreset(corr, 255, mm::PresetOrder::GRB);   // 3 out-channels
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

} // namespace

TEST_CASE("RmtLedDriver sizes the symbol buffer in prepare") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64);

    // 64 lights x 3 channels = 192 wire bytes (the peripheral expands each to 8 symbols).
    REQUIRE(d.frameBuffer() != nullptr);
    CHECK(d.frameCapacity() >= static_cast<size_t>(64) * 3);
}

// After a build the status rests on "driving N of M lights", since prepare() re-asserts it once pins, buffer and counts settle, replacing a stale transient such as a loopback verdict.
TEST_CASE("RmtLedDriver shows 'driving N of M' as the resting status after a build") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "16");
    std::strcpy(d.ledsPerPin, "64");
    wire(d, src, corr, 256);   // defineControls + setSourceBuffer + applyState (the build)

    REQUIRE(d.status() != nullptr);
    CHECK(std::strstr(d.status(), "driving 64 of 256") != nullptr);
    CHECK(d.severity() != mm::MoonModule::Severity::Error);

    // A rebuild (the prepareTree sweep, a resize, an enable) re-asserts it, so the resting status is stable across rebuilds and never silently blanks.
    d.applyState();
    REQUIRE(d.status() != nullptr);
    CHECK(std::strstr(d.status(), "driving 64 of 256") != nullptr);
}

// The buffer sizes to the lights the pins clock out, not the window: one 64-light pin in a 5,740-light grid reserves 64, where sizing to the window asked ~550 KB and darkened a small-heap board.
TEST_CASE("RmtLedDriver sizes symbols to the driven lights, not the whole window") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "16");
    std::strcpy(d.ledsPerPin, "64");    // one pin, 64 lights — the physical 8×8 strip
    wire(d, src, corr, 5740);           // but a 70×82 grid in the buffer (count defaults to all)

    REQUIRE(d.frameBuffer() != nullptr);                          // allocated (64 lights fits easily)
    CHECK(d.frameCapacity() >= static_cast<size_t>(64) * 3);  // holds the 64 it encodes
    // The window is 5740, but the buffer must NOT be sized for it (that was the ~550 KB over-alloc).
    CHECK(d.frameCapacity() < static_cast<size_t>(5740) * 3);
}

TEST_CASE("RmtLedDriver keeps the symbol buffer across a rebuild (reinit must not free it)") {
    // The regression: prepare() resizes then calls reinit(), whose deinit() freed the fresh buffer; a second prepare, as a pins change triggers, keeps the buffer.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64);
    REQUIRE(d.frameBuffer() != nullptr);

    d.applyState();   // simulate a rebuild (the path that runs reinit())
    CHECK(d.frameBuffer() != nullptr);   // would be null with the deinit()-frees bug
    CHECK(d.frameCapacity() >= static_cast<size_t>(64) * 3);
}

TEST_CASE("RmtLedDriver keeps the symbol buffer across a pins change") {
    // Same regression class as above, multi-pin flavour: editing the pins list triggers a rebuild that re-parses and re-inits N channels, none of which may free the symbol buffer tick() encodes into.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64);
    REQUIRE(d.frameBuffer() != nullptr);

    std::strcpy(d.pins, "18,17");
    d.applyState();
    CHECK(d.frameBuffer() != nullptr);
    CHECK(d.pinCount() == 2);
}

TEST_CASE("RmtLedDriver grows the symbol buffer when the grid grows") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 16);
    const size_t cap16 = d.frameCapacity();
    CHECK(cap16 >= static_cast<size_t>(16) * 3);

    // Grow the source to 256 lights and rebuild: capacity must grow to fit.
    src.allocate(256, 3);
    d.setSourceBuffer(&src);
    d.applyState();
    CHECK(d.frameBuffer() != nullptr);
    CHECK(d.frameCapacity() >= static_cast<size_t>(256) * 3);
}

TEST_CASE("RmtLedDriver releases the symbol buffer on release") {
    // The leak CodeRabbit flagged: release must free the buffer (and only release, not deinit, which reinit calls).
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64);
    REQUIRE(d.frameBuffer() != nullptr);

    d.release();
    CHECK(d.frameBuffer() == nullptr);
    CHECK(d.frameCapacity() == 0);
}

TEST_CASE("RmtLedDriver: disabling releases the resource, re-enabling re-acquires (applyState)") {
    // applyState() prepares an enabled module and releases a disabled one, so disabling frees the peripheral and the buffer and re-enabling rebuilds them, the buffer showing which.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64);
    REQUIRE(d.frameBuffer() != nullptr);   // enabled by default → resource held

    // The Scheduler sweeps prepareTree() after the toggle, and a disabled driver stays released through every later sweep, so it leaves a GPIO free for an enabled Parlio driver.
    d.setEnabled(false);
    d.applyState();                         // disabled → release
    CHECK(d.frameBuffer() == nullptr);     // buffer freed (RAM back; pins released)
    CHECK(d.frameCapacity() == 0);

    d.applyState();                         // a later unrelated sweep — STILL released
    CHECK(d.frameBuffer() == nullptr);

    d.setEnabled(true);
    d.applyState();                         // enabled → prepare re-acquires
    CHECK(d.frameBuffer() != nullptr);     // resource re-acquired on re-enable
}

TEST_CASE("RmtLedDriver: a DISABLED driver does not acquire through the boot sweep") {
    // A driver saved disabled and sharing a GPIO with an enabled sibling leaves the pin alone: on hardware a disabled RmtLed on GPIO 20 blanked an enabled ParlioLed there.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    REQUIRE(src.allocate(64, 3));
    mm::rebuildFromPreset(corr, 255, mm::PresetOrder::GRB);
    d.defineControls();
    d.setEnabled(false);          // persisted-disabled at boot
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;

    d.setup();                    // Phase 3: pure wiring, no acquire
    d.applyState();               // Phase 4: disabled → routes to release, no acquire
    CHECK(d.frameBuffer() == nullptr);

    d.setEnabled(true);           // now enable → acquire on the next sweep
    d.applyState();
    CHECK(d.frameBuffer() != nullptr);
}

// release reverses setup, so repeated setup and release cycles leave no buffer, no status and no leaked heap, which ASan in the test runner catches.
TEST_CASE("RmtLedDriver setup/release is repeatable with no residual state") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    src.allocate(64, 3);
    mm::rebuildFromPreset(corr, 255, mm::PresetOrder::GRB);
    d.defineControls();

    for (int cycle = 0; cycle < 4; cycle++) {
        d.setup();                       // (re)init the channel
        d.setSourceBuffer(&src);         // resizeSymbols allocates the buffer
        d.correctionForTest() = corr;
        d.applyState();                // size buffer + reinit, as the Scheduler does
        REQUIRE(d.frameBuffer() != nullptr);

        d.release();                    // must fully reverse the above
        CHECK(d.frameBuffer() == nullptr);   // buffer freed (ASAN: no leak across cycles)
        CHECK(d.frameCapacity() == 0);
        CHECK(d.status() == nullptr);         // no lingering status string
    }
}

// loopbackRxPin shows only while loopbackTest is on and stays bound throughout, so a saved pin loads: the add-then-setHidden pattern of a conditional control.
TEST_CASE("RmtLedDriver loopbackRxPin tracks the loopbackTest toggle") {
    mm::RmtLedDriver d;
    d.defineControls();
    auto setTest = [&](bool on) {
        mm::setControlValue<bool>(d, "loopbackTest", on);
    };
    mm::checkConditionalControl(d, "loopbackRxPin", setTest, /*visibleWhenTrue=*/true);
}

// loopbackTxPin, the self-test's optional transmit pin, is a conditional control like loopbackRxPin: bound always, shown while loopbackTest is on; its effect on the wire is hardware's.
TEST_CASE("RmtLedDriver loopbackTxPin tracks the loopbackTest toggle") {
    mm::RmtLedDriver d;
    d.defineControls();
    auto setTest = [&](bool on) {
        mm::setControlValue<bool>(d, "loopbackTest", on);
    };
    mm::checkConditionalControl(d, "loopbackTxPin", setTest, /*visibleWhenTrue=*/true);
}

// Editing `pins` with the loopback test on re-parses before the self-test runs, so the test transmits on the new pins, the fix ParallelLedDriver carries; pinCount() shows it on the host.
TEST_CASE("RmtLedDriver loopback re-parses pins before testing (no stale-pin verdict)") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    src.allocate(64, 3);
    mm::rebuildFromPreset(corr, 255, mm::PresetOrder::GRB);
    std::strcpy(d.pins, "18");
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
    REQUIRE(d.pinCount() == 1);

    // Turn the test on, then edit pins to a 3-pin set and fire the pin update the way the control framework does (write the buffer, then onControlChanged). The config must already reflect 3 pins by the time the self-test reads pinList_.
    mm::setControlValue<bool>(d, "loopbackTest", true);
    std::strcpy(d.pins, "18,17,16");
    d.onControlChanged("pins");
    CHECK(d.pinCount() == 3);   // refreshed before the test, not the stale 1
}

// A refill has one half-block's wire time before the peripheral replays the half it was due to write: the C3 lit its cube partly and out of order.
TEST_CASE("RmtLedDriver's headroom is how early its slowest refill came against the half-block deadline") {
    using D = mm::RmtLedDriver;
    CHECK_FALSE(D::refillHeadroom(0, 4800).has_value());     // no refill ran yet
    CHECK_FALSE(D::refillHeadroom(4800, 0).has_value());     // no period known
    CHECK(D::refillHeadroom(4800, 4800) == 100);             // every refill on its beat
    CHECK(D::refillHeadroom(3000, 4800) == 100);             // an early one is no more than on time
    CHECK(D::refillHeadroom(7200, 4800) == 50);              // half the deadline spent waiting
    CHECK(D::refillHeadroom(9600, 4800) == 0);               // a whole half late: it replayed stale symbols
    CHECK(D::refillHeadroom(48000, 4800) == 0);              // and far beyond
}

// Off-target no refill runs, so the driver claims no headroom rather than a full one.
TEST_CASE("RmtLedDriver reports no headroom where no refill ran") {
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr);
    d.tick();
    d.tick1s();
    CHECK_FALSE(d.headroom().has_value());
    d.release();
}
