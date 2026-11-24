/// @module Hub75Driver
/// @also Correction

/// Hub75Driver's tick, pinned down to the bit planes. The host's HUB75 port, opened by the test (`setTestHub75` in platform.h) in place of a panel, hands the driver one frame as the ESP32 port does, and each case compares it with what the encoder makes of the colors the panel should show.

#include "doctest.h"
#include "light/drivers/Hub75Driver.h"
#include "hub75_port.h"
#include "platform/Hub75Encode.h"
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

// The default box, kWidth by kHeight (16 wide, 32 high): the smallest that pairs into rows at the driver's default 1/16 scan; the depth is its default, 6-bit.
constexpr uint16_t kWidth = 16;
constexpr uint16_t kHeight = 32;

// A driver on a layout `kWidth` wide and `height` high, its source holding one light per pixel, wired as production wires it, on the heap since a driver and a layer outgrow a test's stack frame.
struct Rig {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::Buffer source;
    mm::Hub75Driver driver;

    explicit Rig(uint16_t height) {
        grid.width = kWidth;
        grid.height = height;
        grid.depth = 1;
        layouts.addChild(&grid);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
        REQUIRE(source.allocate(static_cast<mm::nrOfLightsType>(kWidth) * height, 3));
        driver.setLayer(&layer);
        driver.setSourceBuffer(&source);
    }
    // The owner releases a driver before destroying it, which frees what open() allocated.
    ~Rig() { driver.release(); }

    // Open the port with a linear, full-brightness correction, so a frame's values read straight; `scanLabel` is the status the scan the driver is set to opens with.
    void open(const char* scanLabel = "1/16 scan") {
        mm::Correction correction;
        mm::rebuildFromPreset(correction, 255, mm::PresetOrder::RGB);
        driver.correctionForTest() = correction;
        driver.applyState();
        driver.prepare();
        REQUIRE(std::string(driver.status() ? driver.status() : "").find(scanLabel) != std::string::npos);
    }

    void paint(mm::nrOfLightsType i, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = source.data() + static_cast<size_t>(i) * 3;
        p[0] = r; p[1] = g; p[2] = b;
    }
};

std::unique_ptr<Rig> rig(uint16_t height = kHeight) {
    return std::make_unique<Rig>(height);
}

// An RGB image of kWidth by kHeight pixels, dark but for the one pixel at (x, y); firstDifference reads its height from its size.
std::vector<uint8_t> onePixel(uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> rgb(static_cast<size_t>(kWidth) * kHeight * 3, 0);
    uint8_t* p = rgb.data() + (static_cast<size_t>(y) * kWidth + x) * 3;
    p[0] = r; p[1] = g; p[2] = b;
    return rgb;
}

// Where the frame the driver packed first differs from the encoder's frame for `rgb`, a `kWidth`-wide image, at the scan, depth and brightness given, or -1 where they match.
long firstDifference(const std::vector<uint8_t>& rgb, uint8_t scanRate = 16, uint8_t bitDepth = 6, uint8_t brightness = 255) {
    size_t bytes = 0;
    const uint8_t* frame = mm::platform::hub75TestFrame(&bytes);
    if (!frame) return -2;
    mm::Hub75Geometry geo;
    geo.width = kWidth;
    geo.height = static_cast<uint16_t>(rgb.size() / (static_cast<size_t>(kWidth) * 3));
    geo.scanRate = scanRate;
    geo.bitDepth = bitDepth;
    geo.brightness = brightness;
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

TEST_CASE("Hub75Driver on a host refuses with the cause, as on a chip without the silicon") {
    auto r = rig();
    r->driver.applyState();
    r->driver.prepare();
    CHECK(std::string(r->driver.status() ? r->driver.status() : "").find("ESP32") != std::string::npos);
    CHECK(mm::platform::hub75TestFrame(nullptr) == nullptr);
}

TEST_CASE("The HUB75 port opens at 6 bit planes and refuses 7, though the encoder reaches 8") {
    // The port holds to its maximum depth: the encoder's range is wider, and the reason is at `kHub75PortMaxBitDepth`.
    mm::PortGuard guard;
    mm::platform::Hub75Handle handle;
    mm::platform::Hub75Pins pins;
    CHECK(mm::platform::hub75Init(handle, mm::platform::Hub75Backend::LcdCam, pins, 64, 64, 32, mm::kHub75PortMaxBitDepth));
    CHECK_FALSE(mm::platform::hub75Init(handle, mm::platform::Hub75Backend::LcdCam, pins, 64, 64, 32, mm::kHub75PortMaxBitDepth + 1));
    // A refused re-init leaves the port closed, its buffer and its scan released, where the port had held the earlier frame.
    CHECK(mm::platform::hub75Buffer(handle) == nullptr);
    CHECK_FALSE(mm::platform::hub75Start(handle));
    mm::platform::hub75Deinit(handle);
}

TEST_CASE("Hub75Driver packs an RGB light into the panel's bit planes") {
    mm::PortGuard guard;
    auto r = rig();
    r->open();
    CHECK(paintAndShow(*r, 0, 0, 0) == -1);
}

TEST_CASE("Hub75Driver shows a wide profile's red, green and blue, wherever the profile puts them") {
    // A 32-channel moving head, its color at 9 to 11: correcting one light writes all 32 channels, past any small fixed array.
    mm::PortGuard guard;
    auto r = rig();
    r->open();
    mm::ChannelRole roles[32] = {};
    roles[9] = mm::ChannelRole::Red;
    roles[10] = mm::ChannelRole::Green;
    roles[11] = mm::ChannelRole::Blue;
    rewire(*r, roles, 32);
    CHECK(paintAndShow(*r, 0, 0, 0) == -1);
}

TEST_CASE("Hub75Driver puts a BGR panel's colors on the lines its profile names") {
    // The panel is the light: its blue LEDs sit on the red line, so the profile's order is the order on the wires.
    mm::PortGuard guard;
    auto r = rig();
    r->open();
    const mm::ChannelRole bgr[3] = {mm::ChannelRole::Blue, mm::ChannelRole::Green, mm::ChannelRole::Red};
    rewire(*r, bgr, 3);
    r->paint(0, 200, 100, 50);
    r->driver.tick();
    CHECK(firstDifference(onePixel(0, 0, 50, 100, 200)) == -1);
}

TEST_CASE("Hub75Driver shows a white-first profile's colors, not its white") {
    mm::PortGuard guard;
    auto r = rig();
    r->open();
    const mm::ChannelRole wrgb[4] = {mm::ChannelRole::White, mm::ChannelRole::Red, mm::ChannelRole::Green, mm::ChannelRole::Blue};
    rewire(*r, wrgb, 4);
    CHECK(paintAndShow(*r, 0, 0, 0) == -1);
}

TEST_CASE("Hub75Driver lights a layout with fewer lights than its box, the rest of the panel dark") {
    mm::PortGuard guard;
    auto r = rig();
    r->open();
    // A full frame first, so the correction scratch holds light where the short layout leaves none.
    for (mm::nrOfLightsType i = 0; i < static_cast<mm::nrOfLightsType>(kWidth) * kHeight; i++) r->paint(i, 255, 255, 255);
    r->driver.tick();
    REQUIRE(r->source.allocate(20, 3));   // a ring or any shape short of its box
    std::memset(r->source.data(), 0, static_cast<size_t>(20) * 3);
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
    mm::PortGuard guard;
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

TEST_CASE("Hub75Driver packs the frame the encoder makes at 1/8, 1/16 and 1/32 scan, at every depth, and at full, dim and lowest brightness") {
    // The bytes on the panel's wire are the encoder's, byte for byte.
    mm::PortGuard guard;
    // The layout's height pairs into rows at each scan: 32 rows at 1/8 and 1/16, 64 at 1/32.
    struct Pass { uint8_t scanSel; uint8_t scanRate; uint16_t height; const char* status; };
    const Pass passes[] = {{0, 8, 32, "1/8 scan"}, {1, 16, 32, "1/16 scan"}, {2, 32, 64, "1/32 scan"}};
    const uint8_t brightnesses[] = {255, 100, 1};
    for (const Pass& pass : passes) {
        for (uint8_t depth = 2; depth <= mm::kHub75PortMaxBitDepth; depth++) {
            for (uint8_t brightness : brightnesses) {
                CAPTURE(pass.scanRate);
                CAPTURE(depth);
                CAPTURE(brightness);
                auto r = rig(pass.height);
                r->driver.scanSel = pass.scanSel;
                r->driver.bitDepth = depth;
                r->open(pass.status);
                r->driver.rampBrightness(brightness);
                const mm::nrOfLightsType last = static_cast<mm::nrOfLightsType>(kWidth) * pass.height - 1;
                r->paint(0, 200, 100, 50);
                r->paint(37, 10, 250, 90);
                r->paint(last, 255, 255, 255);
                r->driver.tick();
                std::vector<uint8_t> rgb(static_cast<size_t>(kWidth) * pass.height * 3, 0);   // light order, one pixel per light
                const uint8_t painted[3][3] = {{200, 100, 50}, {10, 250, 90}, {255, 255, 255}};
                const size_t lights[3] = {0, 37, last};
                for (size_t k = 0; k < 3; k++) std::memcpy(rgb.data() + lights[k] * 3, painted[k], 3);
                CHECK(firstDifference(rgb, pass.scanRate, depth, brightness) == -1);
            }
        }
    }
}

// A HUB75 panel's lines carry red, green and blue in that order unless its profile says otherwise, so a new driver starts there.
TEST_CASE("A HUB75 driver starts on the RGB profile") {
    mm::Hub75Driver drv;
    REQUIRE(drv.defaultFixtureName() != nullptr);
    CHECK(std::string(drv.defaultFixtureName()) == "RGB");
}
