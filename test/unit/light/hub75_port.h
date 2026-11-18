#pragma once

/// Helpers the Hub75Driver tests share: the HUB75 test port, opened for one case and closed however it ends (it is process-global, so a guard keeps one case's frame from reaching the next), and a select's option index by label.

#include "../core/conditional_controls.h"   // mm::controlIndex
#include "core/module/Control.h"
#include "doctest.h"
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

/// The index of `label` among the options of select `control`, or -1.
inline int optionIndex(MoonModule& m, const char* control, const char* label) {
    const int i = controlIndex(m, control);
    REQUIRE(i >= 0);
    const ControlDescriptor& c = m.controls()[static_cast<uint8_t>(i)];
    const char* const* options = reinterpret_cast<const char* const*>(c.aux);
    // int32_t to match the descriptor's own `max`: a uint8_t counter compared against a wider signed type is the narrowing CodeQL flags, and it would never terminate on a select with more options than a byte holds.
    for (int32_t k = 0; k < c.max; k++)
        if (std::strcmp(options[k], label) == 0) return static_cast<int>(k);
    return -1;
}

}  // namespace mm
