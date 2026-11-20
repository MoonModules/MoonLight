/// @module platform

/// Pins the PIO seam the RP1 state machine sits behind, in each of its modes. Record stands in for the panels, so what a driver hands over is checked on every host; Stub runs the real engine on the stub backend through the seam's real branch, so the push, the rate and the close are checked end to end with no Pi; the bench Pi then only has to show the wire.
///
/// The refusal paths branch on the piolib build: a build with piolib linked refuses through the kernel check, any other build through the missing library. Availability is host-gated separately: with piolib, a Pi 5 is offered the backend and a Pi 4 is not.

#include "doctest.h"
#include "hub75_engine_bench.h"
#include "platform/Hub75Pio.h"
#include "platform/rpi/hub75_pio_backend_stub.h"
#include "platform/rpi/hub75_pio_pins.h"
#include "unit/light/hub75_port.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using mm::platform::Hub75PioMode;

// The bench wall: 128x64 panels, two in series on each of three lanes.
mm::platform::Hub75PioOptions benchWall() {
    mm::platform::Hub75PioOptions o;
    o.rows = 64;
    o.cols = 128;
    o.panelsPerLane = 2;
    o.parallel = 3;
    return o;
}

// Opens the small wall on the stub with a 100 ms join deadline and waits for its first frame, as the cases of a close that meets a stuck thread begin.
void openSmallWithShortDeadline() {
    mm::platform::setTestHub75PioJoinDeadlineMs(100);
    REQUIRE(mm::platform::hub75PioOpen(mm::test::pioSmall()));
    REQUIRE(mm::test::waitUntil([] { return mm::platform::stubBackendSubmittedFrames() > 0; }));
}

// The PIO check's reason on a Pi 5 running `release`, or empty when PIO serves there.
std::string pioOn(const char* release) {
    mm::PioHost host(release, true);
    const char* why = mm::platform::hub75PioRefusal();
    return why ? why : "";
}

}  // namespace

TEST_CASE("Hub75Pio: PIO is refused on a kernel from 6.18.39 up to 6.18.60, which stalls its reopen") {
    for (const char* release : {"6.18.39", "6.18.50+rpt-rpi-2712", "6.18.59"}) {
        CAPTURE(release);
        CHECK(pioOn(release).find("stalls a PIO reopen") != std::string::npos);
    }
}

TEST_CASE("Hub75Pio: PIO serves on 6.18.60 and later, and on a kernel before 6.18.39") {
    for (const char* release : {"6.18.60+rpt-rpi-2712", "6.19.2", "6.18.38", "6.12.47+rpt-rpi-2712", "7.0.1"}) {
        CAPTURE(release);
        CHECK(pioOn(release).empty());
    }
}

TEST_CASE("Hub75Pio: PIO is refused without /dev/pio0, whatever the kernel") {
    // A Pi 3 or 4 has no RP1, and a kernel that lacks its PIO driver makes no device.
    mm::PioHost host("6.18.60+rpt-rpi-2712", false);
    REQUIRE(mm::platform::hub75PioRefusal() != nullptr);
    CHECK(std::string(mm::platform::hub75PioRefusal()) == "no /dev/pio0 on this system");
}

TEST_CASE("Hub75Pio: a kernel release that does not read as a version leaves PIO to the device") {
    CHECK(pioOn("").empty());
    CHECK(pioOn("rpi-custom").empty());
}

TEST_CASE("Hub75Pio: the canvas is the lane across and the parallel lanes down") {
    mm::PioPortGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    CHECK(mm::platform::hub75PioWidth() == 256);
    CHECK(mm::platform::hub75PioHeight() == 192);
    CHECK(mm::platform::hub75PioLastError() == nullptr);
    CHECK(mm::platform::hub75PioWarning() == nullptr);
}

TEST_CASE("Hub75Pio: an open keeps the options") {
    mm::PioPortGuard guard;
    mm::platform::Hub75PioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.panelsPerLane = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 2;
    o.pixelClockHz = 5000000;
    o.latchBlankWords = 3;
    o.clockFallingEdge = true;
    REQUIRE(mm::platform::hub75PioOpen(o));
    const auto& kept = mm::platform::hub75PioTestPort().options;
    CHECK(kept.rows == 32);
    CHECK(kept.cols == 64);
    CHECK(kept.panelsPerLane == 1);
    CHECK(kept.parallel == 1);
    CHECK(kept.scanRate == 16);
    CHECK(kept.bitDepth == 2);
    CHECK(kept.pixelClockHz == 5000000);
    CHECK(kept.latchBlankWords == 3);
    CHECK(kept.clockFallingEdge);
    CHECK(mm::platform::hub75PioWidth() == 64);
    CHECK(mm::platform::hub75PioHeight() == 32);
}

TEST_CASE("Hub75Pio: a pushed frame arrives byte for byte") {
    mm::PioPortGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3);
    for (size_t i = 0; i < frame.size(); i++) frame[i] = static_cast<uint8_t>(i * 7);
    mm::platform::hub75PioPush(frame.data());
    REQUIRE(mm::platform::hub75PioTestPort().frames == 1);
    CHECK(std::memcmp(mm::platform::hub75PioTestFrame(), frame.data(), frame.size()) == 0);
}

TEST_CASE("Hub75Pio: the correction is handed over as given, brightness 0 included") {
    mm::PioPortGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    mm::platform::hub75PioCorrection(0);
    CHECK(mm::platform::hub75PioTestPort().brightness == 0);
    mm::platform::hub75PioCorrection(200);
    CHECK(mm::platform::hub75PioTestPort().brightness == 200);
}

TEST_CASE("Hub75Pio: the recorder's rate is 0 until a test sets it, holds while open, and ends with the close") {
    mm::PioPortGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    CHECK(mm::platform::hub75PioFps() == 0);
    mm::platform::setTestHub75PioFps(60);
    CHECK(mm::platform::hub75PioFps() == 60);
    mm::platform::hub75PioClose();
    CHECK(mm::platform::hub75PioFps() == 0);
    mm::platform::setTestHub75Pio(Hub75PioMode::Record);   // setting the mode again puts the levers back
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    CHECK(mm::platform::hub75PioFps() == 0);
}

TEST_CASE("Hub75Pio: an open outside a test mode is refused in the test binary before piolib or /dev/pio0 is reached") {
    mm::platform::setTestHub75Pio(Hub75PioMode::Off);
    const uint32_t attempts = mm::platform::hub75PioTestRealAttempts();
    mm::PioHost pi5("6.18.60+rpt-rpi-2712", true);   // a host that would serve the PIO, where piolib is linked
    CHECK_FALSE(mm::platform::hub75PioOpen(benchWall()));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()).find("MM_TEST_REAL_PIO=1") != std::string::npos);   // the refusal says how to opt in
    CHECK(mm::platform::hub75PioTestRealAttempts() == attempts + 1);   // asked for, and stopped at the gate
    CHECK(mm::platform::hub75PioTestRealOpens() == 0);                 // none reached the backend
    CHECK(mm::platform::hub75PioWidth() == 0);
}

#ifndef MM_HUB75_PIO_HAVE_PIOLIB
TEST_CASE("Hub75Pio: without piolib an open outside a test mode fails naming the library") {
    mm::platform::setTestHub75Pio(Hub75PioMode::Off);
    mm::platform::setTestHub75PioGateOpen(true);   // the host's own refusal is the case; piolib is absent, so the real backend stays out of reach
    CHECK_FALSE(mm::platform::hub75PioOpen(benchWall()));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()) == "needs a Raspberry Pi build with piolib");
    CHECK(mm::platform::hub75PioWidth() == 0);
}
#endif

#ifdef MM_HUB75_PIO_HAVE_PIOLIB
TEST_CASE("Hub75Pio: a stalling kernel refuses the open outright, since the PIO output has no fallback") {
    mm::platform::setTestHub75Pio(Hub75PioMode::Off);
    mm::platform::setTestHub75PioGateOpen(true);   // the kernel check refuses before any backend is reached
    mm::PioHost stalls("6.18.50+rpt-rpi-2712", true);
    CHECK_FALSE(mm::platform::hub75PioOpen(benchWall()));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()).find("stalls a PIO reopen") != std::string::npos);
    CHECK(mm::platform::hub75PioWidth() == 0);
}

TEST_CASE("Hub75Pio: without /dev/pio0 the open is refused, whatever the kernel") {
    mm::platform::setTestHub75Pio(Hub75PioMode::Off);
    mm::platform::setTestHub75PioGateOpen(true);   // the device check refuses before any backend is reached
    mm::PioHost host("6.18.60+rpt-rpi-2712", false);
    CHECK_FALSE(mm::platform::hub75PioOpen(benchWall()));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()) == "no /dev/pio0 on this system");
}

TEST_CASE("Hub75Pio: with piolib, the host gates availability: a Pi 5 is offered the backend, a Pi 4 and a stall kernel are not") {
    mm::platform::setTestHub75Pio(Hub75PioMode::Off);
    {
        mm::PioHost pi5("6.18.60+rpt-rpi-2712", true);   // a Pi 5 PIO serves on
        CHECK(mm::platform::hub75PioAvailable());
    }
    {
        mm::PioHost pi4("6.18.60+rpt-rpi-2712", false);   // the same kernel, no /dev/pio0: a Pi 4
        CHECK_FALSE(mm::platform::hub75PioAvailable());
    }
    {
        mm::PioHost stalls("6.18.50+rpt-rpi-2712", true);   // a stall kernel, device present
        CHECK_FALSE(mm::platform::hub75PioAvailable());
    }
}
#endif

TEST_CASE("Hub75Pio: every test mode offers the backend on every host, and Off leaves it to the host") {
    {
        mm::PioPortGuard record(Hub75PioMode::Record);
        CHECK(mm::platform::hub75PioAvailable());
    }
    {
        mm::PioPortGuard stub(Hub75PioMode::Stub);
        CHECK(mm::platform::hub75PioAvailable());
    }
#ifndef MM_HUB75_PIO_HAVE_PIOLIB
    CHECK_FALSE(mm::platform::hub75PioAvailable());   // off, the host decides: no piolib, not offered
#endif
}

TEST_CASE("Hub75Pio: the test hook forces availability either way, and the host decides again after it") {
    mm::platform::setTestHub75Pio(Hub75PioMode::Off);
    {
        mm::PioAvailable off(0);
        CHECK_FALSE(mm::platform::hub75PioAvailable());
    }
    {
        mm::PioAvailable on(1);
        CHECK(mm::platform::hub75PioAvailable());
    }
#ifndef MM_HUB75_PIO_HAVE_PIOLIB
    CHECK_FALSE(mm::platform::hub75PioAvailable());   // after the guards the host decides again: no piolib, not offered
#endif
}

TEST_CASE("Hub75Pio: a recorded open refuses what the engine would refuse, with its reason") {
    mm::PioPortGuard guard;
    // A 32-high panel at 1/32 scan: the encoder needs an even rows-per-scan.
    mm::platform::Hub75PioOptions o = benchWall();
    o.rows = 32;
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()).find("32-high at 1/16") != std::string::npos);
    CHECK(mm::platform::hub75PioWidth() == 0);
    CHECK(mm::platform::hub75PioHeight() == 0);

    o = benchWall();
    o.panelsPerLane = 0;
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    o = benchWall();
    o.parallel = 0;
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    o = benchWall();
    o.bitDepth = 8;
    CHECK(mm::platform::hub75PioOpen(o));   // 8 is inside the PIO range
    mm::platform::hub75PioClose();
    o = benchWall();
    o.bitDepth = 9;
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    o = benchWall();
    o.pixelClockHz = 0;
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    o = benchWall();
    o.scanRate = 64;
    CHECK_FALSE(mm::platform::hub75PioOpen(o));
    CHECK(mm::platform::hub75PioWidth() == 0);
    CHECK(mm::platform::hub75PioHeight() == 0);
}

TEST_CASE("Hub75Pio: after close there is no canvas, and a push is ignored") {
    mm::PioPortGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    mm::platform::hub75PioClose();
    CHECK(mm::platform::hub75PioWidth() == 0);
    CHECK(mm::platform::hub75PioHeight() == 0);
    const uint8_t pixel[3] = {1, 2, 3};
    mm::platform::hub75PioPush(pixel);
    CHECK(mm::platform::hub75PioTestPort().frames == 0);
    CHECK(mm::platform::hub75PioFps() == 0);
}

TEST_CASE("Hub75Pio: a null frame is ignored") {
    mm::PioPortGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    mm::platform::hub75PioPush(nullptr);
    CHECK(mm::platform::hub75PioTestPort().frames == 0);
}

TEST_CASE("Hub75Pio: the newest of several pushes is the one shown, whole, until the next") {
    mm::PioPortGuard guard;
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    std::vector<uint8_t> frame(256 * 192 * 3);
    for (uint8_t v : {1, 2, 3}) {   // three frames faster than the panels take them
        std::fill(frame.begin(), frame.end(), v);
        mm::platform::hub75PioPush(frame.data());
    }
    const uint8_t* shown = mm::platform::hub75PioTestFrame();
    REQUIRE(shown != nullptr);
    CHECK(std::count(shown, shown + frame.size(), uint8_t{3}) == static_cast<long>(frame.size()));
    CHECK(mm::platform::hub75PioTestFrame() == shown);   // no new push, so the same frame stays
}

TEST_CASE("Hub75Pio: the seam counts its opens and its closes, an open that closes another included") {
    mm::PioPortGuard guard;
    CHECK(mm::platform::hub75PioTestPort().opens == 0);
    mm::platform::hub75PioClose();   // the panels are closed: no close to count
    CHECK(mm::platform::hub75PioTestPort().closes == 0);
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));   // closes the first
    CHECK(mm::platform::hub75PioTestPort().opens == 2);
    CHECK(mm::platform::hub75PioTestPort().closes == 1);
    mm::platform::hub75PioClose();
    CHECK(mm::platform::hub75PioTestPort().closes == 2);
    mm::platform::Hub75PioOptions refused = benchWall();
    refused.rows = 0;
    CHECK_FALSE(mm::platform::hub75PioOpen(refused));   // a refused open counts no open
    CHECK(mm::platform::hub75PioTestPort().opens == 2);
}

TEST_CASE("Hub75Pio: the open lever refuses the open in either mode, and setting the mode lifts it") {
    for (Hub75PioMode mode : {Hub75PioMode::Record, Hub75PioMode::Stub}) {
        mm::PioPortGuard guard(mode);
        mm::platform::setTestHub75PioOpenFails(true);
        CHECK_FALSE(mm::platform::hub75PioOpen(benchWall()));
        REQUIRE(mm::platform::hub75PioLastError() != nullptr);
        CHECK(mm::platform::hub75PioWidth() == 0);
        CHECK(mm::platform::hub75PioTestPort().opens == 0);
        mm::platform::setTestHub75Pio(mode);
        CHECK(mm::platform::hub75PioOpen(benchWall()));
    }
}

TEST_CASE("Hub75PioStub: the seam opens the real engine on the stub, and the recorder holds nothing") {
    mm::PioPortGuard guard(Hub75PioMode::Stub);
    REQUIRE(mm::platform::hub75PioOpen(benchWall()));
    const auto& port = mm::platform::hub75PioTestPort();
    CHECK(port.engineHoldsPanels);
    CHECK(port.opens == 1);
    CHECK(mm::platform::hub75PioWidth() == 256);
    CHECK(mm::platform::hub75PioHeight() == 192);
    CHECK(mm::test::waitUntil([] { return mm::platform::stubBackendSubmittedFrames() > 0; }));   // the engine's refresh thread runs
    CHECK(mm::platform::hub75PioTestFrame() == nullptr);   // the recorder keeps no frame: the engine took it
    CHECK(mm::platform::stubBackendLastStart().starts == 1);
}

TEST_CASE("Hub75PioStub: a pushed frame reaches the stub as its encode, and the close leaves a blank") {
    mm::PioPortGuard guard(Hub75PioMode::Stub);
    mm::platform::Hub75PioOptions o = mm::test::pioSmall();
    REQUIRE(mm::platform::hub75PioOpen(o));
    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[0] = 255;   // pixel (0,0), red
    mm::platform::hub75PioPush(frame.data());
    CHECK(mm::platform::hub75PioTestPort().frames == 1);
    // Plane 1, row 0, column 0: slot 1024. R1 of lane 0 is wide bit 9; the address names the last row, 15, on A-D; OE is clear, the panel lit.
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::test::waitUntil([&] {
        return mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 2048 && last[1024] == 0x00F00200u;
    }));
    mm::platform::hub75PioClose();
    CHECK_FALSE(mm::platform::hub75PioTestPort().engineHoldsPanels);
    CHECK(mm::platform::hub75PioTestPort().frames == 0);
    CHECK(mm::platform::stubBackendBlanks() == 1);
    REQUIRE(mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 2048);
    for (uint32_t w : last) CHECK(w == mm::platform::kBlankWideWord);
}

TEST_CASE("Hub75PioStub: a push after the close never reaches the engine") {
    mm::PioPortGuard guard(Hub75PioMode::Stub);
    REQUIRE(mm::platform::hub75PioOpen(mm::test::pioSmall()));
    std::vector<uint8_t> frame(64 * 32 * 3, 9);
    mm::platform::hub75PioPush(frame.data());
    REQUIRE(mm::platform::hub75PioTestPort().frames == 1);
    mm::platform::hub75PioClose();
    mm::platform::hub75PioPush(frame.data());
    CHECK(mm::platform::hub75PioTestPort().frames == 0);   // the engine's count and the recorder's both held still
    CHECK(mm::platform::hub75PioFps() == 0);
    CHECK_FALSE(mm::platform::hub75PioTestPort().engineHoldsPanels);
}

TEST_CASE("Hub75PioStub: the correction reaches the engine, and the rate is the engine's own") {
    mm::PioPortGuard guard(Hub75PioMode::Stub);
    mm::platform::setTestHub75PioFps(99);   // the recorder's lever, which the engine ignores
    REQUIRE(mm::platform::hub75PioOpen(mm::test::pioSmall()));
    mm::platform::hub75PioCorrection(0);
    CHECK(mm::platform::hub75PioTestPort().brightness == 0);
    std::vector<uint8_t> frame(64 * 32 * 3, 255);   // all white
    mm::platform::hub75PioPush(frame.data());
    // At brightness 0 no word of the encode is lit: OE stays set (blanked) through the top plane's window.
    std::vector<uint32_t> last(2048);
    const uint32_t oe = 1u << mm::platform::kWideBitOe;
    CHECK(mm::test::waitUntil([&] {
        if (mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) != 2048) return false;
        return std::count_if(last.begin(), last.end(), [&](uint32_t w) { return (w & oe) == 0; }) == 0;
    }));
    CHECK(mm::platform::hub75PioFps() != 99);
}

TEST_CASE("Hub75PioStub: a correction set before the open holds across it, so the first frames light at it") {
    mm::PioPortGuard guard(Hub75PioMode::Stub);
    mm::platform::hub75PioCorrection(0);   // the output sets the correction ahead of its first push, before the engine opens
    REQUIRE(mm::platform::hub75PioOpen(mm::test::pioSmall()));
    const uint64_t submitted = mm::platform::stubBackendSubmittedFrames();
    std::vector<uint8_t> frame(64 * 32 * 3, 255);   // all white
    mm::platform::hub75PioPush(frame.data());
    CHECK(mm::test::waitUntil([&] { return mm::platform::stubBackendSubmittedFrames() >= submitted + 3; }));
    // Brightness 0 lights no word: the white frame goes out dark although the engine opened after the correction.
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::stubBackendCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(mm::test::litWords(last) == 0);
}

TEST_CASE("Hub75PioStub: frames pushed from another task while the engine runs reach it, and the close joins") {
    // The render task pushes while the refresh thread encodes and submits: the publish-versus-refresh handover under the sanitizer lanes.
    mm::PioPortGuard guard(Hub75PioMode::Stub);
    REQUIRE(mm::platform::hub75PioOpen(mm::test::pioSmall()));
    std::thread render([] {
        std::vector<uint8_t> frame(64 * 32 * 3);
        for (int i = 0; i < 200; i++) {
            std::fill(frame.begin(), frame.end(), static_cast<uint8_t>(i));
            mm::platform::hub75PioPush(frame.data());
        }
    });
    render.join();
    CHECK(mm::platform::hub75PioTestPort().frames == 200);
    mm::platform::hub75PioClose();
    CHECK(mm::platform::stubBackendBlanks() == 1);
}

TEST_CASE("Hub75PioStub: a refused engine open reports its reason and holds no panels") {
    mm::PioPortGuard guard(Hub75PioMode::Stub);
    mm::platform::StubFaults faults;
    faults.failOpen = true;
    mm::platform::stubBackendSetFaults(faults);
    CHECK_FALSE(mm::platform::hub75PioOpen(mm::test::pioSmall()));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()).find("open") != std::string::npos);
    CHECK_FALSE(mm::platform::hub75PioTestPort().engineHoldsPanels);
    CHECK(mm::platform::hub75PioWidth() == 0);

    faults = {};
    faults.failStart = true;
    mm::platform::stubBackendSetFaults(faults);
    CHECK_FALSE(mm::platform::hub75PioOpen(mm::test::pioSmall()));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()).find("start") != std::string::npos);
    CHECK_FALSE(mm::platform::hub75PioTestPort().engineHoldsPanels);

    mm::platform::stubBackendSetFaults({});
    CHECK(mm::platform::hub75PioOpen(mm::test::pioSmall()));   // the next open is clean
}

TEST_CASE("Hub75PioStub: a failed encode at open refuses the open, and the lever lifts with the mode") {
    mm::PioPortGuard guard(Hub75PioMode::Stub);
    mm::platform::setTestHub75PioEncodeFails(-1);
    CHECK_FALSE(mm::platform::hub75PioOpen(mm::test::pioSmall()));
    REQUIRE(mm::platform::hub75PioLastError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioLastError()).find("initial encode failed") != std::string::npos);
    mm::platform::setTestHub75Pio(Hub75PioMode::Stub);
    CHECK(mm::platform::hub75PioOpen(mm::test::pioSmall()));
}

TEST_CASE("Hub75Pio: the failed submits are counted at the seam, by a lever in Record mode and by the engine in Stub mode") {
    {
        mm::PioPortGuard guard(Hub75PioMode::Record);
        REQUIRE(mm::platform::hub75PioOpen(benchWall()));
        CHECK(mm::platform::hub75PioSubmitErrors() == 0);
        mm::platform::setTestHub75PioSubmitErrors(7);
        CHECK(mm::platform::hub75PioSubmitErrors() == 7);
        mm::platform::hub75PioClose();
        CHECK(mm::platform::hub75PioSubmitErrors() == 0);
    }
    mm::PioPortGuard guard(Hub75PioMode::Stub);
    REQUIRE(mm::platform::hub75PioOpen(mm::test::pioSmall()));
    REQUIRE(mm::test::waitUntil([] { return mm::platform::stubBackendSubmittedFrames() > 0; }));
    CHECK(mm::platform::hub75PioSubmitErrors() == 0);
    mm::platform::StubFaults faults;
    faults.failSubmits = -1;
    mm::platform::stubBackendSetFaults(faults);
    CHECK(mm::test::waitUntil([] { return mm::platform::hub75PioSubmitErrors() >= 3; }));
    mm::platform::stubBackendSetFaults({});
}

TEST_CASE("Hub75Pio: a pixel clock past the 20 MHz the bench ran opens with a warning, which the close takes away") {
    for (Hub75PioMode mode : {Hub75PioMode::Record, Hub75PioMode::Stub}) {
        mm::PioPortGuard guard(mode);
        mm::platform::Hub75PioOptions o = mm::test::pioSmall();
        o.pixelClockHz = 20000000;
        REQUIRE(mm::platform::hub75PioOpen(o));
        CHECK(mm::platform::hub75PioWarning() == nullptr);   // the benched clock itself is clean
        o.pixelClockHz = 25000000;
        REQUIRE(mm::platform::hub75PioOpen(o));
        REQUIRE(mm::platform::hub75PioWarning() != nullptr);
        CHECK(std::string(mm::platform::hub75PioWarning()).find("20 MHz") != std::string::npos);
        mm::platform::hub75PioClose();
        CHECK(mm::platform::hub75PioWarning() == nullptr);
    }
}

TEST_CASE("Hub75Pio: a close whose stop fails publishes why though it finished, and a close whose stop works leaves none") {
    mm::PioPortGuard guard(Hub75PioMode::Stub);
    REQUIRE(mm::platform::hub75PioOpen(mm::test::pioSmall()));
    REQUIRE(mm::test::waitUntil([] { return mm::platform::stubBackendSubmittedFrames() > 0; }));
    mm::platform::StubFaults faults;
    faults.failStop = true;
    mm::platform::stubBackendSetFaults(faults);
    mm::platform::hub75PioClose();   // the thread joined, so the close finished, and the failed stop still shows
    REQUIRE(mm::platform::hub75PioCloseError() != nullptr);
    const std::string why = mm::platform::hub75PioCloseError();
    CHECK(why.find("the stop failed") != std::string::npos);
    CHECK(why.find("may hold its last row") != std::string::npos);
    CHECK(why.find("did not return") == std::string::npos);
    mm::platform::stubBackendSetFaults({});
    REQUIRE(mm::platform::hub75PioOpen(mm::test::pioSmall()));   // the engine opens again: the PIO is free
    mm::platform::hub75PioClose();
    CHECK(mm::platform::hub75PioCloseError() == nullptr);        // a stop that worked leaves none
}

TEST_CASE("Hub75Pio: a later close whose retried stop fails again keeps a failure message, and a close whose stop works clears it") {
    mm::PioPortGuard guard(Hub75PioMode::Stub);
    openSmallWithShortDeadline();
    mm::platform::StubFaults faults;
    faults.hangSubmits = true;
    faults.failStop = true;
    mm::platform::stubBackendSetFaults(faults);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // the refresh thread is inside the hung submit
    mm::platform::hub75PioClose();
    REQUIRE(mm::platform::hub75PioCloseError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioCloseError()).find("did not return") != std::string::npos);
    faults.hangSubmits = false;   // the thread returns, and the stop stays refused
    mm::platform::stubBackendSetFaults(faults);
    auto failureOnly = [] {
        mm::platform::hub75PioClose();
        const char* why = mm::platform::hub75PioCloseError();
        return why != nullptr && std::string(why).find("did not return") == std::string::npos;
    };
    CHECK(mm::test::waitUntil(failureOnly));   // the close finished, and it did not clear what the stop said
    REQUIRE(mm::platform::hub75PioCloseError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioCloseError()).find("the stop failed") != std::string::npos);
    mm::platform::stubBackendSetFaults({});
    REQUIRE(mm::platform::hub75PioOpen(mm::test::pioSmall()));
    mm::platform::hub75PioClose();
    CHECK(mm::platform::hub75PioCloseError() == nullptr);
}

TEST_CASE("Hub75Pio: a close that finds the refresh thread stuck says so, and a later close finishes") {
    mm::PioPortGuard guard(Hub75PioMode::Stub);
    openSmallWithShortDeadline();
    mm::test::hangSubmits();
    CHECK(mm::platform::hub75PioCloseError() == nullptr);
    mm::platform::hub75PioClose();
    REQUIRE(mm::platform::hub75PioCloseError() != nullptr);
    CHECK(std::string(mm::platform::hub75PioCloseError()).find("did not return") != std::string::npos);
    CHECK_FALSE(mm::platform::hub75PioOpen(mm::test::pioSmall()));   // refused while the PIO is stuck
    mm::platform::stubBackendSetFaults({});
    CHECK(mm::test::waitUntil([] {   // once the thread has returned, a close finishes
        mm::platform::hub75PioClose();
        return mm::platform::hub75PioCloseError() == nullptr;
    }));
    CHECK(mm::platform::hub75PioOpen(mm::test::pioSmall()));
}
