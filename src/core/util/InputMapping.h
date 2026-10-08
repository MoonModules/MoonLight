#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/module/Control.h"        // ListSource: a mapping list is a list like any other
#include "core/util/JsonSink.h"       // writeListRow emits a row as JSON
#include "core/util/JsonUtil.h"       // parsing a row field, and restoring the persisted list
#include "core/module/Scheduler.h"      // setControl: the one generic control-set primitive

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/// @defgroup InputMapping What a physical input does
/// @{
/// The binding table behind every input service: a row says what one input does to one control.
///
/// Shared by button, infrared and every later transport, because the half that differs between them is only how the event is detected.
/// What happens next is identical, so it lives here once rather than in each module.
///
/// @moreinfo
///
/// ## The target is the surface
///
/// A row names a surface control, a switch, an encoder, a fader or a pad, and the surface decides what that control drives.
/// One place then shows what the device's controls do, and every transport reaches the same switch.
/// A row stores the bank and the number rather than a name, so it cannot point anywhere else.
/// The name, such as `Control.switch1`, exists only at the edges: the wire, the persisted list and the interface.
///
/// ## A pad is a row, not a control
///
/// A pad grid renders from a list, so a pad is reached through that list rather than by control name.
/// Every such list already publishes each row's slot and applies a row, so this needs to know nothing about presets.
/// Any module that grows a pad grid therefore becomes targetable by every input at once.
/// Resolving it here rather than in each service is what makes a button, a remote and every later transport reach a pad the same way.
///
/// A pad fires on the press alone. A release firing it again would re-apply the same preset for no reason, and would make a momentary row unusable.
///
/// ## An event and a reading are different shapes
///
/// A press carries no number, so a Set row writes the fixed value it was given and zero on release.
/// An analog input's value IS the reading, so a row's stored value has nothing to say, and the two paths stay separate rather than sharing a parameter.
/// They differ in what they do with every kind: a toggle or a delta driven fifty times a second is not something a user can mean.
///
/// A reading arrives in the range every surface control uses and is rescaled to whatever the target holds.
/// So a pedal is configured once and works on any target, rather than needing a range per target.
///
/// ## Reading and writing in the control's own units
///
/// A value is read through the scheduler at the control's declared width, and a delta clamps to the control's own bounds rather than wrapping.
///
/// ## A Set needs a release to clear it
///
/// A button reports letting go; a remote does not, since a code arrives as a single event with no matching release.
/// So a Set is offered only where a release exists to clear it, or it would write its value and latch forever.
/// An input without one offers toggle and delta, which are both complete in one event.
///
/// ## A name the surface does not have is refused
///
/// The whole suffix of a name must parse, and the number must be in that bank's own range.
/// A trailing character would otherwise fire the wrong pad, and a number past the range would wrap through the cast and fire another one entirely.
/// A name set through the interface that is not a surface control is refused.
/// A persisted one leaves its row unassigned and the service says so on its status, so a row never points somewhere the editor cannot show.
namespace mm {

/// What a row's value may hold, stated once and used by both the editor's bounds and the field that parses an edit.
inline constexpr int kMinActionValue = -32768;
inline constexpr int kMaxActionValue = 32767;

/// The largest pad a target may name; a number past it addresses nothing, so it is refused rather than wrapped.
inline constexpr unsigned long kMaxPadNumber = 64;


/// What one physical input does: a surface control, and how the input changes it.
/// @xref{the-target-is-the-surface|what a target may name}.
struct InputAction {
    /// How the input changes its target; a toggle is its own kind because a delta cannot express it.
    enum class Kind : uint8_t {
        Toggle = 0,   ///< read the current value, write its inverse. A light switch.
        Set,          ///< write `value`. A momentary hold writes 1 then 0; a pad writes a slot.
        Delta,        ///< add `value` to the target, clamped to its declared bounds. A nudge.
    };

    uint8_t type = 0;             ///< the surface bank, an index into kTargetTypes; 0 for an unassigned row
    uint8_t number = 1;           ///< which control of that bank, from 1
    Kind    kind = Kind::Toggle;   ///< which of the three this row does
    int16_t value = 0;            ///< Set: what to write. Delta: the signed nudge. Toggle: unused.

    /// Whether this row names a target at all.
    bool assigned() const { return type != 0; }
};

/// The surface banks an input can point at, a type plus a number: @xref{the-target-is-the-surface|why only the surface}.
inline constexpr const char* kTargetTypes[] = {
    "",           // unassigned: a row that drives nothing yet
    "switch",     // Control.switchN, toggled
    "encoder",    // Control.encoderN, nudged
    "fader",      // Control.faderN, nudged
    "pad",        // Control.padN, fired: a preset slot, resolved through ControlModule::firePad
};
/// The bank a pad lives in, fired through its list rather than written.
inline constexpr uint8_t kPadType = 4;
/// The module that owns the surface.
inline constexpr const char* kSurfaceModule = "Control";
inline constexpr uint8_t kTargetTypeCount = sizeof(kTargetTypes) / sizeof(kTargetTypes[0]);

/// The highest number each target type has, indexed by type, so a parse cannot accept a control that does not exist.
inline constexpr unsigned long kTargetTypeMaxNumber[kTargetTypeCount] = {
    0,               // unassigned
    8,               // switch
    8,               // encoder
    8,               // fader
    kMaxPadNumber,   // pad
};

/// Whether a target type is numbered; a named test, so a future unnumbered type reads clearly.
inline bool targetTypeIsNumbered(uint8_t type) { return type >= 1 && type < kTargetTypeCount; }

/// A target's name, such as `Control.switch1`, or the empty string for an unassigned row.
inline void targetName(char* out, size_t outLen, uint8_t type, uint8_t number) {
    if (type == 0 || type >= kTargetTypeCount) { out[0] = 0; return; }
    mm::formatTo(out, outLen, "%s.%s%u", kSurfaceModule, kTargetTypes[type], static_cast<unsigned>(number));
}

/// Read a name into a type and a number, false when it is not a surface control: @xref{a-name-the-surface-does-not-have-is-refused|what is refused}.
inline bool parseTarget(const char* target, uint8_t& type, uint8_t& number) {
    type = 0;
    number = 1;
    if (!target || !target[0]) return true;   // unassigned is a valid row
    const size_t prefix = std::strlen(kSurfaceModule);
    if (std::strncmp(target, kSurfaceModule, prefix) != 0 || target[prefix] != '.') return false;
    const char* name = target + prefix + 1;
    for (uint8_t i = 1; i < kTargetTypeCount; i++) {
        const size_t len = std::strlen(kTargetTypes[i]);
        if (std::strncmp(name, kTargetTypes[i], len) != 0) continue;
        const char* digits = name + len;
        // Digits rather than merely something, or a letter reports index zero.
        if (*digits < '0' || *digits > '9') continue;
        char* end = nullptr;
        const unsigned long n = std::strtoul(digits, &end, 10);
        // Bounded by THIS type's count, not by the largest of them: see kTargetTypeMaxNumber.
        if (*end != 0 || n < 1 || n > kTargetTypeMaxNumber[i]) continue;
        type = i;
        number = static_cast<uint8_t>(n);
        return true;
    }
    return false;
}

/// Fire the pad in a grid position, false when there is nothing there: @xref{a-pad-is-a-row-not-a-control|why through the list}.
inline bool firePadRow(MoonModule& mod, uint8_t slot, const char* label,
                       char* outStatus, size_t statusLen) {
    auto& cs = mod.controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (cs[i].type != ControlType::List) continue;
        auto* src = static_cast<ListSource*>(cs[i].ptr);
        if (!src || !src->listAsPads()) continue;
        for (uint8_t row = 0; row < src->listRowCount(); row++) {
            // The row's summary is the only place a slot is published, and a press is a human-rate event, so serializing one row per candidate costs nothing that matters.
            char buf[256];
            JsonSink sink(buf, sizeof(buf));
            src->writeListRow(sink, row);
            if (static_cast<int>(slot) != mm::json::parseInt(buf, "slot")) continue;
            const auto id = static_cast<uint32_t>(mm::json::parseInt(buf, "id"));
            const bool ok = src->applyListRow(id);
            if (outStatus) std::snprintf(outStatus, statusLen, ok ? "%s fired" : "%s refused", label);
            return ok;
        }
        if (outStatus) std::snprintf(outStatus, statusLen, "%s is empty", label);
        return false;
    }
    if (outStatus) std::snprintf(outStatus, statusLen, "%s has no pads", label);
    return false;
}

/// The surface control a row names, its descriptor found on the surface module, or null with a status saying why.
inline const ControlDescriptor* surfaceControl(const InputAction& a, MoonModule*& surface, char* control, size_t controlLen,
                                               char* outStatus, size_t statusLen) {
    mm::formatTo(control, controlLen, "%s%u", kTargetTypes[a.type], static_cast<unsigned>(a.number));
    Scheduler* sched = Scheduler::instance();
    surface = sched ? sched->firstByName(kSurfaceModule) : nullptr;
    if (!surface) {
        if (outStatus) mm::formatTo(outStatus, statusLen, "no %s module", kSurfaceModule);
        return nullptr;
    }
    const ControlList& ctrls = surface->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++)
        if (std::strcmp(ctrls[i].name, control) == 0) return &ctrls[i];
    if (outStatus) mm::formatTo(outStatus, statusLen, "%s has no %s", kSurfaceModule, control);
    return nullptr;
}

/// Write a surface control through the one primitive every transport uses, clamped to its bounds.
inline void writeSurfaceControl(const ControlDescriptor& c, const char* control, int next,
                                char* outStatus, size_t statusLen) {
    if (next < c.min) next = static_cast<int>(c.min);
    if (next > c.max) next = static_cast<int>(c.max);
    char valueJson[32];
    // A boolean takes true or false and everything else a number.
    if (c.type == ControlType::Bool) mm::formatTo(valueJson, sizeof(valueJson), "{\"value\":%s}", next ? "true" : "false");
    else                             mm::formatTo(valueJson, sizeof(valueJson), "{\"value\":%d}", next);
    if (Scheduler* sched = Scheduler::instance()) sched->setControl(kSurfaceModule, control, valueJson);
    if (outStatus) mm::formatTo(outStatus, statusLen, "%s -> %d", control, next);
}

/// Apply an action to its surface control, false when the row is unassigned or the surface lacks the control.
inline bool runInputAction(const InputAction& a, bool pressed,
                           char* outStatus, size_t statusLen) {
    if (!a.assigned()) return false;
    char control[16];
    MoonModule* surface = nullptr;
    // A pad is a row rather than a control: @xref{a-pad-is-a-row-not-a-control|how it resolves}.
    if (a.type == kPadType) {
        // On the press alone, a pad firing once.
        if (!pressed) return false;
        surfaceControl(a, surface, control, sizeof(control), nullptr, 0);
        if (!surface) return false;
        return firePadRow(*surface, static_cast<uint8_t>(a.number - 1), control, outStatus, statusLen);
    }
    const ControlDescriptor* c = surfaceControl(a, surface, control, sizeof(control), outStatus, statusLen);
    if (!c) return false;
    // Read at the control's own declared width: @xref{reading-and-writing-in-the-controls-own-units|why}.
    int32_t current = 0;
    if (!Scheduler::instance()->getControlWide(kSurfaceModule, control, current)) return false;
    int next = 0;
    switch (a.kind) {
        case InputAction::Kind::Toggle: next = current == 0 ? 1 : 0; break;
        case InputAction::Kind::Set:    next = pressed ? a.value : 0; break;
        case InputAction::Kind::Delta:  next = static_cast<int>(current) + a.value; break;
    }
    writeSurfaceControl(*c, control, next, outStatus, statusLen);
    return true;
}

/// Drive a surface control with a continuous value, rescaled into its range: @xref{an-event-and-a-reading-are-different-shapes|why this is separate from the event path}.
inline bool runInputLevel(const InputAction& a, uint8_t level,
                          char* outStatus, size_t statusLen) {
    if (!a.assigned()) return false;
    // A pad is momentary, so an analog row pointed at one does nothing rather than firing repeatedly on the way past.
    if (a.type == kPadType) {
        if (outStatus) mm::formatTo(outStatus, statusLen, "pad%u: a pad takes a press", static_cast<unsigned>(a.number));
        return false;
    }
    char control[16];
    MoonModule* surface = nullptr;
    const ControlDescriptor* c = surfaceControl(a, surface, control, sizeof(control), outStatus, statusLen);
    if (!c) return false;
    const int lo = static_cast<int>(c->min), hi = static_cast<int>(c->max);
    // Rounded rather than truncated, or a pedal pushed all the way lands one short of the maximum.
    const int next = hi > lo ? lo + static_cast<int>((static_cast<int32_t>(level) * (hi - lo) + 127) / 255) : lo;
    writeSurfaceControl(*c, control, next, outStatus, statusLen);
    return true;
}

/// The action half of a row as a document, emitted by every service so one row shape renders wherever it came from.
inline void writeInputActionFields(JsonSink& sink, const InputAction& a) {
    char name[24];
    targetName(name, sizeof(name), a.type, a.number);
    sink.append(",\"target\":");
    sink.writeJsonString(name);
    sink.append(",\"kind\":");
    sink.writeJsonString(a.kind == InputAction::Kind::Toggle ? "toggle"
                       : a.kind == InputAction::Kind::Set    ? "set" : "delta");
    sink.appendf(",\"value\":%d", static_cast<int>(a.value));
}

/// The target-type options as a shared set, emitted once per list rather than per row.
inline void writeInputTargetOptions(JsonSink& sink) {
    // The contents only, the serializer having already opened the object; a second brace made the document invalid and blanked the whole interface.
    sink.append("\"targets\":[");
    for (uint8_t i = 0; i < kTargetTypeCount; i++) {
        if (i) sink.append(",");
        sink.writeJsonString(kTargetTypes[i][0] ? kTargetTypes[i] : "(none)");
    }
    sink.append("]");
}

/// The target half alone, for an input whose value is the reading: a kind and a value would be stored, shown and ignored.
inline void writeInputTargetDetailField(JsonSink& sink, const InputAction& a) {
    sink.appendf("{\"name\":\"target\",\"type\":\"select\",\"optionsRef\":\"targets\",\"value\":%d},"
                 "{\"name\":\"number\",\"type\":\"uint8\",\"value\":%d}",
                 static_cast<int>(a.type), static_cast<int>(a.number));
}

/// The action half as editable fields, so a user retargets an input without the interface: @xref{a-set-needs-a-release-to-clear-it|why a Set is not always offered}.
inline void writeInputActionDetailFields(JsonSink& sink, const InputAction& a,
                                         bool hasRelease = true) {
    // A dropdown and a number rather than a text box, a typo otherwise being invisible until the input does nothing.
    sink.appendf("{\"name\":\"target\",\"type\":\"select\",\"optionsRef\":\"targets\",\"value\":%d},"
                 "{\"name\":\"number\",\"type\":\"uint8\",\"value\":%d},",
                 static_cast<int>(a.type), static_cast<int>(a.number));
    // All three positions stay whatever is usable, the parser mapping an index straight onto the kind; dropping one would make a delta arrive as the latch this prevents.
    const int kindValue = static_cast<int>(a.kind);
    sink.appendf("{\"name\":\"kind\",\"type\":\"select\",\"value\":%d,"
                 "\"options\":%s},"
                 // A signed range, since a delta's whole point is that it can go down and the renderer honors the bounds.
                 "{\"name\":\"value\",\"type\":\"uint8\",\"value\":%d,"
                 "\"min\":%d,\"max\":%d}",
                 kindValue,
                 hasRelease ? "[\"toggle\",\"set\",\"delta\"]"
                            : "[\"toggle\",\"set (needs a release)\",\"delta\"]",
                 static_cast<int>(a.value),
                 static_cast<int>(kMinActionValue), static_cast<int>(kMaxActionValue));
}

/// Set one action field from a row edit; false for a field this does not own, so a module can try its own afterwards.
inline bool setInputActionField(InputAction& a, const char* field, const char* valueJson) {
    if (std::strcmp(field, "target") == 0) {
        // A name from the interface or an index from the dropdown, both landing on a bank and a number.
        char buf[24] = {};
        json::parseString(valueJson, "value", buf, sizeof(buf));
        if (buf[0]) {
            // Parsed aside, so a refused name leaves the row as it was.
            uint8_t type = 0, number = 1;
            if (!parseTarget(buf, type, number)) return false;
            a.type = type;
            a.number = number;
            return true;
        }
        const int type = json::parseInt(valueJson, "value");
        if (type < 0 || type >= kTargetTypeCount) return false;
        a.type = static_cast<uint8_t>(type);
        // The number the row already had, within the new bank.
        if (a.number > kTargetTypeMaxNumber[a.type]) a.number = static_cast<uint8_t>(kTargetTypeMaxNumber[a.type] ? kTargetTypeMaxNumber[a.type] : 1);
        return true;
    }
    if (std::strcmp(field, "number") == 0) {
        // An unassigned row has no bank to number, and the spinner renders for every row.
        if (a.type == 0) return false;
        const int number = json::parseInt(valueJson, "value");
        if (number < 1 || number > static_cast<int>(kTargetTypeMaxNumber[a.type])) return false;
        a.number = static_cast<uint8_t>(number);
        return true;
    }
    if (std::strcmp(field, "kind") == 0) {
        // A name or an index, one field serving both callers with no second name for it.
        char buf[16] = {};
        json::parseString(valueJson, "value", buf, sizeof(buf));
        if (buf[0] == 0) {
            const int idx = json::parseInt(valueJson, "value");
            if (idx < 0 || idx > 2) return false;
            a.kind = static_cast<InputAction::Kind>(idx);
            return true;
        }
        if (std::strcmp(buf, "toggle") == 0)     a.kind = InputAction::Kind::Toggle;
        else if (std::strcmp(buf, "set") == 0)   a.kind = InputAction::Kind::Set;
        else if (std::strcmp(buf, "delta") == 0) a.kind = InputAction::Kind::Delta;
        else return false;   // an unknown kind is refused rather than silently defaulted
        return true;
    }
    if (std::strcmp(field, "value") == 0) {
        // Clamped rather than cast, or a number past the field's range wraps into a delta that steps the wrong way.
        const int v = json::parseInt(valueJson, "value");
        a.value = static_cast<int16_t>(v < kMinActionValue ? kMinActionValue
                                     : v > kMaxActionValue ? kMaxActionValue : v);
        return true;
    }
    return false;
}

/// Read a persisted row's action, false when its target is not a surface control and the row was left unassigned.
inline bool readInputAction(const json::JsonDoc& doc, const json::JsonNode* el, InputAction& a) {
    char target[32] = {};
    json::readString(json::member(doc, el, "target"), target, sizeof(target));
    const bool onSurface = parseTarget(target, a.type, a.number);
    char kind[16] = {};
    json::readString(json::member(doc, el, "kind"), kind, sizeof(kind));
    a.kind = std::strcmp(kind, "set") == 0   ? InputAction::Kind::Set
           : std::strcmp(kind, "delta") == 0 ? InputAction::Kind::Delta
                                             : InputAction::Kind::Toggle;
    a.value = static_cast<int16_t>(json::readInt(json::member(doc, el, "value")));
    return onSurface;
}

/// Say on a service's status how many restored rows lost a target that was not on the surface.
inline void reportOffSurface(MoonModule& service, char* buf, size_t len, unsigned dropped) {
    if (!dropped) return;
    mm::formatTo(buf, len, "%u row%s unassigned: not on the surface", dropped, dropped == 1 ? "" : "s");
    service.setStatus(buf, MoonModule::Severity::Warning);
}

/// @}
}  // namespace mm
