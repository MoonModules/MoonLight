/// @module RmtLedDriver
/// @also Drivers, Correction

#include "doctest.h"
#include "light/drivers/RmtLedDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"
#include "unit/core/conditional_controls.h"  // shared conditional-control helpers

#include <cstring>  // std::strcpy (writing the pins text control directly)

// These tests pin the symbol-buffer LIFECYCLE, the exact class of bug that reached hardware: a review fix made deinit() free symbols_, and because reinit() calls deinit(), every rebuild freed the buffer tick() needs, so the driver silently stopped transmitting. None of that touches the RMT peripheral (ESP32-only); it's pure host-testable buffer ownership, which is why a unit test is the right guard. The frameBuffer()/frameCapacity() accessors are test-only (mirror ArtNet's correctedBuffer()).

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

// The resting status is "driving N of M lights" after a build, shown by DEFAULT (the way MoonLed does), not only after the user touches a control. prepare() re-asserts it after the full build (pins + buffer + counts settled), so a driver that built cleanly always advertises its consumption. This also overwrites any stale transient (e.g. a prior loopback verdict), which must NOT linger as the resting status once the driver is driving lights.
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

// The symbol buffer sizes to what the pins CLOCK OUT (txLightCount_), NOT the window. A small strip on one pin (ledsPerPin 64) inside a huge grid (window = all 5740 lights) must reserve symbols for 64, not 5740, else it tries to alloc ~550 KB it never encodes, the alloc fails on a small-heap board, and the strip goes dark even though only 64 lights were wanted (the bug this pins; ParallelLedDriver already sizes its frame to the driven count, RmtLed did not). ledsPerPin caps the pin; tick() only encodes 64.
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
    // The regression: prepare() does resizeSymbols() THEN reinit(), and a bad reinit()->deinit() freed symbols_ right after it was allocated, so the buffer was null by the time tick() ran. A second prepare (what a pins change / topology rebuild triggers) must leave the buffer present.
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
    // Core's applyState() routes an effectively-enabled module to prepare() (acquire) and a disabled one to release() (release), so setEnabled(false) + the post-toggle prepareTree() sweep frees the peripheral + buffer (pins reusable). Observable via the symbol buffer: present when enabled, gone after the disabled sweep, rebuilt on the re-enabled sweep.
    mm::RmtLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, src, corr, 64);
    REQUIRE(d.frameBuffer() != nullptr);   // enabled by default → resource held

    // The Scheduler runs a whole-tree prepareTree() AFTER the enabled-toggle (Scheduler::setControl), which calls applyState() on every module. A just-disabled one routes to release → released, and must STAY released on subsequent sweeps (else a disabled RMT driver re-grabs a GPIO an enabled Parlio driver now owns).
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
    // Core's applyState() (the boot Phase-4 sweep + every prepareTree) calls prepare() only when effectively-enabled, else release(). A driver persisted DISABLED but sharing a GPIO with an enabled sibling must not grab the peripheral/pins, else it steals the pin and the enabled sibling's output stays dead. This reproduces the hardware symptom: a disabled RmtLed on GPIO 20 blanking an enabled ParlioLed on the same pin.
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

// MoonModule contract: release reverses setup, so setup→release→setup→release cycles leave no residue, no leaked heap (ASAN in the test runner catches that), no stuck state. After each release the driver must look untouched: no symbol buffer, no status. Run several cycles to surface any accumulation.
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

// Conditional control: loopbackRxPin is visible only while loopbackTest is on, hidden otherwise, but always bound (so a saved rxPin loads regardless). Same add-then-setHidden pattern as NetworkModule (architecture.md § Conditional controls). This pins the exact behavior that, with the old UI, showed the pin at the wrong times; a regression in the C++ flag now fails here.
TEST_CASE("RmtLedDriver loopbackRxPin tracks the loopbackTest toggle") {
    mm::RmtLedDriver d;
    d.defineControls();
    auto setTest = [&](bool on) {
        mm::setControlValue<bool>(d, "loopbackTest", on);
    };
    mm::checkConditionalControl(d, "loopbackRxPin", setTest, /*visibleWhenTrue=*/true);
}

// loopbackTxPin is the optional TX override (transmit on it instead of pins[0] during the self-test). Like loopbackRxPin it's a conditional control: always bound (so a saved override loads), shown only while loopbackTest is on. The override's effect on the transmitted pin is hardware-only (rmtTxChannels==0 on host), but the conditional-visibility contract is host-testable here.
TEST_CASE("RmtLedDriver loopbackTxPin tracks the loopbackTest toggle") {
    mm::RmtLedDriver d;
    d.defineControls();
    auto setTest = [&](bool on) {
        mm::setControlValue<bool>(d, "loopbackTest", on);
    };
    mm::checkConditionalControl(d, "loopbackTxPin", setTest, /*visibleWhenTrue=*/true);
}

// Editing `pins` while the loopback test is ON must refresh the parsed config before the self-test runs, onControlChanged fires before the prepareTree sweep re-parses, so without the in-branch parseConfig() the test would transmit on the OLD pin and show a verdict for it. Mirrors the fix in ParallelLedDriver; this pins the RMT sibling that the dedup left behind. Host-observable via pinCount(): the refresh re-parses to the new pin set even though the platform loopback itself is inert.
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
