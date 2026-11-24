// SPDX-License-Identifier: GPL-3.0-or-later

/// The GPIO repack tables and scatter loop, documented in hub75_gpio_repack.h.

#include "platform/rpi/hub75_gpio_repack.h"

#include <cassert>

#include "platform/rpi/hub75_gpio_pins.h"
#include "platform/rpi/hub75_rpi_common.h"   // the default-layout slot bits and the color scatter

namespace mm::platform {

namespace {

// The raw GPIO number is the bit position in the GPIO data word.
constexpr int gpioBit(uint8_t gpio) { return gpio; }

}  // namespace

void buildGpioRepackTables(GpioRepackTables& out) {
    // Color tables: per lane, each combination of its six color bits maps to the matching GPIO bits in the data word.
    hub75FillColorTables(out.color, kLanePins, gpioBit);
}

void repackSlotsToGpio(const uint16_t* const laneSlots[kHub75Lanes], int lanes,
                       size_t slots, const GpioRepackTables& tables, uint32_t* gpioOut) {
    assert(lanes >= 1 && lanes <= kHub75Lanes);
    assert(laneSlots[0] != nullptr && gpioOut != nullptr);
    for (size_t i = 0; i < slots; i++) {
        const uint32_t w = tables.color[0][laneSlots[0][i] & 0x3F];
        gpioOut[i] = hub75OrLaneColors(w, laneSlots, lanes, i, tables.color);
    }
}

uint32_t packGpioWordReference(uint16_t slot0, uint16_t slot1, uint16_t slot2,
                               int lanes) {
    const uint16_t slotByLane[kHub75Lanes] = {slot0, slot1, slot2};
    return hub75ReferenceColors(slotByLane, lanes, kLanePins, gpioBit);
}

}  // namespace mm::platform
