/// @module Hub75Driver
/// @also DriverBase

/// What Hub75Driver asks of its output: the card the output publishes, in the order the card shows it, and the status and the peripheral block it answers once the bus runs.

#include "doctest.h"
#include "../core/conditional_controls.h"   // mm::isHidden, mm::isVisible, mm::controlIndex
#include "core/module/Control.h"
#include "hub75_port.h"
#include "light/drivers/Hub75Driver.h"
#include "light/layers/Buffer.h"
#include "platform/Hub75Bus.h"   // kHub75PortMaxBitDepth
#include "platform/platform.h"
#include "wall_fixture.h"

#include <cstring>
#include <memory>
#include <string>

namespace {

// A driver on a 16-wide wall `height` high, wired as production wires it, on the heap since a driver and a layer outgrow a test's stack frame.
struct WiredDriver {
    mm::Wall wall;
    mm::Buffer source;
    mm::Hub75Driver driver;

    explicit WiredDriver(uint16_t height = 32) : wall(16, height) {
        REQUIRE(source.allocate(16 * height, 3));
        driver.setLayer(&wall.layer);
        driver.setSourceBuffer(&source);
        driver.defineControls();   // the card first, as the module owner builds it, so the backends on offer are known
        driver.applyState();
    }
    // The owner releases a driver before destroying it, which frees what prepare() allocated.
    ~WiredDriver() { driver.release(); }
};

const char* const kPinNames[] = {"r1", "g1", "b1", "r2", "g2", "b2", "a", "b", "c", "d", "e", "clk", "lat", "oe"};

}  // namespace

TEST_CASE("Hub75Driver's card lists the board, the fourteen pins, the peripheral, the scan rate, the depth, the clock edge and the refresh, each bound to its own member") {
    // A saved config reads the card by name and writes through the bound member, so the order, the ranges and the binding stay as they are.
    auto d = std::make_unique<mm::Hub75Driver>();
    d->defineControls();
    const char* const expected[] = {"board", "r1", "g1", "b1", "r2", "g2", "b2", "a", "b", "c", "d", "e",
                                    "clk", "lat", "oe", "peripheral", "scanRate", "bitDepth", "clockEdge", "refresh"};
    const void* const bound[] = {&d->boardSel, &d->r1, &d->g1, &d->b1, &d->r2, &d->g2, &d->b2, &d->addrA, &d->addrB,
                                 &d->addrC, &d->addrD, &d->addrE, &d->clk, &d->lat, &d->oe, &d->peripheralSel_,
                                 &d->scanSel, &d->bitDepth, &d->clockEdgeSel, nullptr};   // the refresh readout binds a text buffer
    const size_t count = sizeof(expected) / sizeof(expected[0]);
    // The driver's own controls follow the base's, so the card's tail is Hub75Driver's, from its board on.
    const uint8_t total = d->controls().count();
    REQUIRE(total >= count);
    const uint8_t first = static_cast<uint8_t>(total - count);
    for (size_t i = 0; i < count; i++) {
        CAPTURE(i);
        const mm::ControlDescriptor& c = d->controls()[static_cast<uint8_t>(first + i)];
        CHECK(std::strcmp(c.name, expected[i]) == 0);
        if (bound[i]) CHECK(c.ptr == bound[i]);
    }
    const auto& depth = d->controls()[static_cast<uint8_t>(first + 17)];
    CHECK(depth.min == 2);
    CHECK(depth.max == mm::kHub75PortMaxBitDepth);
    CHECK(d->controls()[static_cast<uint8_t>(first + 16)].max == 3);   // 1/8, 1/16, 1/32
    CHECK(d->controls()[static_cast<uint8_t>(first + 18)].max == 2);   // rising, falling
    CHECK(d->controls()[static_cast<uint8_t>(first + 19)].type == mm::ControlType::ReadOnly);
    CHECK(d->scanSel == 1);
    CHECK(d->bitDepth == 6);
    CHECK(d->clockEdgeSel == mm::Hub75Driver::kEdgeRising);
}

TEST_CASE("Hub75Driver's status names the geometry, the scan, the depth and the backend the bus runs at") {
    mm::PortGuard guard;
    auto w = std::make_unique<WiredDriver>();
    CHECK(w->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(w->driver.status()) == "16x32, 1/16 scan, 6-bit (LCD_CAM)");
}

TEST_CASE("Hub75Driver holds the peripheral block only while its bus runs") {
    mm::PortGuard guard;
    auto w = std::make_unique<WiredDriver>();   // applying its state opens the bus
    REQUIRE(w->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(w->driver.hwBlock() == mm::LedHwBlock::LcdCam);
    w->driver.release();
    CHECK(w->driver.hwBlock() == mm::LedHwBlock::None);
    w->driver.prepare();
    CHECK(w->driver.hwBlock() == mm::LedHwBlock::LcdCam);
}

TEST_CASE("Hub75Driver asks for a re-prepare on a change to the board, each of the fourteen pins, the peripheral, the scan rate, the bit depth or the clock edge, and for none on the refresh readout or the brightness") {
    auto d = std::make_unique<mm::Hub75Driver>();
    d->defineControls();
    for (const char* name : kPinNames) {
        CAPTURE(name);
        CHECK(d->affectsPrepare(name));
    }
    for (const char* name : {"board", "peripheral", "scanRate", "bitDepth", "clockEdge"}) {
        CAPTURE(name);
        CHECK(d->affectsPrepare(name));
    }
    CHECK_FALSE(d->affectsPrepare("refresh"));
    CHECK_FALSE(d->affectsPrepare("localBrightness"));
}

TEST_CASE("Hub75Driver offers the clock edge as rising, then falling") {
    // The labels index the select, so the first reads rising, the default, and the second falling.
    auto d = std::make_unique<mm::Hub75Driver>();
    d->defineControls();
    CHECK(mm::optionIndex(*d, "clockEdge", "rising") == mm::Hub75Driver::kEdgeRising);
    CHECK(mm::optionIndex(*d, "clockEdge", "falling") == mm::Hub75Driver::kEdgeFalling);
}

TEST_CASE("Hub75Driver offers the scan rate as 1/8, 1/16, then 1/32, on the scan select and not the clock edge's") {
    // The labels index the select, and kScanRates maps each index to the rate it names.
    auto d = std::make_unique<mm::Hub75Driver>();
    d->defineControls();
    CHECK(mm::optionIndex(*d, "scanRate", "1/8") == 0);
    CHECK(mm::optionIndex(*d, "scanRate", "1/16") == 1);
    CHECK(mm::optionIndex(*d, "scanRate", "1/32") == 2);
    CHECK(mm::optionIndex(*d, "scanRate", "rising") == -1);
    CHECK(mm::optionIndex(*d, "clockEdge", "1/16") == -1);
}

TEST_CASE("Hub75Driver hides the fourteen pin rows on a published board and shows them on Custom, re-rendering the card on each pick") {
    // A published map is soldered, so its pins stay bound and hidden; Custom is the hand-wired panel, whose pins the user edits.
    auto d = std::make_unique<mm::Hub75Driver>();
    d->defineControls();
    const int published = mm::optionIndex(*d, "board", "MatrixPortal S3");
    const int custom = mm::optionIndex(*d, "board", "Custom");
    REQUIRE(published >= 0);
    REQUIRE(custom >= 0);

    d->boardSel = static_cast<uint8_t>(custom);
    d->onControlChanged("board");
    for (const char* name : kPinNames) {
        CAPTURE(name);
        CHECK(mm::isVisible(*d, name));
    }

    d->boardSel = static_cast<uint8_t>(published);
    d->onControlChanged("board");
    for (const char* name : kPinNames) {
        CAPTURE(name);
        CHECK(mm::isHidden(*d, name));
    }
}

TEST_CASE("Hub75Driver keeps a pin edit on an editable board, since only a board pick writes a board's map") {
    auto d = std::make_unique<mm::Hub75Driver>();
    d->defineControls();
    d->boardSel = static_cast<uint8_t>(mm::optionIndex(*d, "board", "S3 generic"));   // editable, and not Custom
    d->onControlChanged("board");
    CHECK(d->r1 == 4);
    d->r1 = 20;
    d->onControlChanged("r1");
    CHECK(d->r1 == 20);
}

TEST_CASE("Hub75Driver leaves the card as it is on a change the output does not answer with a rebuild") {
    auto d = std::make_unique<mm::Hub75Driver>();
    d->defineControls();
    const int r1 = mm::controlIndex(*d, "r1");
    REQUIRE(r1 >= 0);
    REQUIRE(mm::isHidden(*d, "r1"));   // the first board is published, so its pins are hidden
    d->controls().setHidden(static_cast<uint8_t>(r1), false);   // a mark only a rebuild would take back
    d->onControlChanged("r1");
    CHECK(mm::isVisible(*d, "r1"));
    d->onControlChanged("board");   // a board change does rebuild, which hides the pin again
    CHECK(mm::isHidden(*d, "r1"));
}

TEST_CASE("Hub75Driver publishes the measured refresh on its one-second tick while the bus runs") {
    // The readout starts empty; the first tick publishes the platform's rate, a first 0 Hz included, since a host panel measures 0.
    mm::PortGuard guard;
    auto w = std::make_unique<WiredDriver>();
    const int i = mm::controlIndex(w->driver, "refresh");
    REQUIRE(i >= 0);
    const char* const refresh = static_cast<const char*>(w->driver.controls()[static_cast<uint8_t>(i)].ptr);
    CHECK(std::string(refresh) == "");
    w->driver.release();
    w->driver.tick1s();
    CHECK(std::string(refresh) == "");   // a stopped bus leaves the readout as it was
    w->driver.prepare();
    w->driver.tick1s();
    CHECK(std::string(refresh) == "0 Hz");
    mm::platform::setTestHub75RefreshHz(1508);
    w->driver.tick1s();
    CHECK(std::string(refresh) == "1508 Hz");
    mm::platform::setTestHub75RefreshHz(60);
    w->driver.tick1s();
    CHECK(std::string(refresh) == "60 Hz");
}

TEST_CASE("Hub75Driver sizes its correction scratch to one RGB frame of the layout's box") {
    mm::PortGuard guard;
    auto w = std::make_unique<WiredDriver>();   // 16 x 32 lights, three bytes each
    REQUIRE(w->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(w->driver.dynamicBytes() == 16u * 32u * 3u);
}

TEST_CASE("Hub75Driver offers LCD_CAM and Parlio where the silicon has both, and opens the one the peripheral select names") {
    mm::PortGuard guard(true);
    auto w = std::make_unique<WiredDriver>();
    mm::Hub75Driver& d = w->driver;
    const mm::platform::Hub75TestPort& port = mm::platform::hub75TestPort();
    CHECK(mm::optionIndex(d, "peripheral", "LCD_CAM") == 0);
    const int parlio = mm::optionIndex(d, "peripheral", "Parlio");
    REQUIRE(parlio == 1);
    CHECK(port.backend == mm::platform::Hub75Backend::LcdCam);
    CHECK(d.hwBlock() == mm::LedHwBlock::LcdCam);

    d.peripheralSel_ = static_cast<uint8_t>(parlio);
    d.prepare();
    CHECK(port.backend == mm::platform::Hub75Backend::Parlio);
    CHECK(d.hwBlock() == mm::LedHwBlock::Parlio);
    CHECK(std::string(d.status()) == "16x32, 1/16 scan, 6-bit (Parlio)");
}

TEST_CASE("Hub75Driver answers the first backend on offer for a peripheral select past the options") {
    mm::PortGuard guard(true);
    auto w = std::make_unique<WiredDriver>();
    mm::Hub75Driver& d = w->driver;
    REQUIRE(mm::optionIndex(d, "peripheral", "Parlio") == 1);
    mm::platform::setTestHub75(true, false);   // the port offers LCD_CAM alone
    d.rebuildControls();                        // one option; backendIndex_[1] still holds Parlio
    d.peripheralSel_ = 1;
    CHECK(d.selectedBackend() == mm::platform::Hub75Backend::LcdCam);
}

TEST_CASE("Hub75Driver hands the port the fourteen pins in ribbon order and the clock edge the card names") {
    // Fourteen distinct pins on Custom, which keeps what is typed, so a swapped or dropped line reads as the wrong number.
    mm::PortGuard guard;
    auto w = std::make_unique<WiredDriver>();
    mm::Hub75Driver& d = w->driver;
    d.boardSel = static_cast<uint8_t>(mm::optionIndex(d, "board", "Custom"));
    d.onControlChanged("board");
    int8_t* const pins[] = {&d.r1, &d.g1, &d.b1, &d.r2, &d.g2, &d.b2, &d.addrA, &d.addrB,
                            &d.addrC, &d.addrD, &d.addrE, &d.clk, &d.lat, &d.oe};
    for (size_t i = 0; i < 14; i++) *pins[i] = static_cast<int8_t>(4 + i);
    d.prepare();
    REQUIRE(d.severity() == mm::MoonModule::Severity::Status);

    const mm::platform::Hub75Pins& got = mm::platform::hub75TestPort().pins;
    const uint16_t handed[] = {got.r1, got.g1, got.b1, got.r2, got.g2, got.b2, got.a, got.b,
                               got.c, got.d, got.e, got.clk, got.lat, got.oe};
    for (size_t i = 0; i < 14; i++) {
        CAPTURE(kPinNames[i]);
        CHECK(handed[i] == 4 + i);
    }
    CHECK_FALSE(got.clkFalling);

    d.clockEdgeSel = mm::Hub75Driver::kEdgeFalling;
    d.prepare();
    CHECK(mm::platform::hub75TestPort().pins.clkFalling);
}

TEST_CASE("Hub75Driver asks the platform about the bytes its layout takes, and none for a layout that leaves a partial scan row pair") {
    mm::PortGuard guard;
    auto w = std::make_unique<WiredDriver>();
    mm::Hub75Geometry geo;   // 16x32 at the default 1/16 scan and 6-bit
    geo.width = 16;
    geo.height = 32;
    geo.scanRate = 16;
    geo.bitDepth = 6;
    REQUIRE(geo.frameBytes() > 0);
    CHECK(mm::platform::hub75TestPort().offeredBytes == geo.frameBytes());

    // 24 rows at 1/8 leave three rows a scan, which pair up no better than one; its geometry still has a size, which the output does not offer.
    auto odd = std::make_unique<WiredDriver>(24);
    odd->driver.scanSel = 0;
    odd->driver.rebuildControls();
    CHECK(mm::platform::hub75TestPort().offeredBytes == 0);
}

TEST_CASE("Hub75Driver releases the panel's peripheral when it stops, and when the scan will not start") {
    mm::PortGuard guard;
    auto w = std::make_unique<WiredDriver>();
    mm::Hub75Driver& d = w->driver;
    const mm::platform::Hub75TestPort& port = mm::platform::hub75TestPort();
    REQUIRE(mm::platform::hub75TestFrame(nullptr) != nullptr);
    const uint32_t before = port.deinits;
    d.release();
    CHECK(port.deinits == before + 1);
    CHECK(mm::platform::hub75TestFrame(nullptr) == nullptr);

    mm::platform::setTestHub75StartFails(true);
    d.prepare();
    CHECK(d.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(d.status()) == "the panel scan would not start");
    CHECK(mm::platform::hub75TestFrame(nullptr) == nullptr);   // the opened port is let go, not left holding its pins
    CHECK(d.hwBlock() == mm::LedHwBlock::None);
}

TEST_CASE("Hub75Driver releases the panel's peripheral when the memory for one light of a wider wiring runs out") {
    // The correction buffer is kept from the first prepare, so the refusal reaches only the light scratch, which platform::alloc serves.
    mm::PortGuard guard;
    auto w = std::make_unique<WiredDriver>();
    mm::Hub75Driver& d = w->driver;
    const mm::ChannelRole rgbw[4] = {mm::ChannelRole::Red, mm::ChannelRole::Green, mm::ChannelRole::Blue, mm::ChannelRole::White};
    d.correctionForTest().rebuild(255, rgbw, 4);   // four channels a light, so a scratch of four bytes
    const uint32_t before = mm::platform::hub75TestPort().deinits;
    {
        mm::RefuseAbove refuse(3);
        d.prepare();
    }
    CHECK(d.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(d.status()) == "not enough memory for one light of this fixture");
    CHECK(mm::platform::hub75TestFrame(nullptr) == nullptr);   // no frame reaches the port
    CHECK(d.hwBlock() == mm::LedHwBlock::None);
    CHECK(mm::platform::hub75TestPort().deinits == before + 2);   // the reinit's stop, then the close after the refusal
}
