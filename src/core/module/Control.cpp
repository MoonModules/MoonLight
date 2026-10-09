/// The per-type JSON writing and parsing every consumer shares, so adding a ControlType is one edit here.
/// Kept out of Control.h, which the module headers include only to call addX().

#include "core/module/Control.h"

#include "core/util/JsonSink.h"
#include "core/system/Base64.h"   // the password obfuscation's encoding
#include "core/util/JsonUtil.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace mm {

/// The longest option or palette name a value is saved and matched by; a parsed value longer than this was truncated, so it names nothing.
constexpr size_t kMaxLabel = 63;

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
        case ControlType::Button:      // momentary action, no value to save
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

// One task serves the API and the WebSocket pushes, so a plain flag scoped around each is enough.
static bool g_secretsHidden = false;

SecretsHidden::SecretsHidden(bool hide) MM_NONBLOCKING : prev_(g_secretsHidden) { g_secretsHidden = hide; }
SecretsHidden::~SecretsHidden() MM_NONBLOCKING { g_secretsHidden = prev_; }
bool SecretsHidden::active() MM_NONBLOCKING { return g_secretsHidden; }

void writeObfuscatedPassword(JsonSink& sink, const char* password) {
    if (g_secretsHidden) { sink.append("\"\""); return; }
    constexpr uint8_t kKey = 0x5A;   // shared with app.js's decodePassword
    uint8_t scrambled[64];
    size_t n = std::strlen(password);
    if (n > sizeof(scrambled)) n = sizeof(scrambled);
    for (size_t k = 0; k < n; k++) scrambled[k] = static_cast<uint8_t>(password[k]) ^ kKey;
    char encoded[96];
    base64Encode(std::span(scrambled).first(n), std::span(encoded));
    sink.appendf("\"%s\"", encoded);
}

void writeControlValue(JsonSink& sink, const ControlDescriptor& c, bool saving) {
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
            // Char buffers, written straight into the sink with no truncation; a Password is plain here for persistence, and the API obfuscates it through writeObfuscatedPassword.
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
                char name[kMaxLabel + 1] = {};
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
            // The row summaries, straight from the module's own data; the details ride writeControlMetadata, so the value stays what the collapsed list shows.
            const auto* src = static_cast<const ListSource*>(c.ptr);
            sink.append("[");
            if (src) {
                const uint8_t n = src->listRowCount();
                for (uint8_t r = 0; r < n; r++) {
                    if (r > 0) sink.append(",");
                    // Saving writes what the file keeps, which may be more than the collapsed row shows.
                    if (saving) src->writeListRowSaved(sink, r);
                    else src->writeListRow(sink, r);
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
            // A real [min,max]: a slider's range, and for a Pin, which renders as a plain number, the valid GPIO span.
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
            // int32_t, the type of the bound it is compared against, so a wider option count cannot wrap the counter.
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
            // The detail an expanded row shows, as an array parallel to the summary `value`, which stays small for the collapsed list.
            const auto* src = static_cast<const ListSource*>(c.ptr);
            // Option sets shared by the rows (ListSource::writeListOptionSets), which reference them by name.
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
    // An absent key leaves the control as it is, since parseInt reads a missing key as 0 and would clobber a non-zero default a partial file omits.
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
            // Strict: out-of-range fails. Clamp: snap into [min, max], whose default spans the whole uint16 range.
            if (policy == ApplyPolicy::Strict && (v < c.min || v > c.max)) {
                return ApplyResult::OutOfRange;
            }
            return clampInto(static_cast<uint16_t*>(c.ptr), v, c.min, c.max);
        }
        case ControlType::Int16: {
            int v = mm::json::parseInt(json, key);
            // Strict: out-of-range fails. Clamp: snap into [min, max], which also prevents a narrowing wrap (40000 to -25536).
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
            // Every char-buffer type parses as Text, into c.max bytes, wide enough for a script source.
            size_t maxLen = static_cast<size_t>(c.max > 0 ? c.max : 16);
            // A validator sees the whole value in a scratch buffer before the write, so a rejected value leaves the stored one untouched on every write path.
            if (c.validate) {
                static constexpr size_t kScratch = 1024;   // ≥ any validated Text/TextArea/Password buffer
                // A buffer wider than the scratch is rejected rather than validated truncated; kScratch is where to grow it.
                if (maxLen > kScratch) return ApplyResult::Malformed;
                char scratch[kScratch];
                mm::json::parseString(json, key, scratch, maxLen);
                if (!c.validate(scratch)) return ApplyResult::Malformed;
                // snprintf rather than strncpy, which leaves a full buffer unterminated and which GCC flags for that.
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
            // A label as well as an index, so a config ports across chips whose option lists differ; an overlong label matches none.
            char label[kMaxLabel + 2] = {};   // one past the longest name, so a longer value shows as such
            mm::json::parseString(json, key, label, sizeof(label));
            const bool overlong = std::strlen(label) > kMaxLabel;
            if (label[0]) {
                auto* options = reinterpret_cast<const char* const*>(c.aux);
                if (options && !overlong) {
                    for (int i = 0; i <= hi; i++)
                        if (options[i] && std::strcmp(options[i], label) == 0)
                            return clampInto(static_cast<uint8_t*>(c.ptr), i, 0, hi);
                    // Then on the head before ", " on both sides, since a live detail after it, such as a link speed, changes.
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
            char label[kMaxLabel + 2] = {};   // one past the longest name, so a longer value shows as such
            mm::json::parseString(json, key, label, sizeof(label));
            if (label[0] && c.aux) {
                if (std::strlen(label) <= kMaxLabel) {
                    for (int i = 0; i <= hi; i++) {
                        char name[kMaxLabel + 1] = {};
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
            // The source parses the array and repopulates itself, from a saved file or a state document.
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
