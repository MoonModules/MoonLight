#pragma once

#include "core/module/MoonModule.h"
#include "core/util/InputMapping.h"   // InputAction + runInputLevel: the target half, shared with every input service
#include "core/module/Scheduler.h"
#include "platform/platform.h"   // adcRead / adcMaxCount

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// A list of ADC pins, each driving a control with a value rather than an event.
/// @card AnalogService.png
///
/// The continuous twin of Button, which drives the same controls from a contact. An expression pedal is the shape it is built around.
///
/// @guide
///
/// ## Calibrating a pedal
///
/// Each row maps the travel between its two raw counts onto the target's own range, so one setup works on any target. A Select with five options takes 0 to 4, and a bool takes on and off. Watch the live raw and value readouts while moving the pedal to each end.
///
/// ## Targets
///
/// A row pointed at a pad does nothing, since a pedal held at 40% has no sensible reading as a preset. Reversed bounds mean inverted. Scripts reach the same pins with `adcRead(pin)` and `adcMax()`, the path for a sensor whose mapping is a condition rather than a range.
///
/// @technical
///
/// **Travel.** A pedal rests high and tops out below full scale, so a row names the travel that matters. It also carries an invert for a pot wired backwards. Calibrating sets whichever end was reached first, so reversed bounds are valid.
///
/// **Smoothing.** An ADC pin jitters by a few counts at rest, so the seam reports raw counts. The filter lives here as an average a user can change, and a deadband stops a resting pedal writing at all.
class AnalogService : public MoonModule, public ListSource {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Declare the shared filter settings and the list of input rows.
    void defineControls() override {
        // A weight rather than a time constant, the sample rate being fixed.
        controls_.addControl("smoothing", smoothing_, 1, 100);
        // In target units, without which every tick would write a value one different.
        controls_.addControl("deadband", deadband_, 0, 32);
        /// The pins, each with its raw travel, an invert and a target.
        controls_.addList("inputs", *this);
        MoonModule::defineControls();
    }

    /// Poll every configured pin, which 50 Hz follows comfortably.
    void tick20ms() MM_NONBLOCKING override {
        for (uint8_t i = 0; i < count_; i++) pollRow(rows_[i]);
    }

    // --- ListSource: the analog rows ------------------------------------------------------------

    /// Editable, since the rows are the whole point of this module.
    bool isEditableList() const override { return true; }
    /// How many rows are configured.
    uint8_t listRowCount() const override { return count_; }

    /// Append one row's summary, ending with the live reading a calibration is set from.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.appendf("{\"id\":%u,\"pin\":%d,\"inMin\":%u,\"inMax\":%u,\"invert\":%s",
                     static_cast<unsigned>(r.id), static_cast<int>(r.pin),
                     static_cast<unsigned>(r.inMin), static_cast<unsigned>(r.inMax),
                     r.invert ? "true" : "false");
        writeInputActionFields(sink, r.action);
        sink.appendf(",\"raw\":%u,\"value\":%u}",
                     static_cast<unsigned>(r.raw), static_cast<unsigned>(r.mapped));
    }

    /// The row's editable fields: its pin, its travel, and its target.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.appendf("{\"fields\":[{\"name\":\"pin\",\"type\":\"uint8\",\"value\":%d},"
                     "{\"name\":\"inMin\",\"type\":\"uint16\",\"value\":%u,\"min\":0,\"max\":%u},"
                     "{\"name\":\"inMax\",\"type\":\"uint16\",\"value\":%u,\"min\":0,\"max\":%u},"
                     "{\"name\":\"invert\",\"type\":\"bool\",\"value\":%s},",
                     static_cast<int>(r.pin),
                     static_cast<unsigned>(r.inMin), static_cast<unsigned>(platform::adcMaxCount()),
                     static_cast<unsigned>(r.inMax), static_cast<unsigned>(platform::adcMaxCount()),
                     r.invert ? "true" : "false");
        // The target alone, a kind and a value being meaningless for a level.
        writeInputTargetDetailField(sink, r.action);
        sink.append("]}");
    }

    /// The target options, shared across every row rather than repeated in each.
    void writeListOptionSets(JsonSink& sink) const override { writeInputTargetOptions(sink); }

    /// Append a row at full scale, so it works end to end before anyone calibrates it.
    bool addListRow(uint32_t& outId) override {
        if (count_ >= kMaxRows) return false;
        Row& r = rows_[count_++];
        r = Row{};
        r.id = nextId_++;
        r.inMax = platform::adcMaxCount();
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

    /// Set one field of one row, every edit clearing the sent mark so the next poll writes.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        Row* r = find(id);
        if (!r) return false;
        if (std::strcmp(field, "target") == 0) {
            if (!setInputActionField(r->action, field, valueJson)) return false;
            r->sent = false;
            markDirty();
            return true;
        }
        if (std::strcmp(field, "kind") == 0 || std::strcmp(field, "value") == 0) return false;
        if (std::strcmp(field, "pin") == 0) {
            // Bounded before the narrowing cast, which would otherwise wrap onto another pin.
            const int pin = json::parseInt(valueJson, "value");
            if (pin < -1 || pin > 48) return false;
            r->pin = static_cast<int8_t>(pin);
            r->primed = false;      // a new pin starts its average fresh
            r->sent = false;
            markDirty();
            return true;
        }
        if (std::strcmp(field, "inMin") == 0) {
            r->inMin = clampCount(json::parseInt(valueJson, "value"));
            r->sent = false;
            markDirty();
            return true;
        }
        if (std::strcmp(field, "inMax") == 0) {
            r->inMax = clampCount(json::parseInt(valueJson, "value"));
            r->sent = false;
            markDirty();
            return true;
        }
        if (std::strcmp(field, "invert") == 0) {
            r->invert = json::parseBool(valueJson, "value") || json::parseInt(valueJson, "value") == 1;
            r->sent = false;
            markDirty();
            return true;
        }
        return false;
    }

    /// Rebuild the rows from the persisted list.
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
                r.inMin = clampCount(mm::json::readInt(mm::json::member(doc, el, "inMin")));
                // An absent value reads zero, which would look like a dead pedal.
                const auto* mx = mm::json::member(doc, el, "inMax");
                r.inMax = mx ? clampCount(mm::json::readInt(mx)) : platform::adcMaxCount();
                r.invert = mm::json::readBool(mm::json::member(doc, el, "invert"));
                if (!readInputAction(doc, el, r.action)) dropped++;
            });
        reportOffSurface(*this, statusBuf_, sizeof(statusBuf_), dropped);
        return ok;
    }

private:
    /// One analog input: its wiring, its travel, its target, and its own filter state.
    struct Row {
        uint32_t    id = 0;
        int8_t      pin = -1;
        uint16_t    inMin = 0;           ///< the raw count the travel STARTS at
        uint16_t    inMax = 0;           ///< and where it ends: set to full scale when a row is added
        bool        invert = false;      ///< a pot wired the other way round
        InputAction action{};
        uint16_t    raw = 0;             ///< the last reading, unfiltered: what a user calibrates from
        uint16_t    smoothed = 0;        ///< the running average, in raw counts
        uint8_t     mapped = 0;          ///< what the target last received
        bool        primed = false;      ///< the average holds a real reading, so it can be trusted
        bool        sent = false;        ///< a value has been written, so `mapped` is a real comparison
    };

    /// Clamp a raw count into the converter's range.
    static uint16_t clampCount(int v) {
        if (v < 0) return 0;
        const int max = static_cast<int>(platform::adcMaxCount());
        return static_cast<uint16_t>(v > max ? max : v);
    }

    /// The row with this id, or null.
    Row* find(uint32_t id) {
        for (uint8_t i = 0; i < count_; i++) if (rows_[i].id == id) return &rows_[i];
        return nullptr;
    }

    /// Read one row, filter it, map it, and write its target when the value moved.
    void pollRow(Row& r) MM_NONBLOCKING {
        if (r.pin < 0) return;
        uint16_t raw = 0;
        if (!platform::adcRead(static_cast<uint8_t>(r.pin), raw)) return;
        r.raw = raw;

        // The first reading is taken whole, or every pedal would sweep up from zero on boot.
        if (!r.primed) { r.smoothed = raw; r.primed = true; }
        else {
            // A signed difference, so the average converges from both directions.
            const int32_t diff = static_cast<int32_t>(raw) - static_cast<int32_t>(r.smoothed);
            const int32_t step = diff * static_cast<int32_t>(smoothing_) / 100;
            // A step of zero is where an integer average stops, so the last of it is taken whole.
            r.smoothed = static_cast<uint16_t>(step == 0 ? raw
                                                        : static_cast<int32_t>(r.smoothed) + step);
        }

        const uint8_t value = mapToTarget(r, r.smoothed);
        // A resting pedal jitters, so without this a row would write fifty times a second.
        if (r.sent) {
            const int delta = static_cast<int>(value) - static_cast<int>(r.mapped);
            if (delta <= static_cast<int>(deadband_) && -delta <= static_cast<int>(deadband_)) return;
        }
        r.mapped = value;
        r.sent = true;
        statusBuf_[0] = 0;   // cleared first, so the report below is about this move
        // The continuous path, since a level carries a value where a button carries an event.
        if (!runInputLevel(r.action, value, statusBuf_, sizeof(statusBuf_)) && statusBuf_[0])
            setStatus(statusBuf_, Severity::Warning);
    }

    /// Map a raw count through the row's travel into the range every surface control uses.
    static uint8_t mapToTarget(const Row& r, uint16_t raw) {
        uint16_t lo = r.inMin, hi = r.inMax;
        // A reversed pair says inverted, which is what calibrating in either order produces.
        bool flip = r.invert;
        if (lo > hi) { const uint16_t t = lo; lo = hi; hi = t; flip = !flip; }
        // A zero-width travel has no answer, so report the bottom rather than divide by zero.
        if (hi == lo) return 0;
        if (raw <= lo) return flip ? 255 : 0;
        if (raw >= hi) return flip ? 0 : 255;
        const uint32_t span = static_cast<uint32_t>(hi) - lo;
        const uint32_t pos  = static_cast<uint32_t>(raw) - lo;
        const uint8_t v = static_cast<uint8_t>(pos * 255u / span);
        return flip ? static_cast<uint8_t>(255 - v) : v;
    }

    static constexpr uint8_t kMaxRows = 8;   ///< inputs one device carries
    Row      rows_[kMaxRows];
    uint8_t  count_ = 0;
    uint32_t nextId_ = 1;
    /// How hard the running average pulls toward each reading, as a percentage.
    uint8_t  smoothing_ = 30;
    /// How far the value must move before the control is written.
    /// @guide Without it a resting pedal rewrites its target fifty times a second.
    uint8_t  deadband_ = 1;
    char     statusBuf_[64] = {};
};

}  // namespace mm
