#pragma once

#include <cstdint>
#include <cstdlib>  // std::strtol
#include "core/module/Control.h"  // MM_MAX_GPIO — the build-injected per-chip GPIO ceiling

namespace mm {

/// One text control such as `18,17,16` turned into the pins a driver claims.
///
/// @technical
///
/// ## Why this lives in core
///
/// Both a light driver's `pins` control and the core `PinsModule` pin-ownership map read the same CSV.
/// The parser therefore sits in core, and the dependency runs from the domain into core rather than the other way.
/// It is `strtol`-based like `parseDottedQuad` in `Control.h`, and returns null on success or a static error literal the caller hands straight to `setStatus`.
/// `unit_RmtLedDriver_pins.cpp` and `unit_PinsModule.cpp` pin it on the host.
///
/// ## What a token may be
///
/// A token is a single pin such as `17` or an inclusive range such as `20-23`, and the two mix freely as in `20-22,35,38-40`.
/// That is the same range idiom the IP destination list uses, so a human types consecutive entries once, and spaces around tokens are fine because `strtol` skips them.
///
/// ## What gets rejected, and why the ceiling matters
///
/// The parser refuses empty input, bad tokens, trailing commas, duplicates, ranges that run backwards, pins above the chip's `MM_MAX_GPIO` ceiling, and more entries than the caller's lane cap allows.
///
/// The ceiling check is the crash guard.
/// A pin like 999 parses as a valid integer but is not a GPIO, and handing it to IDF's `gpio_func_sel` faults with a GPIO number error and resets the board.
/// Rejecting it at the one parse boundary every driver shares, RMT, Parlio and i80 alike, keeps a garbage pin from reaching hardware, so no driver re-guards it.

/// Append one pin, enforcing the chip ceiling, the duplicate check and the lane cap.
inline const char* appendPin(long v, uint16_t* out, uint8_t maxPins, uint8_t& nOut) {
    if (v < 0 || v > 0xFFFF) return "invalid pin list";
    if (v > MM_MAX_GPIO) return "pin out of range for this chip";
    if (nOut >= maxPins) return "too many pins for this chip";
    for (uint8_t i = 0; i < nOut; i++)
        if (out[i] == static_cast<uint16_t>(v)) return "duplicate pin";
    out[nOut++] = static_cast<uint16_t>(v);
    return nullptr;
}

/// Parse `s` into `out`, `nOut` receiving the count, returning null or an error literal.
inline const char* parsePinList(const char* s, uint16_t* out, uint8_t maxPins, uint8_t& nOut) {
    nOut = 0;
    if (!s || !*s) return "invalid pin list";
    const char* p = s;
    while (true) {
        char* end = nullptr;
        const long lo = std::strtol(p, &end, 10);
        if (end == p) return "invalid pin list";
        while (*end == ' ') end++;
        if (*end == '-') {                          // a RANGE: expand lo..hi inclusive
            const char* after = end + 1;
            char* e2 = nullptr;
            const long hi = std::strtol(after, &e2, 10);
            if (e2 == after) return "invalid pin range";
            if (hi < lo) return "pin range runs backwards";
            for (long v = lo; v <= hi; v++)
                if (const char* err = appendPin(v, out, maxPins, nOut)) return err;
            end = e2;
        } else {
            if (const char* err = appendPin(lo, out, maxPins, nOut)) return err;
        }
        while (*end == ' ') end++;
        if (*end == '\0') return nullptr;
        if (*end != ',') return "invalid pin list";
        p = end + 1;
    }
}

} // namespace mm
