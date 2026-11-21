/// @module Hub75PiOutput
/// @also Hub75Driver, Correction

/// What the Raspberry Pi outputs share, pinned on the Pi PIO output, on a desktop, no panels attached; a case only one output has lives in its own file.

#include "doctest.h"
#include "light/drivers/Correction.h"
#include "light/drivers/Drivers.h"
#include "unit/core/conditional_controls.h"
#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/PanelsLayout.h"

#include <memory>
#include <string>
#include "hub75_pi_bench.h"

// The status of a 16x32 panel opened at the default 6 bits and healthy.
template <class S>
void checkPlainPanelStatus(mm::Hub75Driver& driver) {
    CHECK(driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(driver.status()) == std::string("16x32, 6-bit, ") + S::label);
}

TEST_CASE_TEMPLATE("Hub75Driver on a Pi output starts its one bitDepth at 6 and hands it over as the engine's depth", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(16, 32);
    CHECK(r->driver.bitDepth == 6);
    r->driver.bitDepth = 4;
    r->open();
    CHECK(S::options().bitDepth == 4);
    CHECK(std::string(r->driver.status()) == std::string("16x32, 4-bit, ") + S::label);
}

TEST_CASE_TEMPLATE("Hub75PiOutput offers 2 to 8 bits, and a depth outside them is the engine's refusal, in the status", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(16, 32);
    const int i = mm::controlIndex(r->driver, "bitDepth");
    REQUIRE(i >= 0);
    CHECK(r->driver.controls()[static_cast<uint8_t>(i)].min == 2);
    CHECK(r->driver.controls()[static_cast<uint8_t>(i)].max == 8);

    r->driver.bitDepth = 11;   // a config loaded from storage sets values and skips the control change; the driver clamps when the output changes
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()).find("bitDepth must be 2..8") != std::string::npos);
    CHECK(S::width() == 0);

    r->driver.bitDepth = 8;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(S::options().bitDepth == 8);
}

TEST_CASE_TEMPLATE("Hub75PiOutput hands light i to canvas pixel i, row by row", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(16, 32);
    r->open();
    CHECK(std::string(r->driver.status()) == std::string("16x32, 6-bit, ") + S::label);

    r->paint(0, 10, 20, 30);
    r->paint(3 * 16 + 5, 40, 50, 60);   // (5, 3)
    r->driver.tick();

    REQUIRE(S::frameCount() == 1);
    CHECK(S::pixel(0, 0)[0] == 10); CHECK(S::pixel(0, 0)[1] == 20); CHECK(S::pixel(0, 0)[2] == 30);
    CHECK(S::pixel(5, 3)[0] == 40); CHECK(S::pixel(5, 3)[1] == 50); CHECK(S::pixel(5, 3)[2] == 60);
}

TEST_CASE_TEMPLATE("Hub75PiOutput refuses a layout smaller than one panel, in full, and opens nothing", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(4, 2);   // 8 lights, a panel is 16 by 32
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()) == "The layout holds fewer lights than one 16x32 panel.");
    CHECK(S::width() == 0);
}

TEST_CASE_TEMPLATE("Hub75PiOutput takes a layout of the canvas turned on its side as a match, since the layout owns rotation", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = std::make_unique<Rig>(1, 1, 32, 16, uint8_t{1}, false);   // one panel 32 across and 16 down, its rows walked down the columns
    r->driver.panelRows = 32;   // the card's 16x32 panel, turned on its side by the layout
    r->driver.panelCols = 16;
    r->open();
    checkPlainPanelStatus<S>(r->driver);
}

TEST_CASE_TEMPLATE("Hub75PiOutput follows two stacked layouts: two panels' worth of lights make two outputs, and the status says so", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    mm::GridLayout second;   // outlives the rig, whose Layouts holds it
    second.width = 16;
    second.height = 32;
    second.depth = 1;
    auto r = Rig::raster(16, 32);
    r->layouts.addChild(&second);
    r->layouts.applyState();
    r->layer.applyState();
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(r->driver.status()) == std::string("16x64, 6-bit, ") + S::label);
    CHECK(S::options().parallel == 2);
}

TEST_CASE_TEMPLATE("Hub75PiOutput shows as many of a larger layout's lights as the panels hold", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(20, 40);
    r->open();
    r->paint(511, 1, 2, 3);   // the canvas's last pixel, (15, 31)
    r->paint(512, 9, 9, 9);   // one past the canvas
    r->driver.tick();

    REQUIRE(S::frameCount() == 1);
    CHECK(S::pixel(15, 31)[0] == 1); CHECK(S::pixel(15, 31)[2] == 3);
}

TEST_CASE_TEMPLATE("Hub75PiOutput joins a light count other than the canvas's with the walk's panel, so none hides another", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(20, 40);   // a raster wider than the 16x32 panel: another light count, and panel 1 leaves plain rows
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()) ==
          "layout 800 lights, panels 512; "
          "the layout's panel 1 does not run in plain rows of 16: lay the panels out in cabling order, each in plain rows with snake off");
}

TEST_CASE_TEMPLATE("Hub75PiOutput warns of lights past the last whole panel, and judges only the panels it drives", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    mm::GridLayout extra;   // 8 lights after one 16x32 panel in plain rows; outlives the rig, whose Layouts holds it
    extra.width = 4;
    extra.height = 2;
    extra.depth = 1;
    auto r = Rig::raster(16, 32, 520);
    r->layouts.addChild(&extra);
    r->layouts.applyState();
    r->layer.applyState();
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()) == "layout 520 lights, panels 512");
}

TEST_CASE_TEMPLATE("Hub75PiOutput shows a wide profile's red, green and blue, wherever the profile puts them", S, mm::PioSeam) {
    typename S::RecordGuard guard;
    const mm::WideProfile wide;
    mm::showsColorsThrough<S>(wide.roles, 32);
}

TEST_CASE_TEMPLATE("Hub75PiOutput shows a white-first profile's colors, not its white", S, mm::PioSeam) {
    typename S::RecordGuard guard;
    const mm::ChannelRole wrgb[4] = {mm::ChannelRole::White, mm::ChannelRole::Red, mm::ChannelRole::Green, mm::ChannelRole::Blue};
    mm::showsColorsThrough<S>(wrgb, 4);
}

TEST_CASE_TEMPLATE("Hub75PiOutput hands over a layout with fewer lights than its box, the rest dark", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(16, 32, 20);   // a ring or any shape short of its box
    r->open();
    r->paint(19, 7, 8, 9);     // (3, 1), the last light
    r->driver.tick();
    REQUIRE(S::frameCount() == 1);
    CHECK(S::pixel(3, 1)[0] == 7);
    CHECK(S::pixel(4, 1)[0] == 0);
}

TEST_CASE_TEMPLATE("Hub75PiOutput refuses panels another driver drives, and leaves them to it", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto first = Rig::raster(16, 32);
    auto second = Rig::raster(16, 32);
    auto drivers = std::make_unique<mm::Drivers>();   // after the rigs, so it goes first
    drivers->addChild(&first->driver);
    drivers->addChild(&second->driver);
    first->open();
    second->open();
    CHECK(second->driver.severity() == mm::MoonModule::Severity::Error);
    second->driver.tick();
    CHECK(S::frameCount() == 0);
    second->driver.release();
    CHECK(S::width() == 16);   // still the first driver's
}

TEST_CASE_TEMPLATE("Hub75PiOutput's lane canvas counts in the driver's memory while the panels run, and goes with them", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::cabled(2, 1, 16, 32);   // two 16x32 panels in cabling order, one lane: a 1024-light canvas
    r->driver.panelsPerLane = 2;
    r->open();
    REQUIRE(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(r->driver.dynamicBytes() == 2 * 1024 * 3);   // the corrected frame and the canvas

    r->driver.release();
    CHECK(r->driver.dynamicBytes() == 0);

    r->open();
    REQUIRE(r->driver.dynamicBytes() == 2 * 1024 * 3);
    REQUIRE(std::string(mm::platform::hub75BackendLabel(r->driver.selectedBackend())) == S::backendLabel);
    mm::PortGuard port;   // a host with a bus port
    becomeMachine(r->driver, mm::platform::Machine::Other);   // the bus output takes over and the Pi output closes, canvas and all
    CHECK(r->driver.dynamicBytes() < 1024 * 3 * 2);
}

TEST_CASE_TEMPLATE("Hub75PiOutput's memory readout is the frame, the canvas and the light scratch while the panels run, and 0 after a release, for a wiring of 32 channels", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::cabled(2, 1, 16, 32);   // two 16x32 panels in one lane: a 1024-light canvas
    r->driver.panelsPerLane = 2;
    r->open();
    REQUIRE(r->driver.severity() == mm::MoonModule::Severity::Status);
    const mm::WideProfile wide;
    r->driver.correctionForTest().rebuild(255, wide.roles, 32);
    r->driver.onCorrectionChanged();
    CHECK(r->driver.dynamicBytes() == 2 * 1024 * 3 + 32);   // the frame, the canvas and one 32-channel light
    r->driver.prepare();   // a prepare keeps the scratch, the canvas and the readout
    CHECK(r->driver.dynamicBytes() == 2 * 1024 * 3 + 32);
    r->driver.release();
    CHECK(r->driver.dynamicBytes() == 0);
}

TEST_CASE_TEMPLATE("Hub75PiOutput closes the panels on release", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(16, 32);
    r->open();
    REQUIRE(S::width() == 16);
    r->driver.release();
    CHECK(S::width() == 0);
}

// A 16x32 raster on a Pi output, opened.
template <class S>
std::unique_ptr<mm::PiRig<S>> openedRaster() {
    auto r = mm::PiRig<S>::raster(16, 32);
    r->open();
    return r;
}

TEST_CASE_TEMPLATE("Hub75PiOutput shows a stall that is already there at the first second after a reopen", S, mm::PioSeam) {
    typename S::RecordGuard guard;
    auto r = openedRaster<S>();
    mm::platform::setTestHub75PioStalled(true);
    r->driver.tick1s();
    REQUIRE(std::string(r->driver.status()).find("the PIO is stalled") != std::string::npos);

    r->driver.prepare();   // the seam is still stalled
    r->driver.tick1s();
    CHECK(std::string(r->driver.status()).find("the PIO is stalled") != std::string::npos);
}

TEST_CASE_TEMPLATE("Hub75PiOutput counts a reopened panel's failed submits from that open", S, mm::PioSeam) {
    typename S::RecordGuard guard;
    auto r = openedRaster<S>();
    mm::platform::setTestHub75PioSubmitErrors(3);
    r->driver.tick1s();
    REQUIRE(r->driver.severity() == mm::MoonModule::Severity::Warning);

    mm::platform::setTestHub75PioSubmitErrors(0);   // the next open's seam counts from 0
    r->driver.prepare();
    r->driver.tick1s();
    checkPlainPanelStatus<S>(r->driver);
}

TEST_CASE_TEMPLATE("Hub75PiOutput places lights by the panels as opened, whatever a control reads before the reopen", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(32, 32);   // two 16 by 32 panels in one lane
    r->driver.panelsPerLane = 2;
    r->open();
    REQUIRE(S::width() == 32);
    r->driver.panelsPerLane = 1;   // a control moved before any prepare
    r->paint(512, 200, 0, 0);      // panel 1, first light
    r->paint(528, 0, 200, 0);      // panel 1, first light of its second row
    r->driver.tick();
    CHECK(S::pixel(16, 0)[0] == 200);   // still the second place of the lane
    CHECK(S::pixel(16, 1)[1] == 200);
}

// Lights run panel by panel in cabling order, so a Pi output puts each where its panel sits on the lane canvas; the expected cells are the placement rule by hand.

TEST_CASE_TEMPLATE("Hub75Driver on a Pi output puts the second panel of a lane to the right of the first", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(32, 32);   // 1024 lights: two 16 by 32 panels
    r->driver.panelsPerLane = 2;
    r->paint(0, 200, 0, 0);       // panel 0, first light
    r->paint(15, 0, 200, 0);      // panel 0, last light of its first row
    r->paint(16, 0, 0, 200);      // panel 0, first light of its second row
    r->paint(512, 100, 100, 100); // panel 1, first light
    r->paint(1023, 200, 200, 0);  // panel 1, last light
    r->open();
    r->driver.tick();
    REQUIRE(S::width() == 32);
    CHECK(S::pixel(0, 0)[0] == 200);
    CHECK(S::pixel(15, 0)[1] == 200);
    CHECK(S::pixel(0, 1)[2] == 200);
    CHECK(S::pixel(16, 0)[0] == 100);
    CHECK(S::pixel(31, 31)[0] == 200);
    CHECK(S::pixel(31, 31)[1] == 200);
}

TEST_CASE_TEMPLATE("Hub75Driver on a Pi output stacks the lanes: six panels, two to a lane on three lanes, make a 32 by 96 canvas", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(32, 96);   // 3072 lights: six 16 by 32 panels
    r->driver.panelsPerLane = 2;
    r->paint(1024, 200, 0, 0);     // panel 2: place 0 of lane 1
    r->paint(2560, 0, 200, 0);     // panel 5: place 1 of lane 2
    r->paint(3071, 0, 0, 200);     // the last light of the last panel
    r->open();
    r->driver.tick();
    REQUIRE(S::width() == 32);
    REQUIRE(S::height() == 96);
    CHECK(S::pixel(0, 32)[0] == 200);
    CHECK(S::pixel(16, 64)[1] == 200);
    CHECK(S::pixel(31, 95)[2] == 200);
}

TEST_CASE_TEMPLATE("Hub75Driver on a Pi output leaves a lane of one panel as it was: light i is canvas pixel i", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(16, 96);   // three 16 by 32 panels, one to a lane
    r->driver.panelsPerLane = 1;
    r->paint(514, 200, 0, 0);
    r->open();
    r->driver.tick();
    CHECK(S::pixel(2, 32)[0] == 200);   // light 514 is panel 1 (row 32 of the canvas), column 2
}

// The panels follow the layout; Hub75Fit's cases pin the geometry text, these that the output shows it whole and leaves the panels closed.

TEST_CASE_TEMPLATE("Hub75PiOutput derives the parallel lanes from the layout, and the card has no such control", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::cabled(2, 3, 16, 32);   // six 16 by 32 panels
    r->driver.panelsPerLane = 2;
    CHECK(mm::controlIndex(r->driver, "parallel") < 0);
    r->open();
    CHECK(S::options().parallel == 3);
    CHECK(std::string(r->driver.status()) == std::string("32x96, 6-bit, ") + S::label);
}

TEST_CASE_TEMPLATE("Hub75PiOutput names the fix in full for panels that do not cable into the lane, and opens nothing", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(112, 32);   // seven 16 by 32 panels
    r->driver.panelsPerLane = 2;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    const std::string s = r->driver.status();
    CHECK(s.rfind("7 panels cannot be cabled in lanes of 2.", 0) == 0);
    CHECK(s.find("Cable all 7 (rates computed from the pixel clock): 1 lane of 7") != std::string::npos);
    CHECK(s.find("Keep lanes of 2 (rates computed from the pixel clock): use 6 panels, 1 spare, as 3 lanes") != std::string::npos);
    CHECK(s.size() > 64);   // past 64 bytes, so the status holds a long message whole
    CHECK(S::width() == 0);
}

TEST_CASE_TEMPLATE("Hub75PiOutput says how many lanes the board drives when the layout needs more", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(64, 32);   // four 16 by 32 panels, one to a lane: four outputs
    r->driver.panelsPerLane = 1;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()).rfind(std::string("4 panels in lanes of 1 need 4 lanes, but the Pi ") + S::label + " board drives at most 3.", 0) == 0);
    CHECK(S::width() == 0);
}

// A raster across panels in series matches every count, so only the walk catches it: the wall shows bands.
TEST_CASE_TEMPLATE("Hub75PiOutput warns when a raster layout feeds panels in series, naming the panel and the fix, and the panels still run", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::raster(256, 64);   // one raster across two 128 by 64 panels
    r->driver.panelRows = 64;
    r->driver.panelCols = 128;
    r->driver.panelsPerLane = 2;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()) ==
          "the layout's panel 1 does not run in plain rows of 128: lay the panels out in cabling order, each in plain rows with snake off");
    CHECK(S::width() == 256);
}

TEST_CASE_TEMPLATE("Hub75PiOutput warns when a Panels layout snakes inside its panels, the Panels layout's default", S, mm::PioSeam) {
    using Rig = mm::PiRig<S>;
    typename S::RecordGuard guard;
    auto r = Rig::cabled(2, 1, 16, 32, true);
    r->driver.panelsPerLane = 2;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()).rfind("the layout's panel 1 does not run in plain rows of 16", 0) == 0);
}
