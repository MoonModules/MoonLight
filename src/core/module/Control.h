#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef MM_MAX_GPIO   // the highest valid GPIO, overridden per chip; the fallback is the widest
#define MM_MAX_GPIO 63
#endif

namespace mm {

/// Parse "A.B.C.D" into four octets, returning false on anything else.
inline bool parseDottedQuad(const char* s, uint8_t out[4]) {
    if (!s) return false;
    int idx = 0;
    const char* p = s;
    while (idx < 4) {
        char* end = nullptr;
        long v = std::strtol(p, &end, 10);
        if (end == p || v < 0 || v > 255) return false;
        out[idx++] = static_cast<uint8_t>(v);
        if (idx == 4) {
            // Trailing junk (e.g. "1.2.3.4x") fails.
            return *end == '\0';
        }
        if (*end != '.') return false;
        p = end + 1;
    }
    return false;  // unreachable
}

/// Format four octets as "A.B.C.D", which sixteen bytes always fits.
inline void formatDottedQuad(char out[16], const uint8_t ip[4]) {
    mm::formatTo(out, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

/// Coerce a string in place into a valid hostname label, which the device name must be.
inline void sanitizeHostname(char* buf) {
    if (!buf) return;
    char* w = buf;                       // the write cursor, which compacts in place
    bool pendingDash = false;            // emit one hyphen before the next keeper
    for (const char* r = buf; *r; ++r) {
        const char c = *r;
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                       || (c >= '0' && c <= '9') || c == '-';
        if (keep) {
            // Never lead with a hyphen, from a run or from a literal one.
            if (c == '-' && w == buf) continue;
            if (pendingDash && w != buf) *w++ = '-';
            pendingDash = false;
            *w++ = c;
        } else {
            pendingDash = true;          // deferring drops a trailing run entirely
        }
    }
    // Literal hyphens are kept as written, so loop to strip every trailing one.
    while (w != buf && w[-1] == '-') --w;
    *w = '\0';
}

/// What a file-path control tells the UI: a directory, an extension and a template.
using FilePathPick = const char* const[3];

/// A control's type, which selects its storage, its widget and its DMX mapping.
enum class ControlType : uint8_t {
    Uint8,      ///< a byte, rendered as a slider, and the preferred default
    Uint16,     ///< a wide unsigned value, rendered as a number input
    Int16,      ///< a signed value, for a coordinate where negatives are legal
    Int32,      ///< a wide signed value, where sixteen bits would wrap
    Pin,        ///< a GPIO number, rendered as a number since a pin is an identity
    Bool,       ///< a toggle
    Text,       ///< a character buffer, rendered as a text input
    TextArea,   ///< a buffer rendered as a resizable multi-line box
    FilePath,   ///< a buffer naming a file, whose contents the UI edits in place
    Password,   ///< secret text, which the API obfuscates rather than sending in clear
    ReadOnly,   ///< display-only text
    ReadOnlyInt,///< display-only telemetry, shown with a unit suffix
    Select,     ///< a dropdown over an options array, stored as an index
    Progress,   ///< a bar showing a value against a total
    IPv4,       ///< four octets, which serialize as a dotted-quad string
    List,       ///< rows a source produces on demand from its own data
    Button,     ///< a momentary action, which reaches the changed hook rather than storage
    Palette     ///< a palette dropdown, whose options carry their own swatch colors
};

class JsonSink;   // forward-declared so the descriptor can hold a pointer

/// Emits a palette control's options, which the light domain owns and core calls.
using PaletteOptionsFn = void (*)(JsonSink& sink);

/// The backing for a list control, which the module owning the data implements.
///
/// Rows come straight from that module's own storage rather than being copied here.
/// An editable source addresses its rows by a stable id, so a reference survives a reorder.
struct ListSource {
    /// A source outlives its control, and is destroyed through this base.
    virtual ~ListSource() = default;
    /// How many rows the list holds, which may change between calls.
    virtual uint8_t listRowCount() const = 0;
    /// Append one row's summary, the fields a collapsed row shows.
    virtual void writeListRow(JsonSink& sink, uint8_t row) const = 0;
    /// Append one row's detail, which by default repeats the summary.
    virtual void writeListRowDetail(JsonSink& sink, uint8_t row) const {
        writeListRow(sink, row);
    }
    /// Append option sets shared across rows, so a repeated select is serialized once.
    virtual void writeListOptionSets(JsonSink& /*sink*/) const {}
    /// Repopulate the rows from persisted JSON, the model owning its own deserialization.
    virtual bool restoreList(const char* /*json*/, const char* /*key*/) { return false; }

    /// Whether these rows are worth writing to flash, which a derived list declines.
    virtual bool persistsList() const { return true; }

    /// Whether this source accepts the four editing operations below.
    virtual bool isEditableList() const { return false; }

    /// Render the rows as a grid of pads, for rows triggered far more often than edited.
    virtual bool listAsPads() const { return false; }

    /// The pad grid's columns, a non-zero value making it a fixed surface with real empty cells.
    virtual uint8_t listGridCols() const { return 0; }
    /// The pad grid's rows, read with the columns above.
    virtual uint8_t listGridRows() const { return 0; }

    /// Append a row with default values, reporting its new stable id.
    virtual bool addListRow(uint32_t& /*outId*/) { return false; }

    /// Remove one row by id, which a protected row refuses.
    virtual bool deleteListRow(uint32_t /*id*/) { return false; }

    /// Move one row by id, which never changes that id.
    virtual bool moveListRow(uint32_t /*id*/, uint8_t /*to*/) { return false; }

    /// Set one field of one row, the source owning which fields are editable.
    virtual bool setListRowField(uint32_t /*id*/, const char* /*field*/,
                                 const char* /*valueJson*/) { return false; }
};

// How much a reader wants to see. One number the whole UI composes against, so a control names the audience it is for rather than every card deciding for itself.
inline constexpr uint8_t kModeUser = 0;       ///< the show: what a light does
inline constexpr uint8_t kModeExpert = 1;     ///< and the tuning an installation needs: the peripheral, the pin, the rate
inline constexpr uint8_t kModeDeveloper = 2;  ///< and what diagnoses the firmware, meaning nothing without the source beside it

/// One control's metadata: what it points at, how to render it, and how to persist it.
///
/// The value lives in the module's own variable, and this borrows a pointer to it.
struct ControlDescriptor {
    void* ptr = nullptr;        ///< the bound variable, which the hot path reads directly
    const char* name = nullptr; ///< the control's name, a flash literal
    uintptr_t aux = 0;          ///< a total, an options array, or a unit, by type
    ControlType type = ControlType::Uint8;   ///< which storage, widget and mapping apply
    // Signed and wide, so one pair bounds every numeric type and sizes every buffer.
    int32_t min = 0;            ///< the lower bound, or unused for a text type
    int32_t max = 255;          ///< the upper bound, or the buffer size for a text type
    /// The sentinel meaning no default was declared, so an ordinary control costs nothing.
    static constexpr int32_t kNoDefault = INT32_MIN;
    /// What the control was born with, for a module whose controls come from data rather than type.
    int32_t def = kNoDefault;
    /// Whether the UI hides this control, which persistence ignores so state survives the toggle.
    bool hidden = false;
    /// Whether persistence writes this Select's label, for options enumerated fresh each boot.
    bool persistLabel = false;
    /// Whether the UI renders this display-only, for a value tooling pushes rather than a user.
    bool readonly = false;
    /// The mode a reader needs before this control is shown: 0 everyone, 1 expert, 2 developer.
    uint8_t minMode = 0;
    /// Whether a numeric renders as a number input, for an integer that is an address not a magnitude.
    bool numberField = false;
    // These sit after the other flags, since the text initializers below are positional.
    bool fader = false;        ///< render as a vertical fader
    bool encoder = false;      ///< render as a rotary encoder
    bool switchRow = false;    ///< render in the horizontal switch strip
    bool displayStrip = false; ///< render as the full-width readout
    /// Whether this is live state rather than configuration, and so is never written to flash.
    bool live = false;
    /// What this surface control drives, one field because each kind drives exactly one thing.
    const char* surfaceTarget = nullptr;
    /// An optional check applied before every write, so the rule lives with the control.
    bool (*validate)(const char* value) = nullptr;
};

/// The set of controls a module exposes, which is its `controls_`.
///
/// A control binds to a class variable by reference, so the hot path reads it directly.
/// Descriptors live in a fixed-capacity array, with no per-control allocation.
///
/// Prior art: MoonLight's `addControl`, which binds a variable the same way.
///
/// @moreinfo
///
/// ## What a control costs
///
/// A descriptor is a pointer, a name, an auxiliary word, the type, the bounds and some flags.
/// That is about forty-eight bytes on a host, and less on a device.
/// The value itself is the module's own variable, of one to four bytes.
/// A module that overflows the default capacity is probably too complex.
///
/// ## Persistence and rebuilding
///
/// Values persist through the filesystem module, which overlays them through each pointer.
/// Calling `defineControls` again clears and rebuilds the set.
/// That is how a conditional control re-evaluates whether it is hidden.
/// The per-type reference is on the type enum, and each `addX` below binds one type.
class ControlList {
public:
    /// Free the descriptor array, the bound variables being the modules' own.
    ~ControlList() { delete[] controls_; }

    /// A list starts empty, and grows as a module declares its controls.
    ControlList() = default;
    /// A list belongs to one module, so it is never copied.
    ControlList(const ControlList&) = delete;
    /// Nor copy-assigned.
    ControlList& operator=(const ControlList&) = delete;
    /// Nor moved, the descriptors being pointed into from elsewhere.
    ControlList(ControlList&&) = delete;
    /// Nor move-assigned.
    ControlList& operator=(ControlList&&) = delete;

    /// Bind a byte as a slider, the preferred default, whose bounds also clamp writes.
    void addControl(const char* name, uint8_t& var, uint8_t min = 0, uint8_t max = 255) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Uint8, min, max};
    }

    /// Bind a wide unsigned value, whose bounds default to its own full range.
    void addControl(const char* name, uint16_t& var,
                    uint16_t min = 0, uint16_t max = UINT16_MAX) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Uint16, min, max};
    }

    /// Bind a signed value, for a coordinate where negatives are legal.
    void addControl(const char* name, int16_t& var,
                    int16_t min = INT16_MIN, int16_t max = INT16_MAX) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Int16, min, max};
    }

    /// Bind a wide signed value, where sixteen bits would wrap.
    void addControl(const char* name, int32_t& var,
                    int32_t min = INT32_MIN, int32_t max = INT32_MAX) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Int32, min, max};
    }

    /// A small signed value is either a pin or telemetry, so the caller names which.
    void addControl(const char* name, int8_t& var, int16_t min = 0, int16_t max = 0) = delete;

    /// Bind a GPIO number, which renders as a number since a pin is an identity not a magnitude.
    void addPin(const char* name, int8_t& var, int16_t min = -1, int16_t max = MM_MAX_GPIO) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Pin, min, max};
    }

    /// Bind a boolean as a toggle, whose range is itself.
    void addControl(const char* name, bool& var) {
        grow();
        controls_[count_++] = {&var, name, 0, ControlType::Bool, 0, 1};
    }

    /// Bind a character buffer as a text input, with an optional check on every write.
    void addText(const char* name, char* var, uint16_t bufSize = 16,
                 bool (*validate)(const char*) = nullptr) {
        grow();
        controls_[count_++] = {.ptr = var, .name = name, .type = ControlType::Text,
                               .max = bufSize, .validate = validate};
    }

    /// Bind a buffer the UI renders as a resizable multi-line box, such as a script's source.
    void addTextArea(const char* name, char* var, uint16_t bufSize = 16,
                     bool (*validate)(const char*) = nullptr) {
        grow();
        controls_[count_++] = {.ptr = var, .name = name, .type = ControlType::TextArea,
                               .max = bufSize, .validate = validate};
    }

    /// Bind a buffer naming a file, whose contents the UI edits, with a picker over a directory.
    void addFilePath(const char* name, char* var, uint16_t bufSize,
                     const FilePathPick& pick,
                     bool (*validate)(const char*) = nullptr) {
        grow();
        controls_[count_++] = {.ptr = var, .name = name,
                               .aux = reinterpret_cast<uintptr_t>(&pick[0]),
                               .type = ControlType::FilePath,
                               .max = bufSize, .validate = validate};
    }

    /// Bind a file-path control with no picker, which is an editor over one fixed path.
    void addFilePath(const char* name, char* var, uint16_t bufSize,
                     bool (*validate)(const char*) = nullptr) {
        grow();
        controls_[count_++] = {.ptr = var, .name = name, .aux = 0,
                               .type = ControlType::FilePath,
                               .max = bufSize, .validate = validate};
    }

    /// Bind a buffer holding a secret, which the API obfuscates rather than sending in clear.
    void addPassword(const char* name, char* var, uint8_t bufSize = 32) {
        grow();
        controls_[count_++] = {var, name, 0, ControlType::Password, 0, bufSize};
    }

    /// Bind a buffer the UI shows but never edits.
    void addReadOnly(const char* name, char* var, uint8_t bufSize = 32) {
        grow();
        controls_[count_++] = {var, name, 0, ControlType::ReadOnly, 0, bufSize};
    }

    /// Bind a small signed telemetry value, shown with the unit suffix the caller owns.
    void addReadOnlyInt(const char* name, int8_t& var, const char* unit) {
        grow();
        controls_[count_++] = {&var, name, reinterpret_cast<uintptr_t>(unit),
                               ControlType::ReadOnlyInt, 0, 0};
    }

    /// Bind an index as a palette dropdown, whose options carry their own swatch colors.
    void addPalette(const char* name, uint8_t& var, PaletteOptionsFn optionsFn, uint8_t optionCount) {
        grow();
        controls_[count_++] = {&var, name, reinterpret_cast<uintptr_t>(optionsFn), ControlType::Palette, 0, optionCount};
    }

    /// Bind an index as a dropdown over the options array the caller owns.
    void addSelect(const char* name, uint8_t& var, const char* const* options, uint8_t optionCount) {
        grow();
        controls_[count_++] = {&var, name, reinterpret_cast<uintptr_t>(options), ControlType::Select, 0, optionCount};
    }

    /// Bind a value as a progress bar against a total, labeled either as bytes or as a count.
    void addProgress(const char* name, uint32_t& var, uint32_t total, bool bytes = true) {
        grow();
        controls_[count_++] = {&var, name, total, ControlType::Progress, bytes ? 1 : 0, 0};
    }

    /// Bind four octets as an address, which serializes as its dotted-quad string.
    void addIPv4(const char* name, uint8_t* var) {
        grow();
        controls_[count_++] = {var, name, 0, ControlType::IPv4, 0, 0};
    }

    /// Bind a source as a list of rows, which it produces on demand from its own data.
    void addList(const char* name, ListSource& source) {
        grow();
        // A non-const reference, since restoring a list repopulates the source's rows.
        controls_[count_++] = {&source, name, 0, ControlType::List, 0, 0};
    }

    /// Add a momentary button, whose click reaches the module's changed hook rather than storage.
    void addButton(const char* name) {
        grow();
        controls_[count_++] = {nullptr, name, 0, ControlType::Button, 0, 0};
    }

    /// Drop every control, which a rebuild does before redeclaring them.
    void clear() { count_ = 0; }
    /// How many controls are declared.
    uint8_t count() const { return count_; }
    /// One control by index.
    const ControlDescriptor& operator[](uint8_t i) const { return controls_[i]; }

    /// Hide or show the control last added, which persistence ignores so state survives.
    void setHidden(uint8_t i, bool hidden) {
        if (i < count_) controls_[i].hidden = hidden;
    }

    /// Record what a control was born with, for one whose default the type cannot supply.
    void setDefault(uint8_t i, int32_t def) {
        if (i < count_) controls_[i].def = def;
    }

    /// Render a control display-only, for a value tooling pushes rather than a user edits.
    void setReadOnly(uint8_t i, bool readonly) {
        if (i < count_) controls_[i].readonly = readonly;
    }

    /// Mark a control expert-only, which the UI shows from expert mode up.
    void setAdvanced(uint8_t i, bool advanced = true) {
        if (i < count_) controls_[i].minMode = advanced ? kModeExpert : kModeUser;
    }

    /// Mark a control developer-only: a number that diagnoses the firmware rather than the show.
    void setDeveloper(uint8_t i) {
        if (i < count_) controls_[i].minMode = kModeDeveloper;
    }

    /// Render a numeric as a number input, for an integer that is an identity not a magnitude.
    void setNumberField(uint8_t i, bool numberField = true) {
        if (i < count_) controls_[i].numberField = numberField;
    }

    /// Persist a Select by its label, for options enumerated fresh each boot.
    void setPersistLabel(uint8_t i, bool persistLabel = true) {
        if (i < count_) controls_[i].persistLabel = persistLabel;
    }

    /// Render a numeric as a vertical fader, for a level a user rides rather than sets once.
    void setFader(uint8_t i, bool fader = true, const char* target = nullptr) {
        if (i < count_) { controls_[i].fader = fader; controls_[i].surfaceTarget = target; }
    }

    /// Render a numeric as a rotary encoder, the third surface affordance beside pads and faders.
    void setEncoder(uint8_t i, bool encoder = true, const char* target = nullptr) {
        if (i < count_) { controls_[i].encoder = encoder; controls_[i].surfaceTarget = target; }
    }

    /// Render a boolean in the switch strip, so each column is one channel across the surface.
    void setSwitchRow(uint8_t i, bool switchRow = true, const char* target = nullptr) {
        if (i < count_) { controls_[i].switchRow = switchRow; controls_[i].surfaceTarget = target; }
    }

    /// Render a read-only text as the full-width readout, which shows whatever was last touched.
    void setDisplayStrip(uint8_t i, bool strip = true) {
        if (i < count_) controls_[i].displayStrip = strip;
    }

    /// Declare a control live state rather than configuration, so it is never written to flash.
    void setLive(uint8_t i, bool live = true) {
        if (i < count_) controls_[i].live = live;
    }

private:
    ControlDescriptor* controls_ = nullptr;
    uint8_t count_ = 0;
    uint8_t capacity_ = 0;

    /// Double the descriptor array when the next control would not fit.
    void grow() {
        if (count_ < capacity_) return;
        uint8_t newCap = capacity_ == 0 ? 4 : capacity_ * 2;
        auto* newArr = new ControlDescriptor[newCap];
        for (uint8_t i = 0; i < count_; i++) newArr[i] = controls_[i];
        delete[] controls_;
        controls_ = newArr;
        capacity_ = newCap;
    }
};

// The serialization API, defined in Control.cpp, whose sink stays forward-declared here.

class JsonSink;

/// The wire-format name of a type, which the UI reads as its rendering cue.
const char* controlTypeName(ControlType t);

/// Whether this type round-trips through persistence, which a derived reading does not.
bool isPersistable(ControlType t);
/// Whether this control's value is written to flash, which also honors a derived list.
bool isPersistable(const ControlDescriptor& c);

/// Whether the types route should emit a default for this type, which a secret declines.
bool hasDefault(ControlType t);

/// Append the value fragment alone, the caller composing the wrapper around it.
void writeControlValue(JsonSink& sink, const ControlDescriptor& c);

/// Append the per-type extras that ride beside the value, such as bounds or options.
void writeControlMetadata(JsonSink& sink, const ControlDescriptor& c);

/// What an apply did, which each caller maps onto its own reporting.
enum class ApplyResult : uint8_t {
    Ok,            ///< value parsed and applied.
    OutOfRange,    ///< numeric value outside the descriptor's bounds (Strict only).
    Malformed,     ///< the value didn't parse (e.g. a bad IPv4 string).
    ReadOnly,      ///< tried to write a display-only control.
};

/// What an out-of-range write does, since an API rejects where a load tolerates.
enum class ApplyPolicy : uint8_t {
    Strict,   ///< reject an out-of-range value (the HTTP API — surfaces as a 400).
    Clamp,    ///< clamp to the nearest valid value (persistence load — tolerates stale on-disk values).
};

/// Parse one value from the enclosing object and apply it, leaving storage alone on failure.
ApplyResult applyControlValue(const ControlDescriptor& c,
                              const char* json, const char* key,
                              ApplyPolicy policy = ApplyPolicy::Strict);

} // namespace mm
