#pragma once

#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/module/Scheduler.h"
#include "platform/platform.h"   // gpioInputBegin / gpioRead / gpioWrite
#include "core/moonlive/MoonLiveBuiltins_common.h"   // the neutral half: math, waveforms, print

#include <cstdint>
#include <cstdio>
#include <cstring>

/// @defgroup moonlive_builtins_service MoonLive service builtins
/// @{
/// The service vocabulary: reading hardware and driving controls, rather than painting lights.
///
/// It lives in core because an input is domain-neutral, and a service must run on a device with no lights configured at all.
///
/// @moreinfo
///
/// ## Its own table
///
/// A service has no canvas, so a table offering pixel writes or coordinates would promise what it cannot keep.
/// It gets the two things that make it a service instead: it reads a pin and it writes a control.

namespace mm::moonlive {

// A contact closes for tens of milliseconds, so a slow script costs its own tick, not the lights.
/// The moment a service runs: the 50 Hz poll, rather than the render frame.
inline constexpr const char* kEntryTick20ms = "tick20ms";

// --- The host functions -------------------------------------------------------------------------

// Pulled up on first use, and debouncing is the script's: it knows what is wired.
/// Read a pin, as 0 or 1.
extern "C" inline uint32_t mm_service_gpioRead(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t pin = static_cast<uint32_t>(args[0]);
    if (pin > 48) return 0;                      // out of range on every supported chip
    // Once per pin: gpioInputBegin sets the resting level, so a per-read call hides every press.
    static bool opened[49] = {};
    if (!opened[pin]) {
        // Cached only on success: marking a refused pin open reported a floating read as a button.
        if (!platform::gpioInputBegin(static_cast<uint8_t>(pin), platform::GpioPull::Up)) return 0;
        opened[pin] = true;
    }
    return platform::gpioRead(static_cast<uint8_t>(pin)) ? 1u : 0u;
}

// Returns 0 rather than failing silently on a pin with no output driver, which a script cannot see.
/// Drive a pin, reporting whether the write took.
extern "C" inline uint32_t mm_service_gpioWrite(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t pin = static_cast<uint32_t>(args[0]);
    if (pin > 48) return 0;
    return platform::gpioWrite(static_cast<uint8_t>(pin), args[1] != 0) ? 1u : 0u;
}

// Raw counts, with `adcMax()` for full scale, so a script normalizes without knowing the chip.
/// Read a pin's ADC count, or 0 where it has none.
extern "C" inline uint32_t mm_service_adcRead(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t pin = static_cast<uint32_t>(args[0]);
    if (pin > 48) return 0;
    uint16_t raw = 0;
    if (!platform::adcRead(static_cast<uint8_t>(pin), raw)) return 0;
    return raw;
}

// A raw count is not a fixed fraction of full scale, so scaling it by hand misleads.
/// Read a pin's voltage in millivolts, or 0 where the chip carries no calibration.
extern "C" inline uint32_t mm_service_adcMv(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t pin = static_cast<uint32_t>(args[0]);
    if (pin > 48) return 0;
    uint16_t mv = 0;
    if (!platform::adcReadMv(static_cast<uint8_t>(pin), mv)) return 0;
    return mv;
}

// So a script scales against the chip it runs on rather than a number typed into its source.
/// The full-scale count `adcRead` reports on this platform.
extern "C" inline uint32_t mm_service_adcMax(const uintptr_t*, uint32_t, const uint8_t*) {
    return platform::adcMaxCount();
}

// The control module alone, so a script cannot rewrite a pin list by naming it.
/// Write a control, reporting whether the write took.
extern "C" inline uint32_t mm_service_setControl(const uintptr_t* args, uint32_t, const uint8_t*) {
    const char* name = reinterpret_cast<const char*>(args[0]);
    if (!name || !name[0]) return 0;
    Scheduler* sched = Scheduler::instance();
    if (!sched) return 0;
    // A JSON object read for its "value" key, since a bare number would not parse.
    char valueJson[32];
    std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}",
                  static_cast<int>(static_cast<int32_t>(args[1])));
    // Through the primitive every transport uses, so a script write looks like an OSC message.
    return sched->setControl("Control", name, valueJson) == Scheduler::SetControlResult::Ok ? 1u : 0u;
}

// --- The tables ---------------------------------------------------------------------------------

// No `width` or `height`: a service has no grid, and a variable always reading zero is a trap.
/// A service script's system variables, which is elapsed milliseconds alone.
inline SysVarTable serviceSysVars() {
    SysVarTable t;
    t.add({"t", SysVarKind::Arg, kArg3});
    return t;
}

/// The service built-in table the binding injects into the compiler.
inline const BuiltinTable& serviceBuiltins() {
    // Built once and reused: the table is around 2 KB by value and never changes after registration.
    static const BuiltinTable table = [] {
        BuiltinTable t;
    // The neutral half: a service gets sin, noise, beat, print and addControl, because none of it is about light.
    addCommonBuiltins(t);
    // gpioRead(pin)          -> 0/1. The input half: any switch, PIR, or level a pin can carry.
    t.add({"gpioRead", 1, /*returns*/ true, BuiltinKind::Call, &mm_service_gpioRead, {}});
    // gpioWrite(pin, on)     -> 0/1. The output half: a relay, an indicator, a chip's enable line.
    t.add({"gpioWrite", 2, /*returns*/ true, BuiltinKind::Call, &mm_service_gpioWrite, {}});
    // adcRead(pin)           -> raw counts. The analog half: a pedal, a pot, a sense divider.
    t.add({"adcRead", 1, /*returns*/ true, BuiltinKind::Call, &mm_service_adcRead, {}});
    // adcMv(pin) -> millivolts, the form a voltage or current sensor needs.
    t.add({"adcMv", 1, /*returns*/ true, BuiltinKind::Call, &mm_service_adcMv, {}});
    // adcMax() -> this platform's full scale, so a script normalizes without a magic number.
    t.add({"adcMax", 0, /*returns*/ true, BuiltinKind::Call, &mm_service_adcMax, {}});
    // setControl(name, v) -> 0/1, where byStr 0x1 marks the first argument as a quoted name.
    t.add({"setControl", 2, /*returns*/ true, BuiltinKind::Call, &mm_service_setControl, {},
           /*byRef*/ 0, /*byStr*/ 0x1});
        return t;
    }();
    return table;
}

/// @}

}  // namespace mm::moonlive
