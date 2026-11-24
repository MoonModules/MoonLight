/// @module platform

/// Pins the GPIO seam the Pi 3/4 GPIO+PWM engine sits behind, in each of its modes. Record stands in for the panels, so what a driver hands over is checked on every host; Stub runs the real engine on the stub backend through the seam's real branch, so the push, the rate and the close are checked end to end with no Pi; the bench Pi then only has to show the wire.
///
/// The refusal paths branch on the hardware build: a build with the hardware backend refuses a host that is no Pi 3 or 4 by its model, any other build through the missing backend. A test binary keeps the real backend out of reach whatever the host is.

#include "doctest.h"
#include "hub75_engine_bench.h"
#include "platform/Hub75Gpio.h"
#include "platform/rpi/hub75_gpio_backend.h"
#include "platform/rpi/hub75_gpio_backend_stub.h"
#include "unit/light/hub75_port.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

using mm::platform::Hub75GpioMode;

// The bench wall: 128x64 panels, two in series on each of three lanes.
mm::platform::Hub75GpioOptions benchWall() {
    mm::platform::Hub75GpioOptions o;
    o.rows = 64;
    o.cols = 128;
    o.panelsPerLane = 2;
    o.parallel = 3;
    return o;
}

}  // namespace

TEST_CASE("Hub75Gpio: the canvas is the lane across and the parallel lanes down") {
    mm::GpioPortGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    CHECK(mm::platform::hub75GpioWidth() == 256);
    CHECK(mm::platform::hub75GpioHeight() == 192);
    CHECK(mm::platform::hub75GpioLastError() == nullptr);
    CHECK(mm::platform::hub75GpioWarning() == nullptr);
}

TEST_CASE("Hub75Gpio: an open keeps the options") {
    mm::GpioPortGuard guard;
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.panelsPerLane = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 2;
    o.pwmLsbNs = 50;
    o.slowdown = 2;
    o.clockFallingEdge = true;
    REQUIRE(mm::platform::hub75GpioOpen(o));
    const auto& kept = mm::platform::hub75GpioTestPort().options;
    mm::checkWallOptions(kept, 32, 64, 1, 1, 16, 2);
    CHECK(kept.pwmLsbNs == 50);
    CHECK(kept.slowdown == 2);
    CHECK(kept.clockFallingEdge);
    CHECK(mm::platform::hub75GpioWidth() == 64);
    CHECK(mm::platform::hub75GpioHeight() == 32);
}

TEST_CASE("Hub75Gpio: a pushed frame arrives byte for byte") {
    mm::GpioPortGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3);
    for (size_t i = 0; i < frame.size(); i++) frame[i] = static_cast<uint8_t>(i * 7);
    mm::platform::hub75GpioPush(frame.data());
    REQUIRE(mm::platform::hub75GpioTestPort().frames == 1);
    CHECK(std::memcmp(mm::platform::hub75GpioTestFrame(), frame.data(), frame.size()) == 0);
}

TEST_CASE("Hub75Gpio: the brightness and the carry bits are handed over as given, brightness 0 and bits 0 included") {
    mm::GpioPortGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    mm::platform::hub75GpioCorrection(0, 3);
    CHECK(mm::platform::hub75GpioTestPort().brightness == 0);
    CHECK(mm::platform::hub75GpioTestPort().carryBits == 3);
    mm::platform::hub75GpioCorrection(200, 0);
    CHECK(mm::platform::hub75GpioTestPort().brightness == 200);
    CHECK(mm::platform::hub75GpioTestPort().carryBits == 0);
}

TEST_CASE("Hub75Gpio: the recorder's rate is 0 until a test sets it, holds while open, and ends with the close") {
    mm::GpioPortGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3, 0);
    mm::platform::hub75GpioPush(frame.data());
    CHECK(mm::platform::hub75GpioFps() == 0);
    mm::platform::setTestHub75GpioFps(60);
    CHECK(mm::platform::hub75GpioFps() == 60);
    mm::platform::hub75GpioClose();
    CHECK(mm::platform::hub75GpioFps() == 0);
    mm::platform::setTestHub75Gpio(Hub75GpioMode::Record);   // setting the mode again puts the levers back
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    CHECK(mm::platform::hub75GpioFps() == 0);
}

TEST_CASE("Hub75Gpio: the recorder's stall and failed dumps are 0 until a test sets them, and end with the close") {
    mm::GpioPortGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    CHECK_FALSE(mm::platform::hub75GpioStalled());
    CHECK(mm::platform::hub75GpioSubmitErrors() == 0);
    mm::platform::setTestHub75GpioStalled(true);
    mm::platform::setTestHub75GpioSubmitErrors(4);
    CHECK(mm::platform::hub75GpioStalled());
    CHECK(mm::platform::hub75GpioSubmitErrors() == 4);
    mm::platform::hub75GpioClose();
    CHECK_FALSE(mm::platform::hub75GpioStalled());
    CHECK(mm::platform::hub75GpioSubmitErrors() == 0);
}

TEST_CASE("Hub75Gpio: an open outside a test mode is refused in the test binary before /dev/mem is reached") {
    mm::platform::setTestHub75Gpio(Hub75GpioMode::Off);
    const uint32_t attempts = mm::platform::hub75GpioTestRealAttempts();
    mm::PiSocGuard pi4(mm::platform::PiSoc::Bcm2711);   // a host that would serve the GPIO, where the hardware backend is built
    CHECK_FALSE(mm::platform::hub75GpioOpen(benchWall()));
    REQUIRE(mm::platform::hub75GpioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75GpioLastError()) == mm::platform::kGpioTestRefusal);   // the closed gate says why
    CHECK(mm::platform::hub75GpioTestRealAttempts() == attempts + 1);   // asked for, and stopped at the gate
    CHECK(mm::platform::hub75GpioTestRealOpens() == 0);                 // none reached the backend
    CHECK(mm::platform::hub75GpioWidth() == 0);
}

#ifndef MM_HUB75_GPIO_HAVE_HW
TEST_CASE("Hub75Gpio: without the hardware backend an open outside a test mode fails naming the build") {
    mm::platform::setTestHub75Gpio(Hub75GpioMode::Off);
    mm::platform::setTestHub75GpioGateOpen(true);   // the build's own refusal is the case; the hardware backend is absent, so the real backend stays out of reach
    CHECK_FALSE(mm::platform::hub75GpioOpen(benchWall()));
    REQUIRE(mm::platform::hub75GpioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75GpioLastError()) == mm::platform::kGpioNoBackendRefusal);
    CHECK(mm::platform::hub75GpioWidth() == 0);
}

TEST_CASE("Hub75Gpio: without the hardware backend a Pi 4 is not offered the output, whatever the host says") {
    mm::platform::setTestHub75Gpio(Hub75GpioMode::Off);
    mm::PiSocGuard pi4(mm::platform::PiSoc::Bcm2711);   // a host that would serve the GPIO if the build could drive it
    CHECK_FALSE(mm::platform::hub75GpioAvailable());
}
#endif

#ifdef MM_HUB75_GPIO_HAVE_HW
TEST_CASE("Hub75Gpio: with the hardware backend the host gates availability: a Pi 3 or 4 is offered the backend, a Pi 5 and another board are not") {
    mm::platform::setTestHub75Gpio(Hub75GpioMode::Off);
    {
        mm::PiSocGuard pi4(mm::platform::PiSoc::Bcm2711);
        CHECK(mm::platform::hub75GpioAvailable());
    }
    {
        mm::PiSocGuard pi3(mm::platform::PiSoc::Bcm2837);
        CHECK(mm::platform::hub75GpioAvailable());
    }
    {
        mm::MachineGuard pi5(mm::platform::Machine::Pi5);   // its header belongs to the PIO output
        CHECK_FALSE(mm::platform::hub75GpioAvailable());
    }
    {
        mm::PiSocGuard other(mm::platform::PiSoc::Other);
        CHECK_FALSE(mm::platform::hub75GpioAvailable());
    }
}
#endif

TEST_CASE("Hub75Gpio: the refusal names why a host is no Pi 3 or 4, a Pi 5 by the output it takes instead") {
    mm::PiSocGuard other(mm::platform::PiSoc::Other);
    REQUIRE(mm::platform::hub75GpioRefusal() != nullptr);
#ifdef MM_HUB75_GPIO_HAS_ENGINE
    CHECK(std::string(mm::platform::hub75GpioRefusal()) == mm::platform::kGpioNotAPiRefusal);
    {
        mm::MachineGuard pi5(mm::platform::Machine::Pi5);
        CHECK(std::string(mm::platform::hub75GpioRefusal()) == mm::platform::kGpioPi5Refusal);
    }
    {
        mm::PiSocGuard pi4(mm::platform::PiSoc::Bcm2711);
        CHECK(mm::platform::hub75GpioRefusal() == nullptr);
    }
    {
        mm::PiSocGuard pi3(mm::platform::PiSoc::Bcm2837);
        CHECK(mm::platform::hub75GpioRefusal() == nullptr);
    }
#endif
}

TEST_CASE("Hub75Gpio: every test mode offers the backend on every host, and Off leaves it to the host") {
    {
        mm::GpioPortGuard record(Hub75GpioMode::Record);
        CHECK(mm::platform::hub75GpioAvailable());
    }
    {
        mm::GpioPortGuard stub(Hub75GpioMode::Stub);
        CHECK(mm::platform::hub75GpioAvailable());
    }
#ifndef MM_HUB75_GPIO_HAVE_HW
    CHECK_FALSE(mm::platform::hub75GpioAvailable());   // off, the host decides: no hardware backend, not offered
#endif
}

TEST_CASE("Hub75Gpio: the test hook forces availability either way, and the host decides again after it") {
    mm::platform::setTestHub75Gpio(Hub75GpioMode::Off);
    {
        mm::GpioAvailable off(0);
        CHECK_FALSE(mm::platform::hub75GpioAvailable());
    }
    {
        mm::GpioAvailable on(1);
        CHECK(mm::platform::hub75GpioAvailable());
    }
#ifndef MM_HUB75_GPIO_HAVE_HW
    CHECK_FALSE(mm::platform::hub75GpioAvailable());   // after the guards the host decides again: no hardware backend, not offered
#endif
}

TEST_CASE("Hub75Gpio: the seam resolves auto slowdown by the host's model, the same answer the engine runs, and passes a chosen one through") {
    // Auto is the per-model default: 2 on a Pi 4, 1 on a Pi 3 and everywhere else.
    {
        mm::PiSocGuard pi4(mm::platform::PiSoc::Bcm2711);
        CHECK(mm::platform::hub75GpioResolvedSlowdown(-1) == 2);
    }
    {
        mm::PiSocGuard pi3(mm::platform::PiSoc::Bcm2837);
        CHECK(mm::platform::hub75GpioResolvedSlowdown(-1) == 1);
    }
    {
        mm::PiSocGuard other(mm::platform::PiSoc::Other);
        CHECK(mm::platform::hub75GpioResolvedSlowdown(-1) == 1);
    }
    CHECK(mm::platform::hub75GpioResolvedSlowdown(3) == 3);
    CHECK(mm::platform::hub75GpioResolvedSlowdown(0) == 0);
}

TEST_CASE("Hub75Gpio: the seam quotes the column time the engine runs, through the model") {
    CHECK(mm::platform::hub75GpioPredictedColumnTimeNs(1) == mm::platform::hub75GpioColumnTimeNs(1));
    CHECK(mm::platform::hub75GpioPredictedColumnTimeNs(2) == 247);
}

TEST_CASE("Hub75Gpio: the seam answers the scan and the driven width a wiring gives, so the output asks it and not the engine's table") {
    using mm::platform::hub75GpioDrivenCols;
    using mm::platform::hub75GpioRowsDriveScan;
    using mm::platform::hub75GpioScanRate;
    CHECK(hub75GpioScanRate(mm::platform::kHub75MultiplexNormal, 64) == 32);
    CHECK(hub75GpioScanRate(mm::platform::kHub75MultiplexNormal, 32) == 16);
    CHECK(hub75GpioScanRate(mm::platform::kHub75MultiplexStripe, 32) == 8);
    CHECK(hub75GpioScanRate(mm::platform::kHub75MultiplexStripe, 64) == 16);
    CHECK(hub75GpioDrivenCols(mm::platform::kHub75MultiplexNormal, 64) == 64);
    CHECK(hub75GpioDrivenCols(mm::platform::kHub75MultiplexStripe, 64) == 128);
    CHECK(hub75GpioDrivenCols(mm::platform::kHub75MultiplexStripe, 40000) == 80000);   // past 16 bits, as a width of its own
    for (const uint8_t rows : {uint8_t{16}, uint8_t{32}, uint8_t{64}}) CHECK(hub75GpioRowsDriveScan(mm::platform::kHub75MultiplexNormal, rows));
    for (const uint8_t rows : {uint8_t{8}, uint8_t{17}, uint8_t{33}, uint8_t{40}}) CHECK_FALSE(hub75GpioRowsDriveScan(mm::platform::kHub75MultiplexNormal, rows));
    CHECK(hub75GpioRowsDriveScan(mm::platform::kHub75MultiplexStripe, 32));
    CHECK(hub75GpioRowsDriveScan(mm::platform::kHub75MultiplexStripe, 64));
    CHECK_FALSE(hub75GpioRowsDriveScan(mm::platform::kHub75MultiplexStripe, 16));
}

TEST_CASE("Hub75Gpio: a recorded open refuses what the engine would refuse, with its reason") {
    mm::GpioPortGuard guard;
    // A 32-high panel at 1/32 scan: the encoder needs an even rows-per-scan.
    mm::platform::Hub75GpioOptions o = benchWall();
    o.rows = 32;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    REQUIRE(mm::platform::hub75GpioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75GpioLastError()).find("32-high at 1/16") != std::string::npos);
    CHECK(mm::platform::hub75GpioWidth() == 0);
    CHECK(mm::platform::hub75GpioHeight() == 0);

    o = benchWall();
    o.panelsPerLane = 0;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    o = benchWall();
    o.parallel = 0;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    o = benchWall();
    o.bitDepth = 9;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    // 8 opens: the GPIO output goes to 8 real planes, the encoder's limit.
    o = benchWall();
    o.bitDepth = 8;
    CHECK(mm::platform::hub75GpioOpen(o));
    mm::platform::hub75GpioClose();
    o = benchWall();
    o.pwmLsbNs = 10;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    o = benchWall();
    o.pwmLsbNs = 3001;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    o = benchWall();
    o.slowdown = 5;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    o = benchWall();
    o.scanRate = 64;
    CHECK_FALSE(mm::platform::hub75GpioOpen(o));
    CHECK(mm::platform::hub75GpioWidth() == 0);
    CHECK(mm::platform::hub75GpioHeight() == 0);
}

TEST_CASE("Hub75Gpio: after close there is no canvas, and a push is ignored") {
    mm::GpioPortGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    mm::platform::hub75GpioClose();
    CHECK(mm::platform::hub75GpioWidth() == 0);
    CHECK(mm::platform::hub75GpioHeight() == 0);
    const uint8_t pixel[3] = {1, 2, 3};
    mm::platform::hub75GpioPush(pixel);
    CHECK(mm::platform::hub75GpioTestPort().frames == 0);
    CHECK(mm::platform::hub75GpioFps() == 0);
}

TEST_CASE("Hub75Gpio: a null frame is ignored") {
    mm::GpioPortGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    mm::platform::hub75GpioPush(nullptr);
    CHECK(mm::platform::hub75GpioTestPort().frames == 0);
}

TEST_CASE("Hub75Gpio: the newest of several pushes is the one shown, whole, until the next") {
    mm::GpioPortGuard guard;
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3);
    for (uint8_t v : {1, 2, 3}) {   // three frames faster than the panels take them
        std::fill(frame.begin(), frame.end(), v);
        mm::platform::hub75GpioPush(frame.data());
    }
    const uint8_t* shown = mm::platform::hub75GpioTestFrame();
    REQUIRE(shown != nullptr);
    CHECK(std::count(shown, shown + frame.size(), uint8_t{3}) == static_cast<long>(frame.size()));
    CHECK(mm::platform::hub75GpioTestFrame() == shown);   // no new push, so the same frame stays
}

TEST_CASE("Hub75Gpio: the seam counts its opens and its closes, an open that closes another included") {
    mm::GpioPortGuard guard;
    CHECK(mm::platform::hub75GpioTestPort().opens == 0);
    mm::platform::hub75GpioClose();   // the panels are closed: no close to count
    CHECK(mm::platform::hub75GpioTestPort().closes == 0);
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));   // MoonLight re-opens on every setting change, and the open closes the first
    CHECK(mm::platform::hub75GpioTestPort().opens == 2);
    CHECK(mm::platform::hub75GpioTestPort().closes == 1);
    CHECK(mm::platform::hub75GpioWidth() == 256);
    CHECK(mm::platform::hub75GpioHeight() == 192);
    mm::platform::hub75GpioClose();
    CHECK(mm::platform::hub75GpioTestPort().closes == 2);
    mm::platform::Hub75GpioOptions refused = benchWall();
    refused.rows = 0;
    CHECK_FALSE(mm::platform::hub75GpioOpen(refused));   // a refused open counts no open
    CHECK(mm::platform::hub75GpioTestPort().opens == 2);
}

TEST_CASE("Hub75Gpio: the open lever refuses the open in either mode, and setting the mode lifts it") {
    for (Hub75GpioMode mode : {Hub75GpioMode::Record, Hub75GpioMode::Stub}) {
        mm::GpioPortGuard guard(mode);
        mm::platform::setTestHub75GpioOpenFails(true);
        CHECK_FALSE(mm::platform::hub75GpioOpen(benchWall()));
        REQUIRE(mm::platform::hub75GpioLastError() != nullptr);
        CHECK(mm::platform::hub75GpioWidth() == 0);
        CHECK(mm::platform::hub75GpioTestPort().opens == 0);
        mm::platform::setTestHub75Gpio(mode);
        CHECK(mm::platform::hub75GpioOpen(benchWall()));
    }
}

#ifdef MM_HUB75_GPIO_HAS_ENGINE
TEST_CASE("Hub75GpioStub: the seam opens the real engine on the stub, and the recorder holds nothing") {
    mm::GpioPortGuard guard(Hub75GpioMode::Stub);
    REQUIRE(mm::platform::hub75GpioOpen(benchWall()));
    const auto& port = mm::platform::hub75GpioTestPort();
    CHECK(port.engineHoldsPanels);
    CHECK(port.opens == 1);
    CHECK(mm::platform::hub75GpioWidth() == 256);
    CHECK(mm::platform::hub75GpioHeight() == 192);
    CHECK(mm::test::waitUntil([] { return mm::platform::gpioStubSubmittedFrames() > 0; }));   // the engine's refresh thread runs
    CHECK(mm::platform::hub75GpioTestFrame() == nullptr);   // the recorder keeps no frame: the engine took it
}

TEST_CASE("Hub75GpioStub: a pushed frame reaches the stub as its encode, and the close leaves a blank") {
    mm::GpioPortGuard guard(Hub75GpioMode::Stub);
    REQUIRE(mm::platform::hub75GpioOpen(mm::test::gpioSmall()));
    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[0] = 128;   // pixel (0,0), red at half: plane 1 lit
    mm::platform::hub75GpioPush(frame.data());
    CHECK(mm::platform::hub75GpioTestPort().frames == 1);
    // Plane 1, row 0, column 0: slot 1024. R1 of lane 0 is GPIO 11, and no control bit reaches the word.
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::test::waitUntil([&] {
        return mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048 && last[1024] == 0x800u;
    }));
    mm::platform::hub75GpioClose();
    CHECK_FALSE(mm::platform::hub75GpioTestPort().engineHoldsPanels);
    CHECK(mm::platform::hub75GpioTestPort().frames == 0);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    for (uint32_t w : last) CHECK(w == 0u);   // every color bit clear
}

TEST_CASE("Hub75GpioStub: the warning for a refused realtime request is there right after the open") {
    mm::GpioPortGuard guard(Hub75GpioMode::Stub);
    for (int i = 0; i < 20; i++) {
        REQUIRE(mm::platform::hub75GpioOpen(mm::test::gpioSmall()));
        const char* warning = mm::platform::hub75GpioWarning();
#ifndef __linux__
        REQUIRE(warning != nullptr);   // no realtime scheduling off Linux, so every open warns
#endif
        if (warning != nullptr) CHECK(std::string(warning).find("real-time priority") != std::string::npos);
        mm::platform::hub75GpioClose();
    }
}

TEST_CASE("Hub75GpioStub: a push after the close never reaches the engine") {
    mm::GpioPortGuard guard(Hub75GpioMode::Stub);
    REQUIRE(mm::platform::hub75GpioOpen(mm::test::gpioSmall()));
    mm::platform::hub75GpioClose();
    std::vector<uint8_t> frame(64 * 32 * 3, 255);
    mm::platform::hub75GpioPush(frame.data());
    CHECK(mm::platform::hub75GpioTestPort().frames == 0);
}

TEST_CASE("Hub75GpioStub: the engine's rate, stall and failed dumps answer through the seam, 0 once closed") {
    mm::GpioPortGuard guard(Hub75GpioMode::Stub);
    REQUIRE(mm::platform::hub75GpioOpen(mm::test::gpioSmall()));
    CHECK(mm::test::waitUntil([] { return mm::platform::hub75GpioFps() > 0; }, 4000));   // a rate window is a second
    CHECK_FALSE(mm::platform::hub75GpioStalled());
    CHECK(mm::platform::hub75GpioSubmitErrors() == 0);
    mm::platform::hub75GpioClose();
    CHECK(mm::platform::hub75GpioFps() == 0);
}

TEST_CASE("Hub75GpioStub: the correction set before the open lights the first frame, and a later one lights the next") {
    mm::GpioPortGuard guard(Hub75GpioMode::Stub);
    mm::platform::hub75GpioCorrection(128, 0);   // half brightness, no carry: 200 x 128 / 255 is 1.57 levels of 64, shown as 1
    REQUIRE(mm::platform::hub75GpioOpen(mm::test::gpioSmall()));
    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[0] = 200;   // pixel (0,0), red
    mm::platform::hub75GpioPush(frame.data());
    // Level 1 of the 2-bit depth: plane 0 lit (slot 0), plane 1 dark (slot 1024), on lane 0's R1, GPIO 11.
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::test::waitUntil([&] { return mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048 && last[0] == 0x800u; }));
    CHECK(last[1024] == 0u);
    mm::platform::hub75GpioCorrection(255, 0);   // full brightness: 200 / 64 is 3.1, shown as 3, both planes lit
    REQUIRE(mm::test::waitUntil([&] { return mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048 && last[1024] == 0x800u; }));
    CHECK(last[0] == 0x800u);
}

TEST_CASE("Hub75GpioStub: the encode lever fails the engine's encodes, and the last good frame keeps showing") {
    mm::GpioPortGuard guard(Hub75GpioMode::Stub);
    REQUIRE(mm::platform::hub75GpioOpen(mm::test::gpioSmall()));
    mm::platform::setTestHub75GpioEncodeFails(-1);
    std::vector<uint8_t> frame(64 * 32 * 3, 255);
    mm::platform::hub75GpioPush(frame.data());
    const uint64_t submitted = mm::platform::gpioStubSubmittedFrames();
    CHECK(mm::test::waitUntil([&] { return mm::platform::gpioStubSubmittedFrames() > submitted + 3; }));   // the held frame keeps submitting
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    for (uint32_t w : last) CHECK(w == 0u);   // the white frame fails to encode, so the black one from the open stays
}
#endif
