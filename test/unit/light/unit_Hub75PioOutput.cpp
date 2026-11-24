/// @module Hub75PioOutput
/// @also Hub75Driver, Correction

/// Hub75Driver on a Raspberry Pi 5's header, through the RP1's PIO, pinned on a desktop, no panels attached. The PIO seam records what the output opened and handed over (the `hub75Pio*` seam in platform/Hub75Pio.h); the bench Pi then only has to show it.

#include "doctest.h"
#include "light/drivers/Hub75PioOutput.h"
#include "light/drivers/Correction.h"
#include "light/drivers/Drivers.h"
#include "correction_presets.h"
#include "unit/core/conditional_controls.h"
#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/PanelsLayout.h"

#include <cstring>
#include <memory>
#include <string>
#include "hub75_pi_bench.h"

namespace {

using Rig = mm::PiRig<mm::PioSeam>;

}  // namespace

TEST_CASE("Hub75Driver on a Pi 5 offers Pi PIO and drives it through the PIO output") {
    mm::PioPortGuard guard;
    auto r = Rig::raster(16, 32);
    CHECK(r->driver.selectedBackend() == mm::platform::Hub75Backend::Pio);
    CHECK(std::string(mm::platform::hub75BackendLabel(mm::platform::Hub75Backend::Pio)) == "Pi PIO");
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(r->driver.hwBlock() == mm::LedHwBlock::PiHeader);
}

namespace {

// The driver on the PIO output with the PIO on offer, on the heap, past a test's frame limit.
std::unique_ptr<mm::Hub75Driver> piDriver() {
    auto driver = std::make_unique<mm::Hub75Driver>();
    REQUIRE(pickBackend(*driver, "Pi PIO"));
    return driver;
}

// The options the PIO output would open the seam with, from the driver's settings.
mm::platform::Hub75PioOptions optionsOf(mm::Hub75Driver& driver) {
    return static_cast<mm::Hub75PioOutput*>(driver.outputForTest())->options();
}

}  // namespace

TEST_CASE("Hub75PioOutput starts from the seam's own defaults") {
    mm::PioPortGuard guard;
    const mm::platform::Hub75PioOptions defaults{};
    auto driver = piDriver();
    const auto o = optionsOf(*driver);
    mm::checkWallOptions(o, defaults.rows, defaults.cols, defaults.panelsPerLane, 1, 32, defaults.bitDepth);   // the layout decides the lanes in open(); 64 rows, the default, derive 1/32
    CHECK(o.pixelClockHz == defaults.pixelClockHz);
    CHECK(o.latchBlankWords == defaults.latchBlankWords);
    CHECK(o.clockFallingEdge == defaults.clockFallingEdge);
}

TEST_CASE("Hub75PioOutput derives the scan from the panel's rows: 64 rows is 1/32, 32 rows 1/16, 16 rows 1/8") {
    mm::PioPortGuard guard;
    auto driver = piDriver();
    CHECK(optionsOf(*driver).scanRate == 32);   // 64 rows, the default, is 1/32, so the wall needs no hand setting
    driver->panelRows = 32;
    CHECK(optionsOf(*driver).scanRate == 16);
    driver->panelRows = 16;
    CHECK(optionsOf(*driver).scanRate == 8);
}

TEST_CASE("Hub75Driver on Pi PIO opens at 8-bit when set, and the status names it") {
    mm::PioPortGuard guard;
    auto r = Rig::raster(16, 32);
    r->driver.bitDepth = 8;
    r->open();
    CHECK(mm::platform::hub75PioTestPort().options.bitDepth == 8);
    CHECK(std::string(r->driver.status()) == "16x32, 8-bit, PIO");
}

TEST_CASE("Hub75PioOutput opens the wall with its controls: three lanes of two 128x64 panels at 1/32 scan") {
    mm::PioPortGuard guard;
    auto r = Rig::cabled(2, 3, 128, 64);   // the wall: six 128 by 64 panels
    r->driver.panelsPerLane = 2;
    r->driver.scanSel = 0;   // 1/8: the bus output's select, which the PIO output ignores; the scan derives from rows, so the wall is 1/32 with no hand setting
    r->driver.bitDepth = 4;
    r->driver.clockEdgeSel = 1;   // falling
    r->driver.pixelClockKhz = 20000;
    r->driver.latchBlankWords = 2;
    r->open();

    const auto& o = mm::platform::hub75PioTestPort().options;
    mm::checkWallOptions(o, 64, 128, 2, 3, 32, 4);
    CHECK(o.pixelClockHz == 20000000);
    CHECK(o.latchBlankWords == 2);
    CHECK(o.clockFallingEdge);
    CHECK(std::string(r->driver.status()) == "256x192, 4-bit, PIO");
}

TEST_CASE("Hub75PioOutput reopens the panels on every PIO option and on nothing else") {
    mm::PioPortGuard guard;
    auto r = Rig::raster(16, 32);
    for (const char* n : {"rows", "cols", "panelsPerLane", "bitDepth", "clockEdge",
                          "pixelClock", "latchBlank", "peripheral"}) {
        CAPTURE(n);
        CHECK(r->driver.affectsPrepare(n));
    }
    CHECK_FALSE(r->driver.affectsPrepare("scanRate"));   // no such control on the PIO card: the scan derives from rows
    CHECK_FALSE(r->driver.affectsPrepare("curve"));
    CHECK_FALSE(r->driver.affectsPrepare("localBrightness"));
    CHECK_FALSE(r->driver.affectsPrepare("shown"));
}

TEST_CASE("Hub75PioOutput names why the panels would not open, and stays inert") {
    mm::PioPortGuard guard;
    auto r = Rig::raster(16, 32);
    mm::platform::setTestHub75PioOpenFails(true);   // a fit geometry on a PIO that will not open: the seam's reason is the status
    r->driver.prepare();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()) == "the test seam refuses the open");
    r->driver.tick();   // must not crash, and has no frame to hand over
    CHECK(mm::platform::hub75PioWidth() == 0);
}

TEST_CASE("Hub75PioOutput quotes the rates from the pixel clock") {
    mm::PioPortGuard guard;
    auto r = Rig::cabled(5, 1, 128, 64);   // five 128 by 64 panels, which lanes of 2 cannot cable
    r->driver.panelsPerLane = 2;
    r->open();
    CHECK(std::string(r->driver.status()).find("1 lane of 5 (640x64, 81 Hz)") != std::string::npos);

    r->driver.pixelClockKhz = 20000;   // twice the clock, twice the rate
    r->open();
    CHECK(std::string(r->driver.status()).find("1 lane of 5 (640x64, 163 Hz)") != std::string::npos);
}

TEST_CASE("Hub75PioOutput quotes a narrow panel's rate with the encoder's hold words, the rate the wire runs") {
    mm::PioPortGuard guard;
    auto r = Rig::raster(64, 32);   // one 64 by 32 panel at 8 bits, which lanes of 2 cannot cable
    r->driver.panelRows = 32;
    r->driver.panelCols = 64;
    r->driver.panelsPerLane = 2;
    r->driver.bitDepth = 8;
    r->open();
    CHECK(std::string(r->driver.status()).find("1 lane of 1 (64x32, 606 Hz)") != std::string::npos);   // the shift alone would quote 1,221 Hz
}
