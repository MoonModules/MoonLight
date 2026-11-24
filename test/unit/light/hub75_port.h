#pragma once

/// Helpers the HUB75 tests share: the HUB75 test ports, bus, PIO and GPIO, opened for one case and closed however it ends (each is process-global, so a guard keeps one case's frame from reaching the next), the Pi seams' host stand-ins, and a select's option index by label.

#include "../core/conditional_controls.h"   // mm::controlIndex
#include "core/module/Control.h"
#include "doctest.h"
#include "platform/Hub75Gpio.h"
#include "platform/Hub75Pio.h"
#include "platform/machine.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace mm {

/// Opens the platform's HUB75 test port, which hands the driver a frame as the ESP32 port does, and closes it again.
struct PortGuard {
    /// Open the port on LCD_CAM, and on Parlio too when `parlio`.
    explicit PortGuard(bool parlio = false) { platform::setTestHub75(true, parlio); }
    /// Close the port.
    ~PortGuard() { platform::setTestHub75(false); }
};

/// The host answers as `kind` for the case, then reads the real machine again.
struct MachineGuard {
    /// Answer as `kind`.
    explicit MachineGuard(platform::Machine kind) { platform::setTestMachine(kind); }
    /// Read the host again.
    ~MachineGuard() { platform::clearTestMachine(); }
};

/// Holds the PIO seam in `mode` for the case, on a host that answers as a Raspberry Pi 5, and leaves the seam closed and off after it, its levers and the stub backend's faults and records back to their start, and the host as it is.
struct PioPortGuard {
    /// Hold the seam in `mode`: the recorder by default.
    explicit PioPortGuard(platform::Hub75PioMode mode = platform::Hub75PioMode::Record) {
        platform::setTestHub75Pio(mode);
        platform::setTestMachine(platform::Machine::Pi5);
    }
    /// Close the seam, release it and read the host again.
    ~PioPortGuard() {
        platform::setTestHub75Pio(platform::Hub75PioMode::Off);
        platform::clearTestMachine();
    }
};

/// The PIO check stands in for a host running `release`, with /dev/pio0 present or absent, then reads the real one again.
struct PioHost {
    /// Stand in for a host on `release`, with /dev/pio0 when `device`.
    PioHost(const char* release, bool device) { platform::setTestPioHost(release, device); }
    /// Read the real host again.
    ~PioHost() { platform::setTestPioHost(nullptr, false); }
};

/// The PIO availability stands forced as `available` for the case, then the host decides again.
struct PioAvailable {
    /// Force the availability as `available`: 0 unavailable, 1 available.
    explicit PioAvailable(int available) { platform::setTestHub75PioAvailable(available); }
    /// Let the host decide again.
    ~PioAvailable() { platform::setTestHub75PioAvailable(-1); }
};

/// Holds the GPIO seam in `mode` for the case, on a host that answers as an earlier Raspberry Pi, and leaves the seam closed and off after it, its levers and the stub backend's records back to their start, and the host as it is.
struct GpioPortGuard {
    /// Hold the seam in `mode`: the recorder by default.
    explicit GpioPortGuard(platform::Hub75GpioMode mode = platform::Hub75GpioMode::Record) {
        platform::setTestHub75Gpio(mode);
        platform::setTestMachine(platform::Machine::PiGpio);
    }
    /// Close the seam, release it and read the host again.
    ~GpioPortGuard() {
        platform::setTestHub75Gpio(platform::Hub75GpioMode::Off);
        platform::clearTestMachine();
    }
};

/// The host answers as a board with the SoC `soc` for the case, then as one with no SoC (`Other`), as the test binary rests.
struct PiSocGuard {
    /// Answer as `soc`.
    explicit PiSocGuard(platform::PiSoc soc) { platform::setTestPiSoc(soc); }
    /// Rest at `Other`.
    ~PiSocGuard() { platform::setTestPiSoc(platform::PiSoc::Other); }
};

/// The GPIO availability stands forced as `available` for the case, then the host decides again.
struct GpioAvailable {
    /// Force the availability as `available`: 0 unavailable, 1 available.
    explicit GpioAvailable(int available) { platform::setTestHub75GpioAvailable(available); }
    /// Let the host decide again.
    ~GpioAvailable() { platform::setTestHub75GpioAvailable(-1); }
};

/// Whether the options `o` of a Pi seam hold the wall a case names: a panel `rows` by `cols`, `panelsPerLane` of them on each of `parallel` lanes, at `scanRate` and `bitDepth` planes.
template <class Options>
void checkWallOptions(const Options& o, int rows, int cols, int panelsPerLane, int parallel, int scanRate, int bitDepth) {
    CHECK(o.rows == rows);
    CHECK(o.cols == cols);
    CHECK(o.panelsPerLane == panelsPerLane);
    CHECK(o.parallel == parallel);
    CHECK(o.scanRate == scanRate);
    CHECK(o.bitDepth == bitDepth);
}

/// Grow a wall of 2,047-column panels, 240 rows, three lanes at 8 bits, a panel per lane at a time: whether `check` takes the last whose `bytes` fit `ceiling` and refuses the next, which `o` is left at, with the refusal in `error`.
template <class Options, class Bytes, class Check>
bool refusesPastCeiling(Options& o, Bytes bytes, Check check, uint64_t ceiling, std::string& error) {
    o.parallel = 3;
    o.rows = 240;
    o.scanRate = 8;
    o.cols = 2047;
    o.bitDepth = 8;
    int fits = 0;
    for (int panels = 1; panels <= platform::kHub75PanelsPerLaneMax; panels++) {
        o.panelsPerLane = static_cast<uint8_t>(panels);
        if (bytes(o) > ceiling) break;
        fits = panels;
    }
    if (fits == 0 || fits == platform::kHub75PanelsPerLaneMax) return false;
    o.panelsPerLane = static_cast<uint8_t>(fits);
    const bool takesLast = check(o, error);
    o.panelsPerLane = static_cast<uint8_t>(fits + 1);
    return takesLast && !check(o, error);
}

/// The index of `label` among the options of select `control`, or -1.
inline int optionIndex(MoonModule& m, const char* control, const char* label) {
    const int i = controlIndex(m, control);
    REQUIRE(i >= 0);
    const ControlDescriptor& c = m.controls()[static_cast<uint8_t>(i)];
    const char* const* options = reinterpret_cast<const char* const*>(c.aux);
    // int32_t to match the descriptor's own `max`: a uint8_t counter compared against a wider signed type is the narrowing CodeQL flags, and it would loop forever on a select with more options than a byte holds.
    for (int32_t k = 0; k < c.max; k++)
        if (std::strcmp(options[k], label) == 0) return static_cast<int>(k);
    return -1;
}

}  // namespace mm
