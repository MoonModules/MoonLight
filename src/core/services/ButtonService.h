#pragma once

#include "core/module/MoonModule.h"
#include "core/util/InputMapping.h"   // InputAction: the target half, shared with the infrared service
#include "platform/platform.h"   // gpioInputBegin / gpioRead

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// A list of push buttons, each on its own GPIO, each driving a control.
/// @card ButtonService.png
///
/// A press and a UI click are the same thing to whatever they drive. A foot pedal needs no module of its own, being electrically a momentary switch. Neither does a PIR motion sensor: a set row holds the lights on while it sees motion, and off once still.
///
/// @guide
///
/// ## What a row does
///
/// Each row binds a pin to a surface control; a catalog board's first button drives `Control.switch1`, the lights' relay. The pressed readout shows a button working before it is bound.
///
/// A push button takes `toggle`. A latching switch takes `set` with value 1, on while closed, or `flip`, toggling on every change, for a staircase with a remote.
///
/// @technical
///
/// **Momentary or latching.** A toggle row flips its target on each press and ignores the release; a set row writes while held and clears on release, which a pedal needs. Only a set row acts on the release, or a toggle would fire twice per push; a flip row acts on both edges, which is what a latching switch needs, from the level it settles at first, so a switch closed at boot flips nothing.
///
/// **Debounce.** A bouncing contact is a property of the switch, so this module owns the time constant, polled at 50 Hz since a press lasts tens of milliseconds.
class ButtonService : public MoonModule, public ListSource {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Declare the shared debounce and the list of button rows.
    void defineControls() override {
        controls_.addControl("debounceMs", debounceMs_, 1, 200);
        /// The buttons, each with a pin, its wiring, a live pressed readout and a target.
        /// @field pin The GPIO the button is wired to; -1 leaves the row unused.
        /// @field activeLow Active low for a switch to ground, the usual wiring; active high for one feeding 3V3.
        /// @fields InputAction
        controls_.addList("buttons", *this);
        MoonModule::defineControls();
    }

    /// Open every pin the restored rows name.
    void setup() override {
        MoonModule::setup();
        beginPins();
    }

    /// Retry a set row whose write failed, once a second: its target may be on a surface restored after the row.
    void tick1s() MM_NONBLOCKING override {
        for (uint8_t i = 0; i < count_; i++) {
            Row& r = rows_[i];
            if (!r.open || r.synced || !r.tried || r.action.kind != InputAction::Kind::Set) continue;
            r.synced = act(r);
        }
        MoonModule::tick1s();
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
        openInputPinFields(sink, r.pin);
        sink.appendf(",{\"name\":\"activeLow\",\"type\":\"select\",\"value\":%d,"
                     "\"options\":[\"active high\",\"active low\"]},",
                     r.activeLow ? 1 : 0);
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
        if (setInputActionField(r->action, field, valueJson)) { r->synced = r->tried = false; markDirty(); return true; }
        if (std::strcmp(field, "pin") == 0) {
            if (!parseInputPin(valueJson, r->pin)) return false;
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
        uint32_t    id = 0;          ///< the handle the editor addresses the row by
        int8_t      pin = -1;        ///< the `pin` field, bounded by parseInputPin
        bool        activeLow = true;    ///< a switch to ground with a pull-up: the usual wiring
        InputAction action{};
        bool        open = false;        ///< the pin was accepted by the seam, so it is worth polling
        bool        pressed = false;     ///< the settled state
        bool        candidate = false;   ///< the level being timed
        uint16_t    sinceChange = 0;     ///< ms the candidate has held
        bool        synced = false;      ///< a set row has written its settled state once, so its target follows the switch from the start
        bool        tried = false;       ///< a set row's first write ran, a failed one retried on the slow tick; a flip row's baseline is taken
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
        r.synced = r.tried = false;
    }

    /// Whether the level is the one the row already acted on; a set row never written goes on once, so a switch open at boot turns its target off.
    static bool settled(const Row& r, bool raw, bool set) MM_NONBLOCKING {
        return raw == r.pressed && (r.synced || r.tried || !set);
    }

    /// A flip row's first settled level is its baseline, so a switch closed at boot flips nothing.
    void takeBaseline(Row& r, bool raw) MM_NONBLOCKING {
        if (r.sinceChange < debounceMs_) { r.sinceChange += 20; return; }
        r.pressed = raw;
        r.tried = true;
        r.sinceChange = 0;
    }

    /// Read one row, debounce it, and act on a settled edge.
    void pollRow(Row& r) MM_NONBLOCKING {
        if (r.pin < 0 || !r.open) return;
        const bool raw = platform::gpioRead(static_cast<uint8_t>(r.pin)) != r.activeLow;

        // By time rather than by sample count, which would change meaning with the poll rate.
        if (raw != r.candidate) { r.candidate = raw; r.sinceChange = 0; return; }
        const bool set = r.action.kind == InputAction::Kind::Set;
        const bool flip = r.action.kind == InputAction::Kind::Flip;
        if (flip && !r.tried) { takeBaseline(r, raw); return; }
        if (!debounced(r, raw, set)) return;
        r.pressed = raw;
        onSettled(r, set, flip);
    }

    /// Act on a settled level: a toggle on the press alone, a set and a flip on both edges.
    void onSettled(Row& r, bool set, bool flip) MM_NONBLOCKING {
        if (!set && !flip && !r.pressed) return;
        const bool done = act(r);
        if (set) { r.synced = done; r.tried = true; }   // a target still missing, as on a surface restoring, is retried by tick1s
    }

    /// Whether `raw` is a new level held for the debounce time, restarting the count when it is.
    bool debounced(Row& r, bool raw, bool set) MM_NONBLOCKING {
        if (settled(r, raw, set)) { r.sinceChange = 0; return false; }
        if (r.sinceChange < debounceMs_) { r.sinceChange += 20; return false; }
        r.sinceChange = 0;
        return true;
    }

    /// Run the row's action on its settled state and report it, since doing nothing looks identical to a broken switch.
    bool act(const Row& r) MM_NONBLOCKING {
        char msg[sizeof(statusBuf_)] = {};   // built aside, so a run with nothing to say leaves the status, such as the restore's warning, as it is
        const bool done = runInputAction(r.action, r.pressed, msg, sizeof(msg));
        if (done || msg[0]) { std::memcpy(statusBuf_, msg, sizeof(msg)); setStatus(statusBuf_); }
        return done;
    }

    /// How many buttons one device carries, with room for a pedalboard.
    static constexpr uint8_t kMaxRows = 8;

    Row      rows_[kMaxRows];
    uint8_t  count_ = 0;
    uint32_t nextId_ = 1;
    /// How long a level must hold before it counts as a press, shared by every button.
    uint8_t  debounceMs_ = 25;
    char     statusBuf_[48] = {};
};

}  // namespace mm
