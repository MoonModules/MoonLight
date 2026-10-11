#pragma once

/// @defgroup host_bus The host-bus contract
/// @{
/// What every parallel peripheral must do off-device, asserted once and called per type.
///
/// @moreinfo
///
/// Four DMA peripherals do one job off-device, holding a frame, so the desktop backs them all with one heap buffer and the assertions are written once.
///
/// ## Why the buffers must appear in the memory readout
///
/// The platform allocates them, and leaving them out of the driver's figure made the card lie about the one thing a user picks a driver on.
/// On the bench a one-lane i80 board read 512 bytes and cost 49 KB more free heap, since an i80 frame is sized by the bus width.

#include "doctest.h"
#include "correction_presets.h"
#include "light/drivers/Correction.h"
#include "light/drivers/ParallelLedDriver.h"
#include "light/layers/Buffer.h"
#include "unit/core/conditional_controls.h"

#include <cstdint>
#include <cstring>

namespace mm::test {

/// Wire a driver to a peripheral declared before it, so it outlives the borrow, and a buffer of `lights`, on `benchPins` unless the case set its own.
template <typename Peripheral>
inline void wireParallel(mm::ParallelLedDriver& d, Peripheral& peripheral, mm::Buffer& src, mm::Correction& corr,
                         mm::nrOfLightsType lights, const char* benchPins) {
    d.setPeripheralForTest(&peripheral);
    // Pins default to unset, so a fresh driver idles; these cases exercise the lane logic rather than the default.
    if (d.pins[0] == '\0') std::strcpy(d.pins, benchPins);
    // An empty buffer is wired on purpose for zero lights, so a masked allocation failure would fail cases downstream.
    REQUIRE(src.allocate(lights, 3) == (lights > 0));
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);   // 3 out-channels
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

/// The frame ParallelLedDriver::frameBytesFor sizes: (lights x outCh x 24 + 864 pad) x slotBytes, up to 64; one slot byte per 8 lanes.
inline size_t expectParallelFrame(mm::nrOfLightsType maxLights, uint8_t outCh, uint8_t slotBytes = 1) {
    if (maxLights == 0) return 0;
    const size_t raw = (static_cast<size_t>(maxLights) * outCh * 24 + 800 + 64) * slotBytes;
    return (raw + 63) & ~static_cast<size_t>(63);
}

/// setup/release cycles on `pins` leave no residue: the status clears every time.
template <typename Peripheral>
inline void checkSetupReleaseRepeatable(const char* pins, uint8_t lanes) {
    Peripheral peripheral;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    d.setPeripheralForTest(&peripheral);
    src.allocate(64, 3);
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    std::strcpy(d.pins, pins);
    d.defineControls();
    for (int cycle = 0; cycle < 4; cycle++) {
        d.setup();
        d.setSourceBuffer(&src);
        d.correctionForTest() = corr;
        d.applyState();
        REQUIRE(d.laneCount() == lanes);
        d.release();
        CHECK(d.status() == nullptr);
    }
}

/// loopbackTxPin is bound always and shown only while loopbackTest is on.
template <typename Peripheral>
inline void checkLoopbackTxPinTracksTest() {
    Peripheral peripheral;
    mm::ParallelLedDriver d;
    d.setPeripheralForTest(&peripheral);
    d.defineControls();
    auto setTest = [&](bool on) { mm::test::setControlValue<bool>(d, "loopbackTest", on); };
    mm::test::checkConditionalControl(d, "loopbackTxPin", setTest, /*visibleWhenTrue=*/true);
}

/// Pin the memory-backed bus contract for one peripheral type.
template <typename Peripheral>
inline void checkHostBusAllocates() {
    mm::ParallelLedDriver drv;
    Peripheral peripheral;
    peripheral.attach(&drv);              // busInit reads the pin list through the owner
    drv.setPeripheralForTest(&peripheral);  // borrowed, not owned, so no delete of a local

    REQUIRE(peripheral.busInit(768, /*wantSecondBuffer=*/false));
    CHECK(peripheral.busCapacity() == 768);

    uint8_t* buf = peripheral.busBuffer(0);
    REQUIRE(buf != nullptr);
    // Writable and reading back: a bus that carries the frame rather than discarding it.
    buf[0] = 0xA5;
    buf[767] = 0x5A;
    CHECK(buf[0] == 0xA5);
    CHECK(buf[767] == 0x5A);

    CHECK(peripheral.busTransmit(0, 768));
    // Truncating would look like a good frame while dropping every light past the end.
    CHECK_FALSE(peripheral.busTransmit(0, 4096));
    // A false here reads as an incomplete frame and stalls a bus that is never busy.
    CHECK(peripheral.busWait(0, 10));

    peripheral.busDeinit();
    CHECK(peripheral.busBuffer(0) == nullptr);
}

/// The DMA buffers must show up in the driver's memory readout: @xref{why-the-buffers-must-appear-in-the-memory-readout|why}.
template <typename Peripheral>
inline void checkHostBusCountedInHeapReadout() {
    mm::ParallelLedDriver drv;
    Peripheral peripheral;
    peripheral.attach(&drv);
    drv.setPeripheralForTest(&peripheral);

    // Through the public readout the card renders, which is the number this test exists to pin.
    drv.publishHeapBytesForTest();
    const size_t before = drv.dynamicBytes();

    REQUIRE(peripheral.busInit(4096, /*wantSecondBuffer=*/false));
    drv.publishHeapBytesForTest();
    const size_t single = drv.dynamicBytes();
    CHECK(single >= before + 4096);   // the frame is now visible, whoever allocated it

    // Counting it is what tells a user the second buffer costs a whole extra frame.
    REQUIRE(peripheral.busInit(4096, /*wantSecondBuffer=*/true));
    drv.doubleBuffer = true;
    drv.publishHeapBytesForTest();
    const size_t doubled = drv.dynamicBytes();
    CHECK(doubled == single + 4096);   // exactly one extra frame, not a re-count of the whole driver

    peripheral.busDeinit();
}

/// Double-buffering lets the driver encode one frame while the other is in flight, so the two must be distinct allocations, since aliasing them would tear every frame.
template <typename Peripheral>
inline void checkHostBusDoubleBuffer() {
    mm::ParallelLedDriver drv;
    Peripheral peripheral;
    peripheral.attach(&drv);
    drv.setPeripheralForTest(&peripheral);

    REQUIRE(peripheral.busInit(256, /*wantSecondBuffer=*/true));
    REQUIRE(peripheral.busBuffer(0) != nullptr);
    REQUIRE(peripheral.busBuffer(1) != nullptr);
    CHECK(peripheral.busBuffer(0) != peripheral.busBuffer(1));

    // Re-initializing releases the second, rather than leaving a stale span to read from.
    REQUIRE(peripheral.busInit(256, /*wantSecondBuffer=*/false));
    CHECK(peripheral.busBuffer(1) == nullptr);
}

/// @}
}  // namespace mm::test
