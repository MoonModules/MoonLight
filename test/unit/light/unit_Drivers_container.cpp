/// @module Drivers

#include "doctest.h"
#include "light/drivers/Drivers.h"
#include "light/drivers/FixtureProfilesModule.h"   // the non-deletable boot-wired fixture-profile library
#include "light/drivers/NetworkSendDriver.h"     // a real driver, for the sibling-instance cases
#include "light/drivers/ParallelLedDriver.h"     // its installer and bench controls, for the mode case
#include "light/drivers/RmtLedDriver.h"
#include "correction_presets.h"                  // mm::test::rebuildFromPreset
#include "../core/conditional_controls.h"   // mm::test::setControlValue
#include "platform/platform.h"                 // gpioRead: the desktop reads back what gpioWrite put there

#include <cstring>
#include <string>

// Regression: the UI's enable/disable toggle on a child driver (e.g. ArtNet, Preview) was a no-op, the driver kept running. Cause: Drivers::tick() called child(i)->tick() unconditionally, skipping the per-child `enabled` check that Layer::tick() does for effects and Effects::tick() does for its child Layers.
// (The Scheduler only walks top-level modules, so it never sees these children.)
//
// These tests pin the gate so the regression can't return silently. A stub driver counts its loop calls; toggling `enabled` must flip whether the count advances.

namespace {

// Minimal DriverBase stub: counts loop calls. Ignores the source buffer it would normally consume, this test cares only about whether tick() runs.
class CountingDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    void tick() MM_NONBLOCKING override { loopCalls++; }
    int loopCalls = 0;
};

} // namespace

// A minimal driver so a test can read the resulting LUT. Each driver owns its Correction (DriverBase::correction_); the container fills it via rebuildCorrection(). Defines the correction controls (localBrightness / fixture / whiteMode) so a test can drive them.
class CorrectionCapturingDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    void tick() MM_NONBLOCKING override {}
    void defineDriverControls() override { defineCorrectionControls(); }
};

// Counts prepare() vs onCorrectionChanged() so a test can prove a refresh is correction-only. prepare() is the STRUCTURAL rebuild (reinits a real driver's output peripheral, blanking the strip for a tick); onCorrectionChanged() is the light tier-1 refresh that touches no peripheral.
class RebuildTrackingDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    void tick() MM_NONBLOCKING override {}
    void defineDriverControls() override { defineCorrectionControls(); }
    void prepare() override { prepareCalls++; }
    void onCorrectionChanged() override { correctionCalls++; }
    int prepareCalls = 0;
    int correctionCalls = 0;
};

// Regression: a profile edit re-ran prepare on the whole tree, whose output reinit blanked every strip for half a second; the list-changed hook re-resolves the correction alone.
TEST_CASE("A fixture-profile list edit re-resolves each driver's correction without re-preparing it") {
    mm::FixtureProfilesModule lib;       // construction claims the library seat
    mm::Drivers drivers;
    RebuildTrackingDriver drv;
    drivers.addChild(&drv);
    drivers.on = true;
    drivers.brightness = 200;
    drv.defineControls();
    drivers.setup();                     // seeds each driver's correction
    const int baselineCorrection = drv.correctionCalls;
    const int baselinePrepare = drv.prepareCalls;

    drv.onListChanged(lib);
    CHECK(drv.correctionCalls > baselineCorrection);   // the edit reaches the output
    CHECK(drv.prepareCalls == baselinePrepare);        // with no peripheral reinit, so no blank

    // Another module's list is not one a driver resolves from.
    const int afterLib = drv.correctionCalls;
    drv.onListChanged(drivers);
    CHECK(drv.correctionCalls == afterLib);
}

// Core reads the light pipeline through LightOutput, so the seat must hold only a live, prepared Drivers.
TEST_CASE("Drivers is the light output core reads, from prepare until release") {
    mm::Drivers drivers;
    CHECK(mm::LightOutput::active() != &drivers);   // a probe that never prepares never publishes
    drivers.prepare();
    CHECK(mm::LightOutput::active() == &drivers);
    CHECK(&mm::lightSummary() == &drivers.summary());
    // A palette's own color finds a palette of the same color.
    const mm::LightOutput& out = drivers;
    const mm::RGB c = out.paletteRgb(out.nearestPalette(mm::RGB{255, 0, 0}));
    CHECK(c.r == 255);
    drivers.release();
    CHECK(mm::LightOutput::active() == nullptr);
    CHECK(mm::lightSummary().lightCount == 0);      // the all-zero summary, never a dangling one
}

// The `on` control is master power: on=false scales the correction LUT to zero (output black) while PRESERVING the brightness value, so on=true restores the exact level. It rides the same cheap LUT rebuild as brightness (no pipeline realloc). This pins the shared power control IR/MQTT/WLED drive.
TEST_CASE("Drivers::on gates the correction LUT without clobbering brightness") {
    mm::Drivers drivers;
    CorrectionCapturingDriver drv;
    drivers.addChild(&drv);
    drv.defineControls();
    // Linear: what this pins is that `on` gates the LUT WITHOUT losing the brightness value, and a perceptual curve would restate every expected number as a lookup without testing anything new.
    mm::test::setControlValue<uint8_t>(drv, "curve", 3);   // 3 = linear
    drivers.setup();                       // seeds drv's own correction_ from on(true)+brightness

    drivers.brightness = 200;
    drivers.on = true;
    drivers.onControlChanged("brightness");        // rebuild LUT at full power
    CHECK(drivers.effectiveBrightness() == 200);
    CHECK(drv.correctionForTest().briLut[255] == 200);   // (255 * 200) / 255 == 200

    // Turn off → LUT scales to black, but the brightness value is untouched.
    drivers.on = false;
    drivers.onControlChanged("on");
    CHECK(drivers.brightness == 200);            // value preserved
    CHECK(drivers.effectiveBrightness() == 0);
    CHECK(drv.correctionForTest().briLut[255] == 0);     // output black

    // Turn back on → the exact level returns, no stored-value juggling.
    drivers.on = true;
    drivers.onControlChanged("on");
    CHECK(drv.correctionForTest().briLut[255] == 200);
}

// Regression (the localBrightness bug): a per-driver localBrightness change must RE-SCALE that driver's correction LUT, global × local, just like a global brightness change does. The bug was that localBrightness edits didn't reach the LUT (only global did). Both sliders must reach output.
TEST_CASE("Drivers: a localBrightness change re-scales the driver's correction LUT") {
    mm::Drivers drivers;
    CorrectionCapturingDriver drv;
    drivers.addChild(&drv);
    drivers.brightness = 200;
    drivers.on = true;
    drv.defineControls();                           // bind the correction controls (localBrightness etc.)
    // LINEAR, so the arithmetic below reads as the multiplication it is testing. What this pins is that BOTH sliders reach the LUT, which a perceptual curve would leave true but express as table lookups nobody can check by eye. The curve has its own tests.
    mm::test::setControlValue<uint8_t>(drv, "curve", 3);   // 3 = linear
    drivers.setup();                                // seeds the driver's correction (global 200, local 255)
    CHECK(drv.correctionForTest().briLut[255] == 200);   // global 200 × local 255/255 = 200

    // Halve the driver's LOCAL brightness, its own control change must re-bake the LUT to global × local = 200 × 128/255 ≈ 100. This is the path the bug missed.
    mm::test::setControlValue<uint8_t>(drv, "localBrightness", 128);
    drv.onControlChanged("localBrightness");
    CHECK(drv.correctionForTest().briLut[255] == 100);   // (200 * 128) / 255 == 100

    // And the global slider still composes on top: raising global to 255 with local 128 → 128.
    drivers.brightness = 255;
    drivers.onControlChanged("brightness");
    CHECK(drv.correctionForTest().briLut[255] == 128);   // (255 * 128) / 255 == 128
}

// Disabled child drivers don't tick: toggling `enabled` flips whether that driver's tick() runs.
TEST_CASE("Drivers::tick() skips disabled child drivers") {
    mm::Drivers drivers;
    CountingDriver a, b;
    drivers.addChild(&a);
    drivers.addChild(&b);

    // Both enabled by default → both tick.
    drivers.tick();
    CHECK(a.loopCalls == 1);
    CHECK(b.loopCalls == 1);

    // Disable `a` → only `b` ticks.
    a.setEnabled(false);
    drivers.tick();
    CHECK(a.loopCalls == 1);  // unchanged
    CHECK(b.loopCalls == 2);

    // Disable `b` too → neither ticks.
    b.setEnabled(false);
    drivers.tick();
    CHECK(a.loopCalls == 1);
    CHECK(b.loopCalls == 2);

    // Re-enable `a` → only `a` ticks.
    a.setEnabled(true);
    drivers.tick();
    CHECK(a.loopCalls == 2);
    CHECK(b.loopCalls == 2);
}

// The "+ add" picker under Drivers must offer ONLY drivers, not every generic system module, else the 6 drivers are buried under ~18 generics (Devices, Filesystem, …). acceptsChildRoles drives that picker, so it returns "driver" alone. The one non-driver child (the boot-wired FixtureProfiles library) is added directly at boot, bypassing this check, and is non-deletable, so it needs no "generic" here. Pins the filter the product owner asked for.
TEST_CASE("Drivers accepts only driver-role children in the add picker") {
    mm::Drivers drivers;
    CHECK(std::strcmp(drivers.acceptsChildRoles(), "driver") == 0);
}

// The boot-wired fixture-profile library is a permanent singleton: not user-deletable (Drivers accepts only `driver`, so a deleted library could never be re-added, and every driver resolves its profile through it). Mirrors the boot-wired PreviewDriver's userEditable(false).
TEST_CASE("FixtureProfiles library is a non-deletable singleton") {
    mm::FixtureProfilesModule lib;
    CHECK_FALSE(lib.userEditable());
}

// Regression, the Drivers half of the dangling LivePalettes seam (the seam-contract half is pinned in unit_Palette.cpp): the /api/modules probe constructs a Drivers, reads its controls, and destroys it. That throwaway used to publish the seam from defineControls() and so owned it when it died, first dangling it (the /api/state SIGSEGV) and, once clear() ran in the destructor, emptying the running device's scripted-palette list instead. Publication belongs to prepare(), which only a scheduler-mounted module runs, so a probe must leave the seam exactly as it found it.
TEST_CASE("a probe Drivers (controls read, never prepared) leaves the scripted-palette seam alone") {
    static const char* names[] = {"running.mlp"};
    static const char* tags[]  = {""};
    mm::LivePalettes::set(names, tags, 1);            // the running Drivers' publication
    {
        mm::Drivers probe;                            // what serveModules builds…
        probe.defineControls();                       // …to read the control list…
    }                                                 // …and immediately destroys
    CHECK(mm::LivePalettes::count() == 1);
    CHECK(std::strcmp(mm::LivePalettes::nameAt(0), "running.mlp") == 0);
    mm::LivePalettes::clear();
}

// The power relay is the physical expression of "the lights are off", and brightness 0 is off as much as `on` = false is: a WLED-style client says off by sending bri 0 without touching `on`, and a strip at zero still draws its idle current through a closed relay. So the relay opens at brightness 0 and closes again the moment brightness returns, with `on` unchanged either way.
TEST_CASE("the relay opens at brightness 0 and closes again when brightness returns") {
    mm::platform::clearTestGpioLevel();
    mm::Drivers drivers;
    std::strcpy(drivers.relayPins, "12");
    drivers.on = true;
    drivers.brightness = 100;
    drivers.onControlChanged("relayPins");            // entering the pin closes the relay at once
    CHECK(mm::platform::gpioRead(12));

    drivers.brightness = 0;
    drivers.onControlChanged("brightness");
    CHECK_FALSE(mm::platform::gpioRead(12));          // off by brightness, `on` still true

    drivers.brightness = 1;
    drivers.onControlChanged("brightness");
    CHECK(mm::platform::gpioRead(12));                // the smallest non-zero brightness is on

    drivers.on = false;
    drivers.onControlChanged("on");
    CHECK_FALSE(mm::platform::gpioRead(12));          // and `on` still opens it whatever brightness says
    mm::platform::clearTestGpioLevel();
}

// A typo in the relay list must not leave the previous relays closed. Reporting the parse error and returning looked right, but the pins from the last VALID list stayed asserted on GPIOs no control named any more: the strip kept its power through a brightness of zero, and nothing in the UI said why. An unparseable list means no relays, which is the same state as an empty one.
TEST_CASE("a typo in the relay list releases the relays it used to hold") {
    mm::platform::clearTestGpioLevel();
    mm::Drivers drivers;
    std::strcpy(drivers.relayPins, "12,13");
    drivers.on = true;
    drivers.brightness = 100;
    drivers.onControlChanged("relayPins");
    CHECK(mm::platform::gpioRead(12));
    CHECK(mm::platform::gpioRead(13));

    // Mid-edit the list is briefly nonsense, which is the normal way a user types one.
    std::strcpy(drivers.relayPins, "12,,x");
    drivers.onControlChanged("relayPins");
    CHECK_FALSE(mm::platform::gpioRead(12));
    CHECK_FALSE(mm::platform::gpioRead(13));
    REQUIRE(drivers.status() != nullptr);
    CHECK(std::string(drivers.status()) == "invalid pin list");

    // And the driver has forgotten them, so a later brightness change does not resurrect either pin.
    drivers.brightness = 200;
    drivers.onControlChanged("brightness");
    CHECK_FALSE(mm::platform::gpioRead(12));
    CHECK_FALSE(mm::platform::gpioRead(13));

    // A corrected list takes effect normally, and the card stops naming the typo.
    std::strcpy(drivers.relayPins, "13");
    drivers.onControlChanged("relayPins");
    CHECK(mm::platform::gpioRead(13));
    CHECK_FALSE(mm::platform::gpioRead(12));
    CHECK((drivers.status() == nullptr || std::string(drivers.status()).empty()));
    mm::platform::clearTestGpioLevel();
}

// Removing the MIDDLE of three is what separates a real teardown from a truncation, since removeChild compacts in place.
TEST_CASE("Drivers: three sibling NetworkSendDrivers each own their buffer, and removing the middle one leaves the others driving") {
    mm::Buffer source;
    REQUIRE(source.allocate(64, 3));

    mm::Drivers drivers;
    mm::NetworkSendDriver first, middle, last;
    drivers.addChild(&first);
    drivers.addChild(&middle);
    drivers.addChild(&last);
    for (auto* d : {&first, &middle, &last}) {
        d->setSourceBuffer(&source);
        mm::test::rebuildFromPreset(d->correctionForTest(), 255, mm::test::PresetOrder::RGB);
        d->applyState();
    }

    // Three buffers, not one shared between them: a container that handed out one would show three equal pointers here.
    REQUIRE(first.correctedBuffer().data() != nullptr);
    REQUIRE(middle.correctedBuffer().data() != nullptr);
    REQUIRE(last.correctedBuffer().data() != nullptr);
    CHECK(first.correctedBuffer().data() != middle.correctedBuffer().data());
    CHECK(middle.correctedBuffer().data() != last.correctedBuffer().data());
    CHECK(first.correctedBuffer().data() != last.correctedBuffer().data());
    CHECK(drivers.childCount() == 3);

    const uint8_t* firstBefore = first.correctedBuffer().data();
    const uint8_t* lastBefore  = last.correctedBuffer().data();

    REQUIRE(drivers.removeChild(&middle));
    middle.release();

    // The survivors keep the buffers they were already sending from: the removal freed the one that went and touched neither of the others.
    CHECK(drivers.childCount() == 2);
    CHECK(first.correctedBuffer().data() == firstBefore);
    CHECK(last.correctedBuffer().data() == lastBefore);
    CHECK(first.correctedBuffer().count() == 64);
    CHECK(last.correctedBuffer().count() == 64);

    // The container's view is what a truncation breaks: reading the objects alone passes against a dropped LAST child.
    CHECK(drivers.child(0) == &first);
    CHECK(drivers.child(1) == &last);
    for (uint8_t i = 0; i < drivers.childCount(); i++) CHECK(drivers.child(i) != &middle);
}

// A user sees power, brightness and the look; the wiring an installer sets once waits for expert mode, and the firmware's own bench self-test for developer mode.
TEST_CASE("Drivers: relay pins and double buffering are expert settings, and the loopback test a developer one") {
    auto modeOf = [](mm::MoonModule& m, const char* name) {
        const int i = mm::test::controlIndex(m, name);
        REQUIRE(i >= 0);
        return m.controls()[static_cast<uint8_t>(i)].minMode;
    };
    mm::Drivers drivers;
    drivers.defineControls();
    CHECK(modeOf(drivers, "relayPins") == mm::kModeExpert);
    mm::ParallelLedDriver parallel;
    parallel.defineControls();
    CHECK(modeOf(parallel, "doubleBuffer") == mm::kModeExpert);
    for (const char* name : {"loopbackTest", "loopbackTxPin", "loopbackRxPin", "loopbackStrand", "loopbackIntrusive"})
        CHECK_MESSAGE(modeOf(parallel, name) == mm::kModeDeveloper, name);
    mm::RmtLedDriver rmt;
    rmt.defineControls();
    for (const char* name : {"loopbackTest", "loopbackTxPin", "loopbackRxPin", "loopbackFrame"})
        CHECK_MESSAGE(modeOf(rmt, name) == mm::kModeDeveloper, name);
}
