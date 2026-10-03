/// Why .h + .cpp (Control is now in the core-services file-shape list, see docs/contributing/coding-standards.md § File shape): Control.h started as declarations + inline scalar helpers.
/// The JSON serialization / parsing logic grew to six switches across three files (HttpServerModule, FilesystemModule, scenario_runner).
/// Centralising them here keeps Control.h light for the 20+ MoonModule headers that include it just to call addX() and makes "add a new ControlType" a single-place edit instead of a hunt across three consumers, the "per-type behavior lives with the type" rule in docs/contributing/coding-standards.md applied to wire-format serialization.

#include "core/module/Control.h"

#include "core/util/JsonSink.h"
#include "core/util/JsonUtil.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace mm {

const char* controlTypeName(ControlType t) {
    switch (t) {
        case ControlType::Uint8:       return "uint8";
        case ControlType::Uint16:      return "uint16";
        case ControlType::Int16:       return "int16";
        case ControlType::Int32:       return "int32";
        case ControlType::Pin:         return "pin";
        case ControlType::Bool:        return "bool";
        case ControlType::Text:        return "text";
        case ControlType::TextArea:    return "textarea";
        case ControlType::FilePath:    return "filepath";
        case ControlType::Password:    return "password";
        case ControlType::ReadOnly:    return "display";
        case ControlType::ReadOnlyInt: return "display-int";
        case ControlType::Select:      return "select";
        case ControlType::Palette:     return "palette";
        case ControlType::Progress:    return "progress";
        case ControlType::IPv4:        return "ipv4";
        case ControlType::List:        return "list";
        case ControlType::Button:      return "button";
    }
    return "unknown";
}

bool isPersistable(const ControlDescriptor& c) {
    // LIVE STATE is never written: a value something drives continuously (a script sweeping a fader, a sensor reading) is not configuration, whatever its type. See ControlDescriptor::live.
    if (c.live) return false;
    // A List defers to its source: rows re-derived at setup are not worth writing (see ListSource::persistsList). Every other type answers from the type alone.
    if (c.type == ControlType::List) {
        auto* src = static_cast<ListSource*>(c.ptr);
        if (src && !src->persistsList()) return false;
    }
    return isPersistable(c.type);
}

bool isPersistable(ControlType t) {
    // Display-only / device-derived types: no point saving, the next tick1s overwrites them.
    switch (t) {
        case ControlType::ReadOnly:
        case ControlType::ReadOnlyInt:
        case ControlType::Progress:
        case ControlType::Button:      // momentary action — no value to save
            return false;
        case ControlType::List:
            // Persistable now: the List value is a JSON array the recursive mm::json reader round-trips, restored via ListSource::restoreList (see applyControlValue). The source owns its (de)serialization.
            return true;
        default:
            return true;
    }
}

bool hasDefault(ControlType t) {
    // Defaults are emitted in /api/types so the UI can render a reset-to-default ↺ button. Password is excluded (a default would defeat the secret); the non-persistable types are also excluded (no user input shape to seed).
    if (!isPersistable(t)) return false;
    return t != ControlType::Password;
}

void writeControlValue(JsonSink& sink, const ControlDescriptor& c) {
    switch (c.type) {
        case ControlType::Uint8:
            sink.appendf("%u", *static_cast<uint8_t*>(c.ptr));
            return;
        case ControlType::Uint16:
            sink.appendf("%u", *static_cast<uint16_t*>(c.ptr));
            return;
        case ControlType::Int16:
            sink.appendf("%d", *static_cast<int16_t*>(c.ptr));
            return;
        case ControlType::Int32:
            // int is 32-bit on every target; int32_t is `long` on Xtensa, so %d alone mismatches.
            sink.appendf("%d", static_cast<int>(*static_cast<int32_t*>(c.ptr)));
            return;
        case ControlType::Pin:   // int8_t storage; serialized as a plain integer
            sink.appendf("%d", *static_cast<int8_t*>(c.ptr));
            return;
        case ControlType::Bool:
            sink.append(*static_cast<bool*>(c.ptr) ? "true" : "false");
            return;
        case ControlType::Text:
        case ControlType::TextArea:
        case ControlType::FilePath:
        case ControlType::Password:
        case ControlType::ReadOnly:
            // All char-buffer-backed. Password is rendered as a plain JSON string here; the HTTP API obfuscates separately at the writeControls call site (persistence writes plaintext). writeJsonString walks the source straight into the sink with no intermediate fixed buffer, so there's no truncation ceiling regardless of the source buffer's length.
            sink.writeJsonString(static_cast<char*>(c.ptr));
            return;
        case ControlType::ReadOnlyInt:
            sink.appendf("%d", *static_cast<int8_t*>(c.ptr));
            return;
        case ControlType::Select:
            // persistLabel: the option STRING, for enumerated option lists whose index is not stable across boots; the apply path matches it back by label. Otherwise the index.
            if (c.persistLabel && c.aux) {
                const uint8_t sel = *static_cast<uint8_t*>(c.ptr);
                auto* options = reinterpret_cast<const char* const*>(c.aux);
                if (sel < c.max) { sink.writeJsonString(options[sel]); return; }
            }
            sink.appendf("%u", *static_cast<uint8_t*>(c.ptr));
            return;
        case ControlType::Palette:
            // persistLabel: the NAME, since a scripted palette's index moves as files come and go; the swatches ride in writeControlMetadata.
            if (c.persistLabel && c.aux) {
                char name[64] = {};
                JsonSink names(name, sizeof(name));
                names.requestName(*static_cast<uint8_t*>(c.ptr));
                reinterpret_cast<PaletteOptionsFn>(c.aux)(names);
                if (name[0] && !names.overflowed()) { sink.writeJsonString(name); return; }
            }
            sink.appendf("%u", *static_cast<uint8_t*>(c.ptr));
            return;
        case ControlType::Progress:
            sink.appendf("%lu",
                         static_cast<unsigned long>(*static_cast<uint32_t*>(c.ptr)));
            return;
        case ControlType::IPv4: {
            char ipStr[16];
            formatDottedQuad(ipStr, static_cast<const uint8_t*>(c.ptr));
            sink.appendf("\"%s\"", ipStr);
            return;
        }
        case ControlType::List: {
            // value is an array of row summary objects; the source writes each object straight from the module's own data (no copy, no per-row alloc). Detail objects ride the metadata block (writeControlMetadata) so the value stays the lightweight summary the collapsed UI shows.
            const auto* src = static_cast<const ListSource*>(c.ptr);
            sink.append("[");
            if (src) {
                const uint8_t n = src->listRowCount();
                for (uint8_t r = 0; r < n; r++) {
                    if (r > 0) sink.append(",");
                    src->writeListRow(sink, r);
                }
            }
            sink.append("]");
            return;
        }
        case ControlType::Button:
            // Momentary action, no stored value. Emit a placeholder so the control object is well-formed JSON; the UI renders a button and ignores it.
            sink.append("0");
            return;
    }
}

void writeControlMetadata(JsonSink& sink, const ControlDescriptor& c) {
    // Before the switch: every branch below returns, and a declared default belongs to the control whatever its type is. Emitted only when one was set, so the wire format and every module that relies on the type-level defaults in /api/types are untouched.
    if (c.def != ControlDescriptor::kNoDefault) {
        sink.appendf(",\"default\":%d", static_cast<int>(c.def));
    }
    switch (c.type) {
        case ControlType::Uint8:
        case ControlType::Uint16:
        case ControlType::Int16:
        case ControlType::Int32:
        case ControlType::Pin:
            // Numeric controls carry a real [min,max]; the slider types render it as a range, Pin uses it only as a documented valid-GPIO span (the UI renders Pin as a plain number, keyed off the "pin" type string).
            sink.appendf(",\"min\":%d,\"max\":%d", static_cast<int>(c.min),
                         static_cast<int>(c.max));
            return;
        case ControlType::ReadOnlyInt: {
            // aux holds a borrowed const char* unit suffix (set via addReadOnlyInt). The UI renders "<value> <unit>" verbatim.
            const char* unit = reinterpret_cast<const char*>(c.aux);
            sink.appendf(",\"unit\":\"%s\"", unit ? unit : "");
            return;
        }
        case ControlType::Select: {
            sink.append(",\"options\":[");
            auto* options = reinterpret_cast<const char* const*>(c.aux);
            // int32_t, the type of the bound it is compared against.
            // A uint8_t counter against a wider signed max is a wrap that only today's uint8_t option count keeps unreachable, and the suppression that hid it outlived the reason for it.
            // Same form as the hash walk in MoonModule.h.
            for (int32_t o = 0; o < c.max; o++) {
                if (o > 0) sink.append(",");
                // Escaped, not a raw %s: most option lists are our own literals, but the panel-card interface Select carries OS-supplied adapter descriptions. One containing a quote or a backslash would make all of /api/state invalid and blank the UI.
                sink.writeJsonString(options[o] ? options[o] : "");
            }
            sink.append("]");
            return;
        }
        case ControlType::Palette: {
            // The light domain supplies the option objects ({name, colors}) via the function pointer in `aux`, core stays palette-agnostic. Falls back to an empty array.
            sink.append(",\"options\":[");
            if (c.aux) reinterpret_cast<PaletteOptionsFn>(c.aux)(sink);
            sink.append("]");
            return;
        }
        case ControlType::Progress:
            // `bytes` (in min, see addProgress): 1 → KB label, 0 → plain count.
            sink.appendf(",\"total\":%lu,\"bytes\":%s", static_cast<unsigned long>(c.aux),
                         c.min ? "true" : "false");
            return;
        case ControlType::List: {
            // The summary rows are the `value` (writeControlValue); the per-row detail (shown when a row expands) rides here as a parallel `detail` array, same length and order. Keeping detail out of `value` keeps the collapsed-list payload small when details are richer than summaries.
            const auto* src = static_cast<const ListSource*>(c.ptr);
            // Shared option sets, emitted once per list (rows reference them by name via optionsRef).
            // The contract lives on ListSource::writeListOptionSets (Control.h).
            // Default {}.
            sink.append(",\"optionSets\":{");
            if (src) src->writeListOptionSets(sink);
            sink.append("}");
            sink.append(",\"detail\":[");
            if (src) {
                const uint8_t n = src->listRowCount();
                for (uint8_t r = 0; r < n; r++) {
                    if (r > 0) sink.append(",");
                    src->writeListRowDetail(sink, r);
                }
            }
            sink.append("]");
            return;
        }
        // Everything else: no extras.
        case ControlType::Bool:
        case ControlType::Text:
        case ControlType::TextArea:
        case ControlType::Password:
        case ControlType::ReadOnly:
        case ControlType::IPv4:
        case ControlType::Button:
            return;
        // Where the module keeps its files, and which of them to offer. Both borrowed from the module (addFilePath), so the UI can list a directory without knowing what lives there.
        case ControlType::FilePath: {
            auto* pick = reinterpret_cast<const char* const*>(c.aux);
            if (!pick || !pick[0]) return;          // no picker: an editor with a fixed path
            sink.append(",\"dir\":");
            sink.writeJsonString(pick[0]);
            if (pick[1]) { sink.append(",\"ext\":"); sink.writeJsonString(pick[1]); }
            // What a NEW file starts as. Sent with the metadata rather than fetched: it is a property of the control, and it is the module that knows what a usable file holds.
            if (pick[2]) { sink.append(",\"tmpl\":"); sink.writeJsonString(pick[2]); }
            return;
        }
    }
}

ApplyResult applyControlValue(const ControlDescriptor& c,
                              const char* json, const char* key,
                              ApplyPolicy policy) {
    // Absent key → leave the control at its current value. parseInt/parseBool return 0/false for a missing key, indistinguishable from a real 0.
    // Applying them would clobber a control's non-zero default (e.g. eth phyType=2) when an older/partial persisted file omits the key.
    // The string types already no-op on absence (parseString returns early), but the numeric/select/bool types need this explicit guard.
    // Skipping is correct for both callers: the persistence overlay should preserve defaults for keys it didn't save, and an HTTP /api/control write always includes the key it sets.
    if (!mm::json::hasKey(json, key)) return ApplyResult::Ok;

    // Helper: clamp `v` into [lo, hi] and write to `*dst` of type T. Always returns Ok (clamping is the action, not a failure).
    auto clampInto = [](auto* dst, int v, int lo, int hi) {
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        using T = std::remove_pointer_t<decltype(dst)>;
        *dst = static_cast<T>(v);
        return ApplyResult::Ok;
    };
    switch (c.type) {
        case ControlType::Uint8: {
            int v = mm::json::parseInt(json, key);
            // Strict: out-of-range fails. Clamp: snap into [min, max].
            if (policy == ApplyPolicy::Strict && (v < c.min || v > c.max)) {
                return ApplyResult::OutOfRange;
            }
            return clampInto(static_cast<uint8_t*>(c.ptr), v, c.min, c.max);
        }
        case ControlType::Uint16: {
            int v = mm::json::parseInt(json, key);
            // Strict: out-of-[min,max] fails.
            // Clamp: snap into [min,max].
            // The descriptor's int32 min/max now carry a real uint16 range (default 0..UINT16_MAX = no constraint), so this matches Uint8/Int16.
            if (policy == ApplyPolicy::Strict && (v < c.min || v > c.max)) {
                return ApplyResult::OutOfRange;
            }
            return clampInto(static_cast<uint16_t*>(c.ptr), v, c.min, c.max);
        }
        case ControlType::Int16: {
            int v = mm::json::parseInt(json, key);
            // Strict: out-of-c.min/max fails.
            // Clamp: snap into [min, max].
            // Either way the type-range clamp prevents narrowing wrap (40000 → -25536).
            if (policy == ApplyPolicy::Strict && (v < c.min || v > c.max)) {
                return ApplyResult::OutOfRange;
            }
            return clampInto(static_cast<int16_t*>(c.ptr), v, c.min, c.max);
        }
        case ControlType::Int32: {
            int v = mm::json::parseInt(json, key);
            if (policy == ApplyPolicy::Strict && (v < c.min || v > c.max)) {
                return ApplyResult::OutOfRange;
            }
            return clampInto(static_cast<int32_t*>(c.ptr), v, c.min, c.max);
        }
        case ControlType::Pin: {   // int8_t storage; [min,max] = valid-GPIO span
            int v = mm::json::parseInt(json, key);
            if (policy == ApplyPolicy::Strict && (v < c.min || v > c.max)) {
                return ApplyResult::OutOfRange;
            }
            return clampInto(static_cast<int8_t*>(c.ptr), v, c.min, c.max);
        }
        case ControlType::Bool:
            *static_cast<bool*>(c.ptr) = mm::json::parseBool(json, key);
            return ApplyResult::Ok;
        case ControlType::Text:
        case ControlType::TextArea:
        case ControlType::FilePath:
        case ControlType::Password: {
            // TextArea, FilePath and Password parse identically to Text: only the UI render (TextArea) or serialization (Password) differs. c.max is the buffer size; parseString writes up to maxLen-1 then NUL-terminates. Passing c.max gives "fill the buffer". uint16_t (not uint8_t) so a large textarea (a script source, hundreds of bytes) isn't truncated to 255.
            size_t maxLen = static_cast<size_t>(c.max > 0 ? c.max : 16);
            // A per-control validator (if set) checks the incoming value before the write, so a reject leaves the stored value untouched (no partial write).
            // Parse into a scratch buffer first, validate, then commit, this is the one backend home every write path shares (HTTP, APPLY_OP, persistence).
            // The scratch matches the buffer's full size so a long-but-valid value isn't truncated before the validator sees it; it's sized to the largest validated text/textarea buffer.
            if (c.validate) {
                static constexpr size_t kScratch = 1024;   // ≥ any validated Text/TextArea/Password buffer
                // A buffer wider than the scratch is rejected, so a value is never truncated before the validator sees it (which would be a silent partial write). kScratch is the one place to grow if a validated control legitimately needs a larger buffer.
                if (maxLen > kScratch) return ApplyResult::Malformed;
                char scratch[kScratch];
                mm::json::parseString(json, key, scratch, maxLen);
                if (!c.validate(scratch)) return ApplyResult::Malformed;
                // snprintf, not strncpy: strncpy does NOT NUL-terminate when the source fills the buffer, so it needs the manual terminator that followed, a pattern GCC flags (-Wstringop-truncation) precisely because forgetting that line is a classic bug. snprintf always terminates and truncates identically. parseString already bounded the value to maxLen, so nothing is lost here.
                std::snprintf(static_cast<char*>(c.ptr), maxLen, "%s", scratch);
                return ApplyResult::Ok;
            }
            mm::json::parseString(json, key, static_cast<char*>(c.ptr), maxLen);
            return ApplyResult::Ok;
        }
        case ControlType::Select: {
            // An empty option list (c.max == 0) has no valid index at all, don't accept a value or manufacture index 0 for it. Strict rejects; Lenient leaves the control untouched.
            if (c.max == 0) return policy == ApplyPolicy::Strict ? ApplyResult::OutOfRange : ApplyResult::Ok;
            const int hi = c.max - 1;
            // A Select value may be given as the option LABEL (a string) instead of the index.
            // This is what makes a catalog config board-portable.
            // The index into a board-FILTERED option list varies per chip (an S3 offers fewer peripherals than a P4), but the label is stable.
            // Match the string against the options and use that row; fall back to the numeric index otherwise.
            // Select-only: a Select's aux IS the options array (const char* const*); Palette's aux is a PaletteOptionsFn (a function pointer), so it must not reach this reinterpret_cast. parseString silently truncates a value longer than the buffer, and a truncated label could spuriously equal a real option that happens to share its prefix.
            // Guard by sizing the buffer past any real option label AND rejecting a value that fills it: a label that reaches the cap is longer than any option (or was truncated to it), so it cannot legitimately match, treat it as "no such option" rather than risk a prefix match.
            char label[64] = {};
            mm::json::parseString(json, key, label, sizeof(label));
            const bool overlong = std::strlen(label) >= sizeof(label) - 1;
            if (label[0]) {
                auto* options = reinterpret_cast<const char* const*>(c.aux);
                if (options && !overlong) {
                    for (int i = 0; i <= hi; i++)
                        if (options[i] && std::strcmp(options[i], label) == 0)
                            return clampInto(static_cast<uint8_t*>(c.ptr), i, 0, hi);
                    // Then on the STABLE HEAD of the label, the part before ", ".
                    // An option may carry a live detail after that separator (the panel-card NIC list appends a link speed, "Realtek PCIe GbE, 1 Gb"), and matching the whole string would lose the user's pick the moment that detail changed: a renegotiated link, or the same NIC at 100 Mb instead of 1 Gb, would silently fall back to row 0.
                    // BOTH sides are cut at the separator.
                    // The persisted label carries the detail it was written with, and the option carries the current one, so comparing a whole label against a head never matches.
                    const char* lsep = std::strstr(label, ", ");
                    const size_t lhead = lsep ? static_cast<size_t>(lsep - label)
                                              : std::strlen(label);
                    for (int i = 0; i <= hi; i++) {
                        if (!options[i]) continue;
                        const char* sep = std::strstr(options[i], ", ");
                        const size_t head = sep ? static_cast<size_t>(sep - options[i])
                                                : std::strlen(options[i]);
                        if (head == lhead && std::strncmp(options[i], label, head) == 0)
                            return clampInto(static_cast<uint8_t*>(c.ptr), i, 0, hi);
                    }
                }
                // A label that names no current option (a peripheral this board can't run, or one too long to be any real option) is not an error in Lenient policy. The driver keeps its default; Strict rejects it.
                if (policy == ApplyPolicy::Strict) return ApplyResult::OutOfRange;
                return ApplyResult::Ok;
            }
            int v = mm::json::parseInt(json, key);
            if (policy == ApplyPolicy::Strict && (v < 0 || v > hi)) {
                return ApplyResult::OutOfRange;
            }
            return clampInto(static_cast<uint8_t*>(c.ptr), v, 0, hi);
        }
        case ControlType::Palette: {
            // An empty palette list (c.max == 0) has no valid index, reject/no-op like the Select above.
            if (c.max == 0) return policy == ApplyPolicy::Strict ? ApplyResult::OutOfRange : ApplyResult::Ok;
            const int hi = c.max - 1;
            // A name as well as an index, resolved through the options function's name request since aux is a function, not an array.
            char label[64] = {};
            mm::json::parseString(json, key, label, sizeof(label));
            if (label[0] && c.aux) {
                if (std::strlen(label) < sizeof(label) - 1) {   // one that fills the buffer was truncated, so it names nothing
                    for (int i = 0; i <= hi; i++) {
                        char name[sizeof(label)] = {};
                        JsonSink sink(name, sizeof(name));
                        sink.requestName(static_cast<uint8_t>(i));
                        reinterpret_cast<PaletteOptionsFn>(c.aux)(sink);
                        if (!sink.overflowed() && std::strcmp(name, label) == 0)
                            return clampInto(static_cast<uint8_t*>(c.ptr), i, 0, hi);
                    }
                }
                return policy == ApplyPolicy::Strict ? ApplyResult::OutOfRange : ApplyResult::Ok;
            }
            int v = mm::json::parseInt(json, key);
            if (policy == ApplyPolicy::Strict && (v < 0 || v > hi)) {
                return ApplyResult::OutOfRange;
            }
            return clampInto(static_cast<uint8_t*>(c.ptr), v, 0, hi);
        }
        case ControlType::IPv4: {
            char buf[16] = {};
            mm::json::parseString(json, key, buf, sizeof(buf));
            uint8_t octets[4] = {};
            if (!parseDottedQuad(buf, octets)) return ApplyResult::Malformed;
            std::memcpy(c.ptr, octets, 4);
            return ApplyResult::Ok;
        }
        case ControlType::ReadOnly:
        case ControlType::ReadOnlyInt:
        case ControlType::Progress:
            return ApplyResult::ReadOnly;
        case ControlType::List: {
            // Restore from persistence: hand the source the loaded JSON + this key so it parses the array (recursive mm::json reader) and repopulates itself. A live HTTP write to a List isn't a use case (discovery output), but the persistence-overlay load IS, and it arrives through this same path.
            auto* src = static_cast<ListSource*>(c.ptr);
            // Propagate a parse failure (malformed / missing array) as Malformed rather than masking it as Ok, a corrupt persisted list is a real apply failure.
            if (!src) return ApplyResult::ReadOnly;   // no source bound → nothing to restore
            return src->restoreList(json, key) ? ApplyResult::Ok : ApplyResult::Malformed;
        }
        case ControlType::Button:
            // No value to store, but return Ok (NOT ReadOnly): the HTTP handler runs onControlChanged() only on a non-error apply, and onControlChanged IS the button's action. ReadOnly would 400 and swallow the click.
            return ApplyResult::Ok;
    }
    return ApplyResult::Malformed;  // unreachable; quiets -Wreturn-type
}

} // namespace mm
