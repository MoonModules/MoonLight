// SPDX-License-Identifier: GPL-3.0-or-later

/// The PIO repack tables and loops, documented in hub75_pio_repack.h.

#include "platform/rpi/hub75_pio_repack.h"

#include <cassert>
#include <cstring>   // memset: zero the out-parameter in place, sparing a 1,280-byte temporary

#include "platform/rpi/hub75_pio_pins.h"
#include "platform/rpi/hub75_rpi_common.h"   // the default-layout slot bits and the color scatter

namespace mm::platform {

namespace {

// The seven control slot bits (a, b, c, d, e, lat, oe) and the wide-word bit each drives, in the same order.
constexpr int kControlSlotBits[7] = {kSlotA, kSlotB, kSlotC, kSlotD, kSlotE, kSlotLat, kSlotOe};
constexpr int kControlWideBits[7] = {kWideBitA, kWideBitB, kWideBitC, kWideBitD, kWideBitE, kWideBitStb, kWideBitOe};

// The wide-word bit for a GPIO, as the color scatter places it.
constexpr int wideBit(uint8_t gpio) { return wideBitForGpio(gpio); }

// The wide-word mask for control slot bits 8..14 as `controlBits`, OE inverted: the slot bit means LIT and the pin is active-low, so the pin bit is set when the slot bit is clear.
uint32_t controlMask(unsigned controlBits) {
    uint32_t mask = 0;
    for (int i = 0; i < 7; i++) {
        const bool slotSet = (controlBits & (1u << (kControlSlotBits[i] - 8))) != 0;
        const bool pinSet = (kControlSlotBits[i] == kSlotOe) ? !slotSet : slotSet;
        if (pinSet) mask |= (1u << kControlWideBits[i]);
    }
    return mask;
}

}  // namespace

void buildRepackTables(RepackTables& out) {
    // Zero first (the loops below fill every entry, but a partial write must not leave stale data): memset, not `out = RepackTables{}`, which materializes a 1,280-byte temporary on this stack, the failure this signature avoids.
    std::memset(&out, 0, sizeof(out));
    hub75FillColorTables(out.color, kLanePins, wideBit);
    for (unsigned bits = 0; bits < 128; bits++) out.control[bits] = controlMask(bits);
}

void repackSlots(const uint16_t* const laneSlots[kHub75Lanes], int lanes,
                 size_t slots, const RepackTables& tables, uint32_t* wideOut) {
    assert(lanes >= 1 && lanes <= kHub75Lanes);
    assert(laneSlots[0] != nullptr && wideOut != nullptr);
    for (size_t i = 0; i < slots; i++) {
        const uint16_t s0 = laneSlots[0][i];
        const uint32_t w = tables.color[0][s0 & 0x3F] | tables.control[(s0 >> 8) & 0x7F];
        wideOut[i] = hub75OrLaneColors(w, laneSlots, lanes, i, tables.color);
    }
}

void applyLatchBlanking(uint32_t* wide, size_t slots, size_t rowWords,
                        int blankWords) {
    if (blankWords <= 0 || rowWords == 0) return;
    assert(wide != nullptr);
    // Each row group is rowWords slots, its last the latch word: blank the `blankWords` slots before it (set the OE pin bit); k < rowWords keeps the index inside the row for one-column geometries (no size_t underflow).
    const uint32_t oeBit = (1u << kWideBitOe);
    for (size_t rowStart = 0; rowStart < slots; rowStart += rowWords) {
        const size_t rowEnd = rowStart + rowWords;  // one past the latch
        if (rowEnd > slots) break;
        for (int k = 1; k <= blankWords && static_cast<size_t>(k) < rowWords; k++)
            wide[rowEnd - 1 - static_cast<size_t>(k)] |= oeBit;
    }
}

uint32_t packWideWordReference(uint16_t slot0, uint16_t slot1, uint16_t slot2,
                               int lanes) {
    const uint16_t slotByLane[kHub75Lanes] = {slot0, slot1, slot2};
    uint32_t w = hub75ReferenceColors(slotByLane, lanes, kLanePins, wideBit);
    // Control from lane 0.
    for (int i = 0; i < 7; i++) {
        const bool slotSet = (slot0 & (1u << kControlSlotBits[i])) != 0;
        const bool pinSet = (i == 6) ? !slotSet : slotSet;  // OE inverted
        if (pinSet) w |= (1u << kControlWideBits[i]);
    }
    return w;
}

}  // namespace mm::platform
