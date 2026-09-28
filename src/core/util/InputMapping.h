#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/module/Control.h"        // ListSource: a mapping list is a list like any other
#include "core/util/JsonSink.h"       // writeListRow emits a row as JSON
#include "core/util/JsonUtil.h"       // parsing a row field, and restoring the persisted list
#include "core/module/Scheduler.h"      // setControl: the one generic control-set primitive

#include <cstdint>
#include <cstdio>
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
/// ## The target is a string, and the surface is the recommended one
///
/// A row names `Module.control`, so an input can drive the control surface or a module control directly.
/// Both are the same mechanism with no special case: the surface is a recommendation rather than a rule.
/// Driving the surface is the better path, since one place then shows what the device's controls do and every transport reaches the same switch.
///
/// The editor offers only the surface. An earlier draft also offered a few module controls directly, which was a second path to the same place.
/// Two ways to say one thing is the split brain the two-step model exists to avoid, and it left a user wondering which a given row used.
/// A row can still name any control through the interface, which is the escape hatch for anything the surface does not carry yet.
///
/// ## A pad is a row, not a control
///
/// A pad grid renders from a list, so a pad is reached through that list rather than by control name.
/// Every such list already publishes each row's slot and accepts an activate field, so this needs to know nothing about presets.
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
/// A value is read through the scheduler, which answers at the control's declared width.
/// Reading the raw pointer as a byte was wrong, a wider control holding a large value reading back truncated.
/// The surface's own byte reader clamps too, so a positive delta wrote the wrong number and a negative one could never move a control down.
/// A mapping nudges the CONTROL rather than the surface, so it reads in the control's units and clamps to the control's own bounds.
///
/// A select and a palette store their option COUNT, so the last valid index is one below it.
/// Clamping to the count produced a value the writer clamped again, and a delta that overshot stopped short of the end instead of landing on it.
///
/// ## A Set needs a release to clear it
///
/// A button reports letting go; a remote does not, since a code arrives as a single event with no matching release.
/// So a Set is offered only where a release exists to clear it, or it would write its value and latch forever.
/// An input without one offers toggle and delta, which are both complete in one event.
///
/// ## A target the vocabulary cannot express does nothing
///
/// The whole suffix of a name must parse, and the number must be in that type's own range.
/// A trailing character would otherwise fire the wrong pad, and a number past the range would wrap through the cast and fire another one entirely.
/// One shared bound would let a number past the surface's count through: it parses, it is stored, and it dispatches to nothing.
///
/// A target set through the interface to something the editor cannot represent reads back as unassigned, so the dropdown shows nothing while the row keeps working.
/// That is the honest reading, and silently rewriting it would be worse.
namespace mm {

/// What a row's value may hold, stated once and used by both the editor's bounds and the field that parses an edit.
inline constexpr int kMinActionValue = -32768;
inline constexpr int kMaxActionValue = 32767;

/// The largest pad a target may name; a number past it addresses nothing, so it is refused rather than wrapped.
inline constexpr unsigned long kMaxPadNumber = 64;


/// What one physical input does: a target control, and how the input changes it.
/// @xref{the-target-is-a-string-and-the-surface-is-the-recommended-one|what a target may name}.
struct InputAction {
    /// How the input changes its target; a toggle is its own kind because a delta cannot express it.
    enum class Kind : uint8_t {
        Toggle = 0,   ///< read the current value, write its inverse. A light switch.
        Set,          ///< write `value`. A momentary hold writes 1 then 0; a pad writes a slot.
        Delta,        ///< add `value` to the target, clamped to its declared bounds. A nudge.
    };

    char    target[32] = "";      ///< "Module.control", empty for an unassigned row
    Kind    kind = Kind::Toggle;   ///< which of the three this row does
    int16_t value = 0;            ///< Set: what to write. Delta: the signed nudge. Toggle: unused.

    /// Whether this row names a target at all.
    bool assigned() const { return target[0] != 0; }
};

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
            const bool ok = src->setListRowField(id, "activate", "{\"value\":true}");
            if (outStatus) std::snprintf(outStatus, statusLen, ok ? "%s fired" : "%s refused", label);
            return ok;
        }
        if (outStatus) std::snprintf(outStatus, statusLen, "%s is empty", label);
        return false;
    }
    if (outStatus) std::snprintf(outStatus, statusLen, "%s has no pads", label);
    return false;
}

/// Apply an action to its target through the one primitive every transport uses, false when the row is unassigned or names something absent.
inline bool runInputAction(const InputAction& a, bool pressed,
                           char* outStatus, size_t statusLen) {
    if (!a.assigned()) return false;

    // Split at the dot; a target without one is refused rather than half-applied against a module named for the whole string.
    const char* dot = std::strchr(a.target, '.');
    if (!dot || dot == a.target || !dot[1]) {
        if (outStatus) mm::formatTo(outStatus, statusLen, "%s: not Module.control", a.target);
        return false;
    }
    char module[24] = {};
    const size_t n = static_cast<size_t>(dot - a.target);
    if (n >= sizeof(module)) {
        if (outStatus) mm::formatTo(outStatus, statusLen, "module name too long");
        return false;
    }
    std::memcpy(module, a.target, n);
    const char* control = dot + 1;

    Scheduler* sched = Scheduler::instance();
    if (!sched) return false;
    MoonModule* target = sched->firstByName(module);
    if (!target) {
        if (outStatus) std::snprintf(outStatus, statusLen, "no %s module", module);
        return false;
    }

    // A pad is a row rather than a control: @xref{a-pad-is-a-row-not-a-control|how it resolves}.
    if (std::strncmp(control, "pad", 3) == 0 && control[3] >= '0' && control[3] <= '9') {
        // On the press alone, a pad firing once.
        if (!pressed) return false;
        // The whole suffix must parse and be in range: @xref{a-target-the-vocabulary-cannot-express-does-nothing|what a trailing character would fire}.
        char* end = nullptr;
        const unsigned long padNr = std::strtoul(control + 3, &end, 10);
        if (*end != 0 || padNr < 1 || padNr > kMaxPadNumber) return false;
        return firePadRow(*target, static_cast<uint8_t>(padNr - 1), a.target, outStatus, statusLen);
    }

    // The descriptor, for the current value and the bounds a delta clamps to and a toggle inverts.
    const ControlList& ctrls = target->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        const ControlDescriptor& c = ctrls[i];
        if (std::strcmp(c.name, control) != 0) continue;

        char valueJson[32];
        // Read at the control's own declared width: @xref{reading-and-writing-in-the-controls-own-units|what a byte read got wrong}.
        int32_t current = 0;
        if (!sched->getControlWide(module, control, current)) return false;
        int next = 0;
        switch (a.kind) {
            case InputAction::Kind::Toggle: next = current == 0 ? 1 : 0; break;
            case InputAction::Kind::Set:    next = pressed ? a.value : 0; break;
            case InputAction::Kind::Delta:  next = current + a.value; break;
        }
        // A select and a palette store their option COUNT, so the last valid index is one below it.
        const int hi = (c.type == ControlType::Select || c.type == ControlType::Palette)
                           ? static_cast<int>(c.max) - 1 : static_cast<int>(c.max);
        if (next < c.min) next = c.min;
        if (next > hi) next = hi;

        // A boolean takes true or false and everything else a number, both through the one primitive.
        if (c.type == ControlType::Bool)
            std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%s}", next ? "true" : "false");
        else
            std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", next);
        sched->setControl(module, control, valueJson);
        if (outStatus) std::snprintf(outStatus, statusLen, "%s -> %d", a.target, next);
        return true;
    }
    if (outStatus) std::snprintf(outStatus, statusLen, "%s has no %s", module, control);
    return false;
}

/// Drive a target with a continuous value, rescaled into the control's own range: @xref{an-event-and-a-reading-are-different-shapes|why this is separate from the event path}.
inline bool runInputLevel(const InputAction& a, uint8_t level,
                          char* outStatus, size_t statusLen) {
    if (!a.assigned()) return false;
    const char* dot = std::strchr(a.target, '.');
    if (!dot || dot == a.target || !dot[1]) {
        if (outStatus) mm::formatTo(outStatus, statusLen, "%s: not Module.control", a.target);
        return false;
    }
    char module[24] = {};
    const size_t n = static_cast<size_t>(dot - a.target);
    if (n >= sizeof(module)) {
        if (outStatus) mm::formatTo(outStatus, statusLen, "module name too long");
        return false;
    }
    std::memcpy(module, a.target, n);
    const char* control = dot + 1;

    Scheduler* sched = Scheduler::instance();
    if (!sched) return false;
    MoonModule* target = sched->firstByName(module);
    if (!target) {
        if (outStatus) std::snprintf(outStatus, statusLen, "no %s module", module);
        return false;
    }
    // A pad is momentary, so an analog row pointed at one does nothing rather than firing repeatedly on the way past.
    if (std::strncmp(control, "pad", 3) == 0) {
        if (outStatus) std::snprintf(outStatus, statusLen, "%s: a pad takes a press", a.target);
        return false;
    }

    const ControlList& ctrls = target->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        const ControlDescriptor& c = ctrls[i];
        if (std::strcmp(c.name, control) != 0) continue;
        // The control's own range, the same bound the event path applies.
        const int hi = (c.type == ControlType::Select || c.type == ControlType::Palette)
                           ? static_cast<int>(c.max) - 1 : static_cast<int>(c.max);
        const int lo = static_cast<int>(c.min);
        int next = lo;
        if (hi > lo) {
            // Rounded rather than truncated, or a pedal pushed all the way lands one short of the maximum.
            const int32_t span = static_cast<int32_t>(hi) - lo;
            next = lo + static_cast<int>((static_cast<int32_t>(level) * span + 127) / 255);
        }
        if (next < lo) next = lo;
        if (next > hi) next = hi;

        char valueJson[32];
        if (c.type == ControlType::Bool)
            std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%s}", next ? "true" : "false");
        else
            std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", next);
        sched->setControl(module, control, valueJson);
        if (outStatus) std::snprintf(outStatus, statusLen, "%s -> %d", a.target, next);
        return true;
    }
    if (outStatus) std::snprintf(outStatus, statusLen, "%s has no %s", module, control);
    return false;
}

/// The action half of a row as a document, emitted by every service so one row shape renders wherever it came from.
inline void writeInputActionFields(JsonSink& sink, const InputAction& a) {
    sink.append(",\"target\":");
    sink.writeJsonString(a.target);
    sink.append(",\"kind\":");
    sink.writeJsonString(a.kind == InputAction::Kind::Toggle ? "toggle"
                       : a.kind == InputAction::Kind::Set    ? "set" : "delta");
    sink.appendf(",\"value\":%d", static_cast<int>(a.value));
}

/// The target types an input can point at, a type plus a number: @xref{the-target-is-a-string-and-the-surface-is-the-recommended-one|why only the surface}.
inline constexpr const char* kTargetTypes[] = {
    "",           // unassigned: a row that drives nothing yet
    "switch",     // Control.switchN, toggled
    "encoder",    // Control.encoderN, nudged
    "fader",      // Control.faderN, nudged
    "pad",        // Control.padN, fired: a preset slot, resolved through ControlModule::firePad
};
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

/// Build the stored target string from a type and a number, the two being how a user edits it rather than how it is kept.
inline void composeTarget(char* out, size_t outLen, uint8_t type, uint8_t number) {
    if (type == 0 || type >= kTargetTypeCount) { out[0] = 0; return; }
    std::snprintf(out, outLen, "Control.%s%u", kTargetTypes[type], static_cast<unsigned>(number));
}

/// Read a stored target back into a type and a number for the editor: @xref{a-target-the-vocabulary-cannot-express-does-nothing|what an unrepresentable one reads as}.
inline void decomposeTarget(const char* target, uint8_t& type, uint8_t& number) {
    type = 0;
    number = 1;
    if (!target || !target[0]) return;
    // Anything the editor cannot represent reads back as unassigned while the row keeps working.
    if (std::strncmp(target, "Control.", 8) != 0) return;
    const char* name = target + 8;
    for (uint8_t i = 1; i < kTargetTypeCount; i++) {
        const size_t len = std::strlen(kTargetTypes[i]);
        if (std::strncmp(name, kTargetTypes[i], len) != 0) continue;
        const char* digits = name + len;
        // Digits rather than merely something, or a letter reports index zero and re-composes to a control that does not exist.
        if (*digits < '0' || *digits > '9') continue;
        // The whole suffix, or a trailing character silently retargets a row the editor touched.
        char* end = nullptr;
        const unsigned long n = std::strtoul(digits, &end, 10);
        // Bounded by THIS type's count, not by the largest of them: see kTargetTypeMaxNumber.
        if (*end != 0 || n < 1 || n > kTargetTypeMaxNumber[i]) continue;
        type = i;
        number = static_cast<uint8_t>(n);
        return;
    }
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
    uint8_t type = 0, number = 1;
    decomposeTarget(a.target, type, number);
    sink.appendf("{\"name\":\"target\",\"type\":\"select\",\"optionsRef\":\"targets\",\"value\":%d},"
                 "{\"name\":\"number\",\"type\":\"uint8\",\"value\":%d}",
                 static_cast<int>(type), static_cast<int>(number));
}

/// The action half as editable fields, so a user retargets an input without the interface: @xref{a-set-needs-a-release-to-clear-it|why a Set is not always offered}.
inline void writeInputActionDetailFields(JsonSink& sink, const InputAction& a,
                                         bool hasRelease = true) {
    // A dropdown and a number rather than a text box, a typo otherwise being invisible until the input does nothing.
    uint8_t type = 0, number = 1;
    decomposeTarget(a.target, type, number);
    sink.appendf("{\"name\":\"target\",\"type\":\"select\",\"optionsRef\":\"targets\",\"value\":%d},"
                 "{\"name\":\"number\",\"type\":\"uint8\",\"value\":%d},",
                 static_cast<int>(type), static_cast<int>(number));
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
        // A string from the interface or an index from the dropdown, both writing the one stored format.
        char buf[sizeof(a.target)] = {};
        json::parseString(valueJson, "value", buf, sizeof(buf));
        if (buf[0]) { std::snprintf(a.target, sizeof(a.target), "%s", buf); return true; }
        uint8_t oldType = 0, number = 1;
        decomposeTarget(a.target, oldType, number);   // keep the number the row already had
        const int type = json::parseInt(valueJson, "value");
        if (type < 0 || type >= kTargetTypeCount) return false;
        composeTarget(a.target, sizeof(a.target), static_cast<uint8_t>(type), number);
        return true;
    }
    if (std::strcmp(field, "number") == 0) {
        // Editing the number re-composes the target, so there is no separate stored number to drift from the string.
        uint8_t type = 0, oldNr = 1;
        decomposeTarget(a.target, type, oldNr);
        // Re-composing from an unrepresentable target would clear the string, and the spinner renders for every row.
        if (type == 0) return false;
        const int number = json::parseInt(valueJson, "value");
        if (number < 1 || number > 64) return false;   // the surface's banks are 8; a pad grid is 64
        composeTarget(a.target, sizeof(a.target), type, static_cast<uint8_t>(number));
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

/// @}
}  // namespace mm
