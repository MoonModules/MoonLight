/// @module ParallelLedDriver
/// @also SpiPeripheral

#include "doctest.h"
#include "hub75_port.h"   // optionIndex
#include "light/drivers/SpiPeripheral.h"

#include <cstdint>
#include <vector>

// The parallel driver encodes byte slots, bit L for lane L; an SPI host shifts one, two, four or eight data lines a clock, most significant bit first.

TEST_CASE("SPI packs the driver's slots so the first slot leaves first, lane L on data line L") {
    // Four slots, lanes 0 to 3 lit one at a time.
    std::vector<uint8_t> slots = {0x01, 0x02, 0x04, 0x08};
    CHECK(mm::platform::spiPackSlots(slots.data(), slots.size(), 4) == 2);
    CHECK(slots[0] == 0x12);   // slot 0 (lane 0) in the high nibble, slot 1 (lane 1) in the low
    CHECK(slots[1] == 0x48);

    std::vector<uint8_t> dual = {0x01, 0x02, 0x03, 0x00};
    CHECK(mm::platform::spiPackSlots(dual.data(), dual.size(), 2) == 1);
    CHECK(dual[0] == 0b01'10'11'00);

    // One line: lane 0's bit of eight slots makes a byte; the other lanes' bits are not on the wire.
    std::vector<uint8_t> single = {0x01, 0x00, 0x01, 0x01, 0xFE, 0x00, 0x00, 0x01};
    CHECK(mm::platform::spiPackSlots(single.data(), single.size(), 1) == 1);
    CHECK(single[0] == 0b1011'0001);
}

TEST_CASE("SPI on eight data lines sends the slots as they are, and a short tail packs as zeros") {
    std::vector<uint8_t> octal = {0xA5, 0x3C};
    CHECK(mm::platform::spiPackSlots(octal.data(), octal.size(), 8) == 2);
    CHECK(octal[0] == 0xA5);
    std::vector<uint8_t> odd = {0x0F, 0x0F, 0x0F};   // three slots on four lines: the fourth half-byte is zero
    CHECK(mm::platform::spiPackSlots(odd.data(), odd.size(), 4) == 2);
    CHECK(odd[1] == 0xF0);
}

TEST_CASE("SPI drives the line width the strand count needs") {
    CHECK(mm::platform::spiLinesFor(1) == 1);
    CHECK(mm::platform::spiLinesFor(2) == 2);
    CHECK(mm::platform::spiLinesFor(3) == 4);
    CHECK(mm::platform::spiLinesFor(4) == 4);
    CHECK(mm::platform::spiLinesFor(5) == 8);
}

TEST_CASE("The parallel driver offers SPI beside its other peripherals") {
    mm::ParallelLedDriver d;
    d.defineControls();
    CHECK(mm::optionIndex(d, "peripheral", "SPI") >= 0);
}
