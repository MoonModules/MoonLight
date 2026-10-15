#pragma once

#include "core/util/PinList.h"        // parsePinList: the domain-neutral GPIO-CSV parser (core primitive)
#include "light/util/light_types.h"  // nrOfLightsType

#include <cstdint>
#include <cstdlib>  // std::strtol

namespace mm {

/// The light-domain half of pin and count list parsing for multi-output LED drivers.
///
/// @technical
///
/// `RmtLedDriver` takes one RMT channel per pin and `I80Peripheral` one i80 data lane per pin, each driving consecutive slices of the source buffer from two text controls.
/// The GPIO CSV parser `parsePinList` is a domain-neutral core primitive, while the count distribution here speaks `nrOfLightsType` and so stays in the light layer.
/// Both return null on success, or a static error literal for `setStatus`.
/// `unit_RmtLedDriver_pins.cpp` pins them on the host.
///
/// ## How a count list is read
///
/// `assignCounts` fills one count per pin from the `ledsPerPin` text.
/// It uses the broadcasting idiom NumPy and CSS share, where a scalar applies to all and a list maps element-wise, plus an auto-fit empty case.
///
/// | Written | What it means |
/// |---------|---------------|
/// | empty | an even split, the total over the pin count, the last pin taking the remainder |
/// | `N` | that many on every pin, broadcast |
/// | `3,4,5` | mapped per pin in order, a list shorter than the pins even-splitting the rest over those unlisted |
///
/// A list longer than the pin count ignores the extras, since a stale list after the pins shrank is not an error.
/// Explicit counts are clamped so the running sum never exceeds the total.
///
/// ## The per-pin ceiling
///
/// `maxPerPin` is the driver's protocol ceiling on lights per data line.
/// A pin exceeding it is clamped, so the driver drives the first `maxPerPin` and stays lit rather than choking on the rest.
///
/// It is per-protocol, so each driver passes its own.
/// A WS2812-class one-wire line, whether RMT, LCD_CAM or Parlio, clocks a fixed 30 microseconds a light, so 2048 a pin is already about 16 frames a second.
/// A clocked two-wire SPI type such as APA102 or SK9822 runs at tens of MHz and manages ten thousand or more a pin, so it passes a far higher cap.
/// Passing 0 means no ceiling.
/// The intended way to output fewer lights is the driver's start and count window rather than this safety cap.
///
/// On a clamp the caller's `warn` is set, a warning it shows while still running.
/// That is distinct from the return value, which stays null, because clamping is not an error that idles the driver.
///
/// ## What clamping does to the later pins
///
/// Offsets accumulate from the clamped counts, so clamping one pin shifts every later pin's source slice down by the trimmed amount.
/// For the headline case, a whole grid funneled onto one pin, that is exactly right: the pin drives the first `maxPerPin` and nothing follows it.
///
/// For the pathological case of several pins each over the ceiling, the later strips show a shifted window rather than a truncated one.
/// That is accepted rather than fixed: it degrades rather than crashes, and nobody wires the misconfiguration on purpose.
/// Preserving alignment would need a parallel array of unclamped counts, which is more state for a case the warning already flags.

/// The per-pin light ceiling for a WS2812-class one-wire protocol, past which output is a slideshow.
inline constexpr nrOfLightsType kMaxWs2812LedsPerPin = 2048;

/// The warning a caller shows when a pin's count was clamped to the ceiling.
inline constexpr const char* kClampedWarning =
    "some LEDs not driven: over per-pin max; add pins or use start/count";

/// Fill one count per pin from the `ledsPerPin` text, returning null or an error literal.
inline const char* assignCounts(const char* s, uint8_t nPins,
                                nrOfLightsType totalLights, nrOfLightsType* counts,
                                nrOfLightsType maxPerPin = 0, const char** warn = nullptr) {
    if (warn) *warn = nullptr;
    for (uint8_t i = 0; i < nPins; i++) counts[i] = 0;

    // A single value broadcasts: that many on every pin, clamped to the lights still unassigned.
    {
        const char* p = s;
        while (*p == ' ') p++;
        if (*p) {
            char* end = nullptr;
            const long v = std::strtol(p, &end, 10);
            if (end == p || v < 0) return "invalid count list";
            while (*end == ' ') end++;
            if (*end == '\0') {   // exactly one number → broadcast it
                nrOfLightsType remaining = totalLights;
                for (uint8_t i = 0; i < nPins; i++) {
                    const nrOfLightsType c =
                        (v > static_cast<long>(remaining)) ? remaining
                                                           : static_cast<nrOfLightsType>(v);
                    counts[i] = c;
                    remaining = static_cast<nrOfLightsType>(remaining - c);
                }
                if (maxPerPin > 0)
                    for (uint8_t i = 0; i < nPins; i++)
                        if (counts[i] > maxPerPin) { counts[i] = maxPerPin; if (warn) *warn = kClampedWarning; }
                return nullptr;
            }
        }
    }

    // Empty → even split; a list → map per pin (a short list even-splits the rest).
    nrOfLightsType remaining = totalLights;
    uint8_t nExplicit = 0;
    const char* p = s;
    while (p && *p && nExplicit < nPins) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p || v < 0) return "invalid count list";
        while (*end == ' ') end++;
        const nrOfLightsType c =
            (v > static_cast<long>(remaining)) ? remaining
                                               : static_cast<nrOfLightsType>(v);
        counts[nExplicit++] = c;
        remaining = static_cast<nrOfLightsType>(remaining - c);
        if (*end == '\0') break;
        if (*end != ',') return "invalid count list";
        p = end + 1;
    }
    const uint8_t nRemaining = static_cast<uint8_t>(nPins - nExplicit);
    if (nRemaining > 0) {
        const nrOfLightsType per = static_cast<nrOfLightsType>(remaining / nRemaining);
        for (uint8_t i = nExplicit; i < nPins; i++) counts[i] = per;
        counts[nPins - 1] = static_cast<nrOfLightsType>(
            counts[nPins - 1] + (remaining - per * nRemaining));
    }
    if (maxPerPin > 0)
        for (uint8_t i = 0; i < nPins; i++)
            if (counts[i] > maxPerPin) {
                counts[i] = maxPerPin;
                if (warn) *warn = kClampedWarning;
            }
    return nullptr;
}

} // namespace mm
