#pragma once

#include "core/module/MoonModule.h"
#include "core/util/InputMapping.h"          // InputAction and its runners: the target half, shared with every input service
#include "core/system/FilesystemModule.h"    // noteDirty: schedule the debounced save on a learned bind

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

/// The standard gamepad layout in SDL's GameController names, buttons in the W3C Gamepad API's index order, then the four stick axes.
inline constexpr const char* kGamepadInputs[] = {
    "a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger",
    "back", "start", "leftstick", "rightstick", "dpup", "dpdown", "dpleft", "dpright", "guide",
    "leftx", "lefty", "rightx", "righty"};
/// How many of those are buttons; the rest are axes.
inline constexpr uint8_t kGamepadButtons = 17;
/// How many inputs the layout names.
inline constexpr uint8_t kGamepadInputCount = sizeof(kGamepadInputs) / sizeof(kGamepadInputs[0]);

/// A gamepad's buttons and sticks, each driving a control through the same rows every input service uses.
/// @card GamepadService.png
///
/// The interface reads the pad with the browser's Gamepad API and writes its state into the hidden, live `pad` control.
/// The text is the button bits and then the four axes, each 0 to 255 with 128 at rest; each write runs the rows at once.
/// A stick follows the surface's convention, up and right being more, so a stick, a fader and a paddle point the same way.
class GamepadService : public MoonModule, public ListSource {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Declare the pad's state and the list of rows.
    void defineControls() override {
        controls_.addText("pad", pad_, sizeof(pad_));
        controls_.setHidden(controls_.count() - 1, true);
        controls_.setLive(controls_.count() - 1);
        controls_.addList("inputs", *this);
        MoonModule::defineControls();
    }

    /// A fresh service starts with rows onto the surface, so a pad works once the surface is assigned.
    void setup() override {
        MoonModule::setup();
        if (!restored_) seedDefaults();
    }

    /// Run the rows against each state the interface writes.
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "pad") != 0) return;
        uint32_t buttons = 0;
        uint8_t axes[kAxes] = {128, 128, 128, 128};
        if (!parsePad(pad_, buttons, axes)) return;
        if (!primed_) {
            // The sticks' first positions are rest, so plugging a pad in moves nothing; its buttons are not, since a browser shows a pad only once one is pressed.
            for (uint8_t i = 0; i < kAxes; i++) axes_[i] = axes[i];
            primed_ = true;
        }
        apply(buttons, axes);
    }

    // --- ListSource: the input rows --------------------------------------------------------------

    /// Editable, since the rows are the whole point of this module.
    bool isEditableList() const override { return true; }

    /// How many rows are configured.
    uint8_t listRowCount() const override { return count_; }

    /// Append one row's summary: the input's name and what it drives.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.appendf("{\"id\":%u,\"input\":", static_cast<unsigned>(r.id));
        sink.writeJsonString(r.input < kGamepadInputCount ? kGamepadInputs[r.input] : "");
        writeInputActionFields(sink, r.action);
        sink.append("}");
    }

    /// The row's editable fields: the input, the learn button, and the action, a stick taking a target alone.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        // The last option is "(none)", which is what a row with an unknown input name binds.
        sink.appendf("{\"fields\":[{\"name\":\"input\",\"type\":\"select\",\"optionsRef\":\"inputs\",\"value\":%d},",
                     static_cast<int>(r.input < kGamepadInputCount ? r.input : kGamepadInputCount));
        sink.appendf("{\"name\":\"learn\",\"type\":\"button\",\"label\":\"%s\"},",
                     r.learn ? "waiting..." : "learn");
        if (isAxis(r.input)) writeInputTargetDetailField(sink, r.action);
        else writeInputActionDetailFields(sink, r.action);
        sink.append("]}");
    }

    /// The shared option sets: the targets, and the standard layout's input names.
    void writeListOptionSets(JsonSink& sink) const override {
        writeInputTargetOptions(sink);
        sink.append(",\"inputs\":[");
        for (uint8_t i = 0; i < kGamepadInputCount; i++) {
            sink.writeJsonString(kGamepadInputs[i]);
            sink.append(",");
        }
        sink.append("\"(none)\"]");
    }

    /// Append a row on the first button, ready to learn.
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

    /// Set one field of one row: the shared action fields, the input, or the learn button.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        Row* r = find(id);
        if (!r) return false;
        if (setInputActionField(r->action, field, valueJson)) { markDirty(); return true; }
        if (std::strcmp(field, "input") == 0) {
            // A name from the interface or an index from the dropdown.
            char buf[16] = {};
            json::parseString(valueJson, "value", buf, sizeof(buf));
            const int idx = buf[0] ? inputIndex(buf) : json::parseInt(valueJson, "value");
            if (idx < 0 || idx > kGamepadInputCount) return false;   // kGamepadInputCount is "(none)"
            r->input = static_cast<uint8_t>(idx);
            markDirty();
            return true;
        }
        if (std::strcmp(field, "learn") == 0) {
            // One row learns at a time, or the winner would be whichever the loop reached first.
            for (uint8_t i = 0; i < count_; i++) rows_[i].learn = false;
            r->learn = true;
            setStatus("press a button or move a stick to bind it");
            return true;   // transient intent, not persisted state
        }
        return false;
    }

    /// The rows are configuration, restored by restoreList.
    bool persistsList() const override { return true; }

    /// Rebuild the rows from the persisted list; an empty list stays empty rather than re-seeding.
    bool restoreList(const char* json, const char* key) override {
        count_ = 0;
        restored_ = true;
        unsigned dropped = 0;
        const bool ok = mm::json::forEachListElement(json, key,
            [&](const mm::json::JsonDoc& doc, const mm::json::JsonNode* el) {
                if (count_ >= kMaxRows) return;
                Row& r = rows_[count_++];
                r = Row{};
                r.id = nextId_++;
                char input[16] = {};
                mm::json::readString(mm::json::member(doc, el, "input"), input, sizeof(input));
                const int idx = inputIndex(input);
                r.input = static_cast<uint8_t>(idx < 0 ? kGamepadInputCount : idx);   // an unknown name binds nothing
                if (!readInputAction(doc, el, r.action)) dropped++;
            });
        reportOffSurface(*this, statusBuf_, sizeof(statusBuf_), dropped);
        return ok;
    }

    /// Where an input's name sits in the standard layout, or -1 for a name it does not have.
    static int inputIndex(const char* name) {
        for (uint8_t i = 0; i < kGamepadInputCount; i++)
            if (std::strcmp(name, kGamepadInputs[i]) == 0) return i;
        return -1;
    }

private:
    static constexpr uint8_t kAxes = kGamepadInputCount - kGamepadButtons;
    static constexpr uint8_t kMaxRows = 16;
    /// A stick at rest wanders a count or two, which must not count as a player.
    static constexpr uint8_t kDeadband = 3;
    /// How far a stick moves from rest before learn takes it.
    static constexpr uint8_t kLearnTravel = 64;

    /// One binding: an input of the standard layout and what it drives, the arm flag never persisted.
    struct Row {
        uint32_t    id = 0;
        uint8_t     input = 0;
        bool        learn = false;
        InputAction action{};
    };

    static bool isAxis(uint8_t input) { return input >= kGamepadButtons && input < kGamepadInputCount; }

    /// A Y axis flipped to the fader's convention, the browser reporting it as screen coordinates with down positive.
    static uint8_t upIsMore(uint8_t input, uint8_t value) {
        const bool vertical = input == kGamepadButtons + 1 || input == kGamepadButtons + 3;   // lefty, righty
        return vertical ? static_cast<uint8_t>(255 - value) : value;
    }

    /// Read "buttons,lx,ly,rx,ry"; false for anything else, which then changes nothing.
    static bool parsePad(const char* s, uint32_t& buttons, uint8_t (&axes)[kAxes]) {
        char* end = nullptr;
        buttons = static_cast<uint32_t>(std::strtoul(s, &end, 10));
        if (end == s) return false;
        for (uint8_t i = 0; i < kAxes; i++) {
            if (*end != ',') return false;
            const char* from = end + 1;
            const long v = std::strtol(from, &end, 10);
            if (end == from || v < 0 || v > 255) return false;
            axes[i] = static_cast<uint8_t>(v);
        }
        return *end == 0;
    }

    /// Run every row whose input changed, binding it first when a row is learning.
    void apply(uint32_t buttons, const uint8_t (&axes)[kAxes]) {
        statusBuf_[0] = 0;   // cleared first, so the report below is about this change
        for (uint8_t b = 0; b < kGamepadButtons; b++) {
            const bool was = (buttons_ >> b) & 1u, now = (buttons >> b) & 1u;
            if (was == now) continue;
            if (now && learnInto(b)) { learnedHeld_ |= 1u << b; continue; }
            // The release of the press that was learned runs nothing, or a set row would write 0 to its new target.
            if (!now && (learnedHeld_ & (1u << b))) { learnedHeld_ &= ~(1u << b); continue; }
            for (uint8_t i = 0; i < count_; i++) {
                const Row& r = rows_[i];
                if (r.input != b) continue;
                // Only a set row acts on the release: the others would fire twice for one push.
                if (!now && r.action.kind != InputAction::Kind::Set) continue;
                runInputAction(r.action, now, statusBuf_, sizeof(statusBuf_));
            }
        }
        for (uint8_t a = 0; a < kAxes; a++) {
            const uint8_t input = static_cast<uint8_t>(kGamepadButtons + a);
            const int travel = static_cast<int>(axes[a]) - 128;
            if ((travel > kLearnTravel || travel < -kLearnTravel) && learnInto(input)) continue;
            const int moved = static_cast<int>(axes[a]) - static_cast<int>(axes_[a]);
            if (moved <= kDeadband && moved >= -kDeadband) continue;
            axes_[a] = axes[a];
            for (uint8_t i = 0; i < count_; i++)
                if (rows_[i].input == input) runInputLevel(rows_[i].action, upIsMore(input, axes[a]), statusBuf_, sizeof(statusBuf_));
        }
        buttons_ = buttons;
        if (statusBuf_[0]) setStatus(statusBuf_);
    }

    /// Bind `input` to the armed row, if one is armed.
    bool learnInto(uint8_t input) {
        for (uint8_t i = 0; i < count_; i++) {
            if (!rows_[i].learn) continue;
            rows_[i].input = input;
            rows_[i].learn = false;
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "learned %s", kGamepadInputs[input]);
            setStatus(statusBuf_);
            markDirty();
            FilesystemModule::noteDirty();
            return true;
        }
        return false;
    }

    /// The rows a fresh service starts with: the sticks onto faders, A onto a switch.
    void seedDefaults() {
        struct Seed { const char* input; const char* target; InputAction::Kind kind; int16_t value; };
        static constexpr Seed kSeeds[] = {
            {"lefty",  "Control.fader1",  InputAction::Kind::Set, 0},
            {"righty", "Control.fader2",  InputAction::Kind::Set, 0},
            {"leftx",  "Control.fader3",  InputAction::Kind::Set, 0},
            {"a",      "Control.switch1", InputAction::Kind::Set, 1},
        };
        for (const Seed& s : kSeeds) {
            uint32_t id = 0;
            if (!addListRow(id)) return;
            Row& r = rows_[count_ - 1];
            r.input = static_cast<uint8_t>(inputIndex(s.input));
            parseTarget(s.target, r.action.type, r.action.number);
            r.action.kind = s.kind;
            r.action.value = s.value;
        }
    }

    /// The row with this id, or null.
    Row* find(uint32_t id) {
        for (uint8_t i = 0; i < count_; i++) if (rows_[i].id == id) return &rows_[i];
        return nullptr;
    }

    char     pad_[40] = {};          ///< the newest state the interface wrote
    uint32_t learnedHeld_ = 0;       ///< buttons whose press bound a row, so their release runs nothing
    bool     primed_ = false;        ///< a baseline is held, so a change is a move
    uint32_t buttons_ = 0;           ///< the buttons as last acted on
    uint8_t  axes_[kAxes] = {128, 128, 128, 128};   ///< the axes as last written
    Row      rows_[kMaxRows];
    uint8_t  count_ = 0;
    uint32_t nextId_ = 1;
    bool     restored_ = false;      ///< a persisted list exists, even an empty one
    char     statusBuf_[48] = {};    ///< what the last input did
};

}  // namespace mm
