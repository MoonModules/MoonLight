/// @module I2cBusModule

/// The board's I2C bus is opened once, on the pins the board states, and every device on it attaches to that one bus.

#include "doctest.h"
#include "core/system/I2cBusModule.h"
#include "core/module/Scheduler.h"

#include <cstring>
#include <string>

namespace {
void setPin(mm::I2cBusModule& bus, const char* name, int value) {
    for (uint8_t i = 0; i < bus.controls().count(); i++)
        if (std::strcmp(bus.controls()[i].name, name) == 0) *static_cast<int8_t*>(bus.controls()[i].ptr) = static_cast<int8_t>(value);
}

std::string result(const mm::I2cBusModule& bus) {
    for (uint8_t i = 0; i < bus.controls().count(); i++)
        if (std::strcmp(bus.controls()[i].name, "result") == 0) return static_cast<const char*>(bus.controls()[i].ptr);
    return "";
}

// A module driving one chip on the bus, as AudioService drives its codec or a sensor module its gyro.
struct FakeI2cDevice : mm::MoonModule {
    uint8_t addr;
    const char* role;
    FakeI2cDevice(const char* name, uint8_t a, const char* r) : addr(a), role(r) { setName(name); }
    uint8_t i2cDevices(I2cDevice* out, uint8_t max) const override {
        if (max == 0) return 0;
        out[0] = {addr, role};
        return 1;
    }
};
}  // namespace

// Unset pins claim no GPIO, and a board without a bus never opens one.
TEST_CASE("the bus opens once both pins are set, and closes when one is unset") {
    mm::I2cBusModule bus;
    bus.defineControls();
    bus.prepare();
    CHECK_FALSE(mm::platform::i2cBusReady());
    setPin(bus, "sda", 7);
    setPin(bus, "scl", 8);
    const uint32_t before = mm::platform::i2cBusGeneration();
    bus.prepare();
    CHECK(mm::platform::i2cBusReady());
    CHECK(mm::platform::i2cBusGeneration() != before);   // the devices on it see a new bus
    setPin(bus, "scl", -1);
    bus.prepare();
    CHECK_FALSE(mm::platform::i2cBusReady());
}

// Moving the bus is what tells a device such as the codec to attach again, so each move counts; reopening on the same pins does not.
TEST_CASE("moving the bus to other pins counts as a new bus, and the same pins do not") {
    mm::I2cBusModule bus;
    bus.defineControls();
    setPin(bus, "sda", 7);
    setPin(bus, "scl", 8);
    bus.prepare();
    const uint32_t opened = mm::platform::i2cBusGeneration();
    bus.prepare();
    CHECK(mm::platform::i2cBusGeneration() == opened);
    setPin(bus, "sda", 51);
    setPin(bus, "scl", 50);
    bus.prepare();
    CHECK(mm::platform::i2cBusGeneration() != opened);
    CHECK(bus.affectsPrepare("sda"));
    CHECK(bus.affectsPrepare("scl"));
    bus.release();
    CHECK_FALSE(mm::platform::i2cBusReady());
}

// /api/types builds and releases a throwaway instance of every type on each UI load; releasing that probe must not close the bus the live module holds.
TEST_CASE("releasing an instance that never opened the bus leaves the live bus open") {
    mm::I2cBusModule live;
    live.defineControls();
    setPin(live, "sda", 7);
    setPin(live, "scl", 8);
    live.prepare();
    REQUIRE(mm::platform::i2cBusReady());
    const uint32_t opened = mm::platform::i2cBusGeneration();
    {
        mm::I2cBusModule probe;
        probe.defineControls();
        probe.release();
    }
    CHECK(mm::platform::i2cBusReady());
    CHECK(mm::platform::i2cBusGeneration() == opened);   // the codec on it sees no move
    live.release();
    CHECK_FALSE(mm::platform::i2cBusReady());
}

// A scan with no bus says why, rather than reporting an empty bus that was never probed.
TEST_CASE("a scan without a bus names the missing pins, and with one reports what answered") {
    mm::I2cBusModule bus;
    bus.defineControls();
    bus.onControlChanged("scan");
    REQUIRE(bus.status() != nullptr);
    CHECK(std::string(bus.status()) == "set sda + scl pins first");
    setPin(bus, "sda", 7);
    setPin(bus, "scl", 8);
    bus.prepare();
    bus.onControlChanged("scan");
    CHECK(std::string(bus.status()) == "0 devices found");
    bus.release();
}

// A scan names each address by the module driving it; an unclaimed chip shows bare, and a claimed one that stays silent reads as missing.
TEST_CASE("a scan names each device by the module that drives it, and flags an expected one that does not answer") {
    mm::Scheduler scheduler;
    FakeI2cDevice audio("Audio", 0x18, "ES8311");
    FakeI2cDevice motion("Motion", 0x68, "MPU6050");
    mm::I2cBusModule bus;
    scheduler.addModule(&audio);
    scheduler.addModule(&motion);
    scheduler.addModule(&bus);
    scheduler.setup();
    setPin(bus, "sda", 7);
    setPin(bus, "scl", 8);
    bus.prepare();
    const uint8_t answering[] = {0x18, 0x3c};
    mm::platform::setTestI2cDevices(answering, 2);
    bus.onControlChanged("scan");
    CHECK(result(bus) == "0x18 Audio (ES8311), 0x3c, 0x68 Motion (MPU6050): no answer");
    REQUIRE(bus.status() != nullptr);
    CHECK(std::string(bus.status()) == "2 found, 1 missing");
    mm::platform::setTestI2cDevices(nullptr, 0);
    bus.release();
}
