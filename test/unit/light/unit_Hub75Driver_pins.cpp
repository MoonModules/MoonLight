/// @module Hub75Driver
/// @also PinsModule

#include "doctest.h"
#include "light/drivers/Hub75Driver.h"
#include "../core/conditional_controls.h"   // mm::controlIndex
#include "hub75_port.h"                       // mm::optionIndex
#include "core/system/PinsModule.h"
#include "core/module/Scheduler.h"
#include "core/util/JsonSink.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "platform/platform_io.h"   // GpioCapability, clearTestGpioCapability, setTestGpioCapability

#include <cstring>
#include <string>

using namespace mm;

// A published board map hides its fourteen pin controls and the pin map reads a hidden pin control as free. These tests pin the bridge: a selected published board reaches the map through fixedPins(), an editable board through its visible controls.

namespace {

// Board index by label, so a reordered table cannot silently test a different board.
uint8_t boardNamed(Hub75Driver& d, const char* label) {
    const int k = optionIndex(d, "board", label);
    return k < 0 ? 0xFF : static_cast<uint8_t>(k);
}

void selectBoard(Hub75Driver& d, const char* label) {
    const uint8_t k = boardNamed(d, label);
    REQUIRE(k != 0xFF);
    d.boardSel = k;
    d.onControlChanged("board");   // writes the board's map into the pins and re-renders, as the API does
}

// The capability override is global, so it must come off on EVERY exit path: a failing REQUIRE unwinds past a trailing clearTestGpioCapability() and leaks the fake pin into later cases.
struct ScopedGpioCapability {
    ScopedGpioCapability(uint8_t gpio, platform::GpioCapability cap) {
        platform::clearTestGpioCapability();
        platform::setTestGpioCapability(gpio, cap);
    }
    ~ScopedGpioCapability() { platform::clearTestGpioCapability(); }
};

struct LaneModule : MoonModule {
    int8_t lane = 18;
    LaneModule() { setName("RmtLed"); }
    void defineControls() override { controls_.clear(); controls_.addPin("pin", lane); }
};

const ListSource* pinsSource(const MoonModule& m) {
    for (uint8_t i = 0; i < m.controls().count(); i++)
        if (std::strcmp(m.controls()[i].name, "pins") == 0)
            return static_cast<const ListSource*>(m.controls()[i].ptr);
    return nullptr;
}

std::string allRows(const ListSource& src) {
    std::string out;
    for (ListRow i = 0; i < src.listRowCount(); i++) {
        JsonSink r; src.writeListRow(r, i);
        out += r.data();
    }
    return out;
}

}  // namespace

TEST_CASE("Hub75Driver offers the clock edge, defaults to rising, and re-inits on a change") {
    // Some panel chips sample the clock on the falling edge; driven on the rising one every pixel lands a column over. A fact about the panel, so it sits beside the pins and a change re-inits the port.
    mm::Hub75Driver d;
    d.defineControls();
    const int i = mm::controlIndex(d, "clockEdge");
    REQUIRE(i >= 0);
    CHECK(d.clockEdgeSel == mm::Hub75Driver::kEdgeRising);
    CHECK(d.affectsPrepare("clockEdge"));
}

TEST_CASE("Hub75Driver fills the pins from the first board once, at the first card, and a re-render keeps an edited pin") {
    // The first board a host offers is MoonHub75, whose r1 is GPIO 1.
    mm::Hub75Driver d;
    d.defineControls();
    CHECK(d.r1 == 1);
    d.r1 = 20;
    d.rebuildControls();
    CHECK(d.r1 == 20);
}

TEST_CASE("Hub75Driver starts a P4 with no pins, and the Waveshare P4-NANO row carries that board's free set") {
    // P4 boards wire different pins: the P4-NANO's 39-42 are the TF slot's on the WIFI6-DEV-KIT, off its header, so the generic row names none.
    mm::Hub75Driver d;
    d.defineControls();
    selectBoard(d, "P4 generic");
    CHECK(d.r1 == -1);
    CHECK(d.addrE == -1);
    CHECK(d.oe == -1);
    selectBoard(d, "Waveshare P4-NANO");
    CHECK(d.r1 == 20);
    CHECK(d.addrE == 39);
    CHECK(d.oe == 42);
}

TEST_CASE("Hub75Driver offers 2 to 6 bit planes, though the encoder reaches 8") {
    // The encoder shifts and weights up to 8 planes, but the ESP32 port stops at 6 until the repeated-planes card lands (the reason is at `kHub75PortMaxBitDepth`), so the control and the port bound the depth rather than valid().
    mm::Hub75Driver d;
    d.defineControls();
    const int i = mm::controlIndex(d, "bitDepth");
    REQUIRE(i >= 0);
    CHECK(d.controls()[i].min == 2);
    CHECK(d.controls()[i].max == 6);
}

TEST_CASE("Hub75Driver reports a published board's lines as fixed pins while their controls hide") {
    Hub75Driver d;
    d.defineControls();
    selectBoard(d, "MoonHub75");

    const MoonModule& m = d;   // the hook is the base's public contract; the driver keeps it private
    MoonModule::FixedPin out[16];
    const uint8_t n = m.fixedPins(out, 16);
    CHECK(n == 14);
    bool clkOn18 = false;
    for (uint8_t i = 0; i < n; i++)
        if (out[i].gpio == 18 && std::strcmp(out[i].role, "clk") == 0) clkOn18 = true;
    CHECK(clkOn18);

    // The collector states a capacity and the driver respects it.
    CHECK(m.fixedPins(out, 3) == 3);
    CHECK(m.fixedPins(nullptr, 16) == 0);
}

TEST_CASE("Hub75Driver reports no fixed pins for an editable board: its controls carry the claim") {
    Hub75Driver d;
    d.defineControls();
    selectBoard(d, "Custom");
    const MoonModule& m = d;
    MoonModule::FixedPin out[16];
    CHECK(m.fixedPins(out, 16) == 0);
}

TEST_CASE("PinsModule flags a HUB75 board line colliding with an LED lane") {
    Scheduler scheduler;
    Hub75Driver hub;
    hub.setName("Hub75");
    LaneModule led;
    PinsModule pins;
    scheduler.addModule(&hub);
    scheduler.addModule(&led);
    scheduler.addModule(&pins);
    scheduler.setup();
    selectBoard(hub, "MoonHub75");   // clk on 18, the lane's pin
    pins.tick1s();

    const ListSource* src = pinsSource(pins);
    REQUIRE(src != nullptr);
    const std::string rows = allRows(*src);
    CHECK(rows.find("\"owner\":\"Hub75\"") != std::string::npos);
    CHECK(rows.find("\"role\":\"clk\"") != std::string::npos);
    // Both GPIO-18 rows carry the conflict.
    size_t flagged = 0;
    for (ListRow i = 0; i < src->listRowCount(); i++) {
        JsonSink r; src->writeListRow(r, i);
        const std::string row(r.data());
        if (row.find("\"gpio\":18") != std::string::npos &&
            row.find("\"severity\":\"error\"") != std::string::npos) flagged++;
    }
    CHECK(flagged == 2);
}

// A published map can land a line on a pin THIS module has wired to PSRAM (the MatrixPortal's b, d and b2 on 35-37, the PSRAM bus on an octal part). The LCD bus there resets the chip with no panic, so the driver refuses before init.
TEST_CASE("Hub75Driver refuses a line on a flash/PSRAM pin and names it, rather than resetting") {
    platform::GpioCapability psram;
    psram.reserved = true;
    const ScopedGpioCapability held(36, psram);   // MatrixPortal's b line

    Layouts layouts; GridLayout grid; Layer layer; Hub75Driver d;
    grid.width = 64; grid.height = 64; grid.depth = 1;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.applyState();
    d.setLayer(&layer);
    d.defineControls();
    selectBoard(d, "MatrixPortal S3");
    d.prepare();

    REQUIRE(d.status() != nullptr);
    CHECK(d.severity() == MoonModule::Severity::Error);
    CHECK(std::strstr(d.status(), "b on GPIO 36") != nullptr);
    CHECK(std::strstr(d.status(), "PSRAM") != nullptr);
}
