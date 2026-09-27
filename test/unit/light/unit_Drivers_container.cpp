/// @module Drivers

#include "doctest.h"
#include "light/drivers/Drivers.h"
#include "light/drivers/LightPresetsModule.h"   // the non-deletable boot-wired preset library
#include "../core/conditional_controls.h"   // mm::test::setControlValue
#include "platform/platform.h"                 // gpioRead: the desktop reads back what gpioWrite put there

#include <cstring>

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

// A minimal driver so a test can read the resulting LUT. Each driver owns its Correction (DriverBase::correction_); the container fills it via rebuildCorrection(). Defines the correction controls (localBrightness / preset / whiteMode) so a test can drive them.
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

// Regression (the preset-edit LED-blank bug): editing a live light preset blanked the strip for ~½s, even on drivers NOT using that preset. Cause: the list-mutation handler re-ran a whole-tree prepareTree(), and a physical driver's prepare() reinits its output peripheral (an RMT channel teardown → dark for a tick). A preset edit changes correction DATA, not pipeline STRUCTURE, so the fix routes it through rebuildAllCorrections(), the tier-1 correction refresh, which must re-resolve each driver's correction WITHOUT calling its prepare(). This pins that split so the blank can't return: rebuildAllCorrections() bumps the correction path, never prepare().
TEST_CASE("Drivers::rebuildAllCorrections re-resolves corrections without re-preparing drivers") {
    mm::Drivers drivers;
    RebuildTrackingDriver drv;
    drivers.addChild(&drv);
    drivers.on = true;
    drivers.brightness = 200;
    drv.defineControls();
    drivers.setup();                     // setup() seeds via passBufferToDrivers → rebuildCorrection
    const int baselineCorrection = drv.correctionCalls;   // whatever setup did — we measure the delta
    const int baselinePrepare = drv.prepareCalls;

    // The correction-only refresh the list-mutation handler now calls instead of prepareTree().
    drivers.rebuildAllCorrections();

    CHECK(drv.correctionCalls > baselineCorrection);   // correction WAS re-resolved (edit reaches output)
    CHECK(drv.prepareCalls == baselinePrepare);        // but prepare() was NOT called — no peripheral reinit, no blank
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
    CHECK(drv.correctionForTest().briLut[0][255] == 200);   // (255 * 200) / 255 == 200

    // Turn off → LUT scales to black, but the brightness value is untouched.
    drivers.on = false;
    drivers.onControlChanged("on");
    CHECK(drivers.brightness == 200);            // value preserved
    CHECK(drivers.effectiveBrightness() == 0);
    CHECK(drv.correctionForTest().briLut[0][255] == 0);     // output black

    // Turn back on → the exact level returns, no stored-value juggling.
    drivers.on = true;
    drivers.onControlChanged("on");
    CHECK(drv.correctionForTest().briLut[0][255] == 200);
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
    CHECK(drv.correctionForTest().briLut[0][255] == 200);   // global 200 × local 255/255 = 200

    // Halve the driver's LOCAL brightness, its own control change must re-bake the LUT to global × local = 200 × 128/255 ≈ 100. This is the path the bug missed.
    mm::test::setControlValue<uint8_t>(drv, "localBrightness", 128);
    drv.onControlChanged("localBrightness");
    CHECK(drv.correctionForTest().briLut[0][255] == 100);   // (200 * 128) / 255 == 100

    // And the global slider still composes on top: raising global to 255 with local 128 → 128.
    drivers.brightness = 255;
    drivers.onControlChanged("brightness");
    CHECK(drv.correctionForTest().briLut[0][255] == 128);   // (255 * 128) / 255 == 128
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

// The "+ add" picker under Drivers must offer ONLY drivers, not every generic system module, else the 6 drivers are buried under ~18 generics (Devices, Filesystem, …). acceptsChildRoles drives that picker, so it returns "driver" alone. The one non-driver child (the boot-wired LightPresets library) is added directly at boot, bypassing this check, and is non-deletable, so it needs no "generic" here. Pins the filter the product owner asked for.
TEST_CASE("Drivers accepts only driver-role children in the add picker") {
    mm::Drivers drivers;
    CHECK(std::strcmp(drivers.acceptsChildRoles(), "driver") == 0);
}

// The boot-wired light-preset library is a permanent singleton: not user-deletable (Drivers accepts only `driver`, so a deleted library could never be re-added, and every driver resolves its preset through it). Mirrors the boot-wired PreviewDriver's userEditable(false).
TEST_CASE("LightPresets library is a non-deletable singleton") {
    mm::LightPresetsModule lib;
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

    // And the driver has forgotten them, so a later brightness change does not resurrect either pin.
    drivers.brightness = 200;
    drivers.onControlChanged("brightness");
    CHECK_FALSE(mm::platform::gpioRead(12));
    CHECK_FALSE(mm::platform::gpioRead(13));

    // A corrected list takes effect normally.
    std::strcpy(drivers.relayPins, "13");
    drivers.onControlChanged("relayPins");
    CHECK(mm::platform::gpioRead(13));
    CHECK_FALSE(mm::platform::gpioRead(12));
    mm::platform::clearTestGpioLevel();
}
