/// @module Hub75Driver
/// @also DriverBase

/// What Hub75Driver asks of its output: the card the output publishes, in the order the card shows it, and the status and the peripheral block it answers once the bus runs.

#include "doctest.h"
#include "../core/conditional_controls.h"   // mm::isHidden, mm::isVisible, mm::controlIndex
#include "core/module/Control.h"
#include "hub75_port.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"   // rebuildFromPreset
#include "light/drivers/Hub75Driver.h"
#include "light/layers/Buffer.h"
#include "platform/Hub75Bus.h"   // kHub75PortMaxBitDepth
#include "platform/platform.h"
#include "wall_fixture.h"

#include <cstring>
#include <memory>
#include <string>
#include <type_traits>

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

// An output that counts what the driver asks of it, in place of the bus output, to pin how often the driver calls across the interface.
struct CountingOutput : mm::Hub75Output {
    size_t count = 16u * 32u;           // the lights it answers
    mutable size_t lightsCalls = 0;     // counted from const lights()
    size_t shows = 0;
    size_t probe = 0;                   // the light whose red show() records
    uint8_t probeRed = 0;               // the red of light `probe` in the last frame shown
    const char* warn = nullptr;         // the warning it answers, null while healthy

    void addWiringControls(mm::ControlList&) override {}
    void addControls(mm::ControlList&) override {}
    bool onControlChanged(const char*) override { return false; }
    bool affectsPrepare(const char*) const override { return false; }
    mm::DepthRange depthRange() const override { return {2, 6}; }
    size_t frameBytes(uint16_t, uint16_t) const override { return 0; }
    const char* open(uint16_t, uint16_t) override { return nullptr; }
    const char* describe() const MM_NONBLOCKING override { return "counting"; }
    const char* warning() const MM_NONBLOCKING override { return warn; }
    size_t lights() const MM_NONBLOCKING override { lightsCalls++; return count; }
    void show(const uint8_t* rgb, uint8_t) MM_NONBLOCKING override { shows++; probeRed = rgb[probe * 3]; }
    void tick1s() MM_NONBLOCKING override {}
    void close() override {}
    mm::LedHwBlock hwBlock() const override { return mm::LedHwBlock::LcdCam; }
    uint8_t fixedPins(mm::MoonModule::FixedPin*, uint8_t) const override { return 0; }
};

// A driver on a 16 x 32 wall over a counting output, on the heap since a driver and a layer outgrow a test's stack frame; the driver is unprepared.
struct CountedDriver {
    mm::Wall wall{16, 32};
    mm::Buffer source;
    mm::Hub75Driver driver;
    CountingOutput out;

    CountedDriver() {
        REQUIRE(source.allocate(16 * 32, 3));
        driver.outputForTest() = &out;
        driver.setLayer(&wall.layer);
        driver.setSourceBuffer(&source);
        mm::Correction correction;
        mm::rebuildFromPreset(correction, 255, mm::PresetOrder::RGB);
        driver.correctionForTest() = correction;
    }
    ~CountedDriver() { driver.release(); }
};

// The status a healthy 16x32 panel shows on the bus.
const char* const kHealthyStatus = "16x32, 1/16 scan, 6-bit (LCD_CAM)";
// A driver on the bus in that layout, opened.
std::unique_ptr<WiredDriver> runningWiredDriver() {
    auto w = std::make_unique<WiredDriver>();
    REQUIRE(w->driver.severity() == mm::MoonModule::Severity::Status);
    return w;
}

// Wire `d` for a light of 32 channels under a heap that refuses the scratch for one light, which the status says.
void wideWiringShort(mm::Hub75Driver& d) {
    const mm::WideProfile wide;
    mm::RefuseAbove refuse(16);   // the 32-byte scratch is refused
    d.correctionForTest().rebuild(255, wide.roles, 32);
    d.onCorrectionChanged();
}

// Give `d` a gain, which asks for tables, under a heap that refuses them, which the status says.
void gainsShort(mm::Hub75Driver& d) {
    d.correctionForTest().gainGreen = 128;
    mm::RefuseAbove refuse(512);   // the gained tables, four rows of 256 bytes, are refused
    d.rebuildCorrection(255);
}

const char* const kNoLightText = "not enough memory for one light of this fixture";

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

TEST_CASE("Hub75Driver reads its output's lights once, in prepare(), and a frame costs the output's show() alone") {
    auto w = std::make_unique<CountedDriver>();
    w->driver.prepare();
    REQUIRE(w->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(w->out.lightsCalls == 1);
    w->source.data()[0] = 200;
    for (int frame = 0; frame < 5; frame++) w->driver.tick();
    CHECK(w->out.shows == 5);
    CHECK(w->out.probeRed == 200);
    CHECK(w->out.lightsCalls == 1);   // five frames later, still the prepare()'s one
    w->driver.onCorrectionChanged();
    CHECK(w->out.lightsCalls == 1);
}

TEST_CASE("Hub75Driver reads its output's lights again on every prepare, and the next frame has the new length") {
    auto w = std::make_unique<CountedDriver>();
    w->out.count = 16u * 16u;
    w->driver.prepare();
    REQUIRE(w->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(w->out.lightsCalls == 1);
    CHECK(w->driver.dynamicBytes() == 16u * 16u * 3u);
    w->out.count = 16u * 32u;   // the box the output opens at has grown
    w->driver.prepare();
    REQUIRE(w->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(w->out.lightsCalls == 2);
    REQUIRE(w->driver.dynamicBytes() == 16u * 32u * 3u);   // the scratch holds the new frame, so the frame below stays inside it
    w->out.probe = 300;   // a light past the first box
    w->source.data()[300 * 3] = 99;
    w->driver.tick();
    CHECK(w->out.probeRed == 99);
    CHECK(w->out.lightsCalls == 2);
}

TEST_CASE("Hub75BusOutput is final, so a call whose static type is the bus output resolves without dispatch") {
    CHECK(std::is_final_v<mm::Hub75BusOutput>);
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

TEST_CASE("Hub75Driver offers 48 rows at 1/8 scan and refuses them at 1/16, so the scan select reaches the geometry") {
    // 48 rows leave 6 rows a scan at 1/8, which pair, and 3 at 1/16, which do not; the select alone changes which. A height of 24 pairs at no scan and 32 pairs at 1/8 and 1/16 alike, so of 24, 32 and 48 only 48 shows that.
    mm::PortGuard guard;
    auto w = std::make_unique<WiredDriver>(48);   // the default 1/16 scan
    CHECK(w->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(mm::platform::hub75TestPort().offeredBytes == 0);
    w->driver.scanSel = 0;
    w->driver.rebuildControls();
    CHECK(mm::platform::hub75TestPort().offeredBytes > 0);
    w->driver.prepare();
    CHECK(w->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(w->driver.status()) == "16x48, 1/8 scan, 6-bit (LCD_CAM)");
    w->driver.scanSel = 1;
    w->driver.rebuildControls();
    CHECK(mm::platform::hub75TestPort().offeredBytes == 0);
    w->driver.prepare();
    CHECK(w->driver.severity() == mm::MoonModule::Severity::Error);
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

TEST_CASE("Hub75Driver keeps a light-memory shortage on its status through the one-second tick, and shows the panel's status once the shortage ends") {
    mm::PortGuard guard;
    auto w = runningWiredDriver();
    mm::Hub75Driver& d = w->driver;
    wideWiringShort(d);
    CHECK(d.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(d.status()) == kNoLightText);
    d.tick1s();
    CHECK(d.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(d.status()) == kNoLightText);

    mm::rebuildFromPreset(d.correctionForTest(), 255, mm::PresetOrder::RGB);   // an RGB wiring, which corrects straight into the frame
    d.onCorrectionChanged();
    CHECK(d.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(d.status()) == kHealthyStatus);
}

TEST_CASE("Hub75Driver retries the light scratch at the same width once memory is back, and shows the panel's status") {
    mm::PortGuard guard;
    auto w = runningWiredDriver();
    mm::Hub75Driver& d = w->driver;
    wideWiringShort(d);
    REQUIRE(std::string(d.status()) == kNoLightText);
    const mm::WideProfile wide;
    d.correctionForTest().rebuild(255, wide.roles, 32);   // the same 32 channels, with the memory back
    d.onCorrectionChanged();
    CHECK(d.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(d.status()) == kHealthyStatus);
    CHECK(d.dynamicBytes() == 16u * 32u * 3u + 32u);
}

TEST_CASE("Hub75Driver keeps the gain-memory warning on its status through a prepare and the one-second tick, and shows the panel's status once the gains fit") {
    mm::PortGuard guard;
    auto w = runningWiredDriver();
    mm::Hub75Driver& d = w->driver;
    gainsShort(d);
    CHECK(d.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(d.status()).find("color balance") != std::string::npos);
    d.prepare();   // the order production runs: the rebuild first, then the prepare
    CHECK(d.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(d.status()).find("color balance") != std::string::npos);
    d.tick1s();
    CHECK(d.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(d.status()).find("color balance") != std::string::npos);

    d.rebuildCorrection(255);   // the memory is back, so the gains apply and the warning leaves
    d.tick1s();
    CHECK(d.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(d.status()) == kHealthyStatus);
}

TEST_CASE("Hub75Driver shows the output's warning over the gain warning, and the gain warning again once the output's ends, the same each second") {
    auto w = std::make_unique<CountedDriver>();
    mm::Hub75Driver& d = w->driver;
    d.prepare();
    gainsShort(d);
    const std::string gains = d.status();
    REQUIRE(d.severity() == mm::MoonModule::Severity::Warning);
    w->out.warn = "the output is stalled";
    for (int second = 0; second < 2; second++) {
        d.tick1s();
        CHECK(std::string(d.status()) == "the output is stalled");
    }
    w->out.warn = nullptr;
    for (int second = 0; second < 2; second++) {
        d.tick1s();
        CHECK(std::string(d.status()) == gains);
    }
    d.rebuildCorrection(255);   // the gains fit, so only the output's own status remains
    d.tick1s();
    CHECK(std::string(d.status()) == "counting");
}

TEST_CASE("Hub75Driver keeps the missing light scratch's error over the output's warning, and over the output's status after it ends") {
    auto w = std::make_unique<CountedDriver>();
    mm::Hub75Driver& d = w->driver;
    d.prepare();
    wideWiringShort(d);
    w->out.warn = "the output is stalled";
    d.tick1s();
    CHECK(d.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(d.status()) == kNoLightText);
    w->out.warn = nullptr;
    d.tick1s();
    CHECK(std::string(d.status()) == kNoLightText);
}

TEST_CASE("Hub75Driver's memory readout is the frame and the light scratch while it runs, and 0 after a release, for a wiring of 32 channels") {
    mm::PortGuard guard;
    auto w = runningWiredDriver();
    mm::Hub75Driver& d = w->driver;
    CHECK(d.dynamicBytes() == 16u * 32u * 3u);
    const mm::WideProfile wide;
    d.correctionForTest().rebuild(255, wide.roles, 32);
    d.onCorrectionChanged();
    CHECK(d.dynamicBytes() == 16u * 32u * 3u + 32u);   // the frame and one 32-channel light
    d.prepare();   // a prepare keeps the scratch and the readout
    CHECK(d.dynamicBytes() == 16u * 32u * 3u + 32u);
    d.release();
    CHECK(d.dynamicBytes() == 0);
}

TEST_CASE("Hub75Driver's memory readout counts the gained tables and the 16-bit tables beside the frame, and returns to 0 after a release") {
    mm::PortGuard guard;
    auto w = runningWiredDriver();
    mm::Hub75Driver& d = w->driver;
    d.correctionForTest().gainGreen = 128;
    d.rebuildCorrection(255);
    d.prepare();
    CHECK(d.dynamicBytes() == 16u * 32u * 3u + 4u * 256u);   // the frame and four gained rows of 256 bytes
    d.correctionForTest().wide = true;
    d.rebuildCorrection(255);
    d.prepare();
    CHECK(d.dynamicBytes() == 16u * 32u * 3u + 4u * 256u + 2u * (256u + 4u * 256u));   // and the 16-bit curve and its gained rows
    d.release();
    CHECK(d.dynamicBytes() == 0);
}
