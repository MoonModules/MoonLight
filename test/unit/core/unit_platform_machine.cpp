/// @module platform

/// Pins the platform's answer to what machine this is: the device-tree compatible list read for each board family, a host that has none, and the test lever the case trees over the machine are tested through.

#include "doctest.h"
#include "platform/machine.h"

#include <cstring>
#include <string>

namespace {

using mm::platform::Machine;

// A compatible property as the kernel holds it: the strings NUL-separated, the last NUL-terminated too.
Machine of(const char* list, size_t length) { return mm::platform::machineFromCompatible(list, length); }

}  // namespace

TEST_CASE("Machine: a Raspberry Pi 5, its compute module and the Pi 500 read as a Pi 5") {
    static const char pi5[] = "raspberrypi,5-model-b\0brcm,bcm2712";
    static const char cm5[] = "raspberrypi,5-compute-module\0brcm,bcm2712";
    static const char pi500[] = "raspberrypi,500\0raspberrypi,5-model-b\0brcm,bcm2712";
    static const char bare[] = "brcm,bcm2712";
    static const char board[] = "vendor,board\0brcm,bcm2712-variant";   // a prefix: the SoC family, whatever follows
    CHECK(of(pi5, sizeof(pi5)) == Machine::Pi5);
    CHECK(of(cm5, sizeof(cm5)) == Machine::Pi5);
    CHECK(of(pi500, sizeof(pi500)) == Machine::Pi5);
    CHECK(of(bare, sizeof(bare)) == Machine::Pi5);
    CHECK(of(board, sizeof(board)) == Machine::Pi5);   // the SoC alone, as a board with its own name has it
}

TEST_CASE("Machine: an earlier Raspberry Pi reads as one whose header the GPIO drives") {
    static const char pi4[] = "raspberrypi,4-model-b\0brcm,bcm2711";
    static const char cm4[] = "raspberrypi,4-compute-module\0brcm,bcm2711";
    static const char pi400[] = "raspberrypi,400\0raspberrypi,4-model-b\0brcm,bcm2711";
    static const char pi3[] = "raspberrypi,3-model-b-plus\0brcm,bcm2837";
    static const char zero2[] = "raspberrypi,model-zero-2-w\0brcm,bcm2837";
    static const char pi1[] = "raspberrypi,model-b\0brcm,bcm2835";
    static const char soc[] = "brcm,bcm2711";   // the SoC alone
    // Every array whole, its NUL-separated strings all read.
    const auto gpio = [](const char* name, const char* list, size_t length) {
        CAPTURE(name);
        CHECK(of(list, length) == Machine::PiGpio);
    };
    gpio("pi4", pi4, sizeof(pi4));
    gpio("cm4", cm4, sizeof(cm4));
    gpio("pi400", pi400, sizeof(pi400));   // starts "raspberrypi,4", which is no Pi 5's prefix
    gpio("pi3", pi3, sizeof(pi3));
    gpio("zero2", zero2, sizeof(zero2));
    gpio("pi1", pi1, sizeof(pi1));
    gpio("soc", soc, sizeof(soc));
}

TEST_CASE("Machine: a board that is no Pi, no property, an empty one and a cut one read as another machine") {
    static const char other[] = "linux,dummy-virt";
    static const char rock[] = "radxa,rock-5b\0rockchip,rk3588";
    static const char cut[] = "raspberrypi,5-model-b";   // the NUL after it is missing: the string still ends with the property
    CHECK(of(other, sizeof(other)) == Machine::Other);
    CHECK(of(rock, sizeof(rock)) == Machine::Other);
    CHECK(of(nullptr, 0) == Machine::Other);
    CHECK(of("", 0) == Machine::Other);
    CHECK(of(cut, std::strlen(cut)) == Machine::Pi5);
    CHECK(of("brcm,bcm27", 10) == Machine::PiGpio);
}

TEST_CASE("Machine: a Raspberry Pi 5 reads from cpuinfo's Model line where the device tree cannot be read, as in a container") {
    // /proc/cpuinfo as a Pi keeps it: the cores first, the board's Hardware, Revision, Serial and Model last.
    const auto cpuinfo = [](const char* model) { return std::string("processor\t: 0\nBogoMIPS\t: 108.00\n\nHardware\t: BCM2712\nRevision\t: d04170\nSerial\t\t: 10000000deadbeef\nModel\t\t: ") + model + "\n"; };
    const auto kind = [&](const char* model) { const std::string t = cpuinfo(model); return mm::platform::machineFromCpuinfo(t.data(), t.size()); };
    CHECK(kind("Raspberry Pi 5 Model B Rev 1.0") == Machine::Pi5);
    CHECK(kind("Raspberry Pi Compute Module 5 Rev 1.0") == Machine::Pi5);
    CHECK(kind("Raspberry Pi 500 Rev 1.0") == Machine::Pi5);
    CHECK(kind("Raspberry Pi 4 Model B Rev 1.5") == Machine::PiGpio);
    CHECK(kind("Raspberry Pi Compute Module 4 Rev 1.0") == Machine::PiGpio);
    CHECK(kind("Raspberry Pi 400 Rev 1.0") == Machine::PiGpio);
    CHECK(kind("Raspberry Pi 3 Model B Plus Rev 1.3") == Machine::PiGpio);
    CHECK(kind("Raspberry Pi Zero 2 W Rev 1.0") == Machine::PiGpio);
    CHECK(kind("ASUS Tinker Board") == Machine::Other);
    // No Model line, an empty file, a null text, a cut line, and a model word that is only part of another line.
    CHECK(mm::platform::machineFromCpuinfo("processor\t: 0\nmodel name\t: ARMv8\n", 33) == Machine::Other);
    CHECK(mm::platform::machineFromCpuinfo("", 0) == Machine::Other);
    CHECK(mm::platform::machineFromCpuinfo(nullptr, 0) == Machine::Other);
    static const char cut[] = "Model\t\t: Raspberry Pi 5 Model B";   // no newline at the end
    CHECK(mm::platform::machineFromCpuinfo(cut, sizeof(cut) - 1) == Machine::Pi5);
    static const char x86[] = "model name\t: Intel(R) Xeon(R)\nModel\t\t: Dell PowerEdge\n";
    CHECK(mm::platform::machineFromCpuinfo(x86, sizeof(x86) - 1) == Machine::Other);
}

TEST_CASE("Machine: the SoC of a Pi 3 or 4 reads from the device tree's compatible list, and any other board, a Pi 5 included, is Other") {
    using mm::platform::PiSoc;
    const auto soc = [](const char* list, size_t length) { return mm::platform::piSocFromCompatible(list, length); };
    static const char pi4[] = "raspberrypi,4-model-b\0brcm,bcm2711";
    static const char pi400[] = "raspberrypi,400\0brcm,bcm2711";
    static const char cm4[] = "raspberrypi,4-compute-module\0brcm,bcm2711";
    static const char pi3[] = "raspberrypi,3-model-b-plus\0brcm,bcm2837";
    static const char zero2[] = "raspberrypi,model-zero-2-w\0brcm,bcm2837";
    static const char pi5[] = "raspberrypi,5-model-b\0brcm,bcm2712";
    static const char pi1[] = "raspberrypi,model-b\0brcm,bcm2835";
    static const char rock[] = "radxa,rock-pi-4\0rockchip,rk3399";
    CHECK(soc(pi4, sizeof(pi4)) == PiSoc::Bcm2711);
    CHECK(soc(pi400, sizeof(pi400)) == PiSoc::Bcm2711);
    CHECK(soc(cm4, sizeof(cm4)) == PiSoc::Bcm2711);
    CHECK(soc(pi3, sizeof(pi3)) == PiSoc::Bcm2837);
    CHECK(soc(zero2, sizeof(zero2)) == PiSoc::Bcm2837);
    CHECK(soc(pi5, sizeof(pi5)) == PiSoc::Other);
    CHECK(soc(pi1, sizeof(pi1)) == PiSoc::Other);
    CHECK(soc(rock, sizeof(rock)) == PiSoc::Other);
    CHECK(soc(nullptr, 0) == PiSoc::Other);
    CHECK(soc("brcm,bcm27110", 13) == PiSoc::Other);   // a name that only begins like the SoC's
}

TEST_CASE("Machine: the SoC of a Pi 3 or 4 reads from cpuinfo's Model line where the device tree cannot be read") {
    using mm::platform::PiSoc;
    const auto soc = [](const char* model) { const std::string t = std::string("Hardware\t: BCM2711\nRevision\t: c03115\nModel\t\t: ") + model + "\n"; return mm::platform::piSocFromCpuinfo(t.data(), t.size()); };
    CHECK(soc("Raspberry Pi 4 Model B Rev 1.2") == PiSoc::Bcm2711);
    CHECK(soc("Raspberry Pi 400 Rev 1.0") == PiSoc::Bcm2711);
    CHECK(soc("Raspberry Pi Compute Module 4 Rev 1.0") == PiSoc::Bcm2711);
    CHECK(soc("Raspberry Pi 3 Model B Plus Rev 1.3") == PiSoc::Bcm2837);
    CHECK(soc("Raspberry Pi Compute Module 3 Rev 1.0") == PiSoc::Bcm2837);
    CHECK(soc("Raspberry Pi Zero 2 W Rev 1.0") == PiSoc::Bcm2837);
    CHECK(soc("Raspberry Pi 5 Model B Rev 1.0") == PiSoc::Other);
    CHECK(soc("Raspberry Pi 500 Rev 1.0") == PiSoc::Other);
    CHECK(soc("Raspberry Pi Zero W Rev 1.1") == PiSoc::Other);
    CHECK(soc("ASUS Tinker Board") == PiSoc::Other);
    CHECK(mm::platform::piSocFromCpuinfo(nullptr, 0) == PiSoc::Other);
}

#ifndef ESP_PLATFORM
TEST_CASE("Machine: a test starts on a machine that is Other whatever board runs the suite, and the lever returns there") {
    // The test binary's listener rests the lever at Other, so no case takes the path of the Pi it happens to run on.
    CHECK(mm::platform::machine() == Machine::Other);
    for (Machine kind : {Machine::Esp32, Machine::Pi5, Machine::PiGpio, Machine::Other}) {
        mm::platform::setTestMachine(kind);
        CHECK(mm::platform::machine() == kind);
    }
    mm::platform::setTestMachine(Machine::Pi5);
    mm::platform::clearTestMachine();
    CHECK(mm::platform::machine() == Machine::Other);   // the resting answer, not the host's
}

TEST_CASE("Machine: a test starts on a SoC that is Other whatever board runs the suite, and the lever moves it") {
    CHECK(mm::platform::piSoc() == mm::platform::PiSoc::Other);
    mm::platform::setTestPiSoc(mm::platform::PiSoc::Bcm2711);
    CHECK(mm::platform::piSoc() == mm::platform::PiSoc::Bcm2711);
    mm::platform::setTestPiSoc(mm::platform::PiSoc::Other);   // as the listener leaves it
}

TEST_CASE("Machine: the real host is asked for by name, and is read once") {
    const Machine host = mm::platform::hostMachine();
    mm::platform::setTestMachine(Machine::Esp32);
    CHECK(mm::platform::hostMachine() == host);   // a lever does not move it
    mm::platform::clearTestMachine();
    CHECK(mm::platform::hostMachine() == host);
    // The resting answer is the test binary's; the real board is only what hostMachine() says it is.
    mm::platform::setTestMachineRest(Machine::PiGpio);
    mm::platform::clearTestMachine();
    CHECK(mm::platform::machine() == Machine::PiGpio);
    mm::platform::setTestMachineRest(Machine::Other);   // as the listener leaves it
}
#endif
