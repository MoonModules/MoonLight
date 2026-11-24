/// @module Hub75GpioOutput
/// @also Hub75PiOutput, Correction

/// The HUB75 driver on a Raspberry Pi 3/4's header, through the GPIO bit-bang engine and its hardware PWM OE pulser, pinned on a desktop, no panels attached. The GPIO seam records what the output opened and handed over (the `hub75Gpio*` seam in platform/Hub75Gpio.h); the bench Pi then only has to show it. What both Pi outputs share is in unit_Hub75PiOutput.cpp.
///
/// @technical
///
/// ## Scan from the rows
///
/// A 1/16 default on 1/32 panels shows black stripes (seen on the Pi 5 wall): the GPIO output has no scan control, and the rows decide it, so 64 rows is 1/32 whatever scanSel says.
///
/// Stripe halves the rows first, so the scan is the driven panel's: a 32-row Stripe panel is 1/8, a 64-row one 1/16.
///
/// ## Slowdown readout
///
/// The card stores the select's INDEX: `kSlowdown2` is slowdown 2 and its stored value is 3, and a reader taking the stored 3 as slowdown 3 misquotes it (BENCH-V6). The slowdownTrue readout quotes the slowdown the engine runs, resolved through the seam, the number a bench note must quote, and goes empty when the panels close.
///
/// ## Quoted rate
///
/// A Stripe wall at slowdown 1 runs 235 ns a column with a 130 ns LSB, so the shift bounds every plane: 6 planes x 16 rows x (1280 cols x 235 ns + 0.5 us dark) is 35 Hz. The canvas in the text stays the logical 640x64.

#include "doctest.h"
#include "light/drivers/Hub75GpioOutput.h"
#include "light/drivers/Correction.h"
#include "light/drivers/Drivers.h"
#include "correction_presets.h"
#include "unit/core/conditional_controls.h"
#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/PanelsLayout.h"
#include "platform/rpi/hub75_gpio_backend.h"   // kGpioTestRefusal: what a closed gate says

#include <cstring>
#include <memory>
#include <string>
#include "hub75_pi_bench.h"

namespace {

using Rig = mm::PiRig<mm::GpioSeam>;
using Output = mm::Hub75GpioOutput;

}  // namespace

TEST_CASE("The HUB75 driver on a Pi 3/4 offers Pi GPIO and drives it through the GPIO output") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(16, 32);
    CHECK(r->driver.selectedBackend() == mm::platform::Hub75Backend::Gpio);
    CHECK(std::string(mm::platform::hub75BackendLabel(mm::platform::Hub75Backend::Gpio)) == "Pi GPIO");
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(r->driver.hwBlock() == mm::LedHwBlock::PiHeader);
}

TEST_CASE("The HUB75 driver on a Pi 3/4 offers Pi GPIO alone, and the Pi 5 offers Pi PIO alone, with every Pi setting kept across the change") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(16, 32);
    CHECK(mm::optionIndex(r->driver, "peripheral", "Pi PIO") == -1);   // an earlier Pi has no RP1
    CHECK(mm::optionIndex(r->driver, "peripheral", "Pi GPIO") >= 0);
    r->driver.panelRows = 32;
    r->driver.panelCols = 64;
    r->driver.panelsPerLane = 1;
    r->driver.bitDepth = 8;

    mm::becomeMachine(r->driver, mm::platform::Machine::Pi5);
    CHECK(r->driver.selectedBackend() == mm::platform::Hub75Backend::Pio);
    CHECK(mm::optionIndex(r->driver, "peripheral", "Pi GPIO") == -1);   // the header belongs to the PIO there
    mm::becomeMachine(r->driver, mm::platform::Machine::PiGpio);
    CHECK(r->driver.selectedBackend() == mm::platform::Hub75Backend::Gpio);
    CHECK(r->driver.panelRows == 32);   // the settings live on the driver, so no output change loses them
    CHECK(r->driver.panelCols == 64);
    CHECK(r->driver.panelsPerLane == 1);
    CHECK(r->driver.bitDepth == 8);
}

TEST_CASE("Hub75GpioOutput starts from the seam's own defaults") {
    mm::GpioPortGuard guard;
    const mm::platform::Hub75GpioOptions defaults{};
    auto bench = std::make_unique<mm::Hub75Bench>();   // a driver on its own defaults, which a Rig sets to its panel
    REQUIRE(mm::pickBackend(bench->driver, "Pi GPIO"));
    auto& output = *static_cast<Output*>(bench->driver.outputForTest());
    const auto o = output.options();
    mm::checkWallOptions(o, defaults.rows, defaults.cols, defaults.panelsPerLane, 1, 32, defaults.bitDepth);   // the layout decides the lanes in open(); 64 rows, the default, derive 1/32
    CHECK(o.multiplexing == defaults.multiplexing);
    CHECK(o.pwmLsbNs == defaults.pwmLsbNs);
    CHECK(o.slowdown == defaults.slowdown);
    CHECK(o.clockFallingEdge == defaults.clockFallingEdge);
    CHECK(output.carryBits == mm::platform::kHub75GpioCarryBitsDefault);
}

TEST_CASE("Hub75GpioOutput opens the wall with its controls: three lanes of two 128x64 panels at 1/32 scan") {
    mm::GpioPortGuard guard;
    auto r = Rig::cabled(2, 3, 128, 64);   // the wall: six 128 by 64 panels
    r->driver.panelsPerLane = 2;
    r->driver.bitDepth = 4;
    r->driver.clockEdgeSel = 1;   // falling
    r->out().pwmLsbNs = 3000;
    r->out().slowdownSel = Output::kSlowdown2;
    r->open();

    const auto& o = mm::GpioSeam::options();
    mm::checkWallOptions(o, 64, 128, 2, 3, 32, 4);
    CHECK(o.pwmLsbNs == 3000);
    CHECK(o.slowdown == 2);
    CHECK(o.clockFallingEdge);
    CHECK(std::string(r->driver.status()) == "256x192, 4-bit, GPIO");
}

TEST_CASE("Hub75GpioOutput hands its multiplexing select to the engine, and reopens when it changes") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(16, 32);
    r->out().multiplexingSel = mm::platform::kHub75MultiplexStripe;   // the sign's panels
    r->open();
    CHECK(mm::GpioSeam::options().multiplexing == mm::platform::kHub75MultiplexStripe);
    CHECK(r->driver.affectsPrepare("multiplexing"));
    CHECK_FALSE(r->driver.affectsPrepare("shown"));
}

TEST_CASE("Hub75GpioOutput offers its multiplexing select in the order of the wirings") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(16, 32);
    CHECK(mm::optionIndex(r->driver, "multiplexing", "normal") == mm::platform::kHub75MultiplexNormal);
    CHECK(mm::optionIndex(r->driver, "multiplexing", "Stripe") == mm::platform::kHub75MultiplexStripe);
}

TEST_CASE("Hub75GpioOutput hands the carry bits to the engine with the brightness, two unless the card says otherwise") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(16, 32);
    mm::showGrayAt(*r, 128, 200);
    CHECK(mm::platform::hub75GpioTestPort().carryBits == 2);

    REQUIRE(mm::setControlValue(r->driver, "carryBits", static_cast<uint8_t>(0)));
    r->paint(0, 100, 100, 100);
    r->driver.tick();
    CHECK(mm::platform::hub75GpioTestPort().carryBits == 0);
}

TEST_CASE("Hub75GpioOutput offers carry bits from 0 to the engine's maximum") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(16, 32);
    const int i = mm::controlIndex(r->driver, "carryBits");
    REQUIRE(i >= 0);
    CHECK(r->driver.controls()[static_cast<uint8_t>(i)].min == 0);
    CHECK(r->driver.controls()[static_cast<uint8_t>(i)].max == mm::platform::kHub75GpioCarryBitsMax);
}

TEST_CASE("Hub75GpioOutput offers the LSB time between the engine's bounds") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(16, 32);
    const int i = mm::controlIndex(r->driver, "pwmLsbNs");
    REQUIRE(i >= 0);
    CHECK(r->driver.controls()[static_cast<uint8_t>(i)].min == mm::platform::kHub75GpioPwmLsbMinNs);
    CHECK(r->driver.controls()[static_cast<uint8_t>(i)].max == mm::platform::kHub75GpioPwmLsbMaxNs);
}

TEST_CASE("Hub75GpioOutput derives the scan from the rows, so the wall cannot come up striped") {
    mm::GpioPortGuard guard;
    // The rows decide the scan, so 64 rows is 1/32 whatever scanSel says (@xref{scan-from-the-rows|Technical → Scan from the rows}).
    auto r = Rig::cabled(2, 3, 128, 64);   // the wall: six 128 by 64 panels
    r->driver.panelsPerLane = 2;
    REQUIRE(r->driver.scanSel == 1);   // the driver's default, 1/16: left untouched
    CHECK(mm::controlIndex(r->driver, "scanRate") < 0);
    r->open();
    CHECK(mm::GpioSeam::options().scanRate == 32);

    auto s = Rig::raster(16, 32);   // a 32-row panel is 1/16
    s->open();
    CHECK(mm::GpioSeam::options().scanRate == 16);
}

TEST_CASE("Hub75GpioOutput derives the Stripe scan from the rows: rows/4, the driven panel's scan") {
    mm::GpioPortGuard guard;
    // Stripe halves the rows first, so the scan is the driven panel's (@xref{scan-from-the-rows|Technical → Scan from the rows}).
    auto s = Rig::raster(16, 32);
    s->out().multiplexingSel = mm::platform::kHub75MultiplexStripe;
    s->open();
    CHECK(mm::GpioSeam::options().scanRate == 8);
    CHECK(mm::GpioSeam::options().multiplexing == mm::platform::kHub75MultiplexStripe);

    auto w = Rig::cabled(2, 3, 128, 64);   // the wall's panels, Stripe wired
    w->driver.panelsPerLane = 2;
    w->out().multiplexingSel = mm::platform::kHub75MultiplexStripe;
    w->open();
    CHECK(mm::GpioSeam::options().scanRate == 16);
}

TEST_CASE("Hub75GpioOutput refuses a Stripe panel of 16 rows, whose quarter is no scan, and takes 32 and 64") {
    mm::GpioPortGuard guard;
    auto refused = Rig::raster(24, 16);
    refused->driver.panelRows = 16;
    refused->driver.panelCols = 24;
    refused->out().multiplexingSel = mm::platform::kHub75MultiplexStripe;
    refused->open();
    CHECK(refused->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(refused->driver.status()) == "the Stripe scan is a quarter of the panel's rows, so rows must be 32 or 64");

    for (const uint8_t rows : {uint8_t{32}, uint8_t{64}}) {
        CAPTURE(static_cast<int>(rows));
        auto r = Rig::raster(24, rows);
        r->driver.panelRows = rows;
        r->driver.panelCols = 24;
        r->out().multiplexingSel = mm::platform::kHub75MultiplexStripe;
        r->open();
        CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
    }
}

TEST_CASE("Hub75GpioOutput opens the sign: three 64x32 Stripe panels, one to a lane, 64x96") {
    mm::GpioPortGuard guard;
    auto r = Rig::cabled(1, 3, 64, 32);   // the sign: three 64 by 32 panels
    r->driver.panelsPerLane = 1;
    r->out().multiplexingSel = mm::platform::kHub75MultiplexStripe;   // the sign's panels
    r->out().pwmLsbNs = 160;
    r->out().slowdownSel = Output::kSlowdown2;   // the sign's setting: the card stores the index, slowdown is index - 1
    r->open();
    const auto& o = mm::GpioSeam::options();
    CHECK(o.rows == 32);
    CHECK(o.cols == 64);
    CHECK(o.panelsPerLane == 1);
    CHECK(o.parallel == 3);
    CHECK(o.scanRate == 8);
    CHECK(o.multiplexing == mm::platform::kHub75MultiplexStripe);
    CHECK(o.pwmLsbNs == 160);
    CHECK(o.slowdown == 2);
    CHECK(std::string(r->driver.status()) == "64x96, 6-bit, GPIO");
}

TEST_CASE("Hub75GpioOutput publishes the resolved slowdown, not the stored index") {
    mm::GpioPortGuard guard;
    // The card stores the select's INDEX, and slowdownTrue quotes the slowdown the engine runs (@xref{slowdown-readout|Technical → Slowdown readout}).
    auto r = Rig::raster(16, 32);
    r->out().slowdownSel = Output::kSlowdown2;   // stored 3: slowdown 2
    r->open();
    CHECK(mm::textOf(r->driver, "slowdownTrue") == "2");

    r->out().slowdownSel = Output::kSlowdownAuto;   // auto: the resolved value, whatever this host's model takes
    r->open();
    CHECK(mm::textOf(r->driver, "slowdownTrue") == std::to_string(mm::platform::hub75GpioResolvedSlowdown(-1)));

    r->driver.release();
    CHECK(mm::textOf(r->driver, "slowdownTrue").empty());
}

TEST_CASE("Hub75GpioOutput resolves auto slowdown by the host's model: 2 on a Pi 4, 1 on a Pi 3") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(16, 32);
    r->out().slowdownSel = Output::kSlowdownAuto;
    {
        mm::PiSocGuard pi4(mm::platform::PiSoc::Bcm2711);
        r->open();
        CHECK(mm::textOf(r->driver, "slowdownTrue") == "2");
        CHECK(mm::GpioSeam::options().slowdown == -1);   // the seam is asked for auto, and resolves it
    }
    {
        mm::PiSocGuard pi3(mm::platform::PiSoc::Bcm2837);
        r->open();
        CHECK(mm::textOf(r->driver, "slowdownTrue") == "1");
    }
}

TEST_CASE("Hub75GpioOutput reopens the panels on every GPIO option but carryBits, and on nothing else") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(16, 32);
    for (const char* n : {"rows", "cols", "panelsPerLane", "multiplexing", "bitDepth", "clockEdge",
                          "pwmLsbNs", "slowdown", "peripheral"}) {
        CAPTURE(n);
        CHECK(r->driver.affectsPrepare(n));
    }
    CHECK_FALSE(r->driver.affectsPrepare("scanRate"));   // no such control: the scan follows the rows, and rows reopens
    CHECK_FALSE(r->driver.affectsPrepare("curve"));
    CHECK_FALSE(r->driver.affectsPrepare("localBrightness"));
    CHECK_FALSE(r->driver.affectsPrepare("carryBits"));   // applies on the next frame, through the seam
    CHECK_FALSE(r->driver.affectsPrepare("shown"));
    CHECK_FALSE(r->driver.affectsPrepare("slowdownTrue"));   // a readout, like shown: it follows the slowdown's reopen
}

TEST_CASE("Hub75GpioOutput names why the panels would not open, and stays inert") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(16, 32);
    mm::platform::setTestHub75GpioOpenFails(true);   // a fit geometry on a seam that will not open: its reason is the status
    r->driver.prepare();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()) == "the test seam refuses the open");
    r->driver.tick();   // must not crash, and has no frame to hand over
    CHECK(mm::platform::hub75GpioWidth() == 0);
}

TEST_CASE("Hub75GpioOutput names why a host with no Pi hardware refuses, since the test binary keeps the real backend closed") {
    mm::GpioPortGuard guard(mm::platform::Hub75GpioMode::Off);
    auto r = Rig::raster(16, 32);
    r->driver.prepare();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()) == mm::platform::kGpioTestRefusal);
    r->driver.tick();
    CHECK(mm::platform::hub75GpioWidth() == 0);
}

TEST_CASE("Hub75GpioOutput quotes the rates from the column time, the LSB and the slowdown") {
    mm::GpioPortGuard guard;
    auto r = Rig::cabled(5, 1, 128, 64);   // five 128 by 64 panels, which lanes of 2 cannot cable
    r->driver.panelsPerLane = 2;
    r->out().slowdownSel = Output::kSlowdown1;
    r->open();
    // 235 ns a column at slowdown 1, 130 ns LSB: the shift bounds every plane.
    CHECK(std::string(r->driver.status()).find("1 lane of 5 (640x64, 35 Hz)") != std::string::npos);

    r->out().pwmLsbNs = 3000;   // the longest LSB: the top plane (96 us) is shorter than the shift (168 us), so the rate holds
    r->open();
    CHECK(std::string(r->driver.status()).find("1 lane of 5 (640x64, 35 Hz)") != std::string::npos);

    r->out().slowdownSel = Output::kSlowdown4;   // 271 ns a column: the shift lengthens again, past every plane
    r->open();
    CHECK(std::string(r->driver.status()).find("1 lane of 5 (640x64, 30 Hz)") != std::string::npos);

    r->out().pwmLsbNs = 130;   // back to the default LSB, slowdown 4 alone
    r->open();
    CHECK(std::string(r->driver.status()).find("1 lane of 5 (640x64, 30 Hz)") != std::string::npos);
}

TEST_CASE("Hub75GpioOutput quotes the Stripe rate from the driven width and the driven scan") {
    mm::GpioPortGuard guard;
    auto r = Rig::cabled(5, 1, 128, 64);   // five 128 by 64 panels, which lanes of 2 cannot cable
    r->driver.panelsPerLane = 2;
    r->out().slowdownSel = Output::kSlowdown1;
    r->out().multiplexingSel = mm::platform::kHub75MultiplexStripe;   // 16 driven steps of 256 columns a row
    r->open();
    // 235 ns a column at slowdown 1, 130 ns LSB: the shift bounds every plane, 35 Hz (@xref{quoted-rate|Technical → Quoted rate}).
    CHECK(std::string(r->driver.status()).find("1 lane of 5 (640x64, 35 Hz)") != std::string::npos);
}

TEST_CASE("Hub75GpioOutput quotes the dark interval as the seam names it, so the rate counts the address and latch time") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(64, 32);   // one 64 by 32 panel, which lanes of 2 cannot cable
    r->driver.panelRows = 32;
    r->driver.panelCols = 64;
    r->driver.panelsPerLane = 2;
    r->out().slowdownSel = Output::kSlowdown1;
    r->driver.bitDepth = 2;
    r->open();
    // 2 planes x 16 rows x (64 cols x 235 ns + 0.5 us dark) = 497,280 ns: 2,011 Hz, the engine's own prediction (unit_platform_hub75gpio_engine.cpp pins it from the engine).
    CHECK(std::string(r->driver.status()).find("1 lane of 1 (64x32, 2011 Hz)") != std::string::npos);
}

TEST_CASE("Hub75GpioOutput shows the frames the GPIO shows each second, and nothing once released") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(16, 32);
    r->open();
    mm::GpioSeam::setFps(61);
    r->driver.tick1s();
    CHECK(mm::textOf(r->driver, "shown") == "61 fps");
    r->driver.release();
    CHECK(mm::textOf(r->driver, "shown").empty());
}

TEST_CASE("Hub75GpioOutput warns, in the GPIO's name, while its dumps fail or stall, and says nothing once they stop") {
    mm::GpioPortGuard guard;
    auto r = Rig::raster(16, 32);
    r->open();
    r->driver.tick1s();   // the count the seam starts at

    mm::GpioSeam::setSubmitErrors(3);
    r->driver.tick1s();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()).find("the GPIO is failing: 3 submits failed") != std::string::npos);
    CHECK(std::string(r->driver.status()).find("the PIO") == std::string::npos);   // no other seam's name in the text

    r->driver.tick1s();   // no new failures in the last second
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);

    mm::GpioSeam::setStalled(true);
    r->driver.tick1s();
    CHECK(std::string(r->driver.status()).find("the GPIO is stalled: a submit has not returned") != std::string::npos);
}
