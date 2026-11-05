/// @module Hub75Driver
/// @also Correction

/// Hub75Driver's tick, pinned down to the bit planes without a panel. The desktop's HUB75 port, opened by the test (`setTestHub75` in platform.h), hands the driver one frame as the ESP32 port does, and each case compares it with what the encoder makes of the colors the panel should show.

#include "doctest.h"
#include "light/drivers/Hub75Driver.h"
#include "light/drivers/Hub75Slots.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "platform/platform.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// The port is process-global, so a guard closes it however a case ends.
struct PortGuard {
    PortGuard() { mm::platform::setTestHub75(true); }
    ~PortGuard() { mm::platform::setTestHub75(false); }
};

// 16x32 at the driver's default 1/16 scan, the smallest box that pairs into rows; 6-bit, its default depth.
constexpr uint16_t kWidth = 16;
constexpr uint16_t kHeight = 32;

// A driver on a 16x32 layout whose source holds `lights` lights, wired as production wires it, on the heap since a driver and a layer outgrow a test's stack frame.
struct Rig {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::Buffer source;
    mm::Hub75Driver driver;

    explicit Rig(mm::nrOfLightsType lights) {
        grid.width = kWidth;
        grid.height = kHeight;
        grid.depth = 1;
        layouts.addChild(&grid);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
        REQUIRE(source.allocate(lights, 3));
        driver.setLayer(&layer);
        driver.setSourceBuffer(&source);
    }
    // The owner releases a driver before destroying it, which frees what open() allocated.
    ~Rig() { driver.release(); }

    // Open the port with a linear, full-brightness correction, so a frame's values read straight.
    void open() {
        mm::Correction correction;
        mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);
        driver.correctionForTest() = correction;
        driver.applyState();
        driver.prepare();
        REQUIRE(std::string(driver.status() ? driver.status() : "").find("1/16 scan") != std::string::npos);
    }

    void paint(mm::nrOfLightsType i, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = source.data() + static_cast<size_t>(i) * 3;
        p[0] = r; p[1] = g; p[2] = b;
    }
};

std::unique_ptr<Rig> rig(mm::nrOfLightsType lights = static_cast<mm::nrOfLightsType>(kWidth) * kHeight) {
    return std::make_unique<Rig>(lights);
}

// A 16x32 RGB image, dark but for the one pixel at (x, y).
std::vector<uint8_t> onePixel(uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> rgb(static_cast<size_t>(kWidth) * kHeight * 3, 0);
    uint8_t* p = rgb.data() + (static_cast<size_t>(y) * kWidth + x) * 3;
    p[0] = r; p[1] = g; p[2] = b;
    return rgb;
}

// Where the frame the driver packed first differs from the encoder's frame for `rgb`, or -1 where they match.
long firstDifference(const std::vector<uint8_t>& rgb) {
    size_t bytes = 0;
    const uint8_t* frame = mm::platform::hub75TestFrame(&bytes);
    if (!frame) return -2;
    mm::Hub75Geometry geo;
    geo.width = kWidth;
    geo.height = kHeight;
    geo.scanRate = 16;
    geo.bitDepth = 6;
    std::vector<uint8_t> want(geo.frameBytes(), 0);
    if (bytes != want.size()) return -3;
    mm::hub75Encode(rgb.data(), want.data(), geo);
    for (size_t i = 0; i < bytes; i++)
        if (frame[i] != want[i]) return static_cast<long>(i);
    return -1;
}

// Paint light `i` one color, tick once, and answer where the packed frame first differs from that color alone at (x, y): -1 when it shows there and nowhere else.
long paintAndShow(Rig& r, mm::nrOfLightsType i, uint16_t x, uint16_t y) {
    r.paint(i, 200, 100, 50);
    r.driver.tick();
    return firstDifference(onePixel(x, y, 200, 100, 50));
}

// Wire the fixture with `count` channels in these roles, and tell the driver as the container does when the wiring changes.
void rewire(Rig& r, const mm::ChannelRole* roles, uint8_t count) {
    r.driver.correctionForTest().rebuild(255, roles, count);
    r.driver.onCorrectionChanged();
}

}  // namespace

TEST_CASE("Hub75Driver on a desktop refuses with the cause, as on a chip without the silicon") {
    auto r = rig();
    r->driver.applyState();
    r->driver.prepare();
    CHECK(std::string(r->driver.status() ? r->driver.status() : "").find("ESP32") != std::string::npos);
    CHECK(mm::platform::hub75TestFrame(nullptr) == nullptr);
}

TEST_CASE("Hub75Driver packs an RGB light into the panel's bit planes") {
    PortGuard guard;
    auto r = rig();
    r->open();
    CHECK(paintAndShow(*r, 0, 0, 0) == -1);
}

TEST_CASE("Hub75Driver shows a wide profile's red, green and blue, wherever the profile puts them") {
    // A 32-channel moving head, its color at 9 to 11: correcting one light writes all 32 channels, past any small fixed array.
    PortGuard guard;
    auto r = rig();
    r->open();
    mm::ChannelRole roles[32] = {};
    roles[9] = mm::ChannelRole::Red;
    roles[10] = mm::ChannelRole::Green;
    roles[11] = mm::ChannelRole::Blue;
    rewire(*r, roles, 32);
    CHECK(paintAndShow(*r, 0, 0, 0) == -1);
}

TEST_CASE("Hub75Driver shows a green-first profile's red as red") {
    PortGuard guard;
    auto r = rig();
    r->open();
    const mm::ChannelRole grb[3] = {mm::ChannelRole::Green, mm::ChannelRole::Red, mm::ChannelRole::Blue};
    rewire(*r, grb, 3);
    CHECK(paintAndShow(*r, 0, 0, 0) == -1);
}

TEST_CASE("Hub75Driver shows a white-first profile's colors, not its white") {
    PortGuard guard;
    auto r = rig();
    r->open();
    const mm::ChannelRole wrgb[4] = {mm::ChannelRole::White, mm::ChannelRole::Red, mm::ChannelRole::Green, mm::ChannelRole::Blue};
    rewire(*r, wrgb, 4);
    CHECK(paintAndShow(*r, 0, 0, 0) == -1);
}

TEST_CASE("Hub75Driver lights a layout with fewer lights than its box, the rest of the panel dark") {
    PortGuard guard;
    auto r = rig(20);   // a ring or any shape short of its box
    r->open();
    CHECK(paintAndShow(*r, 19, 3, 1) == -1);   // (3, 1), the last light
}

namespace {
// A container that counts each park of its worker, as Drivers parks core 1.
struct Parking : mm::MoonModule {
    uint32_t parks = 0;
    void quiesce() override { parks++; }
};
}  // namespace

TEST_CASE("Hub75Driver parks its container's worker before a wider wiring moves its scratch, and not for a brightness change") {
    PortGuard guard;
    Parking owner;   // outlives the rig, whose driver points at it
    auto r = rig();
    owner.addChild(&r->driver);
    r->open();
    const mm::ChannelRole wrgb[4] = {mm::ChannelRole::White, mm::ChannelRole::Red, mm::ChannelRole::Green, mm::ChannelRole::Blue};
    const uint32_t before = owner.parks;
    rewire(*r, wrgb, 4);
    CHECK(owner.parks == before + 1);
    r->driver.onCorrectionChanged();   // the same wiring again, as a brightness change reaches it
    CHECK(owner.parks == before + 1);
    CHECK(paintAndShow(*r, 0, 0, 0) == -1);
    owner.removeChild(&r->driver);
}
