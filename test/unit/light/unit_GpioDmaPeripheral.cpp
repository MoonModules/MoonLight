/// @module GpioDmaPeripheral
/// @also ParallelLedDriver, Drivers, Correction

#include "doctest.h"
#include "host_bus.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/drivers/GpioDmaPeripheral.h"
#include "light/layers/Buffer.h"
#include "unit/core/conditional_controls.h"  // shared conditional-control helpers

#include <cstring>

// The host-side half of the GPIO-DMA driver; the timer-paced DMA, the widening and the edge-timing loopback are verified on the Teensy.

namespace {

void wire(mm::ParallelLedDriver& d, mm::GpioDmaPeripheral& peripheral, mm::Buffer& src, mm::Correction& corr,
          mm::nrOfLightsType lights) {
    mm::test::wireParallel(d, peripheral, src, corr, lights, "19,18,14,15,40,41,17,16");
}

} // namespace

TEST_CASE("GpioDmaPeripheral registers under the GPIO-DMA label") {
    mm::GpioDmaPeripheral peripheral;
    CHECK(peripheral.lanesAvailable() == 16);
    CHECK(peripheral.hwBlock() == mm::LedHwBlock::GpioDma);
    CHECK(mm::kGpioDmaPeripheralRegistered);
}

TEST_CASE("GpioDmaPeripheral slices lanes and sizes the frame by the longest") {
    mm::GpioDmaPeripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18,14,15");
    std::strcpy(d.ledsPerPin, "50,20,20");
    wire(d, peripheral, src, corr, 90);

    REQUIRE(d.laneCount() == 3);
    CHECK(d.laneLightCount(0) == 50);
    CHECK(d.laneStart(1) == 50);
    CHECK(d.laneStart(2) == 70);
    CHECK(d.maxLaneLights() == 50);
    CHECK(d.frameBytes() == mm::test::expectParallelFrame(50, 3));
}

// Above 8 lanes the slots widen to 16 bits, which the platform then widens again to the port's 32.
TEST_CASE("GpioDmaPeripheral 16 lanes double the frame (16-bit slots)") {
    mm::GpioDmaPeripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "19,18,14,15,40,41,17,16,22,23,20,21,38,39,26,27");
    std::strcpy(d.ledsPerPin, "50");
    wire(d, peripheral, src, corr, 800);

    REQUIRE(d.laneCount() == 16);
    CHECK(d.frameBytes() == mm::test::expectParallelFrame(50, 3, 2));
    CHECK(d.severity() != mm::MoonModule::Severity::Error);
}

TEST_CASE("GpioDmaPeripheral rejects more than 16 lanes") {
    mm::GpioDmaPeripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "0,1,14,15,16,17,18,19,20,21,22,23,24,25,26,27,38");   // 17 pins
    wire(d, peripheral, src, corr, 170);
    CHECK(d.laneCount() == 0);
    CHECK(d.status() != nullptr);
}

// The wider copy the transfer reads takes the room a second frame would, so the driver never double-buffers on this bus.
TEST_CASE("GpioDmaPeripheral keeps one buffer even when doubleBuffer is asked") {
    mm::GpioDmaPeripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18");
    d.doubleBuffer = true;
    wire(d, peripheral, src, corr, 64);

    CHECK_FALSE(peripheral.supportsDoubleBuffer());
    CHECK(peripheral.busBuffer(0) != nullptr);
    CHECK(peripheral.busBuffer(1) == nullptr);
    for (int i = 0; i < 3; i++) d.tick();   // the synchronous path, frame after frame
    CHECK(d.severity() != mm::MoonModule::Severity::Error);
}

// The DMA reads the widened copy, so the next frame can encode while this one clocks out.
TEST_CASE("GpioDmaPeripheral frees its frame buffer as soon as the transfer starts") {
    mm::ParallelLedDriver drv;
    mm::GpioDmaPeripheral peripheral;
    peripheral.attach(&drv);
    drv.setPeripheralForTest(&peripheral);
    REQUIRE(peripheral.busInit(768, false));
    REQUIRE(peripheral.busTransmit(0, 768));
    CHECK(peripheral.busWait(0, 0));   // no wait for the wire: that happens at the next transmit
    CHECK(peripheral.busTransmit(0, 768));
    peripheral.busDeinit();
}

TEST_CASE("GpioDmaPeripheral allocates a real host bus the driver can encode into") {
    mm::test::checkHostBusAllocates<mm::GpioDmaPeripheral>();
}

// The readout shows what the bus reports holding, which on the Teensy includes the wider copy.
TEST_CASE("GpioDmaPeripheral counts its bus in the driver's memory readout") {
    mm::ParallelLedDriver drv;
    mm::GpioDmaPeripheral peripheral;
    peripheral.attach(&drv);
    drv.setPeripheralForTest(&peripheral);
    drv.publishHeapBytesForTest();
    const size_t before = drv.dynamicBytes();
    REQUIRE(peripheral.busInit(4096, false));
    drv.publishHeapBytesForTest();
    CHECK(drv.dynamicBytes() == before + peripheral.busHeapBytes());
    CHECK(peripheral.busHeapBytes() >= 4096);
    peripheral.busDeinit();
    CHECK(peripheral.busHeapBytes() == 0);
}

TEST_CASE("GpioDmaPeripheral setup/release is repeatable") {
    mm::test::checkSetupReleaseRepeatable<mm::GpioDmaPeripheral>("18,14", 2);
}

TEST_CASE("GpioDmaPeripheral loopbackTxPin tracks the loopbackTest toggle") {
    mm::test::checkLoopbackTxPinTracksTest<mm::GpioDmaPeripheral>();
}

// On a Teensy the W5500's SPI pins are refused, and making one an input would take the network down.
TEST_CASE("GpioDmaPeripheral loopback refuses a receive pin the platform reserves") {
    mm::GpioDmaPeripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    std::strcpy(d.pins, "18");
    wire(d, peripheral, src, corr, 64);
    mm::platform::GpioCapability reserved;
    reserved.reserved = true;
    mm::platform::setTestGpioCapability(12, reserved);
    d.loopbackRxPin = 12;
    d.loopbackTest = true;
    d.onControlChanged("loopbackTest");   // runs the self-test, as a live toggle does
    REQUIRE(d.status() != nullptr);
    CHECK(std::strstr(d.status(), "GPIO 12") != nullptr);
    CHECK(std::strstr(d.status(), "pick another loopbackRxPin") != nullptr);
    mm::platform::clearTestGpioCapability();
}
