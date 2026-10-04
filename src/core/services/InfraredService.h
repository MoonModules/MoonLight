#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/module/MoonModule.h"
#include "core/util/InputMapping.h"        // InputAction: the target half, shared with the button service
#include "core/system/FilesystemModule.h"    // noteDirty: schedule the debounced save on a learned bind
#include "platform/platform.h"        // irRead

#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstring>

namespace mm {

/// An infrared receiver that decodes a remote and drives other modules' controls.
///
/// The same shape as the button service, which drives the same controls from a switch.
/// A row is the binding, so twenty keys are twenty rows, each pointing where you want.
/// @card InfraredService.png
///
/// @moreinfo
///
/// ## Learned, not shipped
///
/// No firmware can carry a code table for every remote in the world.
/// So a binding is taught: arm a row, and the next decoded code binds to it.
/// The first version carried five compiled actions, which made our opinion the user's ceiling.
///
/// A fresh service starts empty, so the first use is to add a row and learn a key.
/// Shipping defaults would need the catalog to express a row, which no operation does yet.
///
/// ## How a press acts
///
/// Through the one control-set primitive every transport uses.
/// So a remote press and a message are indistinguishable to whatever they drive.
/// Pointing a row at the control surface puts the remote where every transport reaches it.
///
/// Prior art: consumer remotes use the NEC protocol, which the chip's own peripheral decodes.
/// That decode lives behind the platform seam.
class InfraredService : public MoonModule, public ListSource {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Declare the receiver pin and the list of learned rows.
    void defineControls() override {
        controls_.addPin("pin", pin_);
        controls_.addList("codes", *this);
        MoonModule::defineControls();
    }

    /// Re-report the receive state when the pin changes.
    void onControlChanged(const char* controlName) override {
        MoonModule::onControlChanged(controlName);
        if (std::strcmp(controlName, "pin") == 0) reportReady();
    }

    /// Report whether the receiver opened, not merely that a pin is set.
    void prepare() override { reportReady(); }

    /// Read one decoded frame, if the receiver has one.
    void tick() MM_NONBLOCKING override {
        if (pin_ < 0) return;
        uint32_t code = 0;
        if (platform::irRead(static_cast<uint16_t>(pin_), code)) processCode(code);
    }

    /// The last decoded code (0 = none yet).
    uint32_t latestCode() const { return lastCode_; }

    /// Feed a decoded code as if from the receiver, since a desktop has none.
    void injectCodeForTest(uint32_t code) { processCode(code); }

    // --- ListSource: the code rows -------------------------------------------------------------

    /// Editable, since the rows are the whole point of this module.
    bool isEditableList() const override { return true; }

    /// How many rows are configured.
    uint8_t listRowCount() const override { return count_; }

    /// Append one row's summary, its code as hex, the way a frame is read everywhere else.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        // The arm flag is transient intent rather than state, so the detail carries it instead.
        sink.appendf("{\"id\":%u,\"code\":\"0x%08lX\"",
                     static_cast<unsigned>(r.id), static_cast<unsigned long>(r.code));
        writeInputActionFields(sink, r.action);
        sink.append("}");
    }

    /// The row's editable fields, arming being a button since it is an action rather than a setting.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= count_) { sink.append("{}"); return; }
        const Row& r = rows_[row];
        sink.append("{\"fields\":[{\"name\":\"code\",\"type\":\"text\",\"value\":");
        char codeStr[16];
        std::snprintf(codeStr, sizeof(codeStr), "0x%08lX", static_cast<unsigned long>(r.code));
        sink.writeJsonString(codeStr);
        sink.appendf("},{\"name\":\"learn\",\"type\":\"button\",\"label\":\"%s\"},",
                     r.learn ? "waiting..." : "learn");
        // A remote code is a single event, so a row that needs a release would latch forever.
        writeInputActionDetailFields(sink, r.action, /*hasRelease=*/false);
        sink.append("]}");
    }

    /// The target options, shared across every row rather than repeated in each.
    void writeListOptionSets(JsonSink& sink) const override { writeInputTargetOptions(sink); }

    /// Append an unbound row, ready to learn.
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

    /// Set one field of one row: the shared action fields, the arm button, or a typed code.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        Row* r = find(id);
        if (!r) return false;
        if (setInputActionField(r->action, field, valueJson)) { markDirty(); return true; }
        if (std::strcmp(field, "learn") == 0) {
            // The key's presence tells a click from an explicit arm, a boolean reading empty.
            char buf[8] = {};
            json::parseString(valueJson, "value", buf, sizeof(buf));
            const bool arm = (buf[0] == 0 && !json::hasKey(valueJson, "value"))
                                 ? true
                                 : (buf[0] != 0 ? true : json::parseBool(valueJson, "value"));
            // One row learns at a time, or the winner would be whichever the loop reached first.
            if (arm) for (uint8_t i = 0; i < count_; i++) rows_[i].learn = false;
            r->learn = arm;
            if (arm) setStatus("press a remote key to bind it");
            return true;   // transient UI intent, not persisted state: no markDirty
        }
        if (std::strcmp(field, "code") == 0) {
            // Hex or decimal, and rejected rather than truncated, which would bind silently.
            char buf[24] = {};
            json::parseString(valueJson, "value", buf, sizeof(buf));
            // A filled buffer means the value did not fit, so what it holds is only a prefix.
            if (std::strlen(buf) == sizeof(buf) - 1) return false;
            char* end = nullptr;
            errno = 0;
            const unsigned long long parsed = std::strtoull(buf, &end, 0);
            // The wider parse, since the narrow one saturates on a device and passes the bound.
            if (end == buf || *end != 0 || errno == ERANGE || parsed > 0xFFFFFFFFULL) return false;
            r->code = static_cast<uint32_t>(parsed);
            claimCode(r->code, r);          // a typed code takes the binding too
            markDirty();
            return true;
        }
        return false;
    }

    /// The rows are configuration, restored by restoreList.
    bool persistsList() const override { return true; }

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
                char codeStr[16] = {};
                mm::json::readString(mm::json::member(doc, el, "code"), codeStr, sizeof(codeStr));
                r.code = static_cast<uint32_t>(std::strtoul(codeStr, nullptr, 0));
                if (!readInputAction(doc, el, r.action)) dropped++;
            });
        offSurface_ = static_cast<uint8_t>(dropped);
        reportOffSurface(*this, statusBuf_, sizeof(statusBuf_), dropped);
        return ok;
    }

private:
    /// One binding: a remote code and what it drives, the arm flag never persisted.
    struct Row {
        uint32_t    id = 0;
        uint32_t    code = 0;     ///< the learned frame, 0 = unbound
        bool        learn = false;
        InputAction action{};
    };

    /// Clear this code from every other row, so one key binds to exactly one action.
    void claimCode(uint32_t code, const Row* keep) {
        if (code == 0) return;
        for (uint8_t i = 0; i < count_; i++)
            if (&rows_[i] != keep && rows_[i].code == code) rows_[i].code = 0;
    }

    /// The row with this id, or null.
    Row* find(uint32_t id) {
        for (uint8_t i = 0; i < count_; i++) if (rows_[i].id == id) return &rows_[i];
        return nullptr;
    }

    /// A decoded code: bind it to the armed row, or run whichever row holds it.
    void processCode(uint32_t code) {
        lastCode_ = code;

        for (uint8_t i = 0; i < count_; i++) {
            if (!rows_[i].learn) continue;
            rows_[i].code = code;
            claimCode(code, &rows_[i]);     // one key, one row: the newest binding wins
            rows_[i].learn = false;
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "learned 0x%08lX",
                          static_cast<unsigned long>(code));
            setStatus(statusBuf_);
            // Written straight into the row, so it schedules its own save rather than waiting.
            markDirty();
            FilesystemModule::noteDirty();
            return;
        }

        for (uint8_t i = 0; i < count_; i++) {
            if (rows_[i].code == 0 || rows_[i].code != code) continue;
            // A row needing a release would latch, a remote having none, so it is reported.
            if (rows_[i].action.kind == InputAction::Kind::Set) {
                setStatus("a set row needs a release, which a remote has no way to send",
                          Severity::Warning);
                return;
            }
            // Reported either way, or a row pointing nowhere looks like a dead remote.
            statusBuf_[0] = 0;
            if (runInputAction(rows_[i].action, /*pressed=*/true, statusBuf_, sizeof(statusBuf_))
                || statusBuf_[0])
                setStatus(statusBuf_);
            return;
        }
        std::snprintf(statusBuf_, sizeof(statusBuf_), "received 0x%08lX (unassigned)",
                      static_cast<unsigned long>(code));
        setStatus(statusBuf_);   // status only: nothing persistent changed, so no dirty mark
    }

    /// Report the true receive state, since a pin can be set while the channel cannot bind.
    void reportReady() {
        // An unset pin releases the channel, which would otherwise stay armed on a cleared pin.
        if (pin_ < 0) { platform::irStop(); setStatus("set pin to receive", Severity::Warning); return; }
        // A restore that unassigned rows says so rather than "ready", or the warning would last one tick.
        if (platform::irChannelReady(static_cast<uint16_t>(pin_)))
            offSurface_ ? reportOffSurface(*this, statusBuf_, sizeof(statusBuf_), offSurface_) : setStatus("ready");
        else setStatus("infrared channel failed to open, pin busy or invalid?", Severity::Error);
    }

    /// Codes one device binds, with room for a full handset.
    static constexpr uint8_t kMaxRows = 24;

    int8_t   pin_ = -1;              ///< receiver GPIO, -1 until a board or user sets it
    uint8_t  offSurface_ = 0;        ///< rows the last restore unassigned, reported until the next one
    Row      rows_[kMaxRows];
    uint8_t  count_ = 0;
    uint32_t nextId_ = 1;
    uint32_t lastCode_ = 0;          ///< last decoded frame (0 = none)
    char     statusBuf_[48] = {};
};

}  // namespace mm
