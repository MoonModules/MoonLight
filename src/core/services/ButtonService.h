#pragma once

#include "core/module/MoonModule.h"
#include "core/util/InputMapping.h"   // InputAction: the target half, shared with the infrared service
#include "platform/platform.h"   // gpioInputBegin / gpioRead

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// A list of push buttons, each on its own GPIO, each driving a control.
///
/// The physical twin of a UI click, and the same shape as the infrared service.
/// A list because boards have more than one, and a single pin did not survive the second board.
/// @card ButtonService.png
///
/// @moreinfo
///
/// ## How a press acts
///
/// Through the one control-set primitive every transport uses.
/// So a press and a message are indistinguishable to whatever they drive.
/// A row names a surface control, so the button lands where every transport reaches it.
///
/// ## Momentary against latching
///
/// A wall switch and a stage pedal want opposite things.
/// A toggle row flips its target on each press and ignores the release.
/// A set row writes while held and clears on release, which is what a pedal needs.
/// So a foot pedal needs no module of its own, being electrically a momentary switch.
///
/// ## Debounced here, not in the platform
///
/// A bouncing contact is a property of the switch, so this module owns the time constant.
/// Polled at 50 Hz, since a press lasts tens of milliseconds.
class ButtonService : public MoonModule, public ListSource {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Declare the shared debounce and the list of button rows.
    void defineControls() override {
        controls_.addControl("debounceMs", debounceMs_, 1, 200);
        controls_.addList("buttons", *this);
        MoonModule::defineControls();
    }

    /// Open every pin the restored rows name.
    void setup() override {
        MoonModule::setup();
        beginPins();
    }

    /// Poll every configured button, which 50 Hz catches comfortably.
    void tick20ms() MM_NONBLOCKING override {
        for (uint8_t i = 0; i < count_; i++) pollRow(rows_[i]);
    }

    // --- ListSource: the button rows -----------------------------------------------------------

    /// Editable, since the rows are the whole point of this module.
    bool isEditableList() const override { return true; }

    /// How many rows are configured.
    uint8_t listRowCount() const override { return count_; }

    /// Append one row's summary, ending with the live state so a button can be seen working.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.appendf("{\"id\":%u,\"pin\":%d,\"activeLow\":%s",
                     static_cast<unsigned>(r.id), static_cast<int>(r.pin),
                     r.activeLow ? "true" : "false");
        writeInputActionFields(sink, r.action);
        sink.appendf(",\"pressed\":%s}", r.pressed ? "true" : "false");
    }

    /// The row's editable fields, so a button is retargeted on the card rather than through the API.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.appendf("{\"fields\":[{\"name\":\"pin\",\"type\":\"uint8\",\"value\":%d},"
                     "{\"name\":\"activeLow\",\"type\":\"select\",\"value\":%d,"
                     "\"options\":[\"active high\",\"active low\"]},",
                     static_cast<int>(r.pin), r.activeLow ? 1 : 0);
        writeInputActionDetailFields(sink, r.action);
        sink.append("]}");
    }

    /// The target-type options, shared across every row rather than repeated in each.
    void writeListOptionSets(JsonSink& sink) const override { writeInputTargetOptions(sink); }

    /// Append a row with default values.
    bool addListRow(uint32_t& outId) override {
        if (count_ >= kMaxRows) return false;
        Row& r = rows_[count_++];
        r = Row{};
        r.id = nextId_++;
        outId = r.id;
        markDirty();
        return true;
    }

    /// Remove one row by id.
    bool deleteListRow(uint32_t id) override {
        for (uint8_t i = 0; i < count_; i++) {
            if (rows_[i].id != id) continue;
            for (uint8_t j = i; j + 1 < count_; j++) rows_[j] = rows_[j + 1];
            count_--;
            markDirty();
            return true;
        }
        return false;
    }

    /// Set one field of one row, the shared action fields first and this module's own after.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        Row* r = find(id);
        if (!r) return false;
        if (setInputActionField(r->action, field, valueJson)) { markDirty(); return true; }
        if (std::strcmp(field, "pin") == 0) {
            r->pin = static_cast<int8_t>(json::parseInt(valueJson, "value"));
            beginPin(*r);          // live, so entering a GPIO works now rather than at reboot
            markDirty();
            return true;
        }
        if (std::strcmp(field, "activeLow") == 0) {
            // A bool from the API and an index from the UI mean the same thing, so both are taken.
            r->activeLow = json::parseBool(valueJson, "value") || json::parseInt(valueJson, "value") == 1;
            beginPin(*r);          // the pull follows activeLow, so re-open the input
            markDirty();
            return true;
        }
        return false;
    }

    /// The rows are configuration, restored by restoreList.
    bool persistsList() const override { return true; }

    /// Rebuild the rows from the persisted list, then open every pin they name.
    bool restoreList(const char* json, const char* key) override {
        count_ = 0;
        unsigned dropped = 0;
        const bool ok = mm::json::forEachListElement(json, key,
            [&](const mm::json::JsonDoc& doc, const mm::json::JsonNode* el) {
                if (count_ >= kMaxRows) return;
                Row& r = rows_[count_++];
                r = Row{};
                r.id = nextId_++;
                r.pin = static_cast<int8_t>(mm::json::readInt(mm::json::member(doc, el, "pin")));
                r.activeLow = mm::json::readBool(mm::json::member(doc, el, "activeLow"));
                if (!readInputAction(doc, el, r.action)) dropped++;
            });
        reportOffSurface(*this, statusBuf_, sizeof(statusBuf_), dropped);
        beginPins();   // a restored row is only a row until its pin is opened
        return ok;
    }

private:
    /// One button: its wiring, its target, and the debounce state it carries between polls.
    struct Row {
        uint32_t    id = 0;
        int8_t      pin = -1;
        bool        activeLow = true;    ///< a switch to ground with a pull-up: the usual wiring
        InputAction action{};
        bool        open = false;        ///< the pin was accepted by the seam, so it is worth polling
        bool        pressed = false;     ///< the settled state
        bool        candidate = false;   ///< the level being timed
        uint16_t    sinceChange = 0;     ///< ms the candidate has held
    };

    /// The row with this id, or null.
    Row* find(uint32_t id) {
        for (uint8_t i = 0; i < count_; i++) if (rows_[i].id == id) return &rows_[i];
        return nullptr;
    }

    /// Open every row's pin.
    void beginPins() { for (uint8_t i = 0; i < count_; i++) beginPin(rows_[i]); }

    /// Open one row's pin as an input, the wiring flag picking which pull it takes.
    void beginPin(Row& r) {
        r.open = false;
        if (r.pin < 0) return;
        // A refused pin is never polled, since reading it returns phantom presses.
        r.open = platform::gpioInputBegin(
            static_cast<uint8_t>(r.pin),
            r.activeLow ? platform::GpioPull::Up : platform::GpioPull::Down);
        if (!r.open) setStatus("that pin cannot be used as an input", Severity::Warning);
        r.pressed = r.candidate = false;
        r.sinceChange = 0;
    }

    /// Read one row, debounce it, and act on a settled edge.
    void pollRow(Row& r) MM_NONBLOCKING {
        if (r.pin < 0 || !r.open) return;
        const bool raw = platform::gpioRead(static_cast<uint8_t>(r.pin)) != r.activeLow;

        // By time rather than by sample count, which would change meaning with the poll rate.
        if (raw != r.candidate) { r.candidate = raw; r.sinceChange = 0; return; }
        if (raw == r.pressed)   { r.sinceChange = 0; return; }        // already settled here
        if (r.sinceChange < debounceMs_) { r.sinceChange += 20; return; }

        r.pressed = raw;
        r.sinceChange = 0;
        // Only a set row acts on the release: the others would fire twice for one push.
        if (r.action.kind != InputAction::Kind::Set && !r.pressed) return;
        statusBuf_[0] = 0;   // cleared first, so the report below is about this press
        // Reported either way, since doing nothing looks identical to a broken switch.
        if (runInputAction(r.action, r.pressed, statusBuf_, sizeof(statusBuf_)) || statusBuf_[0])
            setStatus(statusBuf_);
    }

    /// How many buttons one device carries, with room for a pedalboard.
    static constexpr uint8_t kMaxRows = 8;

    Row      rows_[kMaxRows];
    uint8_t  count_ = 0;
    uint32_t nextId_ = 1;
    uint8_t  debounceMs_ = 25;     ///< shared: one switch type per board, in practice
    char     statusBuf_[48] = {};
};

}  // namespace mm
