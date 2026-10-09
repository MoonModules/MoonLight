/// @module Drivers

#include "doctest.h"
#include "light/drivers/Drivers.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "light/drivers/FixtureProfilesModule.h"   // the non-deletable boot-wired fixture-profile library
#include "light/drivers/NetworkSendDriver.h"     // a real driver, for the sibling-instance cases
#include "light/drivers/ParallelLedDriver.h"     // its installer and bench controls, for the mode case
#include "light/drivers/RmtLedDriver.h"
#include "light/drivers/PreviewDriver.h"     // the one driver safe mode keeps
#include "correction_presets.h"                  // mm::test::rebuildFromPreset
#include "../core/conditional_controls.h"   // mm::test::setControlValue
#include "platform/platform.h"                 // gpioRead: the desktop reads back what gpioWrite put there

#include <cstring>
#include <filesystem>
#include <string>

// Drivers::tick() honors the per-child `enabled` check; a stub driver counts its loop calls, and toggling `enabled` must flip whether the count advances.

namespace {

// Minimal DriverBase stub: counts loop calls. Ignores the source buffer it would normally consume, this test cares only about whether tick() runs.
class CountingDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    void tick() MM_NONBLOCKING override { loopCalls++; }
    int loopCalls = 0;
};

} // namespace

// A minimal driver that owns its Correction (filled by the container via rebuildCorrection()) and defines the correction controls, so a test can read the resulting LUT.
class CorrectionCapturingDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    void tick() MM_NONBLOCKING override {}
    void defineDriverControls() override { defineCorrectionControls(); }
};

// Counts prepare() (the structural rebuild that reinits the output peripheral) against onCorrectionChanged() (the light refresh), to prove a refresh is correction-only.
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

// A driver that re-lays its frame on a correction change, as the parallel and RMT drivers do, counting the times it does.
class FrameLayingDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    void tick() MM_NONBLOCKING override {}
    void defineDriverControls() override { defineCorrectionControls(); }
    void onCorrectionChanged() override { if (outChannelsChanged()) layouts++; }
    void setOutChannels(uint8_t ch) { correction_.outChannels = ch; }
    int layouts = 0;
};

// Regression: every brightness change made the LightCrafter's parallel driver drain its DMA transfer and re-parse its lanes, which a desk fader's stream turned into freezes.
TEST_CASE("a brightness change re-bakes the correction without re-laying the driver's frame; a channel-count change does") {
    mm::Drivers drivers;
    FrameLayingDriver drv;
    drivers.addChild(&drv);
    drivers.on = true;
    drivers.brightness = 200;
    drv.defineControls();
    drivers.setup();
    const int afterSetup = drv.layouts;
    for (const uint8_t b : {10, 90, 255, 0, 61}) {
        drivers.brightness = b;
        drivers.onControlChanged("brightness");
    }
    CHECK(drv.layouts == afterSetup);
    drv.setOutChannels(static_cast<uint8_t>(drv.correction().outChannels + 1));   // as a white channel being added would
    drivers.onControlChanged("brightness");
    CHECK(drv.layouts == afterSetup + 1);
    drivers.removeChild(&drv);
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

// The `on` control is master power: on=false scales the correction LUT to zero while preserving the brightness value, so on=true restores the exact level, with no pipeline realloc.
TEST_CASE("Drivers::on gates the correction LUT without clobbering brightness") {
    mm::Drivers drivers;
    CorrectionCapturingDriver drv;
    drivers.addChild(&drv);
    drv.defineControls();
    // Linear keeps the expected numbers literal; a perceptual curve would turn each into a lookup.
    mm::test::setControlValue<uint8_t>(drv, "curve", static_cast<uint8_t>(mm::Correction::Curve::Linear));
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

// A per-driver localBrightness change re-scales that driver's correction LUT (global × local), as a global brightness change does.
TEST_CASE("Drivers: a localBrightness change re-scales the driver's correction LUT") {
    mm::Drivers drivers;
    CorrectionCapturingDriver drv;
    drivers.addChild(&drv);
    drivers.brightness = 200;
    drivers.on = true;
    drv.defineControls();                           // bind the correction controls (localBrightness etc.)
    // LINEAR, so the arithmetic below reads as the multiplication it is testing. What this pins is that BOTH sliders reach the LUT, which a perceptual curve would leave true but express as table lookups nobody can check by eye. The curve has its own tests.
    mm::test::setControlValue<uint8_t>(drv, "curve", static_cast<uint8_t>(mm::Correction::Curve::Linear));
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

// The "+ add" picker under Drivers offers only drivers, so acceptsChildRoles returns "driver" alone; the boot-wired FixtureProfiles library bypasses it.
TEST_CASE("Drivers accepts only driver-role children in the add picker") {
    mm::Drivers drivers;
    CHECK(std::strcmp(drivers.acceptsChildRoles(), "driver") == 0);
}

// The boot-wired fixture-profile library is a permanent singleton: a deleted one could never be re-added, and every driver resolves its profile through it.
TEST_CASE("FixtureProfiles library is a non-deletable singleton") {
    mm::FixtureProfilesModule lib;
    CHECK_FALSE(lib.userEditable());
}

// The LivePalettes seam is published by prepare(), which only a scheduler-mounted module runs, so a throwaway probe Drivers leaves it as found.
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

// A strip at brightness 0 still draws idle current through a closed relay, so the relay opens at brightness 0 and closes when brightness returns.
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

// An unparseable relay list means no relays, the same as an empty one, so pins from the last valid list never stay asserted on GPIOs no control names.
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

// A scripted palette has no stored stops, so its color comes from the entries it computes now, not from the built-in its index would wrap onto.
TEST_CASE("Drivers gives a scripted palette's color from its live entries") {
    mm::Drivers drivers;
    const mm::Palette saved = *mm::Palettes::active();
    mm::Palette blue;
    for (auto& e : blue.entry) e = mm::RGB{0, 0, 255};
    mm::Palettes::setActiveDirect(blue);            // what a running palette script wrote
    uint16_t hue = 0, sat = 0;
    drivers.paletteHueSat(mm::palettes::kCount, hue, sat);   // the first scripted index
    CHECK(hue == 240);
    CHECK(sat == 255);
    uint16_t builtinHue = 0, builtinSat = 0;
    mm::Palettes::representativeHueSat(0, builtinHue, builtinSat);
    drivers.paletteHueSat(0, hue, sat);             // a built-in still reads its own stops
    CHECK(hue == builtinHue);
    CHECK(sat == builtinSat);
    mm::Palettes::setActiveDirect(saved);
}

namespace {
/// The palette picker's option count, which grows by one per scripted palette found.
uint8_t paletteOptions(mm::Drivers& d) {
    auto& cs = d.controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (std::strcmp(cs[i].name, "palette") == 0) return static_cast<uint8_t>(cs[i].max);
    return 0;
}
}  // namespace

// Regression: every brightness change listed both script folders to rebuild the palette picker, 100 ms on an S3, so a desk fader froze the LEDs.
TEST_CASE("a brightness change does not rescan the palette scripts; a palette file written through the API does") {
    char root[64];
    std::snprintf(root, sizeof(root), "/tmp/mm_palette_scan_%u", static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(root);
    struct RootAfter { std::string prev = mm::platform::fsRootPath(); ~RootAfter() { mm::platform::fsSetRoot(prev.c_str()); } } rootAfter;
    mm::platform::fsSetRoot(root);
    mm::platform::fsMkdir("/moonlive");
    mm::Drivers drivers;
    drivers.defineControls();
    drivers.setup();                                 // the scan that counts, once the filesystem is up
    const uint8_t before = paletteOptions(drivers);

    const char script[] = "void tick() {}";
    REQUIRE(mm::platform::fsWriteAtomic("/moonlive/mine.mlp", script, sizeof(script) - 1));
    drivers.brightness = 90;
    drivers.rebuildControls();                       // what every brightness change does
    CHECK(paletteOptions(drivers) == before);        // no folder listing

    drivers.onFileChanged("/moonlive/mine.mlp");     // what a write through the API announces
    CHECK(paletteOptions(drivers) == before + 1);
    CHECK(mm::Drivers::touchesFolder("/", "/moonlive"));             // a restore of everything
    CHECK_FALSE(mm::Drivers::touchesFolder("/moonlivex/a.mlp", "/moonlive"));
    std::filesystem::remove_all(root);
}

// LEDs without data hold their last frame and its current, so safe mode sends black first, and holds the drivers only when the boots still fail.
TEST_CASE("safe mode darkens the lights, and holds the output drivers after four failed boots") {
    struct Record { ~Record() { mm::platform::setTestBootRecord({}); } } guard;
    mm::platform::setTestBootRecord({0, 2});
    {
        mm::Drivers drivers;
        CountingDriver output;
        drivers.addChild(&output);
        drivers.brightness = 200;
        drivers.setup();
        CHECK(drivers.effectiveBrightness() == 0);   // dark
        CHECK(output.enabled());                      // still sending, black
        drivers.removeChild(&output);
    }
    mm::platform::setTestBootRecord({0, 4});
    mm::Drivers drivers;
    CountingDriver output;
    mm::PreviewDriver preview;
    drivers.addChild(&output);
    drivers.addChild(&preview);
    CHECK_FALSE(output.enabled());
    CHECK(preview.enabled());
    drivers.removeChild(&preview);
    drivers.removeChild(&output);
}

// A device boot raises the lights over five seconds from the first frame, so a supply that cannot hold the level fails early, where the boot record counts it.
TEST_CASE("the lights rise to their brightness after a device boot, and a host process starts at it") {
    struct Record { ~Record() { mm::platform::setTestBootRecord({}); mm::platform::setTestNowMs(0); } } guard;
    mm::platform::setTestNowMs(1000);
    {
        mm::Drivers plain;
        plain.brightness = 200;
        plain.setup();
        CHECK(plain.effectiveBrightness() == 200);   // a host process, as the desktop app and every test
    }
    mm::platform::setTestBootRecord({0, 0, true});
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 1; grid.height = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(5);   // RGBW and one motion slot
    layouts.applyState();
    layer.applyState();
    mm::Drivers drivers;
    drivers.setLayer(&layer);
    drivers.brightness = 200;
    drivers.setup();
    drivers.tick20ms();
    CHECK(drivers.effectiveBrightness() == 0);   // dark until a light is lit
    REQUIRE(layer.buffer().data());
    layer.buffer().data()[mm::FixtureChannels::kMotionBase] = 255;   // an aim alone emits nothing, so the rise waits
    drivers.tick20ms();
    mm::platform::setTestNowMs(1000 + mm::Drivers::kSoftStartMs / 2);
    CHECK(drivers.effectiveBrightness() == 0);
    mm::platform::setTestNowMs(1000);
    layer.buffer().data()[0] = 255;
    drivers.tick20ms();   // the first lit frame starts the rise
    CHECK(drivers.effectiveBrightness() == 0);
    mm::platform::setTestNowMs(1000 + mm::Drivers::kSoftStartMs / 2);
    CHECK(drivers.effectiveBrightness() == 100);
    mm::platform::setTestNowMs(1000 + mm::Drivers::kSoftStartMs);
    drivers.tick20ms();
    CHECK(drivers.effectiveBrightness() == 200);
    layer.buffer().data()[0] = 0;
    mm::Drivers dark;   // a show that stays dark rises once the boot counts as good, and stops scanning for a lit frame
    dark.setLayer(&layer);
    dark.brightness = 200;
    dark.setup();
    mm::platform::setTestNowMs(mm::platform::kBootStableMs);
    dark.tick20ms();
    mm::platform::setTestNowMs(mm::platform::kBootStableMs + mm::Drivers::kSoftStartMs / 2);
    CHECK(dark.effectiveBrightness() == 100);
    layouts.removeChild(&grid);
}
