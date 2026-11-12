/// @module HdmiDriver
/// @also Drivers, Correction

/// The HDMI driver's frames and status, pinned without a bridge: the desktop platform records what the driver handed over (platform.h § HDMI). The bench then only has to confirm a monitor shows it.

#include "doctest.h"
#include "light/drivers/HdmiDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "wall_fixture.h"
#include "light/layers/Buffer.h"
#include "light/layouts/PanelsLayout.h"
#include <cstring>
#include <memory>
#include <string>

namespace {

// The stand-in and virtual time are process-global, so a guard restores both however a case leaves.
struct HdmiSeamGuard {
    HdmiSeamGuard() { mm::platform::hdmiTestClear(); }
    ~HdmiSeamGuard() { mm::platform::hdmiTestClear(); mm::platform::setTestNowMs(0); }
};

// A 4x2 wall with its source buffer and driver, the correction on RGB, as every case starts.
struct Bench {
    HdmiSeamGuard seam;
    mm::Buffer source;
    mm::HdmiDriver driver;
    mm::test::Wall wall{4, 2};
    Bench() { mm::test::wire(driver, source, wall, 8); }
};

}  // namespace

TEST_CASE("HdmiDriver offers the platform's modes and starts in 1280x720 at 60 Hz") {
    Bench b;
    b.driver.prepare();
    CHECK(mm::platform::hdmiTestOpenMode() == 2);
    const mm::platform::HdmiMode* modes = nullptr;
    REQUIRE(mm::platform::hdmiModes(&modes) > 2);
    CHECK(modes[2].width == 1280);
    CHECK(modes[2].height == 720);
}

TEST_CASE("HdmiDriver hands over one pixel per light at the layer's size, keeping its aspect by default") {
    Bench b;
    uint8_t* p = b.source.data() + 5 * 3;
    p[0] = 10; p[1] = 20; p[2] = 30;
    b.driver.prepare();
    mm::platform::setTestNowMs(1000);
    b.driver.tick();
    REQUIRE(mm::platform::hdmiTestFrameCount() == 1);
    CHECK(mm::platform::hdmiTestFrameWidth() == 4);
    CHECK(mm::platform::hdmiTestFrameHeight() == 2);
    CHECK(mm::platform::hdmiTestFrameFit());
    const uint8_t* f = mm::platform::hdmiTestFrameData();
    REQUIRE(f != nullptr);
    CHECK(f[5 * 3 + 0] == 10);
    CHECK(f[5 * 3 + 1] == 20);
    CHECK(f[5 * 3 + 2] == 30);
}

namespace {

// A driver on any layout's face, wired as production wires it; build() applies the layout, so a case can shape it, or refuse memory, first.
template <class LayoutT>
struct LayoutWall {
    HdmiSeamGuard seam;
    mm::Buffer source;
    mm::HdmiDriver driver;
    mm::test::LayoutFace<LayoutT> face;
    uint32_t nowMs = 1000;   ///< each frame a second after the last, past the fps ceiling
    void build() {
        face.build();
        mm::test::wire(driver, source, face.layer, face.layout.lightCount());
    }
    // Light `i` painted (10, 20, 30), one frame shown: the screen pixel index it lands on, or -1 when nowhere.
    int pixelOf(mm::nrOfLightsType i) {
        uint8_t* p = source.data() + static_cast<size_t>(i) * 3;
        p[0] = 10; p[1] = 20; p[2] = 30;
        driver.prepare();
        mm::platform::setTestNowMs(nowMs += 1000);
        driver.tick();
        const uint8_t* f = mm::platform::hdmiTestFrameData();
        if (!f) return -1;
        const int pixels = mm::platform::hdmiTestFrameWidth() * mm::platform::hdmiTestFrameHeight();
        for (int c = 0; c < pixels; c++)
            if (f[c * 3] == 10 && f[c * 3 + 1] == 20 && f[c * 3 + 2] == 30) return c;
        return -1;
    }
};

// `across` panels of `w` by `h` in a row, snaked inside each when `snake`.
std::unique_ptr<LayoutWall<mm::PanelsLayout>> panelWall(uint16_t across, uint16_t w, uint16_t h, bool snake) {
    auto wall = std::make_unique<LayoutWall<mm::PanelsLayout>>();
    mm::test::shapePanels(wall->face.layout, across, w, h, snake);
    wall->build();
    return wall;
}

}  // namespace

// A chained panel wall's lights run panel by panel; the screen shows them where the layout puts them, as one picture.
TEST_CASE("HdmiDriver places each light where the layout puts it, so a wall of chained panels shows as one picture") {
    auto w = panelWall(2, 2, 2, false);   // two 2x2 panels side by side: a 4x2 picture
    CHECK(w->pixelOf(4) == 2);   // panel 1's first light: x 2 of the top row, not the screen's fifth pixel
}

TEST_CASE("HdmiDriver places a snaked layout's lights where the layout puts them") {
    auto w = panelWall(1, 4, 2, true);   // one 4x2 panel, its second row running back
    CHECK(w->pixelOf(4) == 7);   // the second row's first light sits at its right end
}

TEST_CASE("HdmiDriver leaves a light placed outside the box off the screen, without touching another pixel") {
    auto w = std::make_unique<LayoutWall<mm::test::LeftOfBox>>();
    w->build();
    CHECK(w->pixelOf(0) == -1);   // at (-1, 1): no pixel
    std::memset(w->source.data(), 0, 3);   // light 0 dark again, so the next frame shows only light 4
    CHECK(w->pixelOf(4) == 3);    // (3, 0) shows its own light, which a wrapped index would have given to light 0
}

TEST_CASE("HdmiDriver takes no placement table, not even for a moment, when the layout's order is the raster") {
    if (sizeof(mm::nrOfLightsType) <= 3) {   // a table no heavier than a frame: the refusal cannot tell them apart
        MESSAGE("skipped: a light index of " << sizeof(mm::nrOfLightsType) << " bytes");
        return;
    }
    mm::test::RefuseAbove refuse(64 * 32 * 3);   // a frame of the face: the table, four bytes a pixel against three, is refused
    auto w = panelWall(1, 64, 32, false);   // one panel walked in plain rows: light i is pixel i
    mm::platform::setTestHdmiLink(mm::platform::HdmiLink::Monitor);
    w->driver.prepare();
    CHECK(w->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(w->pixelOf(70) == 70);
}

TEST_CASE("HdmiDriver holds a placement table only for a layout that needs one") {
    auto raster = panelWall(1, 64, 32, false);
    auto snaked = panelWall(1, 64, 32, true);
    raster->driver.prepare();
    snaked->driver.prepare();
    CHECK(snaked->driver.dynamicBytes() - raster->driver.dynamicBytes() == 64 * 32 * sizeof(mm::nrOfLightsType));
}

TEST_CASE("HdmiDriver without room for a needed table keeps a picture in the lights' own order, and warns why") {
    mm::test::RefuseAbove refuse(64 * 32 * 3);   // a frame of the face: the table, four bytes a pixel against three, is refused
    auto w = panelWall(1, 64, 32, true);
    mm::platform::setTestHdmiLink(mm::platform::HdmiLink::Monitor);
    w->driver.prepare();
    CHECK(w->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(w->driver.status()).find("no memory to place the lights by the layout") != std::string::npos);
    CHECK(w->pixelOf(70) == 70);   // the straight copy: light 70 on pixel 70, where the layout would put it elsewhere
}

// The frame one light of (10, 20, 30) hands over, corrected through a profile of these roles, uncurved.
static std::string frameThrough(const mm::ChannelRole* roles, uint8_t n) {
    auto b = std::make_unique<Bench>();   // on the heap: a driver and its wall outgrow a test's stack frame
    mm::Correction c;
    c.curve = mm::Correction::Curve::Linear;
    c.rebuild(255, roles, n);
    b->driver.correctionForTest() = c;
    b->driver.prepare();
    uint8_t* p = b->source.data();
    p[0] = 10; p[1] = 20; p[2] = 30;
    mm::platform::setTestNowMs(1000);
    b->driver.tick();
    const uint8_t* f = mm::platform::hdmiTestFrameData();
    if (mm::platform::hdmiTestFrameCount() != 1 || !f) return "";
    return std::to_string(f[0]) + "," + std::to_string(f[1]) + "," + std::to_string(f[2]);
}

// A white-first fixture's colors come from where its profile puts them, not its first three channels.
TEST_CASE("HdmiDriver shows a white-first profile's colors, not its white") {
    const mm::ChannelRole roles[4] = {mm::ChannelRole::White, mm::ChannelRole::Red, mm::ChannelRole::Green, mm::ChannelRole::Blue};
    CHECK(frameThrough(roles, 4) == "10,20,30");
}

// A three-channel fixture in another order, the WS2812's GRB, still reaches the screen as red, green, blue.
TEST_CASE("HdmiDriver shows a GRB profile's colors in screen order") {
    const mm::ChannelRole roles[3] = {mm::ChannelRole::Green, mm::ChannelRole::Red, mm::ChannelRole::Blue};
    CHECK(frameThrough(roles, 3) == "10,20,30");
}

TEST_CASE("HdmiDriver stretches when scaling is set to stretch") {
    Bench b;
    b.driver.scaling = mm::HdmiDriver::kScalingStretch;
    b.driver.prepare();
    mm::platform::setTestNowMs(1000);
    b.driver.tick();
    REQUIRE(mm::platform::hdmiTestFrameCount() == 1);
    CHECK_FALSE(mm::platform::hdmiTestFrameFit());
}

TEST_CASE("HdmiDriver holds its frame rate to the fps ceiling") {
    Bench b;
    b.driver.fps = 10;
    b.driver.prepare();
    mm::platform::setTestNowMs(1000);
    b.driver.tick();
    mm::platform::setTestNowMs(1050);   // inside the 100 ms window
    b.driver.tick();
    CHECK(mm::platform::hdmiTestFrameCount() == 1);
    mm::platform::setTestNowMs(1100);
    b.driver.tick();
    CHECK(mm::platform::hdmiTestFrameCount() == 2);
}

TEST_CASE("HdmiDriver says when no monitor is connected, and shows the picture once one is") {
    Bench b;
    b.driver.prepare();
    CHECK(std::string(b.driver.status()).find("no monitor") != std::string::npos);
    mm::platform::setTestHdmiLink(mm::platform::HdmiLink::Monitor);
    b.driver.tick1s();
    CHECK(std::string(b.driver.status()).find("4x2 on 1280x720 60 Hz") != std::string::npos);
    CHECK(b.driver.severity() == mm::MoonModule::Severity::Status);
}

TEST_CASE("HdmiDriver says when the output did not come up, as an error, since no monitor can help that") {
    Bench b;
    b.driver.prepare();
    mm::platform::setTestHdmiLink(mm::platform::HdmiLink::Down);
    b.driver.tick1s();
    CHECK(std::string(b.driver.status()).find("output did not come up") != std::string::npos);
    CHECK(b.driver.severity() == mm::MoonModule::Severity::Error);
}

TEST_CASE("HdmiDriver names the monitor and its preferred size from the EDID, and warns when it does not list the mode") {
    Bench b;
    b.driver.defineControls();   // the readout is a control, which the wiring alone does not declare
    b.driver.prepare();
    mm::platform::setTestHdmiLink(mm::platform::HdmiLink::Monitor);
    mm::edid::Monitor m;
    std::strcpy(m.name, "DELL U2412M");
    m.preferredW = 1920;
    m.preferredH = 1200;
    m.lists = 0b01011;   // 800x600, 1024x768, 1280x800: no 720p
    mm::platform::setTestHdmiMonitor(m);
    b.driver.tick1s();
    const char* readout = nullptr;
    for (uint8_t i = 0; i < b.driver.controls().count(); i++)
        if (std::strcmp(b.driver.controls()[i].name, "monitor") == 0) readout = static_cast<const char*>(b.driver.controls()[i].ptr);
    REQUIRE(readout != nullptr);
    CHECK(std::string(readout) == "DELL U2412M, prefers 1920x1200, does not list 1280x720");
    CHECK(std::string(b.driver.status()).find("4x2 on 1280x720 60 Hz") != std::string::npos);
    CHECK(b.driver.severity() == mm::MoonModule::Severity::Warning);

    m.lists |= 1u << 2;   // the monitor lists 720p after all
    mm::platform::setTestHdmiMonitor(m);
    b.driver.tick1s();
    CHECK(std::string(readout) == "DELL U2412M, prefers 1920x1200");
    CHECK(b.driver.severity() == mm::MoonModule::Severity::Status);
}

TEST_CASE("HdmiDriver keeps the output through a re-prepare in the same mode, and restarts it on a mode change") {
    Bench b;
    b.driver.prepare();
    mm::platform::setTestNowMs(1000);
    b.driver.tick();
    REQUIRE(mm::platform::hdmiTestFrameCount() == 1);
    b.driver.prepare();   // the tree sweep a network change or an upload runs
    CHECK(mm::platform::hdmiTestFrameCount() == 1);   // a closed output keeps no frames, so the output stayed open
    CHECK(mm::platform::hdmiTestOpenMode() == 2);
    b.driver.mode = 1;
    b.driver.prepare();
    CHECK(mm::platform::hdmiTestFrameCount() == 0);
    CHECK(mm::platform::hdmiTestOpenMode() == 1);
}

TEST_CASE("HdmiDriver shows the bridge's synthesizer word against the programmed one, and the sync it measures") {
    Bench b;
    b.driver.defineControls();
    b.driver.prepare();
    mm::platform::setTestHdmiLink(mm::platform::HdmiLink::Monitor);
    mm::platform::setTestHdmiBridge({0x571e, 0x5555, 18001, 741});
    b.driver.tick1s();
    const char* readout = nullptr;
    for (uint8_t i = 0; i < b.driver.controls().count(); i++)
        if (std::strcmp(b.driver.controls()[i].name, "bridge") == 0) readout = static_cast<const char*>(b.driver.controls()[i].ptr);
    REQUIRE(readout != nullptr);
    CHECK(std::string(readout) == "DDS 571e, wants 5555, sync 18001/741");
    mm::platform::setTestHdmiLink(mm::platform::HdmiLink::Down);
    b.driver.tick1s();
    CHECK(std::string(readout).empty());   // nothing read from a bridge that is not set up
}

TEST_CASE("HdmiDriver hides the bridge readout on a platform without a bridge to read back") {
    Bench b;
    b.driver.defineControls();
    bool found = false;
    for (uint8_t i = 0; i < b.driver.controls().count(); i++)
        if (std::strcmp(b.driver.controls()[i].name, "bridge") == 0) { found = true; CHECK(b.driver.controls()[i].hidden == !mm::platform::hasHdmiBridge); }
    CHECK(found);
}

TEST_CASE("HdmiDriver closes the output on release, so the screen goes dark") {
    Bench b;
    b.driver.prepare();
    REQUIRE(mm::platform::hdmiTestOpenMode() >= 0);
    b.driver.release();
    CHECK(mm::platform::hdmiTestOpenMode() == -1);
    CHECK(b.driver.status() == nullptr);   // the base release clears it along with the staging
}

TEST_CASE("HdmiDriver names the mode a display without the chosen size runs instead, as a warning") {
    Bench b;
    b.driver.prepare();
    mm::platform::setTestHdmiLink(mm::platform::HdmiLink::Monitor);
    mm::platform::setTestHdmiActiveMode({1470, 956, 60});   // a MacBook panel, which has no 1280x720
    b.driver.tick1s();
    CHECK(std::string(b.driver.status()) == "4x2 on 1470x956 60 Hz (no 1280x720)");
    CHECK(b.driver.severity() == mm::MoonModule::Severity::Warning);

    mm::platform::setTestHdmiActiveMode({1280, 720, 0});   // the size after all, from a panel that names no rate
    b.driver.tick1s();
    CHECK(std::string(b.driver.status()) == "4x2 on 1280x720");
    CHECK(b.driver.severity() == mm::MoonModule::Severity::Status);
}
