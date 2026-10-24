#pragma once

#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/module/Scheduler.h"
#include "platform/platform.h"   // gpioInputBegin / gpioRead / gpioWrite
#include "core/moonlive/MoonLiveBuiltins_common.h"   // the neutral half: math, waveforms, print
#include "core/util/JsonFetch.h"   // the documents a script's json values name

#include <cstdint>
#include <cstdio>
#include <cstring>

/// The service vocabulary: reading hardware and driving controls, rather than painting lights.
///
/// It lives in core because an input is domain-neutral, and a service must run on a device with no lights configured at all.
///
/// @technical
///
/// ## Its own table
///
/// A service has no canvas, so a table offering pixel writes or coordinates would promise what it cannot keep. It gets the things that make it a service instead: it reads a pin, it writes a control, and it reads and sends JSON.

namespace mm::moonlive {

/// The moment a service runs: the 50 Hz poll, not the render frame, because a contact closes for tens of milliseconds and a slow script then costs its own tick.
inline constexpr const char* kEntryTick20ms = "tick20ms";

// --- The host functions -------------------------------------------------------------------------

/// Read a pin as 0 or 1, pulled up on first use; debouncing is the script's, since it knows what is wired.
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

/// Drive a pin, returning 0 on a pin with no output driver, which a script cannot see otherwise.
extern "C" inline uint32_t mm_service_gpioWrite(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t pin = static_cast<uint32_t>(args[0]);
    if (pin > 48) return 0;
    return platform::gpioWrite(static_cast<uint8_t>(pin), args[1] != 0) ? 1u : 0u;
}

/// Read a pin's raw ADC count, or 0 where it has none; `adcMax()` gives full scale, so a script normalizes without knowing the chip.
extern "C" inline uint32_t mm_service_adcRead(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t pin = static_cast<uint32_t>(args[0]);
    if (pin > 48) return 0;
    uint16_t raw = 0;
    if (!platform::adcRead(static_cast<uint8_t>(pin), raw)) return 0;
    return raw;
}

/// Read a pin's voltage in millivolts, or 0 where the chip carries no calibration, since a raw count is not a fixed fraction of full scale.
extern "C" inline uint32_t mm_service_adcMv(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t pin = static_cast<uint32_t>(args[0]);
    if (pin > 48) return 0;
    uint16_t mv = 0;
    if (!platform::adcReadMv(static_cast<uint8_t>(pin), mv)) return 0;
    return mv;
}

/// The full-scale count `adcRead` reports on this platform, so a script scales against the chip it runs on.
extern "C" inline uint32_t mm_service_adcMax(const uintptr_t*, uint32_t, const uint8_t*) {
    return platform::adcMaxCount();
}

/// Write a control of the control module alone, so a script cannot rewrite a pin list by naming it; returns whether the write took.
extern "C" inline uint32_t mm_service_setControl(const uintptr_t* args, uint32_t, const uint8_t*) {
    const char* name = reinterpret_cast<const char*>(args[0]);
    if (!name || !name[0]) return 0;
    Scheduler* sched = Scheduler::instance();
    if (!sched) return 0;
    // A JSON object read for its "value" key, since a bare number would not parse.
    char valueJson[32];
    std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}",
                  static_cast<int>(static_cast<int32_t>(args[1])));
    // Through the primitive every transport uses, so a script write looks like an OSC message; live, since a script drives a value rather than configures it, so nothing is saved.
    return sched->setControl("Control", name, valueJson, /*live=*/true) == Scheduler::SetControlResult::Ok ? 1u : 0u;
}

// --- JSON: thin wrappers over JsonFetch and JsonUtil, the compiled reach -------------------------

/// The document a handle names for the calling script, or null: the documents live in `JsonFetch`, and the arena every host call receives names their owner.
inline json::JsonDoc* scriptDoc(uintptr_t handle, const uint8_t* arena) {
    return JsonFetch::instance().doc(arena, static_cast<int>(handle));
}

/// A document parsed from the script's own text, which the compiler also calls where a string literal stands for a json value.
extern "C" inline uint32_t mm_json_parse(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    return static_cast<uint32_t>(JsonFetch::instance().literal(arena, reinterpret_cast<const char*>(args[0])));
}

/// A document fetched from a URL, and again every `seconds` while the script reads it; one second at the least.
extern "C" inline uint32_t mm_json_get(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    const int32_t seconds = signedArg(args[1]);
    const uint32_t periodMs = seconds < 1 ? 1000u : static_cast<uint32_t>(seconds) * 1000u;
    return static_cast<uint32_t>(JsonFetch::instance().fetch(arena, reinterpret_cast<const char*>(args[0]), periodMs));
}

/// 1 when a new document arrived since the script last asked.
extern "C" inline uint32_t mm_json_changed(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    return JsonFetch::instance().changed(arena, static_cast<int>(args[0])) ? 1u : 0u;
}

/// The HTTP status of the last answer, 0 while none came.
extern "C" inline uint32_t mm_json_status(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    return static_cast<uint32_t>(JsonFetch::instance().status(arena, static_cast<int>(args[0])));
}

/// The number at a JSON Pointer, a quoted one included, as a whole number and in fixed point; zeros when there is none.
inline void scriptNumber(const uintptr_t* args, const uint8_t* arena, long& whole, int32_t& fixed) {
    whole = 0;
    fixed = 0;
    const json::JsonDoc* d = scriptDoc(args[0], arena);
    if (!d || !json::readNumber(json::at(*d, reinterpret_cast<const char*>(args[1])), whole, fixed)) { whole = 0; fixed = 0; }
}

/// The number at a JSON Pointer, truncated to a whole one; 0 when absent.
extern "C" inline uint32_t mm_json_int(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    long whole; int32_t fixed;
    scriptNumber(args, arena, whole, fixed);
    return static_cast<uint32_t>(static_cast<int32_t>(whole));
}

/// The number at a JSON Pointer as fixed point, its fraction kept; 0 when absent.
extern "C" inline uint32_t mm_json_fixed(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    long whole; int32_t fixed;
    scriptNumber(args, arena, whole, fixed);
    return static_cast<uint32_t>(fixed);
}

/// How many elements or members the array or object at a JSON Pointer holds; 0 for anything else.
extern "C" inline uint32_t mm_json_size(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    const json::JsonDoc* d = scriptDoc(args[0], arena);
    const json::JsonNode* n = d ? json::at(*d, reinterpret_cast<const char*>(args[1])) : nullptr;
    if (!n || (n->type != json::JsonType::Array && n->type != json::JsonType::Object)) return 0;
    uint32_t count = 0;
    for (int i = n->firstChild; i >= 0; i = d->nodes[i].next) count++;
    return count;
}

/// Set a whole number at a JSON Pointer, creating the members on the way; 1 when it took.
extern "C" inline uint32_t mm_json_setInt(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    json::JsonDoc* d = scriptDoc(args[0], arena);
    return d && json::setInt(*d, reinterpret_cast<const char*>(args[1]), signedArg(args[2])) ? 1u : 0u;
}

/// Set a fixed-point number at a JSON Pointer; 1 when it took.
extern "C" inline uint32_t mm_json_setFixed(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    json::JsonDoc* d = scriptDoc(args[0], arena);
    return d && json::setFixed(*d, reinterpret_cast<const char*>(args[1]), signedArg(args[2])) ? 1u : 0u;
}

/// Set the script's own text at a JSON Pointer; 1 when it took.
extern "C" inline uint32_t mm_json_setText(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    json::JsonDoc* d = scriptDoc(args[0], arena);
    return d && json::setString(*d, reinterpret_cast<const char*>(args[1]), reinterpret_cast<const char*>(args[2])) ? 1u : 0u;
}

/// Append the script's own text to the string at a JSON Pointer; 1 when it took.
extern "C" inline uint32_t mm_json_appendText(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    json::JsonDoc* d = scriptDoc(args[0], arena);
    return d && json::appendText(*d, reinterpret_cast<const char*>(args[1]), reinterpret_cast<const char*>(args[2])) ? 1u : 0u;
}

/// Append another document's value, as text, to the string at a JSON Pointer; 1 when it took.
extern "C" inline uint32_t mm_json_append(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    json::JsonDoc* dst = scriptDoc(args[0], arena);
    const json::JsonDoc* src = scriptDoc(args[2], arena);
    return dst && src && json::appendValue(*dst, reinterpret_cast<const char*>(args[1]), *src,
                                           reinterpret_cast<const char*>(args[3])) ? 1u : 0u;
}

/// Copy the value at one document's pointer to another's, text and whole objects included; 1 when it took.
extern "C" inline uint32_t mm_json_copy(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    json::JsonDoc* dst = scriptDoc(args[0], arena);
    const json::JsonDoc* src = scriptDoc(args[2], arena);
    return dst && src && json::copyValue(*dst, reinterpret_cast<const char*>(args[1]), *src,
                                         reinterpret_cast<const char*>(args[3])) ? 1u : 0u;
}

/// Apply a document to this device, live, and answer a handle to the reply.
extern "C" inline uint32_t mm_json_push(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    return static_cast<uint32_t>(JsonFetch::instance().push(arena, nullptr, static_cast<int>(args[0])));
}

/// Send a document to another device by name, or to a URL, and answer a handle to the reply.
extern "C" inline uint32_t mm_json_pushTo(const uintptr_t* args, uint32_t, const uint8_t* arena) {
    return static_cast<uint32_t>(JsonFetch::instance().push(arena, reinterpret_cast<const char*>(args[0]),
                                                            static_cast<int>(args[1])));
}

/// Register the JSON builtins: `json` converts a literal and the rest read, write and send documents.
inline void addJsonBuiltins(BuiltinTable& t) {
    auto add = [&t](const char* name, uint8_t argc, HostCallFn fn, uint8_t jsonArgs, uint8_t byStr,
                    bool jsonReturn, uint8_t fixedArgs = 0, bool fixedReturn = false) {
        Builtin b{name, argc, /*returns*/ true, BuiltinKind::Call, fn, {}};
        b.jsonArgs = jsonArgs;
        b.byStr = byStr;
        b.jsonReturn = jsonReturn;
        b.fixedArgs = fixedArgs;
        b.fixedReturn = fixedReturn;
        t.add(b);
    };
    add("json",         1, &mm_json_parse,    0x0, 0x1, true);
    add("getJson",      2, &mm_json_get,      0x0, 0x1, true);
    add("jsonChanged",  1, &mm_json_changed,  0x1, 0x0, false);
    add("jsonStatus",   1, &mm_json_status,   0x1, 0x0, false);
    add("jsonInt",      2, &mm_json_int,      0x1, 0x2, false);
    add("jsonFixed",    2, &mm_json_fixed,    0x1, 0x2, false, 0x0, true);
    add("jsonSize",     2, &mm_json_size,     0x1, 0x2, false);
    add("jsonSetInt",   3, &mm_json_setInt,   0x1, 0x2, false);
    add("jsonSetFixed", 3, &mm_json_setFixed, 0x1, 0x2, false, 0x4);
    add("jsonSetText",  3, &mm_json_setText,  0x1, 0x6, false);
    add("jsonCopy",     4, &mm_json_copy,     0x5, 0xA, false);
    add("jsonAppendText", 3, &mm_json_appendText, 0x1, 0x6, false);
    add("jsonAppend",   4, &mm_json_append,   0x5, 0xA, false);
    add("pushJson",     1, &mm_json_push,     0x1, 0x0, true);
    add("pushJsonTo",   2, &mm_json_pushTo,   0x2, 0x1, true);
}

// --- The tables ---------------------------------------------------------------------------------

/// A service script's system variables, which is elapsed milliseconds alone: a service has no grid, and a variable always reading zero is a trap.
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
    // The JSON vocabulary: read the internet, build this device's look, send another device its own.
    addJsonBuiltins(t);
        return t;
    }();
    return table;
}

}  // namespace mm::moonlive
