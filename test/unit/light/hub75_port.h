#pragma once

/// Helpers the HUB75 tests share: the HUB75 test ports, bus and PIO, opened for one case and closed however it ends (each is process-global, so a guard keeps one case's frame from reaching the next), the PIO seam's host stand-ins, and a select's option index by label.

#include "../core/conditional_controls.h"   // mm::controlIndex
#include "core/module/Control.h"
#include "doctest.h"
#include "platform/Hub75Pio.h"
#include "platform/machine.h"
#include "platform/platform.h"

#include <cstring>

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
